#include "local_compiler.h"
#include "utils.h"
#include "logging.h"

#include <iostream>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#endif

namespace suco {

int LocalCompiler::compile(const CompilerCommand& cmd) {
    if (cmd.raw_args.empty()) {
        SUCO_LOG_ERROR("Local compilation failed: Empty argument list");
        return -1;
    }

    SUCO_LOG_INFO("Executing local fallback compilation for: {}", cmd.source_file);

    // Execute the local compiler using the raw command line arguments.
    // cmd.raw_args contains the compiler path followed by all compiler options.
    auto [exit_code, output] = run_and_capture(cmd.raw_args);

    // Forward the captured output to stderr so warning/error highlighting remains intact.
    if (!output.empty()) {
        std::cerr << output << std::flush;
    }

    if (exit_code != 0) {
        SUCO_LOG_WARNING("Local compilation completed with exit code {}", exit_code);
    } else {
        SUCO_LOG_INFO("Local compilation succeeded");
    }

    return exit_code;
}

int LocalCompiler::execute_direct(const std::vector<std::string>& args) {
    if (args.empty()) {
        return -1;
    }
#ifdef _WIN32
    std::vector<const char*> c_args;
    for (const auto& arg : args) {
        c_args.push_back(arg.c_str());
    }
    c_args.push_back(nullptr);
    return static_cast<int>(_spawnvp(_P_WAIT, args[0].c_str(), const_cast<char* const*>(c_args.data())));
#else
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> c_args;
        for (const auto& arg : args) {
            c_args.push_back(const_cast<char*>(arg.c_str()));
        }
        c_args.push_back(nullptr);
        execvp(args[0].c_str(), c_args.data());
        std::exit(127);
    } else if (pid < 0) {
        return -1;
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return status;
    }
#endif
}

std::pair<int, std::string> LocalCompiler::run_and_capture(const std::vector<std::string>& args) {
    return suco::run_local_capture(args);
}

} // namespace suco
