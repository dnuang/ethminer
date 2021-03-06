#include "PoolManager.h"
#include <chrono>

using namespace std;
using namespace dev;
using namespace eth;

static string diffToDisplay(double diff)
{
	static const char* k[] = {"hashes", "kilohashes", "megahashes", "gigahashes", "terahashes", "petahashes"};
	uint32_t i = 0;
	while ((diff > 1000.0) && (i < ((sizeof(k) / sizeof(char *)) - 2)))
	{
		i++;
		diff = diff / 1000.0;
	}
	stringstream ss;
	ss << fixed << setprecision(2) << diff << ' ' << k[i]; 
	return ss.str();
}

PoolManager::PoolManager(PoolClient * client, Farm &farm, MinerType const & minerType, unsigned maxTries) : m_farm(farm), m_minerType(minerType)
{
	p_client = client;
	m_maxConnectionAttempts = maxTries;
	
	p_client->onConnected([&]()
	{
		m_connectionAttempt = 0;
		cnote << "Connected to " << m_connections[m_activeConnectionIdx].Host() << p_client->ActiveEndPoint();

		if (!m_farm.isMining())
		{
			cnote << "Spinning up miners...";
			if (m_minerType == MinerType::CL)
				m_farm.start("opencl", false);
			else if (m_minerType == MinerType::CUDA)
				m_farm.start("cuda", false);
			else if (m_minerType == MinerType::Mixed) {
				m_farm.start("cuda", false);
				m_farm.start("opencl", true);
			}
		}

	});

	p_client->onDisconnected([&]()
	{
		dev::setThreadName("main");
		cnote << "Disconnected from " + m_connections[m_activeConnectionIdx].Host() << p_client->ActiveEndPoint();

		// Do not stop mining here
		// Workloop will determine if we're trying a fast reconnect to same pool
		// or if we're switching to failover(s)

	});

	p_client->onWorkReceived([&](WorkPackage const& wp)
	{
		for (auto h : m_headers)
			if (h == wp.header)
			{
				cwarn << EthYellow "Duplicate job" << wp.header << " discarded" EthReset;
				return;
			}
		m_headers.push_back(wp.header);
		while (m_headers.size() > 4)
			m_headers.pop_front();

		cnote << "New job" << wp.header << "  " + m_connections[m_activeConnectionIdx].Host() + p_client->ActiveEndPoint();
		if (wp.boundary != m_lastBoundary)
		{
			using namespace boost::multiprecision;

			m_lastBoundary = wp.boundary;
			static const uint512_t dividend("0x10000000000000000000000000000000000000000000000000000000000000000");
			const uint256_t divisor(string("0x") + m_lastBoundary.hex());
			cnote << "New pool difficulty:" << EthWhite << diffToDisplay(double(dividend / divisor)) << EthReset;
		}

		m_farm.setWork(wp);
		
	});

	p_client->onSolutionAccepted([&](bool const& stale)
	{
		using namespace std::chrono;
		auto ms = duration_cast<milliseconds>(steady_clock::now() - m_submit_time);
		std::stringstream ss;
		ss << std::setw(4) << std::setfill(' ') << ms.count();
		ss << "ms." << "   " << m_connections[m_activeConnectionIdx].Host() + p_client->ActiveEndPoint();
		cnote << EthLime "**Accepted" EthReset << (stale ? "(stale)" : "") << ss.str();
		m_farm.acceptedSolution(stale);
	});

	p_client->onSolutionRejected([&](bool const& stale)
	{
		using namespace std::chrono;
		auto ms = duration_cast<milliseconds>(steady_clock::now() - m_submit_time);
		std::stringstream ss;
		ss << std::setw(4) << std::setfill(' ') << ms.count();
		ss << "ms." << "   " << m_connections[m_activeConnectionIdx].Host() + p_client->ActiveEndPoint();
		cwarn << EthRed "**Rejected" EthReset << (stale ? "(stale)" : "") << ss.str();
		m_farm.rejectedSolution(stale);
	});

	m_farm.onSolutionFound([&](Solution sol)
	{
		// Solution should passthrough only if client is
		// properly connected. Otherwise we'll have the bad behavior
		// to log nonce submission but receive no response

		if (p_client->isConnected()) {

			m_submit_time = std::chrono::steady_clock::now();

			if (sol.stale)
				cnote << string(EthYellow "Stale nonce 0x") + toHex(sol.nonce);
			else
				cnote << string("Nonce 0x") + toHex(sol.nonce);

			p_client->submitSolution(sol);

		}
		else {

			cnote << string(EthRed "Nonce 0x") + toHex(sol.nonce) << "wasted. Waiting for connection ...";

		}

		return false;
	});
	m_farm.onMinerRestart([&]() {
		dev::setThreadName("main");
		cnote << "Restart miners...";

		if (m_farm.isMining()) {
			cnote << "Shutting down miners...";
			m_farm.stop();
		}

		cnote << "Spinning up miners...";
		if (m_minerType == MinerType::CL)
			m_farm.start("opencl", false);
		else if (m_minerType == MinerType::CUDA)
			m_farm.start("cuda", false);
		else if (m_minerType == MinerType::Mixed) {
			m_farm.start("cuda", false);
			m_farm.start("opencl", true);
		}
	});
}

void PoolManager::stop()
{
	if (m_running.load(std::memory_order_relaxed)) {
		
		cnote << "Shutting down...";

		m_running.store(false, std::memory_order_relaxed);

		if (p_client->isConnected()) 
			p_client->disconnect(); 

		if (m_farm.isMining())
		{
			cnote << "Shutting down miners...";
			m_farm.stop();
		}

	}
}

void PoolManager::workLoop()
{

	dev::setThreadName("main");

	while (m_running.load(std::memory_order_relaxed))
	{

		// Take action only if not pending state (connecting/disconnecting)
		// Otherwise do nothing and wait until connection state is NOT pending
		if (!p_client->isPendingState()) {

			if (!p_client->isConnected()) {

				// Rotate connections if above max attempts threshold
				if (m_connectionAttempt >= m_maxConnectionAttempts) {

					m_connectionAttempt = 0;
					m_activeConnectionIdx++;
					if (m_activeConnectionIdx == m_connections.size()) {
						m_activeConnectionIdx = 0;
					}

					// Stop mining if applicable as we're switching
					if (m_farm.isMining()) {
						cnote << "Shutting down miners...";
						m_farm.stop();

						// Give some time to mining threads to shutdown
						for (auto i = 4; --i; this_thread::sleep_for(chrono::seconds(1))) {
							cnote << "Retrying in " << i << "... \r";
						}

					}

				}

				if (m_connections[m_activeConnectionIdx].Host() != "exit") {

					// Count connectionAttempts
					m_connectionAttempt++;

					// Invoke connections
					p_client->setConnection(m_connections[m_activeConnectionIdx]);
					m_farm.set_pool_addresses(m_connections[m_activeConnectionIdx].Host(), m_connections[m_activeConnectionIdx].Port());
					cnote << "Selected pool" << (m_connections[m_activeConnectionIdx].Host() + ":" + toString(m_connections[m_activeConnectionIdx].Port()));
					p_client->connect();

				}
				else {

					cnote << "No more failover connections.";

					// Stop mining if applicable
					if (m_farm.isMining()) {
						cnote << "Shutting down miners...";
						m_farm.stop();
					}

					m_running.store(false, std::memory_order_relaxed);
					continue;
				}

			}

		}

		// Hashrate reporting
		m_hashrateReportingTimePassed++;
		
		if (m_hashrateReportingTimePassed > m_hashrateReportingTime) {
			auto mp = m_farm.miningProgress();
			std::string h = toHex(toCompactBigEndian(mp.rate(), 1));
			std::string res = h[0] != '0' ? h : h.substr(1);

			// Should be 32 bytes
			// https://github.com/ethereum/wiki/wiki/JSON-RPC#eth_submithashrate
			std::ostringstream ss;
			ss << std::setw(64) << std::setfill('0') << res;

			p_client->submitHashrate("0x" + ss.str());
			m_hashrateReportingTimePassed = 0;
		}

		this_thread::sleep_for(chrono::seconds(1));

	}
}

void PoolManager::addConnection(URI &conn)
{
	m_connections.push_back(conn);

}

void PoolManager::clearConnections()
{
	m_connections.clear();
	m_farm.set_pool_addresses("", 0);
	if (p_client && p_client->isConnected())
		p_client->disconnect();
}

void PoolManager::start()
{
	if (m_connections.size() > 0) {
		m_running.store (true, std::memory_order_relaxed);
		m_workThread = std::thread{ boost::bind(&PoolManager::workLoop, this) };
	}
	else {
		cwarn << "Manager has no connections defined!";
	}
}

