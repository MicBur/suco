# Changelog

All notable changes to the SUCO distributed compilation system will be documented in this file.

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
