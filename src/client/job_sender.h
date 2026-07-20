#pragma once

#include "job_queue.h"
#include "client_config.h"
#include "network_client.h"
#include "hash_util.h"
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <string>

namespace suco {

class JobSender {
public:
    JobSender(JobQueue& queue, const ClientConfig& config, size_t num_sender_threads, RequestContext context);
    ~JobSender();

    // Disable copy/assignment
    JobSender(const JobSender&) = delete;
    JobSender& operator=(const JobSender&) = delete;

    void start();
    void join();

    // Get compilation exit codes for all source files
    std::map<std::string, int> get_results() const;

private:
    void sender_loop(size_t thread_id);
    int process_job_pipeline(JobItem& item, NetworkClient& network);
    int run_local_fallback(const CompilerCommand& cmd);

    JobQueue& queue_;
    const ClientConfig& config_;
    size_t num_sender_threads_;
    RequestContext context_;
    
    std::vector<std::jthread> threads_;
    
    mutable std::mutex results_mutex_;
    std::map<std::string, int> results_;
};

} // namespace suco
