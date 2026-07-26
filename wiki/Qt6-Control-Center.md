# 🎨 Native Windows Qt 6 Desktop Control Center (`suco-gui.exe`)

The **Qt 6 Desktop Control Center** (`suco-gui.exe`) provides a modern Windows system tray application for managing your local SUCO installation and grid worker attachment.

---

## ⚙️ Features

- 📌 **Windows System Tray Icon**: Minimizes to system tray with quick status tooltips.
- 🔴🟢 **WIN-DEV Worker Toggle**: Attach or detach your local Windows machine as an 8-slot worker with one click.
- 📊 **Real-time Grid Status**: Monitors active workers, coordinator latency, and local worker logs.
- 🛠️ **Automatic DLL Dependency Management**: Prepends application path and MinGW bin to process environment so worker startup never fails.

---

## 🚀 Running suco-gui

```cmd
build\suco-gui.exe
```

*Bundled with Qt 6.11.1 runtime DLLs (`Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Network.dll`, `Qt6Widgets.dll`).*
