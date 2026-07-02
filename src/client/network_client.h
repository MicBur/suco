#pragma once

#include "compiler_command.h"
#include "client_config.h"
#include "socket_util.h"

#include <string>
#include <vector>
#include <utility>

namespace suco {

/**
 * @brief Represents the result of a cache query.
 */
struct CacheResult {
    bool hit = false;
    bool wait = false;
    std::vector<char> log;
    std::vector<uint8_t> binary;

    /**
     * @brief Writes the compiled binary output to the target output file.
     * @param path The output file path.
     * @return True if written successfully, false otherwise.
     */
    bool save_to(const std::string& path) const;
};

/**
 * @brief Represents the result of a remote compilation request.
 */
struct CompileResult {
    bool success = false;
    int32_t exit_code = -1;
    std::vector<char> log;
    std::vector<uint8_t> binary;

    /**
     * @brief Writes the compiled binary output to the target output file.
     * @param path The output file path.
     * @return True if written successfully, false otherwise.
     */
    bool save_to(const std::string& path) const;
};

/**
 * @brief Handles network communications with the SUCO coordinator.
 * 
 * Manages socket lifecycles, checks coordinator availability,
 * and executes cache queries and remote compilation tasks.
 */
class NetworkClient {
public:
    /**
     * @brief Constructs a NetworkClient with the given configuration.
     * @param config The client configuration.
     */
    explicit NetworkClient(ClientConfig config);

    /**
     * @brief Cleans up socket connections.
     */
    ~NetworkClient();

    // Disable copy/move to avoid socket resource leaks
    NetworkClient(const NetworkClient&) = delete;
    NetworkClient& operator=(const NetworkClient&) = delete;
    NetworkClient(NetworkClient&&) = delete;
    NetworkClient& operator=(NetworkClient&&) = delete;

    /**
     * @brief Verifies if the coordinator is available on the network.
     * @return True if reachable, false otherwise.
     */
    bool is_available();

    /**
     * @brief Queries the coordinator to check for a cache hit.
     * 
     * Keeps the socket connection alive in case of a cache miss.
     * @param cmd The compilation command (specifically the content hash).
     * @return CacheResult indicating success/failure and containing payload.
     */
    CacheResult try_get_from_cache(const CompilerCommand& cmd);

    /**
     * @brief Requests remote compilation on the SUCO grid coordinator.
     * 
     * Uses the existing active socket connection from a previous cache query.
     * @param cmd The compilation command (preprocessed source and command string).
     * @return CompileResult containing remote process status and output.
     */
    CompileResult try_compile(const CompilerCommand& cmd);

    /**
     * @brief Blockingly waits for a compilation result on the active connection.
     * @return CompileResult containing remote status.
     */
    CompileResult wait_for_result();

private:
    /**
     * @brief Common helper to read the compile response from the socket.
     */
    CompileResult read_compile_response();

    /**
     * @brief Attempts to connect to the coordinator TCP port with a strict timeout.
     */
    bool connect_to_coordinator();

    /**
     * @brief Closes the active socket connection if open.
     */
    void disconnect() noexcept;

    ClientConfig config_;
    socket_t sock_ = INVALID_SOCKET_VAL;
    bool is_connected_ = false;
};

} // namespace suco
