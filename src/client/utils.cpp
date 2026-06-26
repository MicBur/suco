#include "utils.h"

#include <iostream>
#include <cstdio>
#include <array>

#ifdef _WIN32
    #include <windows.h>
    #define popen _popen
    #define pclose _pclose
#else
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace suco {

std::pair<int, std::string> run_local_capture(const std::vector<std::string>& args) {
    if (args.empty()) {
        return { -1, "" };
    }

    // Reconstruct the shell command with proper argument quoting
    std::string cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) {
            cmd += " ";
        }
        const std::string& arg = args[i];
        
        // Quote arguments containing spaces or special characters
        if (arg.find(' ') != std::string::npos || arg.find('\"') != std::string::npos) {
            std::string escaped_arg;
            escaped_arg.reserve(arg.size() + 2);
            escaped_arg += '\"';
            for (char c : arg) {
                if (c == '\"') escaped_arg += "\\\"";
                else escaped_arg += c;
            }
            escaped_arg += '\"';
            cmd += escaped_arg;
        } else {
            cmd += arg;
        }
    }

    // Redirect stderr to stdout so we capture compilation logs/warnings as well
    cmd += " 2>&1";

    std::string output;
    std::array<char, 4096> buffer{};

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return { -1, "" };
    }

    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

    int status = pclose(pipe);
    int exit_code = -1;

#ifdef _WIN32
    exit_code = status;
#else
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else {
        exit_code = status;
    }
#endif

    return { exit_code, output };
}

} // namespace suco
