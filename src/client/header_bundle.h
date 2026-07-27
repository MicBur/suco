#pragma once
//
// v1.0.0 #42 — Remote preprocessing: client-side header-bundle builder.
//
// Scans a translation unit's PROJECT header dependencies (via the compiler's
// own `-MM`, which by construction excludes system headers — design §2.2) and
// packs them into a deterministic, content-addressed archive so a worker can
// preprocess the TU remotely instead of the client running `-E` locally.
//
// This is the foundation slice: it is DORMANT — nothing on the compile hot path
// calls it yet, and there is no wire change. Wiring it behind the opt-in
// SUCO_REMOTE_PREPROCESS flag, plus the worker-side unpack/remap/preprocess, are
// the follow-up steps of #42. Keeping the scanner landable and testable on its
// own keeps each step small and the byte-identity gate (invariant #1) honest.
//
#include <string>
#include <vector>

#include "../common/hash_util.h"  // suco::CacheKeyInput, suco::RequestContext

struct CompilerCommand;

namespace suco::header_bundle {

struct BuildResult {
    bool ok = false;             // false => caller must fall back to local preprocessing (§4)
    std::string hash;            // SHA-256 of the UNCOMPRESSED packed archive (the cache key)
    std::string archive_zstd;    // zstd-compressed packed archive (what goes on the wire)
    std::vector<std::string> paths;  // relative paths included, for logging/diagnostics
    size_t uncompressed_size = 0;
    bool has_time_macros = false;    // a project header references __DATE__/__TIME__/__TIMESTAMP__
};

// Build the project-header bundle for `cmd`. `project_root` bounds what counts as
// a project header: dependencies resolving outside it (system/toolchain headers)
// are left out, on the assumption that a compatible worker resolves them from its
// own toolchain. Returns ok=false on any scan/read failure so the caller can fall
// back — this function never throws for an expected I/O or tooling error.
BuildResult build_header_bundle(const CompilerCommand& cmd, const std::string& project_root);

// Switch `cmd` to the remote-preprocess (V3) path if possible: detect the project
// root, build the header bundle, read the raw source, populate cmd.rpp_* and set
// `use_remote_preprocess`, and override cmd.content_hash with a V3 key derived from
// {raw source + bundle hash} through `key` (so toolchain identity still counts and
// V3 objects get their own cache namespace). Returns true if switched; false leaves
// `cmd` on the normal preprocessed path. Shared by every client dispatch pipeline.
bool enable_remote_preprocess(CompilerCommand& cmd, const suco::CacheKeyInput& key,
                              const suco::RequestContext& ctx);

} // namespace suco::header_bundle
