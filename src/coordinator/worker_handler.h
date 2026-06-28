#pragma once

#include <memory>
#include <string>
#include <functional>
#include "config.h"
#include "worker_manager.h"
#include "job_queue.h"
#include "socket_util.h"
#include "coordinator_types.h"

namespace suco {

/**
 * @brief Zuständig für die dauerhafte Kommunikationsabwicklung mit registrierten Worker-Knoten.
 * Empfängt Heartbeats, aktualisiert Auslastungsdaten, verarbeitet Kompilierungsergebnisse 
 * und leitet Worker-Ausfälle (Disconnects) an den Failover-Manager weiter.
 */
class WorkerHandler {
public:
    using DisconnectHandler = std::function<void(const std::string&)>;

    /**
     * @brief Konstruiert den WorkerHandler mit den benötigten Modulen und Callbacks.
     */
    WorkerHandler(const CoordinatorConfig& config, 
                  WorkerManager& worker_manager, 
                  JobQueue& job_queue,
                  SharedCoordinatorState& state,
                  DisconnectHandler disconnect_handler);
    ~WorkerHandler() = default;

    /**
     * @brief Verarbeitet eine eingehende Worker-Verbindung threadsicher.
     * @param worker_sock Das Kommunikationssocket zum Worker.
     * @param worker_ip Die IP-Adresse des Workers.
     */
    void handle_worker_connection(socket_t worker_sock);

private:
    const CoordinatorConfig& m_config;
    WorkerManager& m_worker_manager;
    JobQueue& m_job_queue;
    SharedCoordinatorState& m_state;
    DisconnectHandler m_disconnect_handler;
};

} // namespace suco
