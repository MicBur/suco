# brain-win.md — SUCO Windows Porting & Local Grid Walkthrough

> **Important**: This is a public file. Do not store passwords, secrets, tokens, or personal host addresses in it.

---

## 🎯 The Windows Goal

Enable developers on Windows machines to build, run, and compile projects using the SUCO distributed compiler grid locally. 

- **Target Toolchain**: Qt Creator / MinGW GCC toolchain (rather than MSVC), providing C++20 and standard CMake/Ninja builds natively on Windows.
- **Verification**: Run a local coordinator and worker node on Windows, and compile a C++ test file via the `suco-cl++` wrapper, demonstrating both a cold compile cache upload and a warm compile cache hit.

---

## ⚙️ Development Environment Setup

- **Compiler**: MinGW GCC 13.1.0 (located at `C:\Qt\Tools\mingw1310_64`).
- **Build Tools**: CMake 3.30.5 and Ninja 1.12.1 (located under `C:\Qt\Tools\CMake_64` and `C:\Qt\Tools\Ninja`).
- **Preinstalled Dependencies**: OpenSSL (1.1.1k) and SQLite3 (3.35.5) are included in the MinGW tools directory under `C:\Qt\Tools\mingw1310_64\opt`.
- **Manual Dependency**: Zstd (1.5.6) compiled from source.

---

## 📦 Building Zstd Static Library from Source

Since Zstd is not preinstalled in the MinGW environment, it is compiled manually in the `thirdparty` folder:

1. **Download and Extract**:
   ```powershell
   New-Item -ItemType Directory -Force -Path "thirdparty"
   Invoke-WebRequest -Uri "https://github.com/facebook/zstd/archive/refs/tags/v1.5.6.zip" -OutFile "thirdparty\zstd.zip"
   Expand-Archive -Path "thirdparty\zstd.zip" -DestinationPath "thirdparty" -Force
   ```
2. **Compile using CMake and Ninja**:
   ```powershell
   $env:PATH = "C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\mingw1310_64\bin;" + $env:PATH
   cd thirdparty/zstd-1.5.6/build/cmake
   cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_MULTITHREAD_SUPPORT=ON
   cmake --build build --config Release
   ```
   *Result*: Generates the static library `libzstd.a` under `thirdparty/zstd-1.5.6/build/cmake/build/lib/`.

---

## 🛠️ Code Modifications for Windows / MinGW Compatibility

The following files were modified to ensure compilation and link compatibility under Windows/MinGW:

### 1. [CMakeLists.txt](file:///c:/Users/micbu/Documents/suco/CMakeLists.txt)
Point Windows builds to the locally compiled Zstd static library if it exists in the workspace:
```diff
 # Find zstd for compression
 if(WIN32 AND NOT CMAKE_TOOLCHAIN_FILE)
     add_library(zstd::libzstd_static STATIC IMPORTED)
-    set_target_properties(zstd::libzstd_static PROPERTIES
-        IMPORTED_LOCATION "D:/zstd-build/lib/Release/zstd_static.lib"
-        INTERFACE_INCLUDE_DIRECTORIES "D:/zstd-src-dir/zstd-1.5.6/lib"
-    )
+    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd-1.5.6")
+        set_target_properties(zstd::libzstd_static PROPERTIES
+            IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd-1.5.6/build/cmake/build/lib/libzstd.a"
+            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd-1.5.6/lib"
+        )
+    else()
+        set_target_properties(zstd::libzstd_static PROPERTIES
+            IMPORTED_LOCATION "D:/zstd-build/lib/Release/zstd_static.lib"
+            INTERFACE_INCLUDE_DIRECTORIES "D:/zstd-src-dir/zstd-1.5.6/lib"
+        )
+    endif()
```

### 2. [msvc_detector.cpp](file:///c:/Users/micbu/Documents/suco/src/common/msvc_detector.cpp)
Include `<vector>` header missing under MinGW:
```diff
 #include "msvc_detector.h"
 #include <iostream>
 #include <cstdlib>
+#include <vector>
```

### 3. [tls_util.cpp](file:///c:/Users/micbu/Documents/suco/src/common/tls_util.cpp)
Add a backward-compatible key generator fallback. OpenSSL 1.1.1k does not have `EVP_RSA_gen`, so we use standard `EVP_PKEY_CTX` APIs:
```diff
 bool make_self_signed(SSL_CTX* ctx) {
-    EVP_PKEY* pkey = EVP_RSA_gen(2048);
+    EVP_PKEY* pkey = nullptr;
+#if OPENSSL_VERSION_NUMBER >= 0x30000000L
+    pkey = EVP_RSA_gen(2048);
+#else
+    EVP_PKEY_CTX* pkey_ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
+    if (pkey_ctx) {
+        if (EVP_PKEY_keygen_init(pkey_ctx) > 0) {
+            if (EVP_PKEY_CTX_set_rsa_keygen_bits(pkey_ctx, 2048) > 0) {
+                EVP_PKEY_keygen(pkey_ctx, &pkey);
+            }
+        }
+        EVP_PKEY_CTX_free(pkey_ctx);
+    }
+#endif
     if (!pkey) return false;
```

### 4. [socket_util.h](file:///c:/Users/micbu/Documents/suco/src/common/socket_util.h)
Map POSIX `SHUT_RDWR` to Windows `SD_BOTH` inside the Winsock2 block:
```diff
     constexpr socket_t INVALID_SOCKET_VAL = INVALID_SOCKET;
     inline int get_socket_error() { return WSAGetLastError(); }
     inline bool is_would_block(int err) { return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS; }
+    #ifndef SHUT_RDWR
+        #define SHUT_RDWR SD_BOTH
+    #endif
```

### 5. [worker.cpp](file:///c:/Users/micbu/Documents/suco/src/worker/worker.cpp)
- Include `<chrono>` for timestamping.
- Replaced POSIX `mkdtemp` with `std::filesystem::temp_directory_path` + unique ID.
- Replaced Unix process-status macros (`WIFEXITED`/`WEXITSTATUS`) with a direct check on the system status code.
- Replaced POSIX `::close` with cross-platform `close_socket` wrapper.
```diff
+#include <chrono>
...
@@ -141,6 +142,12 @@
     uint32_t num_in = ntohl(num_in_net);
     if (num_in > 100000) return;
 
+#ifdef _WIN32
+    std::error_code temp_ec;
+    std::filesystem::path workdir = std::filesystem::temp_directory_path(temp_ec) / ("suco_run_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
+    std::filesystem::create_directories(workdir, temp_ec);
+    if (temp_ec) return;
+#else
     char tmpl[] = "/tmp/suco_run_XXXXXX";
     char* dir = mkdtemp(tmpl);
     if (!dir) return;
     std::filesystem::path workdir(dir);
+#endif
...
@@ -178,7 +178,11 @@
         size_t n;
         while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) log.append(buf, n);
         int st = pclose(pipe);
+#ifdef _WIN32
+        exit_code = (st >= 0) ? st : -1;
+#else
         exit_code = (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
+#endif
     }
...
@@ -442,7 +442,7 @@
         // destructor's join() waited forever and the process only died via systemd's
         // 90s SIGKILL. shutdown() makes the pending accept() return immediately.
         ::shutdown(lsock, SHUT_RDWR);
-        ::close(lsock);
+        close_socket(lsock);
     }
```

### 6. [main.cpp](file:///c:/Users/micbu/Documents/suco/src/suco_wrapper/main.cpp)
- Include `<io.h>` for `_access` under Windows.
- Replace Unix process status macros in `run_local` and `run_one`.
```diff
 #ifdef _WIN32
     #include <windows.h>
     #include <process.h>
+    #include <io.h>
 #else
...
@@ -1911,7 +1911,11 @@
 
     static int run_local(const std::string& cmd_str) {
         int st = std::system(cmd_str.c_str());
+#ifdef _WIN32
+        return (st >= 0) ? st : 1;
+#else
         return (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : 1;
+#endif
     }
...
@@ -2365,7 +2365,11 @@
         char buf[4096]; size_t n;
         while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
         int st = pclose(p);
+#ifdef _WIN32
+        int ec = (st >= 0) ? st : 1;
+#else
         int ec = (st >= 0 && WIFEXITED(st)) ? WEXITSTATUS(st) : 1;
+#endif
         bool cached = out.find("cache hit") != std::string::npos;
```

### 7. [lru_cache.h](file:///c:/Users/micbu/Documents/suco/src/coordinator/lru_cache.h)
Replace MSVC-only `_dupenv_s` with standard `std::getenv` for `LOCALAPPDATA`:
```diff
         if (path.find("%LOCALAPPDATA%") != std::string::npos) {
-            char* local_app_data = nullptr;
-            size_t len = 0;
-            if (_dupenv_s(&local_app_data, &len, "LOCALAPPDATA") == 0 && local_app_data != nullptr) {
+            const char* local_app_data = std::getenv("LOCALAPPDATA");
+            if (local_app_data != nullptr) {
                 std::string expanded(local_app_data);
-                free(local_app_data);
                 std::string rest = path.substr(std::string("%LOCALAPPDATA%").size());
```

---

## 🚀 Building the Main SUCO Targets

Configure and compile the project using the following commands:

```powershell
$env:PATH = "C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\mingw1310_64\opt\bin;" + $env:PATH

# Configure with custom CMAKE_PREFIX_PATH pointing to OpenSSL/SQLite libs
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/Tools/mingw1310_64/opt"

# Build all targets
cmake --build build --config Release
```

Generates 6 executable targets in `build\`:
- `suco.exe` (main wrapper)
- `suco-cl.exe` (C wrapper)
- `suco-cl++.exe` (C++ wrapper)
- `suco-coordinator.exe` (coordinator)
- `suco-worker.exe` (worker)
- `suco-daemon.exe` (daemon)

---

## 🔍 Validation Log

The local grid run was verified on 2026-07-21.

> ⚠️ **This log predates the two dispatch fixes** (see Handoff Notes). The `header-set state stale`
> warning and the local fallback in run 1 below are the bug, not expected behaviour — at this point
> nothing was actually compiled on the worker. Kept as-is because it is the evidence the diagnosis
> was built from.

### 1. Starting the Coordinator
```
[2026-07-21 08:40:14] [INFO] Loading configuration...
[2026-07-21 08:40:14] [INFO] Initializing orchestrator...
[2026-07-21 08:40:14] [INFO] Orchestrator services starting...
suco-coordinator: Cache initialized at C:\Users\micbu\AppData\Local\suco\cache\ (Limit: 5120 MB)
[2026-07-21 08:40:14] [INFO] HistoryWriter initialized successfully with database: C:\Users\micbu\AppData\Local\suco\cache\\history.db
[2026-07-21 08:40:14] [INFO] NetworkServer starting on TCP Port 9000...
[2026-07-21 08:40:14] [INFO] Startup complete. Running on port 9000. Press Ctrl+C to terminate.
[2026-07-21 08:40:14] [INFO] UDP Auto-Discovery broadcast active on Port 9002
[2026-07-21 08:40:14] [INFO] Web dashboard REST API listening on Port 9001
[2026-07-21 08:40:14] [INFO] TCP server listening on Port 9000
```

### 2. Starting the Worker
```
[2026-07-21 08:40:32] [WARN] suco-worker: No active MSVC environment detected and auto-setup failed.
[2026-07-21 08:40:32] [INFO] Detecting available compilers and tools...
[2026-07-21 08:40:33] [INFO] Detected toolchains: g++=13.1.0, cmake=3.30.5, ninja=1.12.1
[2026-07-21 08:40:33] [INFO] Worker direct compilation listener started on port 9005
[2026-07-21 08:40:33] [INFO] Connecting to coordinator at 127.0.0.1:9000...
suco-worker: Connecting to coordinator at 127.0.0.1:9000...
[2026-07-21 08:40:33] [INFO] Worker: Direct compile listener loop started.
[2026-07-21 08:40:33] [INFO] Connected to coordinator.
[2026-07-21 08:40:33] [INFO] Registered successfully (Slots: 4, Name: WIN-DEV, OS: Windows)
```
*Coordinator registration log:*
```
[2026-07-21 08:40:33] [INFO] Worker registered: WIN-DEV (127.0.0.1, OS: Windows, Cores: 4, Toolchains: g++=13.1.0, cmake=3.30.5, ninja=1.12.1, Direct Port: 9005)
```

### 3. Grid Status Query (`.\build\suco.exe status`)
```
=================================================================
                     SUCO GRID STATUS                            
=================================================================
Coordinator:    127.0.0.1:9000 (Online)
Web-Dashboard:  http://127.0.0.1:9001/
Aktive Jobs:    0
Total Requests: 0
Cache-Hit-Rate: 0.0 % (Hits: 0, Misses: 0)
Grid-Auslastung: [....................] 0% (0 / 4 Slots)

Verbundene Worker im Grid: 1
-----------------------------------------------------------------
| Worker Name     | IP-Adresse      | OS         | Slots (Belegt) |
-----------------------------------------------------------------
| WIN-DEV         | 127.0.0.1       | Windows    | 0 / 4          |
-----------------------------------------------------------------
```

### 4. Running Verification Test Compilation

**First Run (Cold Compile & Cache Upload)**:
```powershell
$env:SUCO_REAL_CXX = "g++"
$env:SUCO_NO_DAEMON = "1"
.\build\suco-cl++.exe -c test_compile.cpp -o test_compile.o
```
*Output:*
```
[2026-07-21 08:42:08] [INFO] SUCO Client started
[2026-07-21 08:42:08] [INFO] Local compilation slots: 0 (of 24 cores)
[2026-07-21 08:42:08] [INFO] PipelineOrchestrator: Starting standard pipelining (level=medium, batch_size=16, timeout=5ms)
[2026-07-21 08:42:08] [INFO] PipelineOrchestrator: Optimizing for single job (synchronous execution, 0 worker threads)
[2026-07-21 08:42:08] [INFO] Preprocessing for test_compile.cpp finished in 347 ms
[2026-07-21 08:42:08] [INFO] Querying coordinator cache for test_compile.cpp
[2026-07-21 08:42:08] [INFO] Cache miss for test_compile.cpp
[2026-07-21 08:42:08] [INFO] Connecting directly to worker at 127.0.0.1:9005...
[2026-07-21 08:42:08] [INFO] Uploading compiled binary to coordinator cache for hash 7db801f42058658eef5b8c1b3ac7a9e9897d1bd1386660f609f816edd9512b4e
[2026-07-21 08:42:08] [INFO] Coordinator stored cache entry successfully.
[2026-07-21 08:42:08] [WARN] Grid header-set state stale for test_compile.cpp — recompiling locally
[2026-07-21 08:42:08] [INFO] Executing local fallback compilation for: test_compile.cpp in cwd: C:\Users\micbu\Documents\suco
[2026-07-21 08:42:10] [INFO] Local compilation succeeded
[2026-07-21 08:42:10] [INFO] Uploading local compile result to coordinator cache for hash 7db801f42058658eef5b8c1b3ac7a9e9897d1bd1386660f609f816edd9512b4e
[2026-07-21 08:42:10] [INFO] Coordinator stored cache entry successfully from local compile.
```

**Second Run (Warm Compile - SSD Cache Hit)**:
```powershell
.\build\suco-cl++.exe -c test_compile.cpp -o test_compile.o
```
*Output:*
```
[2026-07-21 08:42:14] [INFO] SUCO Client started
[2026-07-21 08:42:14] [INFO] Local compilation slots: 0 (of 24 cores)
[2026-07-21 08:42:14] [INFO] PipelineOrchestrator: Starting standard pipelining (level=medium, batch_size=16, timeout=5ms)
[2026-07-21 08:42:14] [INFO] PipelineOrchestrator: Optimizing for single job (synchronous execution, 0 worker threads)
[2026-07-21 08:42:15] [INFO] Preprocessing for test_compile.cpp finished in 344 ms
[2026-07-21 08:42:15] [INFO] Querying coordinator cache for test_compile.cpp
[2026-07-21 08:42:15] [INFO] Cache hit for test_compile.cpp
[2026-07-21 08:42:15] [INFO] Coordinator Cache HIT (direct) for test_compile.cpp
```

**Final Status Stats**:
```
Total Requests: 2
Cache-Hit-Rate: 50.0 % (Hits: 1, Misses: 1)
```

---

## 📋 Handoff Notes & Windows Specifics

- **Grid dispatch was broken on Windows by two independent bugs — both fixed, now verified.**
  The `Grid header-set state stale — recompiling locally` warning in the validation log above was
  not a stale-cache hiccup: Windows had *never* distributed a single TU. Both bugs are worth
  knowing about, because each one masqueraded as "it works, just slowly".

  1. **The header-set split only recognised `/usr/` paths** (`header_set_hasher.cpp`). MinGW
     headers live under `C:/Qt/Tools/...`, so no path ever matched, `header_paths` stayed empty and
     neither `header_set_source` nor `stripped_source` was filled in. The hash, however, is fed
     from flags/compiler/toolchain regardless, so it came back **non-empty** — and `job_sender`
     reads a non-empty hash as "this TU has a header set" and swaps the source for the (empty)
     stripped one. The worker received a header-set hash, no header text and an empty TU, and
     could only answer `HEADER_SET_MISSING` (-5). Fix: return `""` when no system headers were
     found, which switches the header-set machinery off for that TU and ships the full source.
     This was latent on Linux too — any TU without system headers hit the same path.
  2. **The worker had no shell on Windows** (`job_executor.cpp`). The compile command is built as
     a shell string (`cd <job dir> && <compiler> ...`); the POSIX branch runs it through `sh -c`,
     the Windows branch handed it straight to `CreateProcessA`, which took the first token
     literally and looked for an executable named `cd` — a cmd.exe builtin that does not exist as
     one. Every remote job died with `CreateProcess failed` → exit -1. Fix: prefix `cmd.exe /c`,
     mirroring `sh -c`, plus `cd /d` so a TEMP on another drive actually switches volume.

  Bug 2 was invisible until bug 1 was fixed — the -5 fired before the compiler was ever started.
  Verified 2026-07-21 on a one-worker local grid: `Direct dispatch OK`, worker `Exit: 0`, no local
  fallback, and `nm` on the resulting object shows the expected mangled symbol from the *edited*
  source (proving the object came from that compile and not from cache).

  **Why this hid for so long:** invariant #3 (a worker's bad state must never fail a build) did
  exactly its job — every failure was silently absorbed into a correct local compile. Correct
  builds, zero distribution, and nothing in the exit codes to notice. When judging Windows grid
  performance, `Cache hit` in the client log proves the **cache** path only; the grid path is
  proven by `Direct dispatch OK` plus a worker-side `Exit: 0`.
- **Open: header sets / PCH are still off on Windows.** The fix above makes Windows ship full
  sources, which distributes correctly but forfeits the header-set and PCH optimisation, because
  the `/usr/` test still never matches MinGW paths. Teaching the split about
  `C:/Qt/Tools/...`-style system headers is the remaining win. Treat it as a cache-key change:
  record golden `hs_hash` values plus resulting objects before and after (invariant #1). Note
  `content_hash` is computed in `job_sender` *before* `HeaderSetHasher::compute_hash` runs, so the
  object cache key itself cannot move — only the header-set key can.
- **Open: `resolve_bin_path` does not resolve a bare `g++` on Windows** — `toolchain_packer.cpp`
  logs `Could not resolve compiler path for: g++` and returns an empty `ToolchainInfo`, so
  toolchain packing is inert. Not fatal (dispatch works without it), likely a missing `.exe`
  suffix / PATH search.
- **Resolved (was: "client logs a store the coordinator never recorded"): the cache is safe, the
  log was lying.** The coordinator caches only `exit_code == 0` (`client_handler.cpp`,
  PACKET_CACHE_STORE), and the client does send the real exit code — so a failed compile was never
  cached. What the client got back was `PACKET_TOOLCHAIN_ACK`, which acknowledges the *packet*, not
  a store, and it reported "Coordinator stored cache entry successfully" for any ACK. A worker
  failing every job therefore produced a log that read like a healthy cache fill. Fixed: the client
  now distinguishes the two cases.
  **Do not "optimise" this by skipping the upload on failure** — the same PACKET_CACHE_STORE
  releases the worker slot reserved during the cache-miss query, so skipping it leaks one slot per
  failed job until the grid runs dry. Only the *store* is conditional, never the report.
  Verified 2026-07-21 with a TU that fails to compile: honest log line, exit 1 propagated to the
  caller, no object written, re-run is still a cache MISS, and grid utilisation back to 0/4 slots.
- **MSVC detection warning**: The client wrapper output `suco msvc_detector error: vswhere.exe nicht gefunden` can be ignored or silenced when working strictly in MinGW mode.
- **Git version control**: The Windows workspace is a real clone — `origin` points at
  `github.com/MicBur/suco`, branch `main` tracks `origin/main`. No ZIP round-trips needed.
  `thirdparty/` is gitignored (rebuild it via the zstd section above), as is `build/`.
  Note `core.autocrlf=true`: the working copy is CRLF, commits stay LF — the
  "LF will be replaced by CRLF" warnings on `git diff` are expected and harmless.
- **IPC Daemon Mode**: Windows does not natively support UNIX sockets used by `suco-daemon` on Linux. Although `suco-daemon.exe` compiles, running clients with `SUCO_NO_DAEMON=1` bypasses the daemon and connects directly to the coordinator, ensuring a reliable build execution pipeline.
