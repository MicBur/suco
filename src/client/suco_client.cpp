#include "suco_client.h"
#include "local_compiler.h"
#include "pipeline_orchestrator.h"
#include "socket_util.h"
#include "ipc_protocol.h"
#include "logging.h"

#include <vector>
#include <map>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <cstring>

#ifndef _WIN32
#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#endif

extern char** environ;

namespace suco {

namespace {

std::map<std::string, std::string> get_whitelisted_env() {
    std::map<std::string, std::string> overrides;
    for (char** env = environ; *env != nullptr; ++env) {
        std::string s(*env);
        size_t eq = s.find('=');
        if (eq != std::string::npos) {
            std::string key = s.substr(0, eq);
            std::string val = s.substr(eq + 1);
            if (key.starts_with("SUCO_") || key == "PATH") {
                overrides[key] = val;
            }
        }
    }
    return overrides;
}

std::string get_executable_directory() {
#ifdef _WIN32
    return ".";
#else
    try {
        auto p = std::filesystem::read_symlink("/proc/self/exe").parent_path();
        return p.string();
    } catch (...) {
        return ".";
    }
#endif
}

bool try_autostart_daemon() {
#ifdef _WIN32
    return false;
#else
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        close(0);
        close(1);
        close(2);
        (void)open("/dev/null", O_RDONLY);
        (void)open("/dev/null", O_WRONLY);
        (void)open("/dev/null", O_WRONLY);

        std::string daemon_path = get_executable_directory() + "/suco-daemon";
        char* args[] = { const_cast<char*>("suco-daemon"), nullptr };
        
        execv(daemon_path.c_str(), args);
        // Fallback to execvp path if not found in same directory
        execvp("suco-daemon", args);
        std::exit(1);
    }
    return pid > 0;
#endif
}

// IPC Client logic
int run_via_daemon(const std::vector<CompilerCommand>& commands, const std::string& sock_path) {
#ifdef _WIN32
    return -1;
#else
    socket_t sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        // Try autostart
        if (!try_autostart_daemon()) {
            close(sock);
            return -1;
        }

        // Wait/retry connecting up to 20 times (1 second max)
        bool connected = false;
        for (int retry = 0; retry < 20; ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            socket_t new_sock = socket(AF_UNIX, SOCK_STREAM, 0);
            if (new_sock >= 0) {
                if (connect(new_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) >= 0) {
                    close(sock);
                    sock = new_sock;
                    connected = true;
                    break;
                }
                close(new_sock);
            }
        }

        if (!connected) {
            close(sock);
            return -1;
        }
    }

    // Connected!
    // Serialize and send request
    std::string request;
    
    // 1. Version
    uint32_t ver_net = htonl(DAEMON_PROTOCOL_VERSION);
    request.append(reinterpret_cast<const char*>(&ver_net), 4);

    // 2. CWD
    std::string cwd = std::filesystem::current_path().string();
    uint32_t cwd_len_net = htonl(cwd.size());
    request.append(reinterpret_cast<const char*>(&cwd_len_net), 4);
    request.append(cwd);

    // 3. Env overrides
    auto envs = get_whitelisted_env();
    uint32_t envs_size_net = htonl(envs.size());
    request.append(reinterpret_cast<const char*>(&envs_size_net), 4);
    for (const auto& [k, v] : envs) {
        uint32_t k_len_net = htonl(k.size());
        request.append(reinterpret_cast<const char*>(&k_len_net), 4);
        request.append(k);

        uint32_t v_len_net = htonl(v.size());
        request.append(reinterpret_cast<const char*>(&v_len_net), 4);
        request.append(v);
    }

    // 4. Commands count
    uint32_t cmd_count_net = htonl(commands.size());
    request.append(reinterpret_cast<const char*>(&cmd_count_net), 4);
    for (const auto& cmd : commands) {
        uint32_t argc_net = htonl(cmd.raw_args.size());
        request.append(reinterpret_cast<const char*>(&argc_net), 4);
        for (const auto& arg : cmd.raw_args) {
            uint32_t arg_len_net = htonl(arg.size());
            request.append(reinterpret_cast<const char*>(&arg_len_net), 4);
            request.append(arg);
        }
    }

    if (!send_all(sock, request.data(), request.size())) {
        close(sock);
        return -1;
    }

    // Read response stream
    int exit_code = 0;
    while (true) {
        uint8_t type = 0;
        if (!read_all(sock, &type, 1)) {
            close(sock);
            return -1; // Abrupt disconnect -> fail/fallback
        }

        uint32_t len_net = 0;
        if (!read_all(sock, &len_net, 4)) {
            close(sock);
            return -1;
        }
        uint32_t len = ntohl(len_net);

        std::string payload;
        payload.resize(len);
        if (len > 0) {
            if (!read_all(sock, &payload[0], len)) {
                close(sock);
                return -1;
            }
        }

        if (type == IPC_RESP_STDOUT) {
            std::cout << payload;
        } else if (type == IPC_RESP_STDERR) {
            std::cerr << payload;
        } else if (type == IPC_RESP_EXIT) {
            try {
                exit_code = std::stoi(payload);
            } catch (...) {
                exit_code = 1;
            }
            break;
        }
    }

    close(sock);
    return exit_code;
#endif
}

} // namespace

SucoClient::SucoClient(ClientConfig config)
    : config_(std::move(config)) {}

int SucoClient::run(const CompilerCommand& command) {
    return run(std::vector<CompilerCommand>{ command });
}

int SucoClient::run(const std::vector<CompilerCommand>& commands) {
    if (commands.empty()) {
        return 0;
    }

    if (commands.size() == 1 && !commands[0].is_compilation_step()) {
        return LocalCompiler::execute_direct(commands[0].raw_args);
    }

    // 1. Compilation mode. DEFAULT: standalone per-file (opt-in daemon via SUCO_DAEMON=1).
    //
    // The daemon (v2.1 default) aggregated parallel make invocations into batched
    // orchestrators. Measured on the real 4-node grid that AGGREGATION HURTS: the
    // per-batch run_and_join() barrier plus centralising preprocessing in the daemon
    // pool made cold -j20 builds ~40% slower than the standalone per-file path
    // (10.9s vs 6.4s — the latter matches Icecream 6.5s). Standalone preprocesses
    // client-side with make's full parallelism and returns each file independently.
    // The daemon's original win was per-process startup amortisation at low -j, but
    // after the 250 ms monitor-join stall fix the standalone path is only ~2% slower
    // at -j1 (11.1s vs 10.9s) — a negligible cost for a 40% parallel gain. So the
    // default is flipped to standalone; SUCO_DAEMON=1 still opts back into the daemon.
    bool daemon_enabled = false;
    const char* daemon_env = std::getenv("SUCO_DAEMON");
    if (daemon_env) {
        std::string val(daemon_env);
        if (val == "1" || val == "true") {
            daemon_enabled = true;
        }
    }
    // SUCO_NO_DAEMON=1 still explicitly forces standalone (overrides SUCO_DAEMON).
    const char* no_daemon_env = std::getenv("SUCO_NO_DAEMON");
    if (no_daemon_env) {
        std::string val(no_daemon_env);
        if (val == "1" || val == "true") {
            daemon_enabled = false;
        }
    }

    if (daemon_enabled) {
        std::string sock_path = get_daemon_socket_path();
        int rc = run_via_daemon(commands, sock_path);
        if (rc >= 0) {
            return rc;
        }
        SUCO_LOG_WARNING("Failed to run via daemon. Falling back to standalone compilation.");
    }

    // 2. Standalone Fallback path
    // Build context
    RequestContext context;
    context.cwd = std::filesystem::current_path().string();
    context.env_overrides = get_whitelisted_env();
    
    const char* path_norm_env = std::getenv("SUCO_PATH_NORMALIZATION");
    if (path_norm_env) {
        context.path_normalization = (std::string(path_norm_env) == "1" || std::string(path_norm_env) == "true");
    } else {
        context.path_normalization = config_.path_normalization;
    }

    PipelineOrchestrator orchestrator(config_, commands.size(), context);
    
    for (const auto& cmd : commands) {
        orchestrator.enqueue_job(cmd);
    }
    
    return orchestrator.run_and_join();
}

} // namespace suco
