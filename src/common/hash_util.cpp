#include "hash_util.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <cctype>
#include <fstream>
#include <cstring>
#include <memory>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <mutex>
#include <unordered_map>

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

std::string strip_predefined_macros(const std::string& source) {
    if (source.empty())
        return {};

    std::string out;
    out.reserve(source.size());

    const char* data = source.data();
    const size_t total = source.size();
    size_t pos = 0;

    while (pos < total) {
        auto* nl = static_cast<const char*>(std::memchr(data + pos, '\n', total - pos));
        size_t eol = nl ? static_cast<size_t>(nl - data) : total;
        size_t len = eol - pos;
        const char* line = data + pos;

        // Trim trailing carriage returns.
        while (len > 0 && line[len - 1] == '\r')
            --len;

        // Find first non-whitespace character.
        size_t first = 0;
        while (first < len && (line[first] == ' ' || line[first] == '\t'))
            ++first;

        // Check if the line is a predefined macro definition/undefinition
        bool should_skip = false;
        if (first < len && line[first] == '#') {
            size_t i = first + 1;
            while (i < len && (line[i] == ' ' || line[i] == '\t'))
                ++i;

            if (i < len) {
                // check if it starts with define or undef
                bool is_define = (len - i >= 6 && std::strncmp(line + i, "define", 6) == 0);
                bool is_undef = (len - i >= 5 && std::strncmp(line + i, "undef", 5) == 0);
                if (is_define || is_undef) {
                    size_t word_len = is_define ? 6 : 5;
                    size_t j = i + word_len;
                    while (j < len && (line[j] == ' ' || line[j] == '\t'))
                        ++j;

                    if (j < len) {
                        // check if macro name starts with __STDC__ or __STDC_
                        if (len - j >= 8 && std::strncmp(line + j, "__STDC__", 8) == 0) {
                            should_skip = true;
                        } else if (len - j >= 7 && std::strncmp(line + j, "__STDC_", 7) == 0) {
                            should_skip = true;
                        }
                    }
                }
            }
        }

        if (should_skip) {
            pos = eol + 1;
            continue;
        }

        out.append(line, len);
        out.push_back('\n');
        pos = eol + 1;
    }

    return out;
}

bool contains_time_macros(const std::string& source) {
    return source.find("__DATE__") != std::string::npos ||
           source.find("__TIME__") != std::string::npos ||
           source.find("__TIMESTAMP__") != std::string::npos;
}

bool uses_cxx_modules(const std::string& source) {
    // C++20 modules break the preprocess-then-ship model: `import foo;` is NOT a
    // preprocessor directive, so it survives preprocessing untouched and the compiler
    // resolves it later against a Compiled Module Interface (gcm.cache/foo.gcm). A
    // remote worker has no CMI, so the job dies with
    //   "error: failed to read compiled module: No such file or directory".
    // Until CMIs are shipped with the job (see ROADMAP-IB.md E3), detect module usage
    // and let the caller compile locally — correct beats fast.
    //
    // Scans line starts only: `import`/`export` must open a line (leading whitespace
    // aside), which keeps `#include <x>` mentioning the words, or an `import` inside a
    // string/identifier (e.g. `reimport`, "import"), from tripping the check.
    size_t pos = 0;
    while (pos < source.size()) {
        size_t eol = source.find('\n', pos);
        if (eol == std::string::npos) eol = source.size();
        size_t b = pos;
        while (b < eol && (source[b] == ' ' || source[b] == '\t')) ++b;

        auto word_at = [&](const char* w, size_t at) {
            size_t n = std::strlen(w);
            if (at + n > eol || std::strncmp(source.data() + at, w, n) != 0) return false;
            // must be followed by a separator, so `importer` doesn't match `import`
            return at + n == eol || source[at + n] == ' ' || source[at + n] == '\t' ||
                   source[at + n] == ';' || source[at + n] == ':' || source[at + n] == '"' ||
                   source[at + n] == '<';
        };

        size_t w = b;
        if (word_at("export", w)) {                 // `export module x;` / `export import x;`
            w += 6;
            while (w < eol && (source[w] == ' ' || source[w] == '\t')) ++w;
        }
        if (word_at("import", w) || word_at("module", w)) return true;

        pos = eol + 1;
    }
    return false;
}

bool find_and_read_cmi(const std::string& module_name, const std::string& cwd, std::string& out) {
    // GCC stores CMIs as gcm.cache/<module-name>.gcm relative to the compiler's working
    // directory (dots in the name are kept verbatim: net.http -> gcm.cache/net.http.gcm).
    // Check the build cwd first, then this process's cwd as a fallback.
    std::vector<std::filesystem::path> roots;
    if (!cwd.empty()) roots.emplace_back(cwd);
    std::error_code ec;
    auto here = std::filesystem::current_path(ec);
    if (!ec) roots.push_back(here);

    for (const auto& r : roots) {
        std::filesystem::path p = r / "gcm.cache" / (module_name + ".gcm");
        std::ifstream f(p, std::ios::binary);
        if (!f) continue;
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!out.empty()) return true;
    }
    return false;
}

std::vector<std::string> scan_module_imports(const std::string& source) {
    // Collect the module names a TU imports: `import foo;` / `export import foo;`
    // (dotted names like `net.http` included). Header units (`import <vector>;` /
    // `import "x.h";`) are skipped — they have no gcm.cache/<name>.gcm we can ship.
    // Declaring lines (`export module foo;`) are NOT imports and are skipped too.
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < source.size()) {
        size_t eol = source.find('\n', pos);
        if (eol == std::string::npos) eol = source.size();
        size_t b = pos;
        while (b < eol && (source[b] == ' ' || source[b] == '\t')) ++b;

        size_t w = b;
        auto starts = [&](const char* kw, size_t at) {
            size_t n = std::strlen(kw);
            return at + n <= eol && std::strncmp(source.data() + at, kw, n) == 0 &&
                   at + n < eol && (source[at + n] == ' ' || source[at + n] == '\t');
        };
        if (starts("export", w)) {
            w += 6;
            while (w < eol && (source[w] == ' ' || source[w] == '\t')) ++w;
        }
        if (starts("import", w)) {
            w += 6;
            while (w < eol && (source[w] == ' ' || source[w] == '\t')) ++w;
            // skip header units — nothing to ship for those
            if (w < eol && source[w] != '<' && source[w] != '"') {
                size_t e = w;
                while (e < eol && (std::isalnum(static_cast<unsigned char>(source[e])) ||
                                   source[e] == '_' || source[e] == '.' || source[e] == ':')) ++e;
                if (e > w) {
                    std::string name = source.substr(w, e - w);
                    if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
                }
            }
        }
        pos = eol + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Path normalization and detection helpers
// ---------------------------------------------------------------------------

namespace {
    std::mutex g_checkout_mutex;
    std::unordered_map<std::string, std::string> g_checkout_cache;
}

std::string detect_checkout_root(const std::string& start_dir, const RequestContext& context) {
    std::string base_cwd = context.cwd;
    if (base_cwd.empty()) {
        try {
            base_cwd = std::filesystem::current_path().string();
        } catch (...) {
            base_cwd = ".";
        }
    }

    std::filesystem::path target_path = start_dir;
    if (target_path.is_relative()) {
        target_path = std::filesystem::path(base_cwd) / target_path;
    }
    
    std::string abs_start_dir;
    try {
        abs_start_dir = std::filesystem::absolute(target_path).lexically_normal().string();
    } catch (...) {
        abs_start_dir = base_cwd;
    }

    if (start_dir == ".") {
        std::lock_guard<std::mutex> lock(g_checkout_mutex);
        auto it = g_checkout_cache.find(abs_start_dir);
        if (it != g_checkout_cache.end()) {
            return it->second;
        }
    }

    std::string root;
    try {
        std::filesystem::path p = abs_start_dir;
        while (p.has_relative_path() && p != p.root_path()) {
            if (std::filesystem::exists(p / ".git") || std::filesystem::exists(p / ".hg")) {
                root = p.lexically_normal().string();
                break;
            }
            p = p.parent_path();
        }
    } catch (...) {
        // Fallback
    }

    if (root.empty()) {
        root = abs_start_dir;
    }

    if (start_dir == ".") {
        std::lock_guard<std::mutex> lock(g_checkout_mutex);
        g_checkout_cache[abs_start_dir] = root;
    }

    return root;
}

std::string normalize_paths(std::string text, const std::string& checkout_root) {
    if (checkout_root.empty() || text.empty()) return text;
    
    std::vector<std::string> search_patterns;
    search_patterns.push_back(checkout_root);
    
    std::string with_slashes = checkout_root;
    std::replace(with_slashes.begin(), with_slashes.end(), '\\', '/');
    search_patterns.push_back(with_slashes);
    
    std::string with_backslashes = checkout_root;
    std::replace(with_backslashes.begin(), with_backslashes.end(), '/', '\\');
    search_patterns.push_back(with_backslashes);
    
    // Sort longer first to avoid partial matching issues
    std::sort(search_patterns.begin(), search_patterns.end(), [](const std::string& a, const std::string& b) {
        return a.size() > b.size();
    });
    
    search_patterns.erase(std::unique(search_patterns.begin(), search_patterns.end()), search_patterns.end());
    
    const std::string placeholder = "{SUCO_ROOT}";
    for (const auto& pattern : search_patterns) {
        if (pattern.empty()) continue;
        size_t pos = 0;
        while ((pos = text.find(pattern, pos)) != std::string::npos) {
            text.replace(pos, pattern.size(), placeholder);
            pos += placeholder.size();
        }
    }
    return text;
}

// ---------------------------------------------------------------------------
// Cache hash computation
// ---------------------------------------------------------------------------

std::string compute_cache_hash(const std::string& normalized_source,
                               const CacheKeyInput& key,
                               const RequestContext& context) {
    EvpCtx ctx(EVP_MD_CTX_new());
    if (!ctx)
        return {};

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        return {};

    bool path_norm = context.path_normalization;

    std::string root = path_norm ? detect_checkout_root(".", context) : "";
    std::string norm_arch = path_norm ? normalize_paths(key.target_arch, root) : key.target_arch;
    std::string norm_compiler = path_norm ? normalize_paths(key.compiler_version, root) : key.compiler_version;
    std::string norm_std = path_norm ? normalize_paths(key.language_standard, root) : key.language_standard;
    std::string norm_defines = path_norm ? normalize_paths(key.sorted_defines, root) : key.sorted_defines;
    std::string norm_includes = path_norm ? normalize_paths(key.sorted_includes, root) : key.sorted_includes;
    std::string norm_flags = path_norm ? normalize_paths(key.normalized_flags, root) : key.normalized_flags;
    std::string norm_source = path_norm ? normalize_paths(normalized_source, root) : normalized_source;

    // Feed all components into the digest, separated by 0x1F (Unit Separator).
    // Layout: v4:<0x1F>arch<0x1F>compiler<0x1F>std<0x1F>defines<0x1F>includes<0x1F>flags<0x1F>source
    // v4: invalidates entries created before the __DATE__/__TIME__ cache guard —
    // objects with baked-in stale timestamps must never be served again.
    constexpr char sep = '\x1F';
    const std::string_view parts[] = {
        "v4:",
        norm_arch,
        norm_compiler,
        norm_std,
        norm_defines,
        norm_includes,
        norm_flags,
        norm_source
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

std::string compute_sha256(const std::string& input) {
    EvpCtx ctx(EVP_MD_CTX_new());
    if (!ctx)
        return {};

    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        return {};

    if (EVP_DigestUpdate(ctx.get(), input.data(), input.size()) != 1)
        return {};

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &digest_len) != 1)
        return {};

    return to_hex(digest, digest_len);
}

std::string get_shared_secret() {
    const char* s = std::getenv("SUCO_SECRET");
    return s ? std::string(s) : std::string();
}

std::string generate_nonce(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), static_cast<int>(bytes)) != 1) return {};
    return to_hex(buf.data(), static_cast<unsigned int>(bytes));
}

std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!HMAC(EVP_sha256(),
              key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(data.data()), data.size(),
              mac, &len)) {
        return {};
    }
    return to_hex(mac, len);
}

bool constant_time_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace suco
