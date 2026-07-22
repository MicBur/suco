#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <set>
#include <atomic>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <thread>
#include <cctype>
#include <chrono>
#include <fstream>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
    #include <io.h>
#else
    #include <unistd.h>
    #include <limits.h>
    #include <sys/wait.h>
    #include <netdb.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

// ClientConfig einbinden
#include "../common/platform_compat.h"
#include "../client/client_config.h"
#include "../common/zstd_util.h"
#include "../common/protocol.h"
#include "../common/socket_util.h"
#include "../common/tls_util.h"
#include "../common/hash_util.h"
#include "msvc_detector.h"
#include "suco_version.h"
#include <fstream>

// Netzwerk-Typen und -Definitionen
#ifdef _WIN32
    using socket_t = SOCKET;
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #define close_socket closesocket
#else
    using socket_t = int;
    #define INVALID_SOCKET_VAL -1
    #define close_socket close
#endif

namespace {

// Hilfsstruktur zur Initialisierung von Sockets unter Windows
#ifdef _WIN32
class SocketInit {
public:
    SocketInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~SocketInit() {
        WSACleanup();
    }
};
#else
class SocketInit {};
#endif

// Hilfsfunktion zum Setzen von Umgebungsvariablen
void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

// Hilfsfunktion zum Lesen von Umgebungsvariablen
std::string get_env_var(const std::string& name) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : "";
}

// Ermittelt das Verzeichnis, in dem dieses Binary liegt (inkl. abschließendem Slash/Backslash)
std::string get_binary_directory(const std::string& argv0) {
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len > 0) {
        std::string p(path, len);
        size_t last_slash = p.find_last_of("\\/");
        if (last_slash != std::string::npos) {
            return p.substr(0, last_slash + 1);
        }
    }
#else
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len != -1) {
        path[len] = '\0';
        std::string p(path);
        size_t last_slash = p.find_last_of("/");
        if (last_slash != std::string::npos) {
            return p.substr(0, last_slash + 1);
        }
    }
#endif
    // Fallback auf argv[0]
    size_t last_slash = argv0.find_last_of("\\/");
    if (last_slash != std::string::npos) {
        return argv0.substr(0, last_slash + 1);
    }
    return "./";
}

// Stellt eine Verbindung zum REST-Port (9001) des Coordinators her und fragt einen Pfad ab
std::string fetch_from_coordinator(const std::string& host, uint16_t port, const std::string& path, std::string& err_msg) {
    SocketInit init;
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_VAL) {
        err_msg = "Socket-Erstellung fehlgeschlagen";
        return "";
    }

#ifdef _WIN32
    DWORD timeout = 1000; // 1 Sekunde Timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    struct hostent* he = gethostbyname(host.c_str());
    if (!he) {
        close_socket(sock);
        err_msg = "Host '" + host + "' konnte nicht aufgelöst werden.";
        return "";
    }
    std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close_socket(sock);
        err_msg = "Verbindung fehlgeschlagen (Coordinator offline?)";
        return "";
    }

    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (send(sock, req.c_str(), static_cast<int>(req.size()), 0) < 0) {
        close_socket(sock);
        err_msg = "HTTP-Request konnte nicht gesendet werden.";
        return "";
    }

    std::string resp;
    char buf[4096];
    while (true) {
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        resp.append(buf, n);
    }
    close_socket(sock);

    size_t pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) {
        err_msg = "Ungültige HTTP-Antwort vom Coordinator erhalten.";
        return "";
    }
    return resp.substr(pos + 4);
}

std::string fetch_stats_from_coordinator(const std::string& host, uint16_t port, std::string& err_msg) {
    return fetch_from_coordinator(host, port, "/api/stats", err_msg);
}

// Hilfsfunktion zum Suchen von einfachen Werten im JSON
std::string find_json_value(const std::string& json, const std::string& key) {
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(":", pos);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        size_t start = pos + 1;
        size_t end = json.find("\"", start);
        if (end == std::string::npos) return "";
        return json.substr(start, end - start);
    } else {
        size_t start = pos;
        size_t end = start;
        while (end < json.size() && ((json[end] >= '0' && json[end] <= '9') || json[end] == '-' || json[end] == '.')) {
            end++;
        }
        return json.substr(start, end - start);
    }
}

struct WorkerStats {
    std::string name;
    std::string ip;
    std::string os;
    int slots_total = 0;
    int slots_used = 0;
    std::string compilers;
    std::string tools;
    std::string qt;
};

// Hilfsfunktion zum Extrahieren von verschachtelten Objekten aus dem Worker-JSON
std::string extract_sub_object(const std::string& obj, const std::string& key) {
    size_t key_pos = obj.find("\"" + key + "\":");
    if (key_pos == std::string::npos) return "";
    size_t start = obj.find("{", key_pos);
    if (start == std::string::npos) return "";
    
    size_t end = start + 1;
    int depth = 1;
    while (end < obj.size() && depth > 0) {
        if (obj[end] == '{') depth++;
        else if (obj[end] == '}') depth--;
        end++;
    }
    return obj.substr(start + 1, end - start - 2);
}

// Formatiert Key-Value Paare aus einem flachen JSON-Objekt
std::string format_kv_pairs(const std::string& sub_obj) {
    std::string result;
    size_t pos = 0;
    while (true) {
        size_t key_start = sub_obj.find("\"", pos);
        if (key_start == std::string::npos) break;
        size_t key_end = sub_obj.find("\"", key_start + 1);
        if (key_end == std::string::npos) break;
        
        std::string key = sub_obj.substr(key_start + 1, key_end - key_start - 1);
        
        size_t colon_pos = sub_obj.find(":", key_end);
        if (colon_pos == std::string::npos) break;
        
        size_t val_start = sub_obj.find("\"", colon_pos);
        if (val_start == std::string::npos) break;
        size_t val_end = sub_obj.find("\"", val_start + 1);
        if (val_end == std::string::npos) break;
        
        std::string val = sub_obj.substr(val_start + 1, val_end - val_start - 1);
        
        if (!result.empty()) result += ", ";
        result += key + " (" + val + ")";
        
        pos = val_end + 1;
    }
    return result.empty() ? "-" : result;
}

// Parsen der Worker aus dem REST JSON-Output
std::vector<WorkerStats> parse_workers(const std::string& json) {
    std::vector<WorkerStats> workers;
    size_t pos = json.find("\"workers\":");
    if (pos == std::string::npos) return workers;
    size_t array_start = json.find("[", pos);
    if (array_start == std::string::npos) return workers;
    
    size_t array_end = json.find("\"recent_jobs\"", array_start);
    if (array_end == std::string::npos) {
        array_end = json.size();
    }
    
    std::string array_content = json.substr(array_start, array_end - array_start);
    size_t obj_pos = 0;
    while (true) {
        size_t obj_start = array_content.find("{", obj_pos);
        if (obj_start == std::string::npos) break;
        
        size_t obj_end = obj_start + 1;
        int depth = 1;
        while (obj_end < array_content.size() && depth > 0) {
            if (array_content[obj_end] == '{') depth++;
            else if (array_content[obj_end] == '}') depth--;
            obj_end++;
        }
        if (depth > 0) break;
        
        std::string obj = array_content.substr(obj_start, obj_end - obj_start);
        WorkerStats w;
        w.name = find_json_value(obj, "name");
        w.ip = find_json_value(obj, "ip");
        w.os = find_json_value(obj, "os");
        
        std::string st = find_json_value(obj, "slots_total");
        std::string su = find_json_value(obj, "slots_used");
        w.slots_total = st.empty() ? 0 : std::stoi(st);
        w.slots_used = su.empty() ? 0 : std::stoi(su);
        
        std::string compilers_json = extract_sub_object(obj, "compilers");
        std::string tools_json = extract_sub_object(obj, "tools");
        std::string qt_json = extract_sub_object(obj, "qt");
        w.compilers = format_kv_pairs(compilers_json);
        w.tools = format_kv_pairs(tools_json);
        w.qt = format_kv_pairs(qt_json);
        
        workers.push_back(w);
        obj_pos = obj_end;
    }
    return workers;
}

// Hilfsfunktion zum Ermitteln des Benutzer-Heimatverzeichnisses
std::string get_home_dir() {
    const char* home_env = std::getenv("HOME");
    if (home_env) return home_env;
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) return userprofile;
    return "";
}

// Markiert compile_commands.json mit "suco_used": true und bereinigt Launcher-Präfixe
void mark_compile_commands(const std::string& bin_dir) {
    std::vector<std::string> paths = {
        "compile_commands.json",
        "build/compile_commands.json",
        "build_windows/compile_commands.json",
        "build_linux/compile_commands.json"
    };
    
    std::string found_path;
    std::error_code ec;
    for (const auto& p : paths) {
        if (std::filesystem::exists(p, ec)) {
            found_path = p;
            break;
        }
    }
    
    if (found_path.empty()) {
        return; // Keine Datei gefunden
    }
    
    std::filesystem::path build_dir = std::filesystem::path(found_path).parent_path();
    if (build_dir.empty()) {
        build_dir = ".";
    }
    
    // Versuche, das Python-Bereinigungsskript zu finden
    std::vector<std::string> script_locations = {
        bin_dir + "/../scripts/suco_clean_compile_commands",
        bin_dir + "/scripts/suco_clean_compile_commands",
        bin_dir + "/suco_clean_compile_commands",
        "./scripts/suco_clean_compile_commands"
    };
    
    std::string script_path;
    for (const auto& loc : script_locations) {
        if (std::filesystem::exists(loc, ec)) {
            script_path = loc;
            break;
        }
    }
    
    if (!script_path.empty()) {
        std::string cmd;
#ifdef _WIN32
        cmd = "python \"" + script_path + "\" \"" + build_dir.string() + "\"";
#else
        cmd = "python3 \"" + script_path + "\" \"" + build_dir.string() + "\"";
#endif
        int ret = std::system(cmd.c_str());
        if (ret == 0) {
            return; // Erfolgreich über Python-Skript gelöst!
        }
    }
    
    // Fallback: Manuelle Bereinigung im C++ Wrapper, falls Python fehlt
    std::ifstream in(found_path, std::ios::binary);
    if (!in.is_open()) return;
    
    std::stringstream ss;
    ss << in.rdbuf();
    in.close();
    
    std::string content = ss.str();
    bool modified = false;
    
    // 1. Markieren mit suco_used und suco_build
    if (content.find("\"suco_used\"") == std::string::npos) {
        std::string target = "\"directory\":";
        std::string replacement = "\"suco_used\": true, \"suco_build\": true, \"directory\":";
        
        size_t pos = 0;
        while ((pos = content.find(target, pos)) != std::string::npos) {
            content.replace(pos, target.length(), replacement);
            pos += replacement.length();
        }
        modified = true;
    }
    
    // 2. Bereinige Launcher-Präfixe aus "command"
    size_t pos = 0;
    while ((pos = content.find("\"command\":", pos)) != std::string::npos) {
        size_t start_quote = content.find('"', pos + 10);
        if (start_quote == std::string::npos) break;
        size_t end_quote = content.find('"', start_quote + 1);
        if (end_quote == std::string::npos) break;
        
        std::string cmd = content.substr(start_quote + 1, end_quote - start_quote - 1);
        size_t space_pos = cmd.find(' ');
        if (space_pos != std::string::npos) {
            std::string first_arg = cmd.substr(0, space_pos);
            bool has_launcher = false;
            std::vector<std::string> suffixes = {
                "suco-cl", "suco-cl++", "suco-cl.exe", "suco-cl++.exe"
            };
            for (const auto& s : suffixes) {
                if (first_arg == s || 
                    (first_arg.length() > s.length() && first_arg.compare(first_arg.length() - s.length() - 1, s.length() + 1, "/" + s) == 0) ||
                    (first_arg.length() > s.length() && first_arg.compare(first_arg.length() - s.length() - 1, s.length() + 1, "\\" + s) == 0)) {
                    has_launcher = true;
                    break;
                }
            }
            
            if (has_launcher) {
                std::string cleaned_cmd = cmd.substr(space_pos + 1);
                while (!cleaned_cmd.empty() && cleaned_cmd[0] == ' ') {
                    cleaned_cmd = cleaned_cmd.substr(1);
                }
                content.replace(start_quote + 1, cmd.length(), cleaned_cmd);
                end_quote = start_quote + 1 + cleaned_cmd.length();
                modified = true;
            }
        }
        pos = end_quote + 1;
    }
    
    // 3. Bereinige Launcher-Präfixe aus "arguments"-Arrays
    pos = 0;
    while ((pos = content.find("\"arguments\":", pos)) != std::string::npos) {
        size_t start_bracket = content.find('[', pos + 12);
        if (start_bracket == std::string::npos) break;
        size_t end_bracket = content.find(']', start_bracket + 1);
        if (end_bracket == std::string::npos) break;
        
        size_t first_quote = content.find('"', start_bracket + 1);
        if (first_quote != std::string::npos && first_quote < end_bracket) {
            size_t second_quote = content.find('"', first_quote + 1);
            if (second_quote != std::string::npos && second_quote < end_bracket) {
                std::string first_arg = content.substr(first_quote + 1, second_quote - first_quote - 1);
                bool has_launcher = false;
                std::vector<std::string> suffixes = {
                    "suco-cl", "suco-cl++", "suco-cl.exe", "suco-cl++.exe"
                };
                for (const auto& s : suffixes) {
                    if (first_arg == s || 
                        (first_arg.length() > s.length() && first_arg.compare(first_arg.length() - s.length() - 1, s.length() + 1, "/" + s) == 0) ||
                        (first_arg.length() > s.length() && first_arg.compare(first_arg.length() - s.length() - 1, s.length() + 1, "\\" + s) == 0)) {
                        has_launcher = true;
                        break;
                    }
                }
                
                if (has_launcher) {
                    size_t comma_pos = content.find(',', second_quote + 1);
                    if (comma_pos != std::string::npos && comma_pos < end_bracket) {
                        content.erase(first_quote, comma_pos + 1 - first_quote);
                    } else {
                        content.erase(first_quote, second_quote + 1 - first_quote);
                    }
                    end_bracket = content.find(']', start_bracket + 1);
                    modified = true;
                }
            }
        }
        pos = end_bracket + 1;
    }
    
    if (modified) {
        std::ofstream out(found_path, std::ios::binary);
        if (out.is_open()) {
            out.write(content.data(), content.size());
            out.close();
            std::cout << "suco: compile_commands.json erfolgreich über Fallback-Routine bereinigt." << std::endl;
        }
    }
}

} // namespace

// Abstract Base Class for OOP Subcommands
class Subcommand {
public:
    virtual ~Subcommand() = default;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;
    virtual int execute(int argc, char** argv, const std::string& bin_dir) = 0;
};

// OOP Subcommand Registry
class SubcommandRegistry {
public:
    static SubcommandRegistry& instance() {
        static SubcommandRegistry reg;
        return reg;
    }

    void register_command(std::unique_ptr<Subcommand> cmd) {
        commands_[cmd->name()] = std::move(cmd);
    }

    bool has_command(const std::string& name) const {
        return commands_.find(name) != commands_.end();
    }

    int execute(const std::string& name, int argc, char** argv, const std::string& bin_dir) {
        auto it = commands_.find(name);
        if (it != commands_.end()) {
            return it->second->execute(argc, argv, bin_dir);
        }
        return -1;
    }

    void print_commands() const {
        std::cout << "Verfügbare Subkommandos:\n";
        for (const auto& [name, cmd] : commands_) {
            std::cout << "  suco " << std::left << std::setw(12) << name << " - " << cmd->description() << "\n";
        }
    }

private:
    std::map<std::string, std::unique_ptr<Subcommand>> commands_;
};

// Subcommand: suco status
class StatusCommand : public Subcommand {
public:
    std::string name() const override { return "status"; }
    std::string description() const override { return "Zeigt den aktuellen Zustand des SUCO-Grids"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        (void)argc; (void)argv; (void)bin_dir;
        suco::ClientConfig config = suco::ClientConfig::load_or_default();
        
        // HTTP Web Server Port ist 9001 (DEFAULT_WEB_PORT)
        uint16_t web_port = 9001; 
        
        std::cout << "Frage Status vom Coordinator bei " << config.coordinator_host << ":" << web_port << " ab...\n" << std::endl;
        
        std::string err_msg;
        std::string json = fetch_stats_from_coordinator(config.coordinator_host, web_port, err_msg);
        
        if (json.empty()) {
            std::cerr << "suco status error: " << err_msg << "\n"
                      << "Stelle sicher, dass der suco-coordinator läuft und erreichbar ist." << std::endl;
            return 1;
        }

        std::string total_reqs = find_json_value(json, "total_requests");
        std::string cache_hits = find_json_value(json, "cache_hits");
        std::string cache_misses = find_json_value(json, "cache_misses");
        std::string active_jobs = find_json_value(json, "active_jobs_count");
        
        auto workers = parse_workers(json);

        int total_slots = 0;
        int used_slots = 0;
        for (const auto& w : workers) {
            total_slots += w.slots_total;
            used_slots += w.slots_used;
        }

        double hit_rate = 0.0;
        try {
            int hits = cache_hits.empty() ? 0 : std::stoi(cache_hits);
            int misses = cache_misses.empty() ? 0 : std::stoi(cache_misses);
            int total = hits + misses;
            if (total > 0) {
                hit_rate = (static_cast<double>(hits) * 100.0) / total;
            }
        } catch (...) {}

        std::cout << "=================================================================" << std::endl;
        std::cout << "                     SUCO GRID STATUS                            " << std::endl;
        std::cout << "=================================================================" << std::endl;
        std::cout << "Coordinator:    " << config.coordinator_host << ":" << config.coordinator_port << " (Online)" << std::endl;
        std::cout << "Web-Dashboard:  http://" << config.coordinator_host << ":" << web_port << "/" << std::endl;
        std::cout << "Aktive Jobs:    " << (active_jobs.empty() ? "0" : active_jobs) << std::endl;
        std::cout << "Total Requests: " << (total_reqs.empty() ? "0" : total_reqs) << std::endl;
        std::cout << "Cache-Hit-Rate: " << std::fixed << std::setprecision(1) << hit_rate << " % "
                  << "(Hits: " << (cache_hits.empty() ? "0" : cache_hits) 
                  << ", Misses: " << (cache_misses.empty() ? "0" : cache_misses) << ")" << std::endl;
        
        std::cout << "Grid-Auslastung: ";
        if (total_slots > 0) {
            int percentage = (used_slots * 100) / total_slots;
            int bar_width = 20;
            int filled = (percentage * bar_width) / 100;
            std::cout << "[";
            for (int i = 0; i < bar_width; ++i) {
                if (i < filled) std::cout << "#";
                else std::cout << ".";
            }
            std::cout << "] " << percentage << "% (" << used_slots << " / " << total_slots << " Slots)" << std::endl;
        } else {
            std::cout << "N/A (Keine Worker)" << std::endl;
        }
        
        std::cout << std::endl;
        std::cout << "Verbundene Worker im Grid: " << workers.size() << std::endl;
        
        if (workers.empty()) {
            std::cout << "Keine aktiven Worker am Coordinator registriert." << std::endl;
        } else {
            std::cout << "-----------------------------------------------------------------" << std::endl;
            std::cout << "| " << std::left << std::setw(15) << "Worker Name" 
                      << " | " << std::left << std::setw(15) << "IP-Adresse" 
                      << " | " << std::left << std::setw(10) << "OS" 
                      << " | " << std::left << std::setw(12) << "Slots (Belegt)" << " |" << std::endl;
            std::cout << "-----------------------------------------------------------------" << std::endl;
            for (const auto& w : workers) {
                std::string slots_str = std::to_string(w.slots_used) + " / " + std::to_string(w.slots_total);
                std::cout << "| " << std::left << std::setw(15) << w.name 
                          << " | " << std::left << std::setw(15) << w.ip 
                          << " | " << std::left << std::setw(10) << w.os 
                          << " | " << std::left << std::setw(12) << slots_str << " |" << std::endl;
            }
            std::cout << "-----------------------------------------------------------------" << std::endl;
        }
        std::cout << "=================================================================" << std::endl;

        return 0;
    }
};

// Subcommand: suco dashboard / monitor
class DashboardCommand : public Subcommand {
public:
    std::string name() const override { return "dashboard"; }
    std::string description() const override { return "Öffnet das Web-Dashboard im Standardbrowser"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        (void)argc; (void)argv;
        std::string wrapper_path = bin_dir + "suco-cl";
#ifdef _WIN32
        wrapper_path += ".exe";
#endif
        std::cout << "suco: Starte SUCO-Dashboard über den Compiler-Client..." << std::endl;
        std::vector<const char*> child_args = { wrapper_path.c_str(), "--monitor", nullptr };
#ifdef _WIN32
        int exit_code = static_cast<int>(_spawnvp(_P_WAIT, wrapper_path.c_str(), const_cast<char* const*>(child_args.data())));
        return (exit_code < 0) ? 1 : exit_code;
#else
        int exit_code = 0;
        pid_t pid = fork();
        if (pid == 0) {
            execvp(wrapper_path.c_str(), const_cast<char* const*>(child_args.data()));
            std::exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        return exit_code;
#endif
    }
};

// Subcommand: suco help
class HelpCommand : public Subcommand {
public:
    std::string name() const override { return "help"; }
    std::string description() const override { return "Zeigt diese Hilfe an"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        (void)argc; (void)argv; (void)bin_dir;
        std::cout << "=================================================================\n"
                  << "                      SUCO GRID UTILITY                          \n"
                  << "=================================================================\n"
                  << "Nutzung als Build-Wrapper:\n"
                  << "  suco <build-befehl> [argumente...]\n"
                  << "  - Führt den Befehl aus und leitet CC/CXX-Aufrufe an das Grid um.\n"
                  << "  Beispiele:\n"
                  << "    suco make -j12\n"
                  << "    suco ninja\n"
                  << "    suco cmake --build build_dir\n\n"
                  << "Nutzung als Subkommando:\n"
                  << "  suco <subcommand> [argumente...]\n\n";
        SubcommandRegistry::instance().print_commands();
        std::cout << "=================================================================\n"
                  << std::endl;
        return 0;
    }
};

// Subcommand: suco workers
class WorkersCommand : public Subcommand {
public:
    std::string name() const override { return "workers"; }
    std::string description() const override { return "Listet alle aktuell registrierten Worker auf"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        (void)argc; (void)argv; (void)bin_dir;
        suco::ClientConfig config = suco::ClientConfig::load_or_default();
        uint16_t web_port = 9001; 
        
        std::string err_msg;
        std::string json = fetch_stats_from_coordinator(config.coordinator_host, web_port, err_msg);
        
        if (json.empty()) {
            std::cerr << "suco workers error: " << err_msg << "\n"
                      << "Stelle sicher, dass der suco-coordinator läuft und erreichbar ist." << std::endl;
            return 1;
        }

        auto workers = parse_workers(json);

        std::cout << "=================================================================" << std::endl;
        std::cout << "                        SUCO GRID WORKERS                        " << std::endl;
        std::cout << "=================================================================" << std::endl;
        std::cout << "Coordinator: " << config.coordinator_host << ":" << config.coordinator_port << " | Verbundene Worker: " << workers.size() << std::endl;
        std::cout << std::endl;

        int total_slots = 0;
        int used_slots = 0;

        if (workers.empty()) {
            std::cout << "Keine aktiven Worker am Coordinator registriert." << std::endl;
        } else {
            for (const auto& w : workers) {
                std::cout << "Worker-Name: " << w.name << " (" << w.ip << ")" << std::endl;
                std::cout << "  OS:        " << w.os << std::endl;
                std::cout << "  Slots:     " << w.slots_used << " / " << w.slots_total << std::endl;
                std::cout << "  Compiler:  " << w.compilers << std::endl;
                std::cout << "  Tools:     " << w.tools << std::endl;
                std::cout << "  Qt:        " << w.qt << std::endl;
                std::cout << "-----------------------------------------------------------------" << std::endl;
                total_slots += w.slots_total;
                used_slots += w.slots_used;
            }
            std::cout << "Gesamte Slots im Grid: " << total_slots 
                      << " (Belegt: " << used_slots 
                      << ", Frei: " << (total_slots - used_slots) << ")" << std::endl;
        }
        std::cout << "=================================================================" << std::endl;

        return 0;
    }
};

// Subcommand: suco cache clear
class CacheClearCommand : public Subcommand {
public:
    std::string name() const override { return "cache"; }
    std::string description() const override { return "Leert den lokalen Cache und den Header-Cache auf den Workern (Nutzung: suco cache clear)"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        (void)bin_dir;
        if (argc < 3 || std::string(argv[2]) != "clear") {
            std::cerr << "suco cache error: Ungültiges Argument.\n"
                      << "Verwende: suco cache clear" << std::endl;
            return 1;
        }

        std::cout << "Leere lokalen SUCO-Cache..." << std::endl;
        std::string home = get_home_dir();
        if (!home.empty()) {
            std::filesystem::path local_cache = std::filesystem::path(home) / ".cache" / "suco";
            std::error_code ec;
            if (std::filesystem::exists(local_cache, ec)) {
                std::filesystem::remove(local_cache / "compiler_metadata_cache.txt", ec);
                std::filesystem::remove(local_cache / "toolchains" / "hash_cache.txt", ec);
                std::filesystem::remove_all(local_cache / "preprocess", ec);
                // The L1 object cache is what actually serves warm rebuilds — leaving it
                // made "cache clear" a no-op for the case users clear for: a build that
                // still hit 341/342 objects locally and looked warm despite the clear.
                std::filesystem::remove_all(local_cache / "objects", ec);
                std::cout << "  Lokale Cache-Dateien gelöscht (Objekt-Cache, Präprozessor-Cache, Metadaten)." << std::endl;
            } else {
                std::cout << "  Kein lokaler Cache-Ordner gefunden." << std::endl;
            }
        } else {
            std::cout << "  Konnte Heimatverzeichnis nicht ermitteln." << std::endl;
        }

        suco::ClientConfig config = suco::ClientConfig::load_or_default();
        uint16_t web_port = 9001; 
        
        std::cout << "Sende Cache-Clear-Anfrage an den Coordinator bei " << config.coordinator_host << ":" << web_port << "..." << std::endl;
        std::string err_msg;
        std::string response = fetch_from_coordinator(config.coordinator_host, web_port, "/api/cache/clear", err_msg);
        
        if (response.empty()) {
            std::cerr << "suco cache clear error: " << err_msg << "\n"
                      << "Stelle sicher, dass der suco-coordinator läuft und erreichbar ist." << std::endl;
            return 1;
        }

        std::cout << "SUCO-Cache erfolgreich geleert (Lokal, Coordinator und Worker)." << std::endl;
        return 0;
    }
};

// Subcommand: suco doctor [--json]
// Comprehensive grid health check with 10 diagnostic checks.
class DoctorCommand : public Subcommand {
public:
    std::string name() const override { return "doctor"; }
    std::string description() const override { return "Grid health check — diagnoses connectivity, workers, toolchain, cache, daemon"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        (void)bin_dir;
        bool json_mode = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--json") json_mode = true;
        }

        suco::ClientConfig config = suco::ClientConfig::load_or_default();
        uint16_t web_port = 9001;

        struct CheckResult {
            std::string name;
            std::string status; // "ok", "warn", "fail"
            std::string message;
            int duration_ms = 0;
        };
        std::vector<CheckResult> results;
        int errors = 0, warnings = 0;

        auto add_ok = [&](const std::string& n, const std::string& msg, int ms = 0) {
            results.push_back({n, "ok", msg, ms});
        };
        auto add_warn = [&](const std::string& n, const std::string& msg) {
            results.push_back({n, "warn", msg, 0});
            warnings++;
        };
        auto add_fail = [&](const std::string& n, const std::string& msg) {
            results.push_back({n, "fail", msg, 0});
            errors++;
        };

        // Helper: TCP probe with latency
        auto tcp_probe = [](const std::string& host, uint16_t port, int timeout_ms) -> int {
            SocketInit init;
            socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock == INVALID_SOCKET_VAL) return -1;

#ifdef _WIN32
            DWORD tv = static_cast<DWORD>(timeout_ms);
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
#else
            struct timeval tv{};
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

            struct sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            struct hostent* he = gethostbyname(host.c_str());
            if (!he) { close_socket(sock); return -1; }
            std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

            auto start = std::chrono::steady_clock::now();
            int rc = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
            auto end = std::chrono::steady_clock::now();
            close_socket(sock);

            if (rc != 0) return -1;
            return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
        };

        // Helper: PACKET_HELLO handshake
        auto hello_probe = [](const std::string& host, uint16_t port) -> int {
            SocketInit init;
            socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock == INVALID_SOCKET_VAL) return -1;

#ifdef _WIN32
            DWORD tv = 3000;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&tv, sizeof(tv));
#else
            struct timeval tv{};
            tv.tv_sec = 3;
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

            struct sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            struct hostent* he = gethostbyname(host.c_str());
            if (!he) { close_socket(sock); return -1; }
            std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
                close_socket(sock);
                return -1;
            }

            // Send PACKET_HELLO (type=18) + version 200
            uint32_t pkt_type = htonl(18);
            uint32_t version = htonl(200);
            if (send(sock, (const char*)&pkt_type, 4, 0) != 4 ||
                send(sock, (const char*)&version, 4, 0) != 4) {
                close_socket(sock);
                return -1;
            }

            uint32_t resp_type = 0, resp_version = 0;
            if (recv(sock, (char*)&resp_type, 4, MSG_WAITALL) != 4 ||
                recv(sock, (char*)&resp_version, 4, MSG_WAITALL) != 4) {
                close_socket(sock);
                return -1;
            }
            close_socket(sock);

            resp_type = ntohl(resp_type);
            resp_version = ntohl(resp_version);
            if (resp_type != 18) return -1;
            return static_cast<int>(resp_version);
        };

        // Helper: run shell command and capture output
        auto run_cmd = [](const std::string& cmd) -> std::string {
            std::string result;
#ifdef _WIN32
            FILE* pipe = _popen(cmd.c_str(), "r");
#else
            FILE* pipe = popen(cmd.c_str(), "r");
#endif
            if (!pipe) return {};
            char buf[256];
            while (fgets(buf, sizeof(buf), pipe)) result += buf;
#ifdef _WIN32
            _pclose(pipe);
#else
            pclose(pipe);
#endif
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            return result;
        };

        // =====================================================================
        // CHECK 1: Environment overrides
        // =====================================================================
        {
            static const char* env_vars[] = {
                "SUCO_COORDINATOR_HOST", "SUCO_COORDINATOR_PORT",
                "SUCO_CACHE_DIR", "SUCO_TIMEOUT_MS", "SUCO_CONN_TIMEOUT_MS",
                "SUCO_MAX_SLOTS", "SUCO_LOG_LEVEL",
                "SUCO_PIPELINE_AGGRESSIVENESS", "SUCO_MAX_INFLIGHT_BATCHES",
                "SUCO_HEADER_CACHE_ENABLED", "SUCO_LOCAL_PREP_CACHE_ENABLED",
                "SUCO_HEADER_CACHE_DIR", "SUCO_HEADER_CACHE_MAX_SIZE_GB",
                "SUCO_COMPRESSION", "SUCO_COMPRESSION_LEVEL",
                "SUCO_PATH_NORMALIZATION",
                "SUCO_NO_DAEMON", "SUCO_DAEMON",
                "SUCO_IGNORE_VERSION", "SUCO_FORCE_PROTOCOL_VERSION",
                "SUCO_SUMMARY", "SUCO_LOCAL_SLOTS",
                nullptr
            };
            int count = 0;
            std::string details;
            for (int i = 0; env_vars[i]; ++i) {
                const char* val = std::getenv(env_vars[i]);
                if (val) {
                    if (!details.empty()) details += ", ";
                    details += std::string(env_vars[i]) + "=" + val;
                    count++;
                }
            }
            if (count == 0)
                add_ok("env", "No SUCO_* overrides set (using defaults)");
            else
                add_ok("env", std::to_string(count) + " SUCO_* override(s) active: " + details);
        }

        // =====================================================================
        // CHECK 2: Config file
        // =====================================================================
        {
            std::string path = suco::ClientConfig::get_default_config_path();
            std::error_code ec;
            if (std::filesystem::exists(path, ec)) {
                auto sz = std::filesystem::file_size(path, ec);
                add_ok("config", "Config file: " + path + " (" + std::to_string(sz) + " bytes)");
            } else {
                add_ok("config", "No config file at " + path + " (using defaults + env)");
            }
        }

        // =====================================================================
        // CHECK 3: Coordinator TCP
        // =====================================================================
        int coord_latency = tcp_probe(config.coordinator_host, config.coordinator_port, 2000);
        if (coord_latency >= 0) {
            add_ok("coordinator", "Coordinator reachable at " + config.coordinator_host + ":" +
                std::to_string(config.coordinator_port) + " (" + std::to_string(coord_latency) + "ms)",
                coord_latency);
        } else {
            add_fail("coordinator", "Coordinator unreachable at " + config.coordinator_host + ":" +
                std::to_string(config.coordinator_port) +
                " — Fix: start suco-coordinator, or set SUCO_COORDINATOR_HOST/PORT");
        }

        // =====================================================================
        // CHECK 4: Dashboard TCP
        // =====================================================================
        int dash_latency = tcp_probe(config.coordinator_host, web_port, 2000);
        if (dash_latency >= 0) {
            add_ok("dashboard", "Dashboard reachable at " + config.coordinator_host + ":" +
                std::to_string(web_port) + " (" + std::to_string(dash_latency) + "ms)", dash_latency);
        } else {
            add_warn("dashboard", "Dashboard unreachable at " + config.coordinator_host + ":" +
                std::to_string(web_port) + " (non-critical, compilation works without it)");
        }

        // =====================================================================
        // CHECK 5: Protocol version (PACKET_HELLO handshake)
        // =====================================================================
        if (coord_latency >= 0) {
            int coord_ver = hello_probe(config.coordinator_host, config.coordinator_port);
            if (coord_ver < 0) {
                add_fail("protocol", "Protocol handshake failed (coordinator not responding)");
            } else if (coord_ver == 200) {
                add_ok("protocol", "Protocol version match: client=200, coordinator=" + std::to_string(coord_ver));
            } else {
                add_fail("protocol", "Protocol version MISMATCH: client=200, coordinator=" +
                    std::to_string(coord_ver) + " — Fix: update to same SUCO version");
            }
        } else {
            add_fail("protocol", "Protocol handshake skipped (coordinator unreachable)");
        }

        // =====================================================================
        // CHECK 6: Workers + Slots (reuses fetch_stats_from_coordinator + parse_workers)
        // =====================================================================
        if (coord_latency >= 0) {
            std::string err_msg;
            std::string json = fetch_stats_from_coordinator(config.coordinator_host, web_port, err_msg);
            if (!json.empty()) {
                auto workers = parse_workers(json);
                int total_slots = 0, used_slots = 0;
                std::string worker_list;
                for (const auto& w : workers) {
                    total_slots += w.slots_total;
                    used_slots += w.slots_used;
                    if (!worker_list.empty()) worker_list += ", ";
                    worker_list += w.name + " (" + std::to_string(w.slots_used) + "/" +
                        std::to_string(w.slots_total) + " slots)";
                }
                if (workers.empty()) {
                    add_warn("workers", "No workers registered at coordinator (grid has no capacity)");
                } else {
                    add_ok("workers", std::to_string(workers.size()) + " worker(s), " +
                        std::to_string(total_slots) + " total slots (" +
                        std::to_string(total_slots - used_slots) + " free): " + worker_list);
                }
            } else {
                add_warn("workers", "Could not query workers: " + err_msg);
            }
        } else {
            add_fail("workers", "Worker check skipped (coordinator unreachable)");
        }

        // =====================================================================
        // CHECK 7: Toolchain (compilers in PATH)
        // =====================================================================
        {
            std::string gpp = run_cmd("g++ --version 2>/dev/null | head -1");
            std::string clang = run_cmd("clang++ --version 2>/dev/null | head -1");
            std::string toolchain_msg;
            bool found = false;

            if (!gpp.empty()) {
                toolchain_msg += "g++: " + gpp;
                found = true;
            }
            if (!clang.empty()) {
                if (!toolchain_msg.empty()) toolchain_msg += "; ";
                toolchain_msg += "clang++: " + clang;
                found = true;
            }
#ifdef _WIN32
            std::string cl_ver = run_cmd("cl 2>&1 | head -1");
            if (!cl_ver.empty() && cl_ver.find("Microsoft") != std::string::npos) {
                if (!toolchain_msg.empty()) toolchain_msg += "; ";
                toolchain_msg += "cl: " + cl_ver;
                found = true;
            }
#endif
            if (found) {
                add_ok("toolchain", toolchain_msg);
            } else {
                add_fail("toolchain", "No compiler found in PATH (g++, clang++, cl.exe)");
            }

            // Compare with grid workers if available
            if (coord_latency >= 0) {
                std::string err_msg;
                std::string json = fetch_stats_from_coordinator(config.coordinator_host, web_port, err_msg);
                if (!json.empty()) {
                    auto workers = parse_workers(json);
                    for (const auto& w : workers) {
                        if (w.compilers.empty() || w.compilers == "-") continue;
                        // Show worker compiler for comparison
                        add_ok("toolchain_grid", "Worker " + w.name + ": " + w.compilers);
                    }
                }
            }
        }

        // =====================================================================
        // CHECK 8: Cache directory
        // =====================================================================
        {
            std::string cache_dir = config.cache_directory;
            if (cache_dir.empty()) {
                add_warn("cache", "Cache directory not configured");
            } else if (!std::filesystem::exists(cache_dir)) {
                add_ok("cache", "Cache dir " + cache_dir + " (will be created on first use)");
            } else {
#ifdef _WIN32
                bool writable = (_access(cache_dir.c_str(), 2) == 0);
#else
                bool writable = (access(cache_dir.c_str(), W_OK) == 0);
#endif
                if (!writable) {
                    add_fail("cache", "Cache dir NOT writable: " + cache_dir);
                } else {
                    std::string info = "Cache dir writable: " + cache_dir;
                    try {
                        auto space = std::filesystem::space(cache_dir);
                        double avail_gb = static_cast<double>(space.available) / (1024.0 * 1024.0 * 1024.0);
                        info += " (" + std::to_string(static_cast<int>(avail_gb)) + " GB free)";
                        if (avail_gb < 1.0) {
                            add_warn("cache", info + " — less than 1 GB free!");
                        } else {
                            add_ok("cache", info);
                        }
                    } catch (...) {
                        add_ok("cache", info);
                    }
                }
            }
        }

        // =====================================================================
        // CHECK 9: zstd compression (real roundtrip)
        // =====================================================================
        {
            if (config.compression_enabled) {
                // Real roundtrip: compress → decompress → memcmp
                const std::string probe = "SUCO doctor zstd roundtrip verification probe";
                std::string compressed = suco::compress_zstd(probe, config.compression_level);
                if (compressed.empty()) {
                    add_fail("compression", "zstd compress failed (level=" +
                        std::to_string(config.compression_level) + ")");
                } else {
                    std::string decompressed = suco::decompress_zstd(compressed);
                    if (decompressed == probe) {
                        int ratio = static_cast<int>(
                            (static_cast<double>(compressed.size()) / probe.size() - 1.0) * 100);
                        add_ok("compression", "zstd roundtrip OK (level=" +
                            std::to_string(config.compression_level) +
                            ", ratio: " + std::to_string(ratio) + "%)");
                    } else {
                        add_fail("compression", "zstd roundtrip MISMATCH — decompress did not reproduce input");
                    }
                }
            } else {
                add_warn("compression", "Compression disabled (SUCO_COMPRESSION=off) — "
                    "network traffic will not be compressed");
            }
        }

        // =====================================================================
        // CHECK 10: Daemon health
        // =====================================================================
        {
            const char* no_daemon = std::getenv("SUCO_NO_DAEMON");
            if (no_daemon && (std::string(no_daemon) == "1" || std::string(no_daemon) == "true")) {
                add_ok("daemon", "Daemon disabled by SUCO_NO_DAEMON=1 (standalone mode)");
            } else {
                // Check if daemon socket exists
                std::string sock_path;
#ifdef _WIN32
                sock_path = "\\\\.\\pipe\\suco_daemon_pipe";
                HANDLE pipe = CreateFileA(sock_path.c_str(), GENERIC_READ, 0, nullptr,
                                          OPEN_EXISTING, 0, nullptr);
                if (pipe != INVALID_HANDLE_VALUE) {
                    CloseHandle(pipe);
                    add_ok("daemon", "Daemon active at " + sock_path);
                } else {
                    add_ok("daemon", "Daemon enabled (default), socket not yet created (starts on first build)");
                }
#else
                const char* xdg = std::getenv("XDG_RUNTIME_DIR");
                if (xdg && *xdg) {
                    sock_path = std::string(xdg) + "/suco/daemon.sock";
                } else {
                    sock_path = "/tmp/suco-" + std::to_string(getuid()) + "/daemon.sock";
                }
                if (std::filesystem::exists(sock_path)) {
                    add_ok("daemon", "Daemon socket active: " + sock_path);
                } else {
                    add_ok("daemon", "Daemon enabled (default), socket not yet created (starts on first build)");
                }
#endif
            }
        }

        // =====================================================================
        // Output
        // =====================================================================
        if (json_mode) {
            // JSON output: array of check objects
            std::cout << "[\n";
            for (size_t i = 0; i < results.size(); ++i) {
                const auto& r = results[i];
                std::cout << "  {\"name\":\"" << r.name << "\","
                          << "\"status\":\"" << r.status << "\","
                          << "\"message\":\"";
                // Escape quotes in message
                for (char c : r.message) {
                    if (c == '"') std::cout << "\\\"";
                    else if (c == '\\') std::cout << "\\\\";
                    else std::cout << c;
                }
                std::cout << "\",\"duration_ms\":" << r.duration_ms << "}";
                if (i + 1 < results.size()) std::cout << ",";
                std::cout << "\n";
            }
            std::cout << "]\n";
        } else {
            // Human-readable output
            std::cout << "\n  SUCO Doctor — Grid Health Check\n"
                      << "  ================================\n\n";
            for (const auto& r : results) {
                if (r.status == "ok")
                    std::cout << "  [\xe2\x9c\x93] " << r.message << "\n";
                else if (r.status == "warn")
                    std::cout << "  [!] " << r.message << "\n";
                else
                    std::cout << "  [\xe2\x9c\x97] " << r.message << "\n";
            }
            std::cout << "\n  ================================\n";
            if (errors == 0 && warnings == 0)
                std::cout << "  Result: ALL CHECKS PASSED\n\n";
            else if (errors == 0)
                std::cout << "  Result: PASSED with " << warnings << " warning(s)\n\n";
            else
                std::cout << "  Result: " << errors << " error(s), " << warnings << " warning(s)\n\n";
        }

        return errors > 0 ? 1 : 0;
    }
};

// Subcommand: suco config
class ConfigCommand : public Subcommand {
public:
    std::string name() const override { return "config"; }
    std::string description() const override { return "Verwaltet die SUCO CLI-Konfiguration (Nutzung: suco config show)"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        if (argc >= 3) {
            std::string sub = argv[2];
            if (sub != "show") {
                std::cerr << "suco config error: Unbekanntes Argument '" << sub << "'.\n"
                          << "Verwende: suco config show" << std::endl;
                return 1;
            }
        }

        suco::ClientConfig config = suco::ClientConfig::load_or_default();
        std::string cc_path = bin_dir + "suco-cl";
        std::string cxx_path = bin_dir + "suco-cl++";
#ifdef _WIN32
        cc_path += ".exe";
        cxx_path += ".exe";
#endif

        std::cout << "=================================================================" << std::endl;
        std::cout << "                  SUCO WRAPPER CONFIGURATION                     " << std::endl;
        std::cout << "=================================================================" << std::endl;
        std::cout << std::left << std::setw(25) << "SUCO-cl Path (CC):" << cc_path << std::endl;
        std::cout << std::left << std::setw(25) << "SUCO-cl++ Path (CXX):" << cxx_path << std::endl;
        std::cout << std::left << std::setw(25) << "Environment CC:" << get_env_var("CC") << std::endl;
        std::cout << std::left << std::setw(25) << "Environment CXX:" << get_env_var("CXX") << std::endl;
        std::cout << "-----------------------------------------------------------------" << std::endl;
        std::cout << std::left << std::setw(25) << "Coordinator Host:" << config.coordinator_host << std::endl;
        std::cout << std::left << std::setw(25) << "Coordinator Port:" << config.coordinator_port << std::endl;
        std::cout << std::left << std::setw(25) << "Cache Directory:" << config.cache_directory << std::endl;
        std::cout << std::left << std::setw(25) << "Max Local Slots:" << config.max_slots << std::endl;
        std::cout << std::left << std::setw(25) << "Connection Timeout:" << config.connection_timeout_ms << " ms" << std::endl;
        std::cout << std::left << std::setw(25) << "Response Timeout:" << config.timeout_ms << " ms" << std::endl;
        std::cout << "=================================================================" << std::endl;

        return 0;
    }
};

// Subcommand: suco setup
class SetupCommand : public Subcommand {
private:
    static std::string to_upper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
        return s;
    }

    static std::string trim_string(std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    }

    static std::string prompt(const std::string& question, const std::string& default_val) {
        std::cout << question << " [" << default_val << "]: ";
        std::string input;
        std::getline(std::cin, input);
        input = trim_string(input);
        if (input.empty()) {
            return default_val;
        }
        return input;
    }

public:
    std::string name() const override { return "setup"; }
    std::string description() const override { return "Unterstuetzt den Nutzer bei der initialen Einrichtung von SUCO"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        std::string config_path = suco::ClientConfig::get_default_config_path();
        bool file_exists = std::filesystem::exists(config_path);

        suco::ClientConfig config = suco::ClientConfig::load_or_default();

        if (file_exists) {
            std::cout << "Eine bestehende Konfigurationsdatei wurde gefunden unter:\n"
                      << "  " << config_path << "\n\n";
            std::cout << "Aktuelle Einstellungen:\n";
            std::cout << "  Coordinator Host: " << config.coordinator_host << "\n";
            std::cout << "  Coordinator Port: " << config.coordinator_port << "\n";
            std::cout << "  Lokale Slots:     " << config.max_slots << "\n";
            std::cout << "  Log-Level:        " << config.log_level << "\n\n";

            std::cout << "Moechtest du diese Konfiguration ueberschreiben? (j/N): ";
            std::string answer;
            std::getline(std::cin, answer);
            answer = to_upper(trim_string(answer));
            if (answer != "J" && answer != "JA" && answer != "Y" && answer != "YES") {
                std::cout << "Setup abgebrochen. Die Konfiguration wurde nicht geaendert." << std::endl;
                return 0;
            }
        }

        std::cout << "\n=================================================================\n";
        std::cout << "                     SUCO SETUP ASSISTANT                        \n";
        std::cout << "=================================================================\n";
        std::cout << "Bitte druecke Enter, um den Standardwert (in Klammern) zu uebernehmen.\n\n";

        // 1. Coordinator Host
        std::string host = prompt("Coordinator Host", config.coordinator_host);

        // 2. Coordinator Port
        int port_val = config.coordinator_port;
        while (true) {
            std::string port_str = prompt("Coordinator Port", std::to_string(port_val));
            try {
                int p = std::stoi(port_str);
                if (p > 0 && p <= 65535) {
                    port_val = p;
                    break;
                }
            } catch (...) {}
            std::cout << "Ungueltiger Port. Bitte gib eine Zahl zwischen 1 und 65535 ein.\n";
        }

        // 3. Parallele Slots
        unsigned int core_count = std::thread::hardware_concurrency();
        int default_slots = (core_count > 0) ? static_cast<int>(core_count) : config.max_slots;
        int slots_val = file_exists ? config.max_slots : default_slots;
        while (true) {
            std::string slots_str = prompt("Parallele Slots (fuer lokalen Fallback)", std::to_string(slots_val));
            try {
                int s = std::stoi(slots_str);
                if (s > 0 && s <= 128) {
                    slots_val = s;
                    break;
                }
            } catch (...) {}
            std::cout << "Ungueltige Slot-Anzahl. Bitte gib eine Zahl zwischen 1 und 128 ein.\n";
        }

        // 4. Log-Level
        std::string level_val = config.log_level;
        while (true) {
            std::string level_str = prompt("Log-Level (DEBUG, INFO, WARN, ERROR)", level_val);
            level_str = to_upper(trim_string(level_str));
            if (level_str == "DEBUG" || level_str == "INFO" || level_str == "WARN" || level_str == "ERROR") {
                level_val = level_str;
                break;
            }
            std::cout << "Ungueltiges Log-Level. Erlaubt sind: DEBUG, INFO, WARN, ERROR.\n";
        }

        // Optional: Lokale Compiler-Erkennung
        std::cout << "\nSuche nach lokalen Compilern...\n";
        std::string detected_compiler = "";
#ifdef _WIN32
        // Windows: cl.exe
        detected_compiler = "cl.exe";
#else
        // Linux/macOS: Suche g++, clang++, gcc
        if (std::system("which g++ >/dev/null 2>&1") == 0) {
            detected_compiler = "g++";
        } else if (std::system("which clang++ >/dev/null 2>&1") == 0) {
            detected_compiler = "clang++";
        } else if (std::system("which gcc >/dev/null 2>&1") == 0) {
            detected_compiler = "gcc";
        }
#endif
        if (!detected_compiler.empty()) {
            std::cout << "[INFO] Lokaler Compiler erkannt: " << detected_compiler << "\n";
        } else {
            std::cout << "[INFO] Kein lokaler Compiler automatisch erkannt (Standard gcc/g++ wird verwendet).\n";
        }

        // Zuweisen und Speichern
        config.coordinator_host = host;
        config.coordinator_port = port_val;
        config.max_slots = slots_val;
        config.log_level = level_val;

        std::cout << "\nSpeichere Konfiguration unter " << config_path << "...\n";
        if (config.save_to_file(config_path)) {
            std::cout << "[ERFOLG] Konfiguration erfolgreich gespeichert!\n";
        } else {
            std::cerr << "[FEHLER] Konfiguration konnte nicht gespeichert werden. Bitte Berechtigungen pruefen.\n";
            return 1;
        }

        // 5. Optionaler Verbindungstest
        std::cout << "\nMoechtest du jetzt einen Verbindungstest zum Coordinator durchfuehren? (J/n): ";
        std::string test_ans;
        std::getline(std::cin, test_ans);
        test_ans = to_upper(trim_string(test_ans));
        if (test_ans.empty() || test_ans == "J" || test_ans == "JA" || test_ans == "Y" || test_ans == "YES") {
            uint16_t rest_port = port_val + 1;
            std::cout << "Teste Verbindung zu http://" << host << ":" << rest_port << " (Dashboard API)...\n";
            std::string err_msg;
            std::string stats = fetch_stats_from_coordinator(host, rest_port, err_msg);
            if (err_msg.empty()) {
                std::cout << "\n[ERFOLG] Verbindungstest erfolgreich! Der Coordinator ist online.\n";
                // Kurze Info aus stats extrahieren
                std::string active_jobs = find_json_value(stats, "active_jobs");
                std::cout << "  Verbundene Worker: " << parse_workers(stats).size() << "\n";
                std::cout << "  Aktive Jobs im Grid: " << (active_jobs.empty() ? "0" : active_jobs) << "\n";
            } else {
                std::cout << "\n[WARNUNG] Der Coordinator unter " << host << ":" << rest_port << " konnte nicht erreicht werden.\n";
                std::cout << "  Fehlerursache: " << err_msg << "\n";
                std::cout << "  Bitte stelle sicher, dass der suco-coordinator laeuft.\n";
            }
        }

        std::cout << "\n=================================================================\n";
        std::cout << "                   SETUP ERFOLGREICH BEENDET                     \n";
        std::cout << "=================================================================\n";

        return 0;
    }
};

// Subcommand: suco install
class InstallCommand : public Subcommand {
private:
    static std::string to_upper(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
        return s;
    }

    static std::string trim_string(std::string s) {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    }

    static std::string prompt(const std::string& question, const std::string& default_val) {
        std::cout << question << " [" << default_val << "]: ";
        std::string input;
        std::getline(std::cin, input);
        input = trim_string(input);
        if (input.empty()) {
            return default_val;
        }
        return input;
    }

    static std::string get_user_home() {
#ifdef _WIN32
        const char* profile = std::getenv("USERPROFILE");
        return profile ? std::string(profile) : ".";
#else
        const char* sudo_user = std::getenv("SUDO_USER");
        if (sudo_user) {
            const char* home = std::getenv("HOME");
            if (home && std::string(home) != "/root") {
                return home;
            }
            return "/home/" + std::string(sudo_user);
        }
        const char* home = std::getenv("HOME");
        return home ? std::string(home) : "/root";
#endif
    }

    static bool is_root() {
#ifdef _WIN32
        return true;
#else
        return getuid() == 0;
#endif
    }

    static bool copy_executable(const std::filesystem::path& src, const std::filesystem::path& dest, bool overwrite) {
        try {
            if (std::filesystem::exists(dest) && !overwrite) {
                return true;
            }
            std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);
#ifndef _WIN32
            std::filesystem::permissions(dest, 
                std::filesystem::perms::owner_all | 
                std::filesystem::perms::group_read | std::filesystem::perms::group_exec | 
                std::filesystem::perms::others_read | std::filesystem::perms::others_exec
            );
#endif
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[FEHLER] Kopieren von " << src.filename() << " fehlgeschlagen: " << e.what() << "\n";
            return false;
        }
    }

public:
    std::string name() const override { return "install"; }
    std::string description() const override { return "Installiert SUCO systemweit und konfiguriert optional Systemd-Dienste"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
#ifdef _WIN32
        std::cout << "suco install ist aktuell nur auf Linux-Systemen verfuegbar.\n"
                  << "Bitte verwende die install.ps1 fuer die Installation unter Windows.\n";
        return 0;
#else
        bool root_access = is_root();
        if (!root_access) {
            std::cout << "\n=================================================================\n";
            std::cout << "⚠️  WARNUNG: Keine Root-Rechte (sudo) erkannt!\n";
            std::cout << "SUCO kann nicht systemweit nach /usr/local/bin kopiert werden und\n";
            std::cout << "Systemd-Dienste koennen nicht direkt geschrieben werden.\n";
            std::cout << "Bitte starte die Installation mit: sudo suco install\n";
            std::cout << "=================================================================\n\n";
        }

        std::string target_dir_default = root_access ? "/usr/local/bin" : "/tmp/suco-bin";
        std::string target_dir = prompt("Zielverzeichnis fuer die Binaries", target_dir_default);

        std::filesystem::path p_suco = std::filesystem::path(target_dir) / "suco";
        std::filesystem::path p_cl = std::filesystem::path(target_dir) / "suco-cl";
        std::filesystem::path p_cxx = std::filesystem::path(target_dir) / "suco-cl++";
        
        bool existing_found = std::filesystem::exists(p_suco) || 
                               std::filesystem::exists(p_cl) || 
                               std::filesystem::exists(p_cxx);

        bool overwrite = true;
        if (existing_found) {
            std::cout << "\nEine bestehende Installation wurde im Zielverzeichnis erkannt:\n"
                      << "  " << target_dir << "\n\n";
            std::string ans = prompt("Moechtest du die bestehenden Binaries ueberschreiben? (J/n)", "J");
            ans = to_upper(trim_string(ans));
            if (ans != "J" && ans != "JA" && ans != "Y" && ans != "YES") {
                overwrite = false;
                std::cout << "[INFO] Bestehende Binaries werden beibehalten.\n";
            }
        }

        std::string start_coord = prompt("suco-coordinator als Systemd-Dienst einrichten? (j/N)", "N");
        start_coord = to_upper(trim_string(start_coord));
        bool setup_coordinator = (start_coord == "J" || start_coord == "JA" || start_coord == "Y");

        std::string start_worker = prompt("suco-worker als Systemd-Dienst einrichten? (j/N)", "N");
        start_worker = to_upper(trim_string(start_worker));
        bool setup_worker = (start_worker == "J" || start_worker == "JA" || start_worker == "Y");

        int worker_slots = 4;
        if (setup_worker) {
            unsigned int cores = std::thread::hardware_concurrency();
            std::string slots_default = cores > 0 ? std::to_string(cores) : "4";
            while (true) {
                std::string slots_str = prompt("Anzahl Slots fuer den Worker-Dienst", slots_default);
                try {
                    int s = std::stoi(slots_str);
                    if (s > 0 && s <= 128) {
                        worker_slots = s;
                        break;
                    }
                } catch (...) {}
                std::cout << "Ungueltige Slot-Anzahl. Bitte eine Zahl zwischen 1 und 128 eingeben.\n";
            }
        }

        std::cout << "\nStarte Installationsvorgang...\n";

        try {
            std::filesystem::create_directories(target_dir);
            if (root_access) {
                std::filesystem::create_directories("/etc/suco");
            }
            std::string user_home = get_user_home();
            std::filesystem::create_directories(user_home + "/.suco");
            std::cout << "[ERFOLG] Verzeichnisse erstellt.\n";
        } catch (const std::exception& e) {
            std::cerr << "[FEHLER] Fehler beim Erstellen der Verzeichnisse: " << e.what() << "\n";
            if (!root_access) {
                std::cerr << "[TIPP] Bitte starte den Befehl mit 'sudo' fuer Schreibrechte im System.\n";
            }
            return 1;
        }

        std::filesystem::path src_dir(bin_dir);
        bool copy_ok = true;
        copy_ok &= copy_executable(src_dir / "suco", p_suco, overwrite);
        copy_ok &= copy_executable(src_dir / "suco-cl", p_cl, overwrite);
        copy_ok &= copy_executable(src_dir / "suco-cl++", p_cxx, overwrite);

        std::filesystem::path p_coord = std::filesystem::path(target_dir) / "suco-coordinator";
        std::filesystem::path p_work = std::filesystem::path(target_dir) / "suco-worker";
        
        if (std::filesystem::exists(src_dir / "suco-coordinator")) {
            copy_ok &= copy_executable(src_dir / "suco-coordinator", p_coord, overwrite);
        }
        if (std::filesystem::exists(src_dir / "suco-worker")) {
            copy_ok &= copy_executable(src_dir / "suco-worker", p_work, overwrite);
        }

        if (!copy_ok) {
            std::cerr << "[FEHLER] Installation der Binaries unvollstaendig.\n";
            return 1;
        }
        std::cout << "[ERFOLG] Binaries erfolgreich kopiert nach: " << target_dir << "\n";

        std::string coord_service = 
            "[Unit]\n"
            "Description=SUCO Compiler Grid Coordinator\n"
            "After=network.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            "User=root\n"
            "WorkingDirectory=/etc/suco\n"
            "ExecStart=" + p_coord.string() + "\n"
            "Restart=on-failure\n"
            "RestartSec=5\n\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n";

        std::string worker_service = 
            "[Unit]\n"
            "Description=SUCO Compiler Grid Worker\n"
            "After=network.target\n\n"
            "[Service]\n"
            "Type=simple\n"
            "User=root\n"
            "WorkingDirectory=/etc/suco\n"
            "ExecStart=" + p_work.string() + " --slots " + std::to_string(worker_slots) + "\n"
            "Restart=on-failure\n"
            "RestartSec=5\n\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n";

        if (setup_coordinator) {
            if (root_access) {
                std::string path = "/etc/systemd/system/suco-coordinator.service";
                std::ofstream out(path);
                if (out.is_open()) {
                    out << coord_service;
                    std::cout << "[ERFOLG] Systemd-Service geschrieben nach: " << path << "\n";
                } else {
                    std::cerr << "[FEHLER] Konnte Dienstdatei nicht schreiben: " << path << "\n";
                }
            } else {
                std::cout << "\n=== GENERIERTER INHALT FUER: /etc/systemd/system/suco-coordinator.service ===\n"
                          << coord_service
                          << "=============================================================================\n";
            }
        }

        if (setup_worker) {
            if (root_access) {
                std::string path = "/etc/systemd/system/suco-worker.service";
                std::ofstream out(path);
                if (out.is_open()) {
                    out << worker_service;
                    std::cout << "[ERFOLG] Systemd-Service geschrieben nach: " << path << "\n";
                } else {
                    std::cerr << "[FEHLER] Konnte Dienstdatei nicht schreiben: " << path << "\n";
                }
            } else {
                std::cout << "\n=== GENERIERTER INHALT FUER: /etc/systemd/system/suco-worker.service ===\n"
                          << worker_service
                          << "=========================================================================\n";
            }
        }

        std::cout << "\n=================================================================\n";
        std::cout << "                 INSTALLATION ERFOLGREICH BEENDET                \n";
        std::cout << "=================================================================\n";
        std::cout << "Binaries installiert unter:  " << target_dir << "\n";
        std::cout << "Systemweites Config-Dir:     /etc/suco\n";
        std::cout << "Nutzerspezifisches Daten-Dir: " << get_user_home() << "/.suco\n";
        
        if (setup_coordinator || setup_worker) {
            std::cout << "\nBefehle zur Aktivierung der Systemd-Dienste:\n";
            if (root_access) {
                std::cout << "  sudo systemctl daemon-reload\n";
                if (setup_coordinator) {
                    std::cout << "  sudo systemctl enable suco-coordinator.service --now\n";
                }
                if (setup_worker) {
                    std::cout << "  sudo systemctl enable suco-worker.service --now\n";
                }
            } else {
                std::cout << "  1. Erstelle die Dienstdateien unter /etc/systemd/system/ mit obigem Inhalt.\n";
                std::cout << "  2. Führe aus: sudo systemctl daemon-reload\n";
                if (setup_coordinator) {
                    std::cout << "  3. Führe aus: sudo systemctl enable suco-coordinator.service --now\n";
                }
                if (setup_worker) {
                    std::cout << "  3. Führe aus: sudo systemctl enable suco-worker.service --now\n";
                }
            }
        }
        std::cout << "=================================================================\n";

        return 0;
#endif
    }
};

// Subcommand: suco run — generic distributed task execution ("distribute any tool").
// Ships declared inputs to a grid worker, runs an arbitrary command there, and writes
// the declared outputs back. Falls back to local execution if no worker is reachable.
// Phase 1 prototype: worker chosen via --worker IP:PORT (auto-discovery + result caching
// via the coordinator are the next phases; design in docs/suco_run_design.md).
class RunCommand : public Subcommand {
public:
    std::string name() const override { return "run"; }
    std::string description() const override { return "Führt ein beliebiges Kommando verteilt auf einem Grid-Worker aus"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        std::vector<std::string> inputs, outputs, cmd;
        std::string worker_target;
        int i = 2; // argv[1] == "run"
        for (; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--") { ++i; break; }
            else if (a == "--in"     && i + 1 < argc) inputs.push_back(argv[++i]);
            else if (a == "--out"    && i + 1 < argc) outputs.push_back(argv[++i]);
            else if (a == "--worker" && i + 1 < argc) worker_target = argv[++i];
            else { std::cerr << "suco run: unbekanntes Argument '" << a << "'\n"; return 2; }
        }
        for (; i < argc; ++i) cmd.push_back(argv[i]);
        if (cmd.empty()) {
            std::cerr << "Usage: suco run [--in DATEI]... [--out DATEI]... [--worker IP:PORT] -- KOMMANDO ARGS\n";
            return 2;
        }
        // Shell-quote each token so the command reconstructed on the worker is
        // equivalent to the original argv (preserves spaces, redirects, quotes).
        auto shq = [](const std::string& s) {
            std::string r = "'";
            for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
            r += "'";
            return r;
        };
        std::string cmd_str;
        for (size_t k = 0; k < cmd.size(); ++k) { if (k) cmd_str += " "; cmd_str += shq(cmd[k]); }

        // --- A4: auto I/O tracking (no --in/--out needed; IncrediBuild-style) ---
        // If the user declared neither inputs nor outputs, reuse a learned manifest
        // for this command, or learn it once by running locally under the tracer.
        if (inputs.empty() && outputs.empty()) {
            std::string cdir = suco::ClientConfig::load_or_default().cache_directory;
            std::string mpath = manifest_path(cdir, cmd_str);
            if (load_manifest(mpath, inputs, outputs)) {
                std::cerr << "suco run: I/O aus Manifest (" << inputs.size() << " in, " << outputs.size() << " out).\n";
            } else {
                std::string trace_lib = find_trace_lib(bin_dir);
                if (trace_lib.empty()) {
                    std::cerr << "suco run: I/O-Tracer (libsuco-trace.so) nicht gefunden — führe lokal aus. "
                                 "Für verteilte Ausführung --in/--out angeben oder den Tracer installieren.\n";
                    return run_local(cmd_str);
                }
                std::cerr << "suco run: lerne I/O des Kommandos (einmalig, lokale Ausführung)…\n";
                int rc = run_local_traced(cmd_str, trace_lib, inputs, outputs);
                save_manifest(mpath, inputs, outputs);
                std::cerr << "suco run: gelernt — " << inputs.size() << " Eingabe(n), "
                          << outputs.size() << " Ausgabe(n) (Manifest gespeichert).\n";
                // Seed the result cache now that inputs are known.
                if (rc == 0) {
                    std::string th = compute_task_hash(cmd_str, inputs);
                    if (!th.empty()) {
                        std::string bundle = serialize_bundle(outputs, rc);
                        write_bundle_file(cdir + "/runcache/" + th + ".bundle", bundle);
                        socket_t c = coord_connect();
                        if (c != INVALID_SOCKET_VAL) { coord_blob_store(c, th, bundle); close_socket(c); }
                    }
                }
                return rc;
            }
        }

        // --- Content-addressed result cache (the "beyond IncrediBuild" part) ---
        // task_hash = sha256(command + each input's path+content hash). A cache hit
        // restores the declared outputs and skips execution entirely.
        std::string cache_dir = suco::ClientConfig::load_or_default().cache_directory;
        std::string task_hash = compute_task_hash(cmd_str, inputs);
        std::string bundle_path = cache_dir + "/runcache/" + task_hash + ".bundle";
        socket_t coord = INVALID_SOCKET_VAL;
        if (!task_hash.empty()) {
            // L1 — local cache (fast, no network).
            int cached_ec = 0;
            if (cache_get(bundle_path, cached_ec)) {
                std::cerr << "suco run: cache hit (lokal, " << task_hash.substr(0, 12) << ") — keine Ausführung.\n";
                return cached_ec;
            }
            // L2 — team-wide coordinator cache.
            coord = coord_connect();
            if (coord != INVALID_SOCKET_VAL) {
                std::string blob;
                if (coord_blob_query(coord, task_hash, blob)) {
                    int ec = 0;
                    if (apply_bundle(blob, ec)) {
                        write_bundle_file(bundle_path, blob);  // seed L1 for next time
                        std::cerr << "suco run: cache hit (team, " << task_hash.substr(0, 12) << ") — keine Ausführung.\n";
                        close_socket(coord);
                        return ec;
                    }
                }
            }
        }

        // Worker selection: explicit --worker, else auto-discover from the coordinator.
        if (worker_target.empty()) {
            suco::ClientConfig cfg = suco::ClientConfig::load_or_default();
            std::string err;
            std::string stats = fetch_stats_from_coordinator(cfg.coordinator_host, 9001, err);
            if (!stats.empty()) worker_target = discover_worker(stats);
        }
        std::string wip; int wport = 0;
        if (!worker_target.empty()) {
            auto c = worker_target.find(':');
            if (c == std::string::npos) { std::cerr << "suco run: --worker braucht IP:PORT\n"; return 2; }
            wip = worker_target.substr(0, c);
            try { wport = std::stoi(worker_target.substr(c + 1)); } catch (...) { wport = 0; }
        }
        int rc;
        if (wip.empty() || wport == 0) {
            std::cerr << "suco run: kein Worker angegeben — lokale Ausführung.\n";
            rc = run_local(cmd_str);
        } else {
            rc = run_remote(wip, wport, cmd_str, inputs, outputs);
            if (rc == kGridFailure) {
                std::cerr << "suco run: Grid nicht erreichbar — lokale Ausführung.\n";
                rc = run_local(cmd_str);
            }
        }
        // Cache only successful runs (a transient failure must not be served as success).
        if (rc == 0 && !task_hash.empty()) {
            std::string bundle = serialize_bundle(outputs, rc);
            write_bundle_file(bundle_path, bundle);                       // L1 local
            if (coord != INVALID_SOCKET_VAL) coord_blob_store(coord, task_hash, bundle);  // L2 team-wide
        }
        if (coord != INVALID_SOCKET_VAL) close_socket(coord);
        return rc;
    }

private:
    static constexpr int kGridFailure = -100000;

    static int run_local(const std::string& cmd_str) {
        int st = std::system(cmd_str.c_str());
#ifdef _WIN32
        return (st >= 0) ? st : 1;
#else
        return (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : 1;
#endif
    }

    // --- A4: auto I/O tracking (IncrediBuild-style transparency) ---
    // Locate the LD_PRELOAD tracer shipped next to the binary.
    static std::string find_trace_lib(const std::string& bin_dir) {
        if (const char* e = std::getenv("SUCO_TRACE_LIB")) {
            std::error_code ec; if (*e && std::filesystem::exists(e, ec)) return e;
        }
        std::vector<std::string> cand = {
            bin_dir + "libsuco-trace.so",
            bin_dir + "../lib/libsuco-trace.so",
            "/usr/local/lib/libsuco-trace.so",
            "/usr/lib/libsuco-trace.so",
        };
        for (auto& c : cand) {
            std::error_code ec;
            if (std::filesystem::exists(c, ec)) return std::filesystem::absolute(c, ec).string();
        }
        return {};
    }

    // Run the command locally under the tracer and classify accessed files into
    // inputs (read, pre-existing, not produced by the command) and outputs
    // (written). Only cwd-relative paths are kept — system libs, /tmp, the
    // compiler etc. are ignored. Paths are returned RELATIVE to cwd so a learned
    // manifest is portable across machines. Returns the command's exit code.
    static int run_local_traced(const std::string& cmd_str, const std::string& trace_lib,
                                std::vector<std::string>& inputs, std::vector<std::string>& outputs) {
        std::error_code ec;
        std::string cwd = std::filesystem::current_path(ec).string();
        if (!cwd.empty() && cwd.back() != '/') cwd += '/';
        std::string trace_file = "/tmp/suco_trace_" + std::to_string(::getpid()) + ".log";
        std::remove(trace_file.c_str());

        std::string full = "SUCO_TRACE_FILE='" + trace_file + "' LD_PRELOAD='" + trace_lib + "' " + cmd_str;
        int rc = run_local(full);

        std::set<std::string> reads, writes;
        std::ifstream tf(trace_file);
        std::string line;
        while (std::getline(tf, line)) {
            if (line.size() < 3 || line[1] != '\t') continue;
            char kind = line[0];
            std::string path = line.substr(2);
            // normalise ./ and keep only paths under cwd
            std::error_code nec;
            std::string abs = std::filesystem::weakly_canonical(path, nec).string();
            if (abs.empty()) abs = path;
            if (abs.rfind(cwd, 0) != 0) continue;           // outside build dir → ignore
            std::string rel = abs.substr(cwd.size());
            if (rel.empty()) continue;
            if (kind == 'W') writes.insert(rel); else reads.insert(rel);
        }
        std::remove(trace_file.c_str());

        for (const auto& r : reads) {
            if (writes.count(r)) continue;                  // produced intermediate, not an input
            std::error_code fe;
            if (std::filesystem::exists(r, fe)) inputs.push_back(r);
        }
        for (const auto& w : writes) outputs.push_back(w);
        std::sort(inputs.begin(), inputs.end());
        std::sort(outputs.begin(), outputs.end());
        return rc;
    }

    // Learned-I/O manifest: keyed only by the command string (inputs unknown up
    // front). Text: line 1 "I <rel>"…, then "O <rel>"…
    static std::string manifest_path(const std::string& cache_dir, const std::string& cmd_str) {
        return cache_dir + "/runmanifest/" + suco::compute_sha256("suco-run-io-v1\x1f" + cmd_str) + ".manifest";
    }
    static bool load_manifest(const std::string& path, std::vector<std::string>& in, std::vector<std::string>& out) {
        std::ifstream f(path);
        if (!f) return false;
        std::string line;
        while (std::getline(f, line)) {
            if (line.size() < 3) continue;
            if (line[0] == 'I') in.push_back(line.substr(2));
            else if (line[0] == 'O') out.push_back(line.substr(2));
        }
        return true;
    }
    static void save_manifest(const std::string& path, const std::vector<std::string>& in, const std::vector<std::string>& out) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::string tmp = path + ".tmp";
        { std::ofstream f(tmp);
          for (const auto& i : in) f << "I " << i << "\n";
          for (const auto& o : out) f << "O " << o << "\n"; }
        std::filesystem::rename(tmp, path, ec);
        if (ec) std::filesystem::remove(tmp, ec);
    }

    static bool read_file(const std::string& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return true;
    }

    int run_remote(const std::string& ip, int port, const std::string& cmd_str,
                   const std::vector<std::string>& inputs, const std::vector<std::string>& outputs) {
        socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET_VAL) return kGridFailure;
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0 ||
            ::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(sock); return kGridFailure;
        }
        if (!suco::tls::wrap_connect(sock)) { close_socket(sock); return kGridFailure; }
        auto send_str = [&](const std::string& s) -> bool {
            uint32_t l = htonl(static_cast<uint32_t>(s.size()));
            return suco::send_all(sock, &l, 4) && (s.empty() || suco::send_all(sock, s.data(), s.size()));
        };
        auto read_str = [&](std::string& out, uint32_t cap) -> bool {
            uint32_t l = 0; if (!suco::read_all(sock, &l, 4)) return false;
            uint32_t len = ntohl(l); if (len > cap) return false;
            out.resize(len); return len == 0 || suco::read_all(sock, out.data(), len);
        };

        // Respond to the worker's direct-listener auth challenge (if a secret is set).
        {
            std::string secret = suco::get_shared_secret();
            if (!secret.empty()) {
                uint32_t nlen_net = 0;
                if (!suco::read_all(sock, &nlen_net, 4)) { close_socket(sock); return kGridFailure; }
                uint32_t nlen = ntohl(nlen_net);
                if (nlen == 0 || nlen > 256) { close_socket(sock); return kGridFailure; }
                std::string nonce(nlen, '\0');
                if (!suco::read_all(sock, nonce.data(), nlen)) { close_socket(sock); return kGridFailure; }
                std::string mac = suco::hmac_sha256_hex(secret, nonce);
                uint32_t mlen_net = htonl(static_cast<uint32_t>(mac.size()));
                if (mac.empty() || !suco::send_all(sock, &mlen_net, 4) || !suco::send_all(sock, mac.data(), mac.size())) {
                    close_socket(sock); return kGridFailure;
                }
            }
        }

        uint32_t rt = htonl(suco::PACKET_RUN_REQ);
        uint32_t nin = htonl(static_cast<uint32_t>(inputs.size()));
        bool ok = suco::send_all(sock, &rt, 4) && send_str(cmd_str) && suco::send_all(sock, &nin, 4);
        for (const auto& in : inputs) {
            std::string content;
            if (!read_file(in, content)) { std::cerr << "suco run: Eingabedatei fehlt: " << in << "\n"; close_socket(sock); return kGridFailure; }
            ok = ok && send_str(in) && send_str(content);
        }
        uint32_t nout = htonl(static_cast<uint32_t>(outputs.size()));
        ok = ok && suco::send_all(sock, &nout, 4);
        for (const auto& o : outputs) ok = ok && send_str(o);
        if (!ok) { close_socket(sock); return kGridFailure; }

        uint32_t resp_type_net = 0;
        if (!suco::read_all(sock, &resp_type_net, 4) || ntohl(resp_type_net) != suco::PACKET_RUN_RESP) {
            close_socket(sock); return kGridFailure;
        }
        int32_t ec_net = 0;
        if (!suco::read_all(sock, &ec_net, 4)) { close_socket(sock); return kGridFailure; }
        int exit_code = static_cast<int32_t>(ntohl(ec_net));
        std::string log;
        if (!read_str(log, 512u << 20)) { close_socket(sock); return kGridFailure; }
        std::cerr << log;
        uint32_t no_net = 0;
        if (!suco::read_all(sock, &no_net, 4)) { close_socket(sock); return kGridFailure; }
        uint32_t no = ntohl(no_net);
        for (uint32_t k = 0; k < no; ++k) {
            std::string path, content;
            if (!read_str(path, 4096) || !read_str(content, 512u << 20)) { close_socket(sock); return kGridFailure; }
            std::filesystem::path outp(path);
            std::error_code ec;
            if (outp.has_parent_path()) std::filesystem::create_directories(outp.parent_path(), ec);
            std::ofstream f(path, std::ios::binary);
            f.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        close_socket(sock);
        return exit_code;
    }

    // task_hash = sha256(command + each sorted input's path + content-hash). Empty if
    // an input is missing (then the task is simply not cached).
    static std::string compute_task_hash(const std::string& cmd_str, std::vector<std::string> inputs) {
        std::sort(inputs.begin(), inputs.end());
        std::string key = "suco-run-v1\x1f" + cmd_str + "\x1f";
        for (const auto& in : inputs) {
            std::string content;
            if (!read_file(in, content)) return {};
            key += in + "\x1f" + suco::compute_sha256(content) + "\x1f";
        }
        return suco::compute_sha256(key);
    }

    // In-memory bundle = [exit_code][num_out]{[path_len][path][content_len][content]}...
    // Shared by the local file cache (L1) and the coordinator blob cache (L2).
    static std::string serialize_bundle(const std::vector<std::string>& outputs, int exit_code) {
        std::string b;
        auto w32 = [&](uint32_t v) { char t[4]; std::memcpy(t, &v, 4); b.append(t, 4); };
        w32(static_cast<uint32_t>(exit_code));
        w32(static_cast<uint32_t>(outputs.size()));
        for (const auto& o : outputs) {
            std::string content; read_file(o, content);
            w32(static_cast<uint32_t>(o.size()));       b += o;
            w32(static_cast<uint32_t>(content.size()));  b += content;
        }
        return b;
    }
    static bool apply_bundle(const std::string& b, int& exit_code) {
        size_t p = 0;
        auto r32 = [&](uint32_t& v) -> bool { if (p + 4 > b.size()) return false; std::memcpy(&v, b.data() + p, 4); p += 4; return true; };
        uint32_t ec = 0, n = 0;
        if (!r32(ec) || !r32(n) || n > 100000) return false;
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t plen = 0; if (!r32(plen) || plen > 65536 || p + plen > b.size()) return false;
            std::string path = b.substr(p, plen); p += plen;
            uint32_t clen = 0; if (!r32(clen) || p + clen > b.size()) return false;
            std::string content = b.substr(p, clen); p += clen;
            std::error_code fec;
            std::filesystem::path op(path);
            if (op.has_parent_path()) std::filesystem::create_directories(op.parent_path(), fec);
            std::ofstream of(path, std::ios::binary);
            of.write(content.data(), static_cast<std::streamsize>(content.size()));
        }
        exit_code = static_cast<int>(ec);
        return true;
    }

    static bool read_bundle_file(const std::string& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return true;
    }
    static void write_bundle_file(const std::string& path, const std::string& bundle) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::string tmp = path + ".tmp";
        { std::ofstream f(tmp, std::ios::binary); f.write(bundle.data(), static_cast<std::streamsize>(bundle.size())); }
        std::filesystem::rename(tmp, path, ec);
        if (ec) std::filesystem::remove(tmp, ec);
    }

    // L1 local cache (file). Returns false on miss.
    static bool cache_get(const std::string& bundle_path, int& exit_code) {
        std::string b;
        return read_bundle_file(bundle_path, b) && apply_bundle(b, exit_code);
    }

    // --- L2 team-wide cache via the coordinator (PACKET_BLOB_QUERY/STORE) ---
    // Connect + HELLO/version + optional HMAC auth. Returns a socket or -1.
    static socket_t coord_connect() {
        suco::ClientConfig cfg = suco::ClientConfig::load_or_default();
        socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET_VAL) return INVALID_SOCKET_VAL;
        struct sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(cfg.coordinator_port);
        struct hostent* he = gethostbyname(cfg.coordinator_host.c_str());
        if (!he) { close_socket(sock); return INVALID_SOCKET_VAL; }
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
        if (::connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) { close_socket(sock); return INVALID_SOCKET_VAL; }
        if (!suco::tls::wrap_connect(sock)) { close_socket(sock); return INVALID_SOCKET_VAL; }
        uint32_t t = htonl(suco::PACKET_HELLO), v = htonl(200);
        if (!suco::send_all(sock, &t, 4) || !suco::send_all(sock, &v, 4)) { close_socket(sock); return INVALID_SOCKET_VAL; }
        uint32_t rt = 0, rv = 0;
        if (!suco::read_all(sock, &rt, 4) || !suco::read_all(sock, &rv, 4) || ntohl(rt) != suco::PACKET_HELLO) { close_socket(sock); return INVALID_SOCKET_VAL; }
        std::string secret = suco::get_shared_secret();
        if (!secret.empty()) {
            uint32_t nl = 0; if (!suco::read_all(sock, &nl, 4)) { close_socket(sock); return INVALID_SOCKET_VAL; }
            nl = ntohl(nl); if (nl == 0 || nl > 256) { close_socket(sock); return INVALID_SOCKET_VAL; }
            std::string nonce(nl, '\0'); if (!suco::read_all(sock, nonce.data(), nl)) { close_socket(sock); return INVALID_SOCKET_VAL; }
            std::string mac = suco::hmac_sha256_hex(secret, nonce);
            uint32_t ml = htonl(static_cast<uint32_t>(mac.size()));
            if (!suco::send_all(sock, &ml, 4) || !suco::send_all(sock, mac.data(), mac.size())) { close_socket(sock); return INVALID_SOCKET_VAL; }
        }
        return sock;
    }
    static bool coord_blob_query(socket_t sock, const std::string& hash, std::string& blob_out) {
        uint32_t t = htonl(suco::PACKET_BLOB_QUERY), hl = htonl(static_cast<uint32_t>(hash.size()));
        if (!suco::send_all(sock, &t, 4) || !suco::send_all(sock, &hl, 4) || !suco::send_all(sock, hash.data(), hash.size())) return false;
        uint8_t hit = 0; if (!suco::read_all(sock, &hit, 1)) return false;
        if (!hit) return false;
        uint32_t bl = 0; if (!suco::read_all(sock, &bl, 4)) return false;
        bl = ntohl(bl); if (bl > (512u << 20)) return false;
        blob_out.resize(bl);
        return bl == 0 || suco::read_all(sock, blob_out.data(), bl);
    }
    static void coord_blob_store(socket_t sock, const std::string& hash, const std::string& blob) {
        uint32_t t = htonl(suco::PACKET_BLOB_STORE), hl = htonl(static_cast<uint32_t>(hash.size())), bl = htonl(static_cast<uint32_t>(blob.size()));
        if (suco::send_all(sock, &t, 4) && suco::send_all(sock, &hl, 4) && suco::send_all(sock, hash.data(), hash.size()) &&
            suco::send_all(sock, &bl, 4) && (blob.empty() || suco::send_all(sock, blob.data(), blob.size()))) {
            uint8_t ack = 0; suco::read_all(sock, &ack, 1);
        }
    }

    // Pick a usable worker (ip:direct_port) from an /api/stats JSON blob: first
    // non-loopback worker with a direct_port and a free slot. Returns "" if none.
    static std::string discover_worker(const std::string& json) {
        size_t pos = json.find("\"workers\"");
        if (pos == std::string::npos) return {};
        auto num_after = [&](const std::string& key, size_t from, size_t limit) -> long {
            size_t k = json.find(key, from);
            if (k == std::string::npos || (limit != std::string::npos && k > limit)) return -1;
            return std::atol(json.c_str() + k + key.size());
        };
        while (true) {
            size_t ipp = json.find("\"ip\":", pos);
            if (ipp == std::string::npos) break;
            size_t q1 = json.find('"', ipp + 5); if (q1 == std::string::npos) break;
            size_t q2 = json.find('"', q1 + 1); if (q2 == std::string::npos) break;
            std::string ip = json.substr(q1 + 1, q2 - q1 - 1);
            pos = q2;
            size_t next_ip = json.find("\"ip\":", q2);
            long dport = num_after("\"direct_port\":", q2, next_ip);
            long used  = num_after("\"slots_used\":", q2, next_ip);
            long total = num_after("\"slots_total\":", q2, next_ip);
            if (dport <= 0) continue;
            if (ip.rfind("127.", 0) == 0 || ip == "localhost") continue;   // unreachable from a remote client
            if (total > 0 && used >= total) continue;                       // no free slot
            return ip + ":" + std::to_string(dport);
        }
        return {};
    }

    static void cache_put(const std::string& bundle_path, const std::vector<std::string>& outputs, int exit_code) {
        write_bundle_file(bundle_path, serialize_bundle(outputs, exit_code));
    }
};

// --- A5: distributed, cached test execution ---
// `suco test` runs a whole test suite across the grid in parallel, each test via the
// `suco run` machinery (remote dispatch + A4 auto-I/O + team result cache). Unchanged
// tests with unchanged inputs are served from cache and never re-run — the big CI win
// IncrediBuild does not offer (it re-executes everything, every time).
class TestCommand : public Subcommand {
public:
    std::string name() const override { return "test"; }
    std::string description() const override { return "Führt eine Test-Suite verteilt + gecacht aus (unveränderte Tests laufen nicht neu)"; }

    int execute(int argc, char** argv, const std::string& bin_dir) override {
        std::string tests_file;
        int jobs = 0;
        std::vector<std::string> inline_cmd;
        int i = 2;
        for (; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--") { ++i; break; }
            else if (a == "--tests" && i + 1 < argc) tests_file = argv[++i];
            else if (a == "--jobs"  && i + 1 < argc) { try { jobs = std::stoi(argv[++i]); } catch (...) {} }
            else { std::cerr << "suco test: unbekanntes Argument '" << a << "'\n"; return 2; }
        }
        for (; i < argc; ++i) inline_cmd.push_back(argv[i]);

        // Collect the test command lines: from --tests FILE, an inline `-- cmd`, or stdin.
        std::vector<std::string> tests;
        if (!tests_file.empty()) {
            std::ifstream f(tests_file);
            if (!f) { std::cerr << "suco test: kann Test-Liste nicht lesen: " << tests_file << "\n"; return 2; }
            std::string line;
            while (std::getline(f, line)) {
                std::string t = trim(line);
                if (!t.empty() && t[0] != '#') tests.push_back(t);
            }
        } else if (!inline_cmd.empty()) {
            std::string one;
            for (size_t k = 0; k < inline_cmd.size(); ++k) { if (k) one += " "; one += inline_cmd[k]; }
            tests.push_back(one);
        } else {
            std::string line;
            while (std::getline(std::cin, line)) {
                std::string t = trim(line);
                if (!t.empty() && t[0] != '#') tests.push_back(t);
            }
        }
        if (tests.empty()) {
            std::cerr << "Usage: suco test [--tests DATEI] [--jobs N] [-- EIN_TEST]\n"
                         "  DATEI: ein Test-Kommando pro Zeile (# = Kommentar). Ohne DATEI/-- : stdin.\n";
            return 2;
        }

        if (jobs <= 0) jobs = grid_parallelism(bin_dir);
        if (jobs <= 0) jobs = 4;
        std::string suco_bin = bin_dir + "suco";

        std::cerr << "suco test: " << tests.size() << " Test(s), Parallelität " << jobs
                  << " (via suco run — verteilt + gecacht)\n";

        std::vector<Res> results(tests.size());
        std::atomic<size_t> next{0};
        auto worker = [&]() {
            for (;;) {
                size_t idx = next.fetch_add(1);
                if (idx >= tests.size()) break;
                results[idx] = run_one(suco_bin, tests[idx]);
            }
        };
        std::vector<std::thread> pool;
        int nthreads = std::min<int>(jobs, static_cast<int>(tests.size()));
        for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();

        int passed = 0, failed = 0, cached = 0;
        std::vector<size_t> failures;
        for (size_t k = 0; k < tests.size(); ++k) {
            if (results[k].cached) ++cached;
            if (results[k].ec == 0) ++passed; else { ++failed; failures.push_back(k); }
        }
        std::cerr << "\n=== Test-Ergebnis ===\n"
                  << "  bestanden: " << passed << " / " << tests.size()
                  << "  (davon aus Cache: " << cached << ")\n";
        if (failed) {
            std::cerr << "  FEHLGESCHLAGEN: " << failed << "\n";
            for (size_t k : failures) {
                std::cerr << "  ✗ [" << results[k].ec << "] " << tests[k] << "\n";
                std::string o = trim(results[k].out);
                if (!o.empty()) {
                    // last few lines of the failing test's output
                    std::cerr << indent_tail(o, 8) << "\n";
                }
            }
        }
        return failed ? 1 : 0;
    }

private:
    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }
    static std::string shq(const std::string& s) {
        std::string r = "'";
        for (char c : s) { if (c == '\'') r += "'\\''"; else r += c; }
        r += "'";
        return r;
    }
    static std::string indent_tail(const std::string& s, int max_lines) {
        std::vector<std::string> lines; std::string cur;
        for (char c : s) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur += c; }
        if (!cur.empty()) lines.push_back(cur);
        std::string r;
        size_t start = lines.size() > (size_t)max_lines ? lines.size() - max_lines : 0;
        for (size_t k = start; k < lines.size(); ++k) r += "      | " + lines[k] + "\n";
        return r;
    }
    struct Res { int ec; bool cached; std::string out; };
    static Res run_one(const std::string& suco_bin, const std::string& test_line) {
        // Wrap the whole test line as a single `sh -c` token so arbitrary shell
        // syntax works; suco run then dispatches + caches it (auto-I/O via A4).
        std::string full = suco_bin + " run -- sh -c " + shq(test_line) + " 2>&1";
        std::string out;
        FILE* p = popen(full.c_str(), "r");
        if (!p) return { 1, false, "popen failed" };
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        int st = pclose(p);
#ifdef _WIN32
        int ec = (st >= 0) ? st : 1;
#else
        int ec = (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : 1;
#endif
        bool cached = out.find("cache hit") != std::string::npos;
        return { ec, cached, out };
    }
    // Size the pool to the grid's total slots (fall back to local cores).
    static int grid_parallelism(const std::string& bin_dir) {
        (void)bin_dir;
        suco::ClientConfig cfg = suco::ClientConfig::load_or_default();
        std::string err;
        std::string stats = ::fetch_stats_from_coordinator(cfg.coordinator_host, 9001, err);
        int slots = 0;
        size_t pos = 0;
        while ((pos = stats.find("\"slots\":", pos)) != std::string::npos) {
            pos += 8;
            try { slots += std::stoi(stats.substr(pos, 8)); } catch (...) {}
        }
        if (slots > 0) return slots + 2;   // a little headroom over grid slots
        unsigned hc = std::thread::hardware_concurrency();
        return hc ? static_cast<int>(hc) : 4;
    }
};

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Automatische Erkennung und Einrichtung der MSVC-Umgebung
    suco::detect_and_setup_msvc();
#endif

    // Registrierung der Subkommandos initialisieren
    auto& registry = SubcommandRegistry::instance();
    registry.register_command(std::make_unique<StatusCommand>());
    registry.register_command(std::make_unique<DashboardCommand>());
    registry.register_command(std::make_unique<HelpCommand>());
    registry.register_command(std::make_unique<WorkersCommand>());
    registry.register_command(std::make_unique<ConfigCommand>());
    registry.register_command(std::make_unique<SetupCommand>());
    registry.register_command(std::make_unique<InstallCommand>());
    registry.register_command(std::make_unique<CacheClearCommand>());
    registry.register_command(std::make_unique<DoctorCommand>());
    registry.register_command(std::make_unique<RunCommand>());
    registry.register_command(std::make_unique<TestCommand>());

    std::string bin_dir = get_binary_directory(argv[0]);

    if (argc < 2) {
        HelpCommand help;
        help.execute(argc, argv, bin_dir);
        return 1;
    }

    std::string first_arg = argv[1];

    // Prüfen, ob es sich um ein Subkommando handelt
    if (registry.has_command(first_arg)) {
        return registry.execute(first_arg, argc, argv, bin_dir);
    }
    
    // Fallback-Prüfung für alternative Hilfe-Argumente
    if (first_arg == "-h" || first_arg == "--help" || first_arg == "help") {
        return registry.execute("help", argc, argv, bin_dir);
    }

    // Version — intercept before the build-command fallback so `suco --version`
    // is not mistaken for a build tool to wrap.
    if (first_arg == "--version" || first_arg == "-V" || first_arg == "version") {
        return suco::print_version("suco");
    }

    // Wenn es kein Subkommando ist, interpretieren wir es als Build-Befehl (z.B. make, ninja)
    std::string cc_path = bin_dir + "suco-cl";
    std::string cxx_path = bin_dir + "suco-cl++";
#ifdef _WIN32
    cc_path += ".exe";
    cxx_path += ".exe";
#endif

    std::string orig_cc = get_env_var("CC");
    std::string orig_cxx = get_env_var("CXX");

    if (orig_cc.empty()) {
#ifdef _WIN32
        orig_cc = "cl.exe";
#else
        orig_cc = "gcc";
#endif
    }
    if (orig_cxx.empty()) {
#ifdef _WIN32
        orig_cxx = "cl.exe";
#else
        orig_cxx = "g++";
#endif
    }

    set_env_var("CC", cc_path);
    set_env_var("CXX", cxx_path);
    set_env_var("SUCO_REAL_CC", orig_cc);
    set_env_var("SUCO_REAL_CXX", orig_cxx);

    std::vector<char*> child_args;
    for (int i = 1; i < argc; ++i) {
        child_args.push_back(argv[i]);
    }
    child_args.push_back(nullptr);

#ifdef _WIN32
    int exit_code = static_cast<int>(_spawnvp(_P_WAIT, argv[1], const_cast<char* const*>(child_args.data())));
    if (exit_code < 0) {
        std::cerr << "suco error: Failed to execute command '" << argv[1] << "' (Befehl nicht gefunden).\n\n";
        registry.print_commands();
        return 127;
    }
    if (exit_code == 0) {
        mark_compile_commands(bin_dir);
    }
    return exit_code;
#else
    int exit_code = 0;
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[1], child_args.data());
        std::cerr << "suco error: Failed to execute command '" << argv[1] << "': " << std::strerror(errno) << " (Befehl nicht gefunden).\n\n";
        std::exit(127);
    } else if (pid > 0) {
        int status = 0;
        waitpid(pid, &status, 0);
        exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    } else {
        std::cerr << "suco error: fork failed\n";
        return 1;
    }
    
    if (exit_code == 0) {
        mark_compile_commands(bin_dir);
    }
    return exit_code;
#endif
}
