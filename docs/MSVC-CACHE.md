# MSVC + SUCO: the shared cache still helps, even without distribution

**TL;DR** — MSVC (`cl.exe`) jobs **cannot** be cross-dispatched to the Linux grid (no Linux box
runs `cl.exe`). But SUCO's coordinator holds a **content-addressed L2 object cache** that is shared
across the whole team, and MSVC compiles populate and hit it like any other. So a warm or clean
rebuild of something a teammate already compiled — same source, same `cl.exe`, same flags — is
served from the cache **as a byte-identical object, with no `cl.exe` recompile.** This is the
ccache/sccache benefit, team-wide, with zero extra setup.

This is the value the VS Code extension's status bar surfaces as `cache NN%` — that number is
toolchain-agnostic and counts MSVC hits too.

---

## Why MSVC is the special case

SUCO's flagship path is *cross-dispatch*: a Windows dev preprocesses locally, a Linux worker
cross-compiles with `x86_64-w64-mingw32-g++`, and a `pe-x86-64` object comes back. That only works
for MinGW targets — **a Linux worker cannot run `cl.exe`**, so an MSVC job stays local.

The client detects this automatically. It keys on the `INCLUDE` env var, which `vcvars64.bat` sets
and which is exactly the condition under which `cl.exe` is usable:

```cpp
// src/client/compiler_command.cpp:146
std::string default_windows_compiler(bool is_cpp) {
    const char* inc = std::getenv("INCLUDE");
    if (inc && *inc) return "cl.exe";     // MSVC dev env active -> local compile, cache-only
    return is_cpp ? "g++" : "gcc";        // otherwise MinGW -> can cross-dispatch to Linux
}
```

So under an MSVC Developer Command Prompt, SUCO does **not** try to ship the job to Linux. It
compiles locally with `cl.exe` — and routes the result through the shared cache.

## How the cache still helps

The cache key is a **SHA-256 content hash of the preprocessed translation unit**, computed before
anything is shipped or stored. Two compiles that preprocess to the same bytes get the same key, on
any machine:

1. Client runs `cl.exe /E /nologo` to preprocess the TU locally.
2. Client hashes the preprocessed output → `content_hash` (the cache key).
3. Client asks the coordinator: *do you already have an object for this key?*
   - **Miss** → `cl.exe /c` compiles locally, the object is uploaded to the coordinator under
     `content_hash`, and stored in the L2 cache.
   - **Hit** → the coordinator returns the stored object. **`cl.exe /c` is never invoked.**
     ```
     src/client/network_client.cpp:287:  SUCO_LOG_INFO("Cache hit for {}", cmd.source_file);
     ```

Because the key is the *content* of the preprocessed source, the hit is shared across the team: if
a colleague already compiled that exact TU with that exact `cl.exe` and flags, your build reuses
their object.

## Reproduce it yourself

From an **x64 Native Tools Command Prompt for VS** (so `INCLUDE`/`cl.exe` are set), with a
coordinator running (default `192.168.0.200:9001`, or your own):

```bat
:: 1. A trivial TU
echo #include ^<vector^> > l2src.cpp
echo int f(){ std::vector^<int^> v; v.push_back(1); return (int)v.size(); } >> l2src.cpp

:: 2. First compile — expect a cache MISS: cl.exe runs, object is uploaded
suco-cl++ /c l2src.cpp /Fo:l2src.obj

:: 3. Clear ONLY the local L1 object cache so the next call must consult the coordinator
::    (leave the coordinator's L2 cache intact). Then compile again:
suco-cl++ /c l2src.cpp /Fo:l2src_2.obj

:: 4. Compare the two objects — they are byte-identical
fc /b l2src.obj l2src_2.obj
```

Set `SUCO_TIMING=1` to see the per-stage timing, and watch the client log line for
`Cache hit for l2src.cpp` on the second pass.

## What was verified (2026-07-26, WIN-DEV detached, 4 Linux workers)

Run on the real grid with `cl.exe` as the local compiler:

| Pass | Local L1 object cache | Coordinator (L2) | `cl.exe /c` invoked? | Result |
| :--- | :--- | :--- | :--- | :--- |
| **1** | empty | miss → store | **yes** | object uploaded under content hash `5ec64941…` |
| **2** | cleared | **hit** | **no** | `Cache hit for l2src.cpp` → object returned |

The Pass-2 object was **byte-identical** to Pass 1's — same content hash, no recompilation. That is
the concrete proof that MSVC benefits from the coordinator cache even though it never touches a
Linux worker.

## Caveats (byte-identity is strict — by design)

- A cache hit requires the preprocessed bytes to match **exactly**. Different `cl.exe` version,
  different `/std`, `/O`, `/D`, or include-order → different content hash → a legitimate miss. This
  is [invariant #1](../brain-claude.md) (byte-identity of cache keys): SUCO never serves an object
  that wasn't produced from the identical input.
- The win is on **repeated / shared** compiles (warm rebuilds, CI re-runs, a teammate already built
  it), not on a one-time cold compile of brand-new code — that still runs `cl.exe` locally.
- MSVC stays **local**; there is no cross-dispatch speedup, only the cache. For distributed
  compilation on Windows, target MinGW (the client defaults to it when no MSVC env is active).

## See it live

The [VS Code extension](../extension/vscode/) status bar (`⚡ SUCO: 4w · cache NN%`) reads the
coordinator's `/api/stats` — the `cache NN%` figure is the hit rate across *all* toolchains, MSVC
included. A rising number on your MSVC project is this mechanism working.
