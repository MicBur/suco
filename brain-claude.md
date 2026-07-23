# brain-claude.md — SUCO project context & cross-machine handoff

> Shared context so any agent/machine (Linux here, Windows via `brain-win.md`) picks up the
> same understanding. **This file is in a PUBLIC repo — never put passwords, `SUCO_SECRET`
> values, tokens, or exploitable host details in it.** Credentials live only in private notes.

Last updated: 2026-07-21.

---

## The goal

SUCO is a distributed C/C++ compiler for a LAN. Target: **match IncrediBuild / Icecream on
speed, but be installable in 30 seconds via `apt`.**

**Status: achieved.** On RocksDB (342 TUs, 4-node grid, idle machine, same-session head-to-head):
- **Cold build: on par with Icecream** (SUCO 100.7s vs icecc 101.9s — parity, within noise).
- **Warm rebuild: ~4.1× faster** (24.7s vs 101.9s) — Icecream has no cache; SUCO serves unchanged
  objects from a content-addressed cache.
- Published publicly: `sudo apt install suco` from https://micbur.github.io/suco.

## Cold-build cost model (where the overhead actually is — 2026-07-22)

Cold builds are SUCO's weak spot (parity with Icecream at best; slower on small/medium projects).
Measured the per-TU CLIENT overhead with `SUCO_TIMING=1` on a 3.3 MB preprocessed TU:
`pp≈496ms` (the compiler's `-E` — unavoidable, Icecream pays it too), then the SUCO-specific tax:
`hset-split≈90ms` (building the two split strings line-by-line) + `prep-store≈52ms` (writing the
warm-cache seed to disk) + `key+hash≈35ms` (SHA-256 for content-addressing) + `norm≈8ms` ≈
**185 ms/TU, +37% on top of preprocessing.**

**The strategic finding: that client tax is NOT the dominant cold cost.** Against the GoogleTest
benchmark (108 files, -j17, cold overhead 33.8 s) the client tax accounts for only ~1.2 s (~4%).
The rest is the network/dispatch path. Profiled it on the real grid with new `[NET]`/`[NET-CONNECT]`
timing (SUCO_TIMING, in pipeline_orchestrator + network_client): per TU, `query+sched≈128ms` and
`dispatch(ship+compile+recv)≈1080ms`. The dispatch is mostly the real remote compile (parallelises
across workers). The `query+sched≈128ms` was the smell — a LAN round-trip should be ~1ms.

**Root cause found (2026-07-22): `TCP_NODELAY` was never set on ANY socket.** Every small
request/response (HELLO, HMAC auth, cache query, dispatch headers) ate ~40ms of Nagle+delayed-ACK
latency per round-trip. Plus `gethostbyname` on the coordinator IP cost ~26ms first-call per
process. Both are per-TU on the no-daemon path (Windows always; Linux if daemon off) because each
compiler invocation is a fresh process with an empty connection pool. **Fix:** a `set_tcp_nodelay`
helper applied on both connect and accept sides (client↔coordinator, client↔worker), and
`inet_pton` before `gethostbyname`. **Both-ends result, verified on the real grid after deploying v0.10.5 to all 4 nodes** (same TU
type, before = 0.10.1 nodes + old client): `query+sched` 128ms→~22ms, and — the surprise —
`dispatch(ship+compile+recv)` ~760ms→~330ms, because the dispatch protocol's small header
round-trips between bulk transfers were ALSO Nagle-delayed (~430ms of delayed-ACK removed).
**Net per TU ~838ms→~352ms — over half gone**, from one missing `setsockopt`. This is the
cold-build lever. Pure latency change, zero byte-identity risk. (Also taken: `std::move` the split
strings — memory traffic only.)

---

## Current state (2026-07-21)

- Public repo: **github.com/MicBur/suco**, all nodes on **0.9.2**, grid healthy (4 workers / 13 slots).
- APT repo built + signed by GitHub Actions on every `v*` tag → GitHub Pages. CI is green.
- Docs: `docs/INSTALL.md` (full setup), `docs/INSTALL-apt.md` (apt + maintainer).
- **Branch `windows-mingw` is pushed and CI-green (2026-07-21)** — including the full Linux battery
  (smoke, cache-correctness, chaos, ASan/UBSan) over the shared-code fixes (`header_set_hasher.cpp`,
  see invariant #8), and a NEW blocking Windows/MinGW CI job (MSYS2) that runs the same smoke test:
  loopback grid, dispatch via cmd.exe, cache hit. PR onto main is the next step; the CI push
  trigger temporarily includes `windows-mingw` — drop after merge. Until merged, `origin/main`
  does not build on Windows and still carries the header-set bug.
- **The Windows MSVC CI job is GREEN (2026-07-22, v0.10.3).** History: its configure was broken
  since 2.1.0 (vcpkg missing `sqlite3`), fixed first; then it failed at build with real MSVC
  errors, fixed via `fix/msvc-build` (PR #4). Root theme: the code was written for MinGW, which
  provides POSIX names under `_WIN32`; MSVC does not. Fixes (all `_MSC_VER`/`WIN32`-guarded, MinGW
  and Linux byte-identical): global `WIN32_LEAN_AND_MEAN`+`NOMINMAX` (winsock v1/v2 header clash),
  a `platform_compat.h` shim (`popen`/`pclose`/`getpid`/`ssize_t`/`<unistd.h>`), and a
  byte-identical rewrite of a greedy `\x1f` escape (module-CMI cache key — `0xFC 'M' 'I' 0x1F`
  stays, so no drift). MinGW is still the *recommended* Windows toolchain (full grid smoke); the
  MSVC job is build-only. **Reproduce MSVC locally:** vcpkg (`openssl zstd sqlite3 --triplet
  x64-windows`) + `cmake -G "Visual Studio 17 2022" -A x64 --toolchain vcpkg.cmake`. Build Tools
  live under `C:\Program Files (x86)\...\2022\BuildTools`.

---

## Architecture (short)

- **Coordinator** (one per grid): job assignment, team-wide L2 content-addressed cache, dashboard
  (`:9001`), UDP auto-discovery (`:9002`), control (`:9000`). **Push scheduling**: holds a
  cache-miss query until a worker slot frees instead of returning "no worker".
- **Worker** (per compile machine, direct-dispatch on `:9005`): compiles preprocessed source,
  keeps a PCH/header-set cache and its own object outputs.
- **Client** (`suco-cl`/`suco-cl++`): preprocesses locally, computes a SHA-256 content hash,
  dispatches **directly** to the assigned worker (payload never funnels through the coordinator),
  keeps a local L1 object cache. Also races free local cores against grid slots.

---

## CRITICAL INVARIANTS — what to watch for (the expensive lessons)

1. **Byte-identity is mandatory.** Any change to source normalization / header-set split / cache
   keying MUST leave the cache keys unchanged. Before such a change, record golden values
   (`hs_hash` + resulting objects) and diff after. A silent drift invalidates every team cache.
2. **Never build an executed command from a SORTED/normalized flag list.** Normalization is for the
   *key*, not for *execution*. (The `-include cstdint` bug: sorting tore `-include` from its value →
   every forced-include project silently lost all distribution and looked merely "slow".)
3. **A worker's bad state must NEVER fail a build.** Remote exit `127` (toolchain missing) and `-5`
   (header set claimed-but-absent) are *infrastructure* signals → recompile locally, do not adopt
   them as compile results.
4. **PCH/header-set caching and `-fmodules-ts` are mutually exclusive.** Under `-fmodules-ts`, GCC
   turns `-x c++-header` into a header *unit*, ignores `-o`, and exits 0 without producing a `.gch`.
   Rule: never trust the exit code when you can check the produced file.
5. **Provenance:** the worker compiles the source under the *client's original filename* inside a
   per-job dir, else `__FILE__` / `STT_FILE` / DWARF point at a temp path. Use `-fdebug-prefix-map`
   (not `-ffile-prefix-map`) so `__FILE__` still resolves via the line markers.
6. **`known_header_sets`** is learned only via the funnel path and (until `cache clear`) never reset
   → it can go stale → the `-5` self-heal covers correctness. Per-job dirs also isolate same-named
   C++20 modules with different contents across concurrent jobs.
7. **Measurement hygiene:** benchmark only on an idle machine. Background apps (browser, k3s, an IDE)
   steal ~1.5 cores and inflate cold *and especially warm* numbers. The bench script waits for
   load < 1.5. Loaded runs looked 40–60s slower — not a regression.
10. **Winsock `SO_RCVTIMEO`/`SO_SNDTIMEO` take a DWORD of MILLISECONDS, not a `struct timeval`.**
    Passing a timeval makes Winsock read its `tv_sec` as milliseconds — a 30 s timeout becomes
    30 ms. This kept the Windows client off every remote coordinator for the entire port: recvs
    crossing the LAN aborted in ~30 ms as "handshake disconnect", while loopback (sub-30 ms) always
    passed, so local smoke tests were green and the real grid was unreachable. **When a Windows
    net path works on loopback but not across a LAN, suspect a timeout unit bug first.** A raw
    `TcpClient` from PowerShell that gets a correct reply proves the server is fine and the bug is
    client-side.
8. **A hash is not a presence flag.** `HeaderSetHasher::compute_hash` digests flags + compiler
   version + toolchain hash regardless of whether any system header was found, so it returned a
   non-empty `header_set_hash` for a TU with *no* header set. All three callers read "non-empty
   hash" as "this TU has a header set" and swap in `stripped_source`, which the same function only
   fills when `header_paths` is non-empty → the worker receives a header-set hash, no header text
   and an EMPTY TU, and can only answer `-5`. Fixed 2026-07-21 (return `""` when `header_paths` is
   empty). **Not a Windows-only bug** — on Linux any TU without system headers reaches it; on
   Windows it was every TU, because the split recognises system headers by a `/usr/` prefix and
   MinGW's live under `C:/Qt/Tools/...`.
9. **Invariant #3 hides TOTAL failure — so count the self-heals.** Because a worker's bad state is
   absorbed into a correct local compile, "the grid distributes nothing" and "the grid is a bit
   slow" are indistinguishable from the build's exit codes. The entire Windows port ran with
   **zero** TUs compiled on a worker while every build succeeded; two independent bugs hid behind
   one warning line, the second only reachable once the first was fixed. The self-heal is right and
   must stay — but a fallback that is invisible is a fallback that is permanent. Judge distribution
   by `Direct dispatch OK` plus a worker-side `Exit: 0`, never by a green build and never by
   `Cache hit` (which proves only the cache path).

## Diagnostic discipline

The obvious hypothesis was wrong several times; only instrumentation found the real cause. Use the
gated debug switches (`SUCO_TIMING`, `SUCO_DEBUG_PAYLOAD`, `SUCO_SLOT_DEBUG`) instead of guessing.
Operational gotchas: `pkill -f` matches your own command → use `pkill -x`; workers/coordinators can
ignore `SIGTERM` → `SIGKILL` when needed.

---

## How to work on it

- Strict loop: **build → test on loopback → verify byte-identity → test on the grid → deploy.**
  Never push untested changes to the grid.
- Roll out with the local (gitignored) `scripts/deploy_grid.sh` — it reads the node password from
  `$SUCO_SSH_PASS` (nothing hardcoded).
- Release: `git tag vX.Y.Z && git push origin vX.Y.Z` → the Actions workflow builds, signs, and
  publishes the APT repo automatically.
- Keep the two READMEs and `CHANGELOG.md` truthful; benchmark claims must be reproducible.

---

## Open items

- **⚠ Security (owner action):** the grid SSH/sudo password must be rotated — an old value was once
  exposed in a public repo. The deploy scripts no longer hardcode it.
- **Blocked on `apt install` on the nodes** (owner's call): sandboxing (`bubblewrap`), ThinLTO
  (`clang lld`). Both are built/designed, waiting on the packages.
- **Untested here:** `.rpm` (dnf/zypper) and a Homebrew formula — need an environment with
  `rpmbuild` / macOS before shipping.
- **Nice-to-have:** further trim client per-TU feed cost (the memchr header-split landed; the goal is
  already met, so this is optimization, not a gap).

---

## For the Windows machine

- `git clone`/`git pull` on the same repo keeps the **code** in sync — that is the reliable
  "same state" for the project. As of 2026-07-21 the Windows box is a real clone with `origin`
  set (it used to be a ZIP copy), so no more manual file shuttling.
- Write your side into `brain-win.md`. Same rule as here: **no secrets — the repo is public.**
- **Cross-compilers are INSTALLED on all four nodes (2026-07-21):** `g++-mingw-w64-x86-64`,
  GCC **13.2**, alternatives set to **posix** threads (matching the Windows client's Qt MinGW
  13.1-posix — the majors line up, the version gate will pass). The scheduler matches Windows
  jobs by a target-qualified dispatch id (`x86_64-w64-mingw32-g++`) against the worker's
  toolchain map — no protocol change, old workers are simply never selected and the client
  compiles locally. **The one remaining step: a release**, so the nodes' workers pick up the
  toolchain probe and start advertising the cross compiler. Linux→Linux jobs are unaffected.
- **Grid auth vs. the Windows client:** the coordinator (on k3master, NOT Brain-OS) has
  `SUCO_SECRET` enabled; a client without it is refused at handshake. Verified from the Windows
  box: the refusal degrades into a clean local compile (exit 0, object produced) — tested 3×.
  One first-ever-contact run ended exit -1 with no object and could NOT be reproduced; noted in
  brain-win.md, worth an eye. To actually join the grid, the Windows client needs `SUCO_SECRET`
  set (value lives in private notes, never in the repo).
- **Header sets / PCH work on Windows now** (2026-07-21): the system-header predicate also accepts
  paths containing `mingw`. Invariant #1 held by construction — additive predicate (no `/usr/`
  path changes membership) and Windows had zero existing header-set keys. Verified end-to-end on
  the loopback grid incl. PCH build/HIT and a `__LINE__` provenance probe byte-identical to
  native. Cosmetic: `linemarker ignored due to incorrect nesting` warnings on the Windows worker
  (push markers stripped with header text, return markers kept) — provenance proven unaffected;
  worth checking whether Linux workers log the same.
- Antigravity's own conversations/settings are not synced through this file; that's tied to the
  Antigravity account, not the git repo.
