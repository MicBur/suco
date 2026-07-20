#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

namespace suco {

class PreprocessingPool {
public:
    explicit PreprocessingPool(size_t thread_count);
    ~PreprocessingPool();

    // Disable copying/assignment
    PreprocessingPool(const PreprocessingPool&) = delete;
    PreprocessingPool& operator=(const PreprocessingPool&) = delete;

    // Enqueue a task for preprocessing
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
        std::future<return_type> res = task->get_future();
        bool run_sync = false;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (workers_.empty()) {
                run_sync = true;
            } else {
                if (stop_) {
                    throw std::runtime_error("enqueue on stopped PreprocessingPool");
                }
                tasks_.emplace([task]() { (*task)(); });
            }
        }
        if (run_sync) {
            (*task)();
        } else {
            condition_.notify_one();
        }
        return res;
    }

    size_t get_active_workers_count() const {
        return active_workers_.load();
    }

private:
    void worker_loop(std::stop_token stop_token);

    std::vector<std::jthread> workers_;
    std::queue<std::function<void()>> tasks_;
    
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    bool stop_ = false;
    std::atomic<size_t> active_workers_{0};
};

} // namespace suco
