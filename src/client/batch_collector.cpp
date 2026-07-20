#include "batch_collector.h"

namespace suco {

BatchCollector::BatchCollector(size_t max_batch_size, size_t timeout_ms, std::function<void(std::vector<JobItem>)> on_batch_ready)
    : max_batch_size_(max_batch_size), timeout_ms_(timeout_ms), on_batch_ready_(std::move(on_batch_ready)) {
    collector_thread_ = std::jthread([this](std::stop_token st) { collector_loop(st); });
}

BatchCollector::~BatchCollector() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    cv_.notify_all();
    if (collector_thread_.joinable()) {
        collector_thread_.request_stop();
        collector_thread_.join();
    }
}

void BatchCollector::add_job(JobItem item) {
    std::vector<JobItem> ready_batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (current_batch_.empty()) {
            batch_start_time_ = std::chrono::steady_clock::now();
        }
        current_batch_.push_back(std::move(item));
        if (current_batch_.size() >= max_batch_size_) {
            ready_batch = std::move(current_batch_);
        }
    }
    if (!ready_batch.empty()) {
        on_batch_ready_(std::move(ready_batch));
    }
    cv_.notify_one();
}

void BatchCollector::finish() {
    std::vector<JobItem> remaining_batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        if (!current_batch_.empty()) {
            remaining_batch = std::move(current_batch_);
        }
    }
    if (!remaining_batch.empty()) {
        on_batch_ready_(std::move(remaining_batch));
    }
    cv_.notify_all();
}

void BatchCollector::collector_loop(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        std::vector<JobItem> timeout_batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (current_batch_.empty()) {
                cv_.wait(lock, [this, stop_token] {
                    return finished_ || stop_token.stop_requested() || !current_batch_.empty();
                });
            }
            
            if (finished_ || stop_token.stop_requested()) {
                if (!current_batch_.empty()) {
                    timeout_batch = std::move(current_batch_);
                }
            } else if (!current_batch_.empty()) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - batch_start_time_).count();
                if (elapsed >= static_cast<long long>(timeout_ms_)) {
                    timeout_batch = std::move(current_batch_);
                } else {
                    auto remaining = timeout_ms_ - elapsed;
                    cv_.wait_for(lock, std::chrono::milliseconds(remaining));
                    
                    now = std::chrono::steady_clock::now();
                    elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - batch_start_time_).count();
                    if (!current_batch_.empty() && (finished_ || elapsed >= static_cast<long long>(timeout_ms_))) {
                        timeout_batch = std::move(current_batch_);
                    }
                }
            }
        }
        
        if (!timeout_batch.empty()) {
            on_batch_ready_(std::move(timeout_batch));
        }
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (finished_ && current_batch_.empty()) {
                return;
            }
        }
    }
}

} // namespace suco
