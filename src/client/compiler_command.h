#pragma once

#include <string>
#include <vector>

/**
 * @brief Domain model representing a compiler invocation.
 * 
 * Holds all parsed command line arguments, compiler flags, and the preprocessed
 * source code. Also provides helper functions for querying compiler properties
 * and preparing the input payload for cache key hashing.
 */
struct CompilerCommand {
    std::string compiler_path;           // e.g. "cl.exe" or "/usr/bin/g++"
    std::string source_file;
    std::string output_file;

    std::string language_standard;       // e.g. "-std=c++17" or "/std:c++20"

    std::vector<std::string> defines;       // Sorted macros, e.g. ["-DNDEBUG"]
    std::vector<std::string> include_paths;  // Sorted include directories
    std::vector<std::string> other_flags;   // Optimizations, warnings, and other flags

    bool is_msvc = false;
    bool is_precompiled_header = false;
    bool is_pch_creation = false;
    bool is_pch_usage = false;
    std::string pch_file;
    bool is_monitor_request = false;

    std::vector<std::string> raw_args; // Holds the arguments for fallback execution

    // Populated dynamically during the pre-processing stage
    std::string preprocessed_source;
    std::string content_hash;            // The final calculated SHA-256 cache hash

    /**
     * @brief Parses command line arguments to build a CompilerCommand.
     * @param argc Argument count.
     * @param argv Argument values.
     * @return The parsed CompilerCommand structure.
     */
    static CompilerCommand parse(int argc, char** argv);

    /**
     * @brief Checks if this invocation is a standard compile-only (-c) command.
     */
    bool is_compilation_step() const;

    /**
     * @brief Extracts the file name of the compiler (e.g. "g++" from "/usr/bin/g++").
     */
    std::string get_compiler_name() const;

    /**
     * @brief Queries the target architecture (cached for performance).
     */
    std::string get_target_architecture() const;

    /**
     * @brief Queries the compiler version string (cached for performance).
     */
    std::string get_compiler_version() const;

    /**
     * @brief Constructs the normalized string payload used for generating the cache key hash.
     */
    std::string get_hash_input() const;

private:
    /**
     * @brief Normalizes preprocessed source output by stripping CRLF, #pragma once, and empty lines.
     */
    static std::string normalize_preprocessed_source(const std::string& input);
};
