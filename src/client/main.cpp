#include "socket_util.h"

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <algorithm>

#ifdef _WIN32
    #include <process.h>
#else
    #include <sys/wait.h>
    #include <unistd.h>
#endif

#include "protocol.h"
#include "hash_util.h"

// Platform-independent process spawn for fallback
int run_fallback_local(char** argv) {
#ifdef _WIN32
    // Windows: Spawn process and wait for it
    // argv[1] is the compiler, argv + 1 is the arguments list starting with the compiler
    intptr_t res = _spawnvp(_P_WAIT, argv[1], argv + 1);
    if (res < 0) {
        std::cerr << "suco error: Failed to execute local fallback compiler (MSVC/GCC) " << argv[1] << std::endl;
        return 1;
    }
    return static_cast<int>(res);
#else
    // Linux: fork & execvp
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[1], argv + 1);
        std::cerr << "suco error: Failed to execute local fallback compiler " << argv[1] << std::endl;
        exit(1);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return 1;
    }
    return 1;
#endif
}

// Run a command and capture its stdout output
std::string run_local_capture(const std::string& cmd, int& exit_code) {
    std::string result;
    char buffer[4096];
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) {
        exit_code = -1;
        return "";
    }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    int status = _pclose(pipe);
    exit_code = status;
#else
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        exit_code = -1;
        return "";
    }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    int status = pclose(pipe);
    exit_code = WEXITSTATUS(status);
#endif
    return result;
}

// Connect to socket with a strict 100ms timeout
socket_t connect_with_timeout(const std::string& host, uint16_t port, int timeout_ms) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_VAL) return INVALID_SOCKET_VAL;

    struct hostent* server = gethostbyname(host.c_str());
    if (!server) {
        close_socket(sock);
        return INVALID_SOCKET_VAL;
    }

    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    serv_addr.sin_port = htons(port);

    // Set non-blocking
    if (!suco::set_socket_nonblocking(sock, true)) {
        close_socket(sock);
        return INVALID_SOCKET_VAL;
    }

    int res = connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    if (res < 0) {
        int err = get_socket_error();
        if (is_would_block(err)) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);

            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = timeout_ms * 1000;

            res = select(static_cast<int>(sock + 1), nullptr, &write_fds, nullptr, &tv);
            if (res > 0) {
                int sock_err = 0;
                socklen_t len = sizeof(sock_err);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sock_err), &len) < 0 || sock_err != 0) {
                    res = -1;
                } else {
                    res = 0; // Connected!
                }
            } else {
                res = -1; // Timeout or error
            }
        } else {
            res = -1;
        }
    }

    // Restore blocking mode
    suco::set_socket_nonblocking(sock, false);

    if (res < 0) {
        close_socket(sock);
        return INVALID_SOCKET_VAL;
    }

    return sock;
}

// Normalize multi-line output (e.g. MSVC /Bv) into a single line for stable hashing
std::string normalize_version_output(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    bool last_was_space = false;
    for (char c : input) {
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!last_was_space && !result.empty()) {
                result += ' ';
                last_was_space = true;
            }
        } else {
            result += c;
            last_was_space = (c == ' ');
        }
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

void query_compiler_metadata(const std::string& compiler, bool is_msvc, std::string& version, std::string& arch) {
    static std::string cached_version;
    static std::string cached_arch;
    static std::once_flag flag;
    
    std::call_once(flag, [&]() {
        if (is_msvc) {
            // Run cl.exe /Bv to get the detailed toolchain version (includes frontends/backends/linker versions)
            std::string cmd = "\"" + compiler + "\" /Bv 2>&1";
            int exit_code = 0;
            std::string out = run_local_capture(cmd, exit_code);
            if (exit_code == 0 && !out.empty()) {
                // Normalize multi-line /Bv output into single stable line
                cached_version = normalize_version_output(out);
                // Parse target architecture from output
                if (out.find("ARM64") != std::string::npos || out.find("arm64") != std::string::npos) {
                    cached_arch = "arm64";
                } else if (out.find("x86") != std::string::npos || out.find("X86") != std::string::npos) {
                    cached_arch = "x86";
                } else {
                    cached_arch = "x64";
                }
            } else {
                cached_version = "MSVC Unknown";
                cached_arch = "x64";
            }
        } else {
            // GCC/Clang
            int exit_code = 0;
            std::string ver_out = run_local_capture("\"" + compiler + "\" -dumpfullversion 2>&1", exit_code);
            if (exit_code == 0 && !ver_out.empty()) {
                while (!ver_out.empty() && (ver_out.back() == '\n' || ver_out.back() == '\r')) {
                    ver_out.pop_back();
                }
                cached_version = ver_out;
            } else {
                cached_version = "GCC Unknown";
            }

            std::string mach_out = run_local_capture("\"" + compiler + "\" -dumpmachine 2>&1", exit_code);
            if (exit_code == 0 && !mach_out.empty()) {
                while (!mach_out.empty() && (mach_out.back() == '\n' || mach_out.back() == '\r')) {
                    mach_out.pop_back();
                }
                cached_arch = mach_out;
            } else {
                cached_arch = "x86_64";
            }
        }
    });

    version = cached_version;
    arch = cached_arch;
}

int main(int argc, char** argv) {
    suco::SocketInit sock_init; // Automatically starts Winsock under Windows

    if (argc < 2) {
        std::cerr << "Usage: suco <compiler> [flags] -c <source> -o <output>" << std::endl;
        std::cerr << "       suco --monitor  (Opens the compilation grid dashboard in your browser)" << std::endl;
        return 1;
    }

    // Parse CLI helpers
    if (argc == 2 && (std::string(argv[1]) == "--monitor" || std::string(argv[1]) == "--dashboard")) {
        std::string coordinator_host = "127.0.0.1";
        if (const char* env_host = std::getenv("SUCO_COORDINATOR_HOST")) {
            coordinator_host = env_host;
        }
        std::string url = "http://" + coordinator_host + ":" + std::to_string(suco::DEFAULT_WEB_PORT);
        std::cout << "Opening SUCO Grid Monitor Dashboard at " << url << " ..." << std::endl;

        std::string cmd;
#ifdef _WIN32
        cmd = "start " + url;
#else
        // Check for WSL
        std::ifstream version_file("/proc/version");
        std::string version_str;
        bool is_wsl = false;
        if (version_file.is_open() && std::getline(version_file, version_str)) {
            if (version_str.find("Microsoft") != std::string::npos || version_str.find("microsoft") != std::string::npos) {
                is_wsl = true;
            }
        }
        if (is_wsl) {
            cmd = "cmd.exe /C start " + url + " 2>/dev/null";
        } else {
            cmd = "xdg-open " + url + " 2>/dev/null || open " + url + " 2>/dev/null";
        }
#endif
        int r = std::system(cmd.c_str());
        (void)r;
        return 0;
    }

    std::string compiler = argv[1];
    std::string compiler_lower = compiler;
    for (auto& c : compiler_lower) c = std::tolower(c);

    bool is_msvc = (compiler_lower.rfind("cl") != std::string::npos || compiler_lower.rfind("cl.exe") != std::string::npos);

    bool has_c = false;
    std::string output_file;
    std::string source_file;
    std::vector<std::string> other_flags;

    // Parse arguments based on compiler type (MSVC cl.exe vs GCC g++)
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (is_msvc) {
            // MSVC flags
            if (arg == "/c" || arg == "-c") {
                has_c = true;
            } else if (arg.rfind("/Fo", 0) == 0) {
                output_file = arg.substr(3); // e.g. /Fofile.obj
                if (output_file.empty() && i + 1 < argc) {
                    output_file = argv[++i]; // e.g. /Fo file.obj
                }
            } else if (arg.rfind("-Fo", 0) == 0) {
                output_file = arg.substr(3);
                if (output_file.empty() && i + 1 < argc) {
                    output_file = argv[++i];
                }
            } else if (arg.rfind(".cpp") != std::string::npos || 
                       arg.rfind(".cc") != std::string::npos || 
                       arg.rfind(".cxx") != std::string::npos || 
                       arg.rfind(".c") != std::string::npos) {
                source_file = arg;
            } else {
                other_flags.push_back(arg);
            }
        } else {
            // GCC/Clang flags
            if (arg == "-c") {
                has_c = true;
            } else if (arg == "-o" && i + 1 < argc) {
                output_file = argv[++i];
            } else if (arg.rfind(".cpp") != std::string::npos || 
                       arg.rfind(".cc") != std::string::npos || 
                       arg.rfind(".cxx") != std::string::npos || 
                       arg.rfind(".c") != std::string::npos) {
                source_file = arg;
            } else {
                other_flags.push_back(arg);
            }
        }
    }

    // Fallback to local compile if not a standard compilation step
    if (!has_c || output_file.empty() || source_file.empty()) {
        return run_fallback_local(argv);
    }

    // PCH flags: Too complex for distributed cache in Phase 1 → fallback
    for (const auto& flag : other_flags) {
        if (flag.rfind("/Yu", 0) == 0 || flag.rfind("/Yc", 0) == 0 ||
            flag.rfind("/Fp", 0) == 0 || flag == "-include-pch" ||
            flag.rfind("-fpch-preprocess", 0) == 0) {
            return run_fallback_local(argv);
        }
    }

    // Preprocessing command
    std::stringstream pp_cmd;
    if (is_msvc) {
        pp_cmd << "\"" << compiler << "\" /E /nologo ";
        for (const auto& flag : other_flags) {
            // Skip MSVC options not suitable for preprocessor
            if (flag != "/nologo" && flag.rfind("/F", 0) != 0) {
                pp_cmd << flag << " ";
            }
        }
        pp_cmd << source_file;
    } else {
        pp_cmd << "\"" << compiler << "\" -E ";
        for (const auto& flag : other_flags) {
            pp_cmd << flag << " ";
        }
        pp_cmd << source_file;
    }

    // Run local preprocessor
    int pp_exit = 0;
    std::string preprocessed_source = run_local_capture(pp_cmd.str(), pp_exit);
    if (pp_exit != 0) {
        std::cerr << preprocessed_source;
        return pp_exit;
    }

    // Extrahiere Defines, Include-Pfade, Standard und normalisierte Flags
    std::vector<std::string> sorted_defines;
    std::vector<std::string> sorted_include_paths;
    std::string lang_standard = "";
    std::vector<std::string> clean_flags;

    for (size_t i = 0; i < other_flags.size(); ++i) {
        const std::string& flag = other_flags[i];
        if (is_msvc) {
            if (flag.rfind("/D", 0) == 0 || flag.rfind("-D", 0) == 0) {
                if (flag.size() == 2) {
                    if (i + 1 < other_flags.size()) {
                        sorted_defines.push_back(flag + other_flags[i + 1]);
                        i++;
                    } else {
                        sorted_defines.push_back(flag);
                    }
                } else {
                    sorted_defines.push_back(flag);
                }
            } else if (flag.rfind("/I", 0) == 0 || flag.rfind("-I", 0) == 0) {
                if (flag.size() == 2) {
                    if (i + 1 < other_flags.size()) {
                        sorted_include_paths.push_back(flag + other_flags[i + 1]);
                        i++;
                    } else {
                        sorted_include_paths.push_back(flag);
                    }
                } else {
                    sorted_include_paths.push_back(flag);
                }
            } else if (flag.rfind("/std:", 0) == 0 || flag.rfind("-std:", 0) == 0) {
                lang_standard = flag;
            } else if (flag.rfind("/O", 0) == 0 || flag.rfind("-O", 0) == 0 ||
                       flag.rfind("/W", 0) == 0 || flag.rfind("-W", 0) == 0) {
                clean_flags.push_back(flag);
            }
        } else {
            // GCC/Clang
            if (flag.rfind("-D", 0) == 0) {
                if (flag.size() == 2) {
                    if (i + 1 < other_flags.size()) {
                        sorted_defines.push_back(flag + other_flags[i + 1]);
                        i++;
                    } else {
                        sorted_defines.push_back(flag);
                    }
                } else {
                    sorted_defines.push_back(flag);
                }
            } else if (flag.rfind("-I", 0) == 0) {
                if (flag.size() == 2) {
                    if (i + 1 < other_flags.size()) {
                        sorted_include_paths.push_back(flag + other_flags[i + 1]);
                        i++;
                    } else {
                        sorted_include_paths.push_back(flag);
                    }
                } else {
                    sorted_include_paths.push_back(flag);
                }
            } else if (flag.rfind("-std=", 0) == 0) {
                lang_standard = flag;
            } else if (flag.rfind("-O", 0) == 0 || flag.rfind("-W", 0) == 0) {
                clean_flags.push_back(flag);
            }
        }
    }

    // Sortierung für Reihenfolgeunabhängigkeit
    std::sort(sorted_defines.begin(), sorted_defines.end());
    std::sort(sorted_include_paths.begin(), sorted_include_paths.end());
    std::sort(clean_flags.begin(), clean_flags.end());

    // Erzeuge delimitierte Strings mit 0x1F
    std::string sorted_defines_str;
    for (size_t i = 0; i < sorted_defines.size(); ++i) {
        if (i > 0) sorted_defines_str += '\x1F';
        sorted_defines_str += sorted_defines[i];
    }

    std::string sorted_include_paths_str;
    for (size_t i = 0; i < sorted_include_paths.size(); ++i) {
        if (i > 0) sorted_include_paths_str += '\x1F';
        sorted_include_paths_str += sorted_include_paths[i];
    }

    // Flags mit 0x1F (Unit Separator) trennen — konsistent mit Defines/Includes
    std::string flags_str;
    for (size_t i = 0; i < clean_flags.size(); ++i) {
        if (i > 0) flags_str += '\x1F';
        flags_str += clean_flags[i];
    }

    // Abfrage der Compiler-Metadaten
    std::string compiler_version;
    std::string target_architecture;
    query_compiler_metadata(compiler, is_msvc, compiler_version, target_architecture);

    // Normalisiere die präprozessierte Quelldatei
    std::string normalized_source = suco::normalize_preprocessed_source(preprocessed_source);

    // Guard: Leere normalisierte Source (z.B. reine Header-only Datei ohne Code)
    // → Fallback auf lokale Kompilierung, da Cache-Hit sinnlos wäre
    if (normalized_source.empty()) {
        return run_fallback_local(argv);
    }

    // Compute versioned cache hash over normalized source and build metadata
    suco::CacheKeyInput cache_key{
        target_architecture, compiler_version, lang_standard,
        sorted_defines_str, sorted_include_paths_str, flags_str
    };
    std::string source_hash = suco::compute_cache_hash(normalized_source, cache_key);

    // Get coordinator host
    std::string coordinator_host = "127.0.0.1";
    if (const char* env_host = std::getenv("SUCO_COORDINATOR_HOST")) {
        coordinator_host = env_host;
    }

    // Connect to Coordinator with strict 100ms connection timeout (resilient fallback)
    socket_t sock = connect_with_timeout(coordinator_host, suco::DEFAULT_PORT, 100);
    if (sock == INVALID_SOCKET_VAL) {
        // Fallback silently to local compilation
        return run_fallback_local(argv);
    }

    // Construct compiler command to send to coordinator (use space-separated for actual command)
    std::stringstream remote_cmd;
    remote_cmd << compiler;
    for (const auto& f : clean_flags) {
        remote_cmd << " " << f;
    }
    if (!lang_standard.empty()) {
        remote_cmd << " " << lang_standard;
    }
    std::string remote_cmd_str = remote_cmd.str();

    // 1. Send CACHE_QUERY packet
    // Format: [4 bytes: type = CACHE_QUERY] + [4 bytes: hash_len = 64] + [hash_string] + [4 bytes: file_len] + [file_string]
    uint32_t type = htonl(suco::PACKET_CACHE_QUERY);
    uint32_t hash_len = htonl(source_hash.size());
    uint32_t file_len = htonl(source_file.size());
    if (!suco::send_all(sock, &type, 4) ||
        !suco::send_all(sock, &hash_len, 4) ||
        !suco::send_all(sock, source_hash.c_str(), source_hash.size()) ||
        !suco::send_all(sock, &file_len, 4) ||
        !suco::send_all(sock, source_file.c_str(), source_file.size())) {
        close_socket(sock);
        return run_fallback_local(argv);
    }

    // 2. Read Response Type
    uint32_t resp_type_net = 0;
    if (!suco::read_all(sock, &resp_type_net, 4)) {
        close_socket(sock);
        return run_fallback_local(argv);
    }
    uint32_t resp_type = ntohl(resp_type_net);

    if (resp_type == suco::PACKET_CACHE_HIT) {
        // Cache Hit! Read: [4 bytes: log_len] + [log] + [4 bytes: bin_len] + [bin_data]
        uint32_t log_len = 0;
        if (!suco::read_all(sock, &log_len, 4)) {
            close_socket(sock);
            return run_fallback_local(argv);
        }
        log_len = ntohl(log_len);
        if (log_len > 0) {
            std::vector<char> log_buf(log_len);
            if (suco::read_all(sock, log_buf.data(), log_len)) {
                std::cerr.write(log_buf.data(), log_len);
            }
        }

        uint32_t bin_len = 0;
        if (!suco::read_all(sock, &bin_len, 4)) {
            close_socket(sock);
            return run_fallback_local(argv);
        }
        bin_len = ntohl(bin_len);
        if (bin_len > 0) {
            std::vector<uint8_t> bin_data(bin_len);
            if (suco::read_all(sock, bin_data.data(), bin_len)) {
                std::ofstream out(output_file, std::ios::binary);
                if (out.is_open()) {
                    out.write(reinterpret_cast<const char*>(bin_data.data()), bin_len);
                    out.close();
                    close_socket(sock);
                    return 0; // Success!
                }
            }
        }
        close_socket(sock);
        return run_fallback_local(argv);
    }

    // Cache Miss! Coordinator expects: [COMPILE_REQ] + [cmd_len] + [cmd] + [file_len] + [filename] + [src_len] + [source]
    if (resp_type == suco::PACKET_CACHE_MISS) {
        uint32_t req_type = htonl(suco::PACKET_COMPILE_REQ);
        uint32_t cmd_len = htonl(remote_cmd_str.size());
        uint32_t file_len = htonl(source_file.size());
        uint32_t src_len = htonl(preprocessed_source.size());

        if (!suco::send_all(sock, &req_type, 4) ||
            !suco::send_all(sock, &cmd_len, 4) ||
            !suco::send_all(sock, remote_cmd_str.c_str(), remote_cmd_str.size()) ||
            !suco::send_all(sock, &file_len, 4) ||
            !suco::send_all(sock, source_file.c_str(), source_file.size()) ||
            !suco::send_all(sock, &src_len, 4) ||
            !suco::send_all(sock, preprocessed_source.c_str(), preprocessed_source.size())) {
            close_socket(sock);
            return run_fallback_local(argv);
        }

        // Wait for Compilation Response: [COMPILE_RESP] + [exit_code] + [log_len] + [log] + [bin_len] + [bin]
        uint32_t compile_resp_type_net = 0;
        if (!suco::read_all(sock, &compile_resp_type_net, 4)) {
            close_socket(sock);
            return run_fallback_local(argv);
        }
        uint32_t compile_resp_type = ntohl(compile_resp_type_net);
        if (compile_resp_type != suco::PACKET_COMPILE_RESP) {
            close_socket(sock);
            return run_fallback_local(argv);
        }

        int32_t exit_code_net = 0;
        if (!suco::read_all(sock, &exit_code_net, 4)) {
            close_socket(sock);
            return run_fallback_local(argv);
        }
        int32_t exit_code = ntohl(exit_code_net);

        uint32_t log_len = 0;
        if (!suco::read_all(sock, &log_len, 4)) {
            close_socket(sock);
            return run_fallback_local(argv);
        }
        log_len = ntohl(log_len);
        if (log_len > 0) {
            std::vector<char> log_buf(log_len);
            if (suco::read_all(sock, log_buf.data(), log_len)) {
                std::cerr.write(log_buf.data(), log_len);
            }
        }

        uint32_t bin_len = 0;
        if (!suco::read_all(sock, &bin_len, 4)) {
            close_socket(sock);
            return run_fallback_local(argv);
        }
        bin_len = ntohl(bin_len);
        if (bin_len > 0) {
            std::vector<uint8_t> bin_data(bin_len);
            if (suco::read_all(sock, bin_data.data(), bin_len)) {
                if (exit_code == 0) {
                    std::ofstream out(output_file, std::ios::binary);
                    if (out.is_open()) {
                        out.write(reinterpret_cast<const char*>(bin_data.data()), bin_len);
                        out.close();
                    }
                }
            }
        }
        close_socket(sock);
        return exit_code;
    }

    close_socket(sock);
    return run_fallback_local(argv);
}
