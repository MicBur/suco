#pragma once
//
// v1.0.0 #42 — Remote preprocessing: the on-the-wire header-bundle format.
//
// A header bundle is a set of {relative path -> file bytes} shipped so a worker
// can preprocess a translation unit remotely. This file defines ONLY the
// container format; the client-side dependency scan that fills it lives in
// src/client/header_bundle.*, and the worker-side unpack reuses unpack() here.
//
// Determinism is the whole point (invariant #1, byte-identity of cache keys):
// the SAME set of files MUST pack to the SAME bytes regardless of the order the
// scanner discovered them or the platform it ran on. So entries are sorted by
// path (raw byte order) and every integer is little-endian. Paths are stored
// with '/' separators; callers normalise before packing.
//
// The format is deliberately trivial (not tar): a 8-byte magic+version, a u32
// entry count, then per entry [u32 path_len | path | u64 content_len | content].
// It carries no timestamps, modes, or uids — nothing that would perturb the hash.
//
#include <cstdint>
#include <string>
#include <vector>

namespace suco::header_bundle {

struct File {
    std::string path;     // relative, '/'-separated, e.g. "src/foo/bar.h"
    std::string content;  // raw bytes
};

// Magic "SUCOHB" + format version. Bump only on an incompatible layout change.
constexpr char kMagic[6] = {'S', 'U', 'C', 'O', 'H', 'B'};
constexpr uint16_t kVersion = 1;

// Serialise `files` into one deterministic archive buffer. `files` may be in any
// order and may contain duplicate paths; pack() sorts by path and drops later
// duplicates so the output depends only on the final content of each path.
// The result is NOT compressed — compress with zstd_util separately if desired.
std::string pack(std::vector<File> files);

// Inverse of pack(). Returns false (and leaves `out` unspecified) if `archive`
// is truncated, has a bad magic/version, or declares sizes past its end.
bool unpack(const std::string& archive, std::vector<File>& out);

// Worker side (v1.0.0 #42): unpack `archive` and write every entry under
// `dest_dir`, recreating the relative directory structure. Refuses any entry
// whose path escapes `dest_dir` (absolute, or containing "..") so a hostile
// bundle can't write outside the workspace. Returns false on a bad archive, an
// unsafe path, or an I/O error.
bool materialize(const std::string& archive, const std::string& dest_dir);

// Worker side (v1.0.0 #42): rewrite a compile's include flags so they point at
// the materialised bundle instead of the client's original tree — the crux of
// remote preprocessing (design §6.3). For each entry in `include_flags`:
//   * a "-I<path>" (or "-I <path>") that lexically resolves INSIDE `project_root`
//     becomes "-I<dest_dir>/<path-relative-to-root>" (the unpacked location);
//   * anything else (system/toolchain -I outside the root, non-include flags) is
//     passed through unchanged — a compatible worker resolves system headers
//     from its own toolchain (design §2.2).
// Pure lexical path math (no disk access) so it is deterministic and testable.
std::vector<std::string> remap_include_flags(const std::vector<std::string>& include_flags,
                                             const std::string& project_root,
                                             const std::string& dest_dir);

} // namespace suco::header_bundle
