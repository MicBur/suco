#include "coordinator.h"
#include "protocol.h"
#include "logging.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <chrono>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace suco {

static const std::string FALLBACK_DASHBOARD_HTML = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <title>SUCO Dashboard - Fallback</title>
    <style>
        body { font-family: sans-serif; background: #080b11; color: #e2e8f0; padding: 2rem; text-align: center; }
        h1 { color: #00f2fe; }
    </style>
</head>
<body>
    <h1>SUCO Grid Monitor</h1>
    <p>dashboard.html wurde nicht gefunden. Bitte platziere die dashboard.html im Arbeitsverzeichnis.</p>
</body>
</html>
)HTML";

static std::string escape_json_string(const std::string& input) {
    std::string output;
    for (char c : input) {
        if (c == '\\') {
            output += "\\\\";
        } else if (c == '"') {
            output += "\\\"";
        } else if (c == '/') {
            output += "\\/";
        } else if (c == '\b') {
            output += "\\b";
        } else if (c == '\f') {
            output += "\\f";
        } else if (c == '\n') {
            output += "\\n";
        } else if (c == '\r') {
            output += "\\r";
        } else if (c == '\t') {
            output += "\\t";
        } else {
            output += c;
        }
    }
    return output;
}

Coordinator::Coordinator(const CoordinatorConfig& config)
    : m_config(config),
      m_cache(nullptr),
      m_worker_manager(config),
      m_scheduler(config),
      m_job_queue(),
      m_client_handler(m_config, m_job_queue, m_scheduler, m_worker_manager, m_state, m_cache),
      m_worker_handler(m_config, m_worker_manager, m_job_queue, m_state, [this](const std::string& ip) { on_worker_disconnected(ip); }),
      m_network_server(nullptr)
{
    // Callbacks für den NetworkServer registrieren
    m_network_server = std::make_unique<NetworkServer>(
        m_config,
        [this](socket_t s) { m_client_handler.handle_client_connection(s); },
        [this](socket_t s) { m_worker_handler.handle_worker_connection(s); }
    );
}

Coordinator::~Coordinator() {
    stop();
}

void Coordinator::start() {
    if (m_running.exchange(true)) {
        return; // Läuft bereits
    }

    SUCO_LOG_INFO("Orchestrator services starting...");
    
    // 1. LRU Cache initialisieren (5 GB Limit)
    m_cache = std::make_unique<LruCache>(
        m_config.get_cache_directory(),
        5ULL * 1024ULL * 1024ULL * 1024ULL
    );

    // 2. Network Server starten (TCP und UDP Discovery)
    m_network_server->start();

    // 3. REST Dashboard Web Server starten
    m_web_thread = std::thread(&Coordinator::run_web_server, this);

    // 4. Health Monitor starten
    m_monitor_thread = std::thread(&Coordinator::run_health_monitor, this);
}

void Coordinator::stop() {
    if (!m_running.exchange(false)) {
        return; // Läuft nicht
    }

    SUCO_LOG_INFO("Orchestrator services stopping...");

    // 1. Web Server Socket schließen, um Thread zu entblocken
    if (m_web_server_fd != INVALID_SOCKET_VAL) {
        close_socket(m_web_server_fd);
        m_web_server_fd = INVALID_SOCKET_VAL;
    }

    // 2. Network Server stoppen
    m_network_server->stop();

    // 3. Threads joinen
    if (m_web_thread.joinable()) {
        m_web_thread.join();
    }
    if (m_monitor_thread.joinable()) {
        m_monitor_thread.join();
    }

    // 4. Cache freigeben
    m_cache.reset();

    SUCO_LOG_INFO("Orchestrator services stopped.");
}

void Coordinator::on_worker_disconnected(const std::string& worker_ip) {
    SUCO_LOG_WARNING("Orchestrating failover for crashed worker {}", worker_ip);
    
    // 1. Reschedulet alle aktiven Jobs des ausgefallenen Workers in die JobQueue
    auto rescheduled_jobs = m_job_queue.reschedule_worker_jobs(worker_ip);
    if (!rescheduled_jobs.empty()) {
        SUCO_LOG_INFO("Successfully rescheduled {} jobs back to PENDING.", rescheduled_jobs.size());
    }

    // 2. Bereinige aktive Jobs im geteilten Zustand
    {
        std::lock_guard<std::mutex> lock(m_state.mutex);
        for (auto it = m_state.active_jobs.begin(); it != m_state.active_jobs.end();) {
            if (it->worker_ip == worker_ip) {
                it = m_state.active_jobs.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 3. Triggere Rescheduling für getrennte Worker-IP
    for (const auto& job_ptr : rescheduled_jobs) {
        std::string filename = job_ptr->filename;
        RunningJobDetail details;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(m_state.running_details_mutex);
            auto it = m_state.running_job_details.find(filename);
            if (it != m_state.running_job_details.end()) {
                details = it->second;
                found = true;
            }
        }

        if (found) {
            if (details.attempts >= 3) {
                SUCO_LOG_ERROR("Job {} reached max reschedule attempts (3). Aborting.", filename);
                
                std::shared_ptr<CompileResult> res;
                {
                    std::lock_guard<std::mutex> lock(m_state.results_mutex);
                    auto it = m_state.compile_results.find(filename);
                    if (it != m_state.compile_results.end()) {
                        res = it->second;
                    }
                }
                
                if (res) {
                    std::lock_guard<std::mutex> lock(res->mutex);
                    res->exit_code = -1;
                    res->log = "suco-coordinator error: Job aborted after worker crashed multiple times.";
                    res->ready = true;
                    res->cv.notify_all();
                }
            } else {
                // Erhöhe Versuche und markiere als PENDING in JobQueue
                {
                    std::lock_guard<std::mutex> lock(m_state.running_details_mutex);
                    m_state.running_job_details[filename].attempts++;
                }
                
                // Job zurück in PENDING Queue
                auto job = std::make_shared<Job>();
                job->filename = filename;
                job->hash = details.hash;
                job->command = details.command;
                job->source = details.source;
                m_job_queue.add_job(job);
            }
        }
    }
}

void Coordinator::run_health_monitor() {
    while (m_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_config.get_heartbeat_interval_ms()));
        if (!m_running) break;

        // Prüft, ob Worker das Heartbeat-Timeout überschritten haben
        auto dead_ips = m_worker_manager.cleanup_inactive_workers();
        for (const auto& ip : dead_ips) {
            on_worker_disconnected(ip);
        }
    }
}

void Coordinator::run_web_server() {
    uint16_t port = DEFAULT_WEB_PORT;
    m_web_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_web_server_fd == INVALID_SOCKET_VAL) {
        SUCO_LOG_ERROR("Failed to create web socket.");
        return;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(m_web_server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(m_web_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(m_web_server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        SUCO_LOG_ERROR("Web server bind failed on Port {}", port);
        close_socket(m_web_server_fd);
        m_web_server_fd = INVALID_SOCKET_VAL;
        return;
    }

    if (listen(m_web_server_fd, 10) < 0) {
        close_socket(m_web_server_fd);
        m_web_server_fd = INVALID_SOCKET_VAL;
        return;
    }

    SUCO_LOG_INFO("Web dashboard REST API listening on Port {}", port);

    while (m_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        socket_t client_sock = accept(m_web_server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET_VAL) {
            if (!m_running) break;
            continue;
        }

        std::thread([this, client_sock]() {
            char buffer[2048];
            std::memset(buffer, 0, sizeof(buffer));
#ifdef _WIN32
            int read_bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
#else
            ssize_t read_bytes = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
#endif
            if (read_bytes <= 0) {
                close_socket(client_sock);
                return;
            }

            std::string req(buffer);
            std::stringstream ss(req);
            std::string method, path;
            ss >> method >> path;

            if (method == "GET") {
                if (path == "/" || path == "/index.html" || path == "/dashboard.html") {
                    std::string html_content = FALLBACK_DASHBOARD_HTML;
                    std::ifstream file("dashboard.html");
                    if (!file.is_open()) {
                        file.open("../dashboard.html");
                    }
                    if (file.is_open()) {
                        std::stringstream file_ss;
                        file_ss << file.rdbuf();
                        html_content = file_ss.str();
                        file.close();
                    }

                    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
                                           std::to_string(html_content.size()) + "\r\nConnection: close\r\n\r\n" + html_content;
                    send(client_sock, response.c_str(), static_cast<int>(response.size()), 0);
                } else if (path == "/api/stats") {
                    std::stringstream json;
                    json << "{\n";
                    {
                        std::lock_guard<std::mutex> lock(m_state.mutex);
                        json << "  \"total_requests\": " << m_state.total_requests << ",\n";
                        json << "  \"cache_hits\": " << m_state.cache_hits << ",\n";
                        json << "  \"cache_misses\": " << m_state.cache_misses << ",\n";
                        json << "  \"active_jobs_count\": " << m_state.active_jobs.size() << ",\n";

                        // Aktive Compile-Jobs
                        json << "  \"active_jobs\": [\n";
                        auto now = std::chrono::steady_clock::now();
                        for (size_t i = 0; i < m_state.active_jobs.size(); ++i) {
                            const auto& job = m_state.active_jobs[i];
                            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - job.start_time).count();
                            json << "    {\n";
                            json << "      \"filename\": \"" << escape_json_string(job.filename) << "\",\n";
                            json << "      \"worker_ip\": \"" << job.worker_ip << "\",\n";
                            json << "      \"duration_ms\": " << elapsed << "\n";
                            json << "    }";
                            if (i + 1 < m_state.active_jobs.size()) json << ",";
                            json << "\n";
                        }
                        json << "  ],\n";

                        // Aktive Worker
                        json << "  \"workers\": [\n";
                        auto active_workers = m_worker_manager.get_active_workers();
                        for (size_t i = 0; i < active_workers.size(); ++i) {
                            const auto& w = active_workers[i];
                            json << "    {\n";
                            json << "      \"name\": \"" << w->name << "\",\n";
                            json << "      \"ip\": \"" << w->ip << "\",\n";
                            json << "      \"os\": \"" << w->os << "\",\n";
                            json << "      \"slots_total\": " << w->slots_total << ",\n";
                            json << "      \"slots_used\": " << w->slots_used << ",\n";
                            json << "      \"cpu_cores_usage\": [";
                            for (size_t c = 0; c < w->cpu_cores_usage.size(); ++c) {
                                json << w->cpu_cores_usage[c];
                                if (c + 1 < w->cpu_cores_usage.size()) json << ", ";
                            }
                            json << "]\n";
                            json << "    }";
                            if (i + 1 < active_workers.size()) json << ",";
                            json << "\n";
                        }
                        json << "  ],\n";

                        // Zuletzt beendete Jobs
                        json << "  \"recent_jobs\": [\n";
                        for (size_t i = 0; i < m_state.recent_jobs.size(); ++i) {
                            const auto& rj = m_state.recent_jobs[i];
                            json << "    {\n";
                            json << "      \"filename\": \"" << escape_json_string(rj.filename) << "\",\n";
                            json << "      \"exit_code\": " << rj.exit_code << ",\n";
                            json << "      \"cache_hit\": " << (rj.cache_hit ? "true" : "false") << "\n";
                            json << "    }";
                            if (i + 1 < m_state.recent_jobs.size()) json << ",";
                            json << "\n";
                        }
                        json << "  ]\n";
                    }
                    json << "}\n";

                    std::string json_str = json.str();
                    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                           std::to_string(json_str.size()) + "\r\nConnection: close\r\n\r\n" + json_str;
                    send(client_sock, response.c_str(), static_cast<int>(response.size()), 0);
                }
            }
            close_socket(client_sock);
        }).detach();
    }

    if (m_web_server_fd != INVALID_SOCKET_VAL) {
        close_socket(m_web_server_fd);
        m_web_server_fd = INVALID_SOCKET_VAL;
    }
}

} // namespace suco
