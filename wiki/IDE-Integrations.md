# 🛠️ IDE & Build Tool Integrations

SUCO Grid integrates seamlessly into modern IDEs and build systems.

---

## 🔹 VS Code Extension (`extension/vscode/`)

The **SUCO Grid VS Code extension** surfaces real-time grid metrics directly in the VS Code status bar:

- **Status Bar Display**: `⚡ SUCO: 4w · cache 75%`
- **CMake Launcher Toggle**: One-click action to inject `CMAKE_CXX_COMPILER_LAUNCHER=suco-cl++` into `.vscode/settings.json`.

---

## 🔹 Visual Studio 2022 VSIX Extension (`extension/visualstudio/`)

The **Visual Studio 2022 VSIX Extension** supports CMake Open Folder mode:

- **Options Page**: Tools -> Options -> SUCO Grid
- **Launcher Injection**: Automatically injects `suco-cl++` into MSVC CMake project settings.

---

## 🔹 CMake Integration (`SUCO.cmake`)

Include `SUCO.cmake` in your root `CMakeLists.txt`:

```cmake
include(SUCO.cmake)
```
Or specify the launcher on the command line:

```bash
cmake -B build -G Ninja -DCMAKE_CXX_COMPILER_LAUNCHER=suco-cl++
cmake --build build -j16
```
