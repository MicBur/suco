# Remote Preprocessing — implementation plan (v1.0.0 #42)

Companion to the concept in [remote_preprocessing_design.md](remote_preprocessing_design.md).
This file is the **executable plan**: the exact wire framing, worker command
reconstruction, and the byte-identity procedure that gates default-on. It is
grounded in the current code, so the remaining wire slice is a small, verifiable diff.

## Status — WIRE COMPLETE (correctness), opt-in (2026-07-27)

The end-to-end path is landed and grid-verified (#59): `SUCO_REMOTE_PREPROCESS=1` →
client ships RAW source + bundle → worker preprocesses (`Compiling direct RPP job`) →
byte-identical, deterministic objects (7/7 cache hits), binary runs; no-flag default
unchanged. Remaining work is OPTIMISATION only (§6): the client still runs a redundant
local `-E` for V3 TUs, so the client-CPU win isn't realised yet.

## Status (history)

- **Done & merged (dormant):** the byte-deterministic bundle format
  (`src/common/header_bundle_format.*`: `pack`/`unpack`/`materialize`/`remap_include_flags`),
  the client dependency scanner (`src/client/header_bundle.*`), and their self-tests.
- **Reserved:** `PACKET_DIRECT_COMPILE_REQ_V3 = 26` (protocol.h); the client config
  flag `remote_preprocess_enabled` (env `SUCO_REMOTE_PREPROCESS`, default off).
- **Validated (2026-07-27):** the core approach is proven **byte-identical/deterministic**
  on real g++ (node3, 15.2) — see §4a. The only `-g` caveat has a one-flag fix. So the
  wire slice is now de-risked plumbing, not an open research question.
- **Not done:** the wire slice below (client V3 send + worker V3 receive/execute + the
  V3 cache-key derivation). It still touches the compile path, so land it with the
  §5 grid byte-identity cmp across the real corpora.

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
5. Build the command: `<compiler> <base flags> <remapped -I...> -x c++ -c <rel_name> -o <temp_out>`.
   For byte-identity add **`-ffile-prefix-map=<job_dir>=.`** (empirically the only extra
   flag needed — see §4a; it normalises the per-job workspace path out of debug info so
   `-g` builds are deterministic across workers). `-x c++` because the input is RAW
   source, not `c++-cpp-output`. Run from `<job_dir>` with the source under its original
   relative name so `__FILE__` matches native.
6. Run from `job_dir` via `run_local_capture`; read `temp_out`; respond via the
   SAME response writer the V1 path uses; clean up.

## 4a. Empirical byte-identity findings (verified on node3, g++ 15.2, 2026-07-27)

The core approach — `materialize` a project-header bundle into a fresh workspace,
`remap_include_flags`, then compile the RAW source there — was tested against real
g++ before writing any wire code. Results, comparing the object built in two
DIFFERENT workspace temp dirs (the V3-to-V3 determinism that caching needs):

| Case | Result |
| :--- | :--- |
| `-O2`, nested project headers | **byte-identical / deterministic** — no extra flags |
| `-O2`, vs a plain native `g++ -c src/hello.cpp -Iinc` | **byte-identical** |
| `-g`, no prefix map | **non-deterministic** — the absolute workspace path leaks into debug info (`DW_AT_comp_dir` / file tables) |
| `-g`, with `-ffile-prefix-map=<workspace>=.` | **deterministic** |

**Conclusion:** the approach yields cacheable, byte-identical objects. The ONLY extra
flag the worker must add for correctness under `-g` is `-ffile-prefix-map=<workspace>=.`
— exactly analogous to the existing preprocessed path's `-fdebug-prefix-map=<job_dir>=.`
(job_executor.cpp:277). Prerequisites the wire code must honour, confirmed by the test:
- the raw source is compiled from the SAME relative path the client used
  (`src/hello.cpp`), run from the workspace root — so `__FILE__` and embedded paths match;
- project headers are materialised at their project-root-relative paths, and `-I`
  flags are remapped to `<workspace>/<rel>`, so include resolution is identical.

Cache-key note: V3 does NOT preprocess on the client, so the existing content_hash
(computed from preprocessed source) is unavailable. Derive the V3 cache key
deterministically from {normalised command, raw source bytes, bundle hash} instead —
all three are already stable and on hand at dispatch time.

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
- **Skip local `-E`** for V3 TUs — the actual client-CPU win, and the one remaining
  piece to make #42 deliver its value. Today `enable_remote_preprocess` runs at
  `pipeline_orchestrator.cpp` ~line 433, AFTER the `-E` at line 367, so V3 TUs pay for
  both `-E` (expensive) and `-MM` (cheap, inside `build_header_bundle`). The win:
  decide V3-eligibility BEFORE line 367 — if `build_header_bundle` succeeds (cheap
  `-MM`), skip the `-E` (`pp_args`/`run_local_capture`) and route the job straight to
  the V3 cache-query + dispatch with the V3 content_hash, bypassing the
  normalize/preprocessed-hash stages entirely. This is a hot-path reorder of the
  per-job flow (the downstream normalize/cache/dispatch is built around `pp_output`),
  so it wants its own focused change + a no-flag regression run, not a bolt-on.
- **System-header drift guard** (design §6.2): optionally include selected system
  headers in the bundle when worker/client toolchains differ.
