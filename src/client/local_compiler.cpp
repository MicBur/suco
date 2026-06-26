#include "local_compiler.h"
#include "utils.h"
#include "logging.h"

#include <iostream>

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

std::pair<int, std::string> LocalCompiler::run_and_capture(const std::vector<std::string>& args) {
    return suco::run_local_capture(args);
}

} // namespace suco
