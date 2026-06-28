#include "worker_handler.h"
#include "protocol.h"
#include <iostream>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <chrono>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

// Importiere temporär globale Variablen und Methoden aus main.cpp zur schrittweisen Migration
namespace suco {

struct RecentJob {
    std::string filename;
    int32_t exit_code;
    bool cache_hit;
};

struct ActiveJob {
    std::string filename;
    std::string worker_ip;
    std::chrono::steady_clock::time_point start_time;
};

struct CoordinatorState {
    std::mutex mutex;
    std::vector<std::shared_ptr<WorkerNode>> workers;
    uint64_t total_requests;
    uint64_t cache_hits;
    uint64_t cache_misses;
    std::unordered_map<std::string, std::vector<socket_t>> pending_compilations;
    std::vector<ActiveJob> active_jobs;
    std::vector<RecentJob> recent_jobs;
};

struct CompileResult {
    bool ready;
    int32_t exit_code;
    std::string log;
    std::vector<uint8_t> bin;
    std::condition_variable cv;
    std::mutex mutex;
};

extern CoordinatorState g_state;
extern std::mutex g_results_mutex;
extern std::unordered_map<std::string, std::shared_ptr<CompileResult>> g_compile_results;

// Failover Handler in main.cpp deklariert
extern void handle_worker_disconnect(const std::string& worker_ip);

WorkerHandler::WorkerHandler(const CoordinatorConfig& config, 
                             WorkerManager& worker_manager, 
                             JobQueue& job_queue)
    : m_config(config),
      m_worker_manager(worker_manager),
      m_job_queue(job_queue) {}

void WorkerHandler::handle_worker_connection(socket_t worker_sock, const std::string& worker_ip) {
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

    std::cout << "suco-coordinator: Worker registered: " << name << " (" << worker_ip 
              << ", OS: " << os_str << ", Cores: " << slots_total << ")" << std::endl;

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

    m_worker_manager.register_worker(node);

    // Temp compatibility registration in global state
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        g_state.workers.push_back(node);
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

            // Temp compatibility update in global state
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

            uint32_t bin_len_net = 0;
            if (!read_all(worker_sock, &bin_len_net, 4)) break;
            uint32_t bin_len = ntohl(bin_len_net);
            std::vector<uint8_t> bin_data(bin_len);
            if (bin_len > 0) {
                if (!read_all(worker_sock, bin_data.data(), bin_len)) break;
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

    // 5. Worker offline verarbeiten
    std::string ip_to_cleanup = worker_ip;
    
    // Deregister aus WorkerManager
    m_worker_manager.deregister_worker(worker_sock);

    // Deregister aus globaler compatibility Liste
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        for (auto it = g_state.workers.begin(); it != g_state.workers.end(); ++it) {
            if ((*it)->socket == worker_sock) {
                ip_to_cleanup = (*it)->ip;
                g_state.workers.erase(it);
                break;
            }
        }
    }

    close_socket(worker_sock);

    // Triggere Rescheduling für getrennte Worker-IP
    if (!ip_to_cleanup.empty()) {
        std::thread(handle_worker_disconnect, ip_to_cleanup).detach();
    }
}

} // namespace suco
