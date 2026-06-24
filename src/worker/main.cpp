#include "socket_util.h"

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <mutex>
#include <algorithm>

#include "protocol.h"

#ifdef _WIN32
    #include <direct.h>
    #include <process.h>
    #define getpid _getpid
#else
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/wait.h>
#endif

std::mutex socket_write_mutex;
int g_slots_total = 4;
int g_slots_used = 0;

#ifdef _WIN32
FILETIME g_prev_idle, g_prev_kernel, g_prev_user;
void init_windows_cpu_times() {
    GetSystemTimes(&g_prev_idle, &g_prev_kernel, &g_prev_user);
}

double get_windows_cpu_usage(FILETIME& prev_idle, FILETIME& prev_kernel, FILETIME& prev_user) {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;

    auto to_uint64 = [](const FILETIME& ft) {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };

    uint64_t idle_diff = to_uint64(idle) - to_uint64(prev_idle);
    uint64_t kernel_diff = to_uint64(kernel) - to_uint64(prev_kernel);
    uint64_t user_diff = to_uint64(user) - to_uint64(prev_user);

    prev_idle = idle;
    prev_kernel = kernel;
    prev_user = user;

    uint64_t total = kernel_diff + user_diff;
    if (total == 0) return 0.0;

    if (kernel_diff < idle_diff) return 0.0;
    uint64_t active = total - idle_diff;
    return (static_cast<double>(active) / total) * 100.0;
}
#else
struct CpuTime {
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    uint64_t get_total() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    uint64_t get_idle() const {
        return idle + iowait;
    }
};

std::vector<CpuTime> read_proc_stat() {
    std::vector<CpuTime> cpu_times;
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return cpu_times;
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("cpu") == 0) {
            if (line.size() > 3 && line[3] != ' ') {
                std::stringstream ss(line);
                std::string name;
                CpuTime t;
                if (ss >> name >> t.user >> t.nice >> t.system >> t.idle >> t.iowait >> t.irq >> t.softirq >> t.steal) {
                    cpu_times.push_back(t);
                }
            }
        }
    }
    return cpu_times;
}
#endif

// Rebuild command for windows MSVC (cl.exe) and Linux (g++)
std::string rebuild_compiler_command(const std::string& orig_cmd, const std::string& temp_in, const std::string& temp_out, bool is_msvc) {
    std::stringstream ss(orig_cmd);
    std::string word;
    std::string new_cmd;
    bool skip_next = false;
    
    while (ss >> word) {
        if (skip_next) {
            skip_next = false;
            continue;
        }
        
        if (is_msvc) {
            if (word.find("/Fo") == 0) {
                if (word == "/Fo") {
                    skip_next = true;
                }
                continue;
            }
            if (word == "/c" || word == "-c") {
                continue;
            }
            if (word == "-") {
                continue;
            }
        } else {
            if (word == "-o") {
                skip_next = true;
                continue;
            }
            if (word == "-c") {
                continue;
            }
            if (word == "-") {
                continue;
            }
        }
        
        new_cmd += word + " ";
    }
    
    if (is_msvc) {
        new_cmd += " /c \"" + temp_in + "\" /Fo\"" + temp_out + "\"";
    } else {
        new_cmd += " -c \"" + temp_in + "\" -o \"" + temp_out + "\"";
    }
    
    return new_cmd;
}

bool check_is_msvc(const std::string& cmd) {
    std::stringstream ss(cmd);
    std::string first_word;
    ss >> first_word;
    
    size_t last_slash = first_word.find_last_of("\\/");
    std::string exe_name = (last_slash == std::string::npos) ? first_word : first_word.substr(last_slash + 1);
    
    std::transform(exe_name.begin(), exe_name.end(), exe_name.begin(), ::tolower);
    return exe_name == "cl" || exe_name == "cl.exe";
}

std::string run_local_capture(const std::string& cmd, int& exit_code) {
    std::string result;
    char buffer[4096];
    std::string cmd_with_err = cmd + " 2>&1";
    
#ifdef _WIN32
    FILE* pipe = _popen(cmd_with_err.c_str(), "r");
#else
    FILE* pipe = popen(cmd_with_err.c_str(), "r");
#endif

    if (!pipe) {
        exit_code = -1;
        return "suco-worker error: Failed to start compiler process.";
    }
    
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    
#ifdef _WIN32
    int status = _pclose(pipe);
    exit_code = status;
#else
    int status = pclose(pipe);
    exit_code = WEXITSTATUS(status);
#endif

    return result;
}

std::string get_temp_file(const std::string& suffix) {
    static std::mutex temp_mutex;
    std::lock_guard<std::mutex> lock(temp_mutex);
    
#ifdef _WIN32
    char temp_path[MAX_PATH];
    DWORD path_len = GetTempPathA(MAX_PATH, temp_path);
    std::string dir = (path_len > 0) ? std::string(temp_path) : ".\\";
    std::string path = dir + "suco_temp_" + std::to_string(rand()) + "_" + std::to_string(GetCurrentProcessId()) + suffix;
    return path;
#else
    std::string temp_str = "/tmp/suco_temp_XXXXXX" + suffix;
    std::vector<char> temp_chars(temp_str.begin(), temp_str.end());
    temp_chars.push_back('\0');
    int fd = mkstemps(temp_chars.data(), suffix.size());
    if (fd >= 0) {
        close(fd);
        return std::string(temp_chars.data());
    }
    return "/tmp/suco_temp_" + std::to_string(rand()) + "_" + std::to_string(getpid()) + suffix;
#endif
}

std::string get_host_name() {
#ifdef _WIN32
    char buf[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(buf);
    if (GetComputerNameA(buf, &size)) {
        return std::string(buf);
    }
#else
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
#endif
    return "unknown-host";
}

std::string get_os_name() {
#ifdef _WIN32
    return "Windows";
#else
    return "Linux";
#endif
}

int get_logical_cores() {
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return sysinfo.dwNumberOfProcessors;
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (cores > 0) return static_cast<int>(cores);
    return 1;
#endif
}

void run_heartbeat_loop(socket_t sock) {
#ifdef _WIN32
    init_windows_cpu_times();
#endif
#ifndef _WIN32
    auto prev_cpu_times = read_proc_stat();
#endif

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::vector<double> usage(g_slots_total, 0.0);
        
#ifdef _WIN32
        double global_usage = get_windows_cpu_usage(g_prev_idle, g_prev_kernel, g_prev_user);
        for (int i = 0; i < g_slots_total; ++i) {
            usage[i] = global_usage;
        }
#else
        auto curr_cpu_times = read_proc_stat();
        if (curr_cpu_times.size() >= usage.size() && prev_cpu_times.size() == curr_cpu_times.size()) {
            for (size_t i = 0; i < usage.size(); ++i) {
                uint64_t total_diff = curr_cpu_times[i].get_total() - prev_cpu_times[i].get_total();
                uint64_t idle_diff = curr_cpu_times[i].get_idle() - prev_cpu_times[i].get_idle();
                if (total_diff > 0 && total_diff >= idle_diff) {
                    usage[i] = (static_cast<double>(total_diff - idle_diff) / total_diff) * 100.0;
                }
            }
            prev_cpu_times = curr_cpu_times;
        } else {
            prev_cpu_times = curr_cpu_times;
        }
#endif

        uint32_t type_net = htonl(suco::PACKET_HEARTBEAT);
        uint32_t active_net = htonl(g_slots_used);
        uint32_t total_net = htonl(g_slots_total);
        uint32_t cores_net = htonl(usage.size());
        
        std::lock_guard<std::mutex> lock(socket_write_mutex);
        if (!suco::send_all(sock, &type_net, 4) ||
            !suco::send_all(sock, &active_net, 4) ||
            !suco::send_all(sock, &total_net, 4) ||
            !suco::send_all(sock, &cores_net, 4)) {
            std::cerr << "suco-worker: Failed to send heartbeat. Connection broken." << std::endl;
            break;
        }
        if (!usage.empty()) {
            if (!suco::send_all(sock, usage.data(), usage.size() * sizeof(double))) {
                std::cerr << "suco-worker: Failed to send heartbeat usage details. Connection broken." << std::endl;
                break;
            }
        }
    }
}

void run_worker_compile_loop(socket_t sock) {
    while (true) {
        uint32_t req_type_net = 0;
        if (!suco::read_all(sock, &req_type_net, 4)) {
            std::cerr << "suco-worker: Connection lost to coordinator." << std::endl;
            break;
        }
        uint32_t req_type = ntohl(req_type_net);
        
        if (req_type != suco::PACKET_COMPILE_REQ) {
            std::cerr << "suco-worker error: Unexpected packet type " << req_type << std::endl;
            continue;
        }
        
        uint32_t cmd_len_net = 0;
        if (!suco::read_all(sock, &cmd_len_net, 4)) break;
        uint32_t cmd_len = ntohl(cmd_len_net);
        std::vector<char> cmd_buf(cmd_len);
        if (cmd_len > 0) suco::read_all(sock, cmd_buf.data(), cmd_len);
        std::string command(cmd_buf.data(), cmd_len);
        
        uint32_t file_len_net = 0;
        if (!suco::read_all(sock, &file_len_net, 4)) break;
        uint32_t file_len = ntohl(file_len_net);
        std::vector<char> file_buf(file_len);
        if (file_len > 0) suco::read_all(sock, file_buf.data(), file_len);
        std::string filename(file_buf.data(), file_len);
        
        uint32_t src_len_net = 0;
        if (!suco::read_all(sock, &src_len_net, 4)) break;
        uint32_t src_len = ntohl(src_len_net);
        std::vector<char> src_buf(src_len);
        if (src_len > 0) suco::read_all(sock, src_buf.data(), src_len);
        std::string source(src_buf.data(), src_len);
        
        std::cout << "suco-worker: Compiling job " << filename << "..." << std::endl;
        g_slots_used++;
        
        std::thread([sock, command, filename, source]() {
            bool is_msvc = check_is_msvc(command);
            std::string temp_in = get_temp_file(is_msvc ? ".cpp" : ".ii");
            std::string temp_out = get_temp_file(is_msvc ? ".obj" : ".o");
            
            std::ofstream out(temp_in);
            if (out.is_open()) {
                out << source;
                out.close();
            }
            
            std::string final_cmd = rebuild_compiler_command(command, temp_in, temp_out, is_msvc);
            int exit_code = 0;
            std::string compiler_output = run_local_capture(final_cmd, exit_code);
            
            std::vector<uint8_t> bin_data;
            if (exit_code == 0) {
                std::ifstream in(temp_out, std::ios::binary | std::ios::ate);
                if (in.is_open()) {
                    std::streamsize size = in.tellg();
                    in.seekg(0, std::ios::beg);
                    bin_data.resize(size);
                    in.read(reinterpret_cast<char*>(bin_data.data()), size);
                    in.close();
                } else {
                    exit_code = -2;
                    compiler_output += "\nsuco-worker error: Failed to read output object file: " + temp_out;
                }
            }
            
            std::remove(temp_in.c_str());
            std::remove(temp_out.c_str());
            
            g_slots_used = std::max(0, g_slots_used - 1);
            
            uint32_t resp_type_net = htonl(suco::PACKET_COMPILE_RESP);
            uint32_t f_len_net = htonl(filename.size());
            int32_t exit_code_net = htonl(exit_code);
            uint32_t log_len_net = htonl(compiler_output.size());
            uint32_t bin_len_net = htonl(bin_data.size());
            
            std::lock_guard<std::mutex> lock(socket_write_mutex);
            if (suco::send_all(sock, &resp_type_net, 4) &&
                suco::send_all(sock, &f_len_net, 4) &&
                suco::send_all(sock, filename.c_str(), filename.size()) &&
                suco::send_all(sock, &exit_code_net, 4) &&
                suco::send_all(sock, &log_len_net, 4)) {
                if (!compiler_output.empty()) {
                    suco::send_all(sock, compiler_output.c_str(), compiler_output.size());
                }
                suco::send_all(sock, &bin_len_net, 4);
                if (!bin_data.empty()) {
                    suco::send_all(sock, bin_data.data(), bin_data.size());
                }
            }
            std::cout << "suco-worker: Finished job " << filename << " (Exit: " << exit_code << ")" << std::endl;
        }).detach();
    }
}

int main(int argc, char* argv[]) {
    srand(static_cast<unsigned int>(time(nullptr)));
    suco::SocketInit sock_init;
    
    std::string coord_ip = "";
    uint16_t coord_port = suco::DEFAULT_PORT;
    g_slots_total = std::max(1, get_logical_cores());
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--coordinator" && i + 1 < argc) {
            std::string val = argv[++i];
            size_t colon = val.find(':');
            if (colon != std::string::npos) {
                coord_ip = val.substr(0, colon);
                coord_port = static_cast<uint16_t>(std::stoi(val.substr(colon + 1)));
            } else {
                coord_ip = val;
            }
        } else if (arg == "--slots" && i + 1 < argc) {
            g_slots_total = std::max(1, std::stoi(argv[++i]));
        }
    }
    
    if (coord_ip.empty()) {
        std::cout << "suco-worker: No coordinator address specified. Scanning network via UDP broadcast..." << std::endl;
        socket_t udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_sock != INVALID_SOCKET_VAL) {
            struct sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(suco::DEFAULT_UDP_PORT);
            
            int opt = 1;
#ifdef _WIN32
            setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
            setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
            
            if (bind(udp_sock, (struct sockaddr*)&addr, sizeof(addr)) >= 0) {
#ifdef _WIN32
                int timeout_ms = 5000;
                setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
                struct timeval tv;
                tv.tv_sec = 5;
                tv.tv_usec = 0;
                setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
                
                char buffer[256];
                struct sockaddr_in sender_addr;
                socklen_t sender_len = sizeof(sender_addr);
                
                int bytes_received = recvfrom(udp_sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr*)&sender_addr, &sender_len);
                if (bytes_received > 0) {
                    buffer[bytes_received] = '\0';
                    std::string msg(buffer);
                    if (msg.find("SUCO_COORDINATOR_v1") == 0) {
                        coord_ip = inet_ntoa(sender_addr.sin_addr);
                        std::stringstream ss(msg);
                        std::string prefix;
                        uint16_t port_val = 0;
                        if (ss >> prefix >> port_val) {
                            coord_port = port_val;
                        }
                        std::cout << "suco-worker: Discovered coordinator via UDP: " << coord_ip << ":" << coord_port << std::endl;
                    }
                }
            }
            close_socket(udp_sock);
        }
    }
    
    if (coord_ip.empty()) {
        coord_ip = "127.0.0.1";
        std::cout << "suco-worker: UDP Discovery timed out. Falling back to coordinator at " << coord_ip << ":" << coord_port << std::endl;
    }
    
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_VAL) {
        std::cerr << "suco-worker error: Failed to create TCP socket." << std::endl;
        return 1;
    }
    
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(coord_port);
    inet_pton(AF_INET, coord_ip.c_str(), &address.sin_addr);
    
    std::cout << "suco-worker: Connecting to coordinator at " << coord_ip << ":" << coord_port << "..." << std::endl;
    if (connect(sock, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "suco-worker error: Connection failed to " << coord_ip << ":" << coord_port << std::endl;
        close_socket(sock);
        return 1;
    }
    std::cout << "suco-worker: Connected to coordinator." << std::endl;
    
    uint32_t type_net = htonl(suco::PACKET_HEARTBEAT);
    uint32_t slots_net = htonl(g_slots_total);
    
    std::string name = get_host_name();
    uint32_t name_len_net = htonl(static_cast<u_long>(name.size()));
    
    std::string os = get_os_name();
    uint32_t os_len_net = htonl(static_cast<u_long>(os.size()));
    
    if (!suco::send_all(sock, &type_net, 4) ||
        !suco::send_all(sock, &slots_net, 4) ||
        !suco::send_all(sock, &name_len_net, 4) ||
        !suco::send_all(sock, name.c_str(), name.size()) ||
        !suco::send_all(sock, &os_len_net, 4) ||
        !suco::send_all(sock, os.c_str(), os.size())) {
        std::cerr << "suco-worker error: Registration failed." << std::endl;
        close_socket(sock);
        return 1;
    }
    
    std::cout << "suco-worker: Registered successfully (Slots: " << g_slots_total << ", Name: " << name << ", OS: " << os << ")" << std::endl;
    
    std::thread hb_thread(run_heartbeat_loop, sock);
    hb_thread.detach();
    
    run_worker_compile_loop(sock);
    
    close_socket(sock);
    return 0;
}
