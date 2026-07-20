#include "socket_util.h"
#include "ipc_protocol.h"
#include "suco_client.h"
#include "logging.h"
#include "compiler_command.h"
#include "client_config.h"
#include "hash_util.h"
#include "pipeline_orchestrator.h"
#include "local_compiler.h"

#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <map>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <semaphore>
#include <signal.h>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <condition_variable>

#ifndef _WIN32
#include <sys/un.h>
#endif

namespace suco {

namespace {

std::atomic<int> g_active_connections{0};
std::atomic<bool> g_stop_daemon{false};
std::chrono::steady_clock::time_point g_last_activity_time;
std::mutex g_activity_mutex;

// Helper to deserialize request values
bool read_uint32_t_socket(socket_t sock, uint32_t& val) {
    uint32_t val_net = 0;
    if (!read_all(sock, &val_net, 4)) return false;
    val = ntohl(val_net);
    return true;
}

bool read_string_socket(socket_t sock, std::string& str) {
    uint32_t len = 0;
    if (!read_uint32_t_socket(sock, len)) return false;
    str.resize(len);
    if (len > 0) {
        if (!read_all(sock, &str[0], len)) return false;
    }
    return true;
}

// Global semaphore to manage slots across parallel compiles
std::unique_ptr<std::counting_semaphore<1024>> g_global_semaphore;

void update_activity() {
    std::lock_guard<std::mutex> lock(g_activity_mutex);
    g_last_activity_time = std::chrono::steady_clock::now();
}

struct AggregatedJob {
    CompilerCommand cmd;
    RequestContext context;
    ClientConfig config;
    socket_t client_fd;
    
    std::shared_ptr<std::mutex> job_mutex;
    std::shared_ptr<std::condition_variable> job_cv;
    std::shared_ptr<bool> job_completed;
    std::shared_ptr<int> job_exit_code;
};

std::mutex g_agg_mutex;
std::vector<AggregatedJob> g_agg_queue;
std::condition_variable g_agg_cv;

void aggregator_thread_loop() {
    while (!g_stop_daemon) {
        std::vector<AggregatedJob> batch;
        {
            std::unique_lock<std::mutex> lock(g_agg_mutex);
            g_agg_cv.wait(lock, [&]() { return !g_agg_queue.empty() || g_stop_daemon; });
            if (g_stop_daemon) break;
            
            // Aggregation window: wait 3ms for more parallel files to arrive
            // only if there are other active connections and the queue is not yet full
            if (g_active_connections > 1 && g_agg_queue.size() < 12) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
                lock.lock();
            }
            
            // Batch cap: how many jobs one orchestrator handles before its
            // run_and_join() barrier. Smaller = results return to make sooner (better
            // pipeline fill, less head-of-line blocking behind a slow file); larger =
            // more internal aggregation. Tunable via SUCO_DAEMON_BATCH for profiling.
            static const size_t batch_cap = []() -> size_t {
                const char* e = std::getenv("SUCO_DAEMON_BATCH");
                if (e) { try { size_t v = std::stoul(e); if (v >= 1) return v; } catch (...) {} }
                return 16;
            }();
            std::string target_cwd = g_agg_queue[0].context.cwd;
            for (auto it = g_agg_queue.begin(); it != g_agg_queue.end() && batch.size() < batch_cap; ) {
                if (it->context.cwd == target_cwd) {
                    batch.push_back(std::move(*it));
                    it = g_agg_queue.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        if (batch.empty()) continue;
        
        SUCO_LOG_INFO("Aggregator: Processing batch of {} compiled files...", batch.size());
        
        try {
            PipelineOrchestrator orchestrator(batch[0].config, batch.size(), batch[0].context, g_global_semaphore.get());
            for (auto& job : batch) {
                orchestrator.enqueue_job(job.cmd, job.client_fd);
            }
            orchestrator.run_and_join();
            
            for (auto& job : batch) {
                int ec = orchestrator.get_job_exit_code(job.cmd.source_file);
                std::lock_guard<std::mutex> j_lock(*job.job_mutex);
                *job.job_exit_code = ec;
                *job.job_completed = true;
                job.job_cv->notify_one();
            }
        } catch (const std::exception& e) {
            SUCO_LOG_ERROR("Aggregator thread exception: {}", e.what());
            for (auto& job : batch) {
                std::lock_guard<std::mutex> j_lock(*job.job_mutex);
                *job.job_exit_code = 1;
                *job.job_completed = true;
                job.job_cv->notify_one();
            }
        } catch (...) {
            SUCO_LOG_ERROR("Aggregator thread unknown exception");
            for (auto& job : batch) {
                std::lock_guard<std::mutex> j_lock(*job.job_mutex);
                *job.job_exit_code = 1;
                *job.job_completed = true;
                job.job_cv->notify_one();
            }
        }
    }
}

void handle_client(socket_t client_fd) {
    g_active_connections++;
    update_activity();

    SUCO_LOG_DEBUG("Daemon accepted connection, fd={}", client_fd);

    try {
        uint32_t version = 0;
        if (!read_uint32_t_socket(client_fd, version)) {
            SUCO_LOG_ERROR("Failed to read protocol version");
            g_active_connections--;
            close_socket(client_fd);
            return;
        }

        if (version != DAEMON_PROTOCOL_VERSION) {
            SUCO_LOG_ERROR("Protocol version mismatch: client={}, daemon={}", version, DAEMON_PROTOCOL_VERSION);
            send_ipc_frame(client_fd, IPC_RESP_STDERR, "suco error: protocol version mismatch\n");
            send_ipc_frame(client_fd, IPC_RESP_EXIT, "1");
            g_active_connections--;
            close_socket(client_fd);
            return;
        }

        std::string cwd;
        if (!read_string_socket(client_fd, cwd)) {
            SUCO_LOG_ERROR("Failed to read CWD");
            g_active_connections--;
            close_socket(client_fd);
            return;
        }

        uint32_t env_count = 0;
        if (!read_uint32_t_socket(client_fd, env_count)) {
            SUCO_LOG_ERROR("Failed to read env count");
            g_active_connections--;
            close_socket(client_fd);
            return;
        }

        std::map<std::string, std::string> env_overrides;
        for (uint32_t i = 0; i < env_count; ++i) {
            std::string key, val;
            if (!read_string_socket(client_fd, key) || !read_string_socket(client_fd, val)) {
                SUCO_LOG_ERROR("Failed to read env key-value pair");
                g_active_connections--;
                close_socket(client_fd);
                return;
            }
            env_overrides[key] = val;
        }

        uint32_t cmd_count = 0;
        if (!read_uint32_t_socket(client_fd, cmd_count)) {
            SUCO_LOG_ERROR("Failed to read command count");
            g_active_connections--;
            close_socket(client_fd);
            return;
        }

        std::vector<CompilerCommand> commands;
        for (uint32_t c = 0; c < cmd_count; ++c) {
            uint32_t argc = 0;
            if (!read_uint32_t_socket(client_fd, argc)) {
                SUCO_LOG_ERROR("Failed to read argc");
                g_active_connections--;
                close_socket(client_fd);
                return;
            }

            std::vector<std::string> args;
            args.push_back("suco");
            args.reserve(argc + 1);
            for (uint32_t i = 0; i < argc; ++i) {
                std::string arg;
                if (!read_string_socket(client_fd, arg)) {
                    SUCO_LOG_ERROR("Failed to read argument");
                    g_active_connections--;
                    close_socket(client_fd);
                    return;
                }
                args.push_back(arg);
            }

            try {
                auto parsed = CompilerCommand::parse_all(static_cast<int>(args.size()), const_cast<char**>(
                    std::vector<const char*>(args.size() ? [&]() {
                        std::vector<const char*> res;
                        for (const auto& a : args) res.push_back(a.c_str());
                        return res;
                    }() : std::vector<const char*>()).data()
                ));
                for (auto&& p : parsed) {
                    commands.push_back(std::move(p));
                }
            } catch (const std::exception& e) {
                SUCO_LOG_ERROR("Failed to parse compiler arguments: {}", e.what());
                send_ipc_frame(client_fd, IPC_RESP_STDERR, std::string("suco error: parsing args: ") + e.what() + "\n");
                send_ipc_frame(client_fd, IPC_RESP_EXIT, "1");
                t_client_socket = -1;
                g_active_connections--;
                close_socket(client_fd);
                return;
            }
        }

        RequestContext context;
        context.cwd = cwd;
        context.env_overrides = env_overrides;
        
        auto it_pn = env_overrides.find("SUCO_PATH_NORMALIZATION");
        if (it_pn != env_overrides.end()) {
            context.path_normalization = (it_pn->second == "1" || it_pn->second == "true");
        } else {
            context.path_normalization = true;
        }

        ClientConfig config = ClientConfig::load_or_default(context.env_overrides);
        
        int exit_code = 0;

        if (commands.empty()) {
            exit_code = 0;
            t_client_socket = client_fd;
            send_ipc_frame(client_fd, IPC_RESP_EXIT, std::to_string(exit_code));
        } else if (commands.size() == 1 && !commands[0].is_compilation_step()) {
            t_client_socket = client_fd;
            exit_code = LocalCompiler::execute_direct(commands[0].raw_args, context.cwd);
            send_ipc_frame(client_fd, IPC_RESP_EXIT, std::to_string(exit_code));
        } else {
            // Dies ist ein Compilation Step -> Einreihen in den globalen Aggregator!
            t_client_socket = -1; // Trennen für diese Thread-Ausführung
            
            auto job_mutex = std::make_shared<std::mutex>();
            auto job_cv = std::make_shared<std::condition_variable>();
            auto job_completed = std::make_shared<bool>(false);
            auto job_exit_code = std::make_shared<int>(0);
            
            {
                std::lock_guard<std::mutex> lock(g_agg_mutex);
                g_agg_queue.push_back(AggregatedJob{
                    .cmd = commands[0], // da commands.size() == 1 bei Client-Invocations
                    .context = context,
                    .config = config,
                    .client_fd = client_fd,
                    .job_mutex = job_mutex,
                    .job_cv = job_cv,
                    .job_completed = job_completed,
                    .job_exit_code = job_exit_code
                });
                g_agg_cv.notify_one();
            }
            
            std::unique_lock<std::mutex> u_lock(*job_mutex);
            job_cv->wait(u_lock, [&]() { return *job_completed; });
            exit_code = *job_exit_code;
            
            send_ipc_frame(client_fd, IPC_RESP_EXIT, std::to_string(exit_code));
        }

    } catch (const std::exception& e) {
        SUCO_LOG_ERROR("Error handling client request: {}", e.what());
        send_ipc_frame(client_fd, IPC_RESP_STDERR, std::string("suco daemon error: ") + e.what() + "\n");
        send_ipc_frame(client_fd, IPC_RESP_EXIT, "1");
    } catch (...) {
        SUCO_LOG_ERROR("Unknown error handling client request");
        send_ipc_frame(client_fd, IPC_RESP_STDERR, "suco daemon error: unknown exception\n");
        send_ipc_frame(client_fd, IPC_RESP_EXIT, "1");
    }

    t_client_socket = -1;
    g_active_connections--;
    update_activity();
    close_socket(client_fd);
}

// Idle timeout thread function (terminates daemon if inactive for 5 minutes)
void idle_timeout_monitor(socket_t listen_fd) {
    const int timeout_s = 300; // 5 minutes
    while (!g_stop_daemon) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (g_active_connections == 0) {
            std::chrono::steady_clock::time_point last;
            {
                std::lock_guard<std::mutex> lock(g_activity_mutex);
                last = g_last_activity_time;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - last
            ).count();

            if (elapsed >= timeout_s) {
                SUCO_LOG_INFO("Daemon idle timeout reached (5 minutes). Shutting down.");
                g_stop_daemon = true;
                close_socket(listen_fd);
                std::exit(0);
            }
        }
    }
}

} // namespace

} // namespace suco

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

#ifndef _WIN32
    // Ignore SIGPIPE to avoid crashing when clients abruptly disconnect
    signal(SIGPIPE, SIG_IGN);
#endif

    suco::SocketInit sock_init;
    suco::update_activity();

    // Determine global semaphore local slot count
    unsigned int hw_cores = std::thread::hardware_concurrency();
    int default_local_slots = static_cast<int>(hw_cores > 2 ? hw_cores - 2 : 1);
    const char* env_local_slots = std::getenv("SUCO_LOCAL_SLOTS");
    if (env_local_slots) {
        try {
            int ls = std::stoi(env_local_slots);
            if (ls >= 0) default_local_slots = ls;
        } catch (...) {}
    }
    suco::g_global_semaphore = std::make_unique<std::counting_semaphore<1024>>(default_local_slots);

    std::string sock_path = suco::get_daemon_socket_path();
    SUCO_LOG_INFO("Starting SUCO Daemon on {} ...", sock_path);
    SUCO_LOG_INFO("Global local slots capacity: {}", default_local_slots);

#ifdef _WIN32
    SUCO_LOG_ERROR("Daemon mode not implemented on Windows");
    return 1;
#else
    // Remove stale socket file if it exists
    std::filesystem::remove(sock_path);

    socket_t listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        SUCO_LOG_ERROR("Failed to create Unix Domain Socket");
        return 1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        SUCO_LOG_ERROR("Failed to bind socket to {}", sock_path);
        close(listen_fd);
        return 1;
    }

    // Set permission 0600 on socket file
    chmod(sock_path.c_str(), 0600);

    if (listen(listen_fd, 64) < 0) {
        SUCO_LOG_ERROR("Failed to listen on socket");
        close(listen_fd);
        return 1;
    }

    // Start idle monitor thread
    std::thread(suco::idle_timeout_monitor, listen_fd).detach();

    // Start background batch aggregator thread
    std::thread(suco::aggregator_thread_loop).detach();

    SUCO_LOG_INFO("SUCO Daemon successfully listening");

    while (!suco::g_stop_daemon) {
        struct sockaddr_un client_addr{};
        socklen_t len = sizeof(client_addr);
        socket_t client_fd = accept(listen_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &len);
        if (client_fd >= 0) {
            std::thread(suco::handle_client, client_fd).detach();
        } else {
            if (suco::g_stop_daemon) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    close(listen_fd);
    std::filesystem::remove(sock_path);
    return 0;
#endif
}
