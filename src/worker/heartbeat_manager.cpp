#include "heartbeat_manager.h"
#include "protocol.h"
#include "logging.h"
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace suco::worker {

HeartbeatManager::HeartbeatManager(NetworkClient& client, int total_slots, std::atomic<int>& used_slots)
    : m_client(client),
      m_total_slots(total_slots),
      m_used_slots(used_slots) {}

HeartbeatManager::~HeartbeatManager() {
    stop();
}

void HeartbeatManager::start() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread(&HeartbeatManager::run, this);
}

void HeartbeatManager::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void HeartbeatManager::run() {
    // Heartbeat interval. The coordinator overwrites its per-worker slots_used with
    // the value reported here, so this interval bounds how long a LEAKED reservation
    // (a job assigned but abandoned on direct-dispatch failure → never CACHE_STOREd →
    // never decremented) keeps a worker looking busier than it is. 5s was far too long:
    // leaks accumulated → workers looked full → under-assignment → low grid utilisation.
    // Default now 500ms; tune via SUCO_HEARTBEAT_MS.
    int interval_ms = 500;
    if (const char* env = std::getenv("SUCO_HEARTBEAT_MS")) {
        try { interval_ms = std::max(100, std::stoi(env)); } catch (...) {}
    }
    const int steps = std::max(1, interval_ms / 100);
    const int step_ms = std::max(1, interval_ms / steps);

    while (m_running) {
        // Sleep in small steps so stop() is responsive.
        for (int i = 0; i < steps && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        }

        if (!m_running) break;

        std::vector<double> usages = m_metrics.get_cpu_usages();
        uint32_t type_net = htonl(suco::PACKET_HEARTBEAT);
        uint32_t active_net = htonl(m_used_slots.load());
        uint32_t total_net = htonl(m_total_slots);
        uint32_t cores_net = htonl(static_cast<uint32_t>(usages.size()));

        if (!m_client.send_packet(&type_net, 4) ||
            !m_client.send_packet(&active_net, 4) ||
            !m_client.send_packet(&total_net, 4) ||
            !m_client.send_packet(&cores_net, 4)) {
            SUCO_LOG_ERROR("Failed to send heartbeat. Coordinator connection broken.");
            break;
        }

        if (!usages.empty()) {
            if (!m_client.send_packet(usages.data(), usages.size() * sizeof(double))) {
                SUCO_LOG_ERROR("Failed to send CPU details in heartbeat. Coordinator connection broken.");
                break;
            }
        }
        
        SUCO_LOG_DEBUG("Sent heartbeat (Slots: {}/{})", m_used_slots.load(), m_total_slots);
    }
}

} // namespace suco::worker
