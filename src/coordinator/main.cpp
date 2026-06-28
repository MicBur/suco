#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <signal.h>
#include "config.h"
#include "coordinator.h"
#include "socket_util.h"

std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signum) {
    std::cout << "\nsuco-coordinator: Shutdown signal received (" << signum << ")..." << std::endl;
    g_shutdown_requested = true;
}

int main() {
    suco::SocketInit sock_init;
    
    // Register signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::cout << "suco-coordinator: Loading configuration..." << std::endl;
    suco::CoordinatorConfig config = suco::CoordinatorConfig::load();
    
    std::cout << "suco-coordinator: Initializing orchestrator..." << std::endl;
    suco::Coordinator coordinator(config);
    coordinator.start();

    std::cout << "suco-coordinator: Startup complete. Running on port " 
              << config.get_coordinator_port() << ". Press Ctrl+C to terminate." << std::endl;

    // Wait until shutdown is triggered
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "suco-coordinator: Initiating graceful shutdown..." << std::endl;
    coordinator.stop();
    
    std::cout << "suco-coordinator: Shutdown complete. Exiting." << std::endl;
    return 0;
}
