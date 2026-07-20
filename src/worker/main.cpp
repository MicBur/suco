#include "worker.h"
#include "socket_util.h"
#include "logging.h"
#include "msvc_detector.h"
#include "suco_version.h"
#include <cstring>
#include <iostream>

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) return suco::print_version("suco-worker");
    }
    try {
        // Initialisiert Winsock auf Windows-Plattformen automatisch (RAII)
        suco::SocketInit sock_init; 
        
#ifdef _WIN32
        // Automatische Erkennung und Einrichtung der MSVC-Umgebung
        if (suco::detect_and_setup_msvc()) {
            SUCO_LOG_INFO("suco-worker: MSVC environment successfully initialized.");
        } else {
            SUCO_LOG_WARNING("suco-worker: No active MSVC environment detected and auto-setup failed.");
        }
#endif

        // Parst die Kommandozeilenargumente
        auto config = suco::worker::Config::parse(argc, argv);
        
        // Erstellt und startet den Worker
        suco::worker::Worker worker(config);
        return worker.run();
        
    } catch (const std::exception& e) {
        SUCO_LOG_ERROR("suco-worker fatal error: {}", e.what());
        return 1;
    }
}
