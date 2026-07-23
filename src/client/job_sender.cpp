#include "job_sender.h"
#include "logging.h"
#include "hash_util.h"
#include "toolchain_packer.h"
#include "local_compiler.h"
#include "utils.h"
#include "header_set_hasher.h"
#include <iostream>
#include <chrono>

namespace {
std::mutex g_output_mutex;
}

namespace suco {

JobSender::JobSender(JobQueue& queue, const ClientConfig& config, size_t num_sender_threads, RequestContext context)
    : queue_(queue), config_(config), num_sender_threads_(num_sender_threads), context_(std::move(context)) {}

JobSender::~JobSender() {
    join();
}

void JobSender::start() {
    threads_.reserve(num_sender_threads_);
    for (size_t i = 0; i < num_sender_threads_; ++i) {
        threads_.emplace_back([this, i]() { sender_loop(i); });
    }
}

void JobSender::join() {
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads_.clear();
}

std::map<std::string, int> JobSender::get_results() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return results_;
}

void JobSender::sender_loop(size_t thread_id) {
    // Reuse a single NetworkClient instance across jobs processed by this sender thread!
    NetworkClient network(config_);
    
    while (true) {
        auto item_opt = queue_.pop();
        if (!item_opt) {
            break; // Finished
        }
        
        JobItem item = std::move(*item_opt);
        SUCO_LOG_INFO("JobSender thread {} sending {} (Queue size: {})", thread_id, item.cmd.source_file, queue_.size());
        
        if (item.client_socket != -1) {
            t_client_socket = item.client_socket;
        }
        int exit_code = 0;
        if (!item.preprocess_success) {
            SUCO_LOG_WARNING("Preprocessing failed for {}. Falling back to local compilation.", item.cmd.source_file);
            exit_code = run_local_fallback(item.cmd);
        } else {
            exit_code = process_job_pipeline(item, network);
        }
        if (item.client_socket != -1) {
            t_client_socket = -1;
        }
        
        {
            std::lock_guard<std::mutex> lock(results_mutex_);
            results_[item.cmd.source_file] = exit_code;
        }
    }
}

int JobSender::process_job_pipeline(JobItem& item, NetworkClient& network) {
    CompilerCommand cmd = item.cmd;
    cmd.preprocessed_source = std::move(item.preprocess_output);

    // 4. Normalize the preprocessed source code (remove line markings, blank lines, CRLF)
    std::string normalized = suco::normalize_preprocessed_source(cmd.preprocessed_source);
    if (normalized.empty()) {
        SUCO_LOG_WARNING("Normalized preprocessed source is empty for {}. Falling back to local compilation.", cmd.source_file);
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
    cmd.content_hash = suco::compute_cache_hash(normalized, key, context_);
    if (cmd.content_hash.empty()) {
        SUCO_LOG_ERROR("Failed to compute cache hash for {}. Falling back to local compilation.", cmd.source_file);
        return run_local_fallback(cmd);
    }

    // Compute header set hash and split source
    if (config_.header_cache_enabled) {
        HeaderSetHasher::compute_hash(cmd);
    }

    if (!cmd.header_set_hash.empty()) {
        cmd.preprocessed_source = cmd.stripped_source;
    }
        // Ship the FULL preprocessed output, not the normalized one: normalization
        // strips every line marker, and without them the compiler only knows the
        // temp .ii it was handed, so __FILE__/__LINE__ (and asserts, and debug info)
        // point at /tmp/suco_temp_X.ii instead of the real source. The header-set
        // path already ships markers for the same reason. The cache key stays on the
        // normalized text, so keying is unchanged and remains path-independent.
    // else: cmd.preprocessed_source already holds the full output — leave it alone.

    // 5b. Pack/Ensure toolchain archive exists
    bool is_qt = false;
    for (const auto& inc : cmd.include_paths) {
        if (inc.find("qt") != std::string::npos || inc.find("QT") != std::string::npos) {
            is_qt = true;
            break;
        }
    }
    if (!is_qt) {
        for (const auto& def : cmd.defines) {
            if (def.find("qt") != std::string::npos || def.find("QT") != std::string::npos) {
                is_qt = true;
                break;
            }
        }
    }

    ToolchainInfo tc_info = ToolchainPacker::pack(cmd.compiler_path, is_qt);
    if (tc_info.success) {
        cmd.toolchain_hash = tc_info.hash;
        cmd.compiler_path = tc_info.resolved_compiler_path;
    } else {
        SUCO_LOG_WARNING("Failed to pack or locate compiler toolchain for {}. Proceeding without toolchain hash.", cmd.source_file);
    }

    // 6. Check the distributed cache via the coordinator
    CacheResult cache_res = network.try_get_from_cache(cmd);
    if (cache_res.hit) {
        if (cache_res.save_to(cmd.output_file)) {
            if (!cache_res.log.empty()) {
                if (t_client_socket != static_cast<ipc_socket_t>(-1)) {
                    send_ipc_frame(t_client_socket, IPC_RESP_STDERR, std::string(cache_res.log.begin(), cache_res.log.end()));
                } else {
                    std::lock_guard<std::mutex> lock(g_output_mutex);
                    std::cerr.write(cache_res.log.data(), static_cast<std::streamsize>(cache_res.log.size()));
                    std::cerr << std::flush;
                }
            }
            return 0; // Cache hit successfully applied
        }
        SUCO_LOG_ERROR("Failed to save cached binary output to {}", cmd.output_file);
    } else if (cache_res.wait) {
        // 6b. Wait for parallel compile of the same hash to finish
        CompileResult wait_res = network.wait_for_result();
        if (wait_res.success) {
            if (!wait_res.log.empty()) {
                if (t_client_socket != static_cast<ipc_socket_t>(-1)) {
                    send_ipc_frame(t_client_socket, IPC_RESP_STDERR, std::string(wait_res.log.begin(), wait_res.log.end()));
                } else {
                    std::lock_guard<std::mutex> lock(g_output_mutex);
                    std::cerr.write(wait_res.log.data(), static_cast<std::streamsize>(wait_res.log.size()));
                    std::cerr << std::flush;
                }
            }
            if (wait_res.exit_code == 0) {
                if (wait_res.save_to(cmd.output_file)) {
                    return 0;
                }
                SUCO_LOG_ERROR("Failed to save compiled binary output to {}", cmd.output_file);
            } else if (wait_res.exit_code == -1) {
                SUCO_LOG_WARNING("Parallel compilation returned -1 for {} (grid busy or error). Falling back to local compilation.", cmd.source_file);
                return run_local_fallback(cmd);
            } else {
                SUCO_LOG_WARNING("Parallel grid compilation returned exit code {} for {}. Falling back to local compilation for verification.", wait_res.exit_code, cmd.source_file);
                return run_local_fallback(cmd);
            }
        }
        SUCO_LOG_WARNING("Wait for parallel compilation result failed for {}. Falling back to local compilation.", cmd.source_file);
        return run_local_fallback(cmd);
    }

    // 7. Request remote compilation on the grid directly to the assigned worker
    CompileResult result;
    if (!cache_res.worker_ip.empty() && cache_res.worker_port != 0) {
        result = network.try_compile_direct(cmd, cache_res.worker_ip, cache_res.worker_port);
    } else {
        SUCO_LOG_WARNING("No worker assigned by coordinator. Falling back to local compilation.");
        return run_local_fallback(cmd);
    }
    if (result.success) {
        if (!result.log.empty()) {
            if (t_client_socket != static_cast<ipc_socket_t>(-1)) {
                send_ipc_frame(t_client_socket, IPC_RESP_STDERR, std::string(result.log.begin(), result.log.end()));
            } else {
                std::lock_guard<std::mutex> lock(g_output_mutex);
                std::cerr.write(result.log.data(), static_cast<std::streamsize>(result.log.size()));
                std::cerr << std::flush;
            }
        }

        if (result.exit_code == 0) {
            if (result.save_to(cmd.output_file)) {
                return 0; // Compilation succeeded, output stored
            }
            SUCO_LOG_ERROR("Failed to save compiled binary output to {}", cmd.output_file);
        } else if (result.exit_code == -1) {
            SUCO_LOG_WARNING("Grid compilation returned -1 for {} (grid busy or error). Falling back to local compilation.", cmd.source_file);
            return run_local_fallback(cmd);
        } else {
            SUCO_LOG_WARNING("Grid compilation returned exit code {} for {}. Falling back to local compilation for verification.", result.exit_code, cmd.source_file);
            return run_local_fallback(cmd);
        }
    }

    // 8. Fallback locally if grid communication fails
    SUCO_LOG_WARNING("Grid compilation failed or unavailable for {}. Falling back to local compilation.", cmd.source_file);
    return run_local_fallback(cmd);
}

int JobSender::run_local_fallback(const CompilerCommand& cmd) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    return LocalCompiler::compile(cmd, context_.cwd);
}

} // namespace suco
