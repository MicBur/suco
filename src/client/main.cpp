#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/select.h>
#include <hiredis/hiredis.h>
#include "protocol.h"
#include "hash_util.h"

// Helper to run a command locally and capture stdout
std::string run_local_capture(const std::string& cmd, int& exit_code) {
    std::string result;
    char buffer[4096];
    std::string cmd_with_err = cmd + " 2>&1";
    FILE* pipe = popen(cmd_with_err.c_str(), "r");
    if (!pipe) {
        exit_code = -1;
        return "";
    }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    int status = pclose(pipe);
    exit_code = WEXITSTATUS(status);
    return result;
}

// Fallback to local compilation
void fallback_local(char** argv) {
    // Re-execute original command
    execvp(argv[1], argv + 1);
    // If execvp fails:
    std::cerr << "suco error: Failed to execute local fallback compiler " << argv[1] << std::endl;
    exit(1);
}

// Send all bytes to socket
bool send_all(int sock, const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
    while (len > 0) {
        ssize_t sent = send(sock, ptr, len, 0);
        if (sent <= 0) return false;
        ptr += sent;
        len -= sent;
    }
    return true;
}

// Read exact bytes from socket
bool read_all(int sock, void* data, size_t len) {
    char* ptr = static_cast<char*>(data);
    while (len > 0) {
        ssize_t received = recv(sock, ptr, len, 0);
        if (received <= 0) return false;
        ptr += received;
        len -= received;
    }
    return true;
}

// Connect to socket with timeout using select (non-blocking transition)
int connect_with_timeout(int sock, const struct sockaddr* addr, socklen_t addrlen, int timeout_sec) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int res = connect(sock, addr, addrlen);
    if (res < 0) {
        if (errno == EINPROGRESS) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);

            struct timeval tv;
            tv.tv_sec = timeout_sec;
            tv.tv_usec = 0;

            res = select(sock + 1, nullptr, &write_fds, nullptr, &tv);
            if (res > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                    res = -1;
                } else {
                    res = 0; // Success
                }
            } else {
                res = -1; // Timeout or select error
            }
        } else {
            res = -1;
        }
    }

    // Restore blocking mode
    fcntl(sock, F_SETFL, flags);
    return res;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: suco <compiler> [flags] -c <source> -o <output>" << std::endl;
        return 1;
    }

    std::string compiler = argv[1];
    bool has_c = false;
    std::string output_file;
    std::string source_file;
    std::vector<std::string> other_flags;

    // Parse arguments
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
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

    // Fallback if not a standard compilation step (-c is missing or no source/output)
    if (!has_c || output_file.empty() || source_file.empty()) {
        fallback_local(argv);
    }

    // Determine target language for preprocessing and compiling
    std::string lang = "c++";
    if (source_file.rfind(".c") != std::string::npos && source_file.rfind(".cpp") == std::string::npos) {
        lang = "c";
    }

    // Build the preprocessor command
    std::stringstream pp_cmd;
    pp_cmd << compiler << " -E ";
    for (const auto& flag : other_flags) {
        pp_cmd << flag << " ";
    }
    pp_cmd << source_file;

    // Run local preprocessor
    int pp_exit = 0;
    std::string preprocessed_source = run_local_capture(pp_cmd.str(), pp_exit);
    if (pp_exit != 0) {
        std::cerr << preprocessed_source;
        return pp_exit;
    }

    // Gather compiler flags for remote compile (strip includes/macros for hashing/helper clean command)
    std::stringstream compile_flags;
    for (const auto& flag : other_flags) {
        if (flag.rfind("-O", 0) == 0 || flag.rfind("-W", 0) == 0 || 
            flag.rfind("-std=", 0) == 0 || flag.rfind("-m", 0) == 0 || 
            flag.rfind("-f", 0) == 0) {
            compile_flags << flag << " ";
        }
    }
    std::string flags_str = compile_flags.str();

    // Calculate hash
    std::string source_hash = suco::calculate_sha256(preprocessed_source, flags_str);

    // Redis configuration (defaults to localhost, override via Env)
    std::string redis_read_host = "127.0.0.1";
    int redis_read_port = 6379;
    if (const char* env_host = std::getenv("SUCO_REDIS_REPLICA_HOST")) redis_read_host = env_host;
    if (const char* env_port = std::getenv("SUCO_REDIS_REPLICA_PORT")) redis_read_port = std::stoi(env_port);

    std::string redis_write_host = redis_read_host;
    int redis_write_port = redis_read_port;
    if (const char* env_host = std::getenv("SUCO_REDIS_MASTER_HOST")) redis_write_host = env_host;
    if (const char* env_port = std::getenv("SUCO_REDIS_MASTER_PORT")) redis_write_port = std::stoi(env_port);

    // Check Redis replica for Cache Hit with timeout
    struct timeval redis_timeout = { 0, 500000 }; // 500ms
    redisContext* redis_read = redisConnectWithTimeout(redis_read_host.c_str(), redis_read_port, redis_timeout);
    bool cache_hit = false;
    if (redis_read && !redis_read->err) {
        std::string obj_key = "suco:obj:" + source_hash;
        std::string log_key = "suco:log:" + source_hash;

        redisReply* reply_obj = (redisReply*)redisCommand(redis_read, "GET %s", obj_key.c_str());
        if (reply_obj && reply_obj->type == REDIS_REPLY_STRING) {
            // Write binary file
            std::ofstream out(output_file, std::ios::binary);
            if (out.is_open()) {
                out.write(reply_obj->str, reply_obj->len);
                out.close();
                cache_hit = true;

                // Also display compiler warnings if there were any
                redisReply* reply_log = (redisReply*)redisCommand(redis_read, "GET %s", log_key.c_str());
                if (reply_log && reply_log->type == REDIS_REPLY_STRING && reply_log->len > 0) {
                    std::cerr << reply_log->str;
                }
                if (reply_log) freeReplyObject(reply_log);
            }
        }
        if (reply_obj) freeReplyObject(reply_obj);
        redisFree(redis_read);
    }

    if (cache_hit) {
        return 0; // Cache-Hit success!
    }

    // Connect to Remote Helper Service
    std::string helper_host = "127.0.0.1";
    int helper_port = suco::DEFAULT_PORT;
    if (const char* env_host = std::getenv("SUCO_HELPER_HOST")) helper_host = env_host;
    if (const char* env_port = std::getenv("SUCO_HELPER_PORT")) helper_port = std::stoi(env_port);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cerr << "suco warning: Failed to create socket, falling back to local compile." << std::endl;
        fallback_local(argv);
    }

    struct hostent* server = gethostbyname(helper_host.c_str());
    if (!server) {
        std::cerr << "suco warning: Helper host not found, falling back to local compile." << std::endl;
        close(sock);
        fallback_local(argv);
    }

    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    std::memcpy(&serv_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    serv_addr.sin_port = htons(helper_port);

    // Set connection timeout (e.g. 2 seconds)
    if (connect_with_timeout(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr), 2) < 0) {
        std::cerr << "suco warning: Connection to helper " << helper_host << ":" << helper_port 
                  << " failed. Falling back to local compile." << std::endl;
        close(sock);
        fallback_local(argv);
    }

    // Set receive/send timeout for protocol transmission
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    // Construct command to send to remote node
    std::stringstream remote_cmd;
    remote_cmd << compiler << " " << flags_str << " -x " << lang << " -c - -o output.o";
    std::string remote_cmd_str = remote_cmd.str();

    // Serialise Request:
    // [4 bytes: cmd_len] + [cmd] + [4 bytes: src_len] + [src]
    uint32_t cmd_len = htonl(remote_cmd_str.size());
    uint32_t src_len = htonl(preprocessed_source.size());

    if (!send_all(sock, &cmd_len, 4) ||
        !send_all(sock, remote_cmd_str.c_str(), remote_cmd_str.size()) ||
        !send_all(sock, &src_len, 4) ||
        !send_all(sock, preprocessed_source.c_str(), preprocessed_source.size())) {
        std::cerr << "suco error: Sending data to helper failed. Falling back to local compile." << std::endl;
        close(sock);
        fallback_local(argv);
    }

    // Disable send/receive timeout for compilation since it can take longer
    tv.tv_sec = 60; // 60 seconds compile timeout limit
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // Receive Response:
    // [4 bytes: exit_code] + [4 bytes: log_len] + [log] + [4 bytes: bin_len] + [bin]
    int32_t exit_code = -1;
    if (!read_all(sock, &exit_code, 4)) {
        std::cerr << "suco error: Failed to receive exit code. Falling back to local compile." << std::endl;
        close(sock);
        fallback_local(argv);
    }
    exit_code = ntohl(exit_code);

    uint32_t log_len = 0;
    if (!read_all(sock, &log_len, 4)) {
        std::cerr << "suco error: Failed to receive compiler output size. Falling back." << std::endl;
        close(sock);
        fallback_local(argv);
    }
    log_len = ntohl(log_len);

    std::string compiler_output;
    if (log_len > 0) {
        std::vector<char> log_buf(log_len);
        if (!read_all(sock, log_buf.data(), log_len)) {
            std::cerr << "suco error: Failed to read compiler output. Falling back." << std::endl;
            close(sock);
            fallback_local(argv);
        }
        compiler_output.assign(log_buf.data(), log_len);
    }

    uint32_t bin_len = 0;
    if (!read_all(sock, &bin_len, 4)) {
        std::cerr << "suco error: Failed to receive binary object size. Falling back." << std::endl;
        close(sock);
        fallback_local(argv);
    }
    bin_len = ntohl(bin_len);

    std::vector<uint8_t> bin_data;
    if (bin_len > 0) {
        bin_data.resize(bin_len);
        if (!read_all(sock, bin_data.data(), bin_len)) {
            std::cerr << "suco error: Failed to read binary object data. Falling back." << std::endl;
            close(sock);
            fallback_local(argv);
        }
    }
    close(sock);

    // Print compiler output
    if (!compiler_output.empty()) {
        std::cerr << compiler_output;
    }

    if (exit_code == 0 && bin_len > 0) {
        // Save to output file
        std::ofstream out(output_file, std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "suco error: Failed to write output file: " << output_file << std::endl;
            return 1;
        }
        out.write(reinterpret_cast<const char*>(bin_data.data()), bin_len);
        out.close();

        // Save to Redis Master with timeout
        struct timeval redis_write_timeout = { 1, 0 }; // 1s
        redisContext* redis_write = redisConnectWithTimeout(redis_write_host.c_str(), redis_write_port, redis_write_timeout);
        if (redis_write && !redis_write->err) {
            std::string obj_key = "suco:obj:" + source_hash;
            std::string log_key = "suco:log:" + source_hash;

            // Write object binary
            redisReply* r1 = (redisReply*)redisCommand(redis_write, "SET %s %b", obj_key.c_str(), bin_data.data(), (size_t)bin_len);
            // Write log
            redisReply* r2 = (redisReply*)redisCommand(redis_write, "SET %s %s", log_key.c_str(), compiler_output.c_str());
            
            // Set TTL of 7 days (604800 seconds)
            redisCommand(redis_write, "EXPIRE %s 604800", obj_key.c_str());
            redisCommand(redis_write, "EXPIRE %s 604800", log_key.c_str());

            if (r1) freeReplyObject(r1);
            if (r2) freeReplyObject(r2);
            redisFree(redis_write);
        }
    }

    return exit_code;
}
