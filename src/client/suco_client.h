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

    /**
     * @brief Executes the compilation pipeline in parallel for multiple compiler commands.
     */
    int run(const std::vector<CompilerCommand>& commands);

    ClientConfig config_;
};

} // namespace suco
