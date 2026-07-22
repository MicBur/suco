#include "header_cache.h"
#include "job_executor.h"
#include "toolchain_manager.h"
#include "logging.h"
#include "hash_util.h"
#include <fstream>
#include "platform_compat.h"
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <map>
#include <system_error>

namespace suco::worker {

HeaderCache& HeaderCache::get_instance() {
    static HeaderCache instance;
    return instance;
}

void HeaderCache::initialize(const std::string& cache_dir, int max_size_gb) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_dir_ = cache_dir;
    max_size_bytes_ = static_cast<size_t>(max_size_gb) * 1024ULL * 1024 * 1024;

    std::error_code ec;
    if (!std::filesystem::exists(cache_dir_, ec)) {
        std::filesystem::create_directories(cache_dir_, ec);
    }
}

bool HeaderCache::has(const std::string& hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_dir_.empty() || hash.empty()) return false;

    std::filesystem::path gch_path = std::filesystem::path(cache_dir_) / hash / "header.gch";
    std::error_code ec;
    return std::filesystem::exists(gch_path, ec);
}

bool HeaderCache::get(const std::string& hash, std::string& pch_path_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_dir_.empty() || hash.empty()) return false;

    // Succeeds as soon as the header TEXT is here — the .gch is optional. We hand GCC the
    // path of the text file either way; GCC picks up header.gch automatically when it
    // exists and reads the text when it does not. That decouples "this worker can compile
    // the job" from "a PCH was built", which is what lets the PCH build wait until it
    // actually amortises (see JobExecutor / note_use).
    std::filesystem::path hash_dir = std::filesystem::path(cache_dir_) / hash;
    std::error_code ec;
    if (std::filesystem::exists(hash_dir / "header", ec)) {
        pch_path_out = (hash_dir / "header").string();
        return true;
    }
    return false;
}

bool HeaderCache::has_pch(const std::string& hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_dir_.empty() || hash.empty()) return false;
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(cache_dir_) / hash / "header.gch", ec);
}

bool HeaderCache::store_source(const std::string& hash, const std::string& preprocessed_header,
                               const std::string& orig_cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_dir_.empty() || hash.empty() || preprocessed_header.empty()) return false;

    std::filesystem::path hash_dir = std::filesystem::path(cache_dir_) / hash;
    std::filesystem::path header_file = hash_dir / "header";
    std::error_code ec;
    if (std::filesystem::exists(header_file, ec)) return true;   // already stored

    std::filesystem::create_directories(hash_dir, ec);
    std::string filtered = orig_cmd.find("-fdirectives-only") != std::string::npos
                               ? suco::strip_predefined_macros(preprocessed_header)
                               : preprocessed_header;
    // Write via a temp file + rename: a concurrent job must never -include a half-written
    // header (they share this directory by hash).
    std::filesystem::path tmp = hash_dir / ("header.tmp." + std::to_string(::getpid()));
    std::ofstream out(tmp, std::ios::binary);
    if (!out.is_open()) return false;
    out.write(filtered.data(), filtered.size());
    out.close();
    std::filesystem::rename(tmp, header_file, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

bool HeaderCache::get_source(const std::string& hash, std::string& source_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_dir_.empty() || hash.empty()) return false;

    std::filesystem::path header_file = std::filesystem::path(cache_dir_) / hash / "header";
    std::error_code ec;
    if (!std::filesystem::exists(header_file, ec)) return false;

    std::ifstream in(header_file, std::ios::binary);
    if (!in.is_open()) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    source_out = ss.str();
    return !source_out.empty();
}

int HeaderCache::note_use(const std::string& hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ++use_counts_[hash];
}

bool HeaderCache::store(const std::string& hash,
                        const std::string& preprocessed_header,
                        const std::string& orig_cmd,
                        const std::string& toolchain_hash) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (cache_dir_.empty() || hash.empty() || preprocessed_header.empty()) return false;

    // Wait if another thread is already compiling this PCH
    cv_.wait(lock, [this, &hash]() { return !active_compilations_.contains(hash); });

    // Check again if it was compiled by the other thread while we waited
    std::filesystem::path hash_dir = std::filesystem::path(cache_dir_) / hash;
    std::filesystem::path gch_file = hash_dir / "header.gch";
    std::error_code ec;
    if (std::filesystem::exists(gch_file, ec)) {
        return true;
    }

    // Register active compilation
    active_compilations_.insert(hash);

    // Unlock during compiler execution to allow parallel compilations of different hashes!
    lock.unlock();

    std::filesystem::create_directories(hash_dir, ec);

    std::filesystem::path header_file = hash_dir / "header";

    // Write header source to file
    std::string filtered_header;
    if (orig_cmd.find("-fdirectives-only") != std::string::npos) {
        filtered_header = suco::strip_predefined_macros(preprocessed_header);
    } else {
        filtered_header = preprocessed_header;
    }

    std::ofstream out(header_file, std::ios::binary);
    if (!out.is_open()) {
        SUCO_LOG_ERROR("HeaderCache: Failed to create header source file at {}", header_file.string());
        return false;
    }
    out.write(filtered_header.data(), filtered_header.size());
    out.close();

    // Reconstruct compiler PCH command
    std::stringstream ss(orig_cmd);
    std::string word;
    std::string pch_cmd;
    bool skip_next = false;
    bool is_first = true;

    while (ss >> word) {
        if (skip_next) {
            skip_next = false;
            continue;
        }

        // Remove output and compile-only flags
        if (word == "-o") {
            skip_next = true;
            continue;
        }
        if (word == "-c" || word == "-") {
            continue;
        }

        if (is_first) {
            is_first = false;
            if (!toolchain_hash.empty()) {
                std::string tc_path = ToolchainManager::get_toolchain_path(toolchain_hash);
                if (word.starts_with("/")) {
                    word = tc_path + word;
                } else {
                    word = tc_path + "/" + word;
                }
            }
        }
        pch_cmd += word + " ";
    }

    // GCC requires -x c++-header to compile PCH from preprocessed source
    pch_cmd += " -x c++-header -c \"" + header_file.string() + "\" -o \"" + gch_file.string() + "\"";

    int exit_code = 0;
    std::string log = JobExecutor::run_local_capture(pch_cmd, exit_code, 30); // 30s timeout

    // Re-lock to cleanup active compilations and run eviction
    lock.lock();
    active_compilations_.erase(hash);
    cv_.notify_all();

    if (exit_code != 0) {
        SUCO_LOG_WARNING("HeaderCache: Failed to compile PCH for hash {} (Exit: {}). Log:\n{}", hash, exit_code, log);
        std::filesystem::remove_all(hash_dir, ec);
        return false;
    }

    // Exit 0 is NOT proof the PCH exists: with -fmodules-ts, GCC treats `-x c++-header`
    // as a header *unit*, writes it to gcm.cache/ and IGNORES -o — succeeding without
    // ever producing header.gch. Reporting success then makes get() fail forever, and
    // the job compiles stripped source with no headers. Trust the file, not the exit code.
    if (!std::filesystem::exists(gch_file, ec)) {
        SUCO_LOG_WARNING("HeaderCache: compiler reported success but produced no PCH at {} "
                         "(header units / -fmodules-ts?). Disabling header cache for hash {}.",
                         gch_file.string(), hash);
        std::filesystem::remove_all(hash_dir, ec);
        return false;
    }

    SUCO_LOG_INFO("HeaderCache: Successfully stored PCH for hash {}", hash);

    // Run eviction in background/inline after successful storage
    cleanup_lru();
    return true;
}

void HeaderCache::cleanup_lru() {
    // Collect all cached hash directories with their last modification times
    struct CacheItem {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
        size_t size = 0;
    };

    std::vector<CacheItem> items;
    size_t total_size = 0;
    std::error_code ec;

    if (!std::filesystem::exists(cache_dir_, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(cache_dir_, ec)) {
        if (entry.is_directory(ec)) {
            std::filesystem::path gch_file = entry.path() / "header.gch";
            if (std::filesystem::exists(gch_file, ec)) {
                CacheItem item;
                item.path = entry.path();
                item.mtime = std::filesystem::last_write_time(gch_file, ec);
                
                // Calculate folder size
                for (const auto& sub : std::filesystem::directory_iterator(entry.path(), ec)) {
                    if (sub.is_regular_file(ec)) {
                        item.size += std::filesystem::file_size(sub.path(), ec);
                    }
                }
                
                total_size += item.size;
                items.push_back(item);
            }
        }
    }

    // If we exceed our size limit, evict oldest items (LRU)
    if (total_size > max_size_bytes_) {
        std::sort(items.begin(), items.end(), [](const CacheItem& a, const CacheItem& b) {
            return a.mtime < b.mtime; // Oldest first
        });

        for (const auto& item : items) {
            if (total_size <= max_size_bytes_) break;

            SUCO_LOG_INFO("HeaderCache: Evicting cached PCH {} (size: {} MB) to free disk space", 
                          item.path.filename().string(), item.size / (1024 * 1024));
            
            std::filesystem::remove_all(item.path, ec);
            total_size -= item.size;
        }
    }
}

void HeaderCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_dir_.empty()) return;
    std::error_code ec;
    SUCO_LOG_INFO("HeaderCache: Clearing all cached PCH files from {}...", cache_dir_);
    if (std::filesystem::exists(cache_dir_, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_, ec)) {
            std::filesystem::remove_all(entry.path(), ec);
        }
    }
    SUCO_LOG_INFO("HeaderCache: Cache cleared successfully.");
}

} // namespace suco::worker
