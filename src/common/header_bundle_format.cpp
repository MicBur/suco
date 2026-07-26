#include "header_bundle_format.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace suco::header_bundle {

namespace {

// Little-endian appenders — fixed encoding so the archive bytes (and therefore
// its hash) are identical on every platform, regardless of host endianness.
void put_u16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void put_u32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void put_u64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// Bounds-checked little-endian readers. `pos` advances only on success.
bool get_u16(const std::string& in, size_t& pos, uint16_t& v) {
    if (pos + 2 > in.size()) return false;
    v = static_cast<uint16_t>((uint8_t)in[pos]) |
        (static_cast<uint16_t>((uint8_t)in[pos + 1]) << 8);
    pos += 2;
    return true;
}
bool get_u32(const std::string& in, size_t& pos, uint32_t& v) {
    if (pos + 4 > in.size()) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>((uint8_t)in[pos + i]) << (8 * i);
    pos += 4;
    return true;
}
bool get_u64(const std::string& in, size_t& pos, uint64_t& v) {
    if (pos + 8 > in.size()) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>((uint8_t)in[pos + i]) << (8 * i);
    pos += 8;
    return true;
}

} // namespace

std::string pack(std::vector<File> files) {
    // Sort by path so discovery order can't change the bytes. Stable sort keeps
    // the last occurrence deterministic before we dedup.
    std::sort(files.begin(), files.end(),
              [](const File& a, const File& b) { return a.path < b.path; });
    // Drop earlier duplicates of a path, keeping the last (std::unique keeps the
    // first of an equal run, so reverse the intent by keeping the last write:
    // erase all but the final entry for each path).
    std::vector<File> uniq;
    uniq.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        if (i + 1 < files.size() && files[i + 1].path == files[i].path) continue; // a later dup exists
        uniq.push_back(std::move(files[i]));
    }

    std::string out;
    out.append(kMagic, sizeof(kMagic));
    put_u16(out, kVersion);
    put_u32(out, static_cast<uint32_t>(uniq.size()));
    for (const auto& f : uniq) {
        put_u32(out, static_cast<uint32_t>(f.path.size()));
        out.append(f.path);
        put_u64(out, static_cast<uint64_t>(f.content.size()));
        out.append(f.content);
    }
    return out;
}

bool unpack(const std::string& archive, std::vector<File>& out) {
    out.clear();
    size_t pos = 0;
    if (archive.size() < sizeof(kMagic) ||
        std::memcmp(archive.data(), kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    pos = sizeof(kMagic);
    uint16_t version = 0;
    if (!get_u16(archive, pos, version) || version != kVersion) return false;
    uint32_t count = 0;
    if (!get_u32(archive, pos, count)) return false;

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t path_len = 0;
        if (!get_u32(archive, pos, path_len)) return false;
        if (pos + path_len > archive.size()) return false;
        File f;
        f.path.assign(archive, pos, path_len);
        pos += path_len;
        uint64_t content_len = 0;
        if (!get_u64(archive, pos, content_len)) return false;
        if (content_len > archive.size() - pos) return false; // avoids overflow in pos+len
        f.content.assign(archive, pos, static_cast<size_t>(content_len));
        pos += static_cast<size_t>(content_len);
        out.push_back(std::move(f));
    }
    return pos == archive.size();
}

namespace {

// True if `rel` is a safe, in-tree relative path: not absolute, no ".." or root
// component that would let it escape the workspace. Lexical only.
bool is_safe_relative(const fs::path& rel) {
    if (rel.is_absolute()) return false;
    for (const auto& part : rel) {
        if (part == "..") return false;
    }
    return true;
}

} // namespace

bool materialize(const std::string& archive, const std::string& dest_dir) {
    std::vector<File> files;
    if (!unpack(archive, files)) return false;

    std::error_code ec;
    fs::path base = fs::path(dest_dir);
    fs::create_directories(base, ec);
    if (ec) return false;

    for (const auto& f : files) {
        fs::path rel = fs::path(f.path).lexically_normal();
        if (!is_safe_relative(rel)) return false; // zip-slip guard
        fs::path out_path = base / rel;
        if (out_path.has_parent_path()) {
            fs::create_directories(out_path.parent_path(), ec);
            if (ec) return false;
        }
        std::ofstream os(out_path, std::ios::binary | std::ios::trunc);
        if (!os) return false;
        os.write(f.content.data(), static_cast<std::streamsize>(f.content.size()));
        if (!os.good()) return false;
    }
    return true;
}

std::vector<std::string> remap_include_flags(const std::vector<std::string>& include_flags,
                                             const std::string& project_root,
                                             const std::string& dest_dir) {
    const fs::path root = fs::path(project_root).lexically_normal();
    const fs::path dest = fs::path(dest_dir);

    // Rewrite a single include path (the part after "-I") if it lies inside root.
    auto remap_path = [&](const std::string& raw) -> std::string {
        fs::path p = fs::path(raw).lexically_normal();
        fs::path rel = p.lexically_relative(root);
        if (rel.empty()) return raw;                  // unrelated tree
        if (rel == fs::path(".")) return (dest).generic_string();          // the root itself
        if (!is_safe_relative(rel)) return raw;       // resolves outside root -> system/toolchain, leave as-is
        return (dest / rel).generic_string();
    };

    std::vector<std::string> out;
    out.reserve(include_flags.size());
    for (size_t i = 0; i < include_flags.size(); ++i) {
        const std::string& tok = include_flags[i];
        if (tok.rfind("-I", 0) == 0 && tok.size() > 2) {
            out.push_back("-I" + remap_path(tok.substr(2)));       // attached: -I<path>
        } else if (tok == "-I" && i + 1 < include_flags.size()) {
            out.push_back("-I");                                    // separated: -I <path>
            out.push_back(remap_path(include_flags[++i]));
        } else {
            out.push_back(tok);                                     // pass through
        }
    }
    return out;
}

} // namespace suco::header_bundle
