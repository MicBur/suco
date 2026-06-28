#include "network_server.h"
#include "protocol.h"
#include <iostream>
#include <cstring>
#include <chrono>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace suco {

NetworkServer::NetworkServer(const CoordinatorConfig& config, 
                             ConnectionHandler client_handler, 
                             ConnectionHandler worker_handler)
    : m_config(config),
      m_client_handler(client_handler),
      m_worker_handler(worker_handler) {}

NetworkServer::~NetworkServer() {
    stop();
}

void NetworkServer::start() {
    if (m_running.exchange(true)) {
        return;
    }

    std::cout << "suco-coordinator: NetworkServer starting on TCP Port " 
              << m_config.get_coordinator_port() << "..." << std::endl;

    // Starten des TCP-Listeners und UDP-Discovery-Broadcasters
    m_tcp_thread = std::thread(&NetworkServer::run_tcp_listener, this);
    m_udp_thread = std::thread(&NetworkServer::run_udp_broadcast, this);
}

void NetworkServer::stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    std::cout << "suco-coordinator: NetworkServer stopping..." << std::endl;

    // Sockets schließen, um blockierende accept/sendto Aufrufe abzubrechen
    if (m_server_fd != INVALID_SOCKET_VAL) {
        close_socket(m_server_fd);
        m_server_fd = INVALID_SOCKET_VAL;
    }
    if (m_udp_fd != INVALID_SOCKET_VAL) {
        close_socket(m_udp_fd);
        m_udp_fd = INVALID_SOCKET_VAL;
    }

    if (m_tcp_thread.joinable()) {
        m_tcp_thread.join();
    }
    if (m_udp_thread.joinable()) {
        m_udp_thread.join();
    }

    std::cout << "suco-coordinator: NetworkServer stopped." << std::endl;
}

void NetworkServer::run_tcp_listener() {
    m_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server_fd == INVALID_SOCKET_VAL) {
        std::cerr << "suco-coordinator: Failed to create TCP server socket." << std::endl;
        return;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_config.get_coordinator_port());

    if (bind(m_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "suco-coordinator: Bind failed on TCP Port " 
                  << m_config.get_coordinator_port() << std::endl;
        close_socket(m_server_fd);
        m_server_fd = INVALID_SOCKET_VAL;
        return;
    }

    if (listen(m_server_fd, SOMAXCONN) < 0) {
        std::cerr << "suco-coordinator: Listen failed." << std::endl;
        close_socket(m_server_fd);
        m_server_fd = INVALID_SOCKET_VAL;
        return;
    }

    std::cout << "suco-coordinator: TCP server listening on Port " 
              << m_config.get_coordinator_port() << std::endl;

    while (m_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        socket_t client_sock = accept(m_server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET_VAL) {
            if (!m_running) break;
            continue;
        }

        std::string client_ip = inet_ntoa(client_addr.sin_addr);

        // Verbindungs-Klassifizierung
        std::thread([this, client_sock, client_ip]() {
            uint32_t first_packet_type_net = 0;
#ifdef _WIN32
            int res = recv(client_sock, reinterpret_cast<char*>(&first_packet_type_net), 4, MSG_PEEK);
#else
            ssize_t res = recv(client_sock, &first_packet_type_net, 4, MSG_PEEK);
#endif
            if (res < 4) {
                close_socket(client_sock);
                return;
            }

            uint32_t type = ntohl(first_packet_type_net);
            if (type == suco::PACKET_HEARTBEAT) {
                // Heartbeat Byte konsumieren
                uint32_t dummy = 0;
                read_all(client_sock, &dummy, 4);
                m_worker_handler(client_sock);
            } else {
                m_client_handler(client_sock);
            }
        }).detach();
    }
}

void NetworkServer::run_udp_broadcast() {
    m_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_udp_fd == INVALID_SOCKET_VAL) {
        std::cerr << "suco-coordinator: Failed to create UDP socket." << std::endl;
        return;
    }

#ifdef _WIN32
    char broadcast_opt = '1';
    setsockopt(m_udp_fd, SOL_SOCKET, SO_BROADCAST, &broadcast_opt, sizeof(broadcast_opt));
#else
    int broadcast_opt = 1;
    setsockopt(m_udp_fd, SOL_SOCKET, SO_BROADCAST, &broadcast_opt, sizeof(broadcast_opt));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    addr.sin_port = htons(suco::DEFAULT_UDP_PORT);

    std::string beacon = "SUCO_COORDINATOR_v1 " + std::to_string(m_config.get_coordinator_port());
    std::cout << "suco-coordinator: UDP Auto-Discovery broadcast active on Port " 
              << suco::DEFAULT_UDP_PORT << std::endl;

    while (m_running) {
#ifdef _WIN32
        sendto(m_udp_fd, beacon.c_str(), static_cast<int>(beacon.size()), 0, 
               (struct sockaddr*)&addr, sizeof(addr));
#else
        sendto(m_udp_fd, beacon.c_str(), beacon.size(), 0, 
               (struct sockaddr*)&addr, sizeof(addr));
#endif
        
        // Zerstückelter sleep, um reaktiver beenden zu können
        for (int i = 0; i < 30 && m_running; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace suco
