#pragma once

#include <string>
#include <stdint.h>

namespace suco {

struct ClientConfig {
    std::string coordinator_host;
    uint16_t coordinator_port = 9000;
    int connection_timeout_ms = 100;

    static ClientConfig load_or_default();
};

} // namespace suco
