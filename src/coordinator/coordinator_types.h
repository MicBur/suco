#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <memory>
#include "socket_util.h"
#include "worker_manager.h" // Für WorkerNode

namespace suco {

struct ActiveJob {
    uint64_t id;
    std::string filename;
    std::string worker_ip;
    std::string client_ip;
    std::string command;
    std::chrono::steady_clock::time_point start_time;
};

// Klassifiziert das Ziel-Betriebssystem eines Compile-Kommandos.
// Windows-Cross-Targets erkennt man am Compiler-Namen (MinGW-Triple / MSVC).
// Rückgabe: "windows", "linux" (Default) — nie leer, damit das Dashboard
// immer ein Badge zeigen kann.
inline std::string target_os_from_command(const std::string& cmd) {
    if (cmd.find("mingw")    != std::string::npos ||
        cmd.find("cl.exe")   != std::string::npos ||
        cmd.find("clang-cl") != std::string::npos) {
        return "windows";
    }
    return "linux";
}

struct RecentJob {
    std::string filename;
    int32_t exit_code;
    bool cache_hit;
    std::string worker_name;
    std::string target_os; // "windows" | "linux"
};

struct CompileResult {
    bool ready = false;
    int32_t exit_code = 0;
    bool header_cache_hit = false;
    std::string log;
    std::vector<uint8_t> bin;
    uint8_t bin_comp = 0;
    std::condition_variable cv;
    std::mutex mutex;
};

struct RunningJobDetail {
    std::string hash;
    std::string command;
    std::string source;
    socket_t client_sock;
    int attempts = 0;
    std::string header_set_hash;
};

#include <algorithm>

class HistoryWriter;

/**
 * @brief Bündelt den gemeinsamen Zustand des Coordinators, 
 * auf den Client- und Worker-Handler threadsicher zugreifen müssen.
 */
struct SharedCoordinatorState {
    std::mutex mutex;
    uint64_t next_job_id = 1;
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

    // T11: Asynchroner SQLite Writer
    std::shared_ptr<HistoryWriter> history_writer;

    // T12: Mapping content_hash -> worker to decrement slots_used on PACKET_CACHE_STORE
    std::unordered_map<std::string, std::shared_ptr<WorkerNode>> hash_to_worker;

    // T12: Track start times and filenames for SQLite logging on direct path compile store
    std::unordered_map<std::string, int64_t> hash_to_start_time;
    std::unordered_map<std::string, std::string> hash_to_filename;
    // Target OS ("windows"/"linux") derived from the query's required_compiler, so the
    // direct-dispatch store (where the command string is empty) can still label the job.
    std::unordered_map<std::string, std::string> hash_to_target_os;

    std::mutex known_header_sets_mutex;
    std::unordered_set<std::string> known_header_sets;

    // T11: SSE-Clients und Mutex
    std::mutex sse_mutex;
    std::vector<socket_t> sse_clients;

    void broadcast_sse_event(const std::string& event_type, const std::string& json_payload) {
        std::lock_guard<std::mutex> lock(sse_mutex);
        if (sse_clients.empty()) return;

        std::string sse_msg = "event: " + event_type + "\n" +
                              "data: " + json_payload + "\n\n";

        std::vector<socket_t> dead_clients;
        for (socket_t sock : sse_clients) {
            // Sende non-blocking. Wenn send() fehlschlägt oder EWOULDBLOCK/Partial-Send liefert -> trennen.
            int sent = ::send(sock, sse_msg.c_str(), static_cast<int>(sse_msg.size()), 0);
            if (sent != static_cast<int>(sse_msg.size())) {
                dead_clients.push_back(sock);
            }
        }

        // Bereinige tote Clients
        for (socket_t sock : dead_clients) {
            auto it = std::find(sse_clients.begin(), sse_clients.end(), sock);
            if (it != sse_clients.end()) {
                sse_clients.erase(it);
            }
            close_socket(sock);
        }
    }
};

} // namespace suco
