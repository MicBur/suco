#include "network_client.h"
#include "tls_util.h"
#include "logging.h"
#include "protocol.h"
#include "utils.h"
#include "hash_util.h"
#include "zstd_util.h"

#include <fstream>
#include <iostream>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>
#include <chrono>

#ifndef _WIN32
    #include <sys/types.h>
    #include <pwd.h>
    #include <unistd.h>
#endif

namespace suco {

namespace {

struct ConnectionPoolEntry {
    socket_t sock;
    std::string host;
    uint16_t port;
    std::chrono::steady_clock::time_point last_used;
};

std::mutex g_pool_mutex;
std::vector<ConnectionPoolEntry> g_pool;

std::string get_local_toolchain_archive(const std::string& hash) {
    std::string cache_dir = get_toolchain_cache_dir();
    std::error_code ec;
    if (!std::filesystem::exists(cache_dir, ec)) return "";
    for (const auto& entry : std::filesystem::directory_iterator(cache_dir, ec)) {
        if (entry.is_regular_file(ec)) {
            std::string fname = entry.path().filename().string();
            if (fname.find(hash) != std::string::npos && fname.ends_with(".tar.zst")) {
                return entry.path().string();
            }
        }
    }
    return "";
}

} // namespace

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
    : config_(std::move(config)), is_waiting_(false) {}

NetworkClient::~NetworkClient() {
    if (is_connected_ && sock_ != INVALID_SOCKET_VAL && !is_waiting_) {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        g_pool.push_back(ConnectionPoolEntry{
            .sock = sock_,
            .host = config_.coordinator_host,
            .port = config_.coordinator_port,
            .last_used = std::chrono::steady_clock::now()
        });
        sock_ = INVALID_SOCKET_VAL;
        is_connected_ = false;
    } else {
        disconnect();
    }
}

bool NetworkClient::is_available() {
    bool available = connect_to_coordinator();
    disconnect();
    return available;
}

CacheResult NetworkClient::try_get_from_cache(const CompilerCommand& cmd,
                                              const std::function<bool()>& local_takeover_check) {
    bool retried = false;
    while (true) {
        if (!connect_to_coordinator()) {
            return CacheResult{ .hit = false };
        }

        SUCO_LOG_INFO("Querying coordinator cache for {}", cmd.source_file);

        uint32_t type = htonl(suco::PACKET_CACHE_QUERY);
        uint32_t hash_len = htonl(cmd.content_hash.size());
        uint32_t file_len = htonl(cmd.source_file.size());
        uint32_t tc_hash_len = htonl(cmd.toolchain_hash.size());
        // On the wire we advertise the DISPATCH id (target-qualified for MinGW), so
        // the scheduler only picks workers that actually have the right driver.
        const std::string req_comp = cmd.get_dispatch_compiler_id();
        uint32_t req_comp_len = htonl(req_comp.size());
        uint32_t req_comp_ver_len = htonl(cmd.required_compiler_version.size());
        uint32_t hs_hash_len = htonl(cmd.header_set_hash.size());

        if (!suco::send_all(sock_, &type, 4) ||
            !suco::send_all(sock_, &hash_len, 4) ||
            !suco::send_all(sock_, cmd.content_hash.c_str(), cmd.content_hash.size()) ||
            !suco::send_all(sock_, &file_len, 4) ||
            !suco::send_all(sock_, cmd.source_file.c_str(), cmd.source_file.size()) ||
            !suco::send_all(sock_, &tc_hash_len, 4) ||
            (!cmd.toolchain_hash.empty() && !suco::send_all(sock_, cmd.toolchain_hash.c_str(), cmd.toolchain_hash.size())) ||
            !suco::send_all(sock_, &req_comp_len, 4) ||
            (!req_comp.empty() && !suco::send_all(sock_, req_comp.c_str(), req_comp.size())) ||
            !suco::send_all(sock_, &req_comp_ver_len, 4) ||
            (!cmd.required_compiler_version.empty() && !suco::send_all(sock_, cmd.required_compiler_version.c_str(), cmd.required_compiler_version.size())) ||
            !suco::send_all(sock_, &hs_hash_len, 4) ||
            (!cmd.header_set_hash.empty() && !suco::send_all(sock_, cmd.header_set_hash.c_str(), cmd.header_set_hash.size()))) {
            
            SUCO_LOG_ERROR("Failed to send cache query to coordinator");
            disconnect();
            if (!retried) {
                SUCO_LOG_WARNING("Retrying connection to coordinator once...");
                retried = true;
                continue;
            }
            return CacheResult{ .hit = false };
        }

        // The coordinator HOLDS a cache-miss query while the grid is saturated (push
        // scheduling, up to SUCO_PUSH_WAIT_MS). Racing that hold in 50ms slices lets a
        // freed client core win without giving up the hold's instant grid assignment —
        // shortening the hold instead (tried first) degraded scheduling back to lossy
        // polling and cost ~30% cold wall time. Skipped under TLS: data can sit in the
        // SSL buffer where select() cannot see it.
        if (local_takeover_check && !suco::tls::enabled()) {
            bool takeover = false;
            for (;;) {
                fd_set rf;
                FD_ZERO(&rf);
                FD_SET(sock_, &rf);
                struct timeval tv{};
                tv.tv_usec = 50 * 1000;
                int rc = select(static_cast<int>(sock_ + 1), &rf, nullptr, nullptr, &tv);
                if (rc != 0) break;                       // response ready or socket error
                if (local_takeover_check()) { takeover = true; break; }
            }
            if (takeover) {
                disconnect();
                CacheResult r{};
                r.local_takeover = true;
                return r;
            }
        }

        uint32_t resp_type_net = 0;
        if (!suco::read_all(sock_, &resp_type_net, 4)) {
            SUCO_LOG_ERROR("Failed to read cache response type from coordinator");
            disconnect();
            if (!retried) {
                SUCO_LOG_WARNING("Retrying connection to coordinator once...");
                retried = true;
                continue;
            }
            return CacheResult{ .hit = false };
        }
        uint32_t resp_type = ntohl(resp_type_net);

        if (resp_type == suco::PACKET_TOOLCHAIN_REQUEST) {
            uint32_t req_hash_len_net = 0;
            if (!suco::read_all(sock_, &req_hash_len_net, 4)) {
                disconnect();
                if (!retried) { SUCO_LOG_WARNING("Retrying connection once..."); retried = true; continue; }
                return CacheResult{ .hit = false };
            }
            uint32_t req_hash_len = ntohl(req_hash_len_net);
            std::vector<char> req_hash_buf(req_hash_len);
            if (req_hash_len > 0) {
                if (!suco::read_all(sock_, req_hash_buf.data(), req_hash_len)) {
                    disconnect();
                    if (!retried) { SUCO_LOG_WARNING("Retrying connection once..."); retried = true; continue; }
                    return CacheResult{ .hit = false };
                }
            }
            std::string req_hash(req_hash_buf.data(), req_hash_len);

            std::string archive_path = get_local_toolchain_archive(req_hash);
            if (!archive_path.empty() && std::filesystem::exists(archive_path)) {
                SUCO_LOG_INFO("Uploading toolchain archive {} to coordinator...", req_hash);
                uint32_t transfer_type = htonl(suco::PACKET_TOOLCHAIN_TRANSFER);
                if (suco::send_all(sock_, &transfer_type, 4) &&
                    suco::send_file(sock_, archive_path)) {
                    
                    uint32_t ack_type_net = 0;
                    if (suco::read_all(sock_, &ack_type_net, 4)) {
                        uint32_t ack_type = ntohl(ack_type_net);
                        if (ack_type == suco::PACKET_TOOLCHAIN_ACK) {
                            SUCO_LOG_INFO("Coordinator acknowledged toolchain transfer successfully.");
                        }
                    }
                }
            } else {
                SUCO_LOG_ERROR("Requested toolchain archive not found locally: {}", req_hash);
                uint32_t transfer_type = htonl(suco::PACKET_TOOLCHAIN_TRANSFER);
                uint32_t size_zero = 0;
                suco::send_all(sock_, &transfer_type, 4);
                suco::send_all(sock_, &size_zero, 4);
            }

            // Now read the actual cache query response
            if (!suco::read_all(sock_, &resp_type_net, 4)) {
                SUCO_LOG_ERROR("Failed to read cache response type from coordinator after toolchain upload");
                disconnect();
                if (!retried) { SUCO_LOG_WARNING("Retrying connection once..."); retried = true; continue; }
                return CacheResult{ .hit = false };
            }
            resp_type = ntohl(resp_type_net);
        }

        if (resp_type == suco::PACKET_CACHE_HIT) {
            SUCO_LOG_INFO("Cache hit for {}", cmd.source_file);
            
            uint32_t log_len_net = 0;
            if (!suco::read_all(sock_, &log_len_net, 4)) {
                disconnect();
                if (!retried) { SUCO_LOG_WARNING("Retrying connection once..."); retried = true; continue; }
                return CacheResult{ .hit = false };
            }
            uint32_t log_len = ntohl(log_len_net);
            std::vector<char> log_buf;
            if (log_len > 0) {
                log_buf.resize(log_len);
                if (!suco::read_all(sock_, log_buf.data(), log_len)) {
                    disconnect();
                    if (!retried) { SUCO_LOG_WARNING("Retrying connection once..."); retried = true; continue; }
                    return CacheResult{ .hit = false };
                }
            }

            uint8_t bin_comp = 0;
            if (!suco::read_all(sock_, &bin_comp, 1)) {
                disconnect();
                if (!retried) { SUCO_LOG_WARNING("Retrying connection once..."); retried = true; continue; }
                return CacheResult{ .hit = false };
            }

            uint32_t bin_len_net = 0;
            if (!suco::read_all(sock_, &bin_len_net, 4)) {
                disconnect();
                if (!retried) { SUCO_LOG_WARNING("Retrying connection once..."); retried = true; continue; }
                return CacheResult{ .hit = false };
            }
            uint32_t bin_len = ntohl(bin_len_net);
            std::vector<uint8_t> bin_buf;
            if (bin_len > 0) {
                bin_buf.resize(bin_len);
                if (!suco::read_all(sock_, bin_buf.data(), bin_len)) {
                    disconnect();
                    if (!retried) { SUCO_LOG_WARNING("Retrying connection once..."); retried = true; continue; }
                    return CacheResult{ .hit = false };
                }
            }

            if (bin_comp == 1) {
                std::string comp_str(reinterpret_cast<const char*>(bin_buf.data()), bin_buf.size());
                std::string decomp_str = suco::decompress_zstd(comp_str);
                if (decomp_str.empty() && bin_len > 0) {
                    SUCO_LOG_ERROR("Failed to decompress cached binary received from coordinator");
                    disconnect();
                    if (!retried) { SUCO_LOG_WARNING("Retrying connection once..."); retried = true; continue; }
                    return CacheResult{ .hit = false };
                }
                bin_buf.clear();
                bin_buf.insert(bin_buf.end(), decomp_str.begin(), decomp_str.end());
            }

            return CacheResult{ .hit = true, .log = std::move(log_buf), .binary = std::move(bin_buf) };
        }

        if (resp_type == suco::PACKET_CACHE_MISS) {
            SUCO_LOG_INFO("Cache miss for {}", cmd.source_file);
            
            if (cmd.source_file == "dummy.cpp" || cmd.content_hash.rfind("dummy-tc-check-", 0) == 0) {
                disconnect();
                return CacheResult{ .hit = false, .wait = false };
            }
            
            uint32_t ip_len_net = 0;
            if (!suco::read_all(sock_, &ip_len_net, 4)) {
                disconnect();
                return CacheResult{ .hit = false };
            }
            uint32_t ip_len = ntohl(ip_len_net);
            std::vector<char> ip_buf(ip_len);
            if (ip_len > 0) {
                if (!suco::read_all(sock_, ip_buf.data(), ip_len)) {
                    disconnect();
                    return CacheResult{ .hit = false };
                }
            }
            std::string worker_ip(ip_buf.data(), ip_len);

            uint16_t port_net = 0;
            if (!suco::read_all(sock_, &port_net, 2)) {
                disconnect();
                return CacheResult{ .hit = false };
            }
            uint16_t worker_port = ntohs(port_net);
            
            uint8_t hs_known = 0;
            if (!suco::read_all(sock_, &hs_known, 1)) {
                disconnect();
                return CacheResult{ .hit = false };
            }
            bool header_set_known = (hs_known != 0);

            disconnect();

            return CacheResult{ 
                .hit = false, 
                .wait = false, 
                .worker_ip = std::move(worker_ip), 
                .worker_port = worker_port,
                .header_set_known = header_set_known
            };
        }

        if (resp_type == suco::PACKET_CACHE_WAIT) {
            SUCO_LOG_INFO("Cache query pending (waiting for parallel compile of {})", cmd.source_file);
            is_waiting_ = true;
            return CacheResult{ .hit = false, .wait = true };
        }

        SUCO_LOG_ERROR("Received unexpected protocol packet type: {}", resp_type);
        disconnect();
        return CacheResult{ .hit = false };
    }
}

CompileResult NetworkClient::try_compile(const CompilerCommand& cmd) {
    if (!is_connected_) {
        SUCO_LOG_ERROR("Remote compilation failed: No active connection to coordinator");
        return CompileResult{ .success = false };
    }

    SUCO_LOG_INFO("Requesting remote compile for {}", cmd.source_file);

    // Reconstruct the compiler invocation command without local-only parameters
    std::string remote_cmd = cmd.get_remote_compiler_name();
    for (size_t f = 0; f < cmd.other_flags.size(); ++f) {
        const auto& flag = cmd.other_flags[f];
        
        // PCH-bezogene Flags für den remote Compiler filtern, 
        // da der preprozessierte Quellcode bereits voll expandiert ist.
        if (cmd.is_msvc) {
            if (flag.rfind("/Yu", 0) == 0 || flag.rfind("-Yu", 0) == 0 ||
                flag.rfind("/Fp", 0) == 0 || flag.rfind("-Fp", 0) == 0 ||
                flag.rfind("/Yc", 0) == 0 || flag.rfind("-Yc", 0) == 0 ||
                flag.rfind("/Y-", 0) == 0 || flag.rfind("-Y-", 0) == 0) {
                continue;
            }
        } else {
            // The preprocessed source already HAS the forced header and the PCH text
            // expanded into it. Re-applying them remotely is at best redundant and at
            // worst fatal: a project-local "-include config.h" does not exist on the
            // worker. (Both are now single units — sorting used to separate the flag
            // from its value, so the old "skip the next element" trick could not work.)
            if (flag == "-include-pch" || flag.starts_with("-include-pch ") ||
                flag.starts_with("-include ")) {
                continue;
            }
            if (flag.rfind("-fpch-preprocess", 0) == 0) {
                continue;
            }
            if (ends_with(flag, ".gch") || ends_with(flag, ".pch")) {
                continue;
            }
        }
        remote_cmd += " " + flag;
    }
    if (!cmd.language_standard.empty()) {
        remote_cmd += " " + cmd.language_standard;
    }
    if (cmd.required_compiler == "g++" || cmd.required_compiler == "gcc") {
        remote_cmd += " -fdirectives-only";
    }
    if (config_.path_normalization) {
        std::string root = suco::detect_checkout_root(".");
        if (!root.empty()) {
            if (cmd.required_compiler == "g++" || cmd.required_compiler == "gcc" ||
                cmd.required_compiler == "clang++" || cmd.required_compiler == "clang") {
                remote_cmd += " \"-ffile-prefix-map=" + root + "=.\"";
            }
        }
    }

    std::string compressed_src;
    uint8_t src_comp = 0;
    uint32_t src_len = cmd.preprocessed_source.size();
    const char* src_data = cmd.preprocessed_source.data();

    if (config_.compression_enabled && cmd.preprocessed_source.size() >= 4096) {
        compressed_src = suco::compress_zstd(cmd.preprocessed_source, config_.compression_level);
        if (!compressed_src.empty() && compressed_src.size() < cmd.preprocessed_source.size()) {
            src_comp = 1;
            src_len = compressed_src.size();
            src_data = compressed_src.data();
        }
    }

    uint8_t hs_comp = 0;
    uint32_t hs_len = 0;
    const char* hs_data = "";
    std::string compressed_hs;

    if (!cmd.header_set_source.empty()) {
        hs_len = cmd.header_set_source.size();
        hs_data = cmd.header_set_source.data();
        if (config_.compression_enabled && cmd.header_set_source.size() >= 4096) {
            compressed_hs = suco::compress_zstd(cmd.header_set_source, config_.compression_level);
            if (!compressed_hs.empty() && compressed_hs.size() < cmd.header_set_source.size()) {
                hs_comp = 1;
                hs_len = compressed_hs.size();
                hs_data = compressed_hs.data();
            }
        }
    }

    uint32_t req_type = htonl(suco::PACKET_COMPILE_REQ);
    uint32_t cmd_len = htonl(remote_cmd.size());
    uint32_t file_len = htonl(cmd.source_file.size());
    uint32_t src_len_net = htonl(src_len);
    // Dispatch id on the wire (target-qualified for MinGW) — see PACKET_CACHE_QUERY.
    const std::string req_comp = cmd.get_dispatch_compiler_id();
    uint32_t req_comp_len = htonl(req_comp.size());
    uint32_t req_comp_ver_len = htonl(cmd.required_compiler_version.size());
    uint32_t tc_hash_len = htonl(cmd.toolchain_hash.size());
    uint32_t hs_hash_len = htonl(cmd.header_set_hash.size());
    uint32_t hs_len_net = htonl(hs_len);

    if (!suco::send_all(sock_, &req_type, 4) ||
        !suco::send_all(sock_, &cmd_len, 4) ||
        !suco::send_all(sock_, remote_cmd.c_str(), remote_cmd.size()) ||
        !suco::send_all(sock_, &file_len, 4) ||
        !suco::send_all(sock_, cmd.source_file.c_str(), cmd.source_file.size()) ||
        !suco::send_all(sock_, &src_comp, 1) ||
        !suco::send_all(sock_, &src_len_net, 4) ||
        (src_len > 0 && !suco::send_all(sock_, src_data, src_len)) ||
        !suco::send_all(sock_, &req_comp_len, 4) ||
        (!req_comp.empty() && !suco::send_all(sock_, req_comp.c_str(), req_comp.size())) ||
        !suco::send_all(sock_, &req_comp_ver_len, 4) ||
        (!cmd.required_compiler_version.empty() && !suco::send_all(sock_, cmd.required_compiler_version.c_str(), cmd.required_compiler_version.size())) ||
        !suco::send_all(sock_, &tc_hash_len, 4) ||
        (!cmd.toolchain_hash.empty() && !suco::send_all(sock_, cmd.toolchain_hash.c_str(), cmd.toolchain_hash.size())) ||
        !suco::send_all(sock_, &hs_hash_len, 4) ||
        (!cmd.header_set_hash.empty() && !suco::send_all(sock_, cmd.header_set_hash.c_str(), cmd.header_set_hash.size())) ||
        !suco::send_all(sock_, &hs_comp, 1) ||
        !suco::send_all(sock_, &hs_len_net, 4) ||
        (hs_len > 0 && !suco::send_all(sock_, hs_data, hs_len))) {
        SUCO_LOG_ERROR("Failed to transmit compilation payload to coordinator");
        disconnect();
        return CompileResult{ .success = false };
    }

    return read_compile_response(true);
}

CompileResult NetworkClient::wait_for_result() {
    if (!is_connected_) {
        SUCO_LOG_ERROR("Remote compilation wait failed: No active connection to coordinator");
        return CompileResult{ .success = false };
    }

    SUCO_LOG_INFO("Waiting for parallel compile result from coordinator...");
    return read_compile_response(false);
}

CompileResult NetworkClient::read_compile_response(bool keep_alive) {
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

    uint8_t bin_comp = 0;
    if (!suco::read_all(sock_, &bin_comp, 1)) {
        disconnect();
        return CompileResult{ .success = false };
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

    if (bin_comp == 1) {
        std::string comp_str(reinterpret_cast<const char*>(bin_buf.data()), bin_buf.size());
        std::string decomp_str = suco::decompress_zstd(comp_str);
        if (decomp_str.empty() && bin_len > 0) {
            SUCO_LOG_ERROR("Failed to decompress object binary received from coordinator");
            disconnect();
            return CompileResult{ .success = false };
        }
        bin_buf.clear();
        bin_buf.insert(bin_buf.end(), decomp_str.begin(), decomp_str.end());
    }

    // Phase B4: Read header cache hit flag (for backward compatibility, default to false if read fails)
    uint32_t hc_hit_net = 0;
    bool header_cache_hit = false;
    if (suco::read_all(sock_, &hc_hit_net, 4)) {
        header_cache_hit = (ntohl(hc_hit_net) != 0);
    }

    // Work completed, close the socket connection unless keep_alive is true
    if (!keep_alive) {
        disconnect();
    }

    return CompileResult{
        .success = true,
        .exit_code = exit_code,
        .header_cache_hit = header_cache_hit,
        .log = std::move(log_buf),
        .binary = std::move(bin_buf)
    };
}

CompileResult NetworkClient::read_direct_worker_response() {
    uint32_t resp_type_net = 0;
    if (!suco::read_all(sock_, &resp_type_net, 4)) {
        SUCO_LOG_ERROR("Failed to read direct worker response type");
        disconnect();
        return CompileResult{ .success = false };
    }
    if (ntohl(resp_type_net) != suco::PACKET_COMPILE_RESP) {
        SUCO_LOG_ERROR("Unexpected packet type from worker (direct): {}", ntohl(resp_type_net));
        disconnect();
        return CompileResult{ .success = false };
    }

    // Leading filename field (present in the worker's handle_compile_job frame,
    // used by the coordinator relay to route results). Read and discard.
    uint32_t fn_len_net = 0;
    if (!suco::read_all(sock_, &fn_len_net, 4)) { disconnect(); return CompileResult{ .success = false }; }
    uint32_t fn_len = ntohl(fn_len_net);
    if (fn_len > 0) {
        std::vector<char> fn_buf(fn_len);
        if (!suco::read_all(sock_, fn_buf.data(), fn_len)) { disconnect(); return CompileResult{ .success = false }; }
    }

    int32_t exit_code_net = 0;
    if (!suco::read_all(sock_, &exit_code_net, 4)) { disconnect(); return CompileResult{ .success = false }; }
    int32_t exit_code = ntohl(exit_code_net);

    uint32_t log_len_net = 0;
    if (!suco::read_all(sock_, &log_len_net, 4)) { disconnect(); return CompileResult{ .success = false }; }
    uint32_t log_len = ntohl(log_len_net);
    std::vector<char> log_buf;
    if (log_len > 0) {
        log_buf.resize(log_len);
        if (!suco::read_all(sock_, log_buf.data(), log_len)) { disconnect(); return CompileResult{ .success = false }; }
    }

    uint8_t bin_comp = 0;
    if (!suco::read_all(sock_, &bin_comp, 1)) { disconnect(); return CompileResult{ .success = false }; }

    uint32_t bin_len_net = 0;
    if (!suco::read_all(sock_, &bin_len_net, 4)) { disconnect(); return CompileResult{ .success = false }; }
    uint32_t bin_len = ntohl(bin_len_net);
    std::vector<uint8_t> bin_buf;
    if (bin_len > 0) {
        bin_buf.resize(bin_len);
        if (!suco::read_all(sock_, bin_buf.data(), bin_len)) { disconnect(); return CompileResult{ .success = false }; }
    }

    if (bin_comp == 1) {
        std::string comp_str(reinterpret_cast<const char*>(bin_buf.data()), bin_buf.size());
        std::string decomp_str = suco::decompress_zstd(comp_str);
        if (decomp_str.empty() && bin_len > 0) {
            SUCO_LOG_ERROR("Failed to decompress object binary received from worker (direct)");
            disconnect();
            return CompileResult{ .success = false };
        }
        bin_buf.assign(decomp_str.begin(), decomp_str.end());
    }

    // Trailing header-cache-hit flag (best effort — worker always sends it).
    uint32_t hc_hit_net = 0;
    bool header_cache_hit = false;
    if (suco::read_all(sock_, &hc_hit_net, 4)) {
        header_cache_hit = (ntohl(hc_hit_net) != 0);
    }

    return CompileResult{
        .success = true,
        .exit_code = exit_code,
        .header_cache_hit = header_cache_hit,
        .log = std::move(log_buf),
        .binary = std::move(bin_buf)
    };
}

bool NetworkClient::send_module_cmis(const CompilerCommand& cmd) {
    uint32_t count = htonl(static_cast<uint32_t>(cmd.module_cmis.size()));
    if (!suco::send_all(sock_, &count, 4))
        return false;

    for (const auto& [name, bytes] : cmd.module_cmis) {
        std::string compressed;
        uint8_t comp = 0;
        uint32_t len = static_cast<uint32_t>(bytes.size());
        const char* data = bytes.data();

        if (config_.compression_enabled && bytes.size() >= 4096) {
            compressed = suco::compress_zstd(bytes, config_.compression_level);
            if (!compressed.empty() && compressed.size() < bytes.size()) {
                comp = 1;
                len = static_cast<uint32_t>(compressed.size());
                data = compressed.data();
            }
        }

        uint32_t name_len = htonl(static_cast<uint32_t>(name.size()));
        uint32_t len_net = htonl(len);
        if (!suco::send_all(sock_, &name_len, 4) ||
            !suco::send_all(sock_, name.c_str(), name.size()) ||
            !suco::send_all(sock_, &comp, 1) ||
            !suco::send_all(sock_, &len_net, 4) ||
            (len > 0 && !suco::send_all(sock_, data, len)))
            return false;
    }
    return true;
}

bool NetworkClient::connect_to_coordinator() {
    if (is_connected_) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_pool_mutex);
        auto now = std::chrono::steady_clock::now();
        // Bereinige alte/inaktive Verbindungen (älter als 45 Sekunden), um SO_RCVTIMEO (60s) zu umgehen
        for (auto it = g_pool.begin(); it != g_pool.end(); ) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->last_used).count() > 45) {
                close_socket(it->sock);
                it = g_pool.erase(it);
            } else {
                ++it;
            }
        }

        // Finde eine passende Socketverbindung
        for (auto it = g_pool.begin(); it != g_pool.end(); ++it) {
            if (it->host == config_.coordinator_host && it->port == config_.coordinator_port) {
                sock_ = it->sock;
                is_connected_ = true;
                g_pool.erase(it);
                return true;
            }
        }
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
            tv.tv_sec = config_.connection_timeout_ms / 1000;
            tv.tv_usec = (config_.connection_timeout_ms % 1000) * 1000;

            res = select(static_cast<int>(sock_ + 1), nullptr, &write_fds, nullptr, &tv);
            if (res > 0) {
                int sock_err = 0;
                socklen_t len = sizeof(sock_err);
                if (getsockopt(sock_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sock_err), &len) < 0) {
                    SUCO_LOG_ERROR("connect_to_coordinator: getsockopt failed, error = {}", get_socket_error());
                    res = -1;
                } else if (sock_err != 0) {
                    SUCO_LOG_ERROR("connect_to_coordinator: socket error in connect = {}", sock_err);
                    res = -1;
                } else {
                    res = 0; // Successfully connected
                }
            } else if (res == 0) {
                SUCO_LOG_ERROR("connect_to_coordinator: select timeout after {} ms (Target: {}:{})", config_.connection_timeout_ms, config_.coordinator_host, config_.coordinator_port);
                res = -1;
            } else {
                SUCO_LOG_ERROR("connect_to_coordinator: select failed, error = {}", get_socket_error());
                res = -1;
            }
        } else {
            SUCO_LOG_ERROR("connect_to_coordinator: connect failed immediately, error = {}", err);
            res = -1;
        }
    }

    if (res < 0) {
        disconnect();
        return false;
    }

    // Set back to blocking mode for simple send/receive
    if (!suco::set_socket_nonblocking(sock_, false)) {
        SUCO_LOG_ERROR("connect_to_coordinator: failed to set socket back to blocking");
        disconnect();
        return false;
    }

#ifdef _WIN32
    // Winsock SO_RCVTIMEO/SO_SNDTIMEO take a DWORD of MILLISECONDS, not a struct
    // timeval. Passing a timeval made Winsock read its first 4 bytes (tv_sec, ~30)
    // as a 30 ms timeout, so every cross-LAN recv aborted in ~30 ms and the
    // coordinator handshake looked like an instant disconnect. Only localhost
    // (sub-30 ms round trips) ever got through — which is exactly why the grid was
    // unreachable while loopback tests passed.
    DWORD timeout_ms = config_.grid_timeout_ms + 30000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    struct timeval tv_timeout{};
    tv_timeout.tv_sec = (config_.grid_timeout_ms / 1000) + 30;
    tv_timeout.tv_usec = 0;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv_timeout, sizeof(tv_timeout));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv_timeout, sizeof(tv_timeout));
#endif

    // Optional TLS handshake before any protocol bytes (blocking mode is set above).
    // No-op when TLS is off. Only new connections handshake; pooled sockets keep
    // their SSL (registry is keyed by fd).
    if (!suco::tls::wrap_connect(sock_)) {
        SUCO_LOG_ERROR("connect_to_coordinator: TLS handshake failed");
        disconnect();
        return false;
    }

    bool ignore_version = false;
    const char* ignore_ver_env = std::getenv("SUCO_IGNORE_VERSION");
    if (ignore_ver_env && std::string(ignore_ver_env) == "1") {
        ignore_version = true;
    }

    bool simulate_old = false;
    const char* sim_old_env = std::getenv("SUCO_SIMULATE_OLD_CLIENT");
    if (sim_old_env && std::string(sim_old_env) == "1") {
        simulate_old = true;
    }

    // Run the handshake when version-checking is on OR a shared secret is configured
    // (authentication must not be skippable via SUCO_IGNORE_VERSION). Set SUCO_SECRET
    // consistently on coordinator, clients and workers.
    std::string secret = suco::get_shared_secret();
    if (!simulate_old && (!ignore_version || !secret.empty())) {
        uint32_t type = htonl(suco::PACKET_HELLO);
        uint32_t version = 200;
        const char* force_ver_env = std::getenv("SUCO_FORCE_PROTOCOL_VERSION");
        if (force_ver_env) {
            try {
                version = std::stoul(force_ver_env);
            } catch (...) {}
        }
        uint32_t version_net = htonl(version);
        if (!suco::send_all(sock_, &type, 4) || !suco::send_all(sock_, &version_net, 4)) {
            SUCO_LOG_ERROR("connect_to_coordinator: Failed to send HELLO handshake to coordinator");
            disconnect();
            return false;
        }

        uint32_t resp_type_net = 0;
        uint32_t resp_version_net = 0;
        if (!suco::read_all(sock_, &resp_type_net, 4) || !suco::read_all(sock_, &resp_version_net, 4)) {
            SUCO_LOG_ERROR("connect_to_coordinator: Coordinator disconnected during handshake (might be too old or mismatched)");
            disconnect();
            return false;
        }

        uint32_t resp_type = ntohl(resp_type_net);
        uint32_t resp_version = ntohl(resp_version_net);

        if (resp_type != suco::PACKET_HELLO) {
            SUCO_LOG_ERROR("connect_to_coordinator: Handshake failed, received unexpected response packet type: {}", resp_type);
            disconnect();
            return false;
        }

        if (!ignore_version && resp_version != version) {
            SUCO_LOG_ERROR("connect_to_coordinator: Protocol version mismatch: Client version {}, Coordinator version {}. Falling back to local compilation.", version, resp_version);
            disconnect();
            return false;
        }

        // --- Shared-secret authentication response ---
        if (!secret.empty()) {
            uint32_t nlen_net = 0;
            if (!suco::read_all(sock_, &nlen_net, 4)) { disconnect(); return false; }
            uint32_t nlen = ntohl(nlen_net);
            if (nlen == 0 || nlen > 256) {
                SUCO_LOG_ERROR("connect_to_coordinator: bad auth nonce length {} (coordinator secret mismatch?)", nlen);
                disconnect();
                return false;
            }
            std::string nonce(nlen, '\0');
            if (!suco::read_all(sock_, nonce.data(), nlen)) { disconnect(); return false; }
            std::string mac = suco::hmac_sha256_hex(secret, nonce);
            uint32_t mlen_net = htonl(static_cast<uint32_t>(mac.size()));
            if (mac.empty() || !suco::send_all(sock_, &mlen_net, 4) || !suco::send_all(sock_, mac.data(), mac.size())) {
                SUCO_LOG_ERROR("connect_to_coordinator: failed to send auth response");
                disconnect();
                return false;
            }
        }
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

    // Kaskadenfehler vermeiden: Bereinige den Pool für diesen Host/Port
    std::lock_guard<std::mutex> lock(g_pool_mutex);
    for (auto it = g_pool.begin(); it != g_pool.end(); ) {
        if (it->host == config_.coordinator_host && it->port == config_.coordinator_port) {
            close_socket(it->sock);
            it = g_pool.erase(it);
        } else {
            ++it;
        }
    }
}

bool NetworkClient::send_batch_compile_request(const std::vector<JobItem>& jobs) {
    if (!connect_to_coordinator()) {
        SUCO_LOG_ERROR("Batch compile failed: Could not connect to coordinator");
        return false;
    }

    uint32_t type = htonl(suco::PACKET_COMPILE_BATCH_REQ);
    if (!suco::send_all(sock_, &type, 4)) return false;

    uint32_t num_jobs = htonl(static_cast<uint32_t>(jobs.size()));
    if (!suco::send_all(sock_, &num_jobs, 4)) return false;

    for (const auto& item : jobs) {
        const auto& cmd = item.cmd;

        std::string remote_cmd = cmd.get_remote_compiler_name();
        for (size_t f = 0; f < cmd.other_flags.size(); ++f) {
            const auto& flag = cmd.other_flags[f];
            if (cmd.is_msvc) {
                if (flag.rfind("/Yu", 0) == 0 || flag.rfind("-Yu", 0) == 0 ||
                    flag.rfind("/Fp", 0) == 0 || flag.rfind("-Fp", 0) == 0 ||
                    flag.rfind("/Yc", 0) == 0 || flag.rfind("-Yc", 0) == 0 ||
                    flag.rfind("/Y-", 0) == 0 || flag.rfind("-Y-", 0) == 0) {
                    continue;
                }
            } else {
                // The forced header / PCH is already expanded into the preprocessed
                // source; a project-local one does not exist on the worker.
                if (flag == "-include-pch" || flag.starts_with("-include-pch ") ||
                    flag.starts_with("-include ")) {
                    continue;
                }
                if (flag.rfind("-fpch-preprocess", 0) == 0) {
                    continue;
                }
                if (ends_with(flag, ".gch") || ends_with(flag, ".pch")) {
                    continue;
                }
            }
            remote_cmd += " " + flag;
        }
        if (!cmd.language_standard.empty()) {
            remote_cmd += " " + cmd.language_standard;
        }
        if (cmd.required_compiler == "g++" || cmd.required_compiler == "gcc") {
            remote_cmd += " -fdirectives-only";
        }
        if (config_.path_normalization) {
            std::string root = suco::detect_checkout_root(".");
            if (!root.empty()) {
                if (cmd.required_compiler == "g++" || cmd.required_compiler == "gcc" ||
                    cmd.required_compiler == "clang++" || cmd.required_compiler == "clang") {
                    remote_cmd += " \"-ffile-prefix-map=" + root + "=.\"";
                }
            }
        }

        std::string compressed_src;
        uint8_t src_comp = 0;
        uint32_t src_len = cmd.preprocessed_source.size();
        const char* src_data = cmd.preprocessed_source.data();

        if (config_.compression_enabled && cmd.preprocessed_source.size() >= 4096) {
            compressed_src = suco::compress_zstd(cmd.preprocessed_source, config_.compression_level);
            if (!compressed_src.empty() && compressed_src.size() < cmd.preprocessed_source.size()) {
                src_comp = 1;
                src_len = compressed_src.size();
                src_data = compressed_src.data();
            }
        }

        uint8_t hs_comp = 0;
        uint32_t hs_len = 0;
        const char* hs_data = "";
        std::string compressed_hs;

        if (!cmd.header_set_source.empty()) {
            hs_len = cmd.header_set_source.size();
            hs_data = cmd.header_set_source.data();
            if (config_.compression_enabled && cmd.header_set_source.size() >= 4096) {
                compressed_hs = suco::compress_zstd(cmd.header_set_source, config_.compression_level);
                if (!compressed_hs.empty() && compressed_hs.size() < cmd.header_set_source.size()) {
                    hs_comp = 1;
                    hs_len = compressed_hs.size();
                    hs_data = compressed_hs.data();
                }
            }
        }

        uint32_t hash_len = htonl(cmd.content_hash.size());
        uint32_t file_len = htonl(cmd.source_file.size());
        uint32_t src_len_net = htonl(src_len);
        uint32_t tc_len = htonl(cmd.toolchain_hash.size());
        uint32_t cmd_len = htonl(remote_cmd.size());
        uint32_t req_c_len = htonl(cmd.required_compiler.size());
        uint32_t req_cv_len = htonl(cmd.required_compiler_version.size());
        uint32_t hs_hash_len = htonl(cmd.header_set_hash.size());
        uint32_t hs_len_net = htonl(hs_len);

        if (!suco::send_all(sock_, &hash_len, 4) ||
            !suco::send_all(sock_, cmd.content_hash.c_str(), cmd.content_hash.size()) ||
            !suco::send_all(sock_, &file_len, 4) ||
            !suco::send_all(sock_, cmd.source_file.c_str(), cmd.source_file.size()) ||
            !suco::send_all(sock_, &src_comp, 1) ||
            !suco::send_all(sock_, &src_len_net, 4) ||
            (src_len > 0 && !suco::send_all(sock_, src_data, src_len)) ||
            !suco::send_all(sock_, &tc_len, 4) ||
            (!cmd.toolchain_hash.empty() && !suco::send_all(sock_, cmd.toolchain_hash.c_str(), cmd.toolchain_hash.size())) ||
            !suco::send_all(sock_, &cmd_len, 4) ||
            !suco::send_all(sock_, remote_cmd.c_str(), remote_cmd.size()) ||
            !suco::send_all(sock_, &req_c_len, 4) ||
            (!cmd.required_compiler.empty() && !suco::send_all(sock_, cmd.required_compiler.c_str(), cmd.required_compiler.size())) ||
            !suco::send_all(sock_, &req_cv_len, 4) ||
            (!cmd.required_compiler_version.empty() && !suco::send_all(sock_, cmd.required_compiler_version.c_str(), cmd.required_compiler_version.size())) ||
            !suco::send_all(sock_, &hs_hash_len, 4) ||
            (!cmd.header_set_hash.empty() && !suco::send_all(sock_, cmd.header_set_hash.c_str(), cmd.header_set_hash.size())) ||
            !suco::send_all(sock_, &hs_comp, 1) ||
            !suco::send_all(sock_, &hs_len_net, 4) ||
            (hs_len > 0 && !suco::send_all(sock_, hs_data, hs_len))) {
            SUCO_LOG_ERROR("Failed to send batch job data to coordinator");
            disconnect();
            return false;
        }
    }
    return true;
}

std::vector<BatchJobResult> NetworkClient::read_batch_compile_response() {
    if (!is_connected_) {
        SUCO_LOG_ERROR("Read batch compile response failed: No active connection");
        return {};
    }

    int timeout_ms = config_.grid_timeout_ms;

    if (timeout_ms > 0) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock_, &read_fds);

        struct timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int select_res = select(static_cast<int>(sock_ + 1), &read_fds, nullptr, nullptr, &tv);
        if (select_res <= 0) {
            if (select_res == 0) {
                SUCO_LOG_WARNING("read_batch_compile_response: Timeout of {} ms exceeded. Coordinator is too slow or worker is dead. Triggering local fallback racing.", timeout_ms);
            } else {
                SUCO_LOG_ERROR("read_batch_compile_response: select failed during response wait, error = {}", get_socket_error());
            }
            disconnect();
            return {};
        }
    }

    uint32_t resp_type_net = 0;
    if (!suco::read_all(sock_, &resp_type_net, 4)) {
        disconnect();
        return {};
    }
    uint32_t resp_type = ntohl(resp_type_net);
    if (resp_type != suco::PACKET_COMPILE_BATCH_RESP) {
        SUCO_LOG_ERROR("Unexpected packet type from coordinator: {}", resp_type);
        disconnect();
        return {};
    }

    uint32_t num_jobs_net = 0;
    if (!suco::read_all(sock_, &num_jobs_net, 4)) {
        disconnect();
        return {};
    }
    uint32_t num_jobs = ntohl(num_jobs_net);

    uint32_t coord_sched_net = 0;
    uint32_t worker_comp_net = 0;
    uint32_t tc_hand_net = 0;

    if (!suco::read_all(sock_, &coord_sched_net, 4) ||
        !suco::read_all(sock_, &worker_comp_net, 4) ||
        !suco::read_all(sock_, &tc_hand_net, 4)) {
        disconnect();
        return {};
    }

    last_coord_scheduling_ms = ntohl(coord_sched_net);
    last_worker_compilation_ms = ntohl(worker_comp_net);
    last_toolchain_handling_ms = ntohl(tc_hand_net);

    std::vector<BatchJobResult> results;
    results.reserve(num_jobs);

    for (uint32_t i = 0; i < num_jobs; ++i) {
        BatchJobResult res;

        uint32_t hash_len_net = 0;
        if (!suco::read_all(sock_, &hash_len_net, 4)) {
            disconnect();
            return {};
        }
        uint32_t hash_len = ntohl(hash_len_net);
        std::vector<char> hash_buf(hash_len);
        if (hash_len > 0) {
            if (!suco::read_all(sock_, hash_buf.data(), hash_len)) {
                disconnect();
                return {};
            }
        }
        res.content_hash = std::string(hash_buf.data(), hash_len);

        int32_t exit_code_net = 0;
        if (!suco::read_all(sock_, &exit_code_net, 4)) {
            disconnect();
            return {};
        }
        res.exit_code = ntohl(exit_code_net);

        uint32_t cache_hit_net = 0;
        if (!suco::read_all(sock_, &cache_hit_net, 4)) {
            disconnect();
            return {};
        }
        res.cache_hit = (ntohl(cache_hit_net) != 0);

        uint32_t hc_hit_net = 0;
        if (!suco::read_all(sock_, &hc_hit_net, 4)) {
            disconnect();
            return {};
        }
        res.header_cache_hit = (ntohl(hc_hit_net) != 0);

        uint32_t log_len_net = 0;
        if (!suco::read_all(sock_, &log_len_net, 4)) {
            disconnect();
            return {};
        }
        uint32_t log_len = ntohl(log_len_net);
        if (log_len > 0) {
            res.log.resize(log_len);
            if (!suco::read_all(sock_, res.log.data(), log_len)) {
                disconnect();
                return {};
            }
        }

        uint8_t bin_comp = 0;
        if (!suco::read_all(sock_, &bin_comp, 1)) {
            disconnect();
            return {};
        }

        uint32_t bin_len_net = 0;
        if (!suco::read_all(sock_, &bin_len_net, 4)) {
            disconnect();
            return {};
        }
        uint32_t bin_len = ntohl(bin_len_net);
        if (bin_len > 0) {
            res.binary.resize(bin_len);
            if (!suco::read_all(sock_, res.binary.data(), bin_len)) {
                disconnect();
                return {};
            }
        }

        if (bin_comp == 1) {
            std::string comp_str(reinterpret_cast<const char*>(res.binary.data()), res.binary.size());
            std::string decomp_str = suco::decompress_zstd(comp_str);
            if (decomp_str.empty() && bin_len > 0) {
                SUCO_LOG_ERROR("Failed to decompress batch binary received from coordinator");
                disconnect();
                return {};
            }
            res.binary.clear();
            res.binary.insert(res.binary.end(), decomp_str.begin(), decomp_str.end());
        }

        results.push_back(std::move(res));
    }

    disconnect();
    return results;
}

CompileResult NetworkClient::try_compile_direct(const CompilerCommand& cmd, const std::string& worker_ip, uint16_t worker_port) {
    SUCO_LOG_INFO("Connecting directly to worker at {}:{}...", worker_ip, worker_port);
    
    // 1. Verbindung zum Worker aufbauen
    sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ == INVALID_SOCKET_VAL) {
        SUCO_LOG_ERROR("Failed to create socket for direct worker connection");
        return CompileResult{ .success = false };
    }
    
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(worker_port);
    
    bool conn_ok = false;
    if (inet_pton(AF_INET, worker_ip.c_str(), &address.sin_addr) > 0) {
        if (::connect(sock_, (struct sockaddr*)&address, sizeof(address)) >= 0) {
            conn_ok = true;
        }
    }
    
    if (!conn_ok) {
        SUCO_LOG_ERROR("Failed to connect directly to worker at {}:{}", worker_ip, worker_port);
        close_socket(sock_);
        sock_ = INVALID_SOCKET_VAL;
        return CompileResult{ .success = false };
    }
    
    is_connected_ = true;

    // Optional TLS handshake to the worker's direct listener before any bytes.
    if (!suco::tls::wrap_connect(sock_)) {
        SUCO_LOG_ERROR("Direct dispatch: TLS handshake to worker {}:{} failed", worker_ip, worker_port);
        disconnect();
        return CompileResult{ .success = false };
    }

    // 1b. Respond to the worker's direct-listener auth challenge when a shared secret
    // is configured (the worker sends a nonce before accepting the request).
    {
        std::string secret = suco::get_shared_secret();
        if (!secret.empty()) {
            uint32_t nlen_net = 0;
            if (!suco::read_all(sock_, &nlen_net, 4)) { disconnect(); return CompileResult{ .success = false }; }
            uint32_t nlen = ntohl(nlen_net);
            if (nlen == 0 || nlen > 256) { disconnect(); return CompileResult{ .success = false }; }
            std::string nonce(nlen, '\0');
            if (!suco::read_all(sock_, nonce.data(), nlen)) { disconnect(); return CompileResult{ .success = false }; }
            std::string mac = suco::hmac_sha256_hex(secret, nonce);
            uint32_t mlen_net = htonl(static_cast<uint32_t>(mac.size()));
            if (mac.empty() || !suco::send_all(sock_, &mlen_net, 4) || !suco::send_all(sock_, mac.data(), mac.size())) {
                disconnect(); return CompileResult{ .success = false };
            }
        }
    }

    // 2. Befehl rekonstruieren (identisch zu try_compile)
    std::string remote_cmd = cmd.get_remote_compiler_name();
    for (size_t f = 0; f < cmd.other_flags.size(); ++f) {
        const auto& flag = cmd.other_flags[f];
        if (cmd.is_msvc) {
            if (flag.rfind("/Yu", 0) == 0 || flag.rfind("-Yu", 0) == 0 ||
                flag.rfind("/Fp", 0) == 0 || flag.rfind("-Fp", 0) == 0 ||
                flag.rfind("/Yc", 0) == 0 || flag.rfind("-Yc", 0) == 0 ||
                flag.rfind("/Y-", 0) == 0 || flag.rfind("-Y-", 0) == 0) {
                continue;
            }
        } else {
            // The forced header / PCH is already expanded into the preprocessed source;
            // a project-local one does not exist on the worker.
            if (flag == "-include-pch" || flag.starts_with("-include-pch ") ||
                flag.starts_with("-include ")) {
                continue;
            }
            if (flag.rfind("-fpch-preprocess", 0) == 0) {
                continue;
            }
            if (ends_with(flag, ".gch") || ends_with(flag, ".pch")) {
                continue;
            }
        }
        remote_cmd += " " + flag;
    }
    if (!cmd.language_standard.empty()) {
        remote_cmd += " " + cmd.language_standard;
    }
    if (cmd.required_compiler == "g++" || cmd.required_compiler == "gcc") {
        remote_cmd += " -fdirectives-only";
    }
    if (config_.path_normalization) {
        std::string root = suco::detect_checkout_root(".");
        if (!root.empty()) {
            if (cmd.required_compiler == "g++" || cmd.required_compiler == "gcc" ||
                cmd.required_compiler == "clang++" || cmd.required_compiler == "clang") {
                remote_cmd += " \"-ffile-prefix-map=" + root + "=.\"";
            }
        }
    }

    std::string compressed_src;
    uint8_t src_comp = 0;
    uint32_t src_len = cmd.preprocessed_source.size();
    const char* src_data = cmd.preprocessed_source.data();

    if (config_.compression_enabled && cmd.preprocessed_source.size() >= 4096) {
        compressed_src = suco::compress_zstd(cmd.preprocessed_source, config_.compression_level);
        if (!compressed_src.empty() && compressed_src.size() < cmd.preprocessed_source.size()) {
            src_comp = 1;
            src_len = compressed_src.size();
            src_data = compressed_src.data();
        }
    }

    std::string compressed_hs;
    uint8_t hs_comp = 0;
    uint32_t hs_len = 0;
    const char* hs_data = nullptr;

    if (!cmd.header_set_source.empty()) {
        hs_len = cmd.header_set_source.size();
        hs_data = cmd.header_set_source.data();
        if (config_.compression_enabled && cmd.header_set_source.size() >= 4096) {
            compressed_hs = suco::compress_zstd(cmd.header_set_source, config_.compression_level);
            if (!compressed_hs.empty() && compressed_hs.size() < cmd.header_set_source.size()) {
                hs_comp = 1;
                hs_len = compressed_hs.size();
                hs_data = compressed_hs.data();
            }
        }
    }

    // 3. Sende PACKET_DIRECT_COMPILE_REQ
    // NOTE: field order must match the worker's direct-listener parser
    // (worker.cpp handle direct request): cmd, file, src, toolchain_hash,
    // header_set_hash, header_set_source. The worker does NOT read
    // required_compiler/version (the compiler is already embedded in the command
    // string), so we must NOT send them here or the stream desyncs.
    // E3: module TUs use the V2 type, which appends the CMI block. Pre-E3 workers
    // reject the unknown type, which surfaces as a failed job and a local fallback —
    // far better than desyncing their parser with fields they never read.
    if (std::getenv("SUCO_DEBUG_PAYLOAD")) {
        SUCO_LOG_INFO("[PAYLOAD] {} src_bytes={} hs_hash={} hs_bytes={} first_line=[{}]",
                      cmd.source_file, cmd.preprocessed_source.size(),
                      cmd.header_set_hash.substr(0, 8), cmd.header_set_source.size(),
                      cmd.preprocessed_source.substr(0, cmd.preprocessed_source.find('\n')));
    }
    const bool has_cmis = !cmd.module_cmis.empty();
    uint32_t req_type = htonl(has_cmis ? suco::PACKET_DIRECT_COMPILE_REQ_V2
                                       : suco::PACKET_DIRECT_COMPILE_REQ);
    uint32_t cmd_len = htonl(remote_cmd.size());
    uint32_t file_len = htonl(cmd.source_file.size());
    uint32_t src_len_net = htonl(src_len);
    uint32_t tc_hash_len = htonl(cmd.toolchain_hash.size());
    uint32_t hs_hash_len = htonl(cmd.header_set_hash.size());
    uint32_t hs_len_net = htonl(hs_len);

    if (!suco::send_all(sock_, &req_type, 4) ||
        !suco::send_all(sock_, &cmd_len, 4) ||
        !suco::send_all(sock_, remote_cmd.c_str(), remote_cmd.size()) ||
        !suco::send_all(sock_, &file_len, 4) ||
        !suco::send_all(sock_, cmd.source_file.c_str(), cmd.source_file.size()) ||
        !suco::send_all(sock_, &src_comp, 1) ||
        !suco::send_all(sock_, &src_len_net, 4) ||
        (src_len > 0 && !suco::send_all(sock_, src_data, src_len)) ||
        !suco::send_all(sock_, &tc_hash_len, 4) ||
        (!cmd.toolchain_hash.empty() && !suco::send_all(sock_, cmd.toolchain_hash.c_str(), cmd.toolchain_hash.size())) ||
        !suco::send_all(sock_, &hs_hash_len, 4) ||
        (!cmd.header_set_hash.empty() && !suco::send_all(sock_, cmd.header_set_hash.c_str(), cmd.header_set_hash.size())) ||
        !suco::send_all(sock_, &hs_comp, 1) ||
        !suco::send_all(sock_, &hs_len_net, 4) ||
        (hs_len > 0 && !suco::send_all(sock_, hs_data, hs_len))) {
        SUCO_LOG_ERROR("Failed to transmit compilation payload to worker");
        disconnect();
        return CompileResult{ .success = false };
    }

    if (has_cmis && !send_module_cmis(cmd)) {
        SUCO_LOG_ERROR("Failed to transmit module CMIs to worker");
        disconnect();
        return CompileResult{ .success = false };
    }

    // 4. Antwort vom Worker lesen.
    // The worker replies via handle_compile_job, whose frame carries a leading
    // filename field and a trailing header-cache-hit flag (needed by the
    // coordinator relay path). read_compile_response() expects the compact
    // coordinator→client frame, so the direct path needs its own parser.
    CompileResult result = read_direct_worker_response();
    disconnect();

    if (!result.success) {
        return result;
    }

    // 5. Report the result to the coordinator.
    //
    // This runs even when the remote compile FAILED, and must: the same
    // PACKET_CACHE_STORE releases the worker slot reserved during the cache-miss
    // query (client_handler), so skipping it on failure would leak one slot per
    // failed job until the grid ran dry. Only the STORE half is conditional —
    // the coordinator caches nothing unless exit_code == 0.
    //
    // Say which of the two is happening. This used to announce an upload and then
    // report "stored successfully" for any acknowledged packet, so a worker that
    // failed every single job still produced a log that read like a healthy cache
    // fill — which is exactly how the Windows dispatch outage stayed invisible.
    const bool cacheable = (result.exit_code == 0);
    if (cacheable) {
        SUCO_LOG_INFO("Uploading compiled binary to coordinator cache for hash {}", cmd.content_hash);
    } else {
        SUCO_LOG_INFO("Remote compile of {} failed (exit {}) — reporting to coordinator to release "
                      "the worker slot; not cached", cmd.source_file, result.exit_code);
    }
    if (!connect_to_coordinator()) {
        SUCO_LOG_WARNING("Failed to connect to coordinator to store cache entry");
        return result;
    }

    // PACKET_CACHE_STORE senden
    uint32_t store_type = htonl(suco::PACKET_CACHE_STORE);
    uint32_t hash_len_store = htonl(cmd.content_hash.size());
    int32_t exit_code_store = htonl(result.exit_code);
    uint32_t log_len_store = htonl(result.log.size());
    
    uint8_t bin_comp_store = 0;
    uint32_t bin_len_store = result.binary.size();
    const void* bin_data_store = result.binary.data();
    std::string compressed_bin;

    if (config_.compression_enabled && result.binary.size() >= 4096) {
        std::string bin_str(reinterpret_cast<const char*>(result.binary.data()), result.binary.size());
        compressed_bin = suco::compress_zstd(bin_str, 1);
        if (!compressed_bin.empty() && compressed_bin.size() < result.binary.size()) {
            bin_comp_store = 1;
            bin_len_store = compressed_bin.size();
            bin_data_store = compressed_bin.data();
        }
    }
    uint32_t bin_len_store_net = htonl(bin_len_store);

    // NOTE: the payload guards must be "empty OR send-succeeded", not
    // "non-empty AND send-succeeded". A clean compile has an empty log; with the
    // old `(log.size() > 0 && send)` form the whole && chain short-circuited to
    // false on every warning-free build — so the store was reported as failed
    // AND bin_comp/bin_len/binary were never sent, leaving the coordinator
    // blocked reading a payload that never arrives (multi-second hang + no cache
    // entry). Direct dispatch exercises this path on every miss, so it must be
    // correct.
    if (suco::send_all(sock_, &store_type, 4) &&
        suco::send_all(sock_, &hash_len_store, 4) &&
        suco::send_all(sock_, cmd.content_hash.c_str(), cmd.content_hash.size()) &&
        suco::send_all(sock_, &exit_code_store, 4) &&
        suco::send_all(sock_, &log_len_store, 4) &&
        (result.log.empty() || suco::send_all(sock_, result.log.data(), result.log.size())) &&
        suco::send_all(sock_, &bin_comp_store, 1) &&
        suco::send_all(sock_, &bin_len_store_net, 4) &&
        (bin_len_store == 0 || suco::send_all(sock_, bin_data_store, bin_len_store))) {

        // Warte auf ACK
        uint32_t ack_type_net = 0;
        if (suco::read_all(sock_, &ack_type_net, 4)) {
            uint32_t ack_type = ntohl(ack_type_net);
            // The ACK acknowledges the packet, not a store — the coordinator sends it
            // whether or not it cached anything. Only claim a store when one was possible.
            if (ack_type == suco::PACKET_TOOLCHAIN_ACK && cacheable) {
                SUCO_LOG_INFO("Coordinator stored cache entry successfully.");
            }
        }
    } else {
        SUCO_LOG_WARNING("Failed to send cache store request to coordinator");
    }

    disconnect();
    return result;
}

bool NetworkClient::upload_to_cache(const std::string& hash, const std::string& output_file) {
    std::ifstream infile(output_file, std::ios::binary);
    if (!infile.is_open()) {
        SUCO_LOG_WARNING("Failed to open local file {} for cache upload", output_file);
        return false;
    }
    std::vector<uint8_t> binary((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
    infile.close();

    SUCO_LOG_INFO("Uploading local compile result to coordinator cache for hash {}", hash);
    if (!connect_to_coordinator()) {
        SUCO_LOG_WARNING("Failed to connect to coordinator for local compile cache upload");
        return false;
    }

    uint32_t store_type = htonl(suco::PACKET_CACHE_STORE);
    uint32_t hash_len_store = htonl(hash.size());
    int32_t exit_code_store = htonl(0);
    uint32_t log_len_store = htonl(0);
    
    uint8_t bin_comp_store = 0;
    uint32_t bin_len_store = binary.size();
    const void* bin_data_store = binary.data();
    std::string compressed_bin;

    if (config_.compression_enabled && binary.size() >= 4096) {
        std::string bin_str(reinterpret_cast<const char*>(binary.data()), binary.size());
        compressed_bin = suco::compress_zstd(bin_str, 1);
        if (!compressed_bin.empty() && compressed_bin.size() < binary.size()) {
            bin_comp_store = 1;
            bin_len_store = compressed_bin.size();
            bin_data_store = compressed_bin.data();
        }
    }
    uint32_t bin_len_store_net = htonl(bin_len_store);

    bool ok = suco::send_all(sock_, &store_type, 4) &&
              suco::send_all(sock_, &hash_len_store, 4) &&
              suco::send_all(sock_, hash.c_str(), hash.size()) &&
              suco::send_all(sock_, &exit_code_store, 4) &&
              suco::send_all(sock_, &log_len_store, 4) &&
              suco::send_all(sock_, &bin_comp_store, 1) &&
              suco::send_all(sock_, &bin_len_store_net, 4) &&
              (bin_len_store > 0 && suco::send_all(sock_, bin_data_store, bin_len_store));

    if (ok) {
        uint32_t ack_type_net = 0;
        if (suco::read_all(sock_, &ack_type_net, 4)) {
            uint32_t ack_type = ntohl(ack_type_net);
            if (ack_type == suco::PACKET_TOOLCHAIN_ACK) {
                SUCO_LOG_INFO("Coordinator stored cache entry successfully from local compile.");
            }
        }
    }

    disconnect();
    return ok;
}

} // namespace suco
