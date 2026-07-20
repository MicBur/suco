# 💻 VS Code & CMake Integration Guide für SUCO

Dieses Dokument beschreibt, wie du SUCO nahtlos in Visual Studio Code (VS Code) mit der **CMake Tools Extension** und gängigen C++ Language Servern (**C/C++ Extension** oder **clangd**) integrierst.

---

## 1. Funktionsweise der Integration

Die Einbindung von SUCO erfolgt über das CMake-Modul `cmake/SUCO.cmake`. Es führt beim Generieren und Bauen folgende Schritte vollautomatisch aus:

1. **Option `WITH_SUCO`:** Ermöglicht das flexible Ein- und Ausschalten von SUCO direkt über CMake (z. B. `cmake -DWITH_SUCO=OFF .`).
2. **Compiler-Launcher:** Registriert `suco-cl` und `suco-cl++` als Compiler-Launcher, sodass die verteilten Kompiliervorgänge transparent über das Grid laufen.
3. **IDE-Unterstützung (Automatische Bereinigung):** Wenn `CMAKE_EXPORT_COMPILE_COMMANDS` aktiv ist, generiert CMake eine `compile_commands.json`. Da darin standardmäßig die SUCO-Wrapper-Pfade als Präfixe enthalten wären, bereinigt ein automatisches CMake-Post-Build-Target (`suco_clean_compile_commands`) die Datei nach jedem Build:
   * **Entfernung des Wrappers:** Die Befehle werden so umgeschrieben, dass die echten Compiler-Pfade am Anfang stehen (z. B. `/usr/bin/g++` statt `suco-cl++ /usr/bin/g++`). Das garantiert eine **fehlerfreie IntelliSense-Funktionalität** und Navigation.
   * **Status-Attribut:** Jeder bereinigte Eintrag wird mit `"suco_used": true` und `"suco_build": true` markiert.

---

## 2. Empfohlene Workspace-Einstellungen (`.vscode/settings.json`)

Erstelle eine `.vscode/settings.json` in deinem Projektordner, um VS Code optimal für SUCO zu konfigurieren:

```json
{
  // 1. Zwingend Ninja als Generator verwenden (wichtig für compile_commands.json)
  "cmake.generator": "Ninja",

  // 2. Pfad zur compile_commands.json für die Microsoft C/C++ Engine
  "C_Cpp.default.compileCommands": "${workspaceFolder}/build/compile_commands.json",

  // 3. (Optional) Falls du die leistungsfähigere clangd Extension nutzt:
  "clangd.arguments": [
    "--compile-commands-dir=${workspaceFolder}/build",
    "--background-index",
    "--clang-tidy"
  ],

  // 4. Automatische Konfiguration beim Öffnen aktivieren
  "cmake.configureOnOpen": true
}
```

> [!NOTE]
> **clangd vs. Microsoft C++:** Bei Verwendung von `clangd` empfiehlt es sich, die IntelliSense-Engine der Microsoft C/C++ Extension zu deaktivieren (`"C_Cpp.intelliSenseEngine": "disabled"`), um Konflikte und doppelte Warnungen zu vermeiden.

---

## 3. Empfohlene Build-Tasks (`.vscode/tasks.json`)

Um Builds, Konfigurationen und Cache-Bereinigungen komfortabel über Tastatur-Shortcuts zu steuern, lege folgende `.vscode/tasks.json` an:

```json
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "SUCO: CMake Configure",
      "type": "shell",
      "command": "cmake -G Ninja -B ${workspaceFolder}/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DWITH_SUCO=ON .",
      "group": "build",
      "problemMatcher": [],
      "detail": "Konfiguriert das CMake-Projekt mit dem Ninja-Generator und SUCO-Unterstützung."
    },
    {
      "label": "SUCO: Grid Build (Ninja)",
      "type": "shell",
      "command": "suco ninja -C ${workspaceFolder}/build",
      "group": {
        "kind": "build",
        "isDefault": true
      },
      "dependsOn": "SUCO: CMake Configure",
      "problemMatcher": "$gcc",
      "detail": "Kompiliert das Projekt über das SUCO-Grid (mit Fallback auf lokale Kerne)."
    },
    {
      "label": "SUCO: Build ausschalten (Lokaler Build)",
      "type": "shell",
      "command": "cmake -B ${workspaceFolder}/build -DWITH_SUCO=OFF . && cmake --build ${workspaceFolder}/build",
      "group": "build",
      "problemMatcher": "$gcc",
      "detail": "Konfiguriert das Projekt ohne SUCO (WITH_SUCO=OFF) und baut rein lokal."
    },
    {
      "label": "SUCO: Cache leeren",
      "type": "shell",
      "command": "suco cache clear",
      "group": "none",
      "problemMatcher": [],
      "detail": "Leert den lokalen Preprocessor-Cache und den Grid-Header-Cache auf den Workern."
    }
  ]
}
```

---

## 4. Schritt-für-Schritt-Integration in ein bestehendes Projekt

1. **SUCO.cmake kopieren:**
   Kopiere die Datei `cmake/SUCO.cmake` aus dem SUCO-Repository in den `cmake/`-Ordner deines Projektes.
2. **CMakeLists.txt anpassen:**
   Füge direkt nach `project(...)` in deiner obersten `CMakeLists.txt` die Integration hinzu:
   ```cmake
   include(cmake/SUCO.cmake)
   ```
3. **VS Code Configs anlegen:**
   Erstelle den Ordner `.vscode` im Projekt-Root und kopiere die obige `settings.json` und `tasks.json` hinein.
4. **Bauen:**
   * Drücke `Strg + Umschalt + B` (bzw. `Cmd + Umschalt + B` unter macOS), um den Task **"SUCO: Grid Build (Ninja)"** zu starten.
   * IntelliSense und Sprung-zu-Definition (Go-to-Definition) funktionieren nach dem ersten Build sofort perfekt, da die `compile_commands.json` im Hintergrund bereinigt wird.
