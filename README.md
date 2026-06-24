<p align="center">
  <img src="assets/suco_banner.png" alt="SUCO Banner" width="100%">
</p>

<p align="center">
  <img src="assets/suco_logo.png" alt="SUCO Logo" width="120">
</p>

<h1 align="center">SUCO Lite</h1>
<p align="center">
  <strong>SUper COmpiler Grid – Verteiltes C/C++ Kompilierungs- und Caching-System für lokale Netzwerke.</strong>
</p>

<p align="center">
  <a href="https://github.com/MicBur/suco/releases"><img src="https://img.shields.io/badge/version-v1.1.0-00f2fe?style=for-the-badge&logo=github" alt="Version"></a>
  <a href="#"><img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-4facfe?style=for-the-badge" alt="Platform"></a>
  <a href="#"><img src="https://img.shields.io/badge/language-C%2B%2B17-9b51e0?style=for-the-badge" alt="C++17"></a>
  <a href="#"><img src="https://img.shields.io/badge/cache-SHA--256%20SSD-10b981?style=for-the-badge" alt="Cache"></a>
</p>

<p align="center">
  <em>Kompiliere einmal. Cache für immer. Verteilte Builds ohne Konfiguration.</em>
</p>

---

## ⚡ Auf einen Blick

SUCO Lite ist eine **hochperformante, leichtgewichtige Alternative** zu teuren proprietären Lösungen wie IncrediBuild oder veralteten Systemen wie Icecream/distcc. Es wurde für **maximale Geschwindigkeit bei minimalem Setup** entwickelt:

- 🔍 **Zero-Config Auto-Discovery** – Worker finden den Coordinator automatisch per UDP Broadcast
- 💾 **Intelligenter SSD-Cache** – SHA-256-basierter LRU-Cache mit Metadaten-Tracking
- 📊 **Live Web-Dashboard** – Echtzeit-Monitoring aller Worker, CPU-Kerne und Jobs
- 🛡️ **Resilienter Fallback** – Bei Coordinator-Ausfall kompiliert der Client in <100ms lokal weiter
- 🖥️ **Cross-Platform** – Native Unterstützung für Windows (MSVC) und Linux (GCC/Clang)

---

## 🏆 Benchmark-Ergebnisse

### Einzeldatei-Benchmark (~318.000 Zeilen preprocessed)

| Zustand | Dauer | Beschreibung | Gewinn |
|:---|:---|:---|:---|
| **Cache Miss** | 25,5s | Preprocessierung → Verteilung → Kompilierung → Rücktransfer | – |
| **Cache Hit** | 0,7s | Preprocessierung → SHA-256 Lookup → Sofortige Rückgabe aus SSD | **97,1%** 🚀 |

### Distributed Grid Benchmark (30 Dateien × 800 Funktionen)

> Getestet auf einem Grid mit **3× HP EliteDesk 800 G2 Mini** (je 4 Kerne, 12 Slots gesamt)

| Durchlauf | Dauer | Beschreibung | Gewinn |
|:---|:---|:---|:---|
| **Run 1 – Cache Miss** | 55,5s | 30 Dateien parallel auf 12 Slots verteilt | – |
| **Run 2 – Cache Hit** | 1,3s | Alle 30 Dateien sofort aus dem SSD-Cache | **97,6%** 🚀 |

<p align="center">
  <strong>42× schneller beim zweiten Build. 30 Dateien in 1,3 Sekunden.</strong>
</p>

### Warum ist der Cache so schnell?

```
Normaler Build:          SUCO Cache Hit:
┌──────────────┐         ┌──────────────┐
│ Preprocessor │ ~0.1s   │ Preprocessor │ ~0.1s
├──────────────┤         ├──────────────┤
│ Parser/AST   │ ~3.0s   │ SHA-256 Hash │ ~0.001s
├──────────────┤         ├──────────────┤
│ Optimierung  │ ~15.0s  │ Cache Lookup │ ~0.002s
├──────────────┤         ├──────────────┤
│ Codegen      │ ~7.0s   │ SSD → Client │ ~0.5s
└──────────────┘         └──────────────┘
     ~25s                    ~0.7s
```

Der Client führt **nur die Preprocessierung** (Phase 1) lokal aus und berechnet einen SHA-256-Hash. Bei einem Cache Hit werden die CPU-intensiven Phasen (Parsing, Optimierung, Codegen) **komplett übersprungen** — die fertige `.obj`-Datei kommt direkt vom SSD-Cache.

---

## 🏗️ Architektur

<p align="center">
  <img src="assets/architecture.png" alt="SUCO Architektur" width="80%">
</p>

SUCO besteht aus drei nativen C++17-Komponenten:

```
┌─────────────────────────────────────────────────────────────────┐
│                     ENTWICKLER-MASCHINE                        │
│  ┌──────────┐    ┌──────────────────────────────────────────┐  │
│  │  suco    │───▶│           SUCO COORDINATOR               │  │
│  │ (Client) │◀───│  ┌────────────┐  ┌───────────────────┐   │  │
│  └──────────┘    │  │ SSD Cache  │  │  Web Dashboard    │   │  │
│   g++ wrapper    │  │ (LRU 5GB)  │  │  :9001            │   │  │
│                  │  └────────────┘  └───────────────────┘   │  │
│                  └──────────┬───────────────────────────────┘  │
└─────────────────────────────┼─────────────────────────────────┘
                              │ TCP :9000
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
     ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
     │  WORKER #1   │ │  WORKER #2   │ │  WORKER #3   │
     │  HP Mini G2  │ │  HP Mini G2  │ │  HP Mini G2  │
     │  4 Kerne     │ │  4 Kerne     │ │  4 Kerne     │
     └──────────────┘ └──────────────┘ └──────────────┘
              UDP Auto-Discovery :9002
```

### Komponenten

| Komponente | Beschreibung |
|---|---|
| **`suco`** (Client) | Compiler-Wrapper. Preprocessiert lokal, berechnet SHA-256, fragt Coordinator. Bei Ausfall: automatischer lokaler Fallback in <100ms. |
| **`suco-coordinator`** | Zentraler Hub. Verwaltet SSD-LRU-Cache, verteilt Jobs per Least-Loaded-Scheduling, hostet Web-Dashboard. |
| **`suco-worker`** | Kompilierungs-Knoten. Registriert sich per UDP, empfängt preprocessed Source, kompiliert und sendet `.obj` zurück. |

### Cache-Architektur (Phase 1)

Der Cache verwendet einen **versionierten, metadatenreichen Hash-Key**:

```
v1:⟨0x1F⟩Target⟨0x1F⟩CompilerVersion⟨0x1F⟩Standard⟨0x1F⟩Defines⟨0x1F⟩Includes⟨0x1F⟩Flags⟨0x1F⟩NormalizedSource
```

- **Versionierung**: `v1:` Prefix für zukunftssichere Schema-Migration
- **Metadaten**: Target-Architektur, Compiler-Version, C++-Standard, sortierte Defines & Include-Pfade
- **Source-Normalisierung**: Entfernt `#line`-Direktiven, Whitespace-only-Zeilen, normalisiert Pfade — hochoptimiert mit `memchr` für 300k+ Zeilen in <5ms
- **Separator**: ASCII `0x1F` (Unit Separator) — kann in keinem normalen Dateiinhalt vorkommen

Jeder Cache-Eintrag speichert zusätzlich eine `.meta`-JSON-Datei mit allen Build-Metadaten.

---

## 💻 Installation

### Abhängigkeiten

| Platform | Pakete |
|---|---|
| **Windows** | Visual Studio (MSVC), CMake ≥ 3.15, OpenSSL |
| **Linux** | `build-essential`, `cmake`, `libssl-dev` |

### Bauen

```bash
# Linux / WSL
cmake -B build_linux -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build_linux

# Windows (Developer PowerShell)
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Installer (Linux)

```bash
sudo bash install.sh
# Wähle: 1) Coordinator + Worker   2) Nur Worker
```

Der Installer kopiert die Binaries nach `/usr/local/bin/`, richtet systemd-Services ein und öffnet die Firewall-Ports.

---

## ⚙️ Verwendung

### 1. Coordinator starten

```bash
./suco-coordinator
# Dashboard: http://localhost:9001
# Cache: ~/.cache/suco/ (Standard 5 GB LRU)
```

### 2. Worker starten (auf allen Helfer-Rechnern)

```bash
# Auto-Discovery (findet Coordinator automatisch per UDP)
./suco-worker --slots 4

# Oder mit expliziter Adresse
./suco-worker --coordinator 192.168.0.200:9000 --slots 8
```

### 3. Kompilieren über SUCO

```bash
# Linux (GCC)
suco g++ -O3 -std=c++17 -c myfile.cpp -o myfile.o

# Windows (MSVC)
suco cl.exe /O2 /EHsc /c myfile.cpp /Fo myfile.obj

# In CMake
set(CMAKE_CXX_COMPILER_LAUNCHER suco)
```

### Umgebungsvariablen

| Variable | Default | Beschreibung |
|---|---|---|
| `SUCO_COORDINATOR_HOST` | Auto-Discovery | IP/Hostname des Coordinators |
| `SUCO_COORDINATOR_PORT` | `9000` | TCP-Port des Coordinators |
| `SUCO_CACHE_DIR` | `~/.cache/suco/` | Pfad zum SSD-Cache |
| `SUCO_MAX_CACHE_MB` | `5120` | Maximale Cache-Größe in MB |

---

## 📊 Live Web-Dashboard

<p align="center">
  <img src="assets/dashboard_preview.png" alt="SUCO Dashboard" width="100%">
</p>

Das integrierte Dashboard auf **Port 9001** zeigt in Echtzeit:

- 🟢 **Worker-Status** mit individueller CPU-Kernauslastung pro Maschine
- 📈 **Cache Hit Rate** als animierter Progress-Ring
- ⚡ **Aktive Kompilierungen** mit Live-Timer
- 📋 **Job-Verlauf** mit Hit/Miss-Badges

---

## 🔧 Netzwerk-Ports

| Port | Protokoll | Zweck |
|---|---|---|
| `9000` | TCP | Compile-Requests (Client ↔ Coordinator ↔ Worker) |
| `9001` | TCP | Web-Dashboard & REST API |
| `9002` | UDP | Auto-Discovery Broadcast |

---

## 📁 Projektstruktur

```
suco/
├── src/
│   ├── client/          # suco (Compiler-Wrapper)
│   │   └── main.cpp
│   ├── coordinator/     # suco-coordinator (Cache + Scheduling)
│   │   ├── main.cpp
│   │   └── lru_cache.h
│   ├── worker/          # suco-worker (Kompilierungs-Knoten)
│   │   └── main.cpp
│   ├── helper/          # suco-helper (Standalone Daemon)
│   │   ├── main.cpp
│   │   └── web_server.h
│   └── common/          # Shared Code
│       ├── protocol.h
│       ├── socket_util.h
│       ├── hash_util.h
│       └── hash_util.cpp
├── dashboard.html       # Web-Dashboard UI
├── install.sh           # Linux Installer
├── install.ps1          # Windows Installer
├── CMakeLists.txt
└── assets/              # README-Bilder
```

---

## 🤝 Vergleich mit Alternativen

| Feature | SUCO Lite | IncrediBuild | Icecream | distcc |
|---|:---:|:---:|:---:|:---:|
| **Preis** | ✅ Kostenlos | ❌ ~$2000/Lizenz | ✅ Kostenlos | ✅ Kostenlos |
| **SSD Cache** | ✅ SHA-256 LRU | ✅ Proprietär | ❌ Nein | ❌ Nein |
| **Auto-Discovery** | ✅ UDP Broadcast | ✅ Agent-basiert | ✅ mDNS | ❌ Manuell |
| **Web Dashboard** | ✅ Glassmorphism | ✅ GUI App | ⚠️ Icemon | ❌ Nein |
| **Windows + Linux** | ✅ Native | ⚠️ Nur Windows | ✅ Ja | ✅ Ja |
| **Zero-Config Worker** | ✅ Ja | ❌ Agent nötig | ✅ Ja | ❌ Manuell |
| **Fallback bei Ausfall** | ✅ <100ms | ⚠️ Langsam | ❌ Nein | ❌ Nein |
| **Setup-Zeit** | ✅ ~2 Min | ❌ ~30 Min | ⚠️ ~10 Min | ⚠️ ~15 Min |

---

## 📜 Lizenz

MIT License – Siehe [LICENSE](LICENSE).

---

<p align="center">
  <sub>Entwickelt mit ⚡ von <a href="https://github.com/MicBur">MicBur</a></sub>
</p>
