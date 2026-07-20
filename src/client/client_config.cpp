#include "client_config.h"

#include <cstdlib>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#endif

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

std::string get_default_header_cache_directory() {
#ifdef _WIN32
    const char* user_profile = std::getenv("USERPROFILE");
    if (user_profile) {
        return std::string(user_profile) + "\\.cache\\suco\\headers";
    }
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data) {
        return std::string(local_app_data) + "\\suco\\headers";
    }
    return "C:\\Temp\\suco-headers";
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.cache/suco/headers";
    }
    return "/tmp/suco-headers";
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
    // 1. Systemweite Konfiguration prüfen
    if (std::filesystem::exists("/etc/suco/suco.conf")) {
        return "/etc/suco/suco.conf";
    }

    // 2. HOME-Umgebungsvariable prüfen
    const char* home = std::getenv("HOME");
    if (home) {
        return std::string(home) + "/.suco.conf";
    }

    // 3. Fallback über getpwuid(getuid()) bei gelöschter Umgebung (z.B. unter SCons)
    struct passwd* pw = getpwuid(getuid());
    if (pw && pw->pw_dir) {
        return std::string(pw->pw_dir) + "/.suco.conf";
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
        out << "pipeline_aggressiveness=" << pipeline_aggressiveness << "\n";
        out << "max_inflight_batches=" << max_inflight_batches << "\n";
        out << "header_cache_enabled=" << (header_cache_enabled ? "1" : "0") << "\n";
        out << "local_prep_cache_enabled=" << (local_prep_cache_enabled ? "1" : "0") << "\n";
        out << "header_cache_dir=" << header_cache_directory << "\n";
        out << "header_cache_max_size_gb=" << header_cache_max_size_gb << "\n";
        out << "compression_enabled=" << (compression_enabled ? "1" : "0") << "\n";
        out << "compression_level=" << compression_level << "\n";
        out << "path_normalization=" << (path_normalization ? "1" : "0") << "\n";
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
    config.connection_timeout_ms = 3000;
    config.grid_timeout_ms = 4000;
    config.max_slots = 4;
    config.log_level = "INFO";
    config.pipeline_aggressiveness = "medium";
    config.max_inflight_batches = 4;
    config.header_cache_enabled = true;
    config.local_prep_cache_enabled = true;
    config.header_cache_directory = get_default_header_cache_directory();
    config.header_cache_max_size_gb = 8;
    config.path_normalization = true;

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
                    } else if (key == "pipeline_aggressiveness") {
                        config.pipeline_aggressiveness = val;
                    } else if (key == "max_inflight_batches") {
                        config.max_inflight_batches = std::stoul(val);
                    } else if (key == "header_cache_enabled") {
                        config.header_cache_enabled = (val == "1" || val == "true");
                    } else if (key == "local_prep_cache_enabled") {
                        config.local_prep_cache_enabled = (val == "1" || val == "true");
                    } else if (key == "header_cache_dir") {
                        config.header_cache_directory = val;
                    } else if (key == "header_cache_max_size_gb") {
                        config.header_cache_max_size_gb = std::stoi(val);
                    } else if (key == "compression_enabled") {
                        config.compression_enabled = (val == "1" || val == "true");
                    } else if (key == "compression_level") {
                        config.compression_level = std::stoi(val);
                    } else if (key == "path_normalization") {
                        config.path_normalization = (val == "1" || val == "true");
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
    config.grid_timeout_ms = get_env_int("SUCO_GRID_TIMEOUT_MS", config.grid_timeout_ms);
    config.max_slots = get_env_int("SUCO_MAX_SLOTS", config.max_slots);
    config.log_level = get_env_string("SUCO_LOG_LEVEL", config.log_level);
    config.pipeline_aggressiveness = get_env_string("SUCO_PIPELINE_AGGRESSIVENESS", config.pipeline_aggressiveness);
    config.max_inflight_batches = static_cast<size_t>(get_env_int("SUCO_MAX_INFLIGHT_BATCHES", static_cast<int>(config.max_inflight_batches)));
    
    const char* hc_enabled_env = std::getenv("SUCO_HEADER_CACHE_ENABLED");
    if (hc_enabled_env) {
        config.header_cache_enabled = (std::string(hc_enabled_env) == "1" || std::string(hc_enabled_env) == "true");
    }
    const char* lpc_enabled_env = std::getenv("SUCO_LOCAL_PREP_CACHE_ENABLED");
    if (lpc_enabled_env) {
        config.local_prep_cache_enabled = (std::string(lpc_enabled_env) == "1" || std::string(lpc_enabled_env) == "true");
    }
    config.header_cache_directory = get_env_string("SUCO_HEADER_CACHE_DIR", config.header_cache_directory);
    config.header_cache_max_size_gb = get_env_int("SUCO_HEADER_CACHE_MAX_SIZE_GB", config.header_cache_max_size_gb);
    
    const char* comp_env = std::getenv("SUCO_COMPRESSION");
    if (comp_env) {
        std::string s_comp(comp_env);
        config.compression_enabled = (s_comp != "off" && s_comp != "OFF" && s_comp != "false" && s_comp != "0");
    }
    config.compression_level = get_env_int("SUCO_COMPRESSION_LEVEL", config.compression_level);
    
    const char* path_norm_env = std::getenv("SUCO_PATH_NORMALIZATION");
    if (path_norm_env) {
        config.path_normalization = (std::string(path_norm_env) == "1" || std::string(path_norm_env) == "true");
    }

    // 4. In Umgebungsvariablen exportieren für Kindprozesse und Bibliotheken
    set_env_var("SUCO_COORDINATOR_HOST", config.coordinator_host);
    set_env_var("SUCO_COORDINATOR_PORT", std::to_string(config.coordinator_port));
    set_env_var("SUCO_CACHE_DIR", config.cache_directory);
    set_env_var("SUCO_TIMEOUT_MS", std::to_string(config.timeout_ms));
    set_env_var("SUCO_CONN_TIMEOUT_MS", std::to_string(config.connection_timeout_ms));
    set_env_var("SUCO_MAX_SLOTS", std::to_string(config.max_slots));
    set_env_var("SUCO_LOG_LEVEL", config.log_level);
    set_env_var("SUCO_PIPELINE_AGGRESSIVENESS", config.pipeline_aggressiveness);
    set_env_var("SUCO_MAX_INFLIGHT_BATCHES", std::to_string(config.max_inflight_batches));
    set_env_var("SUCO_HEADER_CACHE_ENABLED", config.header_cache_enabled ? "1" : "0");
    set_env_var("SUCO_LOCAL_PREP_CACHE_ENABLED", config.local_prep_cache_enabled ? "1" : "0");
    set_env_var("SUCO_HEADER_CACHE_DIR", config.header_cache_directory);
    set_env_var("SUCO_HEADER_CACHE_MAX_SIZE_GB", std::to_string(config.header_cache_max_size_gb));
    set_env_var("SUCO_PATH_NORMALIZATION", config.path_normalization ? "1" : "0");

    return config;
}

ClientConfig ClientConfig::load_or_default(const std::map<std::string, std::string>& env_overrides) {
    ClientConfig config;

    // 1. Zuerst Defaults setzen
    config.coordinator_host = "127.0.0.1";
    config.coordinator_port = 9000;
    config.cache_directory = get_default_cache_directory();
    config.timeout_ms = 5000;
    config.connection_timeout_ms = 3000;
    config.grid_timeout_ms = 4000;
    config.max_slots = 4;
    config.log_level = "INFO";
    config.pipeline_aggressiveness = "medium";
    config.max_inflight_batches = 4;
    config.header_cache_enabled = true;
    config.local_prep_cache_enabled = true;
    config.header_cache_directory = get_default_header_cache_directory();
    config.header_cache_max_size_gb = 8;
    config.path_normalization = true;

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
                    } else if (key == "pipeline_aggressiveness") {
                        config.pipeline_aggressiveness = val;
                    } else if (key == "max_inflight_batches") {
                        config.max_inflight_batches = std::stoul(val);
                    } else if (key == "header_cache_enabled") {
                        config.header_cache_enabled = (val == "1" || val == "true");
                    } else if (key == "local_prep_cache_enabled") {
                        config.local_prep_cache_enabled = (val == "1" || val == "true");
                    } else if (key == "header_cache_dir") {
                        config.header_cache_directory = val;
                    } else if (key == "header_cache_max_size_gb") {
                        config.header_cache_max_size_gb = std::stoi(val);
                    } else if (key == "compression_enabled") {
                        config.compression_enabled = (val == "1" || val == "true");
                    } else if (key == "compression_level") {
                        config.compression_level = std::stoi(val);
                    } else if (key == "path_normalization") {
                        config.path_normalization = (val == "1" || val == "true");
                    }
                } catch (...) {
                }
            }
        }
    }

    // 3. Aus Overrides überschreiben
    auto get_override = [&](const char* name, const std::string& default_val) -> std::string {
        auto it = env_overrides.find(name);
        return (it != env_overrides.end()) ? it->second : default_val;
    };
    auto get_override_int = [&](const char* name, int default_val) -> int {
        auto it = env_overrides.find(name);
        if (it != env_overrides.end()) {
            try { return std::stoi(it->second); } catch (...) {}
        }
        return default_val;
    };

    config.coordinator_host = get_override("SUCO_COORDINATOR_HOST", config.coordinator_host);
    config.coordinator_port = static_cast<uint16_t>(get_override_int("SUCO_COORDINATOR_PORT", config.coordinator_port));
    config.cache_directory = get_override("SUCO_CACHE_DIR", config.cache_directory);
    config.timeout_ms = get_override_int("SUCO_TIMEOUT_MS", config.timeout_ms);
    config.connection_timeout_ms = get_override_int("SUCO_CONN_TIMEOUT_MS", config.connection_timeout_ms);
    config.grid_timeout_ms = get_override_int("SUCO_GRID_TIMEOUT_MS", config.grid_timeout_ms);
    config.max_slots = get_override_int("SUCO_MAX_SLOTS", config.max_slots);
    config.log_level = get_override("SUCO_LOG_LEVEL", config.log_level);
    config.pipeline_aggressiveness = get_override("SUCO_PIPELINE_AGGRESSIVENESS", config.pipeline_aggressiveness);
    config.max_inflight_batches = static_cast<size_t>(get_override_int("SUCO_MAX_INFLIGHT_BATCHES", static_cast<int>(config.max_inflight_batches)));
    
    auto it_hc = env_overrides.find("SUCO_HEADER_CACHE_ENABLED");
    if (it_hc != env_overrides.end()) {
        config.header_cache_enabled = (it_hc->second == "1" || it_hc->second == "true");
    }
    auto it_lpc = env_overrides.find("SUCO_LOCAL_PREP_CACHE_ENABLED");
    if (it_lpc != env_overrides.end()) {
        config.local_prep_cache_enabled = (it_lpc->second == "1" || it_lpc->second == "true");
    }
    config.header_cache_directory = get_override("SUCO_HEADER_CACHE_DIR", config.header_cache_directory);
    config.header_cache_max_size_gb = get_override_int("SUCO_HEADER_CACHE_MAX_SIZE_GB", config.header_cache_max_size_gb);
    
    auto it_comp = env_overrides.find("SUCO_COMPRESSION");
    if (it_comp != env_overrides.end()) {
        std::string s_comp = it_comp->second;
        config.compression_enabled = (s_comp != "off" && s_comp != "OFF" && s_comp != "false" && s_comp != "0");
    }
    config.compression_level = get_override_int("SUCO_COMPRESSION_LEVEL", config.compression_level);
    
    auto it_pn = env_overrides.find("SUCO_PATH_NORMALIZATION");
    if (it_pn != env_overrides.end()) {
        config.path_normalization = (it_pn->second == "1" || it_pn->second == "true");
    }

    return config;
}

} // namespace suco
