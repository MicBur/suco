#include "compiler_command.h"
#include "suco_client.h"
#include "logging.h"
#include "utils.h"
#include "protocol.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

namespace {

// Opens the web-based monitoring dashboard in the default web browser
void open_dashboard(const suco::ClientConfig& config) {
    std::string url = std::format("http://{}:{}", config.coordinator_host, suco::DEFAULT_WEB_PORT);
    SUCO_LOG_INFO("Opening SUCO Grid Monitor Dashboard at {} ...", url);

    std::string cmd;
#ifdef _WIN32
    cmd = "start " + url;
#else
    // Detect Windows Subsystem for Linux (WSL)
    std::ifstream version_file("/proc/version");
    std::string version_str;
    bool is_wsl = false;
    if (version_file.is_open() && std::getline(version_file, version_str)) {
        if (version_str.find("Microsoft") != std::string::npos || version_str.find("microsoft") != std::string::npos) {
            is_wsl = true;
        }
    }

    if (is_wsl) {
        cmd = "cmd.exe /C start " + url + " 2>/dev/null";
    } else {
        cmd = "xdg-open " + url + " 2>/dev/null || open " + url + " 2>/dev/null";
    }
#endif
    int r = std::system(cmd.c_str());
    (void)r;
}

} // namespace

int main(int argc, char** argv) {
    try {
        suco::SocketInit sock_init; // Automatically initializes Winsock on Windows platforms

        CompilerCommand command = CompilerCommand::parse(argc, argv);
        suco::ClientConfig config = suco::ClientConfig::load_or_default();

        // If the user requested the dashboard monitoring interface
        if (command.is_monitor_request) {
            open_dashboard(config);
            return 0;
        }

        SUCO_LOG_INFO("SUCO Client started");

        suco::SucoClient client(config);
        return client.run(command);

    } catch (const std::exception& e) {
        SUCO_LOG_ERROR("Fatal error occurred in SUCO client: {}", e.what());
        return 1;
    }
}
