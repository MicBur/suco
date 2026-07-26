# Remote Preprocessing — implementation plan (v1.0.0 #42)

Companion to the concept in [remote_preprocessing_design.md](remote_preprocessing_design.md).
This file is the **executable plan**: the exact wire framing, worker command
reconstruction, and the byte-identity procedure that gates default-on. It is
grounded in the current code, so the remaining wire slice is a small, verifiable diff.

## Status

- **Done & merged (dormant):** the byte-deterministic bundle format
  (`src/common/header_bundle_format.*`: `pack`/`unpack`/`materialize`/`remap_include_flags`),
  the client dependency scanner (`src/client/header_bundle.*`), and their self-tests.
- **Reserved:** `PACKET_DIRECT_COMPILE_REQ_V3 = 26` (protocol.h); the client config
  flag `remote_preprocess_enabled` (env `SUCO_REMOTE_PREPROCESS`, default off).
- **Not done:** the wire slice below. It touches the byte-identity-critical compile
  path, so it must land with the grid byte-identity loop (§4), not blind.

## 1. Why V3 needs its own path (not a tweak of the existing one)

The normal direct-dispatch path ships **preprocessed** source and the worker compiles
it with `-x c++-cpp-output` (`JobExecutor::rebuild_compiler_command`, job_executor.cpp).
Remote preprocessing ships **raw** source + project headers; the worker must instead:
- compile with `-x c++` (raw), not `-x c++-cpp-output`;
- keep and **remap** the `-I` flags (the preprocessed path can ignore them);
- lay out project headers so both `-I` and source-relative `#include "…"` resolve.

So V3 is a **separate** send function and a **separate** worker execute function.
Do NOT modify `rebuild_compiler_command` or `handle_compile_job` — keeping the two
paths disjoint is what makes the gated-off feature unable to affect normal compiles.

## 2. Wire framing — `PACKET_DIRECT_COMPILE_REQ_V3`

Mirror the field-at-a-time framing of the V1/V2 sender
(`network_client.cpp` ~line 1446) and receiver (`worker.cpp` ~line 830). All lengths
are `uint32` network byte order; every blob may be zstd-compressed with a leading
`uint8` flag, exactly like the existing `src`/`hs` fields.

```
req_type            u32   = PACKET_DIRECT_COMPILE_REQ_V3 (26)
cmd_len, cmd              base command WITHOUT -I flags and WITHOUT -c/-o (see §3)
file_len, file           client's relative source path (as today)
proot_len, project_root  ABSOLUTE, normalised project root (for include remap)
inc_count           u32   number of include flags
  repeated: iflag_len,u32 + iflag bytes   each original -I<path> (ABSOLUTE path)
src_comp            u8    1 = raw source is zstd
src_len, src             RAW source bytes (NOT preprocessed)
tc_hash_len, tc_hash     toolchain hash (as today)
bundle_comp         u8    1 = bundle archive is zstd
bundle_len, bundle       header_bundle::pack() output (compressed)
```

The client MUST send **absolute** `project_root` and `-I` paths so the worker's
lexical `remap_include_flags` is unambiguous (relative client paths can't be remapped
without the client CWD).

## 3. Client side (`network_client.cpp`, gated by `remote_preprocess_enabled`)

In the direct-dispatch send, when the flag is on AND `build_header_bundle(cmd, root)`
returns `ok`:
1. Read the **raw** source bytes of `cmd.source_file` (skip using `cmd.preprocessed_source`).
2. Canonicalise `project_root` and each `cmd.include_paths` entry to absolute.
3. Build the **base command**: the existing `remote_cmd` minus the `-I…` tokens
   (they travel as the separate `inc` list) and minus `-c`/`-o` (worker re-adds).
4. Send the V3 frame (§2). On any bundle failure → fall through to the existing
   V1/V2 path (never fail the build for a remote-preprocess miss — design §4).

Note: for the FIRST validation milestone it is acceptable to still preprocess
locally and ignore the result — that isolates "does the remote compile match?"
from the pipeline change that actually skips local `-E`. The CPU win (bypassing
`preprocessing_pool` for V3 TUs) is a follow-up once byte-identity holds.

## 4. Worker side (new `JobExecutor::execute_remote_preprocess`, called from a V3 branch)

Do not reuse `execute()`'s preprocessed-input assumptions. New function:
1. `job_dir = get_temp_file(".job")`; create it.
2. `header_bundle::materialize(bundle, job_dir)` — lays out project headers under
   `job_dir/<relpath>` (zip-slip guarded).
3. Write the raw source to `job_dir/<safe rel_name>` (reuse `job_source_name`).
4. `remapped = remap_include_flags(inc_list, project_root, job_dir)`.
5. Build the command: `<compiler> <base flags> <remapped -I...> -x c++ -c <rel_name> -o <temp_out>`
   plus the same determinism flags the normal path adds
   (`-fdebug-prefix-map=<job_dir>=.`, and keep `-ffile-prefix-map=<project_root>=.`
   so `__FILE__`/paths match native — this is the crux for byte-identity).
6. Run from `job_dir` via `run_local_capture`; read `temp_out`; respond via the
   SAME response writer the V1 path uses; clean up.

## 5. Byte-identity gate (invariant #1 — the whole point)

Before flipping any default, prove on the real grid that a V3-built object is
**byte-identical** to the native and to the existing-path grid object:

```bash
# native
g++ -c foo.cpp -o native.o <flags>
# existing grid path
SUCO_REMOTE_PREPROCESS=0 suco-cl++ -c foo.cpp -o grid_pp.o <flags>
# remote-preprocess path
SUCO_REMOTE_PREPROCESS=1 suco-cl++ -c foo.cpp -o grid_rpp.o <flags>
cmp native.o grid_rpp.o && cmp grid_pp.o grid_rpp.o && echo "BYTE-IDENTICAL"
```

Run across the RocksDB / GoogleTest corpora (the existing benchmark TUs). Any diff
is a bug in §3–§4 (usually a path not prefix-mapped, or an include not remapped).
Only after 100% identity across the corpus does `remote_preprocess_enabled` earn a
default, and even then likely opt-in first. Until then it stays off and CI never
sets the flag.

## 6. Later optimisations (not needed for correctness)

- **Bundle dedup via the blob cache** (packets 23/24): upload the bundle once under
  its hash; workers fetch by hash instead of receiving it inline every TU. The design
  doc's BUNDLE_QUERY/TRANSFER/REQ (19–21) are unnecessary — those numbers are already
  taken and the generic blob cache already does content-addressed storage.
- **Skip local `-E`** in `pipeline_orchestrator` for V3 TUs — the actual client-CPU win.
- **System-header drift guard** (design §6.2): optionally include selected system
  headers in the bundle when worker/client toolchains differ.
