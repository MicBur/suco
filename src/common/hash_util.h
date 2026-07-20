#pragma once

#include <string>
#include <vector>
#include <cstdlib>
#include <map>

namespace suco {

struct RequestContext {
    std::string cwd;
    bool path_normalization = true;
    std::map<std::string, std::string> env_overrides;
};

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

// Filters out predefined macros from preprocessed source to avoid warnings on workers
std::string strip_predefined_macros(const std::string& source);

// True if the (preprocessed) source still references __DATE__/__TIME__/__TIMESTAMP__.
// Since fast preprocessing (-fdirectives-only / -frewrite-includes) leaves macros
// unexpanded, caching such translation units would serve stale timestamps —
// callers must compile them locally and bypass all caches (ccache does the same).
bool contains_time_macros(const std::string& source);

/**
 * @brief True if the translation unit uses C++20 modules (import / module / export
 * module). Such TUs cannot be dispatched: `import` survives preprocessing and the
 * worker has no Compiled Module Interface, so the remote compile fails outright.
 * Callers compile these locally until CMIs are shipped with the job (ROADMAP-IB.md E3).
 */
bool uses_cxx_modules(const std::string& source);

/**
 * @brief Module names this TU imports (`import foo;`, `export import net.http;`).
 * Header units (`import <vector>;`) are excluded — they have no shippable CMI.
 * Used to collect gcm.cache/<name>.gcm files and ship them with the job (E3).
 */
std::vector<std::string> scan_module_imports(const std::string& source);

/**
 * @brief Read the CMI (gcm.cache/<module>.gcm) for a module, relative to the build
 * cwd. False if it is not there (caller must then compile locally — a missing CMI
 * cannot be reconstructed remotely).
 */
bool find_and_read_cmi(const std::string& module_name, const std::string& cwd, std::string& out);

// Detects the checkout root directory (walking up looking for .git/.hg or falling back to CWD)
std::string detect_checkout_root(const std::string& start_dir, const RequestContext& context = RequestContext{});

// Replaces occurrences of checkout root (slash/backslash variants) with {SUCO_ROOT}
std::string normalize_paths(std::string text, const std::string& checkout_root);

// Computes a versioned SHA-256 over the normalized source and all build metadata.
std::string compute_cache_hash(const std::string& normalized_source,
                               const CacheKeyInput& key,
                               const RequestContext& context);

// Helper to compute a standard SHA-256 hash of a string.
std::string compute_sha256(const std::string& input);

// --- Authentication helpers (shared-secret HMAC challenge-response) ---
// Shared secret from SUCO_SECRET (empty string => authentication disabled).
std::string get_shared_secret();
// Cryptographically random hex nonce (`bytes` random bytes -> 2*bytes hex chars).
// Empty on failure.
std::string generate_nonce(size_t bytes = 32);
// HMAC-SHA256(key, data) as lowercase hex. Empty on failure.
std::string hmac_sha256_hex(const std::string& key, const std::string& data);
// Timing-safe comparison (avoids leaking MAC bytes via compare duration).
bool constant_time_equals(const std::string& a, const std::string& b);

inline std::string get_toolchain_cache_dir() {
    std::string home;
    const char* home_env = std::getenv("HOME");
    if (home_env && *home_env) {
        home = home_env;
    } else {
        const char* user_profile = std::getenv("USERPROFILE");
        if (user_profile && *user_profile) {
            home = user_profile;
        }
    }
    if (home.empty()) {
#ifdef _WIN32
        const char* temp_env = std::getenv("TEMP");
        if (temp_env && *temp_env) {
            home = temp_env;
        } else {
            home = "C:\\Temp";
        }
#else
        home = "/tmp";
#endif
    }
    return home + "/.cache/suco/toolchains";
}

} // namespace suco
