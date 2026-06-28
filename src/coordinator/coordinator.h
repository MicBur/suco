#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include "config.h"
#include "worker_manager.h"
#include "scheduler.h"
#include "job_queue.h"

namespace suco {

/**
 * @brief Der zentrale Orchestrator des SUCO Coordinators.
 * Verbindet die Teilmodule (Konfiguration, WorkerManager, Scheduler, JobQueue)
 * und steuert den Lebenszyklus der Netzwerk- und Überwachungsthreads.
 */
class Coordinator {
public:
    /**
     * @brief Konstruiert den Coordinator mit einer Konfigurationsreferenz.
     */
    explicit Coordinator(const CoordinatorConfig& config);
    ~Coordinator();

    /**
     * @brief Startet alle Hintergrund-Dienste und Netzwerk-Listener.
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
    // Module
    CoordinatorConfig m_config;
    WorkerManager m_worker_manager;
    Scheduler m_scheduler;
    JobQueue m_job_queue;

    std::atomic<bool> m_running{false};

    // Interne Threads
    std::thread m_tcp_thread;
    std::thread m_udp_thread;
    std::thread m_monitor_thread;

    // Thread-Funktionen
    void run_tcp_listener();
    void run_udp_broadcast();
    void run_health_monitor();
};

} // namespace suco
