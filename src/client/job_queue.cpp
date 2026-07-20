#include "job_queue.h"

namespace suco {

void JobQueue::push(JobItem item) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(item));
    }
    cv_.notify_one();
}

std::optional<JobItem> JobQueue::pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || finished_; });
    
    if (queue_.empty() && finished_) {
        return std::nullopt;
    }
    
    JobItem item = std::move(queue_.front());
    queue_.pop();
    return item;
}

void JobQueue::finish() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
    }
    cv_.notify_all();
}

size_t JobQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

} // namespace suco
