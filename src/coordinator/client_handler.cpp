#include "client_handler.h"
#include "protocol.h"
#include "logging.h"
#include <iostream>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <cstring>
#include <chrono>

#ifndef _WIN32
#include <arpa/inet.h>
#endif

namespace suco {

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
    uint32_t type_net = 0;
    if (!read_all(client_sock, &type_net, 4)) {
        close_socket(client_sock);
        return;
    }
    uint32_t type = ntohl(type_net);

    if (type == PACKET_CACHE_QUERY) {
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

        std::vector<uint8_t> cached_obj;
        std::string cached_log;
        bool cache_found = false;
        if (m_cache) {
            cache_found = m_cache->get(hash, cached_obj, cached_log);
        }

        if (cache_found) {
            SUCO_LOG_INFO("Cache HIT for job {} (hash: {})", query_filename, hash);
            {
                std::lock_guard<std::mutex> lock(m_state.mutex);
                m_state.total_requests++;
                m_state.cache_hits++;
                
                RecentJob rj{ query_filename, 0, true };
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
            send_all(client_sock, &bin_len, 4);
            if (!cached_obj.empty()) {
                send_all(client_sock, cached_obj.data(), cached_obj.size());
            }
            close_socket(client_sock);
            return;
        }

        SUCO_LOG_INFO("Cache MISS for job {} (hash: {})", query_filename, hash);

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

        uint32_t resp_type = htonl(PACKET_CACHE_MISS);
        if (!send_all(client_sock, &resp_type, 4)) {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }

        uint32_t compile_req_type_net = 0;
        if (!read_all(client_sock, &compile_req_type_net, 4)) {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }

        uint32_t cmd_len_net = 0, file_len_net = 0, src_len_net = 0;
        if (!read_all(client_sock, &cmd_len_net, 4)) {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t cmd_len = ntohl(cmd_len_net);
        std::vector<char> cmd_buf(cmd_len);
        read_all(client_sock, cmd_buf.data(), cmd_len);
        std::string command(cmd_buf.data(), cmd_len);

        if (!read_all(client_sock, &file_len_net, 4)) {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t file_len = ntohl(file_len_net);
        std::vector<char> file_buf(file_len);
        read_all(client_sock, file_buf.data(), file_len);
        std::string filename(file_buf.data(), file_len);

        if (!read_all(client_sock, &src_len_net, 4)) {
            std::lock_guard<std::mutex> lock(m_state.mutex);
            m_state.pending_compilations.erase(hash);
            close_socket(client_sock);
            return;
        }
        uint32_t src_len = ntohl(src_len_net);
        std::vector<char> src_buf(src_len);
        read_all(client_sock, src_buf.data(), src_len);
        std::string source(src_buf.data(), src_len);

        // Registriere Job-Details für eventuelles Failover/Rescheduling
        {
            std::lock_guard<std::mutex> lock(m_state.running_details_mutex);
            m_state.running_job_details[filename] = RunningJobDetail{ hash, command, source, client_sock, 1 };
        }

        // Verwende den ausgelagerten WorkerManager und den Scheduler
        auto active_workers = m_worker_manager.get_active_workers();
        Job current_job;
        current_job.filename = filename;
        current_job.hash = hash;
        current_job.command = command;
        current_job.source = source;

        auto best_worker = m_scheduler.select_best_worker(active_workers, current_job);

        if (!best_worker) {
            // Kein Worker frei
            m_state.mutex.lock();
            m_state.pending_compilations.erase(hash);
            m_state.mutex.unlock();

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

        m_state.mutex.lock();
        ActiveJob aj{ filename, worker_ip, std::chrono::steady_clock::now() };
        m_state.active_jobs.push_back(aj);
        m_state.mutex.unlock();

        auto res = std::make_shared<CompileResult>();
        {
            std::lock_guard<std::mutex> lock(m_state.results_mutex);
            m_state.compile_results[filename] = res;
        }

        bool send_ok = false;
        {
            std::lock_guard<std::mutex> lock(best_worker->write_mutex);
            uint32_t w_req_type = htonl(PACKET_COMPILE_REQ);
            if (send_all(worker_sock, &w_req_type, 4) &&
                send_all(worker_sock, &cmd_len_net, 4) &&
                send_all(worker_sock, command.c_str(), command.size()) &&
                send_all(worker_sock, &file_len_net, 4) &&
                send_all(worker_sock, filename.c_str(), filename.size()) &&
                send_all(worker_sock, &src_len_net, 4) &&
                send_all(worker_sock, source.c_str(), source.size())) {
                send_ok = true;
            }
        }

        if (!send_ok) {
            m_state.mutex.lock();
            m_state.pending_compilations.erase(hash);
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
        res_lock.unlock();

        {
            std::lock_guard<std::mutex> map_lock(m_state.results_mutex);
            m_state.compile_results.erase(filename);
        }
        {
            std::lock_guard<std::mutex> lock(m_state.running_details_mutex);
            m_state.running_job_details.erase(filename);
        }

        if (exit_code == 0 && !bin_data.empty() && m_cache) {
            m_cache->put(hash, bin_data, log_str, filename, command);
        }

        // Slots freigeben
        auto workers_list = m_worker_manager.get_active_workers();
        for (auto& w : workers_list) {
            if (w->socket == worker_sock) {
                w->slots_used = std::max(0, w->slots_used - 1);
                break;
            }
        }

        m_state.mutex.lock();
        for (auto it = m_state.active_jobs.begin(); it != m_state.active_jobs.end(); ++it) {
            if (it->filename == filename) {
                m_state.active_jobs.erase(it);
                break;
            }
        }

        RecentJob rj{ filename, exit_code, false };
        m_state.recent_jobs.push_back(rj);
        if (m_state.recent_jobs.size() > 20) {
            m_state.recent_jobs.erase(m_state.recent_jobs.begin());
        }

        int32_t exit_code_net = htonl(exit_code);
        uint32_t log_len_net = htonl(static_cast<uint32_t>(log_str.size()));
        uint32_t bin_len_net = htonl(static_cast<uint32_t>(bin_data.size()));

        uint32_t resp_type_net = htonl(PACKET_COMPILE_RESP);
        send_all(client_sock, &resp_type_net, 4);
        send_all(client_sock, &exit_code_net, 4);
        send_all(client_sock, &log_len_net, 4);
        if (!log_str.empty()) send_all(client_sock, log_str.c_str(), log_str.size());
        send_all(client_sock, &bin_len_net, 4);
        if (!bin_data.empty()) send_all(client_sock, bin_data.data(), bin_data.size());
        close_socket(client_sock);

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
            send_all(s, &bin_len_net, 4);
            if (!bin_data.empty()) send_all(s, bin_data.data(), bin_data.size());
            close_socket(s);
        }
    }
}

} // namespace suco
