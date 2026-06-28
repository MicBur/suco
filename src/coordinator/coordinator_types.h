#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>
#include "socket_util.h"
#include "worker_manager.h" // Für WorkerNode

namespace suco {

struct ActiveJob {
    std::string filename;
    std::string worker_ip;
    std::chrono::steady_clock::time_point start_time;
};

struct RecentJob {
    std::string filename;
    int32_t exit_code;
    bool cache_hit;
};

struct CompileResult {
    bool ready = false;
    int32_t exit_code = 0;
    std::string log;
    std::vector<uint8_t> bin;
    std::condition_variable cv;
    std::mutex mutex;
};

struct RunningJobDetail {
    std::string hash;
    std::string command;
    std::string source;
    socket_t client_sock;
    int attempts = 0;
};

/**
 * @brief Bündelt den gemeinsamen Zustand des Coordinators, 
 * auf den Client- und Worker-Handler threadsicher zugreifen müssen.
 */
struct SharedCoordinatorState {
    std::mutex mutex;
    uint64_t total_requests = 0;
    uint64_t cache_hits = 0;
    uint64_t cache_misses = 0;
    
    // Map von Datei-Hash auf wartende Client-Sockets (Request Coalescing)
    std::unordered_map<std::string, std::vector<socket_t>> pending_compilations;
    
    // Aktive Kompilierjobs auf den Workern
    std::vector<ActiveJob> active_jobs;
    
    // Verlauf der letzten abgeschlossenen Jobs (für Dashboard)
    std::vector<RecentJob> recent_jobs;

    // Laufende Job-Ergebnisstrukturen
    std::mutex results_mutex;
    std::unordered_map<std::string, std::shared_ptr<CompileResult>> compile_results;

    // Rescheduling-Details laufender Jobs
    std::mutex running_details_mutex;
    std::unordered_map<std::string, RunningJobDetail> running_job_details;
};

} // namespace suco
