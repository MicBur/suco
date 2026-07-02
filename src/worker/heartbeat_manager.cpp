#include "heartbeat_manager.h"
#include "protocol.h"
#include "logging.h"
#include <iostream>
#include <chrono>

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
    // Führe den ersten Heartbeat direkt nach dem Start (nach kurzem Sleep) oder im Loop aus
    while (m_running) {
        // Schlafe in 100ms Schritten, um schnell beendet werden zu können
        for (int i = 0; i < 50 && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
