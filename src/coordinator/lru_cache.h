#pragma once

#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
    #include <direct.h>
    #define getcwd _getcwd
#else
    #include <unistd.h>
#endif

namespace suco {

class LruCache {
private:
    std::string cache_dir_;
    uint64_t max_size_bytes_;
    std::mutex mutex_;

    // Helper to get platform-specific last access time
    time_t get_last_access_time(const std::string& path) {
        struct stat result;
        if (stat(path.c_str(), &result) == 0) {
            return result.st_atime; // Last access time
        }
        return 0;
    }

    // Helper to expand environment variables or home directory
    std::string expand_path(const std::string& path) {
#ifdef _WIN32
        // Under Windows, expand %LOCALAPPDATA%
        if (path.find("%LOCALAPPDATA%") != std::string::npos) {
            const char* local_app_data = std::getenv("LOCALAPPDATA");
            if (local_app_data != nullptr) {
                std::string expanded(local_app_data);
                std::string rest = path.substr(std::string("%LOCALAPPDATA%").size());
                return expanded + rest;
            }
            // Fallback to current directory if environment variable is not set
            return "./suco_cache";
        }
#else
        // Under Linux, expand ~
        if (!path.empty() && path[0] == '~') {
            const char* home = std::getenv("HOME");
            if (home) {
                return std::string(home) + path.substr(1);
            }
            return "./suco_cache";
        }
#endif
        return path;
    }

public:
    LruCache(const std::string& raw_cache_dir, uint64_t max_size_bytes)
        : max_size_bytes_(max_size_bytes) {
        cache_dir_ = expand_path(raw_cache_dir);
        std::filesystem::create_directories(cache_dir_);
        std::cout << "suco-coordinator: Cache initialized at " << cache_dir_ 
                  << " (Limit: " << (max_size_bytes_ / (1024 * 1024)) << " MB)" << std::endl;
    }

    // Retrieve file paths for a given SHA-256 hash (two-level directory structure)
    // Structure: cache_dir/ab/cdef1234...o (and .log for warnings)
    std::pair<std::string, std::string> get_cache_paths(const std::string& hash) {
        if (hash.size() < 2) return {"", ""};
        std::string dir = hash.substr(0, 2);
        std::string rest = hash.substr(2);
        
        std::filesystem::path subdir = std::filesystem::path(cache_dir_) / dir;
        std::string obj_path = (subdir / (rest + ".o")).string();
        std::string log_path = (subdir / (rest + ".log")).string();
        return {obj_path, log_path};
    }

    // Check if hash is cached
    bool get(const std::string& hash, std::vector<uint8_t>& obj_data, std::string& log_data, uint8_t& bin_comp) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto paths = get_cache_paths(hash);
        if (paths.first.empty()) return false;

        // Check if object file exists
        if (!std::filesystem::exists(paths.first)) {
            return false;
        }

        // Read object binary data
        std::ifstream obj_file(paths.first, std::ios::binary | std::ios::ate);
        if (!obj_file.is_open()) return false;
        std::streamsize size = obj_file.tellg();
        if (size < 1) return false;
        obj_file.seekg(0, std::ios::beg);
        
        uint8_t flag = 0;
        if (!obj_file.read(reinterpret_cast<char*>(&flag), 1)) {
            return false;
        }
        bin_comp = flag;

        obj_data.resize(size - 1);
        if (!obj_file.read(reinterpret_cast<char*>(obj_data.data()), size - 1)) {
            return false;
        }
        obj_file.close();

        // Read log warning data (optional)
        if (std::filesystem::exists(paths.second)) {
            std::ifstream log_file(paths.second);
            if (log_file.is_open()) {
                std::stringstream ss;
                ss << log_file.rdbuf();
                log_data = ss.str();
                log_file.close();
            }
        }

        // Update last access time (touch the file)
        // Under C++17 filesystem we can use last_write_time, but to avoid compiler warning issues 
        // with clock types, we can simply touch it by updating last_write_time to now.
        try {
            std::filesystem::last_write_time(paths.first, std::filesystem::file_time_type::clock::now());
        } catch (...) {}

        return true;
    }

    // Helper to escape special characters for JSON format
    std::string escape_json(const std::string& input) {
        std::string output;
        output.reserve(input.size());
        for (char c : input) {
            if (c == '\\') {
                output += "\\\\";
            } else if (c == '"') {
                output += "\\\"";
            } else if (c == '/') {
                output += "\\/";
            } else if (c == '\b') {
                output += "\\b";
            } else if (c == '\f') {
                output += "\\f";
            } else if (c == '\n') {
                output += "\\n";
            } else if (c == '\r') {
                output += "\\r";
            } else if (c == '\t') {
                output += "\\t";
            } else {
                output += c;
            }
        }
        return output;
    }

    // Store compiled results to cache
    void put(const std::string& hash, const std::vector<uint8_t>& obj_data, const std::string& log_data,
             uint8_t bin_comp, const std::string& source_file = "", const std::string& compiler_command = "",
             const std::string& metadata_json = "") {
        std::lock_guard<std::mutex> lock(mutex_);
        auto paths = get_cache_paths(hash);
        if (paths.first.empty()) return;

        // Ensure subdirectories exist (e.g. cache_dir/ab/)
        std::filesystem::create_directories(std::filesystem::path(paths.first).parent_path());

        // Write object binary
        std::ofstream obj_file(paths.first, std::ios::binary);
        if (obj_file.is_open()) {
            obj_file.write(reinterpret_cast<const char*>(&bin_comp), 1);
            obj_file.write(reinterpret_cast<const char*>(obj_data.data()), obj_data.size());
            obj_file.close();
        }

        // Write log warning data if present
        if (!log_data.empty()) {
            std::ofstream log_file(paths.second);
            if (log_file.is_open()) {
                log_file << log_data;
                log_file.close();
            }
        }

        // Write meta file with enriched metadata
        std::string meta_path = paths.first.substr(0, paths.first.size() - 2) + ".meta";
        std::ofstream meta_file(meta_path);
        if (meta_file.is_open()) {
            auto now = std::chrono::system_clock::now();
            auto epoch = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
            meta_file << "{\n";
            meta_file << "  \"cache_version\": \"v1\",\n";
            meta_file << "  \"hash\": \"" << hash << "\",\n";
            meta_file << "  \"source_file\": \"" << escape_json(source_file) << "\",\n";
            meta_file << "  \"compiler_command\": \"" << escape_json(compiler_command) << "\",\n";
            meta_file << "  \"timestamp\": " << epoch << ",\n";
            meta_file << "  \"binary_size\": " << obj_data.size();
            // Append extra metadata if provided (compiler_version, target, etc.)
            if (!metadata_json.empty()) {
                meta_file << ",\n  " << metadata_json;
            }
            meta_file << "\n}\n";
            meta_file.close();
        }

        // Trigger LRU Cleanup in background if threshold is crossed
        clean_up_lru();
    }

    // Clear all files from cache directory
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) {
                std::filesystem::remove_all(entry.path());
            }
            std::cout << "suco-coordinator: Cache cleared successfully" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "suco-coordinator cache error: Failed to clear cache: " << e.what() << std::endl;
        }
    }

    // Get the node that compiled a cache entry from its .meta file
    std::string get_meta_node(const std::string& hash) {
        auto paths = get_cache_paths(hash);
        if (paths.first.empty()) return "";
        std::string meta_path = paths.first.substr(0, paths.first.size() - 2) + ".meta";
        if (!std::filesystem::exists(meta_path)) return "";
        std::ifstream meta_file(meta_path);
        if (!meta_file.is_open()) return "";
        std::string line;
        while (std::getline(meta_file, line)) {
            size_t pos = line.find("\"node\":");
            if (pos != std::string::npos) {
                size_t start = line.find("\"", pos + 7);
                if (start != std::string::npos) {
                    size_t end = line.find("\"", start + 1);
                    if (end != std::string::npos) {
                        return line.substr(start + 1, end - start - 1);
                    }
                }
            }
        }
        return "";
    }

    // Scan directory and perform LRU eviction if size limit is exceeded

    void clean_up_lru() {
        uint64_t current_size = 0;
        struct CacheFile {
            std::string path;
            uint64_t size;
            time_t last_access;
        };
        std::vector<CacheFile> files;

        // Recursively find all files in the cache directory
        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(cache_dir_)) {
                if (entry.is_regular_file()) {
                    std::string p = entry.path().string();
                    uint64_t sz = entry.file_size();
                    current_size += sz;
                    files.push_back({p, sz, get_last_access_time(p)});
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "suco-coordinator cache error: Failed to scan directory: " << e.what() << std::endl;
            return;
        }

        if (current_size <= max_size_bytes_) {
            return; // Cache limit is not exceeded!
        }

        std::cout << "suco-coordinator: Cache size exceeded (" << (current_size / (1024 * 1024)) 
                  << " MB / " << (max_size_bytes_ / (1024 * 1024)) << " MB). Evicting..." << std::endl;

        // Sort files by last access time (oldest first)
        std::sort(files.begin(), files.end(), [](const CacheFile& a, const CacheFile& b) {
            return a.last_access < b.last_access;
        });

        // Delete oldest files until we are down to 80% of max size
        uint64_t target_size = static_cast<uint64_t>(max_size_bytes_ * 0.8);
        for (const auto& file : files) {
            if (current_size <= target_size) break;
            
            if (std::filesystem::remove(file.path)) {
                current_size -= file.size;
                
                // Try to remove corresponding log, obj or meta sibling file if orphaned
                std::string sibling1, sibling2;
                if (file.path.rfind(".o") != std::string::npos) {
                    sibling1 = file.path.substr(0, file.path.size() - 2) + ".log";
                    sibling2 = file.path.substr(0, file.path.size() - 2) + ".meta";
                } else if (file.path.rfind(".log") != std::string::npos) {
                    sibling1 = file.path.substr(0, file.path.size() - 4) + ".o";
                    sibling2 = file.path.substr(0, file.path.size() - 4) + ".meta";
                } else if (file.path.rfind(".meta") != std::string::npos) {
                    sibling1 = file.path.substr(0, file.path.size() - 5) + ".o";
                    sibling2 = file.path.substr(0, file.path.size() - 5) + ".log";
                }
                
                if (!sibling1.empty() && std::filesystem::exists(sibling1)) {
                    uint64_t sib_size = std::filesystem::file_size(sibling1);
                    if (std::filesystem::remove(sibling1)) {
                        current_size -= sib_size;
                    }
                }
                if (!sibling2.empty() && std::filesystem::exists(sibling2)) {
                    uint64_t sib_size = std::filesystem::file_size(sibling2);
                    if (std::filesystem::remove(sibling2)) {
                        current_size -= sib_size;
                    }
                }
            }
        }
        std::cout << "suco-coordinator: Cache eviction complete. New size: " 
                  << (current_size / (1024 * 1024)) << " MB" << std::endl;
    }
};

} // namespace suco
