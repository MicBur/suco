#include "header_bundle.h"

#include "compiler_command.h"
#include "utils.h"
#include "logging.h"
#include "../common/header_bundle_format.h"
#include "../common/hash_util.h"
#include "../common/zstd_util.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace suco::header_bundle {

namespace {

// Read a whole file into a string. Returns false if it can't be opened/read.
bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f.good() && !f.eof()) return false;
    out = ss.str();
    return true;
}

// Parse a Make-style dependency file (`-MF` output) into a list of paths.
// Format is `target.o: a.h b.h \<newline>   c.h`. We drop everything up to the
// first ':' (the target), join continuation lines, unescape `\ ` (a space in a
// path) and `\\`, and split the remainder on unescaped whitespace.
std::vector<std::string> parse_dep_file(const std::string& text) {
    // Strip the "target:" prefix (only the first colon that ends the target).
    std::string body;
    size_t colon = text.find(':');
    body = (colon == std::string::npos) ? text : text.substr(colon + 1);

    std::vector<std::string> deps;
    std::string cur;
    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (c == '\\') {
            char next = (i + 1 < body.size()) ? body[i + 1] : '\0';
            if (next == '\n') { ++i; continue; }            // line continuation
            if (next == ' ' || next == '\\') { cur.push_back(next); ++i; continue; } // escaped char
            cur.push_back(c);
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) { deps.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) deps.push_back(cur);
    return deps;
}

// Normalise a relative path to forward slashes so the bundle bytes (and hash)
// are identical whether the scan ran on Windows or Linux.
std::string to_forward_slash(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

} // namespace

BuildResult build_header_bundle(const CompilerCommand& cmd, const std::string& project_root) {
    BuildResult result;

    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::path(project_root), ec);
    if (ec) root = fs::path(project_root);

    // Write the dependency list to a private temp file rather than parsing merged
    // stdout+stderr (run_local_capture folds them together, so compiler notes
    // could otherwise corrupt the list).
    fs::path tmp = fs::temp_directory_path(ec) /
                   ("suco_dep_" + compute_sha256(cmd.source_file + "|" + cmd.output_file).substr(0, 16) + ".d");

    std::vector<std::string> args;
    args.reserve(cmd.defines.size() + cmd.include_paths.size() + 8);
    args.push_back(cmd.compiler_path);
    args.push_back("-MM");                              // user (project) headers only — excludes system headers
    if (!cmd.language_standard.empty()) args.push_back(cmd.language_standard);
    for (const auto& d : cmd.defines) args.push_back(d);       // already carry their -D prefix
    for (const auto& i : cmd.include_paths) args.push_back(i); // already carry their -I prefix
    args.push_back("-MF");
    args.push_back(tmp.string());
    args.push_back(cmd.source_file);

    auto [exit_code, out] = run_local_capture(args);
    if (exit_code != 0) {
        SUCO_LOG_INFO("header_bundle: dependency scan for {} failed (exit {}), falling back to local preprocessing", cmd.source_file, exit_code);
        fs::remove(tmp, ec);
        return result; // ok = false
    }

    std::string dep_text;
    bool read_ok = read_file(tmp, dep_text);
    fs::remove(tmp, ec);
    if (!read_ok) {
        SUCO_LOG_INFO("header_bundle: could not read dep file for {}, falling back", cmd.source_file);
        return result;
    }

    fs::path source_abs = fs::weakly_canonical(fs::path(cmd.source_file), ec);

    std::vector<File> files;
    for (const std::string& dep : parse_dep_file(dep_text)) {
        fs::path abs = fs::weakly_canonical(fs::path(dep), ec);
        if (ec) { ec.clear(); continue; }
        if (abs == source_abs) continue; // the TU itself is shipped separately, not in the bundle

        // Only project headers: skip anything that resolves outside project_root
        // (system/toolchain headers are assumed present on a compatible worker).
        fs::path rel = fs::relative(abs, root, ec);
        if (ec || rel.empty() || rel.native().rfind(fs::path("..").native(), 0) == 0) {
            ec.clear();
            continue;
        }

        std::string content;
        if (!read_file(abs, content)) {
            // A project header we can't read means we can't build a faithful bundle;
            // fall back rather than ship an incomplete set (byte-identity, invariant #1).
            SUCO_LOG_INFO("header_bundle: unreadable project header {} for {}, falling back", abs.string(), cmd.source_file);
            return result;
        }
        files.push_back(File{ to_forward_slash(rel.generic_string()), std::move(content) });
    }

    result.paths.reserve(files.size());
    for (const auto& f : files) result.paths.push_back(f.path);

    std::string archive = pack(std::move(files));
    result.uncompressed_size = archive.size();
    result.hash = compute_sha256(archive);
    result.archive_zstd = compress_zstd(archive);
    result.ok = true;
    return result;
}

bool enable_remote_preprocess(CompilerCommand& cmd, const suco::CacheKeyInput& key,
                              const suco::RequestContext& ctx) {
    if (cmd.is_msvc || !cmd.module_cmis.empty()) return false;

    std::error_code ec;
    std::string abs_root = suco::detect_checkout_root(".", ctx);
    if (abs_root.empty()) return false;
    std::string c = fs::weakly_canonical(abs_root, ec).string();
    if (!ec) abs_root = c;
    ec.clear();

    BuildResult bundle = build_header_bundle(cmd, abs_root);
    if (!bundle.ok) return false;

    std::ifstream sf(cmd.source_file, std::ios::binary);
    if (!sf) return false;
    std::ostringstream ss;
    ss << sf.rdbuf();
    std::string raw = ss.str();

    // Conservative guards for the skip-local-E path: a TU that would preprocess
    // DIFFERENTLY on the worker must not go remote. Reject __DATE__/__TIME__/
    // __TIMESTAMP__ (they would capture the worker's clock, not the client's) and
    // C++20 modules (`import`/`module` need CMI handling the V3 path doesn't do).
    // These checks work on raw source because both survive preprocessing. A false
    // positive just falls back to normal preprocessing — correct beats fast.
    if (suco::contains_time_macros(raw) || suco::uses_cxx_modules(raw)) return false;

    std::string v3_hash = suco::compute_cache_hash(raw + "\x1E" + bundle.hash, key, ctx);
    if (v3_hash.empty()) return false;

    cmd.rpp_raw_source = std::move(raw);
    cmd.rpp_bundle_archive = bundle.archive_zstd;  // compressed for the wire
    cmd.rpp_project_root = abs_root;
    cmd.rpp_include_flags.clear();
    for (const auto& inc : cmd.include_paths) {
        std::string p = (inc.rfind("-I", 0) == 0) ? inc.substr(2) : inc;
        std::string ap = fs::weakly_canonical(p, ec).string();
        if (ec) { ec.clear(); ap = fs::absolute(p, ec).string(); if (ec) { ec.clear(); ap = p; } }
        cmd.rpp_include_flags.push_back("-I" + ap);
    }
    cmd.content_hash = v3_hash;
    cmd.use_remote_preprocess = true;
    SUCO_LOG_INFO("[RPP] {} -> remote preprocessing ({} project headers, bundle {})",
                  cmd.source_file, bundle.paths.size(), bundle.hash.substr(0, 8));
    return true;
}

} // namespace suco::header_bundle
