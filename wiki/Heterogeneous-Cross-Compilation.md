# ⚡ Heterogeneous Cross-Compilation

SUCO Grid allows **Windows developers** to offload C/C++ compilation directly to **Linux server nodes** without configuring complex cross-chroots manually.

---

## 🔬 How it Works

1. **Client Request**: On Windows, the client runs `suco-cl++.exe` (or MSVC wrapper `suco-cl.exe`).
2. **Preprocessing**: The client preprocesses the source file locally using `g++ -E` or `cl.exe /P` and generates a SHA-256 content hash.
3. **Target Matching**: The coordinator inspects the job's requested toolchain (`x86_64-w64-mingw32-g++`).
4. **Direct Dispatch**: The Windows client connects directly to the assigned Linux worker over TCP.
5. **Cross-Compilation**: The Linux worker executes its MinGW cross-compiler (`x86_64-w64-mingw32-g++ -c -o`).
6. **Binary Object Return**: A native Windows `PE-x86-64` binary object file (`.o` / `.obj`) is returned directly to the Windows client.

---

## 🛠️ Linux Worker Setup

To enable cross-compilation on a Linux worker, install the MinGW cross-compiler packages:

```bash
sudo apt update && sudo apt install -y g++-mingw-w64-x86-64
```

When `suco-worker` starts, it automatically detects `x86_64-w64-mingw32-g++` and advertises it to the coordinator.

---

## ⚙️ Environment Variables

- `SUCO_COORDINATOR_HOST`: Set the IP of the Linux Coordinator (e.g. `192.168.0.200`).
- `SUCO_LOCAL_SLOTS`: Set to `0` to force 100% of jobs to be sent to the remote Linux grid.
- `SUCO_IGNORE_VERSION`: Set to `1` to allow cross-matching across minor GCC/MinGW patch versions.
