#pragma once

#include <string>
#include <stdint.h>

namespace suco::worker {

class Config {
public:
    std::string coordinator_host;
    uint16_t coordinator_port = 9000;
    int slots = 0; // 0 bedeutet: Automatisch (Anzahl logischer CPU-Kerne)
    int job_timeout = 120; // Standardmäßig 120 Sekunden Timeout pro Job

    /**
     * @brief Parst die Kommandozeilenargumente.
     * @param argc Anzahl der Argumente
     * @param argv Array der Argumente
     * @return Das initialisierte Config-Objekt
     */
    static Config parse(int argc, char** argv);
};

} // namespace suco::worker
