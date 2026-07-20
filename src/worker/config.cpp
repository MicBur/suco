#include "config.h"
#include "protocol.h"
#include <algorithm>
#include <iostream>

namespace suco::worker {

Config Config::parse(int argc, char** argv) {
    Config config;
    config.coordinator_port = suco::DEFAULT_PORT;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--coordinator" && i + 1 < argc) {
            std::string val = argv[++i];
            size_t colon = val.find(':');
            if (colon != std::string::npos) {
                config.coordinator_host = val.substr(0, colon);
                try {
                    config.coordinator_port = static_cast<uint16_t>(std::stoi(val.substr(colon + 1)));
                } catch (const std::exception& e) {
                    std::cerr << "suco-worker warning: Invalid port specified in --coordinator, using default: "
                              << e.what() << std::endl;
                    config.coordinator_port = suco::DEFAULT_PORT;
                }
            } else {
                config.coordinator_host = val;
            }
        } else if (arg == "--slots" && i + 1 < argc) {
            try {
                config.slots = std::max(1, std::stoi(argv[++i]));
            } catch (const std::exception& e) {
                std::cerr << "suco-worker warning: Invalid value for --slots, using auto-detection: "
                          << e.what() << std::endl;
                config.slots = 0;
            }
        } else if (arg == "--job-timeout" && i + 1 < argc) {
            try {
                config.job_timeout = std::max(1, std::stoi(argv[++i]));
            } catch (const std::exception& e) {
                std::cerr << "suco-worker warning: Invalid value for --job-timeout, using default 120s: "
                          << e.what() << std::endl;
                config.job_timeout = 120;
            }
        } else if (arg == "--direct-port" && i + 1 < argc) {
            try {
                config.direct_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } catch (const std::exception& e) {
                std::cerr << "suco-worker warning: Invalid value for --direct-port, using default 9005: "
                          << e.what() << std::endl;
                config.direct_port = 9005;
            }
        }
    }

    // Parse environment variables for header cache
    const char* hc_enabled_env = std::getenv("SUCO_HEADER_CACHE_ENABLED");
    config.header_cache_enabled = hc_enabled_env ? (std::string(hc_enabled_env) == "1" || std::string(hc_enabled_env) == "true") : true;

    const char* hc_dir_env = std::getenv("SUCO_HEADER_CACHE_DIR");
    if (hc_dir_env) {
        config.header_cache_dir = hc_dir_env;
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            config.header_cache_dir = std::string(home) + "/.cache/suco/headers";
        } else {
            config.header_cache_dir = "/tmp/.cache/suco/headers";
        }
    }

    const char* hc_size_env = std::getenv("SUCO_HEADER_CACHE_MAX_SIZE_GB");
    if (hc_size_env) {
        try {
            config.header_cache_max_size_gb = std::stoi(hc_size_env);
        } catch (...) {
            config.header_cache_max_size_gb = 8;
        }
    } else {
        config.header_cache_max_size_gb = 8;
    }

    return config;
}

} // namespace suco::worker
