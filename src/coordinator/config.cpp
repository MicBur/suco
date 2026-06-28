#include "config.h"
#include <cstdlib>
#include <sstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace suco {

static std::string resolve_path(std::string path) {
    if (path.empty()) {
        return "";
    }

#ifdef _WIN32
    // Windows Environment Variable resolution (%LOCALAPPDATA% etc.)
    char buffer[MAX_PATH];
    DWORD resolved_len = ExpandEnvironmentStringsA(path.c_str(), buffer, MAX_PATH);
    if (resolved_len > 0 && resolved_len <= MAX_PATH) {
        path = std::string(buffer);
    }
#else
    // Linux Tilde resolution
    if (path.starts_with("~")) {
        const char* home = std::getenv("HOME");
        if (home) {
            path = std::string(home) + path.substr(1);
        }
    }
#endif
    return path;
}

CoordinatorConfig CoordinatorConfig::load() {
    CoordinatorConfig config;

    // 1. Port loading (support both SUCO_PORT and SUCO_COORDINATOR_PORT)
    const char* env_port = std::getenv("SUCO_PORT");
    if (!env_port) {
        env_port = std::getenv("SUCO_COORDINATOR_PORT");
    }
    if (env_port) {
        try {
            config.set_coordinator_port(static_cast<uint16_t>(std::stoi(env_port)));
        } catch (...) {}
    }

    // 2. Heartbeat interval loading
    if (const char* env_hb = std::getenv("SUCO_HEARTBEAT_INTERVAL_MS")) {
        try {
            config.set_heartbeat_interval_ms(static_cast<uint32_t>(std::stoul(env_hb)));
        } catch (...) {}
    }

    // 3. Worker Timeout loading
    if (const char* env_timeout = std::getenv("SUCO_WORKER_TIMEOUT_MS")) {
        try {
            config.set_worker_timeout_ms(static_cast<uint32_t>(std::stoul(env_timeout)));
        } catch (...) {}
    }

    // 4. Job Timeout loading
    if (const char* env_job_timeout = std::getenv("SUCO_JOB_TIMEOUT_MS")) {
        try {
            config.set_job_timeout_ms(static_cast<uint32_t>(std::stoul(env_job_timeout)));
        } catch (...) {}
    }

    // 5. Max Retries loading
    if (const char* env_retries = std::getenv("SUCO_MAX_RETRIES")) {
        try {
            config.set_max_retries_per_job(std::stoi(env_retries));
        } catch (...) {}
    }

    // 6. Worker Weights parsing (format: name:weight,name2:weight2)
    if (const char* env_weights = std::getenv("SUCO_WORKER_WEIGHTS")) {
        std::unordered_map<std::string, double> weights;
        std::string s(env_weights);
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
            size_t colon = item.find(':');
            if (colon != std::string::npos) {
                std::string name = item.substr(0, colon);
                std::string weight_str = item.substr(colon + 1);
                try {
                    weights[name] = std::stod(weight_str);
                } catch (...) {}
            }
        }
        config.set_worker_weights(weights);
    }

    // 7. Cache Directory loading & path expansion
    std::string cache_dir = "~/.cache/suco/";
#ifdef _WIN32
    cache_dir = "%LOCALAPPDATA%\\suco\\cache\\";
#endif

    if (const char* env_cache_dir = std::getenv("SUCO_CACHE_DIR")) {
        cache_dir = env_cache_dir;
    }

    config.set_cache_directory(resolve_path(cache_dir));

    return config;
}

} // namespace suco
