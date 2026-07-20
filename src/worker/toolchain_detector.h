#pragma once

#include <string>
#include <map>

namespace suco::worker {

/**
 * @brief Struct holding details about compilers, build tools, and frameworks on the worker.
 */
struct ToolchainInfo {
    std::map<std::string, std::string> compilers;   // e.g., "g++" -> "14.1.1", "clang++" -> "18.1.3"
    std::map<std::string, std::string> tools;       // e.g., "cmake" -> "3.30.0", "ninja" -> "1.12.1"
    std::map<std::string, std::string> qt_versions; // e.g., "qmake" -> "6.8.0"
};

class ToolchainDetector {
public:
    /**
     * @brief Automatically detects available compilers and tools on the system.
     */
    static ToolchainInfo detect();

    /**
     * @brief Serializes ToolchainInfo into a versioned JSON string.
     */
    static std::string to_json(const ToolchainInfo& info);
};

} // namespace suco::worker
