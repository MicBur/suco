#pragma once

#include <string>
#include <vector>
#include <queue>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <atomic>
#include <thread>
#include "config.h"
#include "job_queue.h"
#include "scheduler.h"
#include "worker_manager.h"
#include "socket_util.h"
#include "coordinator_types.h"
#include "lru_cache.h"

namespace suco {

class BatchProcessor {
public:
    struct BatchJob {
        std::string content_hash;
        std::string source_file;
        std::string preprocessed_source;
        std::string toolchain_hash;
        std::string command;
        std::string required_compiler;
        std::string required_compiler_version;
        std::string header_set_hash;
        std::string header_set_source;
        uint8_t source_compressed = 0;
        uint8_t hs_compressed = 0;
    };

    struct JobResult {
        std::string content_hash;
        int32_t exit_code = -1;
        bool cache_hit = false;
        bool header_cache_hit = false;
        std::string log;
        std::vector<uint8_t> binary;
        uint8_t bin_comp = 0;
    };

    BatchProcessor(const CoordinatorConfig& config,
                   JobQueue& job_queue,
                   const Scheduler& scheduler,
                   WorkerManager& worker_manager,
                   SharedCoordinatorState& state,
                   std::unique_ptr<LruCache>& cache);
    ~BatchProcessor() = default;

    void process_batch_request(socket_t client_sock, const std::string& client_ip);

private:
    struct PrefetchedJob {
        size_t index;
        std::shared_ptr<WorkerNode> assigned_worker;
    };

    void prefetch_next_job();
    void compile_worker_loop();

    struct JobTiming {
        std::chrono::system_clock::time_point queue_start_system;
        std::chrono::steady_clock::time_point queue_start_steady;
        std::chrono::steady_clock::time_point dispatch_steady;
        std::chrono::steady_clock::time_point compile_end_steady;
        std::chrono::steady_clock::time_point result_steady;
        std::string worker_name;
    };

    const CoordinatorConfig& config_;
    JobQueue& job_queue_;
    const Scheduler& scheduler_;
    WorkerManager& worker_manager_;
    SharedCoordinatorState& state_;
    std::unique_ptr<LruCache>& cache_;

    size_t prefetch_limit_ = 8;
    size_t num_threads_ = 12;

    std::vector<BatchJob> jobs_;
    std::vector<JobResult> results_;
    std::vector<JobTiming> timings_;

    std::deque<size_t> pending_jobs_;
    std::deque<PrefetchedJob> prefetched_jobs_;

    std::mutex prefetch_mutex_;
    std::condition_variable cv_prefetch_;

    std::atomic<uint64_t> total_scheduling_ms_{0};
    std::atomic<uint64_t> total_compilation_ms_{0};
    std::atomic<uint64_t> total_toolchain_ms_{0};
};

} // namespace suco
