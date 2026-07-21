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

    std::string required_compiler;          // e.g. "g++", "clang++", "cl"
    std::string required_compiler_version;  // optional

    std::vector<std::string> raw_args; // Holds the arguments for fallback execution

    // Populated dynamically during the pre-processing stage
    std::string preprocessed_source;
    std::string content_hash;            // The final calculated SHA-256 cache hash
    std::string toolchain_hash;          // Hash of the required compiler toolchain
    std::string header_set_hash;
    std::string stripped_source;
    std::string header_set_source;

    // E3: C++20 module CMIs this TU imports, shipped with the job so a remote worker
    // can resolve `import x;` (which survives preprocessing — the compiler only
    // resolves it later against gcm.cache/x.gcm, which the worker does not have).
    // Pairs of (module name, raw .gcm bytes). Their contents feed the cache key.
    std::vector<std::pair<std::string, std::string>> module_cmis;

    /**
     * @brief Parses command line arguments to build a CompilerCommand.
     * @param argc Argument count.
     * @param argv Argument values.
     * @return The parsed CompilerCommand structure.
     */
    static CompilerCommand parse(int argc, char** argv);

    /**
     * @brief Parses command line arguments to build multiple CompilerCommands if multiple files are present.
     */
    static std::vector<CompilerCommand> parse_all(int argc, char** argv);

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
     * @brief The compiler name to put in a job dispatched to a worker.
     *
     * "g++" means "produces objects for THIS machine", which is exactly wrong once the
     * job may land on a machine with a different native target: a MinGW client's job
     * assigned to a Linux worker would be compiled by that worker's own g++ and come
     * back as an ELF object, failing at link time with nothing pointing at the cause.
     * For MinGW targets the toolchain ships a triple-qualified alias
     * (x86_64-w64-mingw32-g++), which names the target instead of the host, so a worker
     * either has it and produces the right object or lacks it and exits 127 — already
     * handled as an infrastructure signal (invariant #3) that recompiles locally.
     *
     * Deliberately limited to MinGW targets: qualifying Linux targets too would make
     * every job depend on an x86_64-linux-gnu-g++ alias existing on every node, which
     * is not guaranteed and would take a working grid down to local-only compiles.
     */
    std::string get_remote_compiler_name() const;

    /**
     * @brief The compiler NAME to advertise in cache queries and job requests.
     *
     * This is what the coordinator's scheduler matches against a worker's advertised
     * toolchain map, so for MinGW targets it must be the target-qualified name a
     * suitable worker actually advertises (x86_64-w64-mingw32-g++) — a worker without
     * that driver is then skipped up front instead of being assigned a job it can only
     * answer with exit 127. Everything else keeps the plain name.
     *
     * Wire-compatible with 0.9.2: the field is a free-form string; old workers simply
     * never advertise the qualified name, so the scheduler finds no worker and the
     * client compiles locally — same net behavior as today, minus the wasted dispatch.
     * The in-process required_compiler member intentionally stays the LOCAL name
     * ("g++"): it doubles as the feature-flag selector for -fdirectives-only and
     * -ffile-prefix-map, which must not change with the dispatch target.
     */
    std::string get_dispatch_compiler_id() const;

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
