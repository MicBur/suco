#include "network_server.h"
#include "protocol.h"
#include "logging.h"
#include "hash_util.h"
#include "tls_util.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include <filesystem>
#include <cerrno>
#include <cstdlib>

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

    SUCO_LOG_INFO("NetworkServer starting on TCP Port {}...", m_config.get_coordinator_port());

    // Starten des TCP-Listeners und UDP-Discovery-Broadcasters
    m_tcp_thread = std::thread(&NetworkServer::run_tcp_listener, this);
    m_udp_thread = std::thread(&NetworkServer::run_udp_broadcast, this);
}

void NetworkServer::stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    SUCO_LOG_INFO("NetworkServer stopping...");

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

    SUCO_LOG_INFO("NetworkServer stopped.");
}

void NetworkServer::run_tcp_listener() {
    m_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_server_fd == INVALID_SOCKET_VAL) {
        SUCO_LOG_ERROR("Failed to create TCP server socket.");
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
        // FATAL: a coordinator that can't bind 9000 is useless (workers/clients can't
        // connect). Previously it kept running while only serving 9001/9002, so a
        // process manager saw it as "up" and never recovered. Exit non-zero so systemd
        // (Restart=on-failure) relaunches once the port is actually free.
        SUCO_LOG_ERROR("FATAL: Bind failed on TCP Port {} — exiting so the service manager can restart.",
                       m_config.get_coordinator_port());
        close_socket(m_server_fd);
        m_server_fd = INVALID_SOCKET_VAL;
        std::exit(1);
    }

    if (listen(m_server_fd, SOMAXCONN) < 0) {
        SUCO_LOG_ERROR("FATAL: Listen failed on TCP Port {} — exiting for restart.",
                       m_config.get_coordinator_port());
        close_socket(m_server_fd);
        m_server_fd = INVALID_SOCKET_VAL;
        std::exit(1);
    }

    SUCO_LOG_INFO("TCP server listening on Port {}", m_config.get_coordinator_port());

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
            // Optional TLS: complete the server handshake before anything is read, so
            // the classification peek below sees decrypted bytes. No-op when TLS is off.
            if (!suco::tls::wrap_accept(client_sock)) {
                close_socket(client_sock);
                return;
            }
            void* ssl = suco::tls::ssl_for(client_sock);
            uint32_t first_packet_type_net = 0;
            // Classify by peeking the 4-byte packet type. A SINGLE recv(MSG_PEEK,4) may
            // return fewer than 4 bytes — TCP can deliver the header in fragments, and
            // under a burst of concurrent connects (make -jN) the peek often races ahead
            // of the full header. Failing on a short read closed healthy connections and
            // surfaced client-side as "coordinator disconnected" -> funnel/local storm.
            // Loop the peek until 4 bytes are buffered (or the peer really closes / errors).
            long peeked = 0;
            for (int attempt = 0; attempt < 200; ++attempt) {   // ~2s worst case
                long r;
                if (ssl) {
                    r = suco::tls::ssl_peek(ssl, &first_packet_type_net, 4);
                } else {
#ifdef _WIN32
                    r = recv(client_sock, reinterpret_cast<char*>(&first_packet_type_net), 4, MSG_PEEK);
#else
                    r = recv(client_sock, &first_packet_type_net, 4, MSG_PEEK);
#endif
                }
                if (r >= 4) { peeked = 4; break; }
                if (r == 0 && !ssl) { peeked = 0; break; }      // peer closed cleanly
                if (r < 0 && !ssl) {
#ifdef _WIN32
                    break;
#else
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { peeked = -1; break; }
#endif
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (peeked < 4) {
                SUCO_LOG_DEBUG("NetworkServer: classification peek incomplete for {} (peeked={}), closing", client_ip, peeked);
                suco::tls::close_tls(client_sock);
                close_socket(client_sock);
                return;
            }

            uint32_t type = ntohl(first_packet_type_net);
            if (type == suco::PACKET_HEARTBEAT) {
                // Heartbeat Byte konsumieren
                uint32_t dummy = 0;
                read_all(client_sock, &dummy, 4);
                m_worker_handler(client_sock);
            } else if (type == suco::PACKET_TOOLCHAIN_DOWNLOAD) {
                // Heartbeat/Type Byte konsumieren
                uint32_t dummy = 0;
                read_all(client_sock, &dummy, 4);
                
                uint32_t hash_len_net = 0;
                if (read_all(client_sock, &hash_len_net, 4)) {
                    uint32_t hash_len = ntohl(hash_len_net);
                    std::vector<char> hash_buf(hash_len);
                    if (hash_len > 0 && read_all(client_sock, hash_buf.data(), hash_len)) {
                        std::string hash(hash_buf.data(), hash_len);
                        
                        std::string cache_dir = get_toolchain_cache_dir();
                        
                        std::string archive_path = cache_dir + "/toolchain-" + hash + ".tar.zst";
                        std::error_code ec;
                        if (std::filesystem::exists(archive_path, ec)) {
                            SUCO_LOG_INFO("NetworkServer: Sending toolchain {} on dedicated connection to {}", hash, client_ip);
                            send_file(client_sock, archive_path);
                        } else {
                            SUCO_LOG_ERROR("NetworkServer: Requested toolchain {} not found at {}!", hash, archive_path);
                            uint32_t size_zero = 0;
                            send_all(client_sock, &size_zero, 4);
                        }
                    }
                }
                close_socket(client_sock);
            } else {
                m_client_handler(client_sock);
            }
        }).detach();
    }
}

void NetworkServer::run_udp_broadcast() {
    m_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_udp_fd == INVALID_SOCKET_VAL) {
        SUCO_LOG_ERROR("Failed to create UDP socket.");
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
    SUCO_LOG_INFO("UDP Auto-Discovery broadcast active on Port {}", suco::DEFAULT_UDP_PORT);

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
