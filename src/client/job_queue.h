#pragma once

#include "compiler_command.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <string>

#include "ipc_protocol.h"

namespace suco {

struct JobItem {
    CompilerCommand cmd;
    bool preprocess_success = false;
    std::string preprocess_output;
    int preprocess_exit_code = 0;
    bool local_cache_hit = false;
    ipc_socket_t client_socket = -1;
};

class JobQueue {
public:
    JobQueue() = default;
    ~JobQueue() = default;

    // Disable copy/assignment
    JobQueue(const JobQueue&) = delete;
    JobQueue& operator=(const JobQueue&) = delete;

    // Push a job to the queue
    void push(JobItem item);

    // Pop a job from the queue (blocking)
    // Returns std::nullopt if the queue is finished/closed
    std::optional<JobItem> pop();

    // Marks the queue as finished (no more items will be pushed)
    void finish();

    // Returns current queue size
    size_t size() const;

private:
    std::queue<JobItem> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool finished_ = false;
};

} // namespace suco
