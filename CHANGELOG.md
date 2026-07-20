# Changelog

All notable changes to the SUCO distributed compilation system will be documented in this file.

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
