#include "worker_handler.h"
#include "protocol.h"
#include "logging.h"
#include "hash_util.h"
#include <iostream>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <chrono>
#include <thread>
#include <filesystem>
#include <sstream>
#include <cstdlib>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

namespace suco {

#ifndef _WIN32
// Resolve the coordinator's own LAN address, used to rewrite a co-located
// worker's loopback registration into something remote clients can reach.
//
// NOTE: do NOT use gethostname()+getaddrinfo() — on stock Debian/Ubuntu the
// hostname resolves to 127.0.1.1 (see /etc/hosts), so that approach returns a
// loopback address and the rewrite silently no-ops (observed on k3master:
// `getent hosts $(hostname)` -> 127.0.1.1). Enumerate interfaces instead, like
// `hostname -I`, and pick a real private-LAN NIC while skipping container/
// overlay ranges (docker0 172.17.*, k3s flannel 10.42.*, k3s svc 10.43.*).
static std::string get_local_routable_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return "127.0.0.1";
    std::string best;      // preferred private-LAN address
    std::string fallback;  // any non-loopback IPv4, if nothing better
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN] = {0};
        auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        if (!inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf))) continue;
        std::string ip = buf;
        if (ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0) continue;
        if (fallback.empty()) fallback = ip;
        if (ip.rfind("172.17.", 0) == 0 || ip.rfind("10.42.", 0) == 0 ||
            ip.rfind("10.43.", 0) == 0) continue;  // container/overlay nets
        if (ip.rfind("192.168.", 0) == 0) { best = ip; break; }  // strongest signal
        if (best.empty() && (ip.rfind("10.", 0) == 0 || ip.rfind("172.", 0) == 0)) best = ip;
    }
    if (ifaddr) freeifaddrs(ifaddr);
    if (!best.empty()) return best;
    if (!fallback.empty()) return fallback;
    return "127.0.0.1";
}
#else
static std::string get_local_routable_ip() {
    return "127.0.0.1";
}
#endif

WorkerHandler::WorkerHandler(const CoordinatorConfig& config, 
                             WorkerManager& worker_manager, 
                             JobQueue& job_queue,
                             SharedCoordinatorState& state,
                             DisconnectHandler disconnect_handler)
    : m_config(config),
      m_worker_manager(worker_manager),
      m_job_queue(job_queue),
      m_state(state),
      m_disconnect_handler(disconnect_handler) {}

static std::string get_socket_ip(socket_t sock) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sock, (struct sockaddr*)&addr, &addr_len) == 0) {
        return inet_ntoa(addr.sin_addr);
    }
    return "unknown";
}

static void parse_toolchain_json(const std::string& json, ToolchainInfo& info) {
    auto extract_section = [](const std::string& j, const std::string& sec_name, std::map<std::string, std::string>& dest_map) {
        size_t sec_pos = j.find("\"" + sec_name + "\":");
        if (sec_pos == std::string::npos) return;
        size_t block_start = j.find("{", sec_pos);
        if (block_start == std::string::npos) return;
        size_t block_end = j.find("}", block_start);
        if (block_end == std::string::npos) return;
        
        std::string section_content = j.substr(block_start + 1, block_end - block_start - 1);
        
        size_t pos = 0;
        while (true) {
            size_t key_start = section_content.find("\"", pos);
            if (key_start == std::string::npos) break;
            size_t key_end = section_content.find("\"", key_start + 1);
            if (key_end == std::string::npos) break;
            
            std::string key = section_content.substr(key_start + 1, key_end - key_start - 1);
            
            size_t colon_pos = section_content.find(":", key_end);
            if (colon_pos == std::string::npos) break;
            
            size_t val_start = section_content.find("\"", colon_pos);
            if (val_start == std::string::npos) break;
            size_t val_end = section_content.find("\"", val_start + 1);
            if (val_end == std::string::npos) break;
            
            std::string val = section_content.substr(val_start + 1, val_end - val_start - 1);
            
            dest_map[key] = val;
            pos = val_end + 1;
        }
    };

    extract_section(json, "compilers", info.compilers);
    extract_section(json, "tools", info.tools);
    extract_section(json, "qt", info.qt_versions);
}

void WorkerHandler::handle_worker_connection(socket_t worker_sock) {
    std::string worker_ip = get_socket_ip(worker_sock);
    if (worker_ip == "127.0.0.1" || worker_ip == "::1" || worker_ip == "localhost") {
        std::string resolved_ip = get_local_routable_ip();
        if (resolved_ip != "127.0.0.1") {
            worker_ip = resolved_ip;
        }
    }
    // 1. Registrierungsdaten lesen
    uint32_t slots_total_net = 0;
    if (!read_all(worker_sock, &slots_total_net, 4)) {
        close_socket(worker_sock);
        return;
    }
    int slots_total = ntohl(slots_total_net);

    uint32_t name_len_net = 0;
    if (!read_all(worker_sock, &name_len_net, 4)) { close_socket(worker_sock); return; }
    uint32_t name_len = ntohl(name_len_net);
    std::vector<char> name_buf(name_len);
    if (name_len > 0) {
        if (!read_all(worker_sock, name_buf.data(), name_len)) { close_socket(worker_sock); return; }
    }
    std::string name(name_buf.data(), name_len);

    uint32_t os_len_net = 0;
    if (!read_all(worker_sock, &os_len_net, 4)) { close_socket(worker_sock); return; }
    uint32_t os_len = ntohl(os_len_net);
    std::vector<char> os_buf(os_len);
    if (os_len > 0) {
        if (!read_all(worker_sock, os_buf.data(), os_len)) { close_socket(worker_sock); return; }
    }
    std::string os_str(os_buf.data(), os_len);

    uint32_t tc_len_net = 0;
    if (!read_all(worker_sock, &tc_len_net, 4)) { close_socket(worker_sock); return; }
    uint32_t tc_len = ntohl(tc_len_net);
    std::vector<char> tc_buf(tc_len);
    if (tc_len > 0) {
        if (!read_all(worker_sock, tc_buf.data(), tc_len)) { close_socket(worker_sock); return; }
    }
    std::string tc_json(tc_buf.data(), tc_len);

    uint16_t direct_port_net = 0;
    if (!read_all(worker_sock, &direct_port_net, 2)) { close_socket(worker_sock); return; }
    uint16_t direct_port = ntohs(direct_port_net);

    // --- Shared-secret authentication for workers (HMAC challenge-response) ---
    // Prevents a rogue worker from joining the grid to read source payloads or serve
    // poisoned objects. Active only when SUCO_SECRET is configured.
    {
        std::string secret = suco::get_shared_secret();
        if (!secret.empty()) {
            std::string nonce = suco::generate_nonce();
            uint32_t nlen = htonl(static_cast<uint32_t>(nonce.size()));
            if (nonce.empty() || !send_all(worker_sock, &nlen, 4) ||
                !send_all(worker_sock, nonce.data(), nonce.size())) {
                close_socket(worker_sock); return;
            }
            uint32_t mlen_net = 0;
            if (!read_all(worker_sock, &mlen_net, 4)) { close_socket(worker_sock); return; }
            uint32_t mlen = ntohl(mlen_net);
            if (mlen == 0 || mlen > 256) {
                SUCO_LOG_ERROR("Coordinator: worker AUTH FAILED (bad MAC length) from {}", worker_ip);
                close_socket(worker_sock); return;
            }
            std::string mac(mlen, '\0');
            if (!read_all(worker_sock, mac.data(), mlen)) { close_socket(worker_sock); return; }
            if (!suco::constant_time_equals(mac, suco::hmac_sha256_hex(secret, nonce))) {
                SUCO_LOG_ERROR("Coordinator: worker AUTH FAILED from {} — rejecting registration", worker_ip);
                close_socket(worker_sock); return;
            }
            // Positive acknowledgement so an accepted worker knows it is in, and a
            // rejected one (socket closed above) sees the read fail and backs off
            // instead of looping on a false "registered" state.
            uint8_t auth_ok = 1;
            if (!send_all(worker_sock, &auth_ok, 1)) { close_socket(worker_sock); return; }
        }
    }

    ToolchainInfo toolchains;
    parse_toolchain_json(tc_json, toolchains);

    std::string tc_summary = "";
    for (const auto& [n, v] : toolchains.compilers) {
        if (!tc_summary.empty()) tc_summary += ", ";
        tc_summary += n + "=" + v;
    }
    for (const auto& [n, v] : toolchains.tools) {
        if (!tc_summary.empty()) tc_summary += ", ";
        tc_summary += n + "=" + v;
    }
    for (const auto& [n, v] : toolchains.qt_versions) {
        if (!tc_summary.empty()) tc_summary += ", ";
        tc_summary += n + "=" + v;
    }
    if (tc_summary.empty()) tc_summary = "none";

    SUCO_LOG_INFO("Worker registered: {} ({}, OS: {}, Cores: {}, Toolchains: {}, Direct Port: {})", 
                  name, worker_ip, os_str, slots_total, tc_summary, direct_port);

    // 2. Socket-Empfangs-Timeout konfigurieren (für Heartbeat-Erkennung)
    #ifdef _WIN32
    int timeout_ms = 10000;
    setsockopt(worker_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    #else
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(worker_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    #endif

    // 3. Im WorkerManager registrieren
    auto node = std::make_shared<WorkerNode>();
    node->socket = worker_sock;
    node->ip = worker_ip;
    node->name = name;
    node->os = os_str;
    node->slots_total = slots_total;
    node->slots_used = 0;
    node->cpu_cores_usage.resize(slots_total, 0.0);
    node->last_heartbeat = std::chrono::steady_clock::now();
    node->toolchains = toolchains;
    node->toolchains_raw_json = tc_json;
    node->direct_port = direct_port;

    m_worker_manager.register_worker(node);

    // T11: Broadcast worker_join
    {
        std::stringstream json;
        json << "{"
             << "\"name\":\"" << name << "\","
             << "\"ip\":\"" << worker_ip << "\","
             << "\"slots\":" << slots_total
             << "}";
        m_state.broadcast_sse_event("worker_join", json.str());
    }

    // 4. Multiplexing Loop für Heartbeats und Kompilierungs-Antworten
    while (true) {
        uint32_t packet_type_net = 0;
        if (!read_all(worker_sock, &packet_type_net, 4)) {
            break; // Connection closed oder timeout
        }
        uint32_t type = ntohl(packet_type_net);

        if (type == PACKET_HEARTBEAT) {
            uint32_t active_slots_net = 0, total_slots_net = 0, cores_count_net = 0;
            if (!read_all(worker_sock, &active_slots_net, 4) ||
                !read_all(worker_sock, &total_slots_net, 4) ||
                !read_all(worker_sock, &cores_count_net, 4)) {
                break;
            }
            int active_slots = ntohl(active_slots_net);
            int total_slots = ntohl(total_slots_net);
            int cores_count = ntohl(cores_count_net);

            std::vector<double> usage(cores_count);
            if (cores_count > 0) {
                if (!read_all(worker_sock, usage.data(), cores_count * sizeof(double))) {
                    break;
                }
            }

            // Update in WorkerManager
            m_worker_manager.update_heartbeat(worker_sock, active_slots, total_slots, usage);
            SUCO_LOG_DEBUG("Received heartbeat from worker {} (slots: {}/{})", worker_ip, active_slots, total_slots);
        }
        else if (type == PACKET_TOOLCHAIN_REQUEST) {
            uint32_t hash_len_net = 0;
            if (!read_all(worker_sock, &hash_len_net, 4)) break;
            uint32_t hash_len = ntohl(hash_len_net);
            std::vector<char> hash_buf(hash_len);
            if (hash_len > 0) {
                if (!read_all(worker_sock, hash_buf.data(), hash_len)) break;
            }
            std::string toolchain_hash(hash_buf.data(), hash_len);

            SUCO_LOG_INFO("Worker {} requested toolchain {}", worker_ip, toolchain_hash);

            std::string cache_dir = get_toolchain_cache_dir();

            std::string archive_path;
            std::error_code ec;
            if (std::filesystem::exists(cache_dir, ec)) {
                for (const auto& entry : std::filesystem::directory_iterator(cache_dir, ec)) {
                    if (entry.is_regular_file(ec)) {
                        std::string fname = entry.path().filename().string();
                        if (fname.find(toolchain_hash) != std::string::npos && fname.ends_with(".tar.zst")) {
                            archive_path = entry.path().string();
                            break;
                        }
                    }
                }
            }

            if (!archive_path.empty() && std::filesystem::exists(archive_path)) {
                uint32_t resp_type_net = htonl(PACKET_TOOLCHAIN_TRANSFER);
                bool send_ok = false;
                {
                    std::lock_guard<std::mutex> lock(node->write_mutex);
                    if (send_all(worker_sock, &resp_type_net, 4) &&
                        send_file(worker_sock, archive_path)) {
                        send_ok = true;
                    }
                }
                if (send_ok) {
                    SUCO_LOG_INFO("Successfully sent toolchain {} to worker {}", toolchain_hash, worker_ip);
                    uint32_t ack_type_net = 0;
                    if (read_all(worker_sock, &ack_type_net, 4)) {
                        uint32_t ack_type = ntohl(ack_type_net);
                        if (ack_type == PACKET_TOOLCHAIN_ACK) {
                            SUCO_LOG_INFO("Worker {} acknowledged toolchain {}", worker_ip, toolchain_hash);
                        }
                    }
                } else {
                    SUCO_LOG_ERROR("Failed to send toolchain {} to worker {}", toolchain_hash, worker_ip);
                }
            } else {
                SUCO_LOG_ERROR("Requested toolchain {} not found on coordinator!", toolchain_hash);
                uint32_t resp_type_net = htonl(PACKET_TOOLCHAIN_TRANSFER);
                uint32_t size_zero = 0;
                std::lock_guard<std::mutex> lock(node->write_mutex);
                send_all(worker_sock, &resp_type_net, 4);
                send_all(worker_sock, &size_zero, 4);
            }
        }
        else if (type == PACKET_COMPILE_RESP) {
            uint32_t file_len_net = 0;
            if (!read_all(worker_sock, &file_len_net, 4)) break;
            uint32_t file_len = ntohl(file_len_net);
            std::vector<char> file_buf(file_len);
            if (file_len > 0) {
                if (!read_all(worker_sock, file_buf.data(), file_len)) break;
            }
            std::string filename(file_buf.data(), file_len);

            int32_t exit_code_net = 0;
            if (!read_all(worker_sock, &exit_code_net, 4)) break;
            int32_t exit_code = ntohl(exit_code_net);

            uint32_t log_len_net = 0;
            if (!read_all(worker_sock, &log_len_net, 4)) break;
            uint32_t log_len = ntohl(log_len_net);
            std::vector<char> log_buf(log_len);
            if (log_len > 0) {
                if (!read_all(worker_sock, log_buf.data(), log_len)) break;
            }
            std::string log_str(log_buf.data(), log_len);

            uint8_t bin_comp = 0;
            if (!read_all(worker_sock, &bin_comp, 1)) break;

            uint32_t bin_len_net = 0;
            if (!read_all(worker_sock, &bin_len_net, 4)) break;
            uint32_t bin_len = ntohl(bin_len_net);
            std::vector<uint8_t> bin_data(bin_len);
            if (bin_len > 0) {
                if (!read_all(worker_sock, bin_data.data(), bin_len)) break;
            }

            uint32_t hc_hit_net = 0;
            if (!read_all(worker_sock, &hc_hit_net, 4)) break;
            bool header_cache_hit = (ntohl(hc_hit_net) != 0);

            std::shared_ptr<CompileResult> res;
            SUCO_LOG_INFO("Received compile response from worker {} for {} (Exit: {})", worker_ip, filename, exit_code);
            {
                std::lock_guard<std::mutex> lock(m_state.results_mutex);
                auto it = m_state.compile_results.find(filename);
                if (it != m_state.compile_results.end()) {
                    res = it->second;
                }
            }

            std::string header_set_hash_to_add;
            {
                std::lock_guard<std::mutex> r_lock(m_state.running_details_mutex);
                auto it = m_state.running_job_details.find(filename);
                if (it != m_state.running_job_details.end()) {
                    header_set_hash_to_add = it->second.header_set_hash;
                }
            }

            if (exit_code == 0 && !header_set_hash_to_add.empty()) {
                {
                    std::lock_guard<std::mutex> khs_lock(m_state.known_header_sets_mutex);
                    m_state.known_header_sets.insert(header_set_hash_to_add);
                }
                {
                    std::lock_guard<std::mutex> khs_lock(node->known_header_sets_mutex);
                    node->known_header_sets.insert(header_set_hash_to_add);
                }
            }

            if (res) {
                std::lock_guard<std::mutex> lock(res->mutex);
                res->exit_code = exit_code;
                res->header_cache_hit = header_cache_hit;
                res->log = log_str;
                res->bin = bin_data;
                res->bin_comp = bin_comp;
                res->ready = true;
                res->cv.notify_all();
            }
        }
    }

    // 5. Worker offline verarbeiten
    std::string ip_to_cleanup = worker_ip;
    
    // T11: Broadcast worker_leave
    {
        std::stringstream json;
        json << "{"
             << "\"name\":\"" << node->name << "\","
             << "\"ip\":\"" << node->ip << "\""
             << "}";
        m_state.broadcast_sse_event("worker_leave", json.str());
    }

    // Deregister aus WorkerManager
    m_worker_manager.deregister_worker(worker_sock);
    close_socket(worker_sock);

    // Triggere Rescheduling über Callback an den Coordinator
    if (m_disconnect_handler && !ip_to_cleanup.empty()) {
        std::thread([this, ip_to_cleanup]() {
            m_disconnect_handler(ip_to_cleanup);
        }).detach();
    }
}

} // namespace suco
