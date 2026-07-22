#include "client_handler.h"
#include "protocol.h"
#include "logging.h"
#include "batch_processor.h"
#include "hash_util.h"
#include "zstd_util.h"
#include <iostream>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include "history_writer.h"

// Slot-accounting instrumentation (enable with SUCO_SLOT_DEBUG=1). Logs every
// reserve/release/no-worker so the exact reservation leak is visible in the log.
static const bool g_slot_dbg = (std::getenv("SUCO_SLOT_DEBUG") != nullptr);

// --- PUSH SCHEDULING ---
// When a cache-miss query finds no free worker, the coordinator HOLDS the query
// (blocks this handler thread) on g_slot_cv until a slot frees — instead of
// returning "no worker" and making the client poll. A freed slot (CACHE_STORE
// release) notifies waiters, which re-run the scheduler and grab a slot. This
// removes the client-side poll gap and the re-query connection churn entirely.
// g_assign_mutex serialises select_best_worker + slots_used++ so two waking
// waiters never grab the same slot. Bounded by SUCO_PUSH_WAIT_MS (0 disables the
// wait -> old poll behaviour). Enabled by default.
static std::mutex g_assign_mutex;
static std::condition_variable g_slot_cv;
static int push_wait_ms_init() {
    if (const char* e = std::getenv("SUCO_PUSH_WAIT_MS")) {
        try { return std::max(0, std::stoi(e)); } catch (...) {}
    }
    return 10000; // 10s: wait this long for a slot before telling the client "no worker"
}
static const int g_push_wait_ms = push_wait_ms_init();
// Woken on every slot release so a waiting query can immediately claim the slot.
static inline void notify_slot_free() { g_slot_cv.notify_all(); }

#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace suco {

namespace {

bool coordinator_has_toolchain(const std::string& hash) {
    if (hash.empty()) return false;
    std::string cache_dir = get_toolchain_cache_dir();
    std::string archive_path = cache_dir + "/toolchain-" + hash + ".tar.zst";
    std::error_code ec;
    return std::filesystem::exists(archive_path, ec);
}

}

ClientHandler::ClientHandler(const CoordinatorConfig& config, 
                             JobQueue& job_queue, 
                             const Scheduler& scheduler, 
                             WorkerManager& worker_manager,
                             SharedCoordinatorState& state,
                             std::unique_ptr<LruCache>& cache)
    : m_config(config),
      m_job_queue(job_queue),
      m_scheduler(scheduler),
      m_worker_manager(worker_manager),
      m_state(state),
      m_cache(cache) {}

static std::string get_socket_ip(socket_t sock) {
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(sock, (struct sockaddr*)&addr, &addr_len) == 0) {
        return inet_ntoa(addr.sin_addr);
    }
    return "unknown";
}

void ClientHandler::handle_client_connection(socket_t client_sock) {
    std::string client_ip = get_socket_ip(client_sock);

#ifdef _WIN32
    // Winsock SO_RCVTIMEO takes a DWORD of milliseconds, not a struct timeval —
    // a timeval here would be read as ~60 ms and cut client connections short.
    DWORD tv_idle_ms = 60000;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_idle_ms), sizeof(tv_idle_ms));
#else
    struct timeval tv_idle{};
    tv_idle.tv_sec = 60;
    tv_idle.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_idle, sizeof(tv_idle));
#endif

    bool handshake_done = false;

    while (true) {
        uint32_t type_net = 0;
        if (!read_all(client_sock, &type_net, 4)) {
            close_socket(client_sock);
            return;
        }
        uint32_t type = ntohl(type_net);

        bool ignore_version = false;
        const char* ignore_ver_env = std::getenv("SUCO_IGNORE_VERSION");
        if (ignore_ver_env && std::string(ignore_ver_env) == "1") {
            ignore_version = true;
        }

        // Authentication is enforced whenever a shared secret is configured — it must
        // NOT be bypassable via SUCO_IGNORE_VERSION.
        std::string secret = suco::get_shared_secret();
        bool auth_required = !secret.empty();

        if (type == PACKET_HELLO) {
            if (handshake_done) {
                SUCO_LOG_ERROR("Coordinator: Unexpected duplicate PACKET_HELLO from client {}", client_ip);
                close_socket(client_sock);
                return;
            }
            uint32_t client_version_net = 0;
            if (!read_all(client_sock, &client_version_net, 4)) {
                close_socket(client_sock);
                return;
            }
            uint32_t client_version = ntohl(client_version_net);

            uint32_t coord_version = 200;
            const char* force_ver_env = std::getenv("SUCO_FORCE_PROTOCOL_VERSION");
            if (force_ver_env) {
                try {
                    coord_version = std::stoul(force_ver_env);
                } catch (...) {}
            }

            uint32_t resp_type_net = htonl(PACKET_HELLO);
            uint32_t resp_version_net = htonl(coord_version);
            send_all(client_sock, &resp_type_net, 4);
            send_all(client_sock, &resp_version_net, 4);

            // --- Shared-secret authentication (HMAC challenge-response) ---
            // Active only when SUCO_SECRET is set on the coordinator. A client with the
            // same secret answers the nonce with HMAC-SHA256(secret, nonce). Anyone
            // without the secret (or an old binary) cannot produce a valid MAC and is
            // dropped. No traffic encryption — that is TLS, a separate step.
            {
                if (auth_required) {
                    std::string nonce = suco::generate_nonce();
                    uint32_t nlen = htonl(static_cast<uint32_t>(nonce.size()));
                    if (nonce.empty() || !send_all(client_sock, &nlen, 4) ||
                        !send_all(client_sock, nonce.data(), nonce.size())) {
                        close_socket(client_sock); return;
                    }
                    uint32_t mlen_net = 0;
                    if (!read_all(client_sock, &mlen_net, 4)) { close_socket(client_sock); return; }
                    uint32_t mlen = ntohl(mlen_net);
                    if (mlen == 0 || mlen > 256) {
                        SUCO_LOG_ERROR("Coordinator: AUTH FAILED (bad MAC length) from client {}", client_ip);
                        close_socket(client_sock); return;
                    }
                    std::string mac(mlen, '\0');
                    if (!read_all(client_sock, mac.data(), mlen)) { close_socket(client_sock); return; }
                    if (!suco::constant_time_equals(mac, suco::hmac_sha256_hex(secret, nonce))) {
                        SUCO_LOG_ERROR("Coordinator: AUTH FAILED for client {} — closing connection", client_ip);
                        close_socket(client_sock); return;
                    }
                }
            }

            if (client_version != 200 && !ignore_version) {
                SUCO_LOG_ERROR("Coordinator: Version mismatch. Client version {}, Coordinator version {}. Closing connection.", client_version, coord_version);
                close_socket(client_sock);
                return;
            }

            handshake_done = true;
            continue;
        } else {
            // Reject any request before a completed handshake when either version
            // checking OR authentication is in force. auth_required must win even if
            // SUCO_IGNORE_VERSION is set, otherwise auth could be skipped entirely.
            if (!handshake_done && (!ignore_version || auth_required)) {
                SUCO_LOG_ERROR("Coordinator: {} from client {} — no completed handshake, dropping (type {})",
                               auth_required ? "AUTH REQUIRED" : "Handshake failed", client_ip, type);
                close_socket(client_sock);
                return;
            }
        }

        // --- Generic content-addressed blob cache (team-wide `suco run` result cache) ---
        if (type == PACKET_BLOB_QUERY) {
            uint32_t hl = 0; if (!read_all(client_sock, &hl, 4)) { close_socket(client_sock); return; }
            hl = ntohl(hl);
            std::vector<char> hb(hl);
            if (hl > 0 && !read_all(client_sock, hb.data(), hl)) { close_socket(client_sock); return; }
            std::string bhash(hb.data(), hl);
            std::vector<uint8_t> blob; std::string blog; uint8_t bcomp = 0;
            bool hit = m_cache && m_cache->get(bhash, blob, blog, bcomp);
            uint8_t hitb = hit ? 1 : 0;
            if (!send_all(client_sock, &hitb, 1)) { close_socket(client_sock); return; }
            if (hit) {
                uint32_t blen = htonl(static_cast<uint32_t>(blob.size()));
                if (!send_all(client_sock, &blen, 4) ||
                    (!blob.empty() && !send_all(client_sock, blob.data(), blob.size()))) { close_socket(client_sock); return; }
            }
            continue;
        }
        if (type == PACKET_BLOB_STORE) {
            uint32_t hl = 0; if (!read_all(client_sock, &hl, 4)) { close_socket(client_sock); return; }
            hl = ntohl(hl);
            std::vector<char> hb(hl);
            if (hl > 0 && !read_all(client_sock, hb.data(), hl)) { close_socket(client_sock); return; }
            std::string bhash(hb.data(), hl);
            uint32_t blen = 0; if (!read_all(client_sock, &blen, 4)) { close_socket(client_sock); return; }
            blen = ntohl(blen);
            if (blen > (512u << 20)) { close_socket(client_sock); return; }
            std::vector<uint8_t> blob(blen);
            if (blen > 0 && !read_all(client_sock, blob.data(), blen)) { close_socket(client_sock); return; }
            if (m_cache) m_cache->put(bhash, blob, "", 0, "suco-run", "", "\"node\": \"run\"");
            uint8_t ack = 1;
            send_all(client_sock, &ack, 1);
            continue;
        }

    if (type == PACKET_CACHE_STORE) {
        uint32_t hash_len_net = 0;
        if (!read_all(client_sock, &hash_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t hash_len = ntohl(hash_len_net);
        std::vector<char> hash_buf(hash_len);
        if (hash_len > 0) {
            if (!read_all(client_sock, hash_buf.data(), hash_len)) { close_socket(client_sock); return; }
        }
        std::string hash(hash_buf.data(), hash_len);

        int32_t exit_code_net = 0;
        if (!read_all(client_sock, &exit_code_net, 4)) { close_socket(client_sock); return; }
        int32_t exit_code = ntohl(exit_code_net);

        uint32_t log_len_net = 0;
        if (!read_all(client_sock, &log_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t log_len = ntohl(log_len_net);
        std::vector<char> log_buf(log_len);
        if (log_len > 0) {
            if (!read_all(client_sock, log_buf.data(), log_len)) { close_socket(client_sock); return; }
        }
        std::string log(log_buf.data(), log_len);

        uint8_t bin_comp = 0;
        if (!read_all(client_sock, &bin_comp, 1)) { close_socket(client_sock); return; }

        uint32_t bin_len_net = 0;
        if (!read_all(client_sock, &bin_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t bin_len = ntohl(bin_len_net);
        std::vector<uint8_t> binary(bin_len);
        if (bin_len > 0) {
            if (!read_all(client_sock, binary.data(), bin_len)) { close_socket(client_sock); return; }
        }

        // Dekomprimiere, falls komprimiert übertragen
        if (bin_comp == 1) {
            std::string comp_str(reinterpret_cast<const char*>(binary.data()), binary.size());
            std::string decomp_str = suco::decompress_zstd(comp_str);
            binary.assign(decomp_str.begin(), decomp_str.end());
            bin_comp = 0;
        }

        // In Cache eintragen, falls exit_code == 0
        if (exit_code == 0 && m_cache) {
            m_cache->put(hash, binary, log, bin_comp, "", "", "\"node\": \"local\"");
            SUCO_LOG_INFO("Coordinator: Stored cache entry for hash {}", hash);
        }

        // Dekrementiere slots_used auf dem zuständigen Worker
        std::shared_ptr<WorkerNode> worker_node;
        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            auto it = m_state.hash_to_worker.find(hash);
            if (it != m_state.hash_to_worker.end()) {
                worker_node = it->second;
                m_state.hash_to_worker.erase(it);
            }
        }
        if (worker_node) {
            worker_node->slots_used = std::max(0, worker_node->slots_used - 1);
            if (g_slot_dbg) SUCO_LOG_INFO("[SLOT] RELEASE worker={} used={}/{} hash={}",
                                          worker_node->name, worker_node->slots_used,
                                          worker_node->slots_total, hash.substr(0, 8));
            notify_slot_free();   // wake a PUSH-waiting query to claim this freed slot
        } else if (g_slot_dbg) {
            SUCO_LOG_INFO("[SLOT] RELEASE-MISS hash={} (no reservation to release)", hash.substr(0, 8));
        }

        // Warte-Clients auflösen
        std::vector<socket_t> waiting_clients;
        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            auto it = m_state.pending_compilations.find(hash);
            if (it != m_state.pending_compilations.end()) {
                waiting_clients = std::move(it->second);
                m_state.pending_compilations.erase(it);
            }
        }

        uint32_t resp_type = htonl(PACKET_COMPILE_RESP);
        uint32_t log_len_out = htonl(static_cast<uint32_t>(log.size()));
        int32_t exit_code_out = htonl(exit_code);
        uint32_t bin_len_out = htonl(static_cast<uint32_t>(binary.size()));
        uint32_t hc_hit = htonl(0);

        for (socket_t waiter : waiting_clients) {
            uint32_t fn_len = htonl(0);
            uint8_t waiter_bin_comp = 0;
            send_all(waiter, &resp_type, 4);
            send_all(waiter, &fn_len, 4);
            send_all(waiter, &exit_code_out, 4);
            send_all(waiter, &log_len_out, 4);
            if (!log.empty()) {
                send_all(waiter, log.c_str(), log.size());
            }
            send_all(waiter, &waiter_bin_comp, 1);
            send_all(waiter, &bin_len_out, 4);
            if (!binary.empty()) {
                send_all(waiter, binary.data(), binary.size());
            }
            send_all(waiter, &hc_hit, 4);
            close_socket(waiter);
        }

        // Logge das Event über HistoryWriter in SQLite und broadcasten via SSE
        int64_t queue_start_ms = 0;
        std::string filename = "";
        {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            auto it_t = m_state.hash_to_start_time.find(hash);
            if (it_t != m_state.hash_to_start_time.end()) {
                queue_start_ms = it_t->second;
                m_state.hash_to_start_time.erase(it_t);
            }
            auto it_f = m_state.hash_to_filename.find(hash);
            if (it_f != m_state.hash_to_filename.end()) {
                filename = it_f->second;
                m_state.hash_to_filename.erase(it_f);
            }
        }

        if (queue_start_ms > 0 && !filename.empty()) {
            int64_t result_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            int64_t total_duration_ms = result_ms - queue_start_ms;

            uint64_t job_id = 0;
            {
                std::lock_guard<std::mutex> lock(m_state.mutex);
                job_id = m_state.next_job_id++;
                
                std::string uploader = (client_ip.empty() || client_ip == "127.0.0.1")
                                           ? "local" : client_ip;
                std::string node_name = worker_node ? worker_node->name : uploader;
                RecentJob rj{ filename, exit_code, false, node_name };
                m_state.recent_jobs.push_back(rj);
                if (m_state.recent_jobs.size() > 20) {
                    m_state.recent_jobs.erase(m_state.recent_jobs.begin());
                }
            }

            if (m_state.history_writer) {
                HistoryWriter::Event ev;
                ev.job_id = job_id;
                ev.source_file = filename;
                ev.content_hash = hash;
                ev.cache_hit = false;
                ev.queue_start_ms = queue_start_ms;
                ev.result_ms = result_ms;
                ev.total_duration_ms = total_duration_ms;
                // Direct-dispatch/store path: the coordinator never sees a separate
                // compile phase (the client compiled straight on the worker and only
                // uploads the .o). The measured total IS effectively the compile cost,
                // and it's what a future cache hit of this hash avoids. Record it as
                // compile_duration_ms so get_time_saved_today_ms() can attribute the
                // saving — otherwise "time saved" stays 0 for every direct-dispatched
                // build (compile_duration_ms would default to 0 and the correlated
                // lookup finds nothing).
                ev.compile_duration_ms = total_duration_ms;
                ev.worker_name = worker_node ? worker_node->name : "";  // attribute to worker (D2 timeline)
                ev.exit_code = exit_code;
                m_state.history_writer->enqueue(std::move(ev));
            }

            auto escape_json = [](const std::string& s) {
                std::string res;
                for (char c : s) {
                    if (c == '\\') res += "\\\\";
                    else if (c == '"') res += "\\\"";
                    else if (c == '\n') res += "\\n";
                    else if (c == '\r') res += "\\r";
                    else if (c == '\t') res += "\\t";
                    else res += c;
                }
                return res;
            };

            std::stringstream json;
            json << "{"
                 << "\"job_id\":" << job_id << ","
                 << "\"file\":\"" << escape_json(filename) << "\","
                 << "\"worker\":\"" << (worker_node ? escape_json(worker_node->name) : "direct") << "\","
                 << "\"cache_hit\":false,"
                 << "\"queue_ms\":0,"
                 << "\"compile_ms\":" << total_duration_ms << ","
                 << "\"total_ms\":" << total_duration_ms << ","
                 << "\"exit_code\":" << exit_code
                 << "}";
            m_state.broadcast_sse_event("job_complete", json.str());
        }

        // Antworte ACK an den Client
        uint32_t ack_type = htonl(PACKET_TOOLCHAIN_ACK);
        send_all(client_sock, &ack_type, 4);
        close_socket(client_sock);
        return;
    }

    if (type == PACKET_CACHE_QUERY) {
        auto job_start_system = std::chrono::system_clock::now();
        auto job_start_steady = std::chrono::steady_clock::now();

        uint32_t hash_len = 0;
        if (!read_all(client_sock, &hash_len, 4)) {
            close_socket(client_sock);
            return;
        }
        hash_len = ntohl(hash_len);
        std::vector<char> hash_buf(hash_len);
        if (!read_all(client_sock, hash_buf.data(), hash_len)) {
            close_socket(client_sock);
            return;
        }
        std::string hash(hash_buf.data(), hash_len);

        uint32_t query_file_len = 0;
        if (!read_all(client_sock, &query_file_len, 4)) {
            close_socket(client_sock);
            return;
        }
        query_file_len = ntohl(query_file_len);
        std::vector<char> query_file_buf(query_file_len);
        if (!read_all(client_sock, query_file_buf.data(), query_file_len)) {
            close_socket(client_sock);
            return;
        }
        std::string query_filename(query_file_buf.data(), query_file_len);

        uint32_t tc_hash_len_net = 0;
        if (!read_all(client_sock, &tc_hash_len_net, 4)) {
            close_socket(client_sock);
            return;
        }
        uint32_t tc_hash_len = ntohl(tc_hash_len_net);
        std::vector<char> tc_hash_buf(tc_hash_len);
        if (tc_hash_len > 0) {
            if (!read_all(client_sock, tc_hash_buf.data(), tc_hash_len)) {
                close_socket(client_sock);
                return;
            }
        }
        std::string toolchain_hash(tc_hash_buf.data(), tc_hash_len);

        uint32_t query_req_comp_len_net = 0;
        if (!read_all(client_sock, &query_req_comp_len_net, 4)) {
            close_socket(client_sock);
            return;
        }
        uint32_t query_req_comp_len = ntohl(query_req_comp_len_net);
        std::vector<char> query_req_comp_buf(query_req_comp_len);
        if (query_req_comp_len > 0) {
            if (!read_all(client_sock, query_req_comp_buf.data(), query_req_comp_len)) {
                close_socket(client_sock);
                return;
            }
        }
        std::string query_required_compiler(query_req_comp_buf.data(), query_req_comp_len);

        uint32_t query_req_comp_ver_len_net = 0;
        if (!read_all(client_sock, &query_req_comp_ver_len_net, 4)) {
            close_socket(client_sock);
            return;
        }
        uint32_t query_req_comp_ver_len = ntohl(query_req_comp_ver_len_net);
        std::vector<char> query_req_comp_ver_buf(query_req_comp_ver_len);
        if (query_req_comp_ver_len > 0) {
            if (!read_all(client_sock, query_req_comp_ver_buf.data(), query_req_comp_ver_len)) {
                close_socket(client_sock);
                return;
            }
        }
        std::string query_required_compiler_version(query_req_comp_ver_buf.data(), query_req_comp_ver_len);

        uint32_t hs_hash_len_net = 0;
        if (!read_all(client_sock, &hs_hash_len_net, 4)) {
            close_socket(client_sock);
            return;
        }
        uint32_t query_hs_hash_len = ntohl(hs_hash_len_net);
        std::vector<char> query_hs_hash_buf(query_hs_hash_len);
        if (query_hs_hash_len > 0) {
            if (!read_all(client_sock, query_hs_hash_buf.data(), query_hs_hash_len)) {
                close_socket(client_sock);
                return;
            }
        }
        std::string query_header_set_hash(query_hs_hash_buf.data(), query_hs_hash_len);

        if (!toolchain_hash.empty()) {
            static std::mutex toolchain_upload_mutex;
            std::lock_guard<std::mutex> upload_lock(toolchain_upload_mutex);
            if (!coordinator_has_toolchain(toolchain_hash)) {
                SUCO_LOG_INFO("Coordinator: Toolchain {} not cached. Requesting from client...", toolchain_hash);
                uint32_t req_type = htonl(PACKET_TOOLCHAIN_REQUEST);
                uint32_t hash_len_net = htonl(toolchain_hash.size());
                if (send_all(client_sock, &req_type, 4) &&
                    send_all(client_sock, &hash_len_net, 4) &&
                    send_all(client_sock, toolchain_hash.c_str(), toolchain_hash.size())) {
                    
                    uint32_t resp_type_net = 0;
                    if (read_all(client_sock, &resp_type_net, 4)) {
                        uint32_t resp_type = ntohl(resp_type_net);
                        if (resp_type == PACKET_TOOLCHAIN_TRANSFER) {
                            std::string cache_dir = get_toolchain_cache_dir();
                            std::error_code ec;
                            std::filesystem::create_directories(cache_dir, ec);
                            std::string target_archive = cache_dir + "/toolchain-" + toolchain_hash + ".tar.zst";
                            if (receive_file(client_sock, target_archive)) {
                                SUCO_LOG_INFO("Coordinator: Successfully received toolchain {} from client", toolchain_hash);
                                uint32_t ack_type = htonl(PACKET_TOOLCHAIN_ACK);
                                send_all(client_sock, &ack_type, 4);
                            } else {
                                SUCO_LOG_ERROR("Coordinator: Failed to receive toolchain file {}", toolchain_hash);
                            }
                        }
                    }
                }
            }
        }

        std::vector<uint8_t> cached_obj;
        std::string cached_log;
        uint8_t cached_bin_comp = 0;
        bool cache_found = false;
        if (m_cache) {
            cache_found = m_cache->get(hash, cached_obj, cached_log, cached_bin_comp);
        }

        if (cache_found) {
            SUCO_LOG_INFO("Cache HIT for job {} (hash: {})", query_filename, hash);
            {
                std::lock_guard<std::mutex> lock(m_state.mutex);
                m_state.total_requests++;
                m_state.cache_hits++;
                
                std::string origin_node = "";
                if (m_cache) {
                    origin_node = m_cache->get_meta_node(hash);
                }
                std::string display_name = "Cache";
                if (!origin_node.empty()) {
                    if (origin_node == "127.0.0.1") {
                        origin_node = "local";
                    }
                    display_name += " (from " + origin_node + ")";
                }
                RecentJob rj{ query_filename, 0, true, display_name };
                m_state.recent_jobs.push_back(rj);
                if (m_state.recent_jobs.size() > 20) {
                    m_state.recent_jobs.erase(m_state.recent_jobs.begin());
                }
            }
            uint32_t resp_type = htonl(PACKET_CACHE_HIT);
            uint32_t log_len = htonl(static_cast<uint32_t>(cached_log.size()));
            uint32_t bin_len = htonl(static_cast<uint32_t>(cached_obj.size()));

            send_all(client_sock, &resp_type, 4);
            send_all(client_sock, &log_len, 4);
            if (!cached_log.empty()) {
                send_all(client_sock, cached_log.c_str(), cached_log.size());
            }
            send_all(client_sock, &cached_bin_comp, 1);
            send_all(client_sock, &bin_len, 4);
            if (!cached_obj.empty()) {
                send_all(client_sock, cached_obj.data(), cached_obj.size());
            }

            // T11: Logge Cache HIT
            auto result_steady = std::chrono::steady_clock::now();
            uint64_t job_id_hit = 0;
            {
                std::lock_guard<std::mutex> lock(m_state.mutex);
                job_id_hit = m_state.next_job_id++;
            }

            int64_t queue_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                job_start_system.time_since_epoch()).count();
            int64_t result_ms = queue_start_ms + std::chrono::duration_cast<std::chrono::milliseconds>(
                result_steady - job_start_steady).count();
            int64_t total_duration_ms = result_ms - queue_start_ms;

            if (m_state.history_writer) {
                HistoryWriter::Event ev;
                ev.job_id = job_id_hit;
                ev.source_file = query_filename;
                ev.content_hash = hash;
                ev.cache_hit = true;
                ev.queue_start_ms = queue_start_ms;
                ev.result_ms = result_ms;
                ev.total_duration_ms = total_duration_ms;
                ev.exit_code = 0;
                m_state.history_writer->enqueue(std::move(ev));
            }

            auto escape_json = [](const std::string& s) {
                std::string res;
                for (char c : s) {
                    if (c == '\\') res += "\\\\";
                    else if (c == '"') res += "\\\"";
                    else if (c == '\n') res += "\\n";
                    else if (c == '\r') res += "\\r";
                    else if (c == '\t') res += "\\t";
                    else res += c;
                }
                return res;
            };

            std::stringstream json;
            json << "{"
                 << "\"job_id\":" << job_id_hit << ","
                 << "\"file\":\"" << escape_json(query_filename) << "\","
                 << "\"worker\":\"\","
                 << "\"cache_hit\":true,"
                 << "\"queue_ms\":0,"
                 << "\"compile_ms\":0,"
                 << "\"total_ms\":" << total_duration_ms << ","
                 << "\"exit_code\":0"
                 << "}";
            m_state.broadcast_sse_event("job_complete", json.str());

            continue;
        }

        SUCO_LOG_INFO("Cache MISS for job {} (hash: {})", query_filename, hash);

        if (query_filename == "dummy.cpp" || hash.rfind("dummy-tc-check-", 0) == 0) {
            m_state.mutex.lock();
            m_state.total_requests++;
            m_state.cache_misses++;
            m_state.mutex.unlock();
            uint32_t resp_type = htonl(PACKET_CACHE_MISS);
            if (!send_all(client_sock, &resp_type, 4)) {
                close_socket(client_sock);
                return;
            }
            continue;
        }

        m_state.mutex.lock();
        auto it = m_state.pending_compilations.find(hash);
        if (it != m_state.pending_compilations.end()) {
            it->second.push_back(client_sock);
            m_state.total_requests++;
            m_state.cache_misses++;
            m_state.mutex.unlock();
            
            uint32_t resp_type = htonl(PACKET_CACHE_WAIT);
            send_all(client_sock, &resp_type, 4);
            return;
        }

        m_state.pending_compilations[hash] = {};
        m_state.total_requests++;
        m_state.cache_misses++;
        m_state.mutex.unlock();

        // 1. Wähle besten Worker für die direkte Kompilierung aus
        auto query_active_workers = m_worker_manager.get_active_workers();
        Job query_current_job;
        query_current_job.filename = query_filename;
        query_current_job.hash = hash;
        query_current_job.toolchain_hash = toolchain_hash;
        query_current_job.required_compiler = query_required_compiler;
        query_current_job.required_compiler_version = query_required_compiler_version;
        
        // PUSH SCHEDULING: pick a worker, holding the query (blocking this handler
        // thread) until one frees if the grid is momentarily full — instead of
        // returning "no worker" and making the client poll. select_best_worker +
        // slots_used++ run under g_assign_mutex so two waking waiters never grab the
        // same slot; a freed slot signals g_slot_cv. Bounded by g_push_wait_ms.
        std::shared_ptr<WorkerNode> query_best_worker;
        {
            std::unique_lock<std::mutex> alk(g_assign_mutex);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(g_push_wait_ms);
            bool waited = false;
            while (true) {
                query_best_worker = m_scheduler.select_best_worker(
                    m_worker_manager.get_active_workers(), query_current_job);
                if (query_best_worker) {
                    query_best_worker->slots_used++;   // reserve atomically with select
                    break;
                }
                if (g_push_wait_ms <= 0 || std::chrono::steady_clock::now() >= deadline) break;
                if (g_slot_dbg && !waited) {
                    SUCO_LOG_INFO("[SLOT] PUSH-WAIT hash={} (grid full, holding query)", hash.substr(0, 8));
                    waited = true;
                }
                // Wake on a slot release (notify_slot_free) or re-check every 50ms
                // (covers slots freed via heartbeat resync, which doesn't signal).
                g_slot_cv.wait_for(alk, std::chrono::milliseconds(50));
            }
        }

        std::string query_worker_ip = "";
        uint16_t direct_port = 0;

        if (query_best_worker) {
            // slots_used already incremented above under g_assign_mutex
            query_worker_ip = query_best_worker->ip;
            direct_port = query_best_worker->direct_port;
            if (g_slot_dbg) SUCO_LOG_INFO("[SLOT] RESERVE-Q worker={} used={}/{} hash={}",
                                          query_best_worker->name, query_best_worker->slots_used,
                                          query_best_worker->slots_total, hash.substr(0, 8));
            
            // Speichere die Zuordnung Worker -> Hash, um bei PACKET_CACHE_STORE den slot zu dekrementieren
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.hash_to_worker[hash] = query_best_worker;
            m_state.hash_to_start_time[hash] = std::chrono::duration_cast<std::chrono::milliseconds>(
                job_start_system.time_since_epoch()).count();
            m_state.hash_to_filename[hash] = query_filename;
            
            SUCO_LOG_INFO("Coordinator: Assigned hash {} directly to worker {} ({}:{})", 
                          hash, query_best_worker->name, query_worker_ip, direct_port);
        } else {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.hash_to_start_time[hash] = std::chrono::duration_cast<std::chrono::milliseconds>(
                job_start_system.time_since_epoch()).count();
            m_state.hash_to_filename[hash] = query_filename;
            SUCO_LOG_WARNING("Coordinator: No compatible worker found for hash {}", hash);
            if (g_slot_dbg) {
                std::string grid;
                for (const auto& w : query_active_workers)
                    grid += w->name + "=" + std::to_string(w->slots_used) + "/" + std::to_string(w->slots_total) + " ";
                SUCO_LOG_INFO("[SLOT] NOWORKER-Q hash={} grid=[ {}]", hash.substr(0, 8), grid);
            }
        }

        // 2. Sende PACKET_CACHE_MISS
        uint32_t resp_type = htonl(PACKET_CACHE_MISS);
        if (!send_all(client_sock, &resp_type, 4)) {
            if (query_best_worker) {
                query_best_worker->slots_used = std::max(0, query_best_worker->slots_used - 1);
                notify_slot_free();
            }
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }

        // 3. Sende Worker IP und Port
        uint32_t ip_len = htonl(static_cast<uint32_t>(query_worker_ip.size()));
        uint16_t port_net = htons(direct_port);
        
        uint8_t hs_known = 0;
        if (!query_header_set_hash.empty() && query_best_worker) {
            std::lock_guard<std::mutex> khs_lock(query_best_worker->known_header_sets_mutex);
            if (query_best_worker->known_header_sets.count(query_header_set_hash) > 0) {
                hs_known = 1;
            }
        }
        
        if (!send_all(client_sock, &ip_len, 4) ||
            (ip_len > 0 && !send_all(client_sock, query_worker_ip.c_str(), query_worker_ip.size())) ||
            !send_all(client_sock, &port_net, 2) ||
            !send_all(client_sock, &hs_known, 1)) {
            if (query_best_worker) {
                query_best_worker->slots_used = std::max(0, query_best_worker->slots_used - 1);
                notify_slot_free();
            }
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }

        close_socket(client_sock);
        return;

        uint32_t compile_req_type_net = 0;
        if (!read_all(client_sock, &compile_req_type_net, 4)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }

        uint32_t cmd_len_net = 0, file_len_net = 0, src_len_net = 0;
        if (!read_all(client_sock, &cmd_len_net, 4)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t cmd_len = ntohl(cmd_len_net);
        std::vector<char> cmd_buf(cmd_len);
        read_all(client_sock, cmd_buf.data(), cmd_len);
        std::string command(cmd_buf.data(), cmd_len);

        if (!read_all(client_sock, &file_len_net, 4)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t file_len = ntohl(file_len_net);
        std::vector<char> file_buf(file_len);
        read_all(client_sock, file_buf.data(), file_len);
        std::string filename(file_buf.data(), file_len);

        uint8_t src_comp = 0;
        if (!read_all(client_sock, &src_comp, 1)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }

        if (!read_all(client_sock, &src_len_net, 4)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t src_len = ntohl(src_len_net);
        std::vector<char> src_buf(src_len);
        if (src_len > 0) {
            read_all(client_sock, src_buf.data(), src_len);
        }
        std::string source(src_buf.data(), src_len);

        uint32_t req_comp_len_net = 0;
        if (!read_all(client_sock, &req_comp_len_net, 4)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t req_comp_len = ntohl(req_comp_len_net);
        std::vector<char> req_comp_buf(req_comp_len);
        if (req_comp_len > 0) {
            read_all(client_sock, req_comp_buf.data(), req_comp_len);
        }
        std::string required_compiler(req_comp_buf.data(), req_comp_len);

        uint32_t req_comp_ver_len_net = 0;
        if (!read_all(client_sock, &req_comp_ver_len_net, 4)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t req_comp_ver_len = ntohl(req_comp_ver_len_net);
        std::vector<char> req_comp_ver_buf(req_comp_ver_len);
        if (req_comp_ver_len > 0) {
            read_all(client_sock, req_comp_ver_buf.data(), req_comp_ver_len);
        }
        std::string required_compiler_version(req_comp_ver_buf.data(), req_comp_ver_len);

        uint32_t tc_hash_len_net_req = 0;
        if (!read_all(client_sock, &tc_hash_len_net_req, 4)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t tc_hash_len_req = ntohl(tc_hash_len_net_req);
        std::vector<char> tc_hash_buf_req(tc_hash_len_req);
        if (tc_hash_len_req > 0) {
            read_all(client_sock, tc_hash_buf_req.data(), tc_hash_len_req);
        }
        std::string toolchain_hash_req(tc_hash_buf_req.data(), tc_hash_len_req);

        uint32_t hs_hash_len_net_req = 0;
        if (!read_all(client_sock, &hs_hash_len_net_req, 4)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t hs_hash_len = ntohl(hs_hash_len_net_req);
        std::vector<char> hs_hash_buf(hs_hash_len);
        if (hs_hash_len > 0) {
            read_all(client_sock, hs_hash_buf.data(), hs_hash_len);
        }
        std::string header_set_hash(hs_hash_buf.data(), hs_hash_len);

        uint8_t hs_comp = 0;
        if (!read_all(client_sock, &hs_comp, 1)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }

        uint32_t hs_src_len_net_req = 0;
        if (!read_all(client_sock, &hs_src_len_net_req, 4)) {
            abort_waiting_clients(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t hs_src_len = ntohl(hs_src_len_net_req);
        std::vector<char> hs_src_buf(hs_src_len);
        if (hs_src_len > 0) {
            read_all(client_sock, hs_src_buf.data(), hs_src_len);
        }
        std::string header_set_source(hs_src_buf.data(), hs_src_len);

        // Registriere Job-Details für eventuelles Failover/Rescheduling
        {
            std::lock_guard<std::mutex> lock(m_state.running_details_mutex);
            m_state.running_job_details[filename] = RunningJobDetail{ hash, command, source, client_sock, 1, header_set_hash };
        }

        // Verwende den ausgelagerten WorkerManager und den Scheduler
        auto active_workers = m_worker_manager.get_active_workers();
        Job current_job;
        current_job.filename = filename;
        current_job.hash = hash;
        current_job.command = command;
        current_job.source = source;
        current_job.source_compressed = (src_comp != 0);
        current_job.required_compiler = required_compiler;
        current_job.required_compiler_version = required_compiler_version;
        current_job.toolchain_hash = toolchain_hash_req;
        current_job.header_set_hash = header_set_hash;
        current_job.header_set_source = header_set_source;
        current_job.hs_compressed = (hs_comp != 0);

        auto best_worker = m_scheduler.select_best_worker(active_workers, current_job);

        if (!best_worker) {
            // Kein Worker frei oder kompatibel
            SUCO_LOG_WARNING("Job {} cannot be scheduled: No compatible/free worker available (Required: '{}{}').",
                             filename, 
                             (required_compiler.empty() ? "any" : required_compiler),
                             (required_compiler_version.empty() ? "" : " v" + required_compiler_version));
            abort_waiting_clients(hash);

            {
                std::lock_guard<std::mutex> lock(m_state.running_details_mutex);
                m_state.running_job_details.erase(filename);
            }

            uint32_t resp_fail_type = htonl(PACKET_COMPILE_RESP);
            int32_t exit_code = htonl(-1);
            uint32_t log_len = htonl(0);
            uint32_t bin_len = htonl(0);
            send_all(client_sock, &resp_fail_type, 4);
            send_all(client_sock, &exit_code, 4);
            send_all(client_sock, &log_len, 4);
            send_all(client_sock, &bin_len, 4);
            close_socket(client_sock);
            return;
        }

        // Worker slots updaten
        best_worker->slots_used++;
        socket_t worker_sock = best_worker->socket;
        std::string worker_ip = best_worker->ip;
        SUCO_LOG_INFO("Assigned job {} to worker {} (free slots before: {})", filename, worker_ip, best_worker->slots_total - best_worker->slots_used + 1);
        if (g_slot_dbg) SUCO_LOG_INFO("[SLOT] RESERVE-B worker={} used={}/{} file={}",
                                      best_worker->name, best_worker->slots_used, best_worker->slots_total, filename);

        // Get client IP address from socket
        std::string client_ip = "unknown";
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        if (getpeername(client_sock, (struct sockaddr*)&peer_addr, &peer_len) == 0) {
            client_ip = inet_ntoa(peer_addr.sin_addr);
        }

        m_state.mutex.lock();
        uint64_t job_id = m_state.next_job_id++;
        ActiveJob aj{ job_id, filename, worker_ip, client_ip, command, std::chrono::steady_clock::now() };
        m_state.active_jobs.push_back(aj);
        m_state.mutex.unlock();

        auto dispatch_steady = std::chrono::steady_clock::now();
        std::string worker_name = best_worker->name;

        auto res = std::make_shared<CompileResult>();
        {
            std::lock_guard<std::mutex> lock(m_state.results_mutex);
            m_state.compile_results[filename] = res;
        }

        bool send_ok = false;
        {
            std::lock_guard<std::mutex> lock(best_worker->write_mutex);
            uint32_t w_req_type = htonl(PACKET_COMPILE_REQ);
            uint32_t tc_hash_len_net_worker = htonl(toolchain_hash_req.size());
            uint32_t hs_hash_len_net_worker = htonl(header_set_hash.size());
            uint32_t hs_src_len_net_worker = htonl(header_set_source.size());
            if (send_all(worker_sock, &w_req_type, 4) &&
                send_all(worker_sock, &cmd_len_net, 4) &&
                send_all(worker_sock, command.c_str(), command.size()) &&
                send_all(worker_sock, &file_len_net, 4) &&
                send_all(worker_sock, filename.c_str(), filename.size()) &&
                send_all(worker_sock, &src_comp, 1) &&
                send_all(worker_sock, &src_len_net, 4) &&
                (src_len == 0 || send_all(worker_sock, source.c_str(), source.size())) &&
                send_all(worker_sock, &tc_hash_len_net_worker, 4) &&
                (toolchain_hash_req.empty() || send_all(worker_sock, toolchain_hash_req.c_str(), toolchain_hash_req.size())) &&
                send_all(worker_sock, &hs_hash_len_net_worker, 4) &&
                (header_set_hash.empty() || send_all(worker_sock, header_set_hash.c_str(), header_set_hash.size())) &&
                send_all(worker_sock, &hs_comp, 1) &&
                send_all(worker_sock, &hs_src_len_net_worker, 4) &&
                (hs_src_len == 0 || send_all(worker_sock, header_set_source.c_str(), header_set_source.size()))) {
                send_ok = true;
            }
        }

        if (!send_ok) {
            abort_waiting_clients(hash);
            m_state.mutex.lock();
            for (auto it = m_state.active_jobs.begin(); it != m_state.active_jobs.end(); ++it) {
                if (it->filename == filename) { m_state.active_jobs.erase(it); break; }
            }
            m_state.mutex.unlock();

            {
                std::lock_guard<std::mutex> res_lock(m_state.results_mutex);
                m_state.compile_results.erase(filename);
            }
            {
                std::lock_guard<std::mutex> lock(m_state.running_details_mutex);
                m_state.running_job_details.erase(filename);
            }

            uint32_t resp_fail_type = htonl(PACKET_COMPILE_RESP);
            int32_t exit_code = htonl(-1);
            uint32_t l_len = 0, b_len = 0;
            send_all(client_sock, &resp_fail_type, 4);
            send_all(client_sock, &exit_code, 4);
            send_all(client_sock, &l_len, 4);
            send_all(client_sock, &b_len, 4);
            close_socket(client_sock);
            return;
        }

        // Warte auf Fertigstellung
        std::unique_lock<std::mutex> res_lock(res->mutex);
        res->cv.wait(res_lock, [&]() { return res->ready; });

        int32_t exit_code = res->exit_code;
        std::string log_str = res->log;
        std::vector<uint8_t> bin_data = res->bin;
        uint8_t bin_comp = res->bin_comp;
        res_lock.unlock();

        auto compile_end_steady = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> map_lock(m_state.results_mutex);
            m_state.compile_results.erase(filename);
        }
        {
            std::lock_guard<std::mutex> lock(m_state.running_details_mutex);
            m_state.running_job_details.erase(filename);
        }

        // Slots freigeben
        auto workers_list = m_worker_manager.get_active_workers();
        for (auto& w : workers_list) {
            if (w->socket == worker_sock) {
                worker_name = w->name;
                w->slots_used = std::max(0, w->slots_used - 1);
                notify_slot_free();   // wake a PUSH-waiting query
                break;
            }
        }

        if (exit_code == 0 && !bin_data.empty() && m_cache) {
            m_cache->put(hash, bin_data, log_str, bin_comp, filename, command, "\"node\": \"" + worker_name + "\"");
        }

        m_state.mutex.lock();
        for (auto it = m_state.active_jobs.begin(); it != m_state.active_jobs.end(); ++it) {
            if (it->filename == filename) {
                m_state.active_jobs.erase(it);
                break;
            }
        }

        RecentJob rj{ filename, exit_code, false, worker_name };
        m_state.recent_jobs.push_back(rj);
        if (m_state.recent_jobs.size() > 20) {
            m_state.recent_jobs.erase(m_state.recent_jobs.begin());
        }

        int32_t exit_code_net = htonl(exit_code);
        uint32_t log_len_net = htonl(static_cast<uint32_t>(log_str.size()));
        uint32_t bin_len_net = htonl(static_cast<uint32_t>(bin_data.size()));

        uint32_t resp_type_net = htonl(PACKET_COMPILE_RESP);
        uint32_t hc_hit_net = htonl(res->header_cache_hit ? 1 : 0);
        send_all(client_sock, &resp_type_net, 4);
        send_all(client_sock, &exit_code_net, 4);
        send_all(client_sock, &log_len_net, 4);
        if (!log_str.empty()) send_all(client_sock, log_str.c_str(), log_str.size());
        send_all(client_sock, &res->bin_comp, 1);
        send_all(client_sock, &bin_len_net, 4);
        if (!bin_data.empty()) send_all(client_sock, bin_data.data(), bin_data.size());
        send_all(client_sock, &hc_hit_net, 4);

        auto result_steady = std::chrono::steady_clock::now();

        int64_t queue_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            job_start_system.time_since_epoch()).count();
        
        int64_t dispatch_ms_val = queue_start_ms + std::chrono::duration_cast<std::chrono::milliseconds>(
            dispatch_steady - job_start_steady).count();
        int64_t compile_end_ms_val = queue_start_ms + std::chrono::duration_cast<std::chrono::milliseconds>(
            compile_end_steady - job_start_steady).count();
        int64_t result_ms_val = queue_start_ms + std::chrono::duration_cast<std::chrono::milliseconds>(
            result_steady - job_start_steady).count();

        int64_t queue_duration_ms = dispatch_ms_val - queue_start_ms;
        int64_t compile_duration_ms = compile_end_ms_val - dispatch_ms_val;
        int64_t total_duration_ms = result_ms_val - queue_start_ms;

        if (m_state.history_writer) {
            HistoryWriter::Event ev;
            ev.job_id = job_id;
            ev.source_file = filename;
            ev.content_hash = hash;
            ev.worker_name = worker_name;
            ev.cache_hit = false;
            ev.queue_start_ms = queue_start_ms;
            ev.dispatch_ms = dispatch_ms_val;
            ev.compile_end_ms = compile_end_ms_val;
            ev.result_ms = result_ms_val;
            ev.queue_duration_ms = queue_duration_ms;
            ev.compile_duration_ms = compile_duration_ms;
            ev.total_duration_ms = total_duration_ms;
            ev.exit_code = exit_code;
            m_state.history_writer->enqueue(std::move(ev));
        }

        auto escape_json = [](const std::string& s) {
            std::string res;
            for (char c : s) {
                if (c == '\\') res += "\\\\";
                else if (c == '"') res += "\\\"";
                else if (c == '\n') res += "\\n";
                else if (c == '\r') res += "\\r";
                else if (c == '\t') res += "\\t";
                else res += c;
            }
            return res;
        };

        std::stringstream json;
        json << "{"
             << "\"job_id\":" << job_id << ","
             << "\"file\":\"" << escape_json(filename) << "\","
             << "\"worker\":\"" << escape_json(worker_name) << "\","
             << "\"cache_hit\":false,"
             << "\"queue_ms\":" << queue_duration_ms << ","
             << "\"compile_ms\":" << compile_duration_ms << ","
             << "\"total_ms\":" << total_duration_ms << ","
             << "\"exit_code\":" << exit_code
             << "}";
        m_state.broadcast_sse_event(exit_code == 0 ? "job_complete" : "job_failed", json.str());

        auto waiting_clients = m_state.pending_compilations[hash];
        m_state.pending_compilations.erase(hash);
        m_state.mutex.unlock();

        // Benachrichtige wartende Clients
        for (auto s : waiting_clients) {
            uint32_t wait_resp = htonl(PACKET_COMPILE_RESP);
            send_all(s, &wait_resp, 4);
            send_all(s, &exit_code_net, 4);
            send_all(s, &log_len_net, 4);
            if (!log_str.empty()) send_all(s, log_str.c_str(), log_str.size());
            send_all(s, &res->bin_comp, 1);
            send_all(s, &bin_len_net, 4);
            if (!bin_data.empty()) send_all(s, bin_data.data(), bin_data.size());
            send_all(s, &hc_hit_net, 4);
            close_socket(s);
        }
        continue;
    } else if (type == PACKET_COMPILE_BATCH_REQ) {
        handle_batch_compile_request(client_sock, client_ip);
        return;
    } else {
        SUCO_LOG_ERROR("Coordinator: Unexpected packet type {} from {}", type, client_ip);
        close_socket(client_sock);
        return;
    }
    }
}

void ClientHandler::handle_batch_compile_request(socket_t client_sock, const std::string& client_ip) {
    BatchProcessor processor(m_config, m_job_queue, m_scheduler, m_worker_manager, m_state, m_cache);
    processor.process_batch_request(client_sock, client_ip);
}

void ClientHandler::abort_waiting_clients(const std::string& hash) {
    std::vector<socket_t> waiting;
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        auto it = m_state.pending_compilations.find(hash);
        if (it != m_state.pending_compilations.end()) {
            waiting = std::move(it->second);
            m_state.pending_compilations.erase(it);
        }
    }
    if (!waiting.empty()) {
        SUCO_LOG_WARNING("Aborting and closing {} waiting clients for hash {}", waiting.size(), hash);
        uint32_t resp_fail_type = htonl(PACKET_COMPILE_RESP);
        int32_t exit_code = htonl(-1);
        uint32_t l_len = 0;
        uint8_t bin_comp = 0;
        uint32_t b_len = 0;
        uint32_t hc_hit = 0;
        for (auto s : waiting) {
            send_all(s, &resp_fail_type, 4);
            send_all(s, &exit_code, 4);
            send_all(s, &l_len, 4);
            send_all(s, &bin_comp, 1);
            send_all(s, &b_len, 4);
            send_all(s, &hc_hit, 4);
            close_socket(s);
        }
    }
}

} // namespace suco
