#pragma once

#include <string>
#include <stdint.h>

namespace suco {

/**
 * @brief Holds configuration options for the SUCO client.
 * 
 * Automatically loads configurations from environment variables
 * and falls back to platform-dependent defaults.
 */
struct ClientConfig {
    std::string coordinator_host;
    uint16_t coordinator_port = 9000;
    std::string cache_directory;
    int timeout_ms = 5000;
    int connection_timeout_ms = 100; // Strict connect timeout for resilient fallback
    int max_slots = 4;               // Maximum parallel jobs for local compilation fallback
    std::string log_level = "INFO";

    /**
     * @brief Loads configuration from environment variables or returns defaults.
     */
    static ClientConfig load_or_default();

    /**
     * @brief Saves the current configuration to the specified config file.
     */
    bool save_to_file(const std::string& filepath) const;

    /**
     * @brief Determines the default platform-dependent configuration file path.
     */
    static std::string get_default_config_path();
};

} // namespace suco
