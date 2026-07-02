#include "worker_handler.h"
#include "protocol.h"
#include "logging.h"
#include <iostream>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <chrono>
#include <thread>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace suco {

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

void WorkerHandler::handle_worker_connection(socket_t worker_sock) {
    std::string worker_ip = get_socket_ip(worker_sock);
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

    SUCO_LOG_INFO("Worker registered: {} ({}, OS: {}, Cores: {})", name, worker_ip, os_str, slots_total);

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
            SUCO_LOG_INFO("Received compile response from worker {} for {} (Exit: {})", worker_ip, filename, exit_code);
            {
                std::lock_guard<std::mutex> lock(m_state.results_mutex);
                auto it = m_state.compile_results.find(filename);
                if (it != m_state.compile_results.end()) {
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
    close_socket(worker_sock);

    // Triggere Rescheduling über Callback an den Coordinator
    if (m_disconnect_handler && !ip_to_cleanup.empty()) {
        std::thread([this, ip_to_cleanup]() {
            m_disconnect_handler(ip_to_cleanup);
        }).detach();
    }
}

} // namespace suco
