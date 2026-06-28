#include "coordinator.h"
#include <iostream>
#include <chrono>

namespace suco {

Coordinator::Coordinator(const CoordinatorConfig& config)
    : m_config(config),
      m_worker_manager(config),
      m_scheduler(config),
      m_job_queue() {}

Coordinator::~Coordinator() {
    stop();
}

void Coordinator::start() {
    if (m_running.exchange(true)) {
        return; // Läuft bereits
    }

    std::cout << "suco-coordinator: Orchestrator services starting..." << std::endl;
    std::cout << "suco-coordinator: Configured port: " << m_config.get_coordinator_port() << std::endl;
    std::cout << "suco-coordinator: Cache directory: " << m_config.get_cache_directory() << std::endl;

    // Starten der Hintergrund-Dienste
    m_tcp_thread = std::thread(&Coordinator::run_tcp_listener, this);
    m_udp_thread = std::thread(&Coordinator::run_udp_broadcast, this);
    m_monitor_thread = std::thread(&Coordinator::run_health_monitor, this);
}

void Coordinator::stop() {
    if (!m_running.exchange(false)) {
        return; // Läuft nicht
    }

    std::cout << "suco-coordinator: Orchestrator services stopping..." << std::endl;

    // Sockets schließen und Threads beenden
    if (m_tcp_thread.joinable()) {
        m_tcp_thread.join();
    }
    if (m_udp_thread.joinable()) {
        m_udp_thread.join();
    }
    if (m_monitor_thread.joinable()) {
        m_monitor_thread.join();
    }

    std::cout << "suco-coordinator: Orchestrator services stopped." << std::endl;
}

void Coordinator::on_worker_disconnected(const std::string& worker_ip) {
    std::cout << "suco-coordinator: Orchestrating failover for crashed worker " << worker_ip << std::endl;
    
    // Reschedulet alle aktiven Jobs des ausgefallenen Workers zurück in die JobQueue
    auto rescheduled_jobs = m_job_queue.reschedule_worker_jobs(worker_ip);
    if (!rescheduled_jobs.empty()) {
        std::cout << "suco-coordinator: Successfully rescheduled " << rescheduled_jobs.size() 
                  << " jobs back to PENDING." << std::endl;
    }
}

void Coordinator::run_tcp_listener() {
    // TCP Socket Listener Stub (wird im nächsten Schritt vollständig aus main.cpp hierhin migriert)
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void Coordinator::run_udp_broadcast() {
    // UDP Auto-Discovery Broadcast Stub (wird im nächsten Schritt migriert)
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void Coordinator::run_health_monitor() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_config.get_heartbeat_interval_ms()));
        if (!m_running) break;

        // Prüft, ob Worker das Heartbeat-Timeout überschritten haben
        auto dead_ips = m_worker_manager.cleanup_inactive_workers();
        for (const auto& ip : dead_ips) {
            on_worker_disconnected(ip);
        }
    }
}

} // namespace suco
