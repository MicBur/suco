#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <thread>
#include <cctype>
#include <fstream>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
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
#include "../client/client_config.h"

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

// Stellt eine Verbindung zum REST-Port (9001) des Coordinators her und fragt /api/stats ab
std::string fetch_stats_from_coordinator(const std::string& host, uint16_t port, std::string& err_msg) {
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

    std::string req = "GET /api/stats HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
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
};

// Parsen der Worker aus dem REST JSON-Output
std::vector<WorkerStats> parse_workers(const std::string& json) {
    std::vector<WorkerStats> workers;
    size_t pos = json.find("\"workers\":");
    if (pos == std::string::npos) return workers;
    size_t array_start = json.find("[", pos);
    if (array_start == std::string::npos) return workers;
    
    // Suchen des Endes vor "recent_jobs", um Konflikte mit den cpu_cores_usage Brackets zu vermeiden
    size_t array_end = json.find("\"recent_jobs\"", array_start);
    if (array_end == std::string::npos) {
        array_end = json.size();
    }
    
    std::string array_content = json.substr(array_start, array_end - array_start);
    size_t obj_pos = 0;
    while (true) {
        size_t obj_start = array_content.find("{", obj_pos);
        if (obj_start == std::string::npos) break;
        size_t obj_end = array_content.find("}", obj_start);
        if (obj_end == std::string::npos) break;
        
        std::string obj = array_content.substr(obj_start, obj_end - obj_start);
        WorkerStats w;
        w.name = find_json_value(obj, "name");
        w.ip = find_json_value(obj, "ip");
        w.os = find_json_value(obj, "os");
        
        std::string st = find_json_value(obj, "slots_total");
        std::string su = find_json_value(obj, "slots_used");
        w.slots_total = st.empty() ? 0 : std::stoi(st);
        w.slots_used = su.empty() ? 0 : std::stoi(su);
        
        workers.push_back(w);
        obj_pos = obj_end + 1;
    }
    return workers;
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

        std::cout << "=================================================================" << std::endl;
        std::cout << "                     SUCO GRID STATUS                            " << std::endl;
        std::cout << "=================================================================" << std::endl;
        std::cout << "Coordinator:    " << config.coordinator_host << ":" << config.coordinator_port << " (Online)" << std::endl;
        std::cout << "Web-Dashboard:  http://" << config.coordinator_host << ":" << web_port << "/" << std::endl;
        std::cout << "Aktive Jobs:    " << (active_jobs.empty() ? "0" : active_jobs) << std::endl;
        std::cout << "Total Requests: " << (total_reqs.empty() ? "0" : total_reqs) 
                  << " (Hits: " << (cache_hits.empty() ? "0" : cache_hits) 
                  << ", Misses: " << (cache_misses.empty() ? "0" : cache_misses) << ")" << std::endl;
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
            std::cout << "Gesamte Slots im Grid: " << total_slots 
                      << " (Belegt: " << used_slots 
                      << ", Frei: " << (total_slots - used_slots) << ")" << std::endl;
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
        std::cout << "                     SUCO GRID WORKERS                           " << std::endl;
        std::cout << "=================================================================" << std::endl;
        std::cout << "Coordinator: " << config.coordinator_host << ":" << config.coordinator_port << " | Verbundene Worker: " << workers.size() << std::endl;
        std::cout << std::endl;

        if (workers.empty()) {
            std::cout << "Keine aktiven Worker am Coordinator registriert." << std::endl;
        } else {
            std::cout << "-----------------------------------------------------------------" << std::endl;
            std::cout << "| " << std::left << std::setw(15) << "Worker Name" 
                      << " | " << std::left << std::setw(15) << "IP-Adresse" 
                      << " | " << std::left << std::setw(10) << "OS" 
                      << " | " << std::left << std::setw(12) << "Slots (Belegt)" << " |" << std::endl;
            std::cout << "-----------------------------------------------------------------" << std::endl;
            int total_slots = 0;
            int used_slots = 0;
            for (const auto& w : workers) {
                std::string slots_str = std::to_string(w.slots_used) + " / " + std::to_string(w.slots_total);
                std::cout << "| " << std::left << std::setw(15) << w.name 
                          << " | " << std::left << std::setw(15) << w.ip 
                          << " | " << std::left << std::setw(10) << w.os 
                          << " | " << std::left << std::setw(12) << slots_str << " |" << std::endl;
                total_slots += w.slots_total;
                used_slots += w.slots_used;
            }
            std::cout << "-----------------------------------------------------------------" << std::endl;
            std::cout << "Gesamte Slots im Grid: " << total_slots 
                      << " (Belegt: " << used_slots 
                      << ", Frei: " << (total_slots - used_slots) << ")" << std::endl;
        }
        std::cout << "=================================================================" << std::endl;

        return 0;
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

int main(int argc, char* argv[]) {
    // Registrierung der Subkommandos initialisieren
    auto& registry = SubcommandRegistry::instance();
    registry.register_command(std::make_unique<StatusCommand>());
    registry.register_command(std::make_unique<DashboardCommand>());
    registry.register_command(std::make_unique<HelpCommand>());
    registry.register_command(std::make_unique<WorkersCommand>());
    registry.register_command(std::make_unique<ConfigCommand>());
    registry.register_command(std::make_unique<SetupCommand>());
    registry.register_command(std::make_unique<InstallCommand>());

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
    return exit_code;
#else
    execvp(argv[1], child_args.data());
    std::cerr << "suco error: Failed to execute command '" << argv[1] << "': " << std::strerror(errno) << " (Befehl nicht gefunden).\n\n";
    registry.print_commands();
    return 127;
#endif
}
