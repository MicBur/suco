#include "toolchain_manager.h"
#include "platform_compat.h"
#include "logging.h"
#include "hash_util.h"

#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <vector>

#ifndef _WIN32
    #include <sys/types.h>
    #include <pwd.h>
    #include <unistd.h>
#endif

namespace suco {

namespace {

std::string get_toolchain_dir() {
    std::string dir = get_toolchain_cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

bool run_cmd_simple(const std::string& cmd, std::string& out_log) {
    char buffer[4096];
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return false;

    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        out_log += buffer;
    }

#ifdef _WIN32
    int status = _pclose(pipe);
    return status == 0;
#else
    int status = pclose(pipe);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

} // namespace

bool ToolchainManager::has_toolchain(const std::string& hash) {
    if (hash.empty()) return false;
    std::string dir = get_toolchain_path(hash);
    std::error_code ec;
    return std::filesystem::exists(dir, ec) && std::filesystem::is_directory(dir, ec);
}

std::string ToolchainManager::get_toolchain_path(const std::string& hash) {
    return get_toolchain_dir() + "/" + hash;
}

bool ToolchainManager::extract_toolchain(const std::string& hash, const std::string& archive_path) {
    if (hash.empty() || archive_path.empty()) return false;
    
    std::string target_dir = get_toolchain_path(hash);
    std::error_code ec;
    if (std::filesystem::exists(target_dir, ec)) return true;   // finished by someone else

    // Extract into a temp dir, then RENAME into place. has_toolchain() only checks
    // "directory exists", so extracting straight into the final path published a
    // half-written toolchain: a concurrent job exec'd a compiler whose binary was
    // not unpacked yet -> "sh: c++: not found" -> exit 127 -> the BUILD failed on a
    // race that a rerun never reproduces. rename(2) on one filesystem is atomic, so
    // the final path either does not exist or is complete.
    std::string tmp_dir = target_dir + ".part." + std::to_string(::getpid());
    std::filesystem::create_directories(tmp_dir, ec);
    if (ec) {
        SUCO_LOG_ERROR("Failed to create staging directory for toolchain: {}. Error: {}", tmp_dir, ec.message());
        return false;
    }

    SUCO_LOG_INFO("Extracting toolchain {} from {}...", hash, archive_path);

    std::string cmd = "tar -I zstd -xf \"" + archive_path + "\" -C \"" + tmp_dir + "\"";
    std::string out_log;
    if (!run_cmd_simple(cmd, out_log)) {
        SUCO_LOG_ERROR("Failed to extract toolchain archive {}. Output: {}", archive_path, out_log);
        std::filesystem::remove_all(tmp_dir, ec);
        return false;
    }

    std::filesystem::rename(tmp_dir, target_dir, ec);
    if (ec) {
        // Lost the publish race to another extractor — fine as long as the winner's
        // toolchain is in place.
        std::error_code ec2;
        bool ok = std::filesystem::exists(target_dir, ec2);
        std::filesystem::remove_all(tmp_dir, ec2);
        if (!ok) SUCO_LOG_ERROR("Failed to publish toolchain {}: {}", hash, ec.message());
        return ok;
    }

    SUCO_LOG_INFO("Successfully extracted toolchain {} into {}", hash, target_dir);
    return true;
}

std::vector<std::string> ToolchainManager::list_toolchains() {
    std::vector<std::string> result;
    std::string base_dir = get_toolchain_dir();
    std::error_code ec;
    if (!std::filesystem::exists(base_dir, ec)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(base_dir, ec)) {
        if (entry.is_directory(ec)) {
            std::string name = entry.path().filename().string();
            // Hex string length for SHA-256 is 64
            if (name.size() == 64) {
                bool is_hex = true;
                for (char c : name) {
                    if (!std::isxdigit(static_cast<unsigned char>(c))) {
                        is_hex = false;
                        break;
                    }
                }
                if (is_hex) {
                    result.push_back(name);
                }
            }
        }
    }
    return result;
}

} // namespace suco
