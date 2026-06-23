#include "hash_util.h"
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

namespace ag {

std::string calculate_sha256(const std::string& content, const std::string& flags) {
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    
    // Hash the flags first, then a separator, then the content
    SHA256_Update(&sha256, flags.c_str(), flags.size());
    const char sep = '|';
    SHA256_Update(&sha256, &sep, 1);
    SHA256_Update(&sha256, content.c_str(), content.size());
    
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256);
    
    std::stringstream ss;
    for(int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

} // namespace ag
