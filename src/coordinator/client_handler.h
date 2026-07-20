#pragma once

#include <memory>
#include <string>
#include "config.h"
#include "job_queue.h"
#include "scheduler.h"
#include "worker_manager.h"
#include "socket_util.h"
#include "coordinator_types.h"
#include "lru_cache.h"

namespace suco {

/**
 * @brief Zuständig für die Abwicklung der Client-Kommunikation.
 * Verarbeitet Cache-Abfragen und Kompilieranfragen der Compiler-Wrapper (Clients).
 */
class ClientHandler {
public:
    /**
     * @brief Konstruiert den ClientHandler mit den benötigten Modulen und dem geteilten Zustand.
     */
    ClientHandler(const CoordinatorConfig& config, 
                  JobQueue& job_queue, 
                  const Scheduler& scheduler, 
                  WorkerManager& worker_manager,
                  SharedCoordinatorState& state,
                  std::unique_ptr<LruCache>& cache);
    ~ClientHandler() = default;

    /**
     * @brief Verarbeitet eine eingehende Client-Verbindung threadsicher.
     * @param client_sock Der Kommunikationssocket zum Client.
     * @param client_ip Die IP-Adresse des Clients.
     */
    void handle_client_connection(socket_t client_sock);

private:
    void handle_batch_compile_request(socket_t client_sock, const std::string& client_ip);
    void abort_waiting_clients(const std::string& hash);
    const CoordinatorConfig& m_config;
    JobQueue& m_job_queue;
    const Scheduler& m_scheduler;
    WorkerManager& m_worker_manager;
    SharedCoordinatorState& m_state;
    std::unique_ptr<LruCache>& m_cache;
};

} // namespace suco
