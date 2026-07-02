#pragma once

#include "client_config.h"
#include "compiler_command.h"
#include "network_client.h"

namespace suco {

/**
 * @brief The central orchestrator that coordinates the SUCO compilation pipeline.
 * 
 * Manages the sequential fallbacks: Cache Hit -> Grid Compile -> Local Compile.
 */
class SucoClient {
public:
    /**
     * @brief Constructs the SucoClient.
     * @param config Configuration options.
     */
    explicit SucoClient(ClientConfig config = ClientConfig::load_or_default());

    /**
     * @brief Executes the compilation pipeline for the given compiler command.
     * @param command The parsed compiler command to compile.
     * @return Exit code of the compilation process (0 for success).
     */
    int run(const CompilerCommand& command);

private:


    /**
     * @brief Tries to compile the source code remotely on the SUCO worker grid.
     * @param cmd The compilation command details.
     * @param out_result Populated with the compilation outputs upon success.
     * @return True if the remote compilation succeeded (even if exit code != 0),
     *         false if grid compilation is unavailable.
     */
    bool try_remote_compile(const CompilerCommand& cmd, CompileResult& out_result);

    /**
     * @brief Compiles the source locally as a fallback measure.
     * @param cmd The compilation command details.
     * @return Exit code of the local compilation process.
     */
    int run_local_fallback(const CompilerCommand& cmd);

    ClientConfig config_;
    NetworkClient network_;
};

} // namespace suco
