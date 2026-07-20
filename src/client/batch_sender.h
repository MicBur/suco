#pragma once

#include "job_queue.h"
#include "client_config.h"
#include "hash_util.h"
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <string>

#include <semaphore>
#include "local_slot_arbiter.h"
#include <unordered_set>

namespace suco {

class BatchSender {
public:
    BatchSender(const ClientConfig& config, size_t max_parallel_batches, std::function<void(std::string, int)> on_job_completed, RequestContext context, suco::LocalSlotArbiter* active_local_semaphore = nullptr);
    ~BatchSender();

    // Disable copy/assignment
    BatchSender(const BatchSender&) = delete;
    BatchSender& operator=(const BatchSender&) = delete;

    // Send a batch of jobs (enqueues it to the sender threads)
    void send_batch(std::vector<JobItem> batch);

    // Wait for all sent batches to complete
    void join();

    size_t get_batches_sent_count() const {
        return batches_sent_.load();
    }

    size_t get_total_coordinator_scheduling_ms() const { return total_coordinator_scheduling_ms_.load(); }
    size_t get_total_worker_compilation_ms() const { return total_worker_compilation_ms_.load(); }
    size_t get_total_toolchain_handling_ms() const { return total_toolchain_handling_ms_.load(); }
    size_t get_total_network_roundtrip_ms() const { return total_network_roundtrip_ms_.load(); }

private:
    void worker_thread_loop(std::stop_token stop_token);
    void process_batch(std::vector<JobItem>& batch);
    int run_local_fallback(const JobItem& item);

    ClientConfig config_;
    size_t max_parallel_batches_;
    std::function<void(std::string, int)> on_job_completed_;
    RequestContext context_;

    std::queue<std::vector<JobItem>> batch_queue_;
    std::vector<std::jthread> workers_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    bool stop_ = false;

    suco::LocalSlotArbiter* active_local_semaphore_ = nullptr;

    struct ActiveBatchInfo {
        std::vector<JobItem> jobs;
        std::chrono::steady_clock::time_point start_time;
        bool local_racing_started = false;
        std::shared_ptr<bool> is_finished;
    };

    std::vector<ActiveBatchInfo> active_batches_list_;
    std::mutex active_batches_mutex_;
    std::jthread monitor_thread_;
    // Interruptible wait for the racing monitor so shutdown (jthread join) does not
    // block on an in-flight 250 ms sleep — that stall was ~250 ms of pure overhead
    // on every suco-cl++ invocation (dominant cost vs. icecream's near-free wrapper).
    std::mutex monitor_wait_mutex_;
    std::condition_variable_any monitor_wait_cv_;
    void monitor_thread_loop(std::stop_token stop_token);

    std::mutex completed_jobs_mutex_;
    std::unordered_set<std::string> completed_jobs_;

    std::atomic<size_t> batches_sent_{0};
    std::atomic<size_t> active_batches_{0};

    std::atomic<size_t> total_coordinator_scheduling_ms_{0};
    std::atomic<size_t> total_worker_compilation_ms_{0};
    std::atomic<size_t> total_toolchain_handling_ms_{0};
    std::atomic<size_t> total_network_roundtrip_ms_{0};

    std::atomic<size_t> header_cache_hits_{0};
    std::atomic<size_t> header_cache_misses_{0};

public:
    size_t get_header_cache_hits() const { return header_cache_hits_.load(); }
    size_t get_header_cache_misses() const { return header_cache_misses_.load(); }
};

} // namespace suco
