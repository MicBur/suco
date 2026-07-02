#include "worker.h"
#include "metrics.h"
#include "job_executor.h"
#include "protocol.h"
#include "logging.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <thread>
#include <csignal>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace suco::worker {

static Worker* g_active_worker = nullptr;

static void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        SUCO_LOG_INFO("Received signal {}. Initiating graceful shutdown...", signal);
        if (g_active_worker) {
            g_active_worker->initiate_shutdown();
        }
    }
}

Worker::Worker(Config config)
    : m_config(std::move(config)),
      m_slots_total(0) {}

Worker::~Worker() {
    if (m_heartbeat_mgr) {
        m_heartbeat_mgr->stop();
    }
    m_net_client.disconnect();
    if (g_active_worker == this) {
        g_active_worker = nullptr;
    }
}

int Worker::run() {
    // 1. Initialisiere Zufallsgenerator und Signale
    srand(static_cast<unsigned int>(time(nullptr)));
    g_active_worker = this;
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 2. Bestimme Slotanzahl
    if (m_config.slots > 0) {
        m_slots_total = m_config.slots;
    } else {
        m_slots_total = std::max(1, Metrics::get_logical_cores());
    }

    while (!m_shutdown_requested) {
        // 3. Coordinator Discovery & Connect
        std::string host = m_config.coordinator_host;
        uint16_t port = m_config.coordinator_port;

        if (host.empty()) {
            SUCO_LOG_INFO("No coordinator address specified. Scanning network via UDP broadcast...");
            if (!m_net_client.discover_coordinator(host, port)) {
                host = "127.0.0.1";
                SUCO_LOG_WARNING("UDP Discovery timed out. Falling back to coordinator at {}:{}", host, port);
            }
        }

        if (m_shutdown_requested) break;

        SUCO_LOG_INFO("Connecting to coordinator at {}:{}...", host, port);
        if (!m_net_client.connect_to(host, port)) {
            if (!m_shutdown_requested) {
                SUCO_LOG_ERROR("Could not connect to coordinator. Retrying in 5 seconds...");
                for (int i = 0; i < 50 && !m_shutdown_requested; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            continue;
        }
        SUCO_LOG_INFO("Connected to coordinator.");

        if (m_shutdown_requested) {
            m_net_client.disconnect();
            break;
        }

        // 4. Registrierung
        std::string name = Metrics::get_host_name();
        std::string os = Metrics::get_os_name();

        if (!m_net_client.register_worker(name, os, m_slots_total)) {
            SUCO_LOG_ERROR("Worker registration failed. Retrying in 5 seconds...");
            m_net_client.disconnect();
            for (int i = 0; i < 50 && !m_shutdown_requested; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        SUCO_LOG_INFO("Registered successfully (Slots: {}, Name: {}, OS: {})", m_slots_total, name, os);

        if (m_shutdown_requested) {
            m_net_client.disconnect();
            break;
        }

        // 5. Starte Heartbeat-System
        m_heartbeat_mgr = std::make_unique<HeartbeatManager>(m_net_client, m_slots_total, m_slots_used);
        m_heartbeat_mgr->start();

        // 6. Gehe in blockierenden Compile Loop
        run_worker_compile_loop();

        // 7. Cleanup nach Abbruch/Verbindungsabbruch
        if (m_heartbeat_mgr) {
            m_heartbeat_mgr->stop();
            m_heartbeat_mgr.reset();
        }
        m_net_client.disconnect();

        if (!m_shutdown_requested) {
            SUCO_LOG_WARNING("Connection lost to coordinator. Reconnecting in 5 seconds...");
            for (int i = 0; i < 50 && !m_shutdown_requested; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    // 8. Warten auf aktive Jobs beim Shutdown
    SUCO_LOG_INFO("Waiting for active compile jobs to finish (Max 10 seconds)...");
    {
        std::unique_lock<std::mutex> lock(m_jobs_mutex);
        if (m_active_jobs_count > 0) {
            bool finished = m_jobs_cv.wait_for(lock, std::chrono::seconds(10), [this]() {
                return m_active_jobs_count == 0;
            });
            if (!finished) {
                SUCO_LOG_WARNING("Forced exit. {} jobs were still running.", m_active_jobs_count);
            } else {
                SUCO_LOG_INFO("All active jobs finished cleanly.");
            }
        } else {
            SUCO_LOG_INFO("No active jobs to wait for.");
        }
    }

    g_active_worker = nullptr;
    SUCO_LOG_INFO("Graceful shutdown complete.");
    return 0;
}

void Worker::initiate_shutdown() {
    m_shutdown_requested = true;
    m_net_client.disconnect(); // Das bricht den blockierenden read-Befehl im compile loop ab
    if (m_heartbeat_mgr) {
        m_heartbeat_mgr->stop();
    }
}

void Worker::run_worker_compile_loop() {
    while (!m_shutdown_requested) {
        uint32_t req_type_net = 0;
        if (!m_net_client.receive_packet(&req_type_net, 4)) {
            if (!m_shutdown_requested) {
                SUCO_LOG_WARNING("Connection lost to coordinator.");
            }
            break;
        }
        
        if (m_shutdown_requested) break;

        uint32_t req_type = ntohl(req_type_net);
        
        if (req_type != suco::PACKET_COMPILE_REQ) {
            SUCO_LOG_ERROR("Unexpected packet type {}", req_type);
            continue;
        }
        
        uint32_t cmd_len_net = 0;
        if (!m_net_client.receive_packet(&cmd_len_net, 4)) break;
        uint32_t cmd_len = ntohl(cmd_len_net);
        std::vector<char> cmd_buf(cmd_len);
        if (cmd_len > 0) {
            if (!m_net_client.receive_packet(cmd_buf.data(), cmd_len)) break;
        }
        std::string command(cmd_buf.data(), cmd_len);
        
        uint32_t file_len_net = 0;
        if (!m_net_client.receive_packet(&file_len_net, 4)) break;
        uint32_t file_len = ntohl(file_len_net);
        std::vector<char> file_buf(file_len);
        if (file_len > 0) {
            if (!m_net_client.receive_packet(file_buf.data(), file_len)) break;
        }
        std::string filename(file_buf.data(), file_len);
        
        uint32_t src_len_net = 0;
        if (!m_net_client.receive_packet(&src_len_net, 4)) break;
        uint32_t src_len = ntohl(src_len_net);
        std::vector<char> src_buf(src_len);
        if (src_len > 0) {
            if (!m_net_client.receive_packet(src_buf.data(), src_len)) break;
        }
        std::string source(src_buf.data(), src_len);
        
        if (m_shutdown_requested) {
            SUCO_LOG_WARNING("Rejecting job {} due to shutdown.", filename);
            break;
        }

        SUCO_LOG_INFO("Compiling job {}...", filename);
        m_slots_used++;
        
        {
            std::lock_guard<std::mutex> lock(m_jobs_mutex);
            m_active_jobs_count++;
        }

        std::thread([this, command, filename, source]() {
            this->handle_compile_job(command, filename, source);
            
            std::lock_guard<std::mutex> lock(m_jobs_mutex);
            m_active_jobs_count--;
            if (m_active_jobs_count == 0) {
                m_jobs_cv.notify_all();
            }
        }).detach();
    }
}

void Worker::handle_compile_job(const std::string& command, const std::string& filename, const std::string& source) {
    // Timeout an den Executor übergeben
    auto job_result = JobExecutor::execute(command, filename, source, m_config.job_timeout);
    
    // Slots-Auslastung sicher reduzieren
    m_slots_used = std::max(0, m_slots_used.load() - 1);
    
    if (m_shutdown_requested) {
        SUCO_LOG_WARNING("Job {} finished during shutdown. Result will be dropped.", filename);
        return;
    }

    uint32_t resp_type_net = htonl(suco::PACKET_COMPILE_RESP);
    uint32_t f_len_net = htonl(static_cast<uint32_t>(filename.size()));
    int32_t exit_code_net = htonl(job_result.exit_code);
    uint32_t log_len_net = htonl(static_cast<uint32_t>(job_result.log.size()));
    uint32_t bin_len_net = htonl(static_cast<uint32_t>(job_result.binary.size()));
    
    if (m_net_client.send_packet(&resp_type_net, 4) &&
        m_net_client.send_packet(&f_len_net, 4) &&
        m_net_client.send_packet(filename.c_str(), filename.size()) &&
        m_net_client.send_packet(&exit_code_net, 4) &&
        m_net_client.send_packet(&log_len_net, 4)) {
        
        if (!job_result.log.empty()) {
            m_net_client.send_packet(job_result.log.c_str(), job_result.log.size());
        }
        m_net_client.send_packet(&bin_len_net, 4);
        if (!job_result.binary.empty()) {
            m_net_client.send_packet(job_result.binary.data(), job_result.binary.size());
        }
    }
    
    SUCO_LOG_INFO("Finished job {} (Exit: {})", filename, job_result.exit_code);
}

} // namespace suco::worker
