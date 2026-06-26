#include "client_config.h"

#include <cstdlib>
#include <string>

namespace suco {
namespace {

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

ClientConfig ClientConfig::load_or_default() {
    ClientConfig config;

    config.coordinator_host = get_env_string("SUCO_COORDINATOR_HOST", "127.0.0.1");
    config.coordinator_port = static_cast<uint16_t>(get_env_int("SUCO_COORDINATOR_PORT", 9000));
    config.cache_directory = get_env_string("SUCO_CACHE_DIR", get_default_cache_directory());
    
    config.timeout_ms = get_env_int("SUCO_TIMEOUT_MS", 5000);
    config.connection_timeout_ms = get_env_int("SUCO_CONN_TIMEOUT_MS", 100);
    config.max_slots = get_env_int("SUCO_MAX_SLOTS", 4);

    return config;
}

} // namespace suco
