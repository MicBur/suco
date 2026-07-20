#include "preprocessing_pool.h"

namespace suco {

PreprocessingPool::PreprocessingPool(size_t thread_count) {
    workers_.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back([this](std::stop_token st) { worker_loop(st); });
    }
}

PreprocessingPool::~PreprocessingPool() {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all();
}

void PreprocessingPool::worker_loop(std::stop_token stop_token) {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            condition_.wait(lock, [this, stop_token] {
                return stop_ || stop_token.stop_requested() || !tasks_.empty();
            });
            
            if ((stop_ || stop_token.stop_requested()) && tasks_.empty()) {
                return;
            }
            
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        
        active_workers_++;
        task();
        active_workers_--;
    }
}

} // namespace suco
