#pragma once

#include <memory>
#include <atomic>
#include <thread>
#include <functional>
#include "socket_util.h"
#include "config.h"

namespace suco {

/**
 * @brief Kapselt die gesamte Low-Level-Netzwerkkommunikation des Coordinators.
 * Öffnet TCP- und UDP-Sockets und leitet eingehende Client- und Worker-Verbindungen 
 * über Callbacks an den Orchestrator weiter.
 */
class NetworkServer {
public:
    using ConnectionHandler = std::function<void(socket_t)>;

    /**
     * @brief Konstruiert den NetworkServer.
     * @param config Referenz auf die Konfiguration des Coordinators.
     * @param client_handler Callback für eingehende Client-Kompilier-Anfragen.
     * @param worker_handler Callback für sich registrierende Worker-Knoten.
     */
    NetworkServer(const CoordinatorConfig& config, 
                  ConnectionHandler client_handler, 
                  ConnectionHandler worker_handler);
    ~NetworkServer();

    /**
     * @brief Startet den TCP-Listener und den UDP-Auto-Discovery-Broadcast.
     */
    void start();

    /**
     * @brief Stoppt alle Verbindungen, schließt die Sockets und join-t die Threads.
     */
    void stop();

private:
    void run_tcp_listener();
    void run_udp_broadcast();

    const CoordinatorConfig& m_config;
    ConnectionHandler m_client_handler;
    ConnectionHandler m_worker_handler;

    std::atomic<bool> m_running{false};
    std::thread m_tcp_thread;
    std::thread m_udp_thread;

    socket_t m_server_fd = INVALID_SOCKET_VAL;
    socket_t m_udp_fd = INVALID_SOCKET_VAL;
};

} // namespace suco
