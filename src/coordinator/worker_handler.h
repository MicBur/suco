#pragma once

#include <memory>
#include <string>
#include "config.h"
#include "worker_manager.h"
#include "job_queue.h"
#include "socket_util.h"

namespace suco {

/**
 * @brief Zuständig für die dauerhafte Kommunikationsabwicklung mit registrierten Worker-Knoten.
 * Empfängt Heartbeats, aktualisiert Auslastungsdaten, verarbeitet Kompilierungsergebnisse 
 * und leitet Worker-Ausfälle (Disconnects) an den Failover-Manager weiter.
 */
class WorkerHandler {
public:
    /**
     * @brief Konstruiert den WorkerHandler mit den benötigten Modulen.
     */
    WorkerHandler(const CoordinatorConfig& config, 
                  WorkerManager& worker_manager, 
                  JobQueue& job_queue);
    ~WorkerHandler() = default;

    /**
     * @brief Verarbeitet die Verbindung eines neu registrierten Workers.
     * @param worker_sock Das Kommunikationssocket zum Worker.
     * @param worker_ip Die IP-Adresse des Workers.
     */
    void handle_worker_connection(socket_t worker_sock, const std::string& worker_ip);

private:
    const CoordinatorConfig& m_config;
    WorkerManager& m_worker_manager;
    JobQueue& m_job_queue;
};

} // namespace suco
