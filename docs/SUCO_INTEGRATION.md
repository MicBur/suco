# SUCO Integration & Developer Experience Guide

Dieses Dokument beschreibt, wie man SUCO in C++-Projekte integriert, insbesondere mit CMake und Visual Studio Code.

---

## 1. MSVC-Umgebungserkennung (Windows)

Unter Windows führt SUCO (`suco` Wrapper und `suco-worker`) beim Start automatisch eine MSVC-Erkennung durch:
1. Es sucht über `vswhere.exe` nach der neuesten installierten Visual Studio-Instanz mit C++ Build Tools.
2. Es lädt das Umgebungsscript `vcvarsall.bat amd64` im Hintergrund und extrahiert alle Umgebungsvariablen (`PATH`, `INCLUDE`, `LIB` etc.).
3. Diese Variablen werden in den laufenden Prozess importiert.

> [!TIP]
> **Vorteil:** Du musst den Client oder Worker nicht mehr aus einer *Developer Command Prompt* heraus starten. Es funktioniert aus jeder normalen PowerShell oder CMD heraus!

---

## 2. CMake-Integration

Um SUCO als Compiler-Launcher in dein CMake-Projekt einzubinden:

1. Kopiere die Datei `cmake/SUCO.cmake` in dein Projektverzeichnis (z. B. unter `cmake/SUCO.cmake`).
2. Füge in der obersten `CMakeLists.txt` deines Projekts (direkt nach dem `project(...)` Befehl) Folgendes hinzu:
   ```cmake
   include(cmake/SUCO.cmake)
   ```
3. Führe den CMake-Konfigurationsschritt wie gewohnt aus. CMake findet automatisch `suco-cl` / `suco-cl++` und registriert sie als Compiler-Launcher (`CMAKE_C_COMPILER_LAUNCHER` / `CMAKE_CXX_COMPILER_LAUNCHER`).

---

## 3. compile_commands.json Markierung

Wenn du CMake mit `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` nutzt, wird im Build-Ordner eine `compile_commands.json` erzeugt.

Wenn du dein Projekt über den SUCO-Build-Wrapper kompilierst:
```bash
suco cmake --build build
# oder
suco ninja -C build
```
wird die `compile_commands.json` nach erfolgreichem Abschluss des Builds automatisch mit `"suco_build": true` markiert.

Dies dient dazu, im Nachhinein schnell und einfach verifizieren zu können, welche Dateien im Projekt über das verteilte Grid kompiliert wurden.

---

## 4. CLI Cache Clearing

Mit dem neuen Befehl:
```bash
suco cache clear
```
kannst du das gesamte Cache-System zurücksetzen:
1. **Lokal:** Die lokalen Client-Caches (`compiler_metadata_cache.txt` und `hash_cache.txt`) werden gelöscht.
2. **Coordinator:** Der Coordinator leert seinen Objekt-Cache.
3. **Worker:** Der Coordinator signalisiert allen Workern im Grid, ihren lokalen PCH-Header-Cache (`HeaderCache`) zu leeren.
