# SUCO — Project Roadmap & Vision

**Goal:** Build the world's best free, zero-configuration distributed C/C++ build system for LAN clusters — measurably outperforming Icecream and distcc on cold and warm builds, with zero-config Windows-to-Linux cross-compilation and native desktop management.

---

## ✅ v0.12.0 — Enterprise Telemetry & Multi-Node CI Grid (Released - July 2026)

- [x] **Prometheus & Grafana Exporter**: HTTP `/metrics` endpoint on the Coordinator (`:9001/metrics`) exposing real-time job counts, cache hit rates, active worker slots, and coordinator uptime.
- [x] **Multi-Node Distributed CI Verification Grid**: Dedicated GitHub Actions Workflow (`multi-runner-grid.yml`) testing heterogeneous Windows-to-Linux cross-compilation across distinct network nodes.
- [x] **Heterogeneous Cross-Compilation**: Windows clients (`suco-cl++.exe`) compile directly on Linux workers (`x86_64-w64-mingw32-g++`), returning binary `pe-x86-64` objects.
- [x] **Linux Service Deployment**: Production systemd service deployment on Linux master node `k3master` (`192.168.0.200`).
- [x] **Native Qt 6 Desktop Control Center (`suco-gui.exe`)**: Built with Qt 6 Widgets & QtNetwork, System Tray state integration, live cluster stats, and one-click `WIN-DEV` worker slot toggle.
- [x] **Windows Installer Setup**: Automated NSIS installer (`suco-0.12.0-windows-x64-setup.exe`) with machine-wide PATH configuration and desktop shortcuts.

---

## 🚀 v1.0.0 — Production Target: Remote Preprocessing & Sandboxing

Tracked under the [v1.0.0 milestone](../../../milestone/1). Some foundations already exist — the
remaining work per item is scoped in its issue.

- [ ] **Remote Preprocessing** (#42): Ship raw headers and source files to worker nodes for remote `-E` preprocessing, eliminating local client CPU overhead. *Design complete ([remote_preprocessing_design.md](remote_preprocessing_design.md)); reuses the header-set-hash machinery.*
- [ ] **Worker Execution Sandboxing** (#43): Isolate remote worker processes on shared networks via Linux namespaces / `bubblewrap`. *`unshare(1)` sandbox already covers `suco run`; this extends it to the compile path.*
- [ ] **TLS / mTLS Encryption** (#44): Mutual TLS authentication for zero-trust enterprise environments. *Transport **encryption already ships** (`SUCO_TLS=1`, self-signed + HMAC auth); remaining work is the cert-based mutual-auth layer.*
- [ ] **Dynamic Job Stealing** (#45): Adaptive work-stealing algorithm between workers for unbalanced CPU node clusters.
- [ ] **macOS ARM64 Support** (#46): Homebrew formula and native Apple Silicon client & worker binaries. *Blocked on a macOS build host.*

---

*Every performance claim on this roadmap ships with reproducible benchmark scripts (`scripts/run_large_windows_benchmark.ps1`) and empirical raw logs.*
