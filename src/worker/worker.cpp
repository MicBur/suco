#include "worker.h"
#include "tls_util.h"
#include "metrics.h"
#include "job_executor.h"
#include "protocol.h"
#include "logging.h"
#include "zstd_util.h"
#include "hash_util.h"
#include <cstring>
#include "toolchain_detector.h"
#include "toolchain_manager.h"
#include "header_cache.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include <thread>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace suco::worker {

static Worker* g_active_worker = nullptr;

// --- Generic distributed task execution (PACKET_RUN_REQ) ---
// Runs an arbitrary command in a fresh temp dir populated with the client's declared
// input files, then returns the declared output files. This is the `suco run` path
// that generalises SUCO from a compiler grid to a generic build-step grid (the core
// of what IncrediBuild does beyond compilation). SECURITY: this executes arbitrary
// commands — the direct listener must be authenticated/network-restricted before use
// on an untrusted LAN (see docs/security_auth.md; auth on the direct port is TODO).
// --- C3: sandboxing for `suco run` (the arbitrary-command surface) ---
// Auth (SUCO_SECRET) controls WHO may submit a task; this controls WHAT it can do.
// Opt-in via SUCO_SANDBOX=1 (a sandboxed task has no network, so it is off by
// default to avoid breaking tasks that legitimately need one).
//
// Uses unshare(1) — present on every stock Linux, no extra package (bubblewrap
// would be nicer but isn't installed on the grid). Inside an unprivileged
// user+mount+pid namespace we are mapped-root, so we can:
//   * remount every mountpoint read-only  -> the task cannot touch the worker's
//     filesystem (home, keys, /usr, the SUCO binaries themselves)
//   * re-bind ONLY the job's workdir read-write -> inputs/outputs still work
//   * --pid   -> cannot see or signal the worker's or other jobs' processes
//   * --net   -> no network: a hostile task cannot exfiltrate or call home
//     (SUCO_SANDBOX_NET=1 keeps networking if a task genuinely needs it)
// Fails CLOSED: if the namespace can't be set up, the command doesn't run.
static bool sandbox_enabled() {
    const char* e = std::getenv("SUCO_SANDBOX");
    return e && (std::strcmp(e, "1") == 0 || std::strcmp(e, "true") == 0);
}

// Verify at STARTUP that the sandbox can actually be built, instead of letting
// every task fail mysteriously later (the sandbox fails closed by design). Some
// distros — Ubuntu >= 24.04 — block unprivileged user namespaces via AppArmor
// (kernel.apparmor_restrict_unprivileged_userns=1), which makes `unshare --user`
// fail with "write failed /proc/self/uid_map: Operation not permitted".
bool Worker::sandbox_selftest_ok() {
    if (!sandbox_enabled()) return true;
    int rc = std::system("unshare --user --map-root-user /bin/true >/dev/null 2>&1");
    if (rc == 0) {
        SUCO_LOG_INFO("Sandbox: ENABLED (unprivileged user namespaces available) — "
                      "`suco run` tasks get a read-only filesystem, private /proc, own PID ns"
                      "%s.", std::getenv("SUCO_SANDBOX_NET") ? "" : " and no network");
        return true;
    }
    SUCO_LOG_ERROR("Sandbox: SUCO_SANDBOX=1 but unprivileged user namespaces are unavailable "
                   "(unshare --user fails). Every task would fail closed, so refusing to start. "
                   "Fix: `sudo apt install bubblewrap` (ships an AppArmor profile) or relax "
                   "`sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`; "
                   "or unset SUCO_SANDBOX to run without isolation.");
    return false;
}

static std::string shell_quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
    r += "'";
    return r;
}

// Strip the worker's own secrets from the environment a task inherits. Without
// this ANY `suco run` task could simply read SUCO_SECRET out of its own env (it is
// inherited from the worker's systemd unit) and impersonate the grid — which would
// defeat the authentication entirely. Applies sandboxed or not.
static const char* kScrubEnv = "unset SUCO_SECRET SUCO_TLS SSHPASS; ";

static std::string build_run_command(const std::string& workdir, const std::string& cmd) {
    const std::string plain = "cd " + workdir + " && ( " + kScrubEnv + cmd + " ) 2>&1";
    if (!sandbox_enabled()) return plain;

    const char* net = std::getenv("SUCO_SANDBOX_NET");
    const bool allow_net = net && (std::strcmp(net, "1") == 0 || std::strcmp(net, "true") == 0);

    // Runs as mapped-root INSIDE the user namespace only — no privilege on the host.
    // `set -e` on the namespace setup makes it fail closed rather than silently
    // running the task unconfined.
    std::string inner =
        "mount --make-rprivate / 2>/dev/null || true; "
        // read-only everything (deepest mounts first so parents don't mask children)
        "for m in $(awk '{print $2}' /proc/self/mounts | sort -r); do "
        "mount -o remount,ro,bind \"$m\" \"$m\" 2>/dev/null || true; done; "
        // Fresh procfs for THIS pid namespace. Without it /proc is still the host's,
        // so a task could read /proc/<pid>/environ of other processes — including the
        // worker's own SUCO_SECRET, which would defeat the auth entirely.
        "mount -t proc none /proc 2>/dev/null || true; "
        // ...then punch the job's workdir back out as read-write
        "mount --bind " + workdir + " " + workdir + " && "
        "mount -o remount,rw,bind " + workdir + " " + workdir + " && "
        "cd " + workdir + " && ( " + kScrubEnv + cmd + " ) 2>&1";

    std::string un = "unshare --user --map-root-user --mount --pid --fork";
    if (!allow_net) un += " --net";
    return un + " /bin/sh -c " + shell_quote(inner);
}

static void handle_run_request(socket_t client_sock) {
    auto read_str = [&](std::string& out, uint32_t cap) -> bool {
        uint32_t len_net = 0;
        if (!read_all(client_sock, &len_net, 4)) return false;
        uint32_t len = ntohl(len_net);
        if (len > cap) return false;
        out.resize(len);
        return len == 0 || read_all(client_sock, out.data(), len);
    };
    auto send_str = [&](const std::string& s) -> bool {
        uint32_t l = htonl(static_cast<uint32_t>(s.size()));
        return send_all(client_sock, &l, 4) && (s.empty() || send_all(client_sock, s.data(), s.size()));
    };

    std::string cmd;
    if (!read_str(cmd, 1u << 20)) return;                       // command, max 1 MB
    uint32_t num_in_net = 0;
    if (!read_all(client_sock, &num_in_net, 4)) return;
    uint32_t num_in = ntohl(num_in_net);
    if (num_in > 100000) return;

#ifdef _WIN32
    std::error_code temp_ec;
    std::filesystem::path workdir = std::filesystem::temp_directory_path(temp_ec) / ("suco_run_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(workdir, temp_ec);
    if (temp_ec) return;
#else
    char tmpl[] = "/tmp/suco_run_XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) return;
    std::filesystem::path workdir(dir);
#endif
    std::error_code ec;

    for (uint32_t i = 0; i < num_in; ++i) {
        std::string rel, content;
        if (!read_str(rel, 4096) || !read_str(content, 512u << 20)) {  // input file <=512 MB
            std::filesystem::remove_all(workdir, ec); return;
        }
        std::filesystem::path full = workdir / rel;
        std::filesystem::create_directories(full.parent_path(), ec);
        std::ofstream f(full, std::ios::binary);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    uint32_t num_out_net = 0;
    if (!read_all(client_sock, &num_out_net, 4)) { std::filesystem::remove_all(workdir, ec); return; }
    uint32_t num_out = ntohl(num_out_net);
    if (num_out > 100000) { std::filesystem::remove_all(workdir, ec); return; }
    std::vector<std::string> out_paths;
    for (uint32_t i = 0; i < num_out; ++i) {
        std::string p;
        if (!read_str(p, 4096)) { std::filesystem::remove_all(workdir, ec); return; }
        out_paths.push_back(p);
    }

    SUCO_LOG_INFO("Worker: running distributed task in {} ({} inputs, {} outputs){}",
                  workdir.string(), num_in, num_out, sandbox_enabled() ? " [sandboxed]" : "");
    std::string full_cmd = build_run_command(workdir.string(), cmd);
    std::string log;
    int exit_code = -1;
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (pipe) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) log.append(buf, n);
        int st = pclose(pipe);
#ifdef _WIN32
        exit_code = (st >= 0) ? st : -1;
#else
        exit_code = (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
#endif
    }

    uint32_t rt = htonl(suco::PACKET_RUN_RESP);
    int32_t ec_net = htonl(exit_code);
    uint32_t no = htonl(static_cast<uint32_t>(out_paths.size()));
    bool ok = send_all(client_sock, &rt, 4) && send_all(client_sock, &ec_net, 4) && send_str(log) &&
              send_all(client_sock, &no, 4);
    for (size_t i = 0; ok && i < out_paths.size(); ++i) {
        std::filesystem::path full = workdir / out_paths[i];
        std::ifstream f(full, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        ok = send_str(out_paths[i]) && send_str(content);
    }
    std::filesystem::remove_all(workdir, ec);
}

static void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        // ASYNC-SIGNAL-SAFE ONLY. Previously this logged (mutex + allocation) and
        // called initiate_shutdown(), which stops the heartbeat manager and thus
        // JOINS a thread — both are undefined behaviour in a signal handler and
        // deadlocked the process: SIGTERM never completed, so every
        // `systemctl stop/restart suco-worker` hung until systemd's 90s timeout
        // SIGKILLed it (and left zombie workers behind).
        // Only set the flag and close the sockets (close() is async-signal-safe);
        // the loops then unwind and clean up in normal context.
        if (g_active_worker) {
            g_active_worker->signal_unblock();
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
    if (m_direct_listener_sock != -1) {
        close_socket(m_direct_listener_sock);
    }
    if (m_direct_listener_thread.joinable()) {
        m_direct_listener_thread.join();
    }
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

    // Refuse to start with a broken sandbox rather than fail every task later.
    if (!sandbox_selftest_ok()) return 1;

    // Initialize the PCH Header Cache (Phase B4)
    HeaderCache::get_instance().initialize(m_config.header_cache_dir, m_config.header_cache_max_size_gb);

    // 1b. Toolchain-Erkennung einmalig beim Start
    SUCO_LOG_INFO("Detecting available compilers and tools...");
    ToolchainInfo tc = ToolchainDetector::detect();
    m_toolchains_json = ToolchainDetector::to_json(tc);
    
    std::string tc_summary = "";
    for (const auto& [name, ver] : tc.compilers) {
        if (!tc_summary.empty()) tc_summary += ", ";
        tc_summary += name + "=" + ver;
    }
    for (const auto& [name, ver] : tc.tools) {
        if (!tc_summary.empty()) tc_summary += ", ";
        tc_summary += name + "=" + ver;
    }
    for (const auto& [name, ver] : tc.qt_versions) {
        if (!tc_summary.empty()) tc_summary += ", ";
        tc_summary += name + "=" + ver;
    }
    if (tc_summary.empty()) {
        tc_summary = "None detected";
    }
    SUCO_LOG_INFO("Detected toolchains: {}", tc_summary);

    // 2. Bestimme Slotanzahl
    if (m_config.slots > 0) {
        m_slots_total = m_config.slots;
    } else {
        m_slots_total = std::max(1, Metrics::get_logical_cores());
    }

    // 2b. Start direct compilation listener
    m_direct_listener_sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (m_direct_listener_sock == INVALID_SOCKET_VAL) {
        SUCO_LOG_ERROR("Failed to create direct listener socket.");
        return 1;
    }
    int optval = 1;
    setsockopt(m_direct_listener_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&optval), sizeof(optval));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_config.direct_port);

    if (::bind(m_direct_listener_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        SUCO_LOG_WARNING("Direct Port {} busy. Trying dynamic port...", m_config.direct_port);
        addr.sin_port = htons(0);
        if (::bind(m_direct_listener_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            SUCO_LOG_ERROR("Failed to bind direct listener socket dynamically.");
            close_socket(m_direct_listener_sock);
            return 1;
        }
    }

    if (::listen(m_direct_listener_sock, SOMAXCONN) < 0) {
        SUCO_LOG_ERROR("Failed to listen on direct socket.");
        close_socket(m_direct_listener_sock);
        return 1;
    }

    struct sockaddr_in bound_addr;
    socklen_t bound_addr_len = sizeof(bound_addr);
    if (::getsockname(m_direct_listener_sock, reinterpret_cast<struct sockaddr*>(&bound_addr), &bound_addr_len) == 0) {
        m_direct_port = ntohs(bound_addr.sin_port);
    } else {
        m_direct_port = m_config.direct_port;
    }

    SUCO_LOG_INFO("Worker direct compilation listener started on port {}", m_direct_port);
    m_direct_listener_thread = std::thread(&Worker::run_direct_listener_loop, this);

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
        m_active_coordinator_host = host;
        m_active_coordinator_port = port;

        if (m_shutdown_requested) {
            m_net_client.disconnect();
            break;
        }

        // 4. Registrierung
        std::string name = Metrics::get_host_name();
        std::string os = Metrics::get_os_name();

        if (!m_net_client.register_worker(name, os, m_slots_total, m_toolchains_json, m_direct_port)) {
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
    if (m_direct_listener_sock != -1) {
        // shutdown() first — close() alone leaves a blocked accept() parked (see
        // signal_unblock), which then deadlocks the destructor's join().
        ::shutdown(m_direct_listener_sock, SHUT_RDWR);
        close_socket(m_direct_listener_sock);
        m_direct_listener_sock = -1;
    }
    if (m_heartbeat_mgr) {
        m_heartbeat_mgr->stop();   // joins a thread — never call this from a signal handler
    }
}

void Worker::signal_unblock() noexcept {
    // Async-signal-safe: an atomic store plus close(), nothing else. Closing the
    // sockets makes the blocking accept()/recv() in the listener and compile loops
    // return, so they observe m_shutdown_requested and unwind; the destructor then
    // performs the real cleanup (heartbeat stop + joins) in normal context.
    m_shutdown_requested = true;
    int lsock = m_direct_listener_sock;
    if (lsock != -1) {
        m_direct_listener_sock = -1;
        // shutdown() BEFORE close(): on Linux close() alone does NOT wake a thread
        // already blocked in accept() — it stayed parked in inet_csk_accept, so the
        // destructor's join() waited forever and the process only died via systemd's
        // 90s SIGKILL. shutdown() makes the pending accept() return immediately.
        ::shutdown(lsock, SHUT_RDWR);
        close_socket(lsock);
    }
    socket_t csock = m_net_client.get_socket();
    if (csock != INVALID_SOCKET_VAL) {
        ::shutdown(csock, SHUT_RDWR);   // unblock a pending recv() without freeing the fd
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
        
        if (req_type == suco::PACKET_CACHE_CLEAR) {
            SUCO_LOG_INFO("Received cache clear request from coordinator. Clearing header cache...");
            HeaderCache::get_instance().clear();
            continue;
        }
        
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
        
        uint8_t src_comp = 0;
        if (!m_net_client.receive_packet(&src_comp, 1)) break;

        uint32_t src_len_net = 0;
        if (!m_net_client.receive_packet(&src_len_net, 4)) break;
        uint32_t src_len = ntohl(src_len_net);
        std::vector<char> src_buf(src_len);
        if (src_len > 0) {
            if (!m_net_client.receive_packet(src_buf.data(), src_len)) break;
        }
        std::string source(src_buf.data(), src_len);

        if (src_comp == 1) {
            source = suco::decompress_zstd(source);
            if (source.empty() && src_len > 0) {
                SUCO_LOG_ERROR("Worker: Failed to decompress source payload.");
                uint32_t resp_type_net = htonl(suco::PACKET_COMPILE_RESP);
                uint32_t f_len_net = htonl(static_cast<uint32_t>(filename.size()));
                int32_t exit_code_net = htonl(-99);
                std::string err_log = "suco-worker error: Failed to decompress source payload (Limit/Corruption)";
                uint32_t log_len_net = htonl(static_cast<uint32_t>(err_log.size()));
                uint8_t bin_comp_err = 0;
                uint32_t bin_len_net_err = 0;
                uint32_t hc_hit_net_err = 0;

                std::lock_guard<std::mutex> lock(m_jobs_mutex);
                if (m_net_client.send_packet(&resp_type_net, 4) &&
                    m_net_client.send_packet(&f_len_net, 4) &&
                    m_net_client.send_packet(filename.c_str(), filename.size()) &&
                    m_net_client.send_packet(&exit_code_net, 4) &&
                    m_net_client.send_packet(&log_len_net, 4) &&
                    m_net_client.send_packet(err_log.c_str(), err_log.size()) &&
                    m_net_client.send_packet(&bin_comp_err, 1) &&
                    m_net_client.send_packet(&bin_len_net_err, 4) &&
                    m_net_client.send_packet(&hc_hit_net_err, 4)) {
                }
                m_slots_used = std::max(0, m_slots_used.load() - 1);
                continue;
            }
        }

        uint32_t tc_hash_len_net = 0;
        if (!m_net_client.receive_packet(&tc_hash_len_net, 4)) break;
        uint32_t tc_hash_len = ntohl(tc_hash_len_net);
        std::vector<char> tc_hash_buf(tc_hash_len);
        if (tc_hash_len > 0) {
            if (!m_net_client.receive_packet(tc_hash_buf.data(), tc_hash_len)) break;
        }
        std::string toolchain_hash(tc_hash_buf.data(), tc_hash_len);

        uint32_t hs_hash_len_net = 0;
        if (!m_net_client.receive_packet(&hs_hash_len_net, 4)) break;
        uint32_t hs_hash_len = ntohl(hs_hash_len_net);
        std::vector<char> hs_hash_buf(hs_hash_len);
        if (hs_hash_len > 0) {
            if (!m_net_client.receive_packet(hs_hash_buf.data(), hs_hash_len)) break;
        }
        std::string header_set_hash(hs_hash_buf.data(), hs_hash_len);

        uint8_t hs_comp = 0;
        if (!m_net_client.receive_packet(&hs_comp, 1)) break;

        uint32_t hs_src_len_net = 0;
        if (!m_net_client.receive_packet(&hs_src_len_net, 4)) break;
        uint32_t hs_src_len = ntohl(hs_src_len_net);
        std::vector<char> hs_src_buf(hs_src_len);
        if (hs_src_len > 0) {
            if (!m_net_client.receive_packet(hs_src_buf.data(), hs_src_len)) break;
        }
        std::string header_set_source(hs_src_buf.data(), hs_src_len);

        if (hs_comp == 1) {
            header_set_source = suco::decompress_zstd(header_set_source);
            if (header_set_source.empty() && hs_src_len > 0) {
                SUCO_LOG_ERROR("Worker: Failed to decompress header set payload.");
                uint32_t resp_type_net = htonl(suco::PACKET_COMPILE_RESP);
                uint32_t f_len_net = htonl(static_cast<uint32_t>(filename.size()));
                int32_t exit_code_net = htonl(-99);
                std::string err_log = "suco-worker error: Failed to decompress header set payload (Limit/Corruption)";
                uint32_t log_len_net = htonl(static_cast<uint32_t>(err_log.size()));
                uint8_t bin_comp_err = 0;
                uint32_t bin_len_net_err = 0;
                uint32_t hc_hit_net_err = 0;

                std::lock_guard<std::mutex> lock(m_jobs_mutex);
                if (m_net_client.send_packet(&resp_type_net, 4) &&
                    m_net_client.send_packet(&f_len_net, 4) &&
                    m_net_client.send_packet(filename.c_str(), filename.size()) &&
                    m_net_client.send_packet(&exit_code_net, 4) &&
                    m_net_client.send_packet(&log_len_net, 4) &&
                    m_net_client.send_packet(err_log.c_str(), err_log.size()) &&
                    m_net_client.send_packet(&bin_comp_err, 1) &&
                    m_net_client.send_packet(&bin_len_net_err, 4) &&
                    m_net_client.send_packet(&hc_hit_net_err, 4)) {
                }
                m_slots_used = std::max(0, m_slots_used.load() - 1);
                continue;
            }
        }

        if (!toolchain_hash.empty()) {
            static std::mutex toolchain_download_mutex;
            std::lock_guard<std::mutex> tc_lock(toolchain_download_mutex);
            if (!ToolchainManager::has_toolchain(toolchain_hash)) {
                SUCO_LOG_INFO("Toolchain {} not found on worker. Downloading via dedicated connection...", toolchain_hash);
                
                socket_t download_sock = ::socket(AF_INET, SOCK_STREAM, 0);
                if (download_sock != INVALID_SOCKET_VAL) {
                    struct sockaddr_in address;
                    std::memset(&address, 0, sizeof(address));
                    address.sin_family = AF_INET;
                    address.sin_port = htons(m_active_coordinator_port);
                    
                    bool conn_ok = false;
                    if (inet_pton(AF_INET, m_active_coordinator_host.c_str(), &address.sin_addr) > 0) {
                        if (::connect(download_sock, (struct sockaddr*)&address, sizeof(address)) >= 0) {
                            conn_ok = true;
                        }
                    }
                    
                    if (conn_ok) {
                        uint32_t req_net = htonl(suco::PACKET_TOOLCHAIN_DOWNLOAD);
                        uint32_t hash_len_net = htonl(toolchain_hash.size());
                        
                        if (send_all(download_sock, &req_net, 4) &&
                            send_all(download_sock, &hash_len_net, 4) &&
                            send_all(download_sock, toolchain_hash.c_str(), toolchain_hash.size())) {
                            
                            std::string cache_dir = ToolchainManager::get_toolchain_path(toolchain_hash);
                            std::error_code ec;
                            std::filesystem::create_directories(std::filesystem::path(cache_dir).parent_path(), ec);
                            
                            std::string temp_archive = "/tmp/suco_tc_recv_" + toolchain_hash + ".tar.zst";
                            if (suco::receive_file(download_sock, temp_archive)) {
                                SUCO_LOG_INFO("Worker successfully received toolchain archive: {}", temp_archive);
                                ToolchainManager::extract_toolchain(toolchain_hash, temp_archive);
                                std::filesystem::remove(temp_archive, ec);
                            } else {
                                SUCO_LOG_ERROR("Failed to receive toolchain archive from coordinator");
                            }
                        }
                    } else {
                        SUCO_LOG_ERROR("Failed to connect to coordinator for toolchain download");
                    }
                    close_socket(download_sock);
                } else {
                    SUCO_LOG_ERROR("Failed to create socket for toolchain download");
                }
            }
        }
        
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

        std::thread([this, command, filename, source, toolchain_hash, header_set_hash, header_set_source]() {
            this->handle_compile_job(command, filename, source, toolchain_hash, header_set_hash, header_set_source);
            
            std::lock_guard<std::mutex> lock(m_jobs_mutex);
            m_active_jobs_count--;
            if (m_active_jobs_count == 0) {
                m_jobs_cv.notify_all();
            }
        }).detach();
    }
}

void Worker::handle_compile_job(const std::string& command, 
                                 const std::string& filename, 
                                 const std::string& source, 
                                 const std::string& toolchain_hash,
                                 const std::string& header_set_hash,
                                 const std::string& header_set_source,
                                 int client_sock,
                                 const std::vector<std::pair<std::string, std::string>>& module_cmis) {
    // Timeout an den Executor übergeben
    auto job_result = JobExecutor::execute(command, filename, source, m_config.job_timeout, toolchain_hash, header_set_hash, header_set_source, module_cmis);
    
    // Slots-Auslastung sicher reduzieren
    m_slots_used = std::max(0, m_slots_used.load() - 1);
    
    if (m_shutdown_requested) {
        SUCO_LOG_WARNING("Job {} finished during shutdown. Result will be dropped.", filename);
        if (client_sock != -1) {
            close_socket(client_sock);
        }
        return;
    }

    bool comp_enabled = true;
    const char* comp_env = std::getenv("SUCO_COMPRESSION");
    if (comp_env) {
        std::string s_comp(comp_env);
        comp_enabled = (s_comp != "off" && s_comp != "OFF" && s_comp != "false" && s_comp != "0");
    }

    uint8_t bin_comp = 0;
    uint32_t bin_len = job_result.binary.size();
    const void* bin_data = job_result.binary.data();
    std::string compressed_bin;

    if (comp_enabled && job_result.binary.size() >= 4096) {
        std::string bin_str(reinterpret_cast<const char*>(job_result.binary.data()), job_result.binary.size());
        compressed_bin = suco::compress_zstd(bin_str, 1);
        if (!compressed_bin.empty() && compressed_bin.size() < job_result.binary.size()) {
            bin_comp = 1;
            bin_len = compressed_bin.size();
            bin_data = compressed_bin.data();
        }
    }

    uint32_t resp_type_net = htonl(suco::PACKET_COMPILE_RESP);
    uint32_t f_len_net = htonl(static_cast<uint32_t>(filename.size()));
    int32_t exit_code_net = htonl(job_result.exit_code);
    uint32_t log_len_net = htonl(static_cast<uint32_t>(job_result.log.size()));
    uint32_t bin_len_net = htonl(bin_len);
    uint32_t hc_hit_net = htonl(job_result.header_cache_hit ? 1 : 0);
 
    if (client_sock != -1) {
        if (send_all(client_sock, &resp_type_net, 4) &&
            send_all(client_sock, &f_len_net, 4) &&
            send_all(client_sock, filename.c_str(), filename.size()) &&
            send_all(client_sock, &exit_code_net, 4) &&
            send_all(client_sock, &log_len_net, 4)) {
            
            if (!job_result.log.empty()) {
                send_all(client_sock, job_result.log.c_str(), job_result.log.size());
            }
            send_all(client_sock, &bin_comp, 1);
            send_all(client_sock, &bin_len_net, 4);
            if (bin_len > 0) {
                send_all(client_sock, bin_data, bin_len);
            }
            send_all(client_sock, &hc_hit_net, 4);
        }
        close_socket(client_sock);
    } else {
        std::lock_guard<std::mutex> lock(m_jobs_mutex);
        if (m_net_client.send_packet(&resp_type_net, 4) &&
            m_net_client.send_packet(&f_len_net, 4) &&
            m_net_client.send_packet(filename.c_str(), filename.size()) &&
            m_net_client.send_packet(&exit_code_net, 4) &&
            m_net_client.send_packet(&log_len_net, 4)) {
            
            if (!job_result.log.empty()) {
                m_net_client.send_packet(job_result.log.c_str(), job_result.log.size());
            }
            m_net_client.send_packet(&bin_comp, 1);
            m_net_client.send_packet(&bin_len_net, 4);
            if (bin_len > 0) {
                m_net_client.send_packet(bin_data, bin_len);
            }
            m_net_client.send_packet(&hc_hit_net, 4);
        }
    }
    
    SUCO_LOG_INFO("Finished job {} (Exit: {})", filename, job_result.exit_code);
    if (job_result.exit_code != 0 && !job_result.log.empty()) {
        // The client discards the log of a failed remote job before falling back to a
        // local compile, so without this the worker-side reason is invisible.
        SUCO_LOG_WARNING("Job {} failed on this worker:\n{}", filename, job_result.log);
    }
}

void Worker::run_direct_listener_loop() {
    SUCO_LOG_INFO("Worker: Direct compile listener loop started.");
    while (!m_shutdown_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        socket_t client_sock = ::accept(m_direct_listener_sock, reinterpret_cast<struct sockaddr*>(&client_addr), &client_addr_len);
        
        if (client_sock == INVALID_SOCKET_VAL) {
            if (m_shutdown_requested) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Start thread to handle compile request
        std::thread([this, client_sock]() {
            // Optional TLS handshake before any bytes are read from the client.
            if (!suco::tls::wrap_accept(client_sock)) {
                close_socket(client_sock);
                return;
            }
            // Authenticate the direct connection (compile AND run) when a shared secret
            // is configured. The direct listener otherwise accepts arbitrary requests
            // from anyone who knows ip:port — for PACKET_RUN_REQ that would be remote
            // code execution. HMAC challenge-response, same scheme as the coordinator.
            {
                std::string secret = suco::get_shared_secret();
                if (!secret.empty()) {
                    std::string nonce = suco::generate_nonce();
                    uint32_t nlen = htonl(static_cast<uint32_t>(nonce.size()));
                    if (nonce.empty() || !send_all(client_sock, &nlen, 4) ||
                        !send_all(client_sock, nonce.data(), nonce.size())) { close_socket(client_sock); return; }
                    uint32_t mlen_net = 0;
                    if (!read_all(client_sock, &mlen_net, 4)) { close_socket(client_sock); return; }
                    uint32_t mlen = ntohl(mlen_net);
                    if (mlen == 0 || mlen > 256) { close_socket(client_sock); return; }
                    std::string mac(mlen, '\0');
                    if (!read_all(client_sock, mac.data(), mlen)) { close_socket(client_sock); return; }
                    if (!suco::constant_time_equals(mac, suco::hmac_sha256_hex(secret, nonce))) {
                        SUCO_LOG_ERROR("Worker: direct-connection AUTH FAILED — rejecting request");
                        close_socket(client_sock); return;
                    }
                }
            }
            uint32_t req_type_net = 0;
            if (!read_all(client_sock, &req_type_net, 4)) {
                close_socket(client_sock);
                return;
            }
            uint32_t req_type = ntohl(req_type_net);
            if (req_type == suco::PACKET_RUN_REQ) {
                handle_run_request(client_sock);   // generic distributed task
                close_socket(client_sock);
                return;
            }
            const bool expect_cmis = (req_type == suco::PACKET_DIRECT_COMPILE_REQ_V2);
            if (req_type != suco::PACKET_DIRECT_COMPILE_REQ && !expect_cmis) {
                SUCO_LOG_ERROR("Worker Direct Listener: Unexpected packet type {}", req_type);
                close_socket(client_sock);
                return;
            }

            uint32_t cmd_len_net = 0;
            if (!read_all(client_sock, &cmd_len_net, 4)) { close_socket(client_sock); return; }
            uint32_t cmd_len = ntohl(cmd_len_net);
            std::vector<char> cmd_buf(cmd_len);
            if (cmd_len > 0) {
                if (!read_all(client_sock, cmd_buf.data(), cmd_len)) { close_socket(client_sock); return; }
            }
            std::string command(cmd_buf.data(), cmd_len);

            uint32_t file_len_net = 0;
            if (!read_all(client_sock, &file_len_net, 4)) { close_socket(client_sock); return; }
            uint32_t file_len = ntohl(file_len_net);
            std::vector<char> file_buf(file_len);
            if (file_len > 0) {
                if (!read_all(client_sock, file_buf.data(), file_len)) { close_socket(client_sock); return; }
            }
            std::string filename(file_buf.data(), file_len);

            uint8_t src_comp = 0;
            if (!read_all(client_sock, &src_comp, 1)) { close_socket(client_sock); return; }

            uint32_t src_len_net = 0;
            if (!read_all(client_sock, &src_len_net, 4)) { close_socket(client_sock); return; }
            uint32_t src_len = ntohl(src_len_net);
            std::vector<char> src_buf(src_len);
            if (src_len > 0) {
                if (!read_all(client_sock, src_buf.data(), src_len)) { close_socket(client_sock); return; }
            }
            std::string source(src_buf.data(), src_len);

            if (src_comp == 1) {
                source = suco::decompress_zstd(source);
            }

            uint32_t tc_hash_len_net = 0;
            if (!read_all(client_sock, &tc_hash_len_net, 4)) { close_socket(client_sock); return; }
            uint32_t tc_hash_len = ntohl(tc_hash_len_net);
            std::vector<char> tc_hash_buf(tc_hash_len);
            if (tc_hash_len > 0) {
                if (!read_all(client_sock, tc_hash_buf.data(), tc_hash_len)) { close_socket(client_sock); return; }
            }
            std::string toolchain_hash(tc_hash_buf.data(), tc_hash_len);

            uint32_t hs_hash_len_net = 0;
            if (!read_all(client_sock, &hs_hash_len_net, 4)) { close_socket(client_sock); return; }
            uint32_t hs_hash_len = ntohl(hs_hash_len_net);
            std::vector<char> hs_hash_buf(hs_hash_len);
            if (hs_hash_len > 0) {
                if (!read_all(client_sock, hs_hash_buf.data(), hs_hash_len)) { close_socket(client_sock); return; }
            }
            std::string header_set_hash(hs_hash_buf.data(), hs_hash_len);

            uint8_t hs_comp = 0;
            if (!read_all(client_sock, &hs_comp, 1)) { close_socket(client_sock); return; }

            uint32_t hs_src_len_net = 0;
            if (!read_all(client_sock, &hs_src_len_net, 4)) { close_socket(client_sock); return; }
            uint32_t hs_src_len = ntohl(hs_src_len_net);
            std::vector<char> hs_src_buf(hs_src_len);
            if (hs_src_len > 0) {
                if (!read_all(client_sock, hs_src_buf.data(), hs_src_len)) { close_socket(client_sock); return; }
            }
            std::string header_set_source(hs_src_buf.data(), hs_src_len);

            if (hs_comp == 1) {
                header_set_source = suco::decompress_zstd(header_set_source);
            }

            // E3: C++20 module CMIs, present only on the V2 packet.
            std::vector<std::pair<std::string, std::string>> module_cmis;
            if (expect_cmis) {
                uint32_t cmi_count_net = 0;
                if (!read_all(client_sock, &cmi_count_net, 4)) { close_socket(client_sock); return; }
                uint32_t cmi_count = ntohl(cmi_count_net);
                if (cmi_count > 4096) {
                    SUCO_LOG_ERROR("Worker Direct Listener: implausible CMI count {}", cmi_count);
                    close_socket(client_sock);
                    return;
                }
                for (uint32_t i = 0; i < cmi_count; ++i) {
                    uint32_t name_len_net = 0;
                    if (!read_all(client_sock, &name_len_net, 4)) { close_socket(client_sock); return; }
                    uint32_t name_len = ntohl(name_len_net);
                    std::vector<char> name_buf(name_len);
                    if (name_len > 0 && !read_all(client_sock, name_buf.data(), name_len)) { close_socket(client_sock); return; }
                    std::string mod_name(name_buf.data(), name_len);

                    uint8_t cmi_comp = 0;
                    if (!read_all(client_sock, &cmi_comp, 1)) { close_socket(client_sock); return; }

                    uint32_t cmi_len_net = 0;
                    if (!read_all(client_sock, &cmi_len_net, 4)) { close_socket(client_sock); return; }
                    uint32_t cmi_len = ntohl(cmi_len_net);
                    std::vector<char> cmi_buf(cmi_len);
                    if (cmi_len > 0 && !read_all(client_sock, cmi_buf.data(), cmi_len)) { close_socket(client_sock); return; }
                    std::string cmi_data(cmi_buf.data(), cmi_len);

                    if (cmi_comp == 1) {
                        cmi_data = suco::decompress_zstd(cmi_data);
                    }
                    module_cmis.emplace_back(std::move(mod_name), std::move(cmi_data));
                }
                SUCO_LOG_INFO("[Modules] Received {} CMI(s) for {}", module_cmis.size(), filename);
            }

            // Download toolchain if missing (same logic as compile loop)
            if (!toolchain_hash.empty()) {
                static std::mutex toolchain_download_mutex;
                std::lock_guard<std::mutex> tc_lock(toolchain_download_mutex);
                if (!ToolchainManager::has_toolchain(toolchain_hash)) {
                    SUCO_LOG_INFO("Toolchain {} not found on worker. Downloading via dedicated connection...", toolchain_hash);
                    
                    socket_t download_sock = ::socket(AF_INET, SOCK_STREAM, 0);
                    if (download_sock != INVALID_SOCKET_VAL) {
                        struct sockaddr_in address;
                        std::memset(&address, 0, sizeof(address));
                        address.sin_family = AF_INET;
                        address.sin_port = htons(m_active_coordinator_port);
                        
                        bool conn_ok = false;
                        if (inet_pton(AF_INET, m_active_coordinator_host.c_str(), &address.sin_addr) > 0) {
                            if (::connect(download_sock, (struct sockaddr*)&address, sizeof(address)) >= 0) {
                                conn_ok = true;
                            }
                        }
                        
                        if (conn_ok) {
                            uint32_t req_net = htonl(suco::PACKET_TOOLCHAIN_DOWNLOAD);
                            uint32_t hash_len_net = htonl(toolchain_hash.size());
                            
                            if (send_all(download_sock, &req_net, 4) &&
                                send_all(download_sock, &hash_len_net, 4) &&
                                send_all(download_sock, toolchain_hash.c_str(), toolchain_hash.size())) {
                                
                                std::string cache_dir = ToolchainManager::get_toolchain_path(toolchain_hash);
                                std::error_code ec;
                                std::filesystem::create_directories(cache_dir, ec);
                                std::string archive_path = cache_dir + "/toolchain-" + toolchain_hash + ".tar.zst";
                                
                                if (receive_file(download_sock, archive_path)) {
                                    SUCO_LOG_INFO("Successfully downloaded toolchain {} from coordinator", toolchain_hash);
                                }
                            }
                        }
                        close_socket(download_sock);
                    }
                }
            }

            SUCO_LOG_INFO("Compiling direct job {}...", filename);
            m_slots_used++;
            
            {
                std::lock_guard<std::mutex> lock(m_jobs_mutex);
                m_active_jobs_count++;
            }

            // Spawn execution thread to keep listener thread clean
            std::thread([this, command, filename, source, toolchain_hash, header_set_hash, header_set_source, client_sock, module_cmis]() {
                this->handle_compile_job(command, filename, source, toolchain_hash, header_set_hash, header_set_source, client_sock, module_cmis);
                
                std::lock_guard<std::mutex> lock(m_jobs_mutex);
                m_active_jobs_count--;
                if (m_active_jobs_count == 0) {
                    m_jobs_cv.notify_all();
                }
            }).detach();

        }).detach();
    }
}

} // namespace suco::worker
