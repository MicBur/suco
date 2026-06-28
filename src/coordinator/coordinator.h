#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include "config.h"
#include "worker_manager.h"
#include "scheduler.h"
#include "job_queue.h"
#include "network_server.h"
#include "client_handler.h"
#include "worker_handler.h"
#include "coordinator_types.h"
#include "lru_cache.h"

namespace suco {

/**
 * @brief Der zentrale Orchestrator des SUCO Coordinators.
 * Verbindet die Teilmodule (Konfiguration, WorkerManager, Scheduler, JobQueue, NetworkServer)
 * und steuert den Lebenszyklus des Cache, der Dashboard REST API und der Überwachungsthreads.
 */
class Coordinator {
public:
    /**
     * @brief Konstruiert den Coordinator mit einer Konfigurationsreferenz.
     */
    explicit Coordinator(const CoordinatorConfig& config);
    ~Coordinator();

    /**
     * @brief Startet alle Hintergrund-Dienste (TCP/UDP Netzwerk-Server, REST Web Server, Health Monitor).
     */
    void start();

    /**
     * @brief Stoppt alle Hintergrund-Dienste sauber und wartet auf die Threads.
     */
    void stop();

    /**
     * @brief Callback bei Worker-Ausfall zur Re-Zuweisung betroffener Jobs.
     * @param worker_ip Die IP-Adresse des getrennten Workers.
     */
    void on_worker_disconnected(const std::string& worker_ip);

private:
    void run_health_monitor();
    void run_web_server();

    CoordinatorConfig m_config;
    std::unique_ptr<LruCache> m_cache;
    SharedCoordinatorState m_state;

    WorkerManager m_worker_manager;
    Scheduler m_scheduler;
    JobQueue m_job_queue;

    ClientHandler m_client_handler;
    WorkerHandler m_worker_handler;
    std::unique_ptr<NetworkServer> m_network_server;

    std::atomic<bool> m_running{false};
    std::thread m_monitor_thread;
    std::thread m_web_thread;

    socket_t m_web_server_fd = INVALID_SOCKET_VAL;
};

} // namespace suco
