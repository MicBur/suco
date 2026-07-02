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
  <a href="#"><img src="https://img.shields.io/badge/language-C%2B%2B20-9b51e0?style=for-the-badge" alt="C++20"></a>
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
- 🛡️ **Transparentes Grid-Failover** – Bricht ein Worker weg, delegiert der Coordinator die Jobs sofort neu
- ↩️ **Resilienter Client-Fallback** – Bei Coordinator-Ausfall kompiliert der Client in <100ms lokal weiter
- 🖥️ **Cross-Platform** – Native Unterstützung für Windows (MSVC) und Linux (GCC/Clang)

---

## 🏆 Benchmark-Ergebnisse

### Einzeldatei-Benchmark (~318.000 Zeilen preprocessed)

| Zustand | Dauer | Beschreibung | Gewinn |
|:---|:---|:---|:---|
| **Cache Miss** | 25,5s | Preprocessierung → Verteilung → Kompilierung → Rücktransfer | – |
| **Cache Hit** | 0,7s | Preprocessierung → SHA-256 Lookup → Sofortige Rückgabe aus SSD | **97,1%** 🚀 |

### Distributed Grid Benchmark (500 Klassen / 10 Slots)

> Getestet im Grid mit **3× HP EliteDesk Mini Nodes** (insgesamt 10 Slots)

| Durchlauf | Dauer | Beschreibung / Performance | Gewinn |
|:---|:---|:---|:---|
| **Run 1 – Native Build** | 7,61s | Lokaler nativer Compiler-Lauf (No SUCO) | – |
| **Run 2 – Cache Miss (Grid)** | 2,01s | **3.78x Beschleunigung** durch automatische Grid-Verteilung | **73.6%** 🚀 |
| **Run 3 – Cache Hit (Grid)** | 1,52s | **5.00x Beschleunigung (80% Zeitersparnis)** aus Coordinator-Cache | **80.0%** 🚀 |

<p align="center">
  <strong>5× schneller durch Grid-Caching. 500 Klassen in 1,5 Sekunden.</strong>
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

SUCO besteht aus mehreren aufeinander abgestimmten nativen C++20-Komponenten:

```
                  ┌────────────────────────────────────────────────────────┐
                  │                 ENTWICKLER-MASCHINE                    │
                  │  ┌───────────────┐                                     │
                  │  │     suco      │ (Einstiegspunkt / Build-Wrapper)    │
                  │  └───────┬───────┘                                     │
                  │          │ Setzt CC/CXX auf suco-cl/suco-cl++          │
                  │          ▼                                             │
                  │  ┌───────────────┐     ┌────────────────────────────┐  │
                  │  │ suco-cl/cl++  │────▶│      SUCO COORDINATOR      │  │
                  │  │(Compiler-Wrp) │◀───│  ┌──────────┐ ┌──────────┐  │  │
                  │  └───────────────┘     │  │SSD Cache │ │Dashboard │  │  │
                  │                        │  │(5GB LRU) │ │  :9001   │  │  │
                  │                        │  └──────────┘ └──────────┘  │  │
                  └────────────────────────┴───────┬────────────────────┘  │
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
| **`suco`** (Wrapper) | Der Haupteinstiegspunkt. Fängt Build-Befehle (`make`, `ninja`, `cmake`) ab, setzt die Umgebungsvariablen `CC`/`CXX` und delegiert an `suco-cl`/`suco-cl++`. |
| **`suco-cl` / `suco-cl++`** | Der eigentliche Compiler-Wrapper. Preprocessiert C/C++ Source lokal, berechnet SHA-256 Hashes und kommuniziert mit dem Coordinator. |
| **`suco-coordinator`** | Zentraler Hub im Netzwerk. Verwaltet den SSD-LRU-Cache, schedult Jobs (Least-Loaded) und hostet das Live Web-Dashboard. |
| **`suco-worker`** | Kompilierungs-Knoten im Grid. Registriert sich per UDP, kompiliert empfangene preprocessed Sources und sendet `.obj` zurück. |

### 🛠️ Subkommandos des Wrappers

Der neue `suco` Wrapper verfügt über ein professionelles OOP-Subcommand-System:

- **`suco setup`**: Startet den interaktiven Setup-Assistenten (konfiguriert Coordinator Host, Port, parallele Slots, Log-Level, sucht lokale Compiler und führt einen Verbindungstest durch).
- **`suco status`**: Zeigt den Zustand des Grid-Coordinators und der verbundenen Worker an.
- **`suco workers`**: Listet alle im Grid registrierten Worker-Knoten mit Auslastung auf.
- **`suco config show`**: Zeigt die aktuell wirksame Konfiguration und Pfade an.
- **`suco help`**: Zeigt die Hilfe für alle verfügbaren Kommandos.

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

### 📦 PCH-Support (Precompiled Headers)

SUCO verfügt über eine intelligente, hybride PCH-Verarbeitung für maximale Kompatibilität und Performance:

- **Zuverlässige PCH-Erkennung**: Erkennt alle gängigen PCH-Flags für MSVC (`/Yu`, `/Yc`, `/Y-`, `/Fp`) und GCC/Clang (`-include-pch`, `-fpch-preprocess`, `.gch`, `.pch`).
- **Hybride Verteilung (Phase 2)**:
  - **PCH-Erstellung** (z. B. Kompilierung von `stdafx.h` oder `header.hpp` zu `.gch`): Wird aus Sicherheitsgründen **lokal** ausgeführt, damit lokale Header-Abhängigkeiten nicht über das Netz kopiert werden müssen.
  - **PCH-Nutzung** (Kompilierung von `.cpp`-Dateien, die das PCH nutzen): Läuft **vollständig remote** im Grid. Der Client filtert beim Senden der Aufgabe alle lokalen PCH-Flags heraus und preprocessed die Quelldatei inklusive des Headers lokal, um absolute Portabilität auf den Grid-Workern sicherzustellen.

### ⚡ Zentrales, thread-sicheres Logging

Das gesamte System (Client, Coordinator, Worker) nutzt eine thread-sichere Logging-Infrastruktur:

- **Einstellbare Detailtiefe**: Steuerbar über die Umgebungsvariable `SUCO_LOG_LEVEL` (Werte: `DEBUG`, `INFO`, `WARN`, `ERROR` - Default: `INFO`).
- **Performance-optimierte Makros**: Bei deaktivierten Log-Leveln (z. B. `DEBUG` im Produktivbetrieb) wird der Formatierungsoverhead durch `std::format` durch vorgeschaltete Guards vollständig übersprungen.
- **Timestamping**: Alle Log-Ausgaben auf `std::cerr` erhalten strukturierte Zeitstempel: `[YYYY-MM-DD HH:MM:SS] [LEVEL] Message`.

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
suco g++ -O3 -std=c++20 -c myfile.cpp -o myfile.o

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
