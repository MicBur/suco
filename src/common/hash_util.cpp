#include "hash_util.h"
#include <openssl/evp.h>
#include <iomanip>
#include <sstream>

namespace suco {

std::string calculate_sha256(const std::string& content, const std::string& flags) {
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (mdctx == nullptr) {
        return "";
    }

    if (EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    // Hash the flags first, then a separator, then the content
    if (EVP_DigestUpdate(mdctx, flags.c_str(), flags.size()) != 1) {
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    const char sep = '|';
    if (EVP_DigestUpdate(mdctx, &sep, 1) != 1) {
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    if (EVP_DigestUpdate(mdctx, content.c_str(), content.size()) != 1) {
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    EVP_MD_CTX_free(mdctx);

    std::stringstream ss;
    for (unsigned int i = 0; i < hash_len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

} // namespace suco
