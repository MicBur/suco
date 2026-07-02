#include "worker.h"
#include "socket_util.h"
#include "logging.h"
#include <iostream>

int main(int argc, char* argv[]) {
    try {
        // Initialisiert Winsock auf Windows-Plattformen automatisch (RAII)
        suco::SocketInit sock_init; 
        
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
