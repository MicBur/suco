#pragma once

#include "compiler_command.h"
#include "client_config.h"
#include "socket_util.h"
#include "job_queue.h"

#include <string>
#include <functional>
#include <vector>
#include <utility>

namespace suco {

struct BatchJobResult {
    std::string content_hash;
    int32_t exit_code = -1;
    bool cache_hit = false;
    bool header_cache_hit = false;
    std::vector<char> log;
    std::vector<uint8_t> binary;
};

/**
 * @brief Represents the result of a cache query.
 */
struct CacheResult {
    bool hit = false;
    bool wait = false;
    std::vector<char> log;
    std::vector<uint8_t> binary;
    std::string worker_ip;
    uint16_t worker_port = 0;
    bool header_set_known = false;
    // Set when the query was abandoned because a LOCAL compile slot freed first
    // (the coordinator holds cache-miss queries while the grid is saturated; the
    // caller races that hold against its own cores). The caller then owns a slot.
    bool local_takeover = false;

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
    bool header_cache_hit = false;
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
     * @brief Sends a batch compile request for multiple jobs.
     */
    bool send_batch_compile_request(const std::vector<JobItem>& jobs);

    /**
     * @brief Reads the batch compilation response from the coordinator.
     */
    std::vector<BatchJobResult> read_batch_compile_response();

    /**
     * @brief Cleans up socket connections.
     */
    ~NetworkClient();

    uint32_t last_coord_scheduling_ms = 0;
    uint32_t last_worker_compilation_ms = 0;
    uint32_t last_toolchain_handling_ms = 0;

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
    /**
     * @brief Cache query; optionally races the coordinator's push-scheduling hold
     *        against local capacity. local_takeover_check is polled every 50ms while
     *        waiting for the response — when it returns true (side effect: the caller
     *        acquired a local slot) the query is abandoned and local_takeover is set.
     */
    CacheResult try_get_from_cache(const CompilerCommand& cmd,
                                   const std::function<bool()>& local_takeover_check = nullptr);

    /**
     * @brief Requests remote compilation on the SUCO grid coordinator.
     * 
     * Uses the existing active socket connection from a previous cache query.
     * @param cmd The compilation command (preprocessed source and command string).
     * @return CompileResult containing remote process status and output.
     */
    CompileResult try_compile(const CompilerCommand& cmd);
    CompileResult try_compile_direct(const CompilerCommand& cmd, const std::string& worker_ip, uint16_t worker_port);

    /**
     * @brief Blockingly waits for a compilation result on the active connection.
     * @return CompileResult containing remote status.
     */
    CompileResult wait_for_result();

    /**
     * @brief Uploads a locally compiled file to the coordinator cache.
     * @param hash The content hash of the compilation.
     * @param output_file The path to the compiled object file.
     * @return True if uploaded successfully, false otherwise.
     */
    bool upload_to_cache(const std::string& hash, const std::string& output_file);

private:
    /**
     * @brief Common helper to read the compile response from the socket.
     */
    CompileResult read_compile_response(bool keep_alive = false);

    /**
     * @brief Reads a worker's direct-compile response frame.
     *
     * Unlike read_compile_response() (compact coordinator→client frame), the
     * worker's handle_compile_job frame is: resp_type, filename, exit_code, log,
     * bin_comp, binary, header_cache_hit. Used only by try_compile_direct().
     */
    CompileResult read_direct_worker_response();

    /**
     * @brief Attempts to connect to the coordinator TCP port with a strict timeout.
     */
    /**
     * @brief Sends the E3 module-CMI block on an already-open direct-compile stream.
     *        Layout: count(4) then per CMI: name_len(4)+name, comp(1), data_len(4)+data.
     */
    bool send_module_cmis(const CompilerCommand& cmd);

    bool connect_to_coordinator();

    /**
     * @brief Closes the active socket connection if open.
     */
    void disconnect() noexcept;

    ClientConfig config_;
    socket_t sock_ = INVALID_SOCKET_VAL;
    bool is_connected_ = false;
    bool is_waiting_ = false;
};

} // namespace suco
