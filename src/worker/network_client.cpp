#include "network_client.h"
#include "tls_util.h"
#include "protocol.h"
#include "hash_util.h"
#include <iostream>
#include <sstream>
#include <cstring>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace suco::worker {

NetworkClient::NetworkClient() : m_sock(INVALID_SOCKET_VAL) {}

NetworkClient::~NetworkClient() {
    disconnect();
}

bool NetworkClient::discover_coordinator(std::string& out_ip, uint16_t& out_port) {
    socket_t udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock == INVALID_SOCKET_VAL) {
        std::cerr << "suco-worker error: Failed to create UDP socket for discovery." << std::endl;
        return false;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(suco::DEFAULT_UDP_PORT);

    int opt = 1;
#ifdef _WIN32
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    if (bind(udp_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "suco-worker error: Failed to bind UDP discovery socket." << std::endl;
        close_socket(udp_sock);
        return false;
    }

#ifdef _WIN32
    int timeout_ms = 5000;
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    char buffer[256];
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);

    int bytes_received = recvfrom(udp_sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr*)&sender_addr, &sender_len);
    if (bytes_received > 0) {
        buffer[bytes_received] = '\0';
        std::string msg(buffer);
        if (msg.find("SUCO_COORDINATOR_v1") == 0) {
            out_ip = inet_ntoa(sender_addr.sin_addr);
            std::stringstream ss(msg);
            std::string prefix;
            uint16_t port_val = 0;
            if (ss >> prefix >> port_val) {
                out_port = port_val;
            } else {
                out_port = suco::DEFAULT_PORT;
            }
            close_socket(udp_sock);
            return true;
        }
    }

    close_socket(udp_sock);
    return false;
}

bool NetworkClient::connect_to(const std::string& host, uint16_t port) {
    disconnect();

    m_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (m_sock == INVALID_SOCKET_VAL) {
        std::cerr << "suco-worker error: Failed to create TCP socket." << std::endl;
        return false;
    }

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) <= 0) {
        std::cerr << "suco-worker error: Invalid IP address: " << host << std::endl;
        disconnect();
        return false;
    }

    std::cout << "suco-worker: Connecting to coordinator at " << host << ":" << port << "..." << std::endl;
    if (connect(m_sock, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "suco-worker error: Connection failed to " << host << ":" << port << std::endl;
        disconnect();
        return false;
    }

    // Optional TLS handshake to the coordinator before registration/heartbeats.
    if (!suco::tls::wrap_connect(m_sock)) {
        std::cerr << "suco-worker error: TLS handshake to coordinator failed" << std::endl;
        disconnect();
        return false;
    }

    return true;
}

void NetworkClient::disconnect() {
    if (m_sock != INVALID_SOCKET_VAL) {
        close_socket(m_sock);
        m_sock = INVALID_SOCKET_VAL;
    }
}

bool NetworkClient::register_worker(const std::string& name, const std::string& os, int slots, const std::string& toolchains_json, uint16_t direct_port) {
    if (!is_connected()) return false;

    uint32_t type_net = htonl(suco::PACKET_HEARTBEAT);
    uint32_t slots_net = htonl(slots);
    
    uint32_t name_len_net = htonl(static_cast<uint32_t>(name.size()));
    uint32_t os_len_net = htonl(static_cast<uint32_t>(os.size()));
    uint32_t toolchains_len_net = htonl(static_cast<uint32_t>(toolchains_json.size()));
    uint16_t direct_port_net = htons(direct_port);

    std::lock_guard<std::mutex> lock(m_send_mutex);
    if (!suco::send_all(m_sock, &type_net, 4) ||
        !suco::send_all(m_sock, &slots_net, 4) ||
        !suco::send_all(m_sock, &name_len_net, 4) ||
        !suco::send_all(m_sock, name.c_str(), name.size()) ||
        !suco::send_all(m_sock, &os_len_net, 4) ||
        !suco::send_all(m_sock, os.c_str(), os.size()) ||
        !suco::send_all(m_sock, &toolchains_len_net, 4) ||
        !suco::send_all(m_sock, toolchains_json.c_str(), toolchains_json.size()) ||
        !suco::send_all(m_sock, &direct_port_net, 2)) {
        return false;
    }

    // Respond to the coordinator's auth challenge when a shared secret is configured.
    std::string secret = suco::get_shared_secret();
    if (!secret.empty()) {
        uint32_t nlen_net = 0;
        if (!suco::read_all(m_sock, &nlen_net, 4)) return false;
        uint32_t nlen = ntohl(nlen_net);
        if (nlen == 0 || nlen > 256) return false;
        std::string nonce(nlen, '\0');
        if (!suco::read_all(m_sock, nonce.data(), nlen)) return false;
        std::string mac = suco::hmac_sha256_hex(secret, nonce);
        uint32_t mlen_net = htonl(static_cast<uint32_t>(mac.size()));
        if (mac.empty() || !suco::send_all(m_sock, &mlen_net, 4) ||
            !suco::send_all(m_sock, mac.data(), mac.size())) {
            return false;
        }
        // Require the coordinator's positive ACK — a rejected worker's socket is
        // closed, so this read fails and registration correctly reports failure.
        uint8_t auth_ok = 0;
        if (!suco::read_all(m_sock, &auth_ok, 1) || auth_ok != 1) {
            return false;
        }
    }

    return true;
}

bool NetworkClient::send_packet(const void* data, size_t len) {
    if (!is_connected()) return false;
    std::lock_guard<std::mutex> lock(m_send_mutex);
    return suco::send_all(m_sock, data, len);
}

bool NetworkClient::receive_packet(void* data, size_t len) {
    if (!is_connected()) return false;
    std::lock_guard<std::mutex> lock(m_receive_mutex);
    return suco::read_all(m_sock, data, len);
}

} // namespace suco::worker
