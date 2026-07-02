#include "client_config.h"

#include <cstdlib>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace suco {
namespace {

// Hilfsfunktion zum Setzen von Umgebungsvariablen
void set_env_var(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

// Safely retrieves a string environment variable, falling back to a default value
std::string get_env_string(const char* name, const std::string& default_val) {
    const char* val = std::getenv(name);
    return val ? std::string(val) : default_val;
}

// Safely retrieves and parses an integer environment variable
int get_env_int(const char* name, int default_val) {
    const char* val = std::getenv(name);
    if (!val) {
        return default_val;
    }
    try {
        return std::stoi(val);
    } catch (...) {
        return default_val;
    }
}

// Trim-Hilfsfunktion für Konfigurationsdateien
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Determines the default cache directory in a platform-independent way
std::string get_default_cache_directory() {
#ifdef _WIN32
    // Windows: Try %LOCALAPPDATA%\suco first, then %USERPROFILE%\.cache\suco
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data) {
        return std::string(local_app_data) + "\\suco";
    }
    const char* user_profile = std::getenv("USERPROFILE");
    if (user_profile) {
        return std::string(user_profile) + "\\.cache\\suco";
    }
    return "C:\\Temp\\suco-cache";
#else
    // Linux/macOS: Try ~/.cache/suco first, then /tmp/suco-cache
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.cache/suco";
    }
    return "/tmp/suco-cache";
#endif
}

} // namespace

std::string ClientConfig::get_default_config_path() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        return std::string(appdata) + "\\suco\\suco.conf";
    }
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        return std::string(userprofile) + "\\.suco.conf";
    }
    return "suco.conf";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.suco.conf";
    }
    return ".suco.conf";
#endif
}

bool ClientConfig::save_to_file(const std::string& filepath) const {
    try {
        std::filesystem::path p(filepath);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream out(filepath);
        if (!out.is_open()) {
            return false;
        }
        out << "coordinator_host=" << coordinator_host << "\n";
        out << "coordinator_port=" << coordinator_port << "\n";
        out << "max_slots=" << max_slots << "\n";
        out << "log_level=" << log_level << "\n";
        out << "cache_dir=" << cache_directory << "\n";
        out << "timeout_ms=" << timeout_ms << "\n";
        out << "connection_timeout_ms=" << connection_timeout_ms << "\n";
        return true;
    } catch (...) {
        return false;
    }
}

ClientConfig ClientConfig::load_or_default() {
    ClientConfig config;

    // 1. Zuerst Defaults setzen
    config.coordinator_host = "127.0.0.1";
    config.coordinator_port = 9000;
    config.cache_directory = get_default_cache_directory();
    config.timeout_ms = 5000;
    config.connection_timeout_ms = 100;
    config.max_slots = 4;
    config.log_level = "INFO";

    // 2. Aus Datei laden, falls vorhanden
    std::string config_path = get_default_config_path();
    if (std::filesystem::exists(config_path)) {
        std::ifstream in(config_path);
        if (in.is_open()) {
            std::string line;
            while (std::getline(in, line)) {
                line = trim(line);
                if (line.empty() || line[0] == '#' || line[0] == ';') {
                    continue;
                }
                size_t pos = line.find('=');
                if (pos == std::string::npos) {
                    continue;
                }
                std::string key = trim(line.substr(0, pos));
                std::string val = trim(line.substr(pos + 1));
                if (key.empty() || val.empty()) {
                    continue;
                }

                try {
                    if (key == "coordinator_host") {
                        config.coordinator_host = val;
                    } else if (key == "coordinator_port") {
                        config.coordinator_port = static_cast<uint16_t>(std::stoi(val));
                    } else if (key == "max_slots") {
                        config.max_slots = std::stoi(val);
                    } else if (key == "log_level") {
                        config.log_level = val;
                    } else if (key == "cache_dir") {
                        config.cache_directory = val;
                    } else if (key == "timeout_ms") {
                        config.timeout_ms = std::stoi(val);
                    } else if (key == "connection_timeout_ms") {
                        config.connection_timeout_ms = std::stoi(val);
                    }
                } catch (...) {
                    // Ignoriere Parsing-Fehler einzelner Zeilen
                }
            }
        }
    }

    // 3. Umgebungsvariablen überschreiben die Werte (falls gesetzt)
    config.coordinator_host = get_env_string("SUCO_COORDINATOR_HOST", config.coordinator_host);
    config.coordinator_port = static_cast<uint16_t>(get_env_int("SUCO_COORDINATOR_PORT", config.coordinator_port));
    config.cache_directory = get_env_string("SUCO_CACHE_DIR", config.cache_directory);
    config.timeout_ms = get_env_int("SUCO_TIMEOUT_MS", config.timeout_ms);
    config.connection_timeout_ms = get_env_int("SUCO_CONN_TIMEOUT_MS", config.connection_timeout_ms);
    config.max_slots = get_env_int("SUCO_MAX_SLOTS", config.max_slots);
    config.log_level = get_env_string("SUCO_LOG_LEVEL", config.log_level);

    // 4. In Umgebungsvariablen exportieren für Kindprozesse und Bibliotheken
    set_env_var("SUCO_COORDINATOR_HOST", config.coordinator_host);
    set_env_var("SUCO_COORDINATOR_PORT", std::to_string(config.coordinator_port));
    set_env_var("SUCO_CACHE_DIR", config.cache_directory);
    set_env_var("SUCO_TIMEOUT_MS", std::to_string(config.timeout_ms));
    set_env_var("SUCO_CONN_TIMEOUT_MS", std::to_string(config.connection_timeout_ms));
    set_env_var("SUCO_MAX_SLOTS", std::to_string(config.max_slots));
    set_env_var("SUCO_LOG_LEVEL", config.log_level);

    return config;
}

} // namespace suco
