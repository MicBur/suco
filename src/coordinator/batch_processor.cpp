#include "batch_processor.h"
#include "logging.h"
#include "version_utils.h"
#include "protocol.h"
#include "history_writer.h"
#include <chrono>
#include <iostream>
#include <algorithm>
#include <sstream>

namespace suco {

BatchProcessor::BatchProcessor(const CoordinatorConfig& config,
                               JobQueue& job_queue,
                               const Scheduler& scheduler,
                               WorkerManager& worker_manager,
                               SharedCoordinatorState& state,
                               std::unique_ptr<LruCache>& cache)
    : config_(config),
      job_queue_(job_queue),
      scheduler_(scheduler),
      worker_manager_(worker_manager),
      state_(state),
      cache_(cache) {
    prefetch_limit_ = config_.get_prefetch_jobs();
    num_threads_ = config_.get_coordinator_batch_threads();
}

void BatchProcessor::process_batch_request(socket_t client_sock, const std::string& client_ip) {
    auto batch_start = std::chrono::steady_clock::now();
    auto batch_start_system = std::chrono::system_clock::now();

    uint32_t num_jobs_net = 0;
    if (!read_all(client_sock, &num_jobs_net, 4)) {
        close_socket(client_sock);
        return;
    }
    uint32_t num_jobs = ntohl(num_jobs_net);
    SUCO_LOG_INFO("BatchProcessor: Processing PACKET_COMPILE_BATCH_REQ from {} containing {} jobs", client_ip, num_jobs);

    jobs_.resize(num_jobs);
    results_.resize(num_jobs);
    timings_.resize(num_jobs);

    for (uint32_t i = 0; i < num_jobs; ++i) {
        timings_[i].queue_start_system = batch_start_system;
        timings_[i].queue_start_steady = batch_start;

        auto& job = jobs_[i];
        results_[i].content_hash = job.content_hash;

        uint32_t hash_len_net = 0;
        if (!read_all(client_sock, &hash_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t hash_len = ntohl(hash_len_net);
        std::vector<char> hash_buf(hash_len);
        if (hash_len > 0) {
            if (!read_all(client_sock, hash_buf.data(), hash_len)) { close_socket(client_sock); return; }
        }
        job.content_hash = std::string(hash_buf.data(), hash_len);
        results_[i].content_hash = job.content_hash;

        uint32_t file_len_net = 0;
        if (!read_all(client_sock, &file_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t file_len = ntohl(file_len_net);
        std::vector<char> file_buf(file_len);
        if (file_len > 0) {
            if (!read_all(client_sock, file_buf.data(), file_len)) { close_socket(client_sock); return; }
        }
        job.source_file = std::string(file_buf.data(), file_len);

        uint8_t src_comp = 0;
        if (!read_all(client_sock, &src_comp, 1)) { close_socket(client_sock); return; }
        uint32_t src_len_net = 0;
        if (!read_all(client_sock, &src_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t src_len = ntohl(src_len_net);
        std::vector<char> src_buf(src_len);
        if (src_len > 0) {
            if (!read_all(client_sock, src_buf.data(), src_len)) { close_socket(client_sock); return; }
        }
        job.preprocessed_source = std::string(src_buf.data(), src_len);
        job.source_compressed = src_comp;

        uint32_t tc_len_net = 0;
        if (!read_all(client_sock, &tc_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t tc_len = ntohl(tc_len_net);
        std::vector<char> tc_buf(tc_len);
        if (tc_len > 0) {
            if (!read_all(client_sock, tc_buf.data(), tc_len)) { close_socket(client_sock); return; }
        }
        job.toolchain_hash = std::string(tc_buf.data(), tc_len);

        uint32_t cmd_len_net = 0;
        if (!read_all(client_sock, &cmd_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t cmd_len = ntohl(cmd_len_net);
        std::vector<char> cmd_buf(cmd_len);
        if (cmd_len > 0) {
            if (!read_all(client_sock, cmd_buf.data(), cmd_len)) { close_socket(client_sock); return; }
        }
        job.command = std::string(cmd_buf.data(), cmd_len);

        uint32_t req_c_len_net = 0;
        if (!read_all(client_sock, &req_c_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t req_c_len = ntohl(req_c_len_net);
        std::vector<char> req_c_buf(req_c_len);
        if (req_c_len > 0) {
            if (!read_all(client_sock, req_c_buf.data(), req_c_len)) { close_socket(client_sock); return; }
        }
        job.required_compiler = std::string(req_c_buf.data(), req_c_len);

        uint32_t req_cv_len_net = 0;
        if (!read_all(client_sock, &req_cv_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t req_cv_len = ntohl(req_cv_len_net);
        std::vector<char> req_cv_buf(req_cv_len);
        if (req_cv_len > 0) {
            if (!read_all(client_sock, req_cv_buf.data(), req_cv_len)) { close_socket(client_sock); return; }
        }
        job.required_compiler_version = std::string(req_cv_buf.data(), req_cv_len);

        uint32_t hs_hash_len_net = 0;
        if (!read_all(client_sock, &hs_hash_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t hs_hash_len = ntohl(hs_hash_len_net);
        std::vector<char> hs_hash_buf(hs_hash_len);
        if (hs_hash_len > 0) {
            if (!read_all(client_sock, hs_hash_buf.data(), hs_hash_len)) { close_socket(client_sock); return; }
        }
        job.header_set_hash = std::string(hs_hash_buf.data(), hs_hash_len);

        uint8_t hs_comp = 0;
        if (!read_all(client_sock, &hs_comp, 1)) { close_socket(client_sock); return; }
        uint32_t hs_src_len_net = 0;
        if (!read_all(client_sock, &hs_src_len_net, 4)) { close_socket(client_sock); return; }
        uint32_t hs_src_len = ntohl(hs_src_len_net);
        std::vector<char> hs_src_buf(hs_src_len);
        if (hs_src_len > 0) {
            if (!read_all(client_sock, hs_src_buf.data(), hs_src_len)) { close_socket(client_sock); return; }
        }
        job.header_set_source = std::string(hs_src_buf.data(), hs_src_len);
        job.hs_compressed = hs_comp;
    }

    // 1. Initial Cache Check & Queue Population
    for (size_t i = 0; i < num_jobs; ++i) {
        const auto& job = jobs_[i];
        auto& res = results_[i];

        bool cache_hit = false;
        std::string log_data;
        std::vector<uint8_t> bin_data;

        state_.mutex.lock();
        uint8_t bin_comp = 0;
        if (cache_ && cache_->get(job.content_hash, bin_data, log_data, bin_comp)) {
            cache_hit = true;
            state_.total_requests++;
            state_.cache_hits++;
            state_.mutex.unlock();

            res.exit_code = 0;
            res.cache_hit = true;
            res.log = std::move(log_data);
            res.binary = std::move(bin_data);
            res.bin_comp = bin_comp;
        } else {
            if (cache_) {
                state_.total_requests++;
                state_.cache_misses++;
            }
            state_.mutex.unlock();
            
            pending_jobs_.push_back(i);
        }
    }

    // 2. Prefetch first window of jobs
    prefetch_next_job();

    // 3. Spawn Thread Pool (Optimized: Avoid spawning threads for small batches)
    size_t actual_threads = std::min(pending_jobs_.size() + prefetched_jobs_.size(), num_threads_);
    if (actual_threads > 1) {
        std::vector<std::jthread> pool;
        pool.reserve(actual_threads);
        for (size_t t = 0; t < actual_threads; ++t) {
            pool.emplace_back([this]() { compile_worker_loop(); });
        }
        // Explicitly wait for workers to complete compiling
        pool.clear();
    } else if (actual_threads == 1) {
        compile_worker_loop();
    }

    auto batch_end = std::chrono::steady_clock::now();
    uint64_t total_batch_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(batch_end - batch_start).count();

    // 4. Send Response packet
    uint32_t resp_type_net = htonl(PACKET_COMPILE_BATCH_RESP);
    send_all(client_sock, &resp_type_net, 4);

    uint32_t num_jobs_net_out = htonl(static_cast<uint32_t>(results_.size()));
    send_all(client_sock, &num_jobs_net_out, 4);

    // Write performance metrics
    uint32_t sched_ms = htonl(static_cast<uint32_t>(total_scheduling_ms_.load()));
    uint32_t comp_ms = htonl(static_cast<uint32_t>(total_compilation_ms_.load()));
    uint32_t tc_ms = htonl(static_cast<uint32_t>(total_toolchain_ms_.load()));
    send_all(client_sock, &sched_ms, 4);
    send_all(client_sock, &comp_ms, 4);
    send_all(client_sock, &tc_ms, 4);

    for (const auto& res : results_) {
        uint32_t hash_len_net_out = htonl(res.content_hash.size());
        send_all(client_sock, &hash_len_net_out, 4);
        send_all(client_sock, res.content_hash.c_str(), res.content_hash.size());

        int32_t exit_code_net_out = htonl(res.exit_code);
        send_all(client_sock, &exit_code_net_out, 4);

        uint32_t cache_hit_net_out = htonl(res.cache_hit ? 1 : 0);
        send_all(client_sock, &cache_hit_net_out, 4);

        uint32_t hc_hit_net_out = htonl(res.header_cache_hit ? 1 : 0);
        send_all(client_sock, &hc_hit_net_out, 4);

        uint32_t log_len_net_out = htonl(res.log.size());
        send_all(client_sock, &log_len_net_out, 4);
        if (!res.log.empty()) {
            send_all(client_sock, res.log.c_str(), res.log.size());
        }

        send_all(client_sock, &res.bin_comp, 1);
        uint32_t bin_len_net_out = htonl(res.binary.size());
        send_all(client_sock, &bin_len_net_out, 4);
        if (!res.binary.empty()) {
            send_all(client_sock, res.binary.data(), res.binary.size());
        }
    }

    auto result_steady = std::chrono::steady_clock::now();

    std::vector<uint64_t> job_ids(results_.size());
    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        for (size_t i = 0; i < results_.size(); ++i) {
            job_ids[i] = state_.next_job_id++;
        }
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

    for (size_t i = 0; i < results_.size(); ++i) {
        const auto& job = jobs_[i];
        const auto& res = results_[i];
        const auto& t = timings_[i];
        uint64_t job_id = job_ids[i];

        int64_t queue_start_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t.queue_start_system.time_since_epoch()).count();
        
        int64_t dispatch_ms = 0;
        int64_t compile_end_ms = 0;
        int64_t result_ms = queue_start_ms + std::chrono::duration_cast<std::chrono::milliseconds>(
            result_steady - t.queue_start_steady).count();

        int64_t queue_duration_ms = 0;
        int64_t compile_duration_ms = 0;
        int64_t total_duration_ms = result_ms - queue_start_ms;

        if (!res.cache_hit && t.dispatch_steady != std::chrono::steady_clock::time_point()) {
            dispatch_ms = queue_start_ms + std::chrono::duration_cast<std::chrono::milliseconds>(
                t.dispatch_steady - t.queue_start_steady).count();
            queue_duration_ms = dispatch_ms - queue_start_ms;
            
            if (t.compile_end_steady != std::chrono::steady_clock::time_point()) {
                compile_end_ms = queue_start_ms + std::chrono::duration_cast<std::chrono::milliseconds>(
                    t.compile_end_steady - t.queue_start_steady).count();
                compile_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    t.compile_end_steady - t.dispatch_steady).count();
            }
        }

        // 1. In SQLite loggen (asynchron)
        if (state_.history_writer) {
            HistoryWriter::Event ev;
            ev.job_id = job_id;
            ev.source_file = job.source_file;
            ev.content_hash = job.content_hash;
            ev.worker_name = t.worker_name;
            ev.cache_hit = res.cache_hit;
            ev.queue_start_ms = queue_start_ms;
            ev.dispatch_ms = dispatch_ms;
            ev.compile_end_ms = compile_end_ms;
            ev.result_ms = result_ms;
            ev.queue_duration_ms = queue_duration_ms;
            ev.compile_duration_ms = compile_duration_ms;
            ev.total_duration_ms = total_duration_ms;
            ev.exit_code = res.exit_code;

            state_.history_writer->enqueue(std::move(ev));
        }

        // 2. Via SSE broadcasten
        std::stringstream json;
        json << "{"
             << "\"job_id\":" << job_id << ","
             << "\"file\":\"" << escape_json(job.source_file) << "\","
             << "\"worker\":\"" << escape_json(t.worker_name) << "\","
             << "\"cache_hit\":" << (res.cache_hit ? "true" : "false") << ","
             << "\"queue_ms\":" << queue_duration_ms << ","
             << "\"compile_ms\":" << compile_duration_ms << ","
             << "\"total_ms\":" << total_duration_ms << ","
             << "\"exit_code\":" << res.exit_code
             << "}";

        state_.broadcast_sse_event(res.exit_code == 0 ? "job_complete" : "job_failed", json.str());
    }

    close_socket(client_sock);

    SUCO_LOG_INFO("BatchProcessor: Batch processed in {}ms (Coordinator: Scheduling={}ms, Compile={}ms)", 
                  total_batch_time_ms, total_scheduling_ms_.load(), total_compilation_ms_.load());
}

void BatchProcessor::prefetch_next_job() {
    std::lock_guard<std::mutex> lock(prefetch_mutex_);
    while (prefetched_jobs_.size() < prefetch_limit_ && !pending_jobs_.empty()) {
        size_t idx = pending_jobs_.front();
        const auto& job = jobs_[idx];

        auto active_workers = worker_manager_.get_active_workers();
        Job current_job;
        current_job.filename = job.source_file;
        current_job.hash = job.content_hash;
        current_job.command = job.command;
        current_job.source = job.preprocessed_source;
        current_job.required_compiler = job.required_compiler;
        current_job.required_compiler_version = job.required_compiler_version;
        current_job.toolchain_hash = job.toolchain_hash;
        current_job.header_set_hash = job.header_set_hash;
        current_job.header_set_source = job.header_set_source;

        // Deadlock-Prevention: Check if a compatible worker exists in the grid at all
        bool has_compatible_worker = false;
        for (const auto& w : active_workers) {
            if (!job.required_compiler.empty()) {
                auto it = w->toolchains.compilers.find(job.required_compiler);
                if (it != w->toolchains.compilers.end()) {
                    if (job.required_compiler_version.empty() || 
                        is_compiler_version_compatible(it->second, job.required_compiler_version)) {
                        has_compatible_worker = true;
                        break;
                    }
                }
            } else {
                has_compatible_worker = true;
                break;
            }
        }

        if (!has_compatible_worker) {
            // Cannot compile remote -> fallback immediately, do not wait
            pending_jobs_.pop_front();
            prefetched_jobs_.push_back(PrefetchedJob{ idx, nullptr });
            cv_prefetch_.notify_one();
            continue;
        }

        auto sched_start = std::chrono::steady_clock::now();
        auto best_worker = scheduler_.select_best_worker(active_workers, current_job);
        auto sched_end = std::chrono::steady_clock::now();
        total_scheduling_ms_ += std::chrono::duration_cast<std::chrono::milliseconds>(sched_end - sched_start).count();

        if (best_worker) {
            best_worker->slots_used++;
            pending_jobs_.pop_front();
            prefetched_jobs_.push_back(PrefetchedJob{ idx, best_worker });
            
            SUCO_LOG_INFO("BatchProcessor: Prefetched {} for worker {} (Slots: {}/{})", 
                          job.source_file, best_worker->name, best_worker->slots_used, best_worker->slots_total);
            cv_prefetch_.notify_one();
        } else {
            // Slots are currently full on compatible workers. Stop prefetching.
            break;
        }
    }
}

void BatchProcessor::compile_worker_loop() {
    while (true) {
        PrefetchedJob prefetched;
        {
            std::unique_lock<std::mutex> lock(prefetch_mutex_);
            cv_prefetch_.wait(lock, [this]() {
                return !prefetched_jobs_.empty() || pending_jobs_.empty();
            });

            if (prefetched_jobs_.empty() && pending_jobs_.empty()) {
                return;
            }

            prefetched = prefetched_jobs_.front();
            prefetched_jobs_.pop_front();
        }

        size_t idx = prefetched.index;
        const auto& job = jobs_[idx];
        auto& res = results_[idx];
        auto best_worker = prefetched.assigned_worker;

        if (!best_worker) {
            SUCO_LOG_WARNING("BatchProcessor: Job {} cannot be scheduled: No compatible worker exists.", job.source_file);
            res.exit_code = -1;
            
            // Try to prefetch the next one since we popped this one
            prefetch_next_job();
            continue;
        }

        socket_t worker_sock = best_worker->socket;

        {
            std::lock_guard<std::mutex> lock(state_.running_details_mutex);
            state_.running_job_details[job.source_file] = RunningJobDetail{ job.content_hash, job.command, job.preprocessed_source, INVALID_SOCKET_VAL, 1, job.header_set_hash };
        }

        timings_[idx].dispatch_steady = std::chrono::steady_clock::now();
        timings_[idx].worker_name = best_worker->name;

        auto comp_res = std::make_shared<CompileResult>();
        {
            std::lock_guard<std::mutex> lock(state_.results_mutex);
            state_.compile_results[job.source_file] = comp_res;
        }

        bool send_ok = false;
        auto tc_start = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(best_worker->write_mutex);
            uint32_t w_req_type = htonl(PACKET_COMPILE_REQ);
            uint32_t cmd_len_net = htonl(job.command.size());
            uint32_t file_len_net = htonl(job.source_file.size());
            uint32_t src_len_net = htonl(job.preprocessed_source.size());
            uint32_t tc_hash_len_worker = htonl(job.toolchain_hash.size());
            uint32_t hs_hash_len_worker = htonl(job.header_set_hash.size());
            uint32_t hs_src_len_worker = htonl(job.header_set_source.size());

            if (send_all(worker_sock, &w_req_type, 4) &&
                send_all(worker_sock, &cmd_len_net, 4) &&
                send_all(worker_sock, job.command.c_str(), job.command.size()) &&
                send_all(worker_sock, &file_len_net, 4) &&
                send_all(worker_sock, job.source_file.c_str(), job.source_file.size()) &&
                send_all(worker_sock, &job.source_compressed, 1) &&
                send_all(worker_sock, &src_len_net, 4) &&
                (job.preprocessed_source.empty() || send_all(worker_sock, job.preprocessed_source.c_str(), job.preprocessed_source.size())) &&
                send_all(worker_sock, &tc_hash_len_worker, 4) &&
                (job.toolchain_hash.empty() || send_all(worker_sock, job.toolchain_hash.c_str(), job.toolchain_hash.size())) &&
                send_all(worker_sock, &hs_hash_len_worker, 4) &&
                (job.header_set_hash.empty() || send_all(worker_sock, job.header_set_hash.c_str(), job.header_set_hash.size())) &&
                send_all(worker_sock, &job.hs_compressed, 1) &&
                send_all(worker_sock, &hs_src_len_worker, 4) &&
                (job.header_set_source.empty() || send_all(worker_sock, job.header_set_source.c_str(), job.header_set_source.size()))) {
                send_ok = true;
            }
        }
        auto tc_end = std::chrono::steady_clock::now();
        total_toolchain_ms_ += std::chrono::duration_cast<std::chrono::milliseconds>(tc_end - tc_start).count();

        if (!send_ok) {
            state_.mutex.lock();
            for (auto it = state_.active_jobs.begin(); it != state_.active_jobs.end(); ++it) {
                if (it->filename == job.source_file) { state_.active_jobs.erase(it); break; }
            }
            state_.mutex.unlock();

            {
                std::lock_guard<std::mutex> res_lock(state_.results_mutex);
                state_.compile_results.erase(job.source_file);
            }
            {
                std::lock_guard<std::mutex> lock(state_.running_details_mutex);
                state_.running_job_details.erase(job.source_file);
            }

            best_worker->slots_used = std::max(0, best_worker->slots_used - 1);
            res.exit_code = -1;

            prefetch_next_job();
            continue;
        }

        // Warte auf Fertigstellung
        auto comp_start = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> res_lock(comp_res->mutex);
        comp_res->cv.wait(res_lock, [&]() { return comp_res->ready; });

        res.exit_code = comp_res->exit_code;
        res.header_cache_hit = comp_res->header_cache_hit;
        res.log = comp_res->log;
        res.binary = comp_res->bin;
        res.bin_comp = comp_res->bin_comp;
        res_lock.unlock();

        auto comp_end = std::chrono::steady_clock::now();
        timings_[idx].compile_end_steady = comp_end;
        total_compilation_ms_ += std::chrono::duration_cast<std::chrono::milliseconds>(comp_end - comp_start).count();

        {
            std::lock_guard<std::mutex> map_lock(state_.results_mutex);
            state_.compile_results.erase(job.source_file);
        }
        {
            std::lock_guard<std::mutex> lock(state_.running_details_mutex);
            state_.running_job_details.erase(job.source_file);
        }

        if (res.exit_code == 0 && !res.binary.empty() && cache_) {
            cache_->put(job.content_hash, res.binary, res.log, res.bin_comp, job.source_file, job.command, "\"node\": \"" + (best_worker ? best_worker->name : "worker") + "\"");
        }

        // Slot freigeben
        best_worker->slots_used = std::max(0, best_worker->slots_used - 1);

        state_.mutex.lock();
        for (auto it = state_.active_jobs.begin(); it != state_.active_jobs.end(); ++it) {
            if (it->filename == job.source_file) {
                state_.active_jobs.erase(it);
                break;
            }
        }
        RecentJob rj{ job.source_file, res.exit_code, false, best_worker ? best_worker->name : "" };
        state_.recent_jobs.push_back(rj);
        if (state_.recent_jobs.size() > 20) {
            state_.recent_jobs.erase(state_.recent_jobs.begin());
        }
        state_.mutex.unlock();

        // Dynamically prefetch new job to fill empty slot
        prefetch_next_job();
    }
}

} // namespace suco
