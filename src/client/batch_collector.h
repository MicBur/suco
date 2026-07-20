#pragma once

#include "job_queue.h"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <chrono>

namespace suco {

class BatchCollector {
public:
    BatchCollector(size_t max_batch_size, size_t timeout_ms, std::function<void(std::vector<JobItem>)> on_batch_ready);
    ~BatchCollector();

    // Disable copy/assignment
    BatchCollector(const BatchCollector&) = delete;
    BatchCollector& operator=(const BatchCollector&) = delete;

    // Add a job to the collector
    void add_job(JobItem item);

    // Marks that no more jobs will be added and pushes any remaining batch
    void finish();

    // Waits for the collector thread to fully exit
    void join() {
        collector_thread_.request_stop();
        if (collector_thread_.joinable()) {
            collector_thread_.join();
        }
    }

private:
    void collector_loop(std::stop_token stop_token);

    size_t max_batch_size_;
    size_t timeout_ms_;
    std::function<void(std::vector<JobItem>)> on_batch_ready_;

    std::vector<JobItem> current_batch_;
    std::chrono::steady_clock::time_point batch_start_time_;
    
    std::jthread collector_thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool finished_ = false;
};

} // namespace suco
