#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol.h"
#include "web_server.h"

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

// Generate temp file name
std::string get_temp_file(const std::string& suffix) {
    std::string temp_str = "/tmp/suco_grid_XXXXXX" + suffix;
    std::vector<char> temp_chars(temp_str.begin(), temp_str.end());
    temp_chars.push_back('\0');
    int fd = mkstemps(temp_chars.data(), suffix.size());
    if (fd >= 0) {
        close(fd);
        return std::string(temp_chars.data());
    }
    return "/tmp/suco_grid_" + std::to_string(rand()) + suffix;
}

// Helper to run a command and capture output
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

// Rebuild client command to compile via temp files
std::string rebuild_command(const std::string& client_cmd, const std::string& in_file, const std::string& out_file) {
    std::stringstream ss(client_cmd);
    std::string word;
    std::string new_cmd;
    bool skip_next = false;
    while (ss >> word) {
        if (skip_next) {
            skip_next = false;
            continue;
        }
        if (word == "-c") {
            continue;
        }
        if (word == "-") {
            continue;
        }
        if (word == "-o") {
            skip_next = true;
            continue;
        }
        new_cmd += word + " ";
    }
    new_cmd += "-c " + in_file + " -o " + out_file;
    return new_cmd;
}

void handle_client(int client_sock) {
    // Set socket receive timeout (60s)
    struct timeval tv;
    tv.tv_sec = 60;
    tv.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // 1. Read request
    uint32_t cmd_len = 0;
    if (!read_all(client_sock, &cmd_len, 4)) {
        close(client_sock);
        return;
    }
    cmd_len = ntohl(cmd_len);

    std::string client_cmd;
    if (cmd_len > 0) {
        std::vector<char> cmd_buf(cmd_len);
        if (!read_all(client_sock, cmd_buf.data(), cmd_len)) {
            close(client_sock);
            return;
        }
        client_cmd.assign(cmd_buf.data(), cmd_len);
    }

    uint32_t src_len = 0;
    if (!read_all(client_sock, &src_len, 4)) {
        close(client_sock);
        return;
    }
    src_len = ntohl(src_len);

    std::string preprocessed_source;
    if (src_len > 0) {
        std::vector<char> src_buf(src_len);
        if (!read_all(client_sock, src_buf.data(), src_len)) {
            close(client_sock);
            return;
        }
        preprocessed_source.assign(src_buf.data(), src_len);
    }

    uint32_t file_len = 0;
    if (!read_all(client_sock, &file_len, 4)) {
        close(client_sock);
        return;
    }
    file_len = ntohl(file_len);

    std::string filename;
    if (file_len > 0) {
        std::vector<char> file_buf(file_len);
        if (!read_all(client_sock, file_buf.data(), file_len)) {
            close(client_sock);
            return;
        }
        filename.assign(file_buf.data(), file_len);
    }

    // IP-Adresse des Clients ermitteln
    struct sockaddr_in peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    std::string client_ip = "127.0.0.1";
    if (getpeername(client_sock, (struct sockaddr*)&peer_addr, &peer_len) == 0) {
        client_ip = inet_ntoa(peer_addr.sin_addr);
    }

    // Job ID generieren
    std::string job_id = std::to_string(rand()) + "_" + std::to_string(client_sock);

    // Job registrieren
    {
        std::lock_guard<std::mutex> lock(suco::g_stats.mutex);
        suco::g_stats.total_requests++;
        suco::g_stats.active_jobs.push_back({
            job_id,
            client_ip,
            filename,
            client_cmd,
            std::chrono::steady_clock::now()
        });
    }

    // 2. Perform Compilation
    // Write preprocessed source to temp file
    std::string suffix = ".ii";
    if (client_cmd.find("-x c") != std::string::npos && client_cmd.find("-x c++") == std::string::npos) {
        suffix = ".i";
    }
    std::string in_file = get_temp_file(suffix);
    std::string out_file = get_temp_file(".o");

    std::ofstream out(in_file);
    if (!out.is_open()) {
        std::cerr << "suco-helper error: Failed to write to temporary file: " << in_file << std::endl;
        close(client_sock);
        return;
    }
    out << preprocessed_source;
    out.close();

    // Rebuild and execute command
    std::string build_cmd = rebuild_command(client_cmd, in_file, out_file);
    int exit_code = 0;
    std::string compiler_output = run_local_capture(build_cmd, exit_code);

    // Read compiled binary
    std::vector<uint8_t> bin_data;
    if (exit_code == 0) {
        std::ifstream in(out_file, std::ios::binary | std::ios::ate);
        if (in.is_open()) {
            std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);
            bin_data.resize(size);
            in.read(reinterpret_cast<char*>(bin_data.data()), size);
            in.close();
        } else {
            exit_code = -2; // Mark read error
            compiler_output += "\nsuco-helper error: Failed to read output object file: " + out_file;
        }
    }

    // Clean up temporary files
    unlink(in_file.c_str());
    unlink(out_file.c_str());

    // 3. Send Response
    // [4 bytes: exit_code] + [4 bytes: log_len] + [log] + [4 bytes: bin_len] + [bin]
    int32_t exit_code_net = htonl(exit_code);
    uint32_t log_len_net = htonl(compiler_output.size());
    uint32_t bin_len_net = htonl(bin_data.size());

    send_all(client_sock, &exit_code_net, 4);
    send_all(client_sock, &log_len_net, 4);
    if (!compiler_output.empty()) {
        send_all(client_sock, compiler_output.c_str(), compiler_output.size());
    }
    send_all(client_sock, &bin_len_net, 4);
    if (!bin_data.empty()) {
        send_all(client_sock, bin_data.data(), bin_data.size());
    }

    // Job entfernen und in Verlauf eintragen
    {
        std::lock_guard<std::mutex> lock(suco::g_stats.mutex);
        for (auto it = suco::g_stats.active_jobs.begin(); it != suco::g_stats.active_jobs.end(); ++it) {
            if (it->id == job_id) {
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - it->start_time
                ).count();

                suco::g_stats.recent_jobs.insert(suco::g_stats.recent_jobs.begin(), {
                    filename,
                    client_ip,
                    exit_code,
                    static_cast<int>(duration),
                    false
                });

                if (suco::g_stats.recent_jobs.size() > 10) {
                    suco::g_stats.recent_jobs.pop_back();
                }

                suco::g_stats.active_jobs.erase(it);
                break;
            }
        }
    }

    close(client_sock);
}

int main() {
    // Initialize random seed
    srand(time(nullptr));

    int port = suco::DEFAULT_PORT;
    if (const char* env_port = std::getenv("SUCO_PORT")) {
        port = std::stoi(env_port);
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "suco-helper: Failed to create server socket." << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "suco-helper: Bind failed on port " << port << std::endl;
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 128) < 0) {
        std::cerr << "suco-helper: Listen failed." << std::endl;
        close(server_fd);
        return 1;
    }

    std::cout << "suco-helper: Compile daemon listening on port " << port << std::endl;

    // Webserver auf Port 9001 im Hintergrund starten
    std::thread web_thread(suco::run_web_server, 9001);
    web_thread.detach();

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock >= 0) {
            // Spawn detached thread to handle client connection concurrently
            std::thread t(handle_client, client_sock);
            t.detach();
        }
    }

    close(server_fd);
    return 0;
}
