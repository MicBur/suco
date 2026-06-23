#ifndef AG_HASH_UTIL_H
#define AG_HASH_UTIL_H

#include <string>

namespace ag {

// Calculates SHA-256 of the preprocessed source code content combined with compiler flags
std::string calculate_sha256(const std::string& content, const std::string& flags);

} // namespace ag

#endif // AG_HASH_UTIL_H
