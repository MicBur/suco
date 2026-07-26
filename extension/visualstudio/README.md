# SUCO Grid Extension for Visual Studio 2022 (VSIX)

The **SUCO Grid Extension for Visual Studio 2022** integrates the SUCO C/C++ Compiler Grid directly into Microsoft Visual Studio.

## Features

- ⚡ **CMake Open Folder Launcher Injection**: Automatically injects `CMAKE_CXX_COMPILER_LAUNCHER=suco-cl++` into Visual Studio CMake projects.
- 📊 **Live Telemetry & Status Bar**: Displays real-time grid metrics (active workers, total slots, L2 SSD cache hit rate) from the SUCO coordinator (`http://<coordinator>:9001/api/stats`).
- 🛠️ **Visual Studio Options Page**: Configure Coordinator Host, Port, and Local Worker Slots under **Tools -> Options -> SUCO Grid**.

## Architecture

- **`SUCOGridPackage.cs`**: Main VSIX Extension AsyncPackage entry point.
- **`SUCOOptionPage.cs`**: Dialog page for user settings in VS Options.
- **`SUCOGrid.csproj`**: Visual Studio 2022 VSIX project file.
