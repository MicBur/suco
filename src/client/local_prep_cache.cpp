#include "local_prep_cache.h"
#include "logging.h"
#include "hash_util.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <mutex>

namespace suco {

namespace {

std::mutex g_prep_cache_mutex;

struct FileState {
    uint64_t mtime = 0;
    uint64_t size = 0;
    bool exists = false;
};

FileState get_file_state(const std::string& path, const std::string& cwd = "") {
    FileState state;
    std::error_code ec;
    std::filesystem::path p(path);
    if (!cwd.empty() && p.is_relative()) {
        p = std::filesystem::path(cwd) / p;
    }
    if (std::filesystem::exists(p, ec)) {
        state.exists = true;
        auto ftime = std::filesystem::last_write_time(p, ec);
        if (!ec) {
            state.mtime = ftime.time_since_epoch().count();
        }
        auto sz = std::filesystem::file_size(p, ec);
        if (!ec) {
            state.size = sz;
        }
    }
    return state;
}

std::string compute_local_key(const CompilerCommand& cmd) {
    // We compute local_key by hashing: Source File Path + Toolchain Hash + Compiler Flags
    std::stringstream input;
    input << cmd.source_file << "\x1F"
          << cmd.toolchain_hash << "\x1F"
          << cmd.language_standard << "\x1F";
          
    for (const auto& d : cmd.defines) input << d << "\x1F";
    for (const auto& i : cmd.include_paths) input << i << "\x1F";
    for (const auto& f : cmd.other_flags) input << f << "\x1F";
    
    return suco::compute_sha256(input.str());
}

std::string compute_cache_key(const CompilerCommand& cmd, const std::string& header_set_hash, const std::string& content_hash = "") {
    // Cache-Key is built from: content_hash + Toolchain Hash + Flags + header_set_hash
    // content_hash MUST be included to prevent different source files with identical
    // flags from sharing the same .source cache file (which would corrupt builds).
    std::stringstream input;
    input << content_hash << "\x1F"
          << cmd.toolchain_hash << "\x1F"
          << cmd.language_standard << "\x1F";
          
    for (const auto& d : cmd.defines) input << d << "\x1F";
    for (const auto& i : cmd.include_paths) input << i << "\x1F";
    for (const auto& f : cmd.other_flags) input << f << "\x1F";
    input << header_set_hash;
    
    std::string key = suco::compute_sha256(input.str());
    SUCO_LOG_DEBUG("compute_cache_key: file={}, content_hash={}, header_set_hash={}, key={}", 
                   cmd.source_file, content_hash, header_set_hash, key);
    return key;
}

} // namespace

bool LocalPrepCache::try_get(
    const ClientConfig& config,
    const CompilerCommand& cmd,
    std::string& preprocessed_source,
    std::string& content_hash,
    std::string& header_set_hash,
    std::string& header_set_source,
    const RequestContext& context
) {
    if (!config.local_prep_cache_enabled) {
        return false;
    }

    std::string local_key = compute_local_key(cmd);
    std::filesystem::path cache_dir = std::filesystem::path(config.cache_directory) / "preprocess";
    std::filesystem::path manifest_path = cache_dir / (local_key + ".manifest");
    
    std::error_code ec;
    if (!std::filesystem::exists(manifest_path, ec)) {
        return false;
    }
    
    std::ifstream in(manifest_path);
    if (!in.is_open()) {
        return false;
    }
    
    std::string line;
    // V2: manifests written before the __DATE__/__TIME__ cache guard may carry
    // hashes of time-macro TUs — force re-preprocessing so the guard applies.
    if (!std::getline(in, line) || line != "SUCO_PREP_MANIFEST_V2") {
        return false;
    }
    
    std::string cached_content_hash;
    std::string cached_header_set_hash;
    size_t dep_count = 0;
    
    while (std::getline(in, line)) {
        if (line.starts_with("content_hash:")) {
            cached_content_hash = line.substr(13);
        } else if (line.starts_with("header_set_hash:")) {
            cached_header_set_hash = line.substr(16);
        } else if (line.starts_with("dependency_count:")) {
            try {
                dep_count = std::stoul(line.substr(17));
            } catch (...) {
                return false;
            }
        } else if (line == "deps:") {
            break;
        }
    }
    
    // Verify dependencies
    for (size_t i = 0; i < dep_count; ++i) {
        if (!std::getline(in, line)) {
            return false;
        }
        
        size_t first_semi = line.find(';');
        if (first_semi == std::string::npos) return false;
        size_t second_semi = line.find(';', first_semi + 1);
        if (second_semi == std::string::npos) return false;
        
        std::string dep_path = line.substr(0, first_semi);
        std::string mtime_str = line.substr(first_semi + 1, second_semi - first_semi - 1);
        std::string size_str = line.substr(second_semi + 1);
        
        uint64_t dep_mtime = 0;
        uint64_t dep_size = 0;
        try {
            dep_mtime = std::stoull(mtime_str);
            dep_size = std::stoull(size_str);
        } catch (...) {
            return false;
        }
        
        FileState state = get_file_state(dep_path, context.cwd);
        if (!state.exists || state.mtime != dep_mtime || state.size != dep_size) {
            SUCO_LOG_DEBUG("Local preprocessor cache miss: Dependency changed or missing: {}", dep_path);
            return false;
        }
    }
    
    std::string cache_key = compute_cache_key(cmd, cached_header_set_hash, cached_content_hash);
    
    // Load preprocessed source
    std::filesystem::path source_path = cache_dir / (cache_key + ".source");
    std::ifstream src_file(source_path, std::ios::binary);
    if (!src_file.is_open()) {
        return false;
    }
    std::stringstream src_ss;
    src_ss << src_file.rdbuf();
    preprocessed_source = src_ss.str();
    
    // Load header set source if applicable
    if (!cached_header_set_hash.empty()) {
        std::filesystem::path hs_path = cache_dir / (cache_key + ".hssource");
        std::ifstream hs_file(hs_path, std::ios::binary);
        if (hs_file.is_open()) {
            std::stringstream hs_ss;
            hs_ss << hs_file.rdbuf();
            header_set_source = hs_ss.str();
        }
    }
    
    content_hash = cached_content_hash;
    header_set_hash = cached_header_set_hash;
    return true;
}

void LocalPrepCache::store(
    const ClientConfig& config,
    const CompilerCommand& cmd,
    const std::string& raw_preprocessed,
    const std::string& preprocessed_source,
    const std::string& content_hash,
    const std::string& header_set_hash,
    const std::string& header_set_source,
    const RequestContext& context
) {
    if (!config.local_prep_cache_enabled) {
        return;
    }

    std::string local_key = compute_local_key(cmd);
    std::string cache_key = compute_cache_key(cmd, header_set_hash, content_hash);
    std::filesystem::path cache_dir = std::filesystem::path(config.cache_directory) / "preprocess";
    
    std::lock_guard<std::mutex> lock(g_prep_cache_mutex);
    
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) return;
    
    // Extract include dependencies from raw preprocessed source
    std::vector<std::string> include_files = extract_includes_from_preprocessed(raw_preprocessed, cmd.is_msvc);
    
    std::vector<std::string> all_deps;
    all_deps.reserve(include_files.size() + 1);
    
    std::string main_path = cmd.source_file;
    std::replace(main_path.begin(), main_path.end(), '\\', '/');
    all_deps.push_back(main_path);
    
    for (const auto& path : include_files) {
        if (path != main_path) {
            all_deps.push_back(path);
        }
    }
    
    struct DepEntry {
        std::string path;
        uint64_t mtime;
        uint64_t size;
    };
    std::vector<DepEntry> entries;
    entries.reserve(all_deps.size());
    
    for (const auto& dep : all_deps) {
        FileState state = get_file_state(dep, context.cwd);
        if (state.exists) {
            entries.push_back(DepEntry{ .path = dep, .mtime = state.mtime, .size = state.size });
        }
    }
    
    // Write manifest
    std::filesystem::path manifest_path = cache_dir / (local_key + ".manifest");
    std::ofstream out(manifest_path);
    if (!out.is_open()) return;
    
    out << "SUCO_PREP_MANIFEST_V2\n"
        << "content_hash:" << content_hash << "\n"
        << "header_set_hash:" << header_set_hash << "\n"
        << "dependency_count:" << entries.size() << "\n"
        << "deps:\n";
        
    for (const auto& entry : entries) {
        out << entry.path << ";" << entry.mtime << ";" << entry.size << "\n";
    }
    out.close();
    
    // Write preprocessed source
    std::filesystem::path source_path = cache_dir / (cache_key + ".source");
    std::ofstream src_file(source_path, std::ios::binary);
    if (src_file.is_open()) {
        src_file.write(preprocessed_source.data(), preprocessed_source.size());
        src_file.close();
    }
    
    // Write header set source if PCH active
    if (!header_set_hash.empty()) {
        std::filesystem::path hs_path = cache_dir / (cache_key + ".hssource");
        std::ofstream hs_file(hs_path, std::ios::binary);
        if (hs_file.is_open()) {
            hs_file.write(header_set_source.data(), header_set_source.size());
            hs_file.close();
        }
    }
}

void LocalPrepCache::clear(const ClientConfig& config) {
    std::filesystem::path cache_dir = std::filesystem::path(config.cache_directory) / "preprocess";
    std::lock_guard<std::mutex> lock(g_prep_cache_mutex);
    std::error_code ec;
    if (std::filesystem::exists(cache_dir, ec)) {
        std::filesystem::remove_all(cache_dir, ec);
    }
}

std::vector<std::string> extract_includes_from_preprocessed(const std::string& content, bool is_msvc) {
    std::vector<std::string> paths;
    paths.reserve(256);
    
    size_t pos = 0;
    size_t len = content.length();
    while (pos < len) {
        if (content[pos] == '#') {
            size_t eol = content.find('\n', pos);
            if (eol == std::string::npos) eol = len;
            
            std::string_view line(&content[pos], eol - pos);
            
            size_t first_quote = line.find('"');
            if (first_quote != std::string_view::npos) {
                size_t second_quote = line.find('"', first_quote + 1);
                if (second_quote != std::string_view::npos) {
                    std::string path(line.substr(first_quote + 1, second_quote - first_quote - 1));
                    std::replace(path.begin(), path.end(), '\\', '/');
                    if (!path.empty() && 
                        path != "<built-in>" && 
                        path != "<command-line>" && 
                        path != "<command line>" && 
                        path != "<stdin>") {
                        paths.push_back(path);
                    }
                }
            }
            pos = eol + 1;
        } else {
            size_t eol = content.find('\n', pos);
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
    }
    
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    return paths;
}

} // namespace suco
