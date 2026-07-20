#pragma once

#include "compiler_command.h"
#include "client_config.h"
#include "preprocessing_pool.h"
#include "batch_collector.h"
#include "batch_sender.h"
#include "hash_util.h"

#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <chrono>
#include <memory>
#include <atomic>
#include <future>
#include <semaphore>
#include "local_slot_arbiter.h"

namespace suco {

class PipelineOrchestrator {
public:
    PipelineOrchestrator(const ClientConfig& config, size_t total_jobs, RequestContext context, std::counting_semaphore<1024>* external_local_semaphore = nullptr);
    ~PipelineOrchestrator();

    void enqueue_job(const CompilerCommand& cmd, ipc_socket_t client_socket = -1);
    int run_and_join();
    int get_job_exit_code(const std::string& source_file);

private:
    void print_summary(double total_elapsed_s);

    const ClientConfig& config_;
    size_t total_jobs_;
    RequestContext context_;
    
    // Pipelining parameters based on aggressiveness
    std::string aggressiveness_level_;
    size_t batch_size_ = 16;
    size_t batch_timeout_ms_ = 5;
    size_t max_parallel_batches_ = 4;
    size_t num_preprocess_threads_ = 4;

    std::unique_ptr<PreprocessingPool> prep_pool_;
    std::unique_ptr<BatchCollector> collector_;
    std::unique_ptr<BatchSender> sender_;

    std::vector<std::future<void>> prep_futures_;
    
    // Metrics collection
    std::chrono::steady_clock::time_point build_start_time_;
    std::atomic<uint64_t> total_prep_time_ms_{0};
    
    std::mutex results_mutex_;
    std::map<std::string, int> completed_jobs_;
    int overall_exit_code_ = 0;

    // Local compilation slots
    int local_slots_ = 0;
    std::unique_ptr<LocalSlotArbiter> slot_arbiter_;
    std::atomic<size_t> local_compile_count_{0};

    std::vector<JobItem> preprocessed_items_;
    bool pipelining_enabled_ = true;

    // Direct dispatch: send cache-miss grid jobs straight to the coordinator-assigned
    // worker instead of funnelling the payload through the coordinator batch path.
    // Default on; disable with SUCO_DIRECT_DISPATCH=0/off/false.
    bool direct_dispatch_enabled_ = true;
    std::atomic<size_t> direct_dispatch_count_{0};

    // Backpressure: when the grid is saturated (coordinator assigns no worker), wait
    // up to this budget re-querying for a direct-dispatch slot instead of flooding the
    // coordinator funnel (which collapses under load -> local-fallback storm). Bounded
    // so a build never hangs. 0 disables the wait. Env: SUCO_GRID_WAIT_MS (default 8000).
    int grid_wait_budget_ms_ = 8000;
    std::atomic<size_t> grid_wait_events_{0};

    // Local content-addressed object cache hits (warm/incremental builds served
    // without any network round-trip).
    std::atomic<size_t> local_obj_hits_{0};
};

} // namespace suco
