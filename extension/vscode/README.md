# SUCO Grid — VS Code extension

Surfaces the [SUCO](https://github.com/MicBur/suco) distributed C/C++ compiler grid inside VS Code.

## What it does

- **Status bar** — polls the coordinator and shows `⚡ SUCO: 4w · cache 87%` (workers online and cache-hit rate), turning amber when the coordinator is unreachable. Works whether you use the grid to *distribute* compiles (MinGW → Linux workers) or just for the **team-wide content-addressed cache** (which speeds up warm/clean MSVC rebuilds too — the compile stays local, the object is served from the coordinator).
- **Toggle grid for CMake** — injects `CMAKE_CXX_COMPILER_LAUNCHER` / `CMAKE_C_COMPILER_LAUNCHER = suco-cl++` into the workspace's CMake Tools settings (and removes it again). Re-run *CMake: Configure* to apply.
- **Open dashboard** — opens the coordinator's `:9001` web dashboard.

Click the status-bar item for the action menu, or run the `SUCO: …` commands from the Command Palette.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `suco.coordinatorHost` | `127.0.0.1` | Coordinator IP/host. Unset → `SUCO_COORDINATOR_HOST` from the environment, then localhost |
| `suco.coordinatorPort` | `9001` | Coordinator web/API port |
| `suco.launcher` | `suco-cl++` | Launcher injected into CMake |
| `suco.pollSeconds` | `5` | Status refresh interval (min 2) |

## Build / run (proof-of-concept)

```bash
cd extension/vscode
npm install
npm run compile          # tsc -> out/extension.js
```

Then press **F5** in VS Code (with this folder open) to launch an Extension Development Host, or package a `.vsix`:

```bash
npm install -g @vscode/vsce
vsce package
```

## Scope

This is a status/UX layer over mechanisms that already work without it (`CMAKE_CXX_COMPILER_LAUNCHER=suco-cl++`, the coordinator `/api/stats` and `/metrics` endpoints). No runtime dependencies — the coordinator is polled with Node's built-in `http`. A Visual Studio (VSIX) counterpart is a planned follow-up, most useful in CMake *Open Folder* mode.
