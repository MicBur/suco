#pragma once

#include "network_client.h"
#include "metrics.h"
#include <thread>
#include <atomic>

namespace suco::worker {

class HeartbeatManager {
public:
    /**
     * @brief Konstruiert den HeartbeatManager.
     * @param client Referenz auf den NetworkClient, über den Heartbeats gesendet werden.
     * @param total_slots Gesamtanzahl an verfügbaren Kompilier-Slots.
     * @param used_slots Atomare Variable, die die aktuell belegten Slots trackt.
     */
    HeartbeatManager(NetworkClient& client, int total_slots, std::atomic<int>& used_slots);
    ~HeartbeatManager();

    // Verhindere Kopieren
    HeartbeatManager(const HeartbeatManager&) = delete;
    HeartbeatManager& operator=(const HeartbeatManager&) = delete;

    /**
     * @brief Startet den periodischen Heartbeat-Thread.
     */
    void start();

    /**
     * @brief Stoppt den Heartbeat-Thread und wartet, bis dieser beendet ist.
     */
    void stop();

private:
    void run();

    NetworkClient& m_client;
    int m_total_slots;
    std::atomic<int>& m_used_slots;
    Metrics m_metrics;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace suco::worker
