<p align="center">
  <img src="assets/suco_banner.png" alt="SUCO Banner" width="100%">
</p>

<p align="center">
  <img src="assets/suco_logo.png" alt="SUCO Logo" width="120">
</p>

<h1 align="center">SUCO</h1>
<p align="center">
  <strong>SUper COmpiler Grid – Verteiltes C/C++ Kompilierungs- und Caching-System für lokale Netzwerke.</strong>
</p>

<p align="center">
  <a href="https://github.com/MicBur/suco/actions/workflows/ci.yml"><img src="https://github.com/MicBur/suco/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/MicBur/suco/releases"><img src="https://img.shields.io/badge/version-v1.7.0-00f2fe?style=for-the-badge&logo=github" alt="Version"></a>
  <a href="#"><img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-4facfe?style=for-the-badge" alt="Platform"></a>
  <a href="#"><img src="https://img.shields.io/badge/language-C%2B%2B20-9b51e0?style=for-the-badge" alt="C++20"></a>
  <a href="#"><img src="https://img.shields.io/badge/cache-SHA--256%20SSD-10b981?style=for-the-badge" alt="Cache"></a>
</p>

<p align="center">
  <em>Kompiliere einmal. Cache für immer. Verteilte Builds ohne Konfiguration.</em>
</p>

---

## ⚡ Auf einen Blick

SUCO ist eine **hochperformante, leichtgewichtige Alternative** zu teuren proprietären Lösungen wie IncrediBuild oder veralteten Systemen wie Icecream/distcc. Es wurde für **maximale Geschwindigkeit bei minimalem Setup** entwickelt:

- 🔍 **Zero-Config Auto-Discovery** – Worker finden den Coordinator automatisch per UDP Broadcast
- 💾 **Intelligenter SSD-Cache** – SHA-256-basierter LRU-Cache mit Metadaten-Tracking
- 📊 **Live Web-Dashboard** – Echtzeit-Monitoring aller Worker, CPU-Kerne und Jobs
- 🛡️ **Transparentes Grid-Failover** – Bricht ein Worker weg, delegiert der Coordinator die Jobs sofort neu
- ↩️ **Resilienter Client-Fallback** – Bei Coordinator-Ausfall kompiliert der Client in <100ms lokal weiter
- 🖥️ **Cross-Platform** – Native Unterstützung für Windows (MSVC) und Linux (GCC/Clang)
- 🪟 **MSVC-Umgebungserkennung** – Findet Visual Studio unter Windows und importiert die Build-Umgebung automatisch
- 🛠️ **CMake- & IDE-Integration** – Einfache Einbindung über `SUCO.cmake` und automatische `compile_commands.json` Markierung
- 🧼 **Grid-weites Cache-Clearing** – Bereinigung aller lokalen und remote Caches über `suco cache clear`

---

## 🏆 Benchmark-Ergebnisse

### Einzeldatei-Benchmark (~318.000 Zeilen preprocessed)

| Zustand | Dauer | Beschreibung | Gewinn |
|:---|:---|:---|:---|
| **Cache Miss** | 25,5s | Preprocessierung → Verteilung → Kompilierung → Rücktransfer | – |
| **Cache Hit** | 0,7s | Preprocessierung → SHA-256 Lookup → Sofortige Rückgabe aus SSD | **97,1%** 🚀 |

### Distributed Grid Benchmark (500 Klassen / 12 Slots)

> Getestet im Grid mit **3× HP EliteDesk Mini Nodes** (insgesamt 12 Slots) unter Verwendung eines **AMD Ryzen AI 9 365 Client** unter paralleler Last `-j12` (Warm-Runs):

| Konfiguration | Dauer (s) | Beschreibung / Hinweise | Speedup |
|:---|:---|:---|:---|
| **Native Build** | 5,2s | Lokaler nativer Compiler-Lauf | 1,00x |
| **distcc (3x Nodes)** | 5,1s | Verteilte Kompilierung via distcc | 1,02x |
| **Icecream (3x Nodes)** | 5,6s | Verteilte Kompilierung via Icecream | 0,93x |
| **SUCO Grid-only** | 10,7s | SUCO Grid-Kompilierung ohne lokale Worker-Slots | 0,49x |
| **SUCO Local Slots** | 8,3s | SUCO Grid-Kompilierung mit lokaler Slot-Zuweisung | 0,63x |

Eine detaillierte Aufschlüsselung über andere Parallelitätsgrade (`-j1` bis `-j12`) sowie Cold/Warm-Zeiten befindet sich im [Vergleichs-Benchmarkbericht](benchmarks/vs_icecream.md).

### Qt6 C++ GUI Grid-Benchmark (500 Klassen / 1000 Compiles)

> Getestet mit 500 generierten Qt6-Klassen im Grid mit **3× Worker Nodes** (parallelisiert mit `make -j16`).

| Durchlauf | Dauer | Beschreibung / Performance | Gewinn |
|:---|:---|:---|:---|
| **Run 1 – Native Build (No SUCO)** | 306,77s | Lokaler nativer Compiler-Lauf (Referenz) | – |
| **Run 2 – SUCO Cold Build** | 22,53s | Erstlauf (100% PCH Hits durch System-Header Hashing) | **13.6x Speedup** 🚀 |
| **Run 3 – SUCO Warm Build** | 258,11s | Zweitlauf (100% Object Hits; limitiert durch lokalen VM-Preprozessor) | **15.9%** 🚀 |

*Ehrliches Fazit:* Der PCH-Cache beschleunigt den Erstlauf dramatisch, da die schweren Qt6/System-Header nur einmalig auf dem Worker gebaut werden. Die Wiederverwendung bereits kompilierter Objektdateien (Warm Build) bringt im WSL-VM-Betrieb eine kleinere Ersparnis, da das serielle lokale Preprocessing der 1000 Dateien auf der VM CPU- und I/O-seitig zum Flaschenhals wird, während das Grid selbst in unter 50ms antwortet.

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
- **`suco status`**: Zeigt den Zustand des Grid-Coordinators, die Cache-Hit-Rate sowie die dynamische Auslastung an.
- **`suco workers`**: Listet alle im Grid registrierten Worker-Knoten gruppiert nach Compilern, Tools und Qt-Versionen auf.
- **`suco cache clear`**: Leert den lokalen Cache sowie den Header-PCH-Cache auf allen aktiven Workern im Grid.
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

| Feature | SUCO | IncrediBuild | Icecream | distcc |
|---|:---:|:---:|:---:|:---:|
| **Preis** | ✅ Kostenlos | ❌ ~$2000/Lizenz | ✅ Kostenlos | ✅ Kostenlos |
| **SSD Cache** | ✅ SHA-256 LRU | ✅ Proprietär | ❌ Nein | ❌ Nein |
| **Auto-Discovery** | ✅ UDP Broadcast | ✅ Agent-basiert | ✅ mDNS | ❌ Manuell |
| **Web Dashboard** | ✅ Glassmorphism | ✅ GUI App | ⚠️ Icemon | ❌ Nein |
| **Windows + Linux** | ✅ Native | ⚠️ Nur Windows | ✅ Ja | ✅ Ja |
| **Zero-Config Worker** | ✅ Ja | ❌ Agent nötig | ✅ Ja | ❌ Manuell |
| **Fallback bei Ausfall** | ✅ <100ms | ⚠️ Langsam | ❌ Nein | ❌ Nein |
| **Setup-Zeit** | ✅ ~2 Min | ❌ ~30 Min | ⚠️ ~10 Min | ⚠️ ~15 Min |

## 🗺️ Roadmap

Die geplanten Meilensteine, zukünftige Skalierungsziele und unser Konzept, um die Leistungslücke zu kommerziellen Systemen vollständig zu schließen, findest du in der [Projekt-Roadmap](docs/ROADMAP.md) sowie im Konzept für [Remote-Preprocessing](docs/remote_preprocessing_design.md).

---

## 📜 Lizenz

MIT License – Siehe [LICENSE](LICENSE).

---

<p align="center">
  <sub>Entwickelt mit ⚡ von <a href="https://github.com/MicBur">MicBur</a></sub>
</p>
