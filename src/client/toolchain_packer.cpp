#include "toolchain_packer.h"
#include "utils.h"
#include "logging.h"
#include "hash_util.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <chrono>
#include <openssl/evp.h>
#include <cctype>
#include <algorithm>

#ifndef _WIN32
    #include <sys/types.h>
    #include <pwd.h>
    #include <unistd.h>
#endif

namespace suco {

namespace {

// Helper to clean trailing newlines and carriage returns
std::string clean_string(std::string str) {
    while (!str.empty() && (str.back() == '\n' || str.back() == '\r')) {
        str.pop_back();
    }
    return trim(str);
}

// Safely get canonical absolute path
std::string get_canonical_path(const std::string& path) {
    std::error_code ec;
    auto abs_p = std::filesystem::absolute(path, ec);
    if (ec) return path;
    auto can_p = std::filesystem::canonical(abs_p, ec);
    if (ec) return abs_p.string();
    return can_p.string();
}

// Resolves tool name using "which" (POSIX) or a PATH walk (Windows, which has
// no `which`; shelling to `where` would spawn a console and need output parsing).
std::string resolve_bin_path(const std::string& name) {
    if (name.starts_with("/") || name.starts_with("./") || name.starts_with("../")) {
        return get_canonical_path(name);
    }
#ifdef _WIN32
    // Drive-letter or UNC absolute paths, and anything already containing a
    // separator, are used as-is (mirrors the POSIX early-out above).
    if ((name.size() > 1 && name[1] == ':') ||
        name.find('\\') != std::string::npos || name.find('/') != std::string::npos) {
        return get_canonical_path(name);
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env) return "";
    std::stringstream ss(path_env);
    std::string dir;
    const bool has_ext = name.size() > 4 &&
        (name.compare(name.size() - 4, 4, ".exe") == 0 ||
         name.compare(name.size() - 4, 4, ".bat") == 0 ||
         name.compare(name.size() - 4, 4, ".cmd") == 0);
    while (std::getline(ss, dir, ';')) {
        if (dir.empty()) continue;
        std::error_code ec;
        std::filesystem::path cand = std::filesystem::path(dir) / name;
        if (!has_ext) {
            std::filesystem::path cand_exe = cand;
            cand_exe += ".exe";
            if (std::filesystem::exists(cand_exe, ec)) return get_canonical_path(cand_exe.string());
        }
        if (std::filesystem::exists(cand, ec) && !std::filesystem::is_directory(cand, ec)) {
            return get_canonical_path(cand.string());
        }
    }
    return "";
#else
    auto [code, out] = run_local_capture({"which", name});
    if (code == 0 && !out.empty()) {
        return get_canonical_path(clean_string(out));
    }
    return "";
#endif
}

// Computes SHA-256 hash of a file
std::string compute_file_hash(const std::string& filepath) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f) return "";

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    char buf[65536];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        if (EVP_DigestUpdate(ctx, buf, f.gcount()) != 1) {
            EVP_MD_CTX_free(ctx);
            return "";
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    EVP_MD_CTX_free(ctx);

    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        result += digits[(digest[i] >> 4) & 0x0F];
        result += digits[digest[i] & 0x0F];
    }
    return result;
}

// Resolves cache directory ~/.cache/suco/toolchains
std::string get_toolchain_dir() {
    std::string dir = get_toolchain_cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string escape_json_string(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

ToolchainInfo ToolchainPacker::pack(const std::string& compiler_path, bool is_qt) {
    ToolchainInfo info;

    // 1. Resolve absolute compiler path
    std::string abs_compiler = resolve_bin_path(compiler_path);
    if (abs_compiler.empty()) {
        SUCO_LOG_ERROR("Could not resolve compiler path for: {}", compiler_path);
        return info;
    }
    info.resolved_compiler_path = abs_compiler;

    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(abs_compiler, ec);
    auto f_size = std::filesystem::file_size(abs_compiler, ec);
    uint64_t mtime_epoch = 0;
    if (!ec) {
        mtime_epoch = std::chrono::duration_cast<std::chrono::seconds>(mtime.time_since_epoch()).count();
    }

    std::string cache_file = get_toolchain_dir() + "/hash_cache.txt";
    std::string cache_prefix = abs_compiler + "|" + std::to_string(mtime_epoch) + "|" + std::to_string(f_size) + "|";
    
    // Check Cache
    std::ifstream cache_in(cache_file);
    if (cache_in.is_open()) {
        std::string line;
        while (std::getline(cache_in, line)) {
            if (line.starts_with(cache_prefix)) {
                std::vector<std::string> parts;
                std::stringstream ss(line.substr(cache_prefix.size()));
                std::string part;
                while (std::getline(ss, part, '|')) {
                    parts.push_back(part);
                }
                
                if (parts.size() >= 6) {
                    info.compiler_type = parts[0];
                    info.compiler_version = parts[1];
                    info.hash = parts[2];
                    info.archive_path = parts[3];
                    info.json_path = parts[4];
                    info.success = true;
                    
                    std::stringstream ss_contents(parts[5]);
                    std::string item;
                    while (std::getline(ss_contents, item, ',')) {
                        if (!item.empty()) info.contents.push_back(item);
                    }
                    
                    if (std::filesystem::exists(info.archive_path) && std::filesystem::exists(info.json_path)) {
                        return info;
                    }
                }
            }
        }
    }

    // 2. Identify compiler type and version
    std::string comp_name = normalize_compiler_name(abs_compiler);
    info.compiler_type = comp_name;

    std::string version = "Unknown";
    if (comp_name == "g++" || comp_name == "clang++") {
        auto [code, out] = run_local_capture({abs_compiler, "-dumpfullversion"});
        if (code == 0 && !out.empty()) {
            version = clean_string(out);
        } else {
            // Fallback to -dumpversion
            auto [code2, out2] = run_local_capture({abs_compiler, "-dumpversion"});
            if (code2 == 0 && !out2.empty()) {
                version = clean_string(out2);
            }
        }
    }
    info.compiler_version = version;

    SUCO_LOG_INFO("Analyzing toolchain: {} ({}) at {}", comp_name, version, abs_compiler);

    std::set<std::string> paths_to_pack;
    paths_to_pack.insert(abs_compiler);

    std::vector<std::string> binaries_to_hash = {abs_compiler};

    // 3. Find compiler components and search paths (Linux/WSL)
    if (comp_name == "g++" || comp_name == "clang++") {
        // cc1plus
        auto [code1, out1] = run_local_capture({abs_compiler, "-print-prog-name=cc1plus"});
        if (code1 == 0 && !out1.empty()) {
            std::string path = get_canonical_path(clean_string(out1));
            if (std::filesystem::exists(path)) {
                paths_to_pack.insert(path);
                binaries_to_hash.push_back(path);
            }
        }
        // cc1 (for pure C)
        auto [code1b, out1b] = run_local_capture({abs_compiler, "-print-prog-name=cc1"});
        if (code1b == 0 && !out1b.empty()) {
            std::string path = get_canonical_path(clean_string(out1b));
            if (std::filesystem::exists(path)) {
                paths_to_pack.insert(path);
                binaries_to_hash.push_back(path);
            }
        }
        // as (assembler)
        auto [code2, out2] = run_local_capture({abs_compiler, "-print-prog-name=as"});
        if (code2 == 0 && !out2.empty()) {
            std::string path = get_canonical_path(clean_string(out2));
            if (std::filesystem::exists(path)) {
                paths_to_pack.insert(path);
                binaries_to_hash.push_back(path);
            }
        }
        // ld (linker)
        auto [code3, out3] = run_local_capture({abs_compiler, "-print-prog-name=ld"});
        if (code3 == 0 && !out3.empty()) {
            std::string path = get_canonical_path(clean_string(out3));
            if (std::filesystem::exists(path)) {
                paths_to_pack.insert(path);
                binaries_to_hash.push_back(path);
            }
        }

        // Include search paths
        auto [code4, out4] = run_local_capture({abs_compiler, "-E", "-v", "-xc++", "/dev/null"});
        // Output is captured via 2>&1
        std::stringstream ss(out4);
        std::string line;
        bool in_includes = false;
        while (std::getline(ss, line)) {
            line = trim(line);
            if (line == "#include <...> search starts here:") {
                in_includes = true;
                continue;
            }
            if (line == "End of search list.") {
                in_includes = false;
                continue;
            }
            if (in_includes && !line.empty()) {
                std::string path = get_canonical_path(line);
                if (std::filesystem::exists(path)) {
                    paths_to_pack.insert(path);
                }
            }
        }

        // Library search paths
        auto [code5, out5] = run_local_capture({abs_compiler, "-print-search-dirs"});
        if (code5 == 0 && !out5.empty()) {
            std::stringstream ss5(out5);
            while (std::getline(ss5, line)) {
                if (line.starts_with("libraries: =")) {
                    std::string lib_part = line.substr(12); // Strip "libraries: ="
                    auto paths = split(lib_part, ':');
                    for (const auto& raw_p : paths) {
                        std::string clean_p = trim(raw_p);
                        if (!clean_p.empty()) {
                            std::string path = get_canonical_path(clean_p);
                            // Filter compiler-specific library paths to avoid packaging all system libraries
                            if (path.find("/gcc/") != std::string::npos || path.find(version) != std::string::npos) {
                                if (std::filesystem::exists(path)) {
                                    paths_to_pack.insert(path);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 4. Handle Qt components if requested
    if (is_qt) {
        std::vector<std::string> qt_tools = {
            "/usr/lib/qt6/moc",
            "/usr/lib/qt6/uic",
            "/usr/lib/qt6/rcc",
            "/usr/lib/qt6/bin/qmake",
            "/usr/lib/qt6/bin/qmake6"
        };
        // Add tools resolved from PATH as well
        for (const auto& name : {"moc", "uic", "rcc", "qmake", "qmake6"}) {
            std::string resolved = resolve_bin_path(name);
            if (!resolved.empty()) {
                qt_tools.push_back(resolved);
            }
        }

        for (const auto& path : qt_tools) {
            if (std::filesystem::exists(path)) {
                std::string clean_path = get_canonical_path(path);
                paths_to_pack.insert(clean_path);
                binaries_to_hash.push_back(clean_path);
            }
        }

        // Include Qt include directories
        std::vector<std::string> qt_includes = {
            "/usr/include/qt6",
            "/usr/include/x86_64-linux-gnu/qt6"
        };
        for (const auto& path : qt_includes) {
            if (std::filesystem::exists(path)) {
                paths_to_pack.insert(get_canonical_path(path));
            }
        }
    }

    // 5. Compute deterministic hash
    EVP_MD_CTX* hash_ctx = EVP_MD_CTX_new();
    if (!hash_ctx) {
        SUCO_LOG_ERROR("Failed to create OpenSSL hash context");
        return info;
    }
    if (EVP_DigestInit_ex(hash_ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(hash_ctx);
        SUCO_LOG_ERROR("Failed to initialize SHA-256 context");
        return info;
    }

    // Feed compiler type and version
    EVP_DigestUpdate(hash_ctx, comp_name.c_str(), comp_name.size());
    EVP_DigestUpdate(hash_ctx, version.c_str(), version.size());

    // Sort binaries to ensure deterministic hash order
    std::sort(binaries_to_hash.begin(), binaries_to_hash.end());
    binaries_to_hash.erase(std::unique(binaries_to_hash.begin(), binaries_to_hash.end()), binaries_to_hash.end());

    for (const auto& bin : binaries_to_hash) {
        std::string f_hash = compute_file_hash(bin);
        if (!f_hash.empty()) {
            EVP_DigestUpdate(hash_ctx, f_hash.c_str(), f_hash.size());
        }
    }

    unsigned char final_digest[EVP_MAX_MD_SIZE];
    unsigned int final_len = 0;
    EVP_DigestFinal_ex(hash_ctx, final_digest, &final_len);
    EVP_MD_CTX_free(hash_ctx);

    static constexpr char digits[] = "0123456789abcdef";
    std::string toolchain_hash;
    toolchain_hash.reserve(final_len * 2);
    for (unsigned int i = 0; i < final_len; ++i) {
        toolchain_hash += digits[(final_digest[i] >> 4) & 0x0F];
        toolchain_hash += digits[final_digest[i] & 0x0F];
    }

    info.hash = toolchain_hash;

    // 6. Setup output paths
    std::string cache_dir = get_toolchain_dir();
    info.archive_path = cache_dir + "/" + comp_name + "-" + version + "-" + toolchain_hash + ".tar.zst";
    info.json_path = cache_dir + "/" + comp_name + "-" + version + "-" + toolchain_hash + ".json";

    for (const auto& p : paths_to_pack) {
        info.contents.push_back(p);
    }

    // 7. Check if already cached
    if (std::filesystem::exists(info.archive_path) && std::filesystem::exists(info.json_path)) {
        SUCO_LOG_INFO("Toolchain {} is already archived & cached.", toolchain_hash);
        info.success = true;
        return info;
    }

    SUCO_LOG_INFO("Creating toolchain archive for hash {}...", toolchain_hash);

    // 8. Pack the toolchain into a .tar.zst.
#ifdef _WIN32
    // Git's GNU tar (first on PATH) is wrong for Windows twice over: --zstd/-I zstd
    // shell out to a zstd.exe that is not installed, and it reads the "C:\..."
    // archive path as a remote rmt host ("Cannot connect to C:"). Use the System32
    // bsdtar (libarchive) instead: it has zstd built in (--zstd) and handles
    // drive-letter paths. bsdtar strips the drive/leading slash itself and stores
    // relative paths, so no -C / dance.
    std::vector<std::string> tar_args = {"C:\\Windows\\System32\\tar.exe", "--zstd", "-cf", info.archive_path};
    for (const auto& abs_p : paths_to_pack) {
        tar_args.push_back(abs_p);
    }
#else
    // POSIX: GNU tar with the external zstd filter, extracting relative to /.
    std::vector<std::string> tar_args = {"tar", "-I", "zstd", "-cf", info.archive_path, "-C", "/"};
    for (const auto& abs_p : paths_to_pack) {
        if (abs_p.starts_with("/")) {
            tar_args.push_back(abs_p.substr(1)); // strip leading slash
        } else {
            tar_args.push_back(abs_p);
        }
    }
#endif

    auto [tar_code, tar_out] = run_local_capture(tar_args);
    if (tar_code != 0) {
        SUCO_LOG_ERROR("tar failed with exit code: {}. Output: {}", tar_code, tar_out);
        std::error_code err;
        std::filesystem::remove(info.archive_path, err);
        return info;
    }

    // 9. Write metadata JSON
    std::ofstream json_f(info.json_path);
    if (json_f) {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        char time_str[64];
        std::strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now_time));

        json_f << "{\n";
        json_f << "  \"compiler_type\": \"" << escape_json_string(comp_name) << "\",\n";
        json_f << "  \"compiler_version\": \"" << escape_json_string(version) << "\",\n";
        json_f << "  \"toolchain_hash\": \"" << escape_json_string(toolchain_hash) << "\",\n";
        json_f << "  \"created_at\": \"" << time_str << "\",\n";
        json_f << "  \"contents\": [\n";
        for (size_t i = 0; i < info.contents.size(); ++i) {
            json_f << "    \"" << escape_json_string(info.contents[i]) << "\"";
            if (i + 1 < info.contents.size()) json_f << ",";
            json_f << "\n";
        }
        json_f << "  ]\n";
        json_f << "}\n";
        json_f.close();
    } else {
        SUCO_LOG_ERROR("Failed to write metadata JSON file: {}", info.json_path);
        std::error_code err;
        std::filesystem::remove(info.archive_path, err);
        return info;
    }

    SUCO_LOG_INFO("Successfully archived toolchain (Archive: {}, Metadata: {})", info.archive_path, info.json_path);
    info.success = true;

    // Cache the newly packed toolchain info
    std::ofstream cache_out(cache_file, std::ios::app);
    if (cache_out) {
        std::string contents_str;
        for (size_t i = 0; i < info.contents.size(); ++i) {
            contents_str += info.contents[i];
            if (i + 1 < info.contents.size()) contents_str += ",";
        }
        
        cache_out << cache_prefix
                  << comp_name << "|"
                  << version << "|"
                  << toolchain_hash << "|"
                  << info.archive_path << "|"
                  << info.json_path << "|"
                  << contents_str << "\n";
    }

    return info;
}

} // namespace suco
