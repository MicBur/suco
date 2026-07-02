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
        }
    }

    return config;
}

} // namespace suco::worker
