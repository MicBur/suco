# Changelog

All notable changes to the SUCO distributed compilation system will be documented in this file.

## [0.11.0] - 2026-07-24

### Fixed
- **Real projects can be built on the grid again.** The header-set split produced two halves that cannot be reassembled into a well-formed translation unit, so any project with a realistic header set failed to compile remotely — and failed the build outright, because the worker returns exit 1, which the client treats as a genuine compile error rather than a worker fault, so invariant #3's local fallback never engaged. Proven by replaying the split outside SUCO: the original preprocessed output compiles clean, the reassembly yields 758 errors. Two independent defects — the `<built-in>`/`<command-line>` preamble is classified as non-header and lands *after* the system headers that need its macros (`'__SIZE_TYPE__' does not name a type`), and dropping the system-header line markers breaks linemarker nesting (`linemarker ignored due to incorrect nesting`), which misattributes the following content into structural errors. The split is now gated on `header_cache_enabled` — which had **never** actually gated it; the documented `SUCO_HEADER_CACHE_ENABLED` switch only influenced the worker's PCH choice, so operators had no way to turn the broken path off — and defaults to **off**. The client ships the full preprocessed source instead: a larger payload, but a correct build. Re-enable with `SUCO_HEADER_CACHE_ENABLED=1` once the split is repaired (#15). Trivial TUs (system headers only) took a different path and always compiled fine, which is why every smoke test passed for so long.
- **An unreachable coordinator no longer costs ~3 minutes per TU.** A single TU opens ~59 coordinator connections (cache query, header-set query, batch send, result upload, plus the backpressure re-query loop), each paying the full `connection_timeout_ms` (3000ms default). The build still produced a correct object via the local fallback, which is exactly why it went unnoticed. A process-wide circuit breaker now presumes the coordinator down after 2 consecutive connect failures and fails fast for 30s; any success resets it, so a coordinator that comes back is picked up again (this matters for the long-lived daemon). The backpressure loop also stops waiting out its 8s grid budget for a slot from a coordinator already known to be unreachable. Measured on Windows against an unreachable host: 181s and no object → 8s and a correct object.

### Added
- **The dashboard shows each job's target OS** — Windows cross-compile (⊞ Win) vs native Linux (🐧 Linux) — in both the active-jobs and history tables. The target is classified from the compiler the coordinator was asked for (a MinGW triple, `cl.exe` or `clang-cl` means Windows), captured at query time and carried to the store, because the direct-dispatch store packet has no command string.
- **A Windows installer** (`suco-<version>-windows-x64-setup.exe`) is published alongside the zip, built from the same staged payload so both ship identical binaries. Installs to Program Files, registers with Add/Remove Programs, and offers Start-menu shortcuts, a machine-wide PATH entry and `SUCO_NO_DAEMON=1` (required on Windows — the IPC daemon is Unix-socket based). PATH is written via PowerShell rather than `setx`, which truncates PATH at 1024 characters and would corrupt the machine environment.
- **A reproducible grid benchmark** (`scripts/bench_grid.sh`, `docs/BENCHMARK.md`) comparing native-Linux and Windows cross-compile throughput, with warm-up rounds, interleaved measurement and medians — run-to-run variance (~20%) exceeded the effect being measured, and single runs reported the two targets as both faster and slower than each other. Result: cross-compiling for Windows costs the grid nothing. Documents the real-project figure (SUCO building itself: 1.54x over local `-j8`) next to the synthetic ceiling (3.34x), and why they differ.
- **CI now compiles the installer script on every PR.** The release workflow only runs on a version tag, so the NSIS script would otherwise stay unverified until release day.

## [0.10.5] - 2026-07-22

### Performance
- **Disabled Nagle's algorithm (TCP_NODELAY) on all grid sockets** — the dominant cold-build network latency. Every small request/response (HELLO handshake, HMAC auth, cache query, dispatch headers) was paying ~40ms of Nagle + delayed-ACK per round-trip; a bare cache query cost ~128ms where a LAN round-trip should be ~1ms. Set on both connect and accept sides (client↔coordinator, client↔worker). Also skip `gethostbyname` (~26ms first-call/process) for IP-literal coordinator hosts via `inet_pton`. Verified on the real grid: query+sched 128ms→78ms client-side alone (~50ms/TU); the server-side change completes the round-trip. Helps every path, daemon or not. Pure latency change, no cache-key or output impact. Adds `SUCO_TIMING`-gated `[NET]`/`[NET-CONNECT]` profiling logs.

## [0.10.4] - 2026-07-22

### Fixed
- **Windows toolchain archiving** now works: it used to fail on every Windows batch compile because Git's GNU tar (first on PATH) shells `--zstd`/`-I zstd` out to a missing `zstd.exe` and reads the `C:\...` archive path as a remote rmt host. Windows now uses the System32 `bsdtar` (libarchive, zstd built in), which stores relative paths just like the Linux archive. POSIX is unchanged. Not needed for Windows→Linux dispatch (the Linux worker uses its own cross compiler), but removes the noisy error and enables toolchain shipping on pure-Windows grids.

## [0.10.3] - 2026-07-22

### Fixed
- **MSVC now builds the whole project natively** (first time the best-effort MSVC CI job is green). The code was written for MinGW, which provides POSIX names under `_WIN32`; MSVC does not. All fixes are `_MSC_VER`/`WIN32`-guarded so MinGW and Linux are byte-for-byte unaffected: `WIN32_LEAN_AND_MEAN`+`NOMINMAX` defined globally on Windows (fixes the winsock v1/v2 header clash — hundreds of `C2011`/`C2375`), a new `platform_compat.h` mapping `popen`/`pclose`/`getpid`/`ssize_t`/`<unistd.h>` to their MSVC equivalents, and a byte-identical rewrite of a greedy `\x1f` hex escape in the module-CMI cache-key token (MSVC `C7744`; the bytes on the grid are unchanged, so cache keys do not drift).

## [0.10.2] - 2026-07-22

### Fixed
- **Windows client could not reach a remote coordinator**: `connect_to_coordinator` set `SO_RCVTIMEO`/`SO_SNDTIMEO` from a `struct timeval`, but Winsock expects a `DWORD` of milliseconds — it read the timeval's `tv_sec` (~30) as a 30 ms timeout, so any recv crossing the LAN (including the coordinator's HELLO reply) aborted in ~30 ms and looked like a handshake disconnect. Loopback (sub-30 ms) always worked, which is why local smoke tests passed while the real grid was unreachable. Windows now passes a DWORD of milliseconds; POSIX keeps the timeval. With this fix a Windows client cross-dispatches a MinGW compile to a Linux node end to end (verified: pe-x86-64 object returned and linked into a working .exe).

## [0.10.1] - 2026-07-22

### Fixed
- **Debian/Ubuntu mingw cross compilers were not advertised**: the 0.10.0 worker probe dropped `x86_64-w64-mingw32-g++` on Debian/Ubuntu nodes because their mingw-w64 packages report a dotless version (`13-posix`) that `extract_version` rejects — so a node with `mingw-w64` installed still would not serve Windows clients. Falls back to `-dumpversion`'s leading integer; the scheduler matches major versions only. Found by the canary node during the 0.10.0 grid rollout.

## [0.10.0] - 2026-07-22

> Version note: releases follow the git-tag series (v0.9.x → v0.10.0). The 2.x headings
> below predate that decision and stay as history.

### Added
- **Native Windows/MinGW port**: all six binaries (`suco`, `suco-cl`, `suco-cl++`, `suco-coordinator`, `suco-worker`, `suco-daemon`) build and run the grid natively under MinGW-w64 GCC (Qt toolchain or MSYS2) — no WSL, no MSVC. Includes OpenSSL 1.1.1 keygen fallback, POSIX→Win32 replacements (`mkdtemp`, process status macros, socket shutdown), and a system-zstd CMake fallback for environments without a vendored build.
- **Windows/MinGW CI job**: blocking `windows-latest` (MSYS2) job that builds all targets and runs the same loopback-grid smoke test as Linux (dispatch, worker compile, cache store, cache hit).
- **Target-qualified dispatch**: Windows clients dispatch MinGW jobs as `x86_64-w64-mingw32-g++` instead of `g++`, so a Linux worker either cross-compiles correctly (with `mingw-w64` installed) or is skipped safely via the existing exit-127 infrastructure fallback — instead of returning an ELF object that fails at link time.
- **Header-set/PCH support on Windows**: the system-header split now recognizes MinGW toolchain paths (Qt `mingw1310_64`, MSYS2 `mingw64`) in addition to `/usr/`. Windows workers cache header text, build PCHs and serve stripped TUs (~KB instead of MB payloads). Purely additive predicate: existing Linux header-set keys are unchanged. Verified including a `__LINE__`/file-symbol provenance probe byte-identical to a native compile.
- **Scheduler-level matching for cross-OS jobs**: clients serialize a target-qualified dispatch id (`x86_64-w64-mingw32-g++` for MinGW targets) into cache queries and job requests, and workers probe/advertise the MinGW cross compilers in their toolchain map. The scheduler therefore only assigns Windows jobs to workers that actually have the driver — no wire-format change (the compiler field was always a free-form string), old workers are simply never selected and the client compiles locally.

### CI
- **Fixed the Windows MSVC job's configure step**: `vcpkg install` was missing `sqlite3`, which `find_package(SQLite3 REQUIRED)` has needed since the coordinator gained its SQLite HistoryWriter (2.1.0) — the job had been failing silently ever since (it is `continue-on-error`). Configure passes again; the job now surfaces the real remaining gap, MSVC compile errors in the source (tracked as a separate porting task; MinGW remains the supported Windows toolchain).

### Fixed
- **Header-set hash claimed a set that did not exist**: `HeaderSetHasher::compute_hash` returned a non-empty hash even when no system headers were found (the digest also covers flags/compiler/toolchain), making callers ship an empty stripped TU; the worker could only answer `HEADER_SET_MISSING`. On Windows this affected every TU (the header split only recognised `/usr/` paths) and silently disabled all distribution; on Linux it was reachable by any TU without system headers. The hash is now empty when the set is.
- **Worker had no shell on Windows**: compile commands (`cd <dir> && <compiler> …`) were passed to `CreateProcessA` verbatim, which looked for an executable named `cd`; every remote job failed with exit -1 and was absorbed by the local-fallback path. Commands now run through `cmd.exe /c` (mirroring `sh -c`), with `cd /d` for cross-drive temp directories.
- **Client reported a cache store that never happened**: after a failed remote compile the client still logged "Coordinator stored cache entry successfully" on any packet ACK, making a fully failing worker look like a healthy cache fill. The store report is now tied to a cacheable (exit 0) result; the packet itself is still always sent, because it doubles as the worker-slot release.
- **`resolve_bin_path` on Windows**: shelled out to nonexistent `which`, so bare compiler names (and drive-letter paths) never resolved and toolchain packing was inert. Now walks `PATH` directly (with `.exe` resolution); POSIX unchanged.

## [2.1.0] - 2026-07-07

### Added
- **IPC Daemon Mode (T12)**: Persistent background daemon (`suco-daemon`) with Unix-socket IPC, batch aggregation (~3 ms collection window), and pipelined preprocessing. Reduces per-process startup overhead and enables overlapped preprocessing with network transfers. Daemon j12-warm is 2% faster than standalone; j1-warm is 6% faster.
- **CI Smoke Test**: Self-contained loopback grid test (`tests/ci_smoke_test.sh`) — starts coordinator + worker on localhost, compiles 6 TUs, verifies cache hits on second pass. Runs in GitHub Actions on both Linux Release and Linux Debug (ASan/UBSan).
- **`__DATE__`/`__TIME__`/`__TIMESTAMP__` Cache Guard**: Translation units referencing time-dependent macros are compiled locally and bypass all caches, preventing stale timestamps from being served. Same approach as ccache.

### Changed
- **Daemon mode is now the default** (previously opt-in via `SUCO_DAEMON=1`). Opt-out via `SUCO_NO_DAEMON=1`. Standalone fallback on daemon failure is unchanged.
- **Cache Salt v3 → v4**: Invalidates all pre-guard cache entries to ensure correctness.
- **Prep Manifest V1 → V2**: Forces re-preprocessing so the time-macro guard applies to all cached prep results.
- **CI Pipeline overhaul**: 3-job matrix (Linux Release + Smoke, Linux Debug ASan/UBSan + Smoke, Windows MSVC). Smoke test uses worker-registration polling and per-compilation timeouts for reliability.

### Fixed
- **CI hang on GitHub Actions runners**: Smoke test cleanup used blocking `wait` + SIGTERM; coordinator's graceful shutdown never completed on 2-core runners. Fixed with SIGKILL + 5s wait timeout.

---

## [2.0.0] - 2026-07-05

### Added
- **Protocol Version Handshake (T5)**: Introduced a `PACKET_HELLO` exchange (Version `200`) during connection setup. Rejects outdated clients or coordinators gracefully to avoid corrupted network state, falling back automatically to local compilation.
- **Path Normalization (T3)**: Normalizes local checkout directories to a virtual `{SUCO_ROOT}` placeholder during SHA-256 caching. Added dynamic `-ffile-prefix-map` flag injection to ensure debug symbols remain identical across team checkouts, boosting shared cache hit rates.
- **GitHub Actions CI (T6)**: Set up a comprehensive `.github/workflows/ci.yml` runner checking builds for Linux (Release + Debug with ASan/UBSan) and Windows (MSVC).
- **Zstd Compression Tuning (T2)**: Exposed client options to adjust or disable compression levels (`SUCO_COMPRESSION_LEVEL`) and bypass grid compilation entirely via environment variables.
- **MIT License (T7)**: Added formal MIT License file and transitioned default documentation (`README.md`) to English (German version archived in `README.de.md`).
- **Remote Preprocessing Design (T4)**: Conceptualized and published the architectural roadmap for remote preprocessing under `docs/remote_preprocessing_design.md` to guide future high-bandwidth scaling.

### Changed
- **Cache Salt v3 (T2.1)**: Incremented the cache prefix salt to `"v3:"` in `hash_util.cpp`. This invalidates legacy cache entries cleanly, preventing potential deserialization crashes or binary corruption from older format versions.

### Fixed
- **Parallel Toolchain Race Conditions (T8)**: Added mutex protection blocks on both the coordinator upload handling (`client_handler.cpp`) and worker extraction routines (`worker.cpp`). This resolves a critical race condition where parallel compilers concurrently decompressed identical toolchain archives.
- **Zstd Decompress Size Safety (T2)**: Enforced a strict 1 GB buffer cap within `decompress_zstd` to protect worker memory footprints against malicious or corrupted zstd payload headers.
- **Path Normalization Performance (T3.1)**: Implemented thread-local static caching of the repository root lookup (`detect_checkout_root`) to avoid repetitive disk traversal per translation unit.
