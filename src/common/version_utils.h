#pragma once

#include <string>
#include <string_view>
#include <cstdlib>

namespace suco {

/**
 * @brief Extracts the major version number from a version string.
 *
 * Parses the leading digits before the first '.' character.
 * Examples: "16.1.1" → 16, "14.2" → 14, "14" → 14, "" → -1, "Unknown" → -1
 *
 * @param version The version string to parse.
 * @return The major version number, or -1 if the string cannot be parsed.
 */
inline int extract_major_version(const std::string& version) {
    if (version.empty()) return -1;

    // Find the first digit
    size_t start = std::string::npos;
    for (size_t i = 0; i < version.size(); ++i) {
        if (version[i] >= '0' && version[i] <= '9') {
            start = i;
            break;
        }
    }
    if (start == std::string::npos) return -1;

    // Extract digits until non-digit or '.'
    size_t len = 0;
    while (start + len < version.size() && version[start + len] >= '0' && version[start + len] <= '9') {
        len++;
    }
    if (len == 0) return -1;

    try {
        return std::stoi(version.substr(start, len));
    } catch (...) {
        return -1;
    }
}

/**
 * @brief Checks if the worker's compiler version is compatible with the required compiler version.
 *
 * Currently, we perform a simple major version comparison. If the major versions match,
 * the versions are considered compatible.
 *
 * @param worker_version The version string reported by the worker.
 * @param required_version The version string required by the job.
 * @return true if the versions are compatible (or if version check should be skipped).
 */
inline bool is_compiler_version_compatible(const std::string& worker_version,
                                           const std::string& required_version) {
    // Check if environment variable is set to bypass version check
    const char* ignore_env = std::getenv("SUCO_IGNORE_VERSION");
    if (ignore_env && (std::string(ignore_env) == "1" || std::string(ignore_env) == "true")) {
        return true;
    }

    // No version requirement → always compatible (backward compatibility)
    if (required_version.empty()) return true;

    // Worker didn't report a version → skip check (backward compatibility)
    if (worker_version.empty()) return true;

    int worker_major = extract_major_version(worker_version);
    int required_major = extract_major_version(required_version);

    // If either version is unparseable, skip check
    if (worker_major < 0 || required_major < 0) return true;

    return worker_major == required_major;
}

} // namespace suco
