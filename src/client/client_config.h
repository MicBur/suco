#pragma once

#include <string>
#include <stdint.h>
#include <map>

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
    int connection_timeout_ms = 3000; // Strict connect timeout for resilient fallback
    int grid_timeout_ms = 4000;       // Grid timeout to fallback/race slow helper nodes
    int max_slots = 4;               // Maximum parallel jobs for local compilation fallback
    std::string log_level = "INFO";
    std::string pipeline_aggressiveness = "medium";
    size_t max_inflight_batches = 4;
    bool header_cache_enabled = true;
    bool local_prep_cache_enabled = true;
    std::string header_cache_directory;
    int header_cache_max_size_gb = 8;
    bool compression_enabled = true;
    int compression_level = 1;
    bool path_normalization = true;

    /**
     * @brief Loads configuration from environment variables or returns defaults.
     */
    static ClientConfig load_or_default();
    static ClientConfig load_or_default(const std::map<std::string, std::string>& env_overrides);

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
