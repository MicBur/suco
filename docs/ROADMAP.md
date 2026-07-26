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

- [ ] **Worker Execution Sandboxing**: Integrate `bubblewrap` (bwrap) / Linux namespaces to isolate remote worker processes on shared networks.
- [ ] **Remote Preprocessing**: Ship raw headers and source files to worker nodes for remote `-E` preprocessing, eliminating local client CPU overhead.
- [ ] **TLS / mTLS Encryption**: Mutual TLS authentication and encrypted network payloads for zero-trust enterprise environments.
- [ ] **macOS ARM64 Support**: Homebrew formula and native Apple Silicon client & worker binaries.
- [ ] **Dynamic Job Stealing**: Adaptive work-stealing algorithm between workers for unbalanced CPU node clusters.

---

*Every performance claim on this roadmap ships with reproducible benchmark scripts (`scripts/run_large_windows_benchmark.ps1`) and empirical raw logs.*
