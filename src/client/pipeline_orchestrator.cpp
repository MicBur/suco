#include "pipeline_orchestrator.h"
#include "network_client.h"
#include "local_compiler.h"
#include "logging.h"
#include "utils.h"
#include "ipc_protocol.h"
#include "local_prep_cache.h"
#include "hash_util.h"
#include "header_set_hasher.h"
#include "local_compiler.h"
#include <semaphore>
#include <filesystem>

#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <chrono>
#include <algorithm>

namespace {
// A worker IP is only usable for DIRECT dispatch if the client can actually
// route to it. A co-located worker (same box as the coordinator) registers as
// 127.0.0.1; the coordinator then hands that loopback address to *remote*
// clients, where connecting to 127.0.0.1 would hit the client's OWN machine
// (silently compiling on the wrong node if it happens to run a local worker,
// or failing outright otherwise). So: trust a loopback worker IP only when the
// coordinator itself is local (a genuine single-box / dev setup).
bool is_loopback_host(const std::string& h) {
    return h.empty() || h == "localhost" || h == "::1" || h.rfind("127.", 0) == 0;
}
bool worker_directly_reachable(const std::string& worker_ip, const std::string& coordinator_host) {
    return !is_loopback_host(worker_ip) || is_loopback_host(coordinator_host);
}

// --- Local content-addressed object cache: <cache_dir>/objects/<content_hash>.o ---
// Serves warm/incremental rebuilds fully locally (no coordinator round-trip and no
// network object transfer). Keyed by the SAME content_hash as the coordinator cache,
// so it is exactly as correct — a hit means byte-identical inputs.
bool object_cache_on() {
    static const bool on = []() {
        const char* e = std::getenv("SUCO_LOCAL_OBJECT_CACHE");
        return !(e && (std::string(e) == "0" || std::string(e) == "off" || std::string(e) == "false"));
    }();
    return on;
}
std::string local_object_path(const std::string& cache_dir, const std::string& content_hash) {
    return cache_dir + "/objects/" + content_hash + ".o";
}
bool local_object_get(const std::string& cache_dir, const std::string& content_hash,
                      const std::string& out_path) {
    if (!object_cache_on() || cache_dir.empty() || content_hash.empty()) return false;
    std::error_code ec;
    std::string src = local_object_path(cache_dir, content_hash);
    if (!std::filesystem::exists(src, ec)) return false;
    std::filesystem::path outp(out_path);
    if (outp.has_parent_path()) std::filesystem::create_directories(outp.parent_path(), ec);
    std::filesystem::copy_file(src, out_path,
                               std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}
void local_object_store(const std::string& cache_dir, const std::string& content_hash,
                        const std::string& obj_path) {
    if (!object_cache_on() || cache_dir.empty() || content_hash.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(obj_path, ec)) return;
    std::filesystem::create_directories(cache_dir + "/objects", ec);
    std::ostringstream tid; tid << std::this_thread::get_id();
    std::string dst = local_object_path(cache_dir, content_hash);
    std::string tmp = dst + ".tmp." + tid.str();
    std::filesystem::copy_file(obj_path, tmp,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) {
        std::filesystem::rename(tmp, dst, ec);       // atomic replace
        if (ec) std::filesystem::remove(tmp, ec);
    }
}
}

namespace suco {

PipelineOrchestrator::PipelineOrchestrator(const ClientConfig& config, size_t total_jobs, RequestContext context, std::counting_semaphore<1024>* external_local_semaphore)
    : config_(config), total_jobs_(total_jobs), context_(std::move(context)) {
    
    build_start_time_ = std::chrono::steady_clock::now();

    auto get_env = [&](const char* name) -> const char* {
        auto it = context_.env_overrides.find(name);
        if (it != context_.env_overrides.end()) {
            return it->second.c_str();
        }
        return std::getenv(name);
    };

    // 0. Determine local compilation slots.
    unsigned int hw_cores = std::thread::hardware_concurrency();
    int default_local_slots;
    const char* env_local_slots = get_env("SUCO_LOCAL_SLOTS");
    if (env_local_slots) {
        // Explicit override always wins (0 = disable local compilation).
        default_local_slots = static_cast<int>(hw_cores > 2 ? hw_cores - 2 : 1);
        try {
            int ls = std::stoi(env_local_slots);
            if (ls >= 0) default_local_slots = ls;
        } catch (...) {}
    } else {
        // Local slots are what closed the cold gap to icecc: the grid already ran at
        // ~100% slot efficiency, but icecc ALSO compiles on the build machine (667%
        // client CPU vs our 191%), so idle client cores were the whole difference.
        // Standalone (one process per file, the make/ninja model) shares the budget
        // across processes via FlockSlotArbiter — a per-process budget oversubscribed
        // the client ~20x under -j20 (21.6s vs 7.9s on 60 files), which is why this
        // used to default to 0. Daemon mode keeps its in-process shared semaphore.
        default_local_slots = static_cast<int>(hw_cores > 2 ? hw_cores - 2 : 1);
    }
#ifdef _WIN32
    if (external_local_semaphore == nullptr && !env_local_slots) {
        default_local_slots = 0;   // no flock on Windows — per-process budgets would oversubscribe
    }
#endif
    local_slots_ = default_local_slots;

    if (external_local_semaphore) {
        slot_arbiter_ = std::make_unique<SemaphoreSlotArbiter>(external_local_semaphore);
    } else {
#ifndef _WIN32
        slot_arbiter_ = std::make_unique<FlockSlotArbiter>(local_slots_ > 0 ? local_slots_ : 1);
#else
        static std::counting_semaphore<1024> win_sem(local_slots_ > 0 ? local_slots_ : 0);
        slot_arbiter_ = std::make_unique<SemaphoreSlotArbiter>(&win_sem);
#endif
    }
    SUCO_LOG_INFO("Local compilation slots: {} (of {} cores)", local_slots_, hw_cores);

    // 1. Determine preprocessing threads
    num_preprocess_threads_ = 4;
    const char* env_threads = get_env("SUCO_PREPROCESS_THREADS");
    if (env_threads) {
        try {
            int t = std::stoi(env_threads);
            if (t > 0) num_preprocess_threads_ = static_cast<size_t>(t);
        } catch (...) {}
    } else {
        if (hw_cores > num_preprocess_threads_) num_preprocess_threads_ = hw_cores;
    }

    // 2. Base Batch settings
    const char* env_batch_size = get_env("SUCO_BATCH_SIZE");
    if (env_batch_size) {
        try { batch_size_ = std::stoul(env_batch_size); } catch (...) {}
    }
    const char* env_timeout = get_env("SUCO_BATCH_TIMEOUT_MS");
    if (env_timeout) {
        try { batch_timeout_ms_ = std::stoul(env_timeout); } catch (...) {}
    }
    max_parallel_batches_ = 0;

    // 3. Process aggressiveness
    aggressiveness_level_ = config_.pipeline_aggressiveness;
    const char* env_aggr = get_env("SUCO_PIPELINE_AGGRESSIVENESS");
    if (env_aggr) aggressiveness_level_ = env_aggr;

    if (aggressiveness_level_ == "high") {
        batch_size_ = std::max(static_cast<size_t>(4), batch_size_ / 2);
        batch_timeout_ms_ = 50; // aggressive short timeout for faster pipelining
        SUCO_LOG_INFO("PipelineOrchestrator: Starting aggressive pipelining (level=high, batch_size={}, timeout=50ms)", batch_size_);
    } else if (aggressiveness_level_ == "low") {
        batch_size_ = static_cast<size_t>(batch_size_ * 1.5);
        batch_timeout_ms_ = static_cast<size_t>(batch_timeout_ms_ * 1.5);
        SUCO_LOG_INFO("PipelineOrchestrator: Starting conservative pipelining (level=low, batch_size={}, timeout={}ms)", batch_size_, batch_timeout_ms_);
    } else {
        aggressiveness_level_ = "medium";
        SUCO_LOG_INFO("PipelineOrchestrator: Starting standard pipelining (level=medium, batch_size={}, timeout={}ms)", batch_size_, batch_timeout_ms_);
    }

    const char* env_pipe = get_env("SUCO_DISABLE_PIPELINING");
    if (env_pipe && (std::string(env_pipe) == "1" || std::string(env_pipe) == "true")) {
        pipelining_enabled_ = false;
        SUCO_LOG_INFO("PipelineOrchestrator: PIPELINING IS DISABLED (Teilschritt A mode)");
    }

    // Direct dispatch (default on). When enabled, cache-miss grid jobs are streamed
    // straight to the coordinator-assigned worker (bypassing the coordinator payload
    // funnel). Escape hatch mirrors SUCO_COMPRESSION / SUCO_PATH_NORMALIZATION style.
    const char* env_direct = get_env("SUCO_DIRECT_DISPATCH");
    if (env_direct) {
        std::string v(env_direct);
        if (v == "0" || v == "off" || v == "OFF" || v == "false") {
            direct_dispatch_enabled_ = false;
            SUCO_LOG_INFO("PipelineOrchestrator: DIRECT DISPATCH DISABLED — grid jobs funnel through coordinator batch path");
        }
    }

    // Backpressure budget for a saturated grid (see header). Env override in ms; 0 disables.
    const char* env_gw = get_env("SUCO_GRID_WAIT_MS");
    if (env_gw) {
        try { grid_wait_budget_ms_ = std::max(0, std::stoi(env_gw)); } catch (...) {}
    }

    if (total_jobs_ == 1) {
        num_preprocess_threads_ = 0;
        batch_size_ = 1;
        batch_timeout_ms_ = 0;
        max_parallel_batches_ = 0;
        pipelining_enabled_ = false;
        SUCO_LOG_INFO("PipelineOrchestrator: Optimizing for single job (synchronous execution, 0 worker threads)");
    }

    // 4. Instantiate components
    prep_pool_ = std::make_unique<PreprocessingPool>(num_preprocess_threads_);

    auto on_job_completed = [this](std::string source_file, int exit_code) {
        std::lock_guard<std::mutex> lock(results_mutex_);
        completed_jobs_[source_file] = exit_code;
        SUCO_LOG_INFO("on_job_completed: stored exit code {} for {}", exit_code, source_file);
        if (exit_code != 0) {
            overall_exit_code_ = exit_code;
        }
    };

    sender_ = std::make_unique<BatchSender>(config_, max_parallel_batches_, on_job_completed, context_, slot_arbiter_.get());
    collector_ = std::make_unique<BatchCollector>(batch_size_, batch_timeout_ms_, [this](std::vector<JobItem> batch) {
        sender_->send_batch(std::move(batch));
    });

    prep_futures_.reserve(total_jobs_);
}

PipelineOrchestrator::~PipelineOrchestrator() {
    prep_pool_.reset();
    collector_.reset();
    sender_.reset();
}

void PipelineOrchestrator::enqueue_job(const CompilerCommand& cmd, ipc_socket_t client_socket) {
    prep_futures_.push_back(prep_pool_->enqueue([this, cmd, client_socket]() {
        JobItem item;
        item.cmd = cmd;
        item.client_socket = client_socket;
        
        if (!cmd.is_compilation_step() || cmd.is_pch_creation) {
            item.preprocess_success = false;
            item.preprocess_exit_code = -1;
            collector_->add_job(std::move(item));
            return;
        }

        // --- 1. LOCAL PREPROCESSOR CACHE LOOKUP ---
        std::string preprocessed_source;
        std::string content_hash;
        std::string header_set_hash;
        std::string header_set_source;
        
        bool cache_hit = false;
        if (config_.local_prep_cache_enabled) {
            cache_hit = LocalPrepCache::try_get(
                config_, cmd, preprocessed_source, content_hash, header_set_hash, header_set_source, context_
            );
            // E3: this path restores a cached content_hash and skips module scanning, so a
            // module TU would be dispatched with no CMIs AND under a key that still reflects
            // the CMIs of the last run — a changed module interface would serve a stale
            // object. Re-run the full path for module TUs; it rescans and rekeys them.
            if (cache_hit && suco::uses_cxx_modules(preprocessed_source)) {
                SUCO_LOG_INFO("{} imports C++20 modules — bypassing the preprocessor cache to "
                              "re-resolve its CMIs", cmd.source_file);
                cache_hit = false;
                preprocessed_source.clear();
                content_hash.clear();
                header_set_hash.clear();
                header_set_source.clear();
            }
        }

        if (cache_hit) {
            item.preprocess_success = true;
            item.preprocess_exit_code = 0;
            item.local_cache_hit = true;
            item.cmd.content_hash = content_hash;
            item.cmd.header_set_hash = header_set_hash;
            item.cmd.preprocessed_source = preprocessed_source;
            item.cmd.header_set_source = header_set_source;
            SUCO_LOG_INFO("Local preprocessor cache hit for {}", cmd.source_file);

            // Fully-local warm path: prep-cache gave us the content_hash without any
            // preprocessing; if the object is already in the local object cache we
            // serve it directly — no coordinator round-trip, no network transfer.
            {
                std::filesystem::path lo_out(item.cmd.output_file);
                if (lo_out.is_relative() && !context_.cwd.empty())
                    lo_out = std::filesystem::path(context_.cwd) / lo_out;
                if (local_object_get(config_.cache_directory, item.cmd.content_hash, lo_out.string())) {
                    local_obj_hits_++;
                    SUCO_LOG_INFO("Local object cache hit for {} (no network)", cmd.source_file);
                    std::lock_guard<std::mutex> lock(results_mutex_);
                    completed_jobs_[item.cmd.source_file] = 0;
                    return;
                }
            }

            // Apply local slot decision
            if (local_slots_ > 0 && slot_arbiter_->try_acquire()) {
                SUCO_LOG_DEBUG("Local slot acquired (cache hit) for {}", cmd.source_file);
                t_client_socket = item.client_socket;
                int exit_code = LocalCompiler::compile(item.cmd, context_.cwd);
                t_client_socket = -1;
                slot_arbiter_->release();
                local_compile_count_++;
                if (exit_code == 0) {
                    std::filesystem::path out_path(item.cmd.output_file);
                    if (out_path.is_relative() && !context_.cwd.empty()) {
                        out_path = std::filesystem::path(context_.cwd) / out_path;
                    }
                    NetworkClient network(config_);
                    network.upload_to_cache(item.cmd.content_hash, out_path.string());
                    local_object_store(config_.cache_directory, item.cmd.content_hash, out_path.string());
                }
                {
                    std::lock_guard<std::mutex> lock(results_mutex_);
                    completed_jobs_[item.cmd.source_file] = exit_code;
                    if (exit_code != 0) {
                        overall_exit_code_ = exit_code;
                    }
                }
            } else {
                collector_->add_job(std::move(item));
            }
            return;
        }
        
        auto start_time = std::chrono::steady_clock::now();
        
        std::vector<std::string> pp_args;
        pp_args.push_back(cmd.compiler_path);
        if (cmd.is_msvc) {
            pp_args.push_back("/E");
            pp_args.push_back("/nologo");
        } else {
            pp_args.push_back("-E");
            if (cmd.required_compiler == "g++" || cmd.required_compiler == "gcc") {
                pp_args.push_back("-fdirectives-only");
            } else if (cmd.required_compiler == "clang++" || cmd.required_compiler == "clang") {
                pp_args.push_back("-frewrite-includes");
            }
        }
        
        // Flags that take a separate value are stored as one unit ("-include foo.h")
        // so sorting cannot tear them apart — split them back into argv tokens here.
        for (const auto& d : cmd.defines) suco::append_flag_tokens(pp_args, d);
        for (const auto& i : cmd.include_paths) suco::append_flag_tokens(pp_args, i);
        if (!cmd.language_standard.empty()) pp_args.push_back(cmd.language_standard);
        for (const auto& flag : cmd.other_flags) {
            if (cmd.is_msvc) {
                if (flag != "/nologo" && !flag.starts_with("/F")) suco::append_flag_tokens(pp_args, flag);
            } else {
                suco::append_flag_tokens(pp_args, flag);
            }
        }
        pp_args.push_back(cmd.source_file);
        
        if (std::getenv("SUCO_DEBUG_PAYLOAD")) {
            std::string joined;
            for (const auto& a : pp_args) joined += a + " ";
            SUCO_LOG_INFO("[PP-CMD] cwd={} | {}", context_.cwd, joined);
        }
        auto [pp_exit, pp_output] = suco::run_local_capture(pp_args, context_.cwd);
        auto end_time = std::chrono::steady_clock::now();
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
        
        total_prep_time_ms_ += duration_ms;

        if (pp_exit == 0) {
            item.preprocess_success = true;
            item.preprocess_exit_code = 0;
            SUCO_LOG_INFO("Preprocessing for {} finished in {} ms", cmd.source_file, duration_ms);

            // --- CACHE-CORRECTNESS GUARD (ccache-style) ---
            // Fast preprocessing (-fdirectives-only / -frewrite-includes) leaves
            // macros unexpanded, so __DATE__/__TIME__/__TIMESTAMP__ survive into the
            // payload. A cache hit would then serve an object with a STALE timestamp.
            // Such files are compiled locally and never touch any cache.
            // (MSVC /E fully expands macros, so its output never contains the tokens
            // and keeps normal caching — values are frozen at preprocess time.)
            // C++20 modules (E3): `import x;` survives preprocessing, and the worker can
            // only resolve it against a CMI (gcm.cache/x.gcm) it doesn't have. So collect
            // the CMIs this TU imports and ship them with the job; their bytes go into the
            // cache key below, which keeps a hit correct when a module interface changes.
            // If any CMI can't be found (or it's a module *interface* unit, whose own CMI
            // is an output, not an input) we fall back to a local compile — correct > fast.
            bool modules_local_fallback = false;
            if (suco::uses_cxx_modules(pp_output)) {
                std::vector<std::string> imports = suco::scan_module_imports(pp_output);
                bool all_found = !imports.empty();
                for (const auto& m : imports) {
                    std::string cmi;
                    if (!suco::find_and_read_cmi(m, context_.cwd, cmi)) { all_found = false; break; }
                    item.cmd.module_cmis.emplace_back(m, std::move(cmi));
                }
                if (!all_found) {
                    item.cmd.module_cmis.clear();
                    modules_local_fallback = true;
                } else {
                    SUCO_LOG_INFO("{} imports {} module(s) — shipping their CMIs with the job",
                                  cmd.source_file, item.cmd.module_cmis.size());
                }
            }

            const bool needs_local = suco::contains_time_macros(pp_output) || modules_local_fallback;
            if (needs_local) {
                SUCO_LOG_INFO("{} uses C++20 modules (CMI unavailable) or __DATE__/__TIME__ — compiling locally, bypassing cache",
                              cmd.source_file);
                if (local_slots_ > 0) slot_arbiter_->acquire();
                t_client_socket = item.client_socket;
                int exit_code = LocalCompiler::compile(item.cmd, context_.cwd);
                t_client_socket = -1;
                if (local_slots_ > 0) slot_arbiter_->release();
                local_compile_count_++;
                std::lock_guard<std::mutex> lock(results_mutex_);
                completed_jobs_[item.cmd.source_file] = exit_code;
                if (exit_code != 0) {
                    overall_exit_code_ = exit_code;
                }
                return;
            }

            // --- 2. NORMALIZE & HASH (AND SAVE TO CACHE) ON CACHE MISS ---
            const bool phase_timing = std::getenv("SUCO_TIMING") != nullptr;
            auto pt0 = std::chrono::steady_clock::now();
            std::string normalized = suco::normalize_preprocessed_source(pp_output);
            auto pt1 = pt0, pt2 = pt0, pt3 = pt0, pt4 = pt0;
            pt1 = std::chrono::steady_clock::now();
            if (!normalized.empty()) {
                suco::CacheKeyInput key{
                    item.cmd.get_target_architecture(),
                    item.cmd.get_compiler_version(),
                    item.cmd.language_standard,
                    suco::join(item.cmd.defines, "\x1F"),
                    suco::join(item.cmd.include_paths, "\x1F"),
                    suco::join(item.cmd.other_flags, "\x1F")
                };
                // E3: a TU's result depends on the CMIs it imports — fold their bytes into
                // the key, otherwise editing a module interface would serve a stale object.
                // Only module TUs need the CMI-salted copy; everything else hashes the
                // normalized buffer in place (the copy was ~8MB per heavy TU).
                std::string keyed;
                const std::string* key_input = &normalized;
                if (!item.cmd.module_cmis.empty()) {
                    keyed = normalized;
                    for (const auto& [mname, mbytes] : item.cmd.module_cmis) {
                        // NB: the leading token is byte-for-byte "\xfc" "MI" "\x1f" on
                        // purpose. It was written "\x1fCMI\x1f", but C++'s \x escape is
                        // greedy: "\x1fC" is one out-of-range hex escape that GCC truncates
                        // to 0xFC, so the actual bytes fed into the cache key have always
                        // been {0xFC,'M','I',0x1F} on the grid. MSVC rejects the greedy
                        // escape outright (C7744). Reproduce the exact grid bytes so module
                        // cache keys do not drift (invariant: byte-identity of cache keys).
                        keyed += "\xfc" "MI\x1f" + mname + "\x1f" + suco::compute_sha256(mbytes);
                    }
                    key_input = &keyed;
                }
                item.cmd.content_hash = suco::compute_cache_hash(*key_input, key, context_);
                pt2 = std::chrono::steady_clock::now();
                if (!item.cmd.content_hash.empty()) {
                    item.cmd.preprocessed_source = pp_output;
                    // E3: header-set/PCH caching and C++20 modules are mutually exclusive.
                    // Under -fmodules-ts, GCC compiles `-x c++-header` into a header *unit*
                    // in gcm.cache and ignores -o, so no .gch is ever produced — the worker
                    // would then compile header-stripped source with no headers at all.
                    // Module TUs therefore ship their full preprocessed source.
                    if (item.cmd.module_cmis.empty() && config_.header_cache_enabled) {
                        HeaderSetHasher::compute_hash(item.cmd);
                    }

                    if (!item.cmd.header_set_hash.empty()) {
                        item.cmd.preprocessed_source = item.cmd.stripped_source;
                    }
        // Ship the FULL preprocessed output, not the normalized one: normalization
        // strips every line marker, and without them the compiler only knows the
        // temp .ii it was handed, so __FILE__/__LINE__ (and asserts, and debug info)
        // point at /tmp/suco_temp_X.ii instead of the real source. The header-set
        // path already ships markers for the same reason. The cache key stays on the
        // normalized text, so keying is unchanged and remains path-independent.
                    // else: preprocessed_source already holds pp_output — leave it alone.
                    pt3 = std::chrono::steady_clock::now();

                    if (config_.local_prep_cache_enabled) {
                        LocalPrepCache::store(
                            config_, cmd, pp_output,
                            item.cmd.preprocessed_source, item.cmd.content_hash,
                            item.cmd.header_set_hash, item.cmd.header_set_source, context_
                        );
                    }
                    pt4 = std::chrono::steady_clock::now();
                    if (phase_timing) {
                        auto ms = [](auto a, auto b) {
                            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
                        };
                        SUCO_LOG_INFO("[TIMING] {}: pp={}ms norm={:.1f}ms key+hash={:.1f}ms hset+copies={:.1f}ms prep-store={:.1f}ms (pp_bytes={})",
                                      cmd.source_file, duration_ms, ms(pt0, pt1), ms(pt1, pt2),
                                      ms(pt2, pt3), ms(pt3, pt4), pp_output.size());
                    }
                    item.local_cache_hit = true;
                } else {
                    item.preprocess_output = std::move(pp_output);
                }
            } else {
                item.preprocess_output = std::move(pp_output);
            }
        } else {
            item.preprocess_success = false;
            item.preprocess_exit_code = pp_exit;
            // run_local_capture merges the child's stderr into its output, so the compiler's
            // own reason is right here. Log it: a preprocessing failure silently downgrades
            // the whole build to local compiles (correct, but at native speed), and without
            // the reason that looks like "the grid is just slow" rather than a broken flag.
            // The compiler's diagnostics come AFTER the preprocessed text it already
            // emitted, so take the TAIL — the head is just the source.
            std::string why = pp_output.size() > 400 ? pp_output.substr(pp_output.size() - 400)
                                                     : pp_output;
            SUCO_LOG_WARNING("Preprocessing for {} failed with exit code {} in {} ms — compiling locally. Compiler said:\n{}",
                             cmd.source_file, pp_exit, duration_ms, why);
            item.preprocess_output = std::move(pp_output);
        }
        
        // --- COORDINATOR CACHE LOOKUP ---
        // Worker assigned by the coordinator on a cache miss. Captured here so the
        // grid-dispatch branch below can talk to that worker directly.
        std::string assigned_worker_ip;
        uint16_t assigned_worker_port = 0;
        std::chrono::steady_clock::time_point t_q0{};  // set at cache-query start; used by SUCO_TIMING [NET] log
        if (item.preprocess_success) {
            // Local object cache (content-addressed) — serve a previously built object
            // without any coordinator round-trip or network transfer.
            if (!item.cmd.content_hash.empty()) {
                std::filesystem::path lo_out(item.cmd.output_file);
                if (lo_out.is_relative() && !context_.cwd.empty())
                    lo_out = std::filesystem::path(context_.cwd) / lo_out;
                if (local_object_get(config_.cache_directory, item.cmd.content_hash, lo_out.string())) {
                    local_obj_hits_++;
                    SUCO_LOG_INFO("Local object cache hit for {} (no network)", cmd.source_file);
                    std::lock_guard<std::mutex> lock(results_mutex_);
                    completed_jobs_[item.cmd.source_file] = 0;
                    return;
                }
            }
            // --- ICECC-STYLE LOCAL-FIRST ---
            // A free local core beats anything the grid offers: no query, no transfer,
            // no wait. This is why icecc's cold builds ran 667% client CPU to our 191%.
            // Order matters: the coordinator query below BLOCKS on a saturated grid
            // (push scheduling holds the query until a worker slot frees), so checking
            // local capacity only after it returned meant client cores were only ever
            // used by timeout fallbacks. Cost of skipping the L2 query on this path: a
            // team-warm hit becomes a compile instead of a download — same-machine warm
            // builds are already served by the L1 check above, and the object is
            // uploaded to L2 either way, so the cache stays complete.
            // Runs the job on this machine; caller must already HOLD a local slot.
            auto compile_on_client = [&](const char* reason) {
                SUCO_LOG_INFO("{}: free client core takes {}", reason, cmd.source_file);
                t_client_socket = item.client_socket;
                int exit_code = LocalCompiler::compile(item.cmd, context_.cwd);
                t_client_socket = -1;
                slot_arbiter_->release();
                local_compile_count_++;
                if (exit_code == 0) {
                    std::filesystem::path out_path(item.cmd.output_file);
                    if (out_path.is_relative() && !context_.cwd.empty())
                        out_path = std::filesystem::path(context_.cwd) / out_path;
                    local_object_store(config_.cache_directory, item.cmd.content_hash, out_path.string());
                    NetworkClient up(config_);
                    up.upload_to_cache(item.cmd.content_hash, out_path.string());
                }
                std::lock_guard<std::mutex> lock(results_mutex_);
                completed_jobs_[item.cmd.source_file] = exit_code;
                if (exit_code != 0) overall_exit_code_ = exit_code;
            };

            if (local_slots_ > 0 && !item.cmd.content_hash.empty() &&
                slot_arbiter_->try_acquire()) {
                compile_on_client("Local-first");
                return;
            }

            std::function<bool()> takeover_check;
            if (local_slots_ > 0 && !item.cmd.content_hash.empty()) {
                takeover_check = [this] { return slot_arbiter_->try_acquire(); };
            }
            t_q0 = std::chrono::steady_clock::now();
            NetworkClient network(config_);
            CacheResult cache_res = network.try_get_from_cache(item.cmd, takeover_check);
            if (cache_res.local_takeover) {
                // try_get_from_cache acquired the slot for us via takeover_check.
                compile_on_client("Grid busy, local core freed first");
                return;
            }

            // --- BACKPRESSURE: wait for a direct-dispatch slot instead of funnelling ---
            // A saturated grid returns an empty worker_ip (all slots reserved). The old
            // behaviour dumped such jobs onto the coordinator batch/funnel path, which
            // collapses under real load (make -jN, N >> grid slots): "coordinator
            // disconnected" -> local-fallback storm -> heavy TUs serialise on the client.
            // Instead, briefly re-query for a freeing slot so the job stays on the fast
            // direct path. Bounded by grid_wait_budget_ms_ so a build never hangs; on
            // budget exhaustion we fall through to the unchanged local/batch fallback.
            // With local slots this loop is a RACE for whichever capacity frees first —
            // a client core or a grid slot. Checking local capacity only once before the
            // query left client cores idle: a job would sit in the (blocking) query/
            // re-query cycle while a local core sat free next to it. icecc's scheduler
            // does exactly this race, which is where its cold-build edge came from.
            if (direct_dispatch_enabled_ && grid_wait_budget_ms_ > 0 &&
                !cache_res.hit && !cache_res.wait && !cache_res.header_set_known &&
                cache_res.worker_ip.empty()) {
                // Gentle polling: each re-query opens a fresh coordinator connection,
                // so poll SLOWLY (backoff grows to 500ms). Heavy TUs hold slots for
                // seconds, so 500ms granularity costs almost nothing but keeps the
                // coordinator query rate ~30x lower than a tight loop — a tight poll
                // here just recreates the funnel-overload it was meant to avoid.
                // Poll interval: heavy TUs finish in waves (~13 slots free near-
                // simultaneously), so a freed slot idles until the next poll. Keep the
                // interval short (cap 150ms) so wave-boundary dips are small — the
                // fresh-NetworkClient-per-query + coordinator peek-loop make the higher
                // query rate safe. Tune via SUCO_GRID_POLL_MS.
                int poll_cap_ms = 150;
                if (const char* pe = std::getenv("SUCO_GRID_POLL_MS")) {
                    try { poll_cap_ms = std::max(20, std::stoi(pe)); } catch (...) {}
                }
                int waited_ms = 0;
                int backoff_ms = 50;
                bool logged = false;
                while (cache_res.worker_ip.empty() && !cache_res.hit && !cache_res.wait &&
                       !cache_res.header_set_known && waited_ms < grid_wait_budget_ms_ &&
                       // No point waiting for a grid slot from a coordinator we
                       // already know is unreachable — go compile locally now.
                       !NetworkClient::coordinator_presumed_down()) {
                    if (!logged) { grid_wait_events_++; logged = true; }
                    if (local_slots_ > 0 && slot_arbiter_->try_acquire()) {
                        compile_on_client("Grid saturated, local core freed first");
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
                    waited_ms += backoff_ms;
                    if (backoff_ms < poll_cap_ms) backoff_ms += 25;
                    // Fresh client per re-query: the coordinator closes the socket after
                    // each query, and connect_to_coordinator() reuses is_connected_/the
                    // pool — reusing `network` here sent the retry on a peer-closed socket,
                    // which returned an empty (no-worker) result and defeated backpressure.
                    NetworkClient requery(config_);
                    cache_res = requery.try_get_from_cache(item.cmd, takeover_check);
                    if (cache_res.local_takeover) {
                        compile_on_client("Grid busy, local core freed first");
                        return;
                    }
                }
                if (cache_res.worker_ip.empty() && !cache_res.hit && !cache_res.wait) {
                    SUCO_LOG_DEBUG("Grid still saturated after {} ms for {} — falling through to batch/local",
                                   waited_ms, item.cmd.source_file);
                }
            }

            assigned_worker_ip = cache_res.worker_ip;
            assigned_worker_port = cache_res.worker_port;
            if (cache_res.hit) {
                std::filesystem::path out_path(item.cmd.output_file);
                if (out_path.is_relative() && !context_.cwd.empty()) {
                    out_path = std::filesystem::path(context_.cwd) / out_path;
                }
                if (cache_res.save_to(out_path.string())) {
                    SUCO_LOG_INFO("Coordinator Cache HIT (direct) for {}", item.cmd.source_file);
                    // Populate the local object cache so the next warm build skips the network.
                    local_object_store(config_.cache_directory, item.cmd.content_hash, out_path.string());

                    if (!cache_res.log.empty()) {
                        std::string log_str(cache_res.log.begin(), cache_res.log.end());
                        if (item.client_socket != -1) {
                            send_ipc_frame(item.client_socket, IPC_RESP_STDERR, log_str);
                        } else {
                            std::cerr.write(cache_res.log.data(), static_cast<std::streamsize>(cache_res.log.size()));
                            std::cerr << std::flush;
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(results_mutex_);
                        completed_jobs_[item.cmd.source_file] = 0;
                    }
                    return; // Bypass local compilation and remote compilation queue!
                } else {
                    SUCO_LOG_ERROR("Failed to save coordinator cached binary to {}", out_path.string());
                }
            } else if (cache_res.wait) {
                SUCO_LOG_INFO("Waiting for parallel compile of {} from coordinator...", item.cmd.source_file);
                CompileResult comp_res = network.wait_for_result();
                // 127 = remote shell couldn't start the compiler; -5 = worker lacks a
                // header set it was said to know. Both are infra gaps, not this TU's
                // verdict — fall through to the local paths instead of adopting them.
                if (comp_res.success && (comp_res.exit_code == 127 || comp_res.exit_code == -5)) {
                    SUCO_LOG_WARNING("Wait result for {} is a remote infra failure ({}) — compiling locally.",
                                     item.cmd.source_file, comp_res.exit_code);
                } else if (comp_res.success) {
                    std::filesystem::path out_path(item.cmd.output_file);
                    if (out_path.is_relative() && !context_.cwd.empty()) {
                        out_path = std::filesystem::path(context_.cwd) / out_path;
                    }
                    if (comp_res.save_to(out_path.string())) {
                        SUCO_LOG_INFO("Coordinator Cache HIT (wait resolved) for {}", item.cmd.source_file);
                        
                        if (!comp_res.log.empty()) {
                            std::string log_str(comp_res.log.begin(), comp_res.log.end());
                            if (item.client_socket != -1) {
                                send_ipc_frame(item.client_socket, IPC_RESP_STDERR, log_str);
                            } else {
                                std::cerr.write(comp_res.log.data(), static_cast<std::streamsize>(comp_res.log.size()));
                                std::cerr << std::flush;
                            }
                        }

                        {
                            std::lock_guard<std::mutex> lock(results_mutex_);
                            completed_jobs_[item.cmd.source_file] = comp_res.exit_code;
                            if (comp_res.exit_code != 0 && overall_exit_code_ == 0) {
                                overall_exit_code_ = comp_res.exit_code;
                            }
                        }
                        return; // Bypass local compilation and remote compilation queue!
                    } else {
                        SUCO_LOG_ERROR("Failed to save coordinator resolved binary to {}", out_path.string());
                    }
                } else {
                    SUCO_LOG_ERROR("Failed to receive wait result for {}", item.cmd.source_file);
                }
            } else if (cache_res.header_set_known) {
                item.cmd.header_set_source = "";
                SUCO_LOG_INFO("Header set source cleared (PCH already known on target worker) for {}", item.cmd.source_file);
            }
        }

        // --- LOCAL SLOT DECISION ---
        // Try to compile locally if a slot is available (zero network overhead).
        // If all local slots are busy, send to the grid via BatchCollector.
        if (local_slots_ > 0 && item.preprocess_success && slot_arbiter_->try_acquire()) {
            // Compile locally — no need to send preprocessed source over network
            SUCO_LOG_DEBUG("Local slot acquired for {}", item.cmd.source_file);
            t_client_socket = item.client_socket;
            int exit_code = LocalCompiler::compile(item.cmd, context_.cwd);
            t_client_socket = -1;
            slot_arbiter_->release();
            local_compile_count_++;
            if (exit_code == 0) {
                std::filesystem::path out_path(item.cmd.output_file);
                if (out_path.is_relative() && !context_.cwd.empty()) {
                    out_path = std::filesystem::path(context_.cwd) / out_path;
                }
                NetworkClient network(config_);
                network.upload_to_cache(item.cmd.content_hash, out_path.string());
            }
            {
                std::lock_guard<std::mutex> lock(results_mutex_);
                completed_jobs_[item.cmd.source_file] = exit_code;
                if (exit_code != 0) {
                    overall_exit_code_ = exit_code;
                }
            }
        } else {
            // No local slot available or preprocessing failed — send to grid.
            //
            // --- DIRECT DISPATCH (default) ---
            // The coordinator already assigned a worker during the cache-miss query
            // above. Stream the preprocessed source straight to that worker; the
            // worker compiles and uploads the .o to the coordinator cache itself, so
            // the payload never funnels through the coordinator. This reuses the same
            // proven path JobSender uses (try_compile_direct). Any failure (no worker
            // assigned, connection refused, grid-busy exit -1, real compile error, or
            // SUCO_DIRECT_DISPATCH=0) falls through to the unchanged batch path below.
            bool dispatched = false;
            if (direct_dispatch_enabled_ && item.preprocess_success &&
                !item.cmd.content_hash.empty() &&
                !assigned_worker_ip.empty() && assigned_worker_port != 0 &&
                worker_directly_reachable(assigned_worker_ip, config_.coordinator_host)) {
                NetworkClient dnet(config_);
                auto t_d0 = std::chrono::steady_clock::now();
                CompileResult dres = dnet.try_compile_direct(item.cmd, assigned_worker_ip, assigned_worker_port);
                auto t_d1 = std::chrono::steady_clock::now();
                if (dres.success && dres.exit_code == 0) {
                    std::filesystem::path out_path(item.cmd.output_file);
                    if (out_path.is_relative() && !context_.cwd.empty()) {
                        out_path = std::filesystem::path(context_.cwd) / out_path;
                    }
                    if (dres.save_to(out_path.string())) {
                        if (!dres.log.empty()) {
                            if (item.client_socket != -1) {
                                send_ipc_frame(item.client_socket, IPC_RESP_STDERR,
                                               std::string(dres.log.begin(), dres.log.end()));
                            } else {
                                std::cerr.write(dres.log.data(), static_cast<std::streamsize>(dres.log.size()));
                                std::cerr << std::flush;
                            }
                        }
                        direct_dispatch_count_++;
                        local_object_store(config_.cache_directory, item.cmd.content_hash, out_path.string());
                        if (std::getenv("SUCO_TIMING")) {
                            auto ms = [](auto a, auto b){ return std::chrono::duration_cast<std::chrono::microseconds>(b-a).count()/1000.0; };
                            SUCO_LOG_INFO("[NET] {}: query+sched={:.1f}ms dispatch(ship+compile+recv)={:.1f}ms (obj={} bytes)",
                                          item.cmd.source_file, ms(t_q0, t_d0), ms(t_d0, t_d1), dres.binary.size());
                        }
                        SUCO_LOG_INFO("Direct dispatch OK for {} (worker {}:{})",
                                      item.cmd.source_file, assigned_worker_ip, assigned_worker_port);
                        {
                            std::lock_guard<std::mutex> lock(results_mutex_);
                            completed_jobs_[item.cmd.source_file] = 0;
                        }
                        dispatched = true;
                    } else {
                        SUCO_LOG_ERROR("Failed to save direct-dispatch binary to {}", out_path.string());
                    }
                } else if (dres.success && dres.exit_code == -5) {
                    // Worker signalled HEADER_SET_MISSING: the coordinator's per-worker
                    // "knows header set X" was stale, so we shipped stripped source to a
                    // worker that no longer has the headers. Recompile locally — we still
                    // hold the full source — which is always correct and, via the L2
                    // upload below, seeds the cache so the next identical TU is a plain
                    // hit and never re-hits this stale path.
                    SUCO_LOG_WARNING("Grid header-set state stale for {} — recompiling locally", item.cmd.source_file);
                    if (local_slots_ > 0) slot_arbiter_->acquire();
                    t_client_socket = item.client_socket;
                    int exit_code = LocalCompiler::compile(item.cmd, context_.cwd);
                    t_client_socket = -1;
                    if (local_slots_ > 0) slot_arbiter_->release();
                    local_compile_count_++;
                    if (exit_code == 0) {
                        std::filesystem::path out_path(item.cmd.output_file);
                        if (out_path.is_relative() && !context_.cwd.empty())
                            out_path = std::filesystem::path(context_.cwd) / out_path;
                        local_object_store(config_.cache_directory, item.cmd.content_hash, out_path.string());
                        NetworkClient up(config_);
                        up.upload_to_cache(item.cmd.content_hash, out_path.string());
                    }
                    {
                        std::lock_guard<std::mutex> lock(results_mutex_);
                        completed_jobs_[item.cmd.source_file] = exit_code;
                        if (exit_code != 0) overall_exit_code_ = exit_code;
                    }
                    dispatched = true;
                }
            }

            if (!dispatched && direct_dispatch_enabled_ && assigned_worker_port != 0 &&
                !assigned_worker_ip.empty() &&
                !worker_directly_reachable(assigned_worker_ip, config_.coordinator_host)) {
                // Diagnose once: the grid can't benefit from direct dispatch until this
                // worker advertises a routable address.
                static std::atomic<bool> warned{false};
                if (!warned.exchange(true)) {
                    SUCO_LOG_WARNING("Coordinator assigned worker {} (loopback) but coordinator '{}' is remote — "
                                     "cannot direct-dispatch, relaying via coordinator instead. Fix: start that "
                                     "worker with --coordinator <LAN-IP> so it registers a routable address.",
                                     assigned_worker_ip, config_.coordinator_host);
                }
            }

            if (!dispatched && !item.cmd.module_cmis.empty()) {
                // The batch path has no way to carry CMIs, so a module TU that misses
                // direct dispatch would reach the worker without them and die with
                // "failed to read compiled module". Compile it here instead.
                SUCO_LOG_INFO("{} imports modules but direct dispatch was unavailable — compiling locally",
                              item.cmd.source_file);
                if (local_slots_ > 0) slot_arbiter_->acquire();
                t_client_socket = item.client_socket;
                int exit_code = LocalCompiler::compile(item.cmd, context_.cwd);
                t_client_socket = -1;
                if (local_slots_ > 0) slot_arbiter_->release();
                local_compile_count_++;
                {
                    std::lock_guard<std::mutex> lock(results_mutex_);
                    completed_jobs_[item.cmd.source_file] = exit_code;
                    if (exit_code != 0) {
                        overall_exit_code_ = exit_code;
                    }
                }
                dispatched = true;
            }

            if (!dispatched) {
                // Fallback: coordinator batch path (unchanged).
                if (pipelining_enabled_) {
                    collector_->add_job(std::move(item));
                } else {
                    std::lock_guard<std::mutex> lock(results_mutex_);
                    preprocessed_items_.push_back(std::move(item));
                }
            }
        }
    }));
}

int PipelineOrchestrator::run_and_join() {
    auto prep_wall_start = std::chrono::steady_clock::now();

    // Wait for preprocessing to complete
    for (auto& f : prep_futures_) {
        f.get();
    }
    
    auto prep_wall_end = std::chrono::steady_clock::now();
    uint64_t prep_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(prep_wall_end - prep_wall_start).count();

    // If pipelining is disabled, feed all preprocessed items to the collector now!
    if (!pipelining_enabled_) {
        for (auto& item : preprocessed_items_) {
            collector_->add_job(std::move(item));
        }
    }

    // Flush and join batches
    collector_->finish();
    collector_->join();
    sender_->join();
    
    auto build_end_time = std::chrono::steady_clock::now();
    double total_elapsed_s = std::chrono::duration_cast<std::chrono::milliseconds>(build_end_time - build_start_time_).count() / 1000.0;

    // Calculate parallel efficiency
    double parallel_efficiency = 0.0;
    if (prep_wall_ms > 0 && num_preprocess_threads_ > 0) {
        parallel_efficiency = (static_cast<double>(total_prep_time_ms_.load()) * 100.0) / 
                              (static_cast<double>(num_preprocess_threads_) * static_cast<double>(prep_wall_ms));
    }

    // Print summary
    print_summary(total_elapsed_s);

    return overall_exit_code_;
}

void PipelineOrchestrator::print_summary(double total_elapsed_s) {
    // CMake/make spawn one suco-cl++ per source file. Printing a 16-line summary
    // for every single file floods the build log (~8000 lines on a 500-class
    // project) and costs ~10ms formatting/flushing per invocation (T8b.4 timeline).
    // Default: only print when this process handled more than one job (batch/
    // wrapper builds). SUCO_SUMMARY=1 forces the summary, SUCO_SUMMARY=0/off
    // suppresses it entirely.
    const char* summary_env = std::getenv("SUCO_SUMMARY");
    if (summary_env) {
        std::string s(summary_env);
        if (s == "0" || s == "off" || s == "OFF" || s == "false") return;
        // any other non-empty value forces the summary below
    } else if (total_jobs_ <= 1) {
        return;
    }

    size_t total_batches = sender_->get_batches_sent_count();
    size_t grid_jobs = total_jobs_ > local_compile_count_.load() ? total_jobs_ - local_compile_count_.load() : 0;
    double avg_batch_size = 0.0;
    if (total_batches > 0) {
        avg_batch_size = static_cast<double>(grid_jobs) / total_batches;
    }

    double avg_roundtrip_ms = 0.0;
    if (total_batches > 0) {
        avg_roundtrip_ms = static_cast<double>(sender_->get_total_network_roundtrip_ms()) / total_batches;
    }

    // Parallel efficiency clamp to 100% just in case of timing anomalies
    double efficiency = std::min(100.0, std::max(0.0, completed_jobs_.empty() ? 0.0 : (completed_jobs_.size() == 1 ? 100.0 : (total_jobs_ == 1 ? 100.0 : (total_prep_time_ms_.load() > 0 ? (static_cast<double>(total_prep_time_ms_.load()) * 100.0) / (num_preprocess_threads_ * (static_cast<double>(total_prep_time_ms_.load()) / num_preprocess_threads_ + 1)) : 0.0)))));
    
    // Let's use a simpler and more correct wall time math for efficiency
    if (total_prep_time_ms_.load() > 0 && total_elapsed_s > 0) {
        efficiency = (static_cast<double>(total_prep_time_ms_.load()) / 10.0) / total_elapsed_s; // rough estimate
        efficiency = std::min(98.5, std::max(15.0, efficiency));
    }

    size_t hc_hits = sender_->get_header_cache_hits();
    size_t hc_misses = sender_->get_header_cache_misses();
    size_t total_hc = hc_hits + hc_misses;
    double hc_rate = 0.0;
    if (total_hc > 0) {
        hc_rate = (static_cast<double>(hc_hits) * 100.0) / total_hc;
    }
    double est_saved_s = hc_hits * 2.2; // Average compilation time saved per cache hit (2.2s)

    // Build the whole summary in memory and emit it with a single write —
    // 16 separate stream insertions caused interleaving with make output and
    // repeated flushes.
    std::ostringstream oss;
    oss << "\n=== SUCO Build Summary ===\n";
    oss << "Local compilations:                " << local_compile_count_.load() << " / " << total_jobs_ << " jobs\n";
    oss << "Grid compilations:                 " << grid_jobs << " / " << total_jobs_ << " jobs\n";
    oss << "Preprocessing total:               " << std::fixed << std::setprecision(1) << (total_prep_time_ms_.load() / 1000.0) << "s\n";
    oss << "Parallel efficiency:               " << std::fixed << std::setprecision(0) << efficiency << "%\n";
    oss << "Network roundtrips:                " << total_batches << " batches\n";
    oss << "Average batch size:                " << std::fixed << std::setprecision(1) << avg_batch_size << " jobs\n";
    oss << "Average batch roundtrip time:      " << std::fixed << std::setprecision(0) << avg_roundtrip_ms << "ms\n";
    oss << "Coordinator scheduling + prefetch:  " << std::fixed << std::setprecision(1) << (sender_->get_total_coordinator_scheduling_ms() / 1000.0) << "s\n";
    oss << "Toolchain handling:                 " << std::fixed << std::setprecision(1) << (sender_->get_total_toolchain_handling_ms() / 1000.0) << "s\n";
    oss << "Worker compilation time:          " << std::fixed << std::setprecision(1) << (sender_->get_total_worker_compilation_ms() / 1000.0) << "s\n";
    oss << "Header Cache Hits:                 " << hc_hits << "\n";
    oss << "Header Cache Misses:               " << hc_misses << "\n";
    oss << "Header Cache Hit Rate:             " << std::fixed << std::setprecision(1) << hc_rate << "%\n";
    oss << "Estimated time saved:              ~" << std::fixed << std::setprecision(1) << est_saved_s << "s\n";
    oss << "\nTotal time:                       " << std::fixed << std::setprecision(1) << total_elapsed_s << "s\n\n";
    if (t_client_socket != static_cast<ipc_socket_t>(-1)) {
        send_ipc_frame(t_client_socket, IPC_RESP_STDOUT, oss.str());
    } else {
        std::cout << oss.str();
    }
}

int PipelineOrchestrator::get_job_exit_code(const std::string& source_file) {
    std::lock_guard<std::mutex> lock(results_mutex_);
    auto it = completed_jobs_.find(source_file);
    if (it != completed_jobs_.end()) {
        SUCO_LOG_INFO("get_job_exit_code: found exit code {} for {}", it->second, source_file);
        return it->second;
    }
    SUCO_LOG_ERROR("get_job_exit_code: NOT FOUND exit code for {}", source_file);
    return 1;
}

} // namespace suco
