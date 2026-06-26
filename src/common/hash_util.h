#pragma once

#include <string>
#include <vector>

namespace suco {

struct CacheKeyInput {
    std::string target_arch;
    std::string compiler_version;
    std::string language_standard;
    std::string sorted_defines;
    std::string sorted_includes;
    std::string normalized_flags;
};

// Strips #line directives, blank lines, and CRLF artifacts from preprocessed source.
std::string normalize_preprocessed_source(const std::string& source);

// Computes a versioned SHA-256 over the normalized source and all build metadata.
std::string compute_cache_hash(const std::string& normalized_source,
                               const CacheKeyInput& key);

} // namespace suco
