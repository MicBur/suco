#include "hash_util.h"
#include <openssl/evp.h>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <cctype>

namespace suco {

std::string normalize_preprocessed_source(const std::string& input) {
    std::string output;
    output.reserve(input.size()); // Pre-allocate buffer

    const char* data = input.data();
    size_t size = input.size();
    size_t start = 0;

    while (start < size) {
        // Fast hardware-backed scanning for the next newline character using memchr
        const char* next_newline = static_cast<const char*>(std::memchr(data + start, '\n', size - start));
        size_t end = next_newline ? (next_newline - data) : size;
        size_t len = end - start;
        const char* line_ptr = data + start;

        // Skip leading whitespace / carriage returns
        size_t first_non_ws = 0;
        while (first_non_ws < len && (line_ptr[first_non_ws] == ' ' || line_ptr[first_non_ws] == '\t' || line_ptr[first_non_ws] == '\r')) {
            first_non_ws++;
        }

        // Entirely skip empty or whitespace-only lines
        if (first_non_ws >= len) {
            start = end + 1;
            continue;
        }

        // Detect lines starting with '#'
        if (line_ptr[first_non_ws] == '#') {
            size_t idx = first_non_ws + 1;
            while (idx < len && (line_ptr[idx] == ' ' || line_ptr[idx] == '\t')) {
                idx++;
            }

            if (idx < len) {
                // MSVC path markers: #line ...
                if (len - idx >= 4 && std::strncmp(line_ptr + idx, "line", 4) == 0) {
                    start = end + 1;
                    continue; // Skip line
                }

                // GCC & MSVC Fallback markers: # <line_number> ...
                if (std::isdigit(static_cast<unsigned char>(line_ptr[idx]))) {
                    start = end + 1;
                    continue; // Skip line
                }
            }
        }

        // Write non-path, non-empty code lines
        output.append(line_ptr, len);
        output += '\n';

        start = end + 1;
    }
    return output;
}

std::string calculate_sha256(
    const std::string& normalized_source,
    const std::string& compiler_version,
    const std::string& target_architecture,
    const std::string& language_standard,
    const std::string& sorted_defines,
    const std::string& sorted_include_paths,
    const std::string& flags_normalized
) {
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (mdctx == nullptr) {
        return "";
    }

    if (EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(mdctx);
        return "";
    }

    // Hash structure: v1:\x1F<Target>\x1F<Compiler>\x1F<Std>\x1F<Defines>\x1F<Includes>\x1F<Flags>\x1F<Source>
    // Delimiter is 0x1F (ASCII Unit Separator)
    const char delimiter = '\x1F';
    const std::string prefix = "v1:";

    auto update_hash = [&](const std::string& str) -> bool {
        return EVP_DigestUpdate(mdctx, str.c_str(), str.size()) == 1;
    };

    auto update_delimiter = [&]() -> bool {
        return EVP_DigestUpdate(mdctx, &delimiter, 1) == 1;
    };

    if (!update_hash(prefix) || !update_delimiter() ||
        !update_hash(target_architecture) || !update_delimiter() ||
        !update_hash(compiler_version) || !update_delimiter() ||
        !update_hash(language_standard) || !update_delimiter() ||
        !update_hash(sorted_defines) || !update_delimiter() ||
        !update_hash(sorted_include_paths) || !update_delimiter() ||
        !update_hash(flags_normalized) || !update_delimiter() ||
        !update_hash(normalized_source)) {
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
