#include "batch_sender.h"
#include "logging.h"
#include "protocol.h"
#include "toolchain_packer.h"
#include "local_compiler.h"
#include "socket_util.h"
#include "network_client.h"
#include "hash_util.h"
#include "utils.h"
#include "header_set_hasher.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <map>
#include <future>
#include <filesystem>

#ifdef _WIN32
    #include <winsock2.h>
#else
    #include <arpa/inet.h>
    #include <sys/socket.h>
#endif

namespace {
std::mutex g_output_mutex;
}

namespace suco {

BatchSender::BatchSender(const ClientConfig& config, size_t max_parallel_batches, std::function<void(std::string, int)> on_job_completed, RequestContext context, suco::LocalSlotArbiter* active_local_semaphore)
    : config_(config), max_parallel_batches_(max_parallel_batches), on_job_completed_(std::move(on_job_completed)), context_(std::move(context)), active_local_semaphore_(active_local_semaphore) {
    workers_.reserve(max_parallel_batches_);
    for (size_t i = 0; i < max_parallel_batches_; ++i) {
        workers_.emplace_back([this](std::stop_token st) { worker_thread_loop(st); });
    }
    monitor_thread_ = std::jthread([this](std::stop_token st) { monitor_thread_loop(st); });
}

BatchSender::~BatchSender() {
    join();
}

void BatchSender::send_batch(std::vector<JobItem> batch) {
    if (workers_.empty()) {
        process_batch(batch);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        batch_queue_.push(std::move(batch));
    }
    cv_.notify_one();
}

void BatchSender::join() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    workers_.clear();
    monitor_thread_.request_stop();
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

void BatchSender::worker_thread_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        std::vector<JobItem> batch;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this, stop_token] {
                return stop_ || stop_token.stop_requested() || !batch_queue_.empty();
            });
            
            if ((stop_ || stop_token.stop_requested()) && batch_queue_.empty()) {
                return;
            }
            
            batch = std::move(batch_queue_.front());
            batch_queue_.pop();
        }
        
        active_batches_++;
        process_batch(batch);
        active_batches_--;
    }
}

void BatchSender::process_batch(std::vector<JobItem>& batch) {
    SUCO_LOG_INFO("Sending batch #{} with {} jobs", batches_sent_.load() + 1, batch.size());

    // Separate preprocessed failures from grid compiles
    std::vector<JobItem> raw_grid_jobs;
    raw_grid_jobs.reserve(batch.size());

    for (auto& item : batch) {
        if (!item.preprocess_success) {
            // Failed preprocessing -> direct local fallback
            SUCO_LOG_WARNING("Preprocessing failed for {}. Falling back to local compilation.", item.cmd.source_file);
            int exit_code = run_local_fallback(item);
            on_job_completed_(item.cmd.source_file, exit_code);
        } else {
            raw_grid_jobs.push_back(std::move(item));
        }
    }

    if (raw_grid_jobs.empty()) {
        return;
    }

    // 0. Normalize and Hash each grid job
    std::vector<JobItem> grid_jobs;
    grid_jobs.reserve(raw_grid_jobs.size());

    for (auto& item : raw_grid_jobs) {
        if (item.local_cache_hit) {
            grid_jobs.push_back(std::move(item));
            continue;
        }

        std::string normalized = suco::normalize_preprocessed_source(item.preprocess_output);
        if (normalized.empty()) {
            SUCO_LOG_WARNING("Normalized preprocessed source is empty for {}. Falling back to local compilation.", item.cmd.source_file);
            int exit_code = run_local_fallback(item);
            on_job_completed_(item.cmd.source_file, exit_code);
            continue;
        }

        suco::CacheKeyInput key{
            item.cmd.get_target_architecture(),
            item.cmd.get_compiler_version(),
            item.cmd.language_standard,
            suco::join(item.cmd.defines, "\x1F"),
            suco::join(item.cmd.include_paths, "\x1F"),
            suco::join(item.cmd.other_flags, "\x1F")
        };
        item.cmd.content_hash = suco::compute_cache_hash(normalized, key, context_);
        if (item.cmd.content_hash.empty()) {
            SUCO_LOG_ERROR("Failed to compute cache hash for {}. Falling back to local compilation.", item.cmd.source_file);
            int exit_code = run_local_fallback(item);
            on_job_completed_(item.cmd.source_file, exit_code);
            continue;
        }

        // Compute header set hash and split source
        item.cmd.preprocessed_source = item.preprocess_output;
        if (config_.header_cache_enabled) {
            HeaderSetHasher::compute_hash(item.cmd);
        }

        if (!item.cmd.header_set_hash.empty()) {
            item.cmd.preprocessed_source = item.cmd.stripped_source;
        }
        // Ship the FULL preprocessed output, not the normalized one: normalization
        // strips every line marker, and without them the compiler only knows the
        // temp .ii it was handed, so __FILE__/__LINE__ (and asserts, and debug info)
        // point at /tmp/suco_temp_X.ii instead of the real source. The header-set
        // path already ships markers for the same reason. The cache key stays on the
        // normalized text, so keying is unchanged and remains path-independent.
        // else: preprocessed_source already holds the full output — leave it alone.

        grid_jobs.push_back(std::move(item));
    }

    if (grid_jobs.empty()) {
        return;
    }

    // 1. Pack Toolchains for each job in the grid list
    for (auto& item : grid_jobs) {
        bool is_qt = false;
        for (const auto& inc : item.cmd.include_paths) {
            if (inc.find("qt") != std::string::npos || inc.find("QT") != std::string::npos) {
                is_qt = true; break;
            }
        }
        if (!is_qt) {
            for (const auto& def : item.cmd.defines) {
                if (def.find("qt") != std::string::npos || def.find("QT") != std::string::npos) {
                    is_qt = true; break;
                }
            }
        }
        
        ToolchainInfo tc_info = ToolchainPacker::pack(item.cmd.compiler_path, is_qt);
        if (tc_info.success) {
            item.cmd.toolchain_hash = tc_info.hash;
            item.cmd.compiler_path = tc_info.resolved_compiler_path;
        }
    }

    // 2. Ensure all toolchains in this batch are uploaded to the coordinator (Vorab-Check)
    std::vector<std::string> checked_hashes;
    for (const auto& item : grid_jobs) {
        if (item.cmd.toolchain_hash.empty()) continue;
        
        if (std::find(checked_hashes.begin(), checked_hashes.end(), item.cmd.toolchain_hash) != checked_hashes.end()) {
            continue;
        }
        
        NetworkClient network(config_);
        CompilerCommand dummy_cmd;
        dummy_cmd.content_hash = "dummy-tc-check-" + item.cmd.toolchain_hash;
        dummy_cmd.source_file = "dummy.cpp";
        dummy_cmd.toolchain_hash = item.cmd.toolchain_hash;
        dummy_cmd.compiler_path = item.cmd.compiler_path;
        
        network.try_get_from_cache(dummy_cmd); // This will handle upload automatically!
        checked_hashes.push_back(item.cmd.toolchain_hash);
    }

    // 3. Connect to Coordinator for batch request
    auto finished_flag = std::make_shared<bool>(false);
    {
        std::lock_guard<std::mutex> lock(active_batches_mutex_);
        ActiveBatchInfo ab;
        ab.jobs = grid_jobs;
        ab.start_time = std::chrono::steady_clock::now();
        ab.is_finished = finished_flag;
        active_batches_list_.push_back(ab);
    }

    NetworkClient network(config_);
    auto net_start = std::chrono::steady_clock::now();
    bool request_sent = network.send_batch_compile_request(grid_jobs);
    std::vector<BatchJobResult> results;
    if (request_sent) {
        results = network.read_batch_compile_response();
    }
    *finished_flag = true;
    {
        std::lock_guard<std::mutex> lock(active_batches_mutex_);
        active_batches_list_.erase(
            std::remove_if(active_batches_list_.begin(), active_batches_list_.end(),
                           [finished_flag](const ActiveBatchInfo& ab) { return ab.is_finished == finished_flag; }),
            active_batches_list_.end()
        );
    }
    auto net_end = std::chrono::steady_clock::now();
    uint64_t roundtrip_ms = std::chrono::duration_cast<std::chrono::milliseconds>(net_end - net_start).count();

    if (request_sent && !results.empty()) {
        total_coordinator_scheduling_ms_ += network.last_coord_scheduling_ms;
        total_worker_compilation_ms_ += network.last_worker_compilation_ms;
        total_toolchain_handling_ms_ += network.last_toolchain_handling_ms;
        total_network_roundtrip_ms_ += roundtrip_ms;
    }

    if (!request_sent || results.empty()) {
        // Coordinator communication failed entirely -> fallback all remaining jobs to local
        SUCO_LOG_ERROR("Batch sending failed or coordinator disconnected. Falling back all {} jobs to local.", grid_jobs.size());
        for (const auto& item : grid_jobs) {
            bool we_won = false;
            {
                std::lock_guard<std::mutex> lock(completed_jobs_mutex_);
                if (completed_jobs_.count(item.cmd.output_file) == 0) {
                    completed_jobs_.insert(item.cmd.output_file);
                    we_won = true;
                }
            }
            if (we_won) {
                int exit_code = run_local_fallback(item);
                on_job_completed_(item.cmd.source_file, exit_code);
            }
        }
        return;
    }

    // Process results for each grid job
    for (size_t i = 0; i < grid_jobs.size(); ++i) {
        const auto& item = grid_jobs[i];
        const auto* res = &results[i];

        bool we_won = false;
        {
            std::lock_guard<std::mutex> lock(completed_jobs_mutex_);
            if (completed_jobs_.count(item.cmd.output_file) == 0) {
                completed_jobs_.insert(item.cmd.output_file);
                we_won = true;
            }
        }

        if (!we_won) {
            SUCO_LOG_INFO("Remote compiler lost the race for {}", item.cmd.output_file);
            continue;
        }

        if (res->cache_hit) {
            SUCO_LOG_INFO("Cache hit for {}", item.cmd.source_file);
        } else {
            SUCO_LOG_INFO("Cache miss for {}", item.cmd.source_file);
            if (!item.cmd.header_set_hash.empty()) {
                if (res->header_cache_hit) {
                    header_cache_hits_++;
                } else {
                    header_cache_misses_++;
                }
            }
        }

        // Print stdout/stderr logs from compilation if present
        if (!res->log.empty()) {
            std::string log_str(res->log.begin(), res->log.end());
            if (item.client_socket != -1) {
                send_ipc_frame(item.client_socket, res->exit_code == 0 ? IPC_RESP_STDOUT : IPC_RESP_STDERR, log_str);
            } else {
                std::lock_guard<std::mutex> lock(g_output_mutex);
                if (res->exit_code == 0) {
                    std::cout << log_str;
                } else {
                    std::cerr << log_str;
                }
            }
        }

        if (res->exit_code == 0) {
            // Save the binary output
            std::filesystem::path out_path(item.cmd.output_file);
            if (out_path.is_relative() && !context_.cwd.empty()) {
                out_path = std::filesystem::path(context_.cwd) / out_path;
            }
            std::ofstream out(out_path, std::ios::binary);
            if (out.is_open() && !res->binary.empty()) {
                out.write(reinterpret_cast<const char*>(res->binary.data()), static_cast<std::streamsize>(res->binary.size()));
                if (out.good()) {
                    on_job_completed_(item.cmd.source_file, 0);
                    continue;
                }
            }
            SUCO_LOG_ERROR("Failed to write output file: {} (resolved: {}). Falling back locally.", item.cmd.output_file, out_path.string());
            int exit_code = run_local_fallback(item);
            on_job_completed_(item.cmd.source_file, exit_code);
        } else if (res->exit_code == -1 || res->exit_code == 127 || res->exit_code == -5) {
            // -1: coordinator grid failure/busy. 127: the worker's shell could not start
            // the compiler (toolchain wiped mid-build). -5: worker was told it knew a
            // header set it no longer has, so stripped source can't compile. All three
            // are infrastructure gaps, never a verdict about the source; failing the
            // user's build over them is wrong. Compile here (we hold the full source).
            SUCO_LOG_WARNING("Remote infrastructure failure ({}) for {} — compiling locally.", res->exit_code, item.cmd.source_file);
            int exit_code = run_local_fallback(item);
            on_job_completed_(item.cmd.source_file, exit_code);
        } else {
            // Compiler failed on worker (actual compile error)
            on_job_completed_(item.cmd.source_file, res->exit_code);
        }
    }
    batches_sent_++;
}

void BatchSender::monitor_thread_loop(std::stop_token stop_token) {
    int timeout_ms = config_.grid_timeout_ms;
    if (timeout_ms <= 0) {
        timeout_ms = 5000;
    }
    while (!stop_token.stop_requested()) {
        {
            // Interruptible 250 ms tick: the stop_token overload registers a stop
            // callback that wakes this wait the instant request_stop() is called
            // (destructor), so join() returns immediately instead of waiting out
            // the sleep. Racing cadence is unchanged when no stop is pending.
            std::unique_lock<std::mutex> wlk(monitor_wait_mutex_);
            monitor_wait_cv_.wait_for(wlk, stop_token, std::chrono::milliseconds(250),
                                      [] { return false; });
        }
        if (stop_token.stop_requested()) break;

        std::vector<ActiveBatchInfo> batches_to_race;
        {
            std::lock_guard<std::mutex> lock(active_batches_mutex_);
            auto now = std::chrono::steady_clock::now();
            for (auto& ab : active_batches_list_) {
                if (*ab.is_finished) continue;
                if (ab.local_racing_started) continue;

                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - ab.start_time).count();
                if (elapsed > timeout_ms) {
                    ab.local_racing_started = true;
                    batches_to_race.push_back(ab);
                }
            }
        }

        for (const auto& ab : batches_to_race) {
            SUCO_LOG_WARNING("Active racing triggered for batch of size {} because it exceeded timeout of {} ms", ab.jobs.size(), timeout_ms);
            for (const auto& job : ab.jobs) {
                // Check if already completed first
                {
                    std::lock_guard<std::mutex> lock(completed_jobs_mutex_);
                    if (completed_jobs_.count(job.cmd.output_file) > 0) {
                        continue;
                    }
                }

                // Spawn a duplicate compile thread
                std::thread([this, job, finished_flag = ab.is_finished]() {
                    // Try to acquire local slot semaphore
                    if (active_local_semaphore_) {
                        active_local_semaphore_->acquire();
                    }
                    
                    // Re-check completion status
                    bool already_done = false;
                    {
                        std::lock_guard<std::mutex> lock(completed_jobs_mutex_);
                        already_done = (completed_jobs_.count(job.cmd.output_file) > 0);
                    }

                    if (already_done || *finished_flag) {
                        if (active_local_semaphore_) {
                            active_local_semaphore_->release();
                        }
                        return;
                    }

                    SUCO_LOG_INFO("Local racing duplicate compile started for {}", job.cmd.output_file);
                    int exit_code = run_local_fallback(job);
                    
                    if (active_local_semaphore_) {
                        active_local_semaphore_->release();
                    }

                    // Try to complete the job
                    bool we_won = false;
                    {
                        std::lock_guard<std::mutex> lock(completed_jobs_mutex_);
                        if (completed_jobs_.count(job.cmd.output_file) == 0) {
                            completed_jobs_.insert(job.cmd.output_file);
                            we_won = true;
                        }
                    }

                    if (we_won) {
                        SUCO_LOG_INFO("Local racing duplicate compile WON the race for {}", job.cmd.output_file);
                        on_job_completed_(job.cmd.source_file, exit_code);
                    } else {
                        SUCO_LOG_INFO("Local racing duplicate compile lost the race for {}", job.cmd.output_file);
                    }
                }).detach();
            }
        }
    }
}

int BatchSender::run_local_fallback(const JobItem& item) {
    if (item.client_socket != -1) {
        t_client_socket = item.client_socket;
    }
    int exit_code = 0;
    {
        std::lock_guard<std::mutex> lock(g_output_mutex);
        exit_code = LocalCompiler::compile(item.cmd, context_.cwd);
    }
    if (item.client_socket != -1) {
        t_client_socket = -1;
    }
    if (exit_code == 0) {
        std::filesystem::path out_path(item.cmd.output_file);
        if (out_path.is_relative() && !context_.cwd.empty()) {
            out_path = std::filesystem::path(context_.cwd) / out_path;
        }
        NetworkClient network(config_);
        network.upload_to_cache(item.cmd.content_hash, out_path.string());
    }
    return exit_code;
}

} // namespace suco
