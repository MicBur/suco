#include "suco_client.h"
#include "local_compiler.h"
#include "hash_util.h"
#include "logging.h"
#include "utils.h"

#include <iostream>
#include <vector>

namespace suco {

SucoClient::SucoClient(ClientConfig config)
    : config_(std::move(config)), network_(config_) {}

int SucoClient::run(const CompilerCommand& command) {
    // 1. Verify if this command is a standard compilation invocation (-c)
    if (!command.is_compilation_step()) {
        return run_local_fallback(command);
    }

    // 2. Fall back locally if precompiled headers (PCH) are active (not cached in Phase 1)
    if (command.is_precompiled_header) {
        SUCO_LOG_INFO("Precompiled headers (PCH) detected. Falling back to local compilation.");
        return run_local_fallback(command);
    }

    // Make a mutable copy of the command to populate preprocessed sources and hashes
    CompilerCommand cmd = command;

    // 3. Perform local pre-processing to resolve all macros and headers
    SUCO_LOG_INFO("Running local preprocessor for {}", cmd.source_file);
    std::vector<std::string> pp_args;
    pp_args.push_back(cmd.compiler_path);
    
    if (cmd.is_msvc) {
        pp_args.push_back("/E");
        pp_args.push_back("/nologo");
    } else {
        pp_args.push_back("-E");
    }

    // Pass defines and include paths to the preprocessor
    for (const auto& d : cmd.defines) {
        pp_args.push_back(d);
    }
    for (const auto& i : cmd.include_paths) {
        pp_args.push_back(i);
    }
    if (!cmd.language_standard.empty()) {
        pp_args.push_back(cmd.language_standard);
    }

    for (const auto& flag : cmd.other_flags) {
        if (cmd.is_msvc) {
            if (flag != "/nologo" && !flag.starts_with("/F")) {
                pp_args.push_back(flag);
            }
        } else {
            pp_args.push_back(flag);
        }
    }
    pp_args.push_back(cmd.source_file);

    auto [pp_exit, pp_output] = suco::run_local_capture(pp_args);
    if (pp_exit != 0) {
        std::cerr << pp_output << std::flush;
        return pp_exit;
    }

    cmd.preprocessed_source = std::move(pp_output);

    // 4. Normalize the preprocessed source code (remove line markings, blank lines, CRLF)
    std::string normalized = suco::normalize_preprocessed_source(cmd.preprocessed_source);
    if (normalized.empty()) {
        SUCO_LOG_WARNING("Normalized preprocessed source is empty. Falling back to local compilation.");
        return run_local_fallback(cmd);
    }

    // 5. Generate a unique cache hash based on metadata and normalized source
    suco::CacheKeyInput key{
        cmd.get_target_architecture(),
        cmd.get_compiler_version(),
        cmd.language_standard,
        suco::join(cmd.defines, "\x1F"),
        suco::join(cmd.include_paths, "\x1F"),
        suco::join(cmd.other_flags, "\x1F")
    };
    cmd.content_hash = suco::compute_cache_hash(normalized, key);
    if (cmd.content_hash.empty()) {
        SUCO_LOG_ERROR("Failed to compute cache hash. Falling back to local compilation.");
        return run_local_fallback(cmd);
    }

    // 6. Check the distributed cache via the coordinator (fastest path)
    CompileResult result;
    if (try_cache_hit(cmd, result)) {
        if (result.save_to(cmd.output_file)) {
            if (!result.log.empty()) {
                std::cerr.write(result.log.data(), static_cast<std::streamsize>(result.log.size()));
                std::cerr << std::flush;
            }
            return 0; // Cache hit successfully applied
        }
        SUCO_LOG_ERROR("Failed to save cached binary output to {}", cmd.output_file);
    }

    // 7. Request remote compilation on the grid via the coordinator (reusing socket)
    if (try_remote_compile(cmd, result)) {
        if (!result.log.empty()) {
            std::cerr.write(result.log.data(), static_cast<std::streamsize>(result.log.size()));
            std::cerr << std::flush;
        }

        if (result.exit_code == 0) {
            if (result.save_to(cmd.output_file)) {
                return 0; // Compilation succeeded, output stored
            }
            SUCO_LOG_ERROR("Failed to save compiled binary output to {}", cmd.output_file);
        } else {
            return result.exit_code; // Grid compilation finished with compiler error
        }
    }

    // 8. Fallback locally if grid communication fails
    SUCO_LOG_WARNING("Grid compilation failed or unavailable. Falling back to local compilation.");
    return run_local_fallback(cmd);
}

bool SucoClient::try_cache_hit(const CompilerCommand& cmd, CompileResult& out_result) {
    if (!network_.is_available()) {
        return false;
    }

    auto cache_res = network_.try_get_from_cache(cmd);
    if (cache_res.hit) {
        out_result.success = true;
        out_result.exit_code = 0;
        out_result.log = std::move(cache_res.log);
        out_result.binary = std::move(cache_res.binary);
        return true;
    }
    return false;
}

bool SucoClient::try_remote_compile(const CompilerCommand& cmd, CompileResult& out_result) {
    // try_compile will reuse the connection established during the cache query
    auto compile_res = network_.try_compile(cmd);
    if (compile_res.success) {
        out_result = std::move(compile_res);
        return true;
    }
    return false;
}

int SucoClient::run_local_fallback(const CompilerCommand& cmd) {
    return LocalCompiler::compile(cmd);
}

} // namespace suco
