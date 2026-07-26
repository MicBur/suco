# SUCO — Project Roadmap & Vision

**Goal:** Build the world's best free, zero-configuration distributed C/C++ build system for LAN clusters — measurably outperforming Icecream and distcc on cold and warm builds, with zero-config Windows-to-Linux cross-compilation and native desktop management.

---

## ✅ v0.11.0 — Heterogeneous Grid & Desktop Management (Released - July 2026)

- [x] **Heterogeneous Cross-Compilation**: Windows clients (`suco-cl++.exe`) compile directly on Linux workers (`x86_64-w64-mingw32-g++`), returning binary `pe-x86-64` objects.
- [x] **Direct Data Path & Socket Optimization**: Direct client-to-worker TCP payload transfers (`:9005`) with `TCP_NODELAY` latency elimination.
- [x] **Native Qt 6 Desktop Control Center (`suco-gui.exe`)**: Built with Qt 6 Widgets & QtNetwork, System Tray state integration, live cluster stats, and one-click `WIN-DEV` worker slot toggle.
- [x] **Windows Installer Setup**: Automated NSIS installer (`suco-0.11.0-windows-x64-setup.exe`) with machine-wide PATH configuration and desktop shortcuts.
- [x] **Circuit Breaker Fault Tolerance (#14)**: Process-wide failover detecting unreachable hosts within 7.96 seconds with seamless local CPU fallback.
- [x] **Content-Addressed L1/L2 Grid Cache**: Shared RocksDB & in-memory object caching delivering **19x speedups** on warm rebuilds.
- [x] **Large Windows Benchmark Suite (101 TUs)**: Empirically verified **4.21x cold speedup** (46.71s native -> 11.10s grid).
- [x] **Signed APT Package Repository**: Published via GitHub Pages (`sudo apt install suco`).

---

## 🔮 v0.12.0 — Security, Sandboxing & Enterprise Metrics (Next)

- [ ] **Worker Execution Sandboxing**: Integrate `bubblewrap` (bwrap) / Linux namespaces to isolate remote worker processes on shared networks.
- [ ] **Prometheus & Grafana Exporter**: Expose `/metrics` endpoint on the Coordinator (`:9000`) for monitoring cluster throughput, L2 hit rates, and per-node CPU/RAM utilization.
- [ ] **Distributed ThinLTO Support**: Accelerate the link phase by distributing ThinLTO object processing across worker nodes.
- [ ] **Native MSVC Toolchain Expansion**: Full parity for native MSVC (`cl.exe`) PCH creation and header-set caching under Windows.

---

## 🚀 v1.0.0 — Remote Preprocessing & Zero-Trust Grid

- [ ] **Remote Preprocessing**: Ship raw headers and source files to worker nodes for remote `-E` preprocessing, eliminating local client CPU overhead.
- [ ] **TLS / mTLS Encryption**: Mutual TLS authentication and encrypted network payloads for zero-trust enterprise environments.
- [ ] **macOS ARM64 Support**: Homebrew formula and native Apple Silicon client & worker binaries.
- [ ] **Dynamic Job Stealing**: Adaptive work-stealing algorithm between workers for unbalanced CPU node clusters.

---

*Every performance claim on this roadmap ships with reproducible benchmark scripts (`scripts/run_large_windows_benchmark.ps1`) and empirical raw logs.*
