# 🚀 SUCO Grid Wiki

Welcome to the official **SUCO Grid Wiki** — the documentation hub for **SUCO** (SUper COmpiler Grid), the zero-configuration distributed C/C++ compilation and content-addressed caching system for local networks.

---

## 📚 Quick Navigation

- 🚀 **[Getting Started](Getting-Started)** — Install SUCO in 30 seconds and run your first distributed build.
- ⚡ **[Heterogeneous Cross-Compilation](Heterogeneous-Cross-Compilation)** — Compile Windows C++ projects on Linux server nodes.
- 💾 **[L2 Content-Addressed SSD Cache](L2-Content-Addressed-SSD-Cache)** — How SUCO achieves 19x speedups on warm rebuilds.
- 🎨 **[Qt 6 Control Center](Qt6-Control-Center)** — Native Windows Desktop GUI (`suco-gui.exe`) and System Tray guide.
- 🛠️ **[IDE & Build Tool Integrations](IDE-Integrations)** — VS Code Extension, Visual Studio 2022 VSIX, CMake & Ninja.
- 📊 **[Prometheus & Grafana Telemetry](Prometheus-&-Grafana-Telemetry)** — Monitoring metrics via `:9001/metrics`.
- ❓ **[Troubleshooting & FAQ](Troubleshooting-&-FAQ)** — Diagnostic switches, circuit breaker, and firewall rules.

---

## 🌟 What is SUCO Grid?

SUCO is a **high-performance, lightweight alternative** to expensive proprietary build systems like IncrediBuild or legacy tools like Icecream and distcc. 

```
  +-------------------------------------------------------------+
  |                   SUCO GRID ARCHITECTURE                    |
  +-------------------------------------------------------------+
  
       [ Windows PC / WIN-DEV ]           [ Linux Master Node ]
       suco-cl++.exe (Client)  ---(TCP)---> Coordinator (:9000)
                                                |
                                      +---------+---------+
                                      |                   |
                                (Direct Dispatch)   (Direct Dispatch)
                                      v                   v
                               [ Linux Worker 1 ]  [ Linux Worker 2 ]
                                (x86_64-mingw32)    (x86_64-mingw32)
```

### Key Differences vs Legacy Systems:

| Feature | 🍦 **Icecream / distcc** | 🚀 **SUCO Grid** |
| :--- | :--- | :--- |
| **Content-Addressed L2 SSD Cache** | ❌ None *(Recompiles unchanged code)* | ✅ **Built-in SHA-256 SSD Cache (19x speedup)** |
| **Heterogeneous Cross-Compiling** | ⚠️ Complex manual chroots | ✅ **Automatic Windows-to-Linux MinGW Cross-Builds** |
| **Direct Dispatch Data Path** | ❌ All traffic funnels through coordinator | ✅ **Clients stream data directly to Workers** |
| **Telemetry & Dashboard** | ⚠️ Legacy Qt application | ✅ **Built-in Web Dashboard & Prometheus `:9001/metrics`** |
| **Setup Overhead** | ⚠️ Complex configuration | ✅ **Zero-Config UDP Auto-Discovery (`sudo apt install suco`)** |

---

## 🏆 Benchmarks (101 Translation Units)

| Configuration | Time (Seconds) | Speedup vs Native | Throughput (TUs / sec) |
| :--- | :--- | :--- | :--- |
| **1. Native Local Build (`g++ -j 24`)** | **45.78 s** | **1.00x** (Baseline) | 2.2 TUs/s |
| **2. SUCO Remote Grid Only (13 Remote Linux Slots)** | **11.96 s** | 🚀 **3.83x** | 8.4 TUs/s |
| **3. SUCO Full Hybrid Grid (21 Slots: Local + Remote)** | **13.19 s** | 🚀 **3.47x** | 7.7 TUs/s |
| **4. SUCO Warm Rebuild (L2 SSD Cache Hits)** | **10.75 s** | ⚡ **4.26x** | 9.4 TUs/s |
