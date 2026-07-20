#pragma once

#include <string>
#include <vector>
#include <utility>

namespace suco {

/**
 * @brief Checks if a string starts with a specific prefix.
 */
inline bool starts_with(const std::string& str, const std::string& prefix) noexcept {
    return str.starts_with(prefix);
}

/**
 * @brief Checks if a string ends with a specific suffix.
 */
inline bool ends_with(const std::string& str, const std::string& suffix) noexcept {
    return str.ends_with(suffix);
}

/**
 * @brief Converts a copy of the string to lower case.
 */
inline std::string to_lower(std::string str) {
    for (char& c : str) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return str;
}

/**
 * @brief Trims leading and trailing whitespace from a string.
 */
inline std::string trim(const std::string& str) {
    const size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

/**
 * @brief Joins elements using a delimiter.
 */
inline std::string join(const std::vector<std::string>& vec, const std::string& delimiter) {
    std::string result;
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i > 0) result += delimiter;
        result += vec[i];
    }
    return result;
}

/**
 * @brief Splits a string by a character delimiter.
 */
inline std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> result;
    size_t start = 0;
    size_t end = str.find(delimiter);
    while (end != std::string::npos) {
        result.push_back(str.substr(start, end - start));
        start = end + 1;
        end = str.find(delimiter, start);
    }
    result.push_back(str.substr(start));
    return result;
}

/**
 * @brief Normalizes multi-line toolchain outputs (e.g. MSVC /Bv) into a single line.
 */
inline std::string normalize_version_output(const std::string& output) {
    std::string result;
    result.reserve(output.size());
    bool last_was_space = false;
    for (char c : output) {
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!last_was_space && !result.empty()) {
                result += ' ';
                last_was_space = true;
            }
        } else {
            result += c;
            last_was_space = (c == ' ');
        }
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

/**
 * @brief Normalizes a compiler path/name to a canonical compiler family name.
 *
 * Handles cross-compiler prefixes (e.g. x86_64-linux-gnu-g++), version suffixes
 * (e.g. g++-14, clang++-18), and platform-specific extensions (.exe).
 *
 * Canonical names: "g++", "clang++", "cl"
 *
 * @param compiler_path Full path or basename of the compiler executable.
 * @return The normalized canonical compiler name, or the lowercased basename if unrecognized.
 */
inline std::string normalize_compiler_name(const std::string& compiler_path) {
    // Extract basename from path
    std::string name = compiler_path;
    size_t last_sep = name.find_last_of("\\/");
    if (last_sep != std::string::npos) {
        name = name.substr(last_sep + 1);
    }

    // Lowercase for case-insensitive matching
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Strip .exe suffix (Windows)
    if (name.size() > 4 && name.substr(name.size() - 4) == ".exe") {
        name = name.substr(0, name.size() - 4);
    }

    // Order matters: check clang++ before clang, g++ before gcc,
    // because "clang++" contains "clang" and "g++" is a substring pattern.

    // Clang family: clang++, clang++-18, x86_64-linux-gnu-clang++, etc.
    if (name.find("clang++") != std::string::npos) {
        return "clang++";
    }
    // clang (C compiler, but in SUCO context we normalize to clang++)
    if (name.find("clang") != std::string::npos) {
        return "clang++";
    }

    // GCC family: g++, g++-12, g++-14, x86_64-linux-gnu-g++, etc.
    if (name.find("g++") != std::string::npos || name.find("c++") != std::string::npos) {
        return "g++";
    }
    // gcc, gcc-12, x86_64-linux-gnu-gcc, etc.
    if (name.find("gcc") != std::string::npos || name == "cc") {
        return "g++";
    }

    // MSVC: cl, cl.exe (already stripped above)
    if (name == "cl") {
        return "cl";
    }

    // Unrecognized compiler — return the lowercased basename as-is
    return name;
}

/**
 * @brief Runs a command locally and captures its exit code and stdout/stderr output.
 * @param args Vector of command arguments (first argument is the executable).
 * @return A pair containing the exit code and the captured output string.
 */
// Splits a flag unit back into argv tokens. Parsing stores options that take a
// separate value as ONE element ("-include foo.h") so sorting cannot separate them;
// argv builders must split them again. Values containing spaces are not supported
// here — they never reach this path (such flags stay in raw_args for local runs).
void append_flag_tokens(std::vector<std::string>& out, const std::string& unit);

std::pair<int, std::string> run_local_capture(const std::vector<std::string>& args, const std::string& cwd = "");

} // namespace suco
