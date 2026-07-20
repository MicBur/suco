#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <signal.h>
#include "config.h"
#include "coordinator.h"
#include "socket_util.h"
#include "logging.h"
#include "suco_version.h"
#include <cstring>

std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int signum) {
    SUCO_LOG_INFO("Shutdown signal received ({})...", signum);
    g_shutdown_requested = true;
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) return suco::print_version("suco-coordinator");
    }
    suco::SocketInit sock_init;
    
    // Register signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    SUCO_LOG_INFO("Loading configuration...");
    suco::CoordinatorConfig config = suco::CoordinatorConfig::load();
    
    SUCO_LOG_INFO("Initializing orchestrator...");
    suco::Coordinator coordinator(config);
    coordinator.start();

    SUCO_LOG_INFO("Startup complete. Running on port {}. Press Ctrl+C to terminate.", config.get_coordinator_port());

    // Wait until shutdown is triggered
    while (!g_shutdown_requested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    SUCO_LOG_INFO("Initiating graceful shutdown...");
    coordinator.stop();
    
    SUCO_LOG_INFO("Shutdown complete. Exiting.");
    return 0;
}
