#include "hash_util.h"

#include <openssl/evp.h>
#include <cctype>
#include <cstring>
#include <memory>

namespace suco {
namespace {

// RAII wrapper for OpenSSL digest context.
struct EvpCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) const { EVP_MD_CTX_free(ctx); }
};
using EvpCtx = std::unique_ptr<EVP_MD_CTX, EvpCtxDeleter>;

// Returns true if the line (after '#' and whitespace) is a preprocessor
// location marker that should be stripped for cache normalization.
// Covers: "#line 42 ..." (MSVC), "# 42 ..." (GCC/Clang), "#pragma once".
bool is_strippable_directive(const char* line, size_t len, size_t hash_pos) {
    size_t i = hash_pos + 1;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
        ++i;

    if (i >= len)
        return false;

    // "# 123 ..." — GCC/Clang line marker
    if (std::isdigit(static_cast<unsigned char>(line[i])))
        return true;

    // "#line ..." — MSVC line marker
    if (len - i >= 4 && std::strncmp(line + i, "line", 4) == 0)
        return true;

    // "#pragma once" — irrelevant for object output
    if (len - i >= 6 && std::strncmp(line + i, "pragma", 6) == 0) {
        size_t j = i + 6;
        while (j < len && (line[j] == ' ' || line[j] == '\t'))
            ++j;
        if (len - j >= 4 && std::strncmp(line + j, "once", 4) == 0)
            return true;
    }

    return false;
}

std::string to_hex(const unsigned char* data, unsigned int len) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        result += digits[(data[i] >> 4) & 0x0F];
        result += digits[data[i] & 0x0F];
    }
    return result;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Source normalization
// ---------------------------------------------------------------------------

std::string normalize_preprocessed_source(const std::string& source) {
    if (source.empty())
        return {};

    std::string out;
    out.reserve(source.size());

    const char* data = source.data();
    const size_t total = source.size();
    size_t pos = 0;

    while (pos < total) {
        // Scan for the next newline using memchr (typically a single x86 instruction).
        auto* nl = static_cast<const char*>(std::memchr(data + pos, '\n', total - pos));
        size_t eol = nl ? static_cast<size_t>(nl - data) : total;
        size_t len = eol - pos;
        const char* line = data + pos;

        // Trim trailing carriage returns (CRLF → LF).
        while (len > 0 && line[len - 1] == '\r')
            --len;

        // Find first non-whitespace character.
        size_t first = 0;
        while (first < len && (line[first] == ' ' || line[first] == '\t'))
            ++first;

        // Skip blank lines.
        if (first >= len) {
            pos = eol + 1;
            continue;
        }

        // Skip preprocessor location markers and #pragma once.
        if (line[first] == '#' && is_strippable_directive(line, len, first)) {
            pos = eol + 1;
            continue;
        }

        out.append(line, len);
        out += '\n';
        pos = eol + 1;
    }

    return out;
}

// ---------------------------------------------------------------------------
// Cache hash computation
// ---------------------------------------------------------------------------

std::string compute_cache_hash(const std::string& normalized_source,
                               const CacheKeyInput& key) {
    EvpCtx ctx(EVP_MD_CTX_new());
    if (!ctx)
        return {};

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        return {};

    // Feed all components into the digest, separated by 0x1F (Unit Separator).
    // Layout: v1:<0x1F>arch<0x1F>compiler<0x1F>std<0x1F>defines<0x1F>includes<0x1F>flags<0x1F>source
    constexpr char sep = '\x1F';
    const std::string_view parts[] = {
        "v1:",
        key.target_arch,
        key.compiler_version,
        key.language_standard,
        key.sorted_defines,
        key.sorted_includes,
        key.normalized_flags,
        normalized_source
    };

    for (size_t i = 0; i < std::size(parts); ++i) {
        if (i > 0 && EVP_DigestUpdate(ctx.get(), &sep, 1) != 1)
            return {};
        if (EVP_DigestUpdate(ctx.get(), parts[i].data(), parts[i].size()) != 1)
            return {};
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &digest_len) != 1)
        return {};

    return to_hex(digest, digest_len);
}

} // namespace suco
