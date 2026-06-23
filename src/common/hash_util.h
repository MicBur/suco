#ifndef SUCO_HASH_UTIL_H
#define SUCO_HASH_UTIL_H

#include <string>

namespace suco {

// Calculates SHA-256 of the preprocessed source code content combined with compiler flags
std::string calculate_sha256(const std::string& content, const std::string& flags);

} // namespace suco

#endif // SUCO_HASH_UTIL_H
