#include "header_set_hasher.h"
#include <cstring>
#include <string_view>
#include "logging.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <set>
#include <mutex>
#include <unordered_map>
#include <openssl/evp.h>

namespace suco {

namespace {

std::mutex g_header_cache_mutex;
std::unordered_map<std::string, std::string> g_header_file_hashes;

// Helper to compute SHA-256 of a file with thread-safe in-memory cache
std::string get_file_sha256(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(g_header_cache_mutex);
        auto it = g_header_file_hashes.find(path);
        if (it != g_header_file_hashes.end()) {
            return it->second;
        }
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) return "";

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    char buf[16384];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0) {
        if (EVP_DigestUpdate(ctx, buf, f.gcount()) != 1) {
            EVP_MD_CTX_free(ctx);
            return "";
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        result += digits[(digest[i] >> 4) & 0x0F];
        result += digits[digest[i] & 0x0F];
    }

    {
        std::lock_guard<std::mutex> lock(g_header_cache_mutex);
        g_header_file_hashes[path] = result;
    }
    return result;
}

} // namespace

std::string HeaderSetHasher::compute_hash(CompilerCommand& cmd) {
    std::set<std::string> header_paths;
    
    // Split the preprocessed output into "system header text" (goes into the PCH /
    // header set) and "stripped source" (the TU itself plus its own line markers).
    //
    // memchr-based single pass over the raw buffer, appending into two RESERVED
    // buffers. The previous stringstream+getline version allocated per line and
    // built a `line + "\n"` temporary per append — on an 8MB preprocessed TU that
    // is ~200k lines of allocator traffic, ~55ms per TU single-threaded and far
    // worse under a parallel build's allocator contention. Byte-identical output to
    // the old loop (verified against recorded header-set hashes): getline's line
    // semantics are replicated exactly, including CR retention and the trailing
    // line without a newline.
    std::string source_base = cmd.source_file;
    size_t last_slash = source_base.find_last_of("/\\");
    if (last_slash != std::string::npos) source_base = source_base.substr(last_slash + 1);

    const std::string& src = cmd.preprocessed_source;
    std::string header_set_source;
    std::string stripped_source;
    header_set_source.reserve(src.size());
    stripped_source.reserve(src.size() / 2);
    bool in_header = false;

    const char* data = src.data();
    const size_t total = src.size();
    size_t pos = 0;
    while (pos < total) {
        const char* nl = static_cast<const char*>(std::memchr(data + pos, '\n', total - pos));
        const size_t eol = nl ? static_cast<size_t>(nl - data) : total;
        std::string_view line(data + pos, eol - pos);
        pos = eol + 1;

        const bool is_marker =
            (line.size() >= 2 && line[0] == '#' && line[1] == ' ') ||
            (line.size() >= 6 && line.compare(0, 6, "#line ") == 0);

        if (is_marker) {
            size_t first_quote = line.find('"');
            size_t last_quote = line.rfind('"');
            if (first_quote != std::string_view::npos && last_quote != std::string_view::npos &&
                last_quote > first_quote) {
                std::string_view file_path = line.substr(first_quote + 1, last_quote - first_quote - 1);

                if (file_path == "<built-in>" || file_path == "<command-line>" ||
                    file_path == "<command line>" || file_path.empty()) {
                    in_header = false;
                } else {
                    // Only system/library headers belong to the header set; the
                    // compilation unit and project-local headers stay in the TU.
                    // "/usr/" covers Linux. A path containing "mingw" covers the
                    // Windows toolchain trees (Qt's mingw1310_64, MSYS2's mingw64);
                    // Linux cross-headers (/usr/x86_64-w64-mingw32/...) were already
                    // matched by the /usr/ prefix. Purely additive: no /usr/ path
                    // changes membership, so existing Linux header-set keys cannot
                    // move (invariant #1), and Windows had no header sets before.
                    size_t path_slash = file_path.find_last_of("/\\");
                    std::string_view f_base = (path_slash == std::string_view::npos)
                                                  ? file_path
                                                  : file_path.substr(path_slash + 1);
                    const bool is_system_path =
                        file_path.starts_with("/usr/") ||
                        file_path.find("mingw") != std::string_view::npos;
                    if (f_base == source_base || !is_system_path) {
                        in_header = false;
                    } else {
                        in_header = true;
                        header_paths.insert(std::string(file_path));
                    }
                }
            }
            // Keep the markers that belong to the compilation unit itself (and to
            // <built-in>/<command-line>): without them the worker resolves
            // __FILE__/__LINE__ against its temp input instead of the real source.
            // Markers of the stripped system headers stay dropped — their text lives
            // in the PCH/header set, not here.
            if (!in_header) {
                stripped_source.append(line.data(), line.size());
                stripped_source.push_back('\n');
            }
            continue;
        }

        if (in_header) {
            header_set_source.append(line.data(), line.size());
            header_set_source.push_back('\n');
        } else {
            stripped_source.append(line.data(), line.size());
            stripped_source.push_back('\n');
        }
    }

    // No system headers found — there is no header set, so say so instead of
    // returning a hash over the flags alone. The hash below is fed from flags,
    // compiler version and toolchain even when header_paths is empty, so it comes
    // back non-empty regardless; job_sender then takes that as "this TU has a header
    // set" and swaps preprocessed_source for the stripped source, which was never
    // filled in — the worker receives a header-set hash, no header text and an EMPTY
    // TU, and can only answer HEADER_SET_MISSING (-5). Every job, every time.
    // That is how it behaves on Windows today: the split below only recognises
    // headers under /usr/, so MinGW's C:/Qt/Tools/... headers never match and the
    // grid path is dead — the -5 self-heal quietly recompiles everything locally.
    // Returning "" here keeps the header-set machinery switched off for this TU and
    // ships the full preprocessed source, which is what the pre-header-set path did.
    // Cache keys are unaffected: content_hash is computed in job_sender before this
    // function runs.
    if (header_paths.empty()) {
        cmd.header_set_hash.clear();
        return "";
    }

    // Move, don't copy: these locals are multi-MB (the whole preprocessed TU split in
    // two) and are not read again after this point — the hash below is fed from
    // header_paths, flags, compiler version and toolchain, never the source text.
    // Copying them was ~half of the per-TU "hset+copies" cost on a cold build.
    cmd.header_set_source = std::move(header_set_source);
    cmd.stripped_source = std::move(stripped_source);


    // Create cryptographic hash context
    EVP_MD_CTX* hash_ctx = EVP_MD_CTX_new();
    if (!hash_ctx) return "";
    EVP_DigestInit_ex(hash_ctx, EVP_sha256(), nullptr);
    
    // Feed header contents (paths are sorted naturally by std::set)
    for (const auto& path : header_paths) {
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::string f_hash = get_file_sha256(path);
            if (!f_hash.empty()) {
                EVP_DigestUpdate(hash_ctx, f_hash.c_str(), f_hash.size());
            }
        }
    }
    
    // Feed normalized compiler flags
    std::string flags_input;
    for (const auto& d : cmd.defines) flags_input += d + " ";
    for (const auto& i : cmd.include_paths) flags_input += i + " ";
    for (const auto& f : cmd.other_flags) {
        if (f == "-c" || f == "-o" || f == "/c" || f.find("/Fo") == 0) continue;
        flags_input += f + " ";
    }
    flags_input += cmd.language_standard + " ";
    EVP_DigestUpdate(hash_ctx, flags_input.c_str(), flags_input.size());
    
    // Feed compiler version + target arch
    std::string comp_ver = cmd.get_compiler_version();
    std::string comp_arch = cmd.get_target_architecture();
    EVP_DigestUpdate(hash_ctx, comp_ver.c_str(), comp_ver.size());
    EVP_DigestUpdate(hash_ctx, comp_arch.c_str(), comp_arch.size());
    
    // Feed toolchain hash
    EVP_DigestUpdate(hash_ctx, cmd.toolchain_hash.c_str(), cmd.toolchain_hash.size());
    
    unsigned char final_digest[EVP_MAX_MD_SIZE];
    unsigned int final_len = 0;
    EVP_DigestFinal_ex(hash_ctx, final_digest, &final_len);
    EVP_MD_CTX_free(hash_ctx);
    
    static constexpr char digits[] = "0123456789abcdef";
    std::string header_set_hash;
    header_set_hash.reserve(final_len * 2);
    for (unsigned int i = 0; i < final_len; ++i) {
        header_set_hash += digits[(final_digest[i] >> 4) & 0x0F];
        header_set_hash += digits[final_digest[i] & 0x0F];
    }
    
    cmd.header_set_hash = header_set_hash;
    return header_set_hash;
}

} // namespace suco
