#include "socket_util.h"

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <condition_variable>
#include <memory>

#include "protocol.h"
#include "lru_cache.h"

namespace suco {

const std::string FALLBACK_DASHBOARD_HTML = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <title>SUCO Dashboard - Fallback</title>
    <style>
        body { font-family: sans-serif; background: #080b11; color: #e2e8f0; padding: 2rem; text-align: center; }
        h1 { color: #00f2fe; }
    </style>
</head>
<body>
    <h1>SUCO Grid Monitor</h1>
    <p>dashboard.html wurde nicht gefunden. Bitte platziere die dashboard.html im Arbeitsverzeichnis.</p>
</body>
</html>
)HTML";

struct WorkerNode {
    socket_t socket;
    std::mutex write_mutex;
    std::string ip;
    std::string name;
    std::string os;
    int slots_total;
    int slots_used;
    std::vector<double> cpu_cores_usage;
    std::chrono::steady_clock::time_point last_heartbeat;
};

struct CompileResult {
    bool ready = false;
    int32_t exit_code = 0;
    std::string log;
    std::vector<uint8_t> bin;
    std::condition_variable cv;
    std::mutex mutex;
};

std::mutex g_results_mutex;
std::unordered_map<std::string, std::shared_ptr<CompileResult>> g_compile_results;

// Active Compile Job representation for Stats API
struct ActiveJob {
    std::string filename;
    std::string worker_ip;
    std::chrono::steady_clock::time_point start_time;
};

struct RecentJob {
    std::string filename;
    int32_t exit_code;
    bool cache_hit;
};

// Coordinator Global State
struct CoordinatorState {
    std::mutex mutex;
    std::vector<std::shared_ptr<WorkerNode>> workers;
    uint64_t total_requests = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    
    // Map from file hash to a list of waiting client sockets (Request Coalescing)
    std::unordered_map<std::string, std::vector<socket_t>> pending_compilations;
    
    // Jobs currently running on workers
    std::vector<ActiveJob> active_jobs;
    
    // History of recently completed jobs
    std::vector<RecentJob> recent_jobs;
};


CoordinatorState g_state;
LruCache* g_cache = nullptr;

// UDP Broadcast Thread: Sends a discovery beacon every 3 seconds on Port 9002
void run_udp_broadcast(uint16_t tcp_port) {
    socket_t sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET_VAL) {
        std::cerr << "suco-coordinator error: Failed to create UDP socket." << std::endl;
        return;
    }

    // Enable broadcast
#ifdef _WIN32
    char broadcast_opt = '1';
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_opt, sizeof(broadcast_opt));
#else
    int broadcast_opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast_opt, sizeof(broadcast_opt));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST); // 255.255.255.255
    addr.sin_port = htons(suco::DEFAULT_UDP_PORT);

    std::string beacon = "SUCO_COORDINATOR_v1 " + std::to_string(tcp_port);
    std::cout << "suco-coordinator: UDP Auto-Discovery beacon active on Port " << suco::DEFAULT_UDP_PORT << std::endl;

    while (true) {
#ifdef _WIN32
        sendto(sock, beacon.c_str(), static_cast<int>(beacon.size()), 0, (struct sockaddr*)&addr, sizeof(addr));
#else
        sendto(sock, beacon.c_str(), beacon.size(), 0, (struct sockaddr*)&addr, sizeof(addr));
#endif
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    close_socket(sock);
}

// Background thread to monitor Worker health. Removes inactive nodes.
void run_worker_monitor() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::lock_guard<std::mutex> lock(g_state.mutex);
        auto now = std::chrono::steady_clock::now();
        
        for (auto it = g_state.workers.begin(); it != g_state.workers.end();) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - (*it)->last_heartbeat).count();
            if (elapsed > 12) {
                std::cout << "suco-coordinator: Worker " << (*it)->name << " (" << (*it)->ip 
                          << ") disconnected (heartbeat timeout)." << std::endl;
                close_socket((*it)->socket);
                it = g_state.workers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// REST API Server (Port 9001) for Dashboard
void run_web_server(uint16_t port) {
    socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET_VAL) {
        std::cerr << "suco-coordinator error: Failed to create web socket." << std::endl;
        return;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "suco-coordinator error: Web server bind failed on Port " << port << std::endl;
        close_socket(server_fd);
        return;
    }

    if (listen(server_fd, 10) < 0) {
        close_socket(server_fd);
        return;
    }

    std::cout << "suco-coordinator: Web dashboard REST API listening on Port " << port << std::endl;

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        socket_t client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET_VAL) continue;

        std::thread([client_sock]() {
            char buffer[2048];
            std::memset(buffer, 0, sizeof(buffer));
#ifdef _WIN32
            int read_bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
#else
            ssize_t read_bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
#endif
            if (read_bytes <= 0) {
                close_socket(client_sock);
                return;
            }

            std::string req(buffer);
            std::stringstream ss(req);
            std::string method, path;
            ss >> method >> path;

            if (method == "GET") {
                if (path == "/" || path == "/index.html" || path == "/dashboard.html") {
                    std::string html_content = suco::FALLBACK_DASHBOARD_HTML;
                    std::ifstream file("dashboard.html");
                    if (!file.is_open()) {
                        file.open("../dashboard.html");
                    }
                    if (file.is_open()) {
                        std::stringstream file_ss;
                        file_ss << file.rdbuf();
                        html_content = file_ss.str();
                        file.close();
                    }

                    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                                           std::to_string(html_content.size()) + "\r\nConnection: close\r\n\r\n" + html_content;
                    send(client_sock, response.c_str(), static_cast<int>(response.size()), 0);
                } else if (path == "/api/stats") {
                    std::stringstream json;
                    json << "{\n";
                    {
                        std::lock_guard<std::mutex> lock(g_state.mutex);
                        json << "  \"total_requests\": " << g_state.total_requests << ",\n";
                        json << "  \"cache_hits\": " << g_state.cache_hits << ",\n";
                        json << "  \"cache_misses\": " << g_state.cache_misses << ",\n";
                        json << "  \"active_jobs_count\": " << g_state.active_jobs.size() << ",\n";

                        // Active compile jobs
                        json << "  \"active_jobs\": [\n";
                        auto now = std::chrono::steady_clock::now();
                        for (size_t i = 0; i < g_state.active_jobs.size(); ++i) {
                            const auto& job = g_state.active_jobs[i];
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - job.start_time).count();
                            json << "    {\n";
                            json << "      \"filename\": \"" << job.filename << "\",\n";
                            json << "      \"worker_ip\": \"" << job.worker_ip << "\",\n";
                            json << "      \"duration_ms\": " << elapsed << "\n";
                            json << "    }";
                            if (i + 1 < g_state.active_jobs.size()) json << ",";
                            json << "\n";
                        }
                        json << "  ],\n";

                        // Connected workers and their core grids
                        json << "  \"workers\": [\n";
                        for (size_t i = 0; i < g_state.workers.size(); ++i) {
                            const auto& w = g_state.workers[i];
                            json << "    {\n";
                            json << "      \"name\": \"" << w->name << "\",\n";
                            json << "      \"ip\": \"" << w->ip << "\",\n";
                            json << "      \"os\": \"" << w->os << "\",\n";
                            json << "      \"slots_total\": " << w->slots_total << ",\n";
                            json << "      \"slots_used\": " << w->slots_used << ",\n";
                            json << "      \"cpu_cores_usage\": [";
                            for (size_t c = 0; c < w->cpu_cores_usage.size(); ++c) {
                                json << w->cpu_cores_usage[c];
                                if (c + 1 < w->cpu_cores_usage.size()) json << ", ";
                            }
                            json << "]\n";
                            json << "    }";
                            if (i + 1 < g_state.workers.size()) json << ",";
                            json << "\n";
                        }
                        json << "  ],\n";

                        // Recent finished compile jobs
                        json << "  \"recent_jobs\": [\n";
                        for (size_t i = 0; i < g_state.recent_jobs.size(); ++i) {
                            const auto& rj = g_state.recent_jobs[i];
                            json << "    {\n";
                            json << "      \"filename\": \"" << rj.filename << "\",\n";
                            json << "      \"exit_code\": " << rj.exit_code << ",\n";
                            json << "      \"cache_hit\": " << (rj.cache_hit ? "true" : "false") << "\n";
                            json << "    }";
                            if (i + 1 < g_state.recent_jobs.size()) json << ",";
                            json << "\n";
                        }
                        json << "  ]\n";
                    }
                    json << "}";

                    std::string json_str = json.str();
                    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " +
                                           std::to_string(json_str.size()) + "\r\nConnection: close\r\n\r\n" + json_str;
                    send(client_sock, response.c_str(), static_cast<int>(response.size()), 0);
                } else {
                    std::string err_404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nNot Found";
                    send(client_sock, err_404.c_str(), static_cast<int>(err_404.size()), 0);
                }
            }
            close_socket(client_sock);
        }).detach();
    }
    close_socket(server_fd);
}

// Least-Loaded Scheduler: Finds best available worker. Assumes lock is held.
int get_best_worker_index() {
    int best_idx = -1;
    double best_ratio = -1.0;

    for (size_t i = 0; i < g_state.workers.size(); ++i) {
        const auto& w = g_state.workers[i];
        int free_slots = w->slots_total - w->slots_used;
        if (free_slots > 0) {
            double ratio = static_cast<double>(free_slots) / w->slots_total;
            if (ratio > best_ratio) {
                best_ratio = ratio;
                best_idx = static_cast<int>(i);
            }
        }
    }
    return best_idx;
}

// Client handler: Decides CACHE_QUERY / COMPILE_REQ
void handle_client_connection(socket_t client_sock, std::string client_ip) {
    uint32_t type_net = 0;
    if (!suco::read_all(client_sock, &type_net, 4)) {
        close_socket(client_sock);
        return;
    }
    uint32_t type = ntohl(type_net);

    if (type == suco::PACKET_CACHE_QUERY) {
        uint32_t hash_len = 0;
        if (!suco::read_all(client_sock, &hash_len, 4)) {
            close_socket(client_sock);
            return;
        }
        hash_len = ntohl(hash_len);
        std::vector<char> hash_buf(hash_len);
        if (!suco::read_all(client_sock, hash_buf.data(), hash_len)) {
            close_socket(client_sock);
            return;
        }
        std::string hash(hash_buf.data(), hash_len);

        uint32_t query_file_len = 0;
        if (!suco::read_all(client_sock, &query_file_len, 4)) {
            close_socket(client_sock);
            return;
        }
        query_file_len = ntohl(query_file_len);
        std::vector<char> query_file_buf(query_file_len);
        if (!suco::read_all(client_sock, query_file_buf.data(), query_file_len)) {
            close_socket(client_sock);
            return;
        }
        std::string query_filename(query_file_buf.data(), query_file_len);

        std::vector<uint8_t> cached_obj;
        std::string cached_log;
        bool cache_found = g_cache->get(hash, cached_obj, cached_log);

        if (cache_found) {
            {
                std::lock_guard<std::mutex> lock(g_state.mutex);
                g_state.total_requests++;
                g_state.cache_hits++;
                
                RecentJob rj{ query_filename, 0, true };
                g_state.recent_jobs.push_back(rj);
                if (g_state.recent_jobs.size() > 20) {
                    g_state.recent_jobs.erase(g_state.recent_jobs.begin());
                }
            }
            uint32_t resp_type = htonl(suco::PACKET_CACHE_HIT);
            uint32_t log_len = htonl(static_cast<u_long>(cached_log.size()));
            uint32_t bin_len = htonl(static_cast<u_long>(cached_obj.size()));

            suco::send_all(client_sock, &resp_type, 4);
            suco::send_all(client_sock, &log_len, 4);
            if (!cached_log.empty()) {
                suco::send_all(client_sock, cached_log.c_str(), cached_log.size());
            }
            suco::send_all(client_sock, &bin_len, 4);
            if (!cached_obj.empty()) {
                suco::send_all(client_sock, cached_obj.data(), cached_obj.size());
            }
            close_socket(client_sock);
            return;
        }

        g_state.mutex.lock();
        auto it = g_state.pending_compilations.find(hash);
        if (it != g_state.pending_compilations.end()) {
            it->second.push_back(client_sock);
            g_state.total_requests++;
            g_state.cache_misses++;
            g_state.mutex.unlock();
            return;
        }

        g_state.pending_compilations[hash] = {};
        g_state.total_requests++;
        g_state.cache_misses++;
        g_state.mutex.unlock();

        uint32_t resp_type = htonl(suco::PACKET_CACHE_MISS);
        if (!suco::send_all(client_sock, &resp_type, 4)) {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }

        uint32_t compile_req_type_net = 0;
        if (!suco::read_all(client_sock, &compile_req_type_net, 4)) {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }

        uint32_t cmd_len_net = 0, file_len_net = 0, src_len_net = 0;
        if (!suco::read_all(client_sock, &cmd_len_net, 4)) {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t cmd_len = ntohl(cmd_len_net);
        std::vector<char> cmd_buf(cmd_len);
        suco::read_all(client_sock, cmd_buf.data(), cmd_len);
        std::string command(cmd_buf.data(), cmd_len);

        if (!suco::read_all(client_sock, &file_len_net, 4)) {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t file_len = ntohl(file_len_net);
        std::vector<char> file_buf(file_len);
        suco::read_all(client_sock, file_buf.data(), file_len);
        std::string filename(file_buf.data(), file_len);

        if (!suco::read_all(client_sock, &src_len_net, 4)) {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t src_len = ntohl(src_len_net);
        std::vector<char> src_buf(src_len);
        suco::read_all(client_sock, src_buf.data(), src_len);
        std::string source(src_buf.data(), src_len);

        g_state.mutex.lock();
        int worker_idx = get_best_worker_index();
        if (worker_idx < 0) {
            g_state.pending_compilations.erase(hash);
            g_state.mutex.unlock();

            uint32_t resp_fail_type = htonl(suco::PACKET_COMPILE_RESP);
            int32_t exit_code = htonl(-1);
            uint32_t log_len = htonl(0);
            uint32_t bin_len = htonl(0);
            suco::send_all(client_sock, &resp_fail_type, 4);
            suco::send_all(client_sock, &exit_code, 4);
            suco::send_all(client_sock, &log_len, 4);
            suco::send_all(client_sock, &bin_len, 4);
            close_socket(client_sock);
            return;
        }

        auto worker = g_state.workers[worker_idx];
        worker->slots_used++;
        socket_t worker_sock = worker->socket;
        std::string worker_ip = worker->ip;
        
        ActiveJob job{ filename, worker_ip, std::chrono::steady_clock::now() };
        g_state.active_jobs.push_back(job);
        g_state.mutex.unlock();

        auto res = std::make_shared<CompileResult>();
        {
            std::lock_guard<std::mutex> lock(g_results_mutex);
            g_compile_results[filename] = res;
        }

        bool send_ok = false;
        {
            std::lock_guard<std::mutex> lock(worker->write_mutex);
            uint32_t w_req_type = htonl(suco::PACKET_COMPILE_REQ);
            if (suco::send_all(worker_sock, &w_req_type, 4) &&
                suco::send_all(worker_sock, &cmd_len_net, 4) &&
                suco::send_all(worker_sock, command.c_str(), command.size()) &&
                suco::send_all(worker_sock, &file_len_net, 4) &&
                suco::send_all(worker_sock, filename.c_str(), filename.size()) &&
                suco::send_all(worker_sock, &src_len_net, 4) &&
                suco::send_all(worker_sock, source.c_str(), source.size())) {
                send_ok = true;
            }
        }

        if (!send_ok) {
            std::lock_guard<std::mutex> lock(g_state.mutex);
            g_state.pending_compilations.erase(hash);
            for (auto it = g_state.active_jobs.begin(); it != g_state.active_jobs.end(); ++it) {
                if (it->filename == filename) { g_state.active_jobs.erase(it); break; }
            }
            {
                std::lock_guard<std::mutex> res_lock(g_results_mutex);
                g_compile_results.erase(filename);
            }

            uint32_t resp_fail_type = htonl(suco::PACKET_COMPILE_RESP);
            int32_t exit_code = htonl(-1);
            uint32_t l_len = 0, b_len = 0;
            suco::send_all(client_sock, &resp_fail_type, 4);
            suco::send_all(client_sock, &exit_code, 4);
            suco::send_all(client_sock, &l_len, 4);
            suco::send_all(client_sock, &b_len, 4);
            close_socket(client_sock);
            return;
        }

        std::unique_lock<std::mutex> res_lock(res->mutex);
        res->cv.wait(res_lock, [&]() { return res->ready; });

        int32_t exit_code = res->exit_code;
        std::string log_str = res->log;
        std::vector<uint8_t> bin_data = res->bin;

        {
            std::lock_guard<std::mutex> map_lock(g_results_mutex);
            g_compile_results.erase(filename);
        }

        if (exit_code == 0 && !bin_data.empty()) {
            g_cache->put(hash, bin_data, log_str);
        }

        g_state.mutex.lock();
        for (auto& w : g_state.workers) {
            if (w->socket == worker_sock) {
                w->slots_used = std::max(0, w->slots_used - 1);
                break;
            }
        }
        for (auto it = g_state.active_jobs.begin(); it != g_state.active_jobs.end(); ++it) {
            if (it->filename == filename) {
                g_state.active_jobs.erase(it);
                break;
            }
        }

        RecentJob rj{ filename, exit_code, false };
        g_state.recent_jobs.push_back(rj);
        if (g_state.recent_jobs.size() > 20) {
            g_state.recent_jobs.erase(g_state.recent_jobs.begin());
        }

        int32_t exit_code_net = htonl(exit_code);
        uint32_t log_len_net = htonl(static_cast<u_long>(log_str.size()));
        uint32_t bin_len_net = htonl(static_cast<u_long>(bin_data.size()));

        uint32_t resp_type_net = htonl(suco::PACKET_COMPILE_RESP);
        suco::send_all(client_sock, &resp_type_net, 4);
        suco::send_all(client_sock, &exit_code_net, 4);
        suco::send_all(client_sock, &log_len_net, 4);
        if (!log_str.empty()) suco::send_all(client_sock, log_str.c_str(), log_str.size());
        suco::send_all(client_sock, &bin_len_net, 4);
        if (!bin_data.empty()) suco::send_all(client_sock, bin_data.data(), bin_data.size());
        close_socket(client_sock);

        auto waiting_clients = g_state.pending_compilations[hash];
        g_state.pending_compilations.erase(hash);
        g_state.mutex.unlock();

        for (auto s : waiting_clients) {
            uint32_t wait_resp = htonl(suco::PACKET_COMPILE_RESP);
            suco::send_all(s, &wait_resp, 4);
            suco::send_all(s, &exit_code_net, 4);
            suco::send_all(s, &log_len_net, 4);
            if (!log_str.empty()) suco::send_all(s, log_str.c_str(), log_str.size());
            suco::send_all(s, &bin_len_net, 4);
            if (!bin_data.empty()) suco::send_all(s, bin_data.data(), bin_data.size());
            close_socket(s);
        }
    }
}

// Handler for permanent worker connection
void handle_worker_connection(socket_t worker_sock, std::string worker_ip) {
    // Read Worker registration details: [slots_total (4)] + [name_len (4)] + [name] + [os_len (4)] + [os]
    uint32_t slots_total_net = 0;
    if (!suco::read_all(worker_sock, &slots_total_net, 4)) {
        close_socket(worker_sock);
        return;
    }
    int slots_total = ntohl(slots_total_net);

    uint32_t name_len_net = 0;
    if (!suco::read_all(worker_sock, &name_len_net, 4)) { close_socket(worker_sock); return; }
    uint32_t name_len = ntohl(name_len_net);
    std::vector<char> name_buf(name_len);
    if (name_len > 0) {
        if (!suco::read_all(worker_sock, name_buf.data(), name_len)) { close_socket(worker_sock); return; }
    }
    std::string name(name_buf.data(), name_len);

    uint32_t os_len_net = 0;
    if (!suco::read_all(worker_sock, &os_len_net, 4)) { close_socket(worker_sock); return; }
    uint32_t os_len = ntohl(os_len_net);
    std::vector<char> os_buf(os_len);
    if (os_len > 0) {
        if (!suco::read_all(worker_sock, os_buf.data(), os_len)) { close_socket(worker_sock); return; }
    }
    std::string os_str(os_buf.data(), os_len);

    std::cout << "suco-coordinator: Worker registered: " << name << " (" << worker_ip 
              << ", OS: " << os_str << ", Cores: " << slots_total << ")" << std::endl;

    auto node = std::make_shared<WorkerNode>();
    node->socket = worker_sock;
    node->ip = worker_ip;
    node->name = name;
    node->os = os_str;
    node->slots_total = slots_total;
    node->slots_used = 0;
    node->cpu_cores_usage.resize(slots_total, 0.0);
    node->last_heartbeat = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.workers.push_back(node);
    }

    // Keep reading from worker (multiplexing heartbeats and compile responses)
    while (true) {
        uint32_t packet_type_net = 0;
        if (!suco::read_all(worker_sock, &packet_type_net, 4)) {
            break; // Connection closed or timeout
        }
        uint32_t type = ntohl(packet_type_net);

        if (type == suco::PACKET_HEARTBEAT) {
            uint32_t active_slots_net = 0, total_slots_net = 0, cores_count_net = 0;
            if (!suco::read_all(worker_sock, &active_slots_net, 4) ||
                !suco::read_all(worker_sock, &total_slots_net, 4) ||
                !suco::read_all(worker_sock, &cores_count_net, 4)) {
                break;
            }
            int active_slots = ntohl(active_slots_net);
            int total_slots = ntohl(total_slots_net);
            int cores_count = ntohl(cores_count_net);

            std::vector<double> usage(cores_count);
            if (cores_count > 0) {
                if (!suco::read_all(worker_sock, usage.data(), cores_count * sizeof(double))) {
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lock(g_state.mutex);
                for (auto& w : g_state.workers) {
                    if (w->socket == worker_sock) {
                        w->slots_used = active_slots;
                        w->slots_total = total_slots;
                        w->cpu_cores_usage = usage;
                        w->last_heartbeat = std::chrono::steady_clock::now();
                        break;
                    }
                }
            }
        } 
        else if (type == suco::PACKET_COMPILE_RESP) {
            uint32_t file_len_net = 0;
            if (!suco::read_all(worker_sock, &file_len_net, 4)) break;
            uint32_t file_len = ntohl(file_len_net);
            std::vector<char> file_buf(file_len);
            if (file_len > 0) {
                if (!suco::read_all(worker_sock, file_buf.data(), file_len)) break;
            }
            std::string filename(file_buf.data(), file_len);

            int32_t exit_code_net = 0;
            if (!suco::read_all(worker_sock, &exit_code_net, 4)) break;
            int32_t exit_code = ntohl(exit_code_net);

            uint32_t log_len_net = 0;
            if (!suco::read_all(worker_sock, &log_len_net, 4)) break;
            uint32_t log_len = ntohl(log_len_net);
            std::vector<char> log_buf(log_len);
            if (log_len > 0) {
                if (!suco::read_all(worker_sock, log_buf.data(), log_len)) break;
            }
            std::string log_str(log_buf.data(), log_len);

            uint32_t bin_len_net = 0;
            if (!suco::read_all(worker_sock, &bin_len_net, 4)) break;
            uint32_t bin_len = ntohl(bin_len_net);
            std::vector<uint8_t> bin_data(bin_len);
            if (bin_len > 0) {
                if (!suco::read_all(worker_sock, bin_data.data(), bin_len)) break;
            }

            std::shared_ptr<CompileResult> res;
            {
                std::lock_guard<std::mutex> lock(g_results_mutex);
                auto it = g_compile_results.find(filename);
                if (it != g_compile_results.end()) {
                    res = it->second;
                }
            }

            if (res) {
                std::lock_guard<std::mutex> lock(res->mutex);
                res->exit_code = exit_code;
                res->log = log_str;
                res->bin = bin_data;
                res->ready = true;
                res->cv.notify_all();
            }
        }
    }

    // Worker disconnected
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        for (auto it = g_state.workers.begin(); it != g_state.workers.end(); ++it) {
            if ((*it)->socket == worker_sock) {
                std::cout << "suco-coordinator: Worker offline: " << (*it)->name << " (" << (*it)->ip << ")" << std::endl;
                g_state.workers.erase(it);
                break;
            }
        }
    }
    close_socket(worker_sock);
}

} // namespace suco

int main() {
    suco::SocketInit sock_init;

    // Cache Path selection based on OS
    std::string cache_path = "~/.cache/suco/";
#ifdef _WIN32
    cache_path = "%LOCALAPPDATA%\\suco\\cache\\";
#endif

    // Cache Size Limit: 5 GB
    suco::g_cache = new suco::LruCache(cache_path, 5ULL * 1024 * 1024 * 1024);

    // 1. Start UDP Discovery Broadcast thread
    std::thread udp_thread(suco::run_udp_broadcast, suco::DEFAULT_PORT);
    udp_thread.detach();

    // 2. Start Worker Health Monitor thread
    std::thread monitor_thread(suco::run_worker_monitor);
    monitor_thread.detach();

    // 3. Start REST Web Server for Dashboard
    std::thread web_thread(suco::run_web_server, suco::DEFAULT_WEB_PORT);
    web_thread.detach();

    // 4. Start main TCP socket for Coordinator compile service
    socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == INVALID_SOCKET_VAL) {
        std::cerr << "suco-coordinator error: Failed to create server socket." << std::endl;
        return 1;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(suco::DEFAULT_PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "suco-coordinator error: Bind failed on Port " << suco::DEFAULT_PORT << std::endl;
        close_socket(server_fd);
        return 1;
    }

    if (listen(server_fd, 128) < 0) {
        std::cerr << "suco-coordinator error: Listen failed." << std::endl;
        close_socket(server_fd);
        return 1;
    }

    std::cout << "suco-coordinator: Compile Coordinator active on Port " << suco::DEFAULT_PORT << std::endl;

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        socket_t client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET_VAL) continue;

        std::string client_ip = inet_ntoa(client_addr.sin_addr);

        std::thread([client_sock, client_ip]() {
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
                uint32_t dummy = 0;
                suco::read_all(client_sock, &dummy, 4);
                suco::handle_worker_connection(client_sock, client_ip);
            } else {
                suco::handle_client_connection(client_sock, client_ip);
            }
        }).detach();
    }

    close_socket(server_fd);
    delete suco::g_cache;
    return 0;
}
