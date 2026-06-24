#ifndef SUCO_HASH_UTIL_H
#define SUCO_HASH_UTIL_H

#include <string>

namespace suco {

// Normalizes the preprocessed source by stripping path-dependent #line directives (linear O(N) memchr implementation)
std::string normalize_preprocessed_source(const std::string& input);

// Calculates SHA-256 using the modern EVP OpenSSL API, combining the normalized source with versioning and target metadata
std::string calculate_sha256(
    const std::string& normalized_source,
    const std::string& compiler_version,
    const std::string& target_architecture,
    const std::string& language_standard,
    const std::string& sorted_defines,
    const std::string& sorted_include_paths,
    const std::string& flags_normalized
);

} // namespace suco

#endif // SUCO_HASH_UTIL_H
