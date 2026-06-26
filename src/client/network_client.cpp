#include "network_client.h"
#include "logging.h"
#include "protocol.h"

#include <fstream>
#include <iostream>
#include <cstring>

namespace suco {

// --- CacheResult implementation ---

bool CacheResult::save_to(const std::string& path) const {
    if (binary.empty()) {
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(binary.data()), static_cast<std::streamsize>(binary.size()));
    return out.good();
}

// --- CompileResult implementation ---

bool CompileResult::save_to(const std::string& path) const {
    if (binary.empty()) {
        return false;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(binary.data()), static_cast<std::streamsize>(binary.size()));
    return out.good();
}

// --- NetworkClient implementation ---

NetworkClient::NetworkClient(ClientConfig config)
    : config_(std::move(config)) {}

NetworkClient::~NetworkClient() {
    disconnect();
}

bool NetworkClient::is_available() {
    bool available = connect_to_coordinator();
    disconnect();
    return available;
}

CacheResult NetworkClient::try_get_from_cache(const CompilerCommand& cmd) {
    if (!connect_to_coordinator()) {
        return CacheResult{ .hit = false };
    }

    SUCO_LOG_INFO("Querying coordinator cache for {}", cmd.source_file);

    uint32_t type = htonl(suco::PACKET_CACHE_QUERY);
    uint32_t hash_len = htonl(cmd.content_hash.size());
    uint32_t file_len = htonl(cmd.source_file.size());

    if (!suco::send_all(sock_, &type, 4) ||
        !suco::send_all(sock_, &hash_len, 4) ||
        !suco::send_all(sock_, cmd.content_hash.c_str(), cmd.content_hash.size()) ||
        !suco::send_all(sock_, &file_len, 4) ||
        !suco::send_all(sock_, cmd.source_file.c_str(), cmd.source_file.size())) {
        SUCO_LOG_ERROR("Failed to send cache query to coordinator");
        disconnect();
        return CacheResult{ .hit = false };
    }

    uint32_t resp_type_net = 0;
    if (!suco::read_all(sock_, &resp_type_net, 4)) {
        SUCO_LOG_ERROR("Failed to read cache response type from coordinator");
        disconnect();
        return CacheResult{ .hit = false };
    }
    uint32_t resp_type = ntohl(resp_type_net);

    if (resp_type == suco::PACKET_CACHE_HIT) {
        SUCO_LOG_INFO("Cache hit for {}", cmd.source_file);
        
        uint32_t log_len_net = 0;
        if (!suco::read_all(sock_, &log_len_net, 4)) {
            disconnect();
            return CacheResult{ .hit = false };
        }
        uint32_t log_len = ntohl(log_len_net);
        std::vector<char> log_buf;
        if (log_len > 0) {
            log_buf.resize(log_len);
            if (!suco::read_all(sock_, log_buf.data(), log_len)) {
                disconnect();
                return CacheResult{ .hit = false };
            }
        }

        uint32_t bin_len_net = 0;
        if (!suco::read_all(sock_, &bin_len_net, 4)) {
            disconnect();
            return CacheResult{ .hit = false };
        }
        uint32_t bin_len = ntohl(bin_len_net);
        std::vector<uint8_t> bin_buf;
        if (bin_len > 0) {
            bin_buf.resize(bin_len);
            if (!suco::read_all(sock_, bin_buf.data(), bin_len)) {
                disconnect();
                return CacheResult{ .hit = false };
            }
        }

        // Cache hit successful: clean up connection
        disconnect();
        return CacheResult{ .hit = true, .log = std::move(log_buf), .binary = std::move(bin_buf) };
    }

    if (resp_type == suco::PACKET_CACHE_MISS) {
        SUCO_LOG_INFO("Cache miss for {}", cmd.source_file);
        // Keep socket connected for subsequent try_compile()
        return CacheResult{ .hit = false };
    }

    SUCO_LOG_ERROR("Received unexpected protocol packet type: {}", resp_type);
    disconnect();
    return CacheResult{ .hit = false };
}

CompileResult NetworkClient::try_compile(const CompilerCommand& cmd) {
    if (!is_connected_) {
        SUCO_LOG_ERROR("Remote compilation failed: No active connection to coordinator");
        return CompileResult{ .success = false };
    }

    SUCO_LOG_INFO("Requesting remote compile for {}", cmd.source_file);

    // Reconstruct the compiler invocation command without local-only parameters
    std::string remote_cmd = cmd.compiler_path;
    for (const auto& flag : cmd.other_flags) {
        remote_cmd += " " + flag;
    }
    if (!cmd.language_standard.empty()) {
        remote_cmd += " " + cmd.language_standard;
    }

    uint32_t req_type = htonl(suco::PACKET_COMPILE_REQ);
    uint32_t cmd_len = htonl(remote_cmd.size());
    uint32_t file_len = htonl(cmd.source_file.size());
    uint32_t src_len = htonl(cmd.preprocessed_source.size());

    if (!suco::send_all(sock_, &req_type, 4) ||
        !suco::send_all(sock_, &cmd_len, 4) ||
        !suco::send_all(sock_, remote_cmd.c_str(), remote_cmd.size()) ||
        !suco::send_all(sock_, &file_len, 4) ||
        !suco::send_all(sock_, cmd.source_file.c_str(), cmd.source_file.size()) ||
        !suco::send_all(sock_, &src_len, 4) ||
        !suco::send_all(sock_, cmd.preprocessed_source.c_str(), cmd.preprocessed_source.size())) {
        SUCO_LOG_ERROR("Failed to transmit compilation payload to coordinator");
        disconnect();
        return CompileResult{ .success = false };
    }

    uint32_t compile_resp_type_net = 0;
    if (!suco::read_all(sock_, &compile_resp_type_net, 4)) {
        SUCO_LOG_ERROR("Failed to read remote compile response type");
        disconnect();
        return CompileResult{ .success = false };
    }
    uint32_t compile_resp_type = ntohl(compile_resp_type_net);

    if (compile_resp_type != suco::PACKET_COMPILE_RESP) {
        SUCO_LOG_ERROR("Unexpected packet type from coordinator: {}", compile_resp_type);
        disconnect();
        return CompileResult{ .success = false };
    }

    int32_t exit_code_net = 0;
    if (!suco::read_all(sock_, &exit_code_net, 4)) {
        SUCO_LOG_ERROR("Failed to read compilation exit code");
        disconnect();
        return CompileResult{ .success = false };
    }
    int32_t exit_code = ntohl(exit_code_net);

    uint32_t log_len_net = 0;
    if (!suco::read_all(sock_, &log_len_net, 4)) {
        disconnect();
        return CompileResult{ .success = false };
    }
    uint32_t log_len = ntohl(log_len_net);
    std::vector<char> log_buf;
    if (log_len > 0) {
        log_buf.resize(log_len);
        if (!suco::read_all(sock_, log_buf.data(), log_len)) {
            disconnect();
            return CompileResult{ .success = false };
        }
    }

    uint32_t bin_len_net = 0;
    if (!suco::read_all(sock_, &bin_len_net, 4)) {
        disconnect();
        return CompileResult{ .success = false };
    }
    uint32_t bin_len = ntohl(bin_len_net);
    std::vector<uint8_t> bin_buf;
    if (bin_len > 0) {
        bin_buf.resize(bin_len);
        if (!suco::read_all(sock_, bin_buf.data(), bin_len)) {
            disconnect();
            return CompileResult{ .success = false };
        }
    }

    // Work completed, close the socket connection
    disconnect();

    return CompileResult{
        .success = true,
        .exit_code = exit_code,
        .log = std::move(log_buf),
        .binary = std::move(bin_buf)
    };
}

bool NetworkClient::connect_to_coordinator() {
    if (is_connected_) {
        return true;
    }

    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ == INVALID_SOCKET_VAL) {
        return false;
    }

    // Configure non-blocking mode to support quick timeouts
    if (!suco::set_socket_nonblocking(sock_, true)) {
        disconnect();
        return false;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.coordinator_port);
    
    struct hostent* he = gethostbyname(config_.coordinator_host.c_str());
    if (!he) {
        disconnect();
        return false;
    }
    std::memcpy(&addr.sin_addr.s_addr, he->h_addr_list[0], static_cast<size_t>(he->h_length));

    int res = connect(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (res < 0) {
        int err = get_socket_error();
        if (is_would_block(err)) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sock_, &write_fds);

            struct timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = config_.connection_timeout_ms * 1000;

            res = select(static_cast<int>(sock_ + 1), nullptr, &write_fds, nullptr, &tv);
            if (res > 0) {
                int sock_err = 0;
                socklen_t len = sizeof(sock_err);
                if (getsockopt(sock_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sock_err), &len) < 0 || sock_err != 0) {
                    res = -1;
                } else {
                    res = 0; // Successfully connected
                }
            } else {
                res = -1; // Select timeout or failure
            }
        } else {
            res = -1;
        }
    }

    // Restore socket back to blocking mode
    if (!suco::set_socket_nonblocking(sock_, false)) {
        disconnect();
        return false;
    }

    if (res < 0) {
        disconnect();
        return false;
    }

    is_connected_ = true;
    return true;
}

void NetworkClient::disconnect() noexcept {
    if (sock_ != INVALID_SOCKET_VAL) {
        close_socket(sock_);
        sock_ = INVALID_SOCKET_VAL;
    }
    is_connected_ = false;
}

} // namespace suco
