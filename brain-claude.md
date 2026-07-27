# brain-claude.md — SUCO project context & cross-machine handoff

> Shared context so any agent/machine (Linux here, Windows via `brain-win.md`) picks up the
> same understanding. **This file is in a PUBLIC repo — never put passwords, `SUCO_SECRET`
> values, tokens, or exploitable host details in it.** Credentials live only in private notes.

Last updated: 2026-07-27 (v0.12.0 released; v1.0.0 milestone open, #42 remote-preprocessing
foundation landed — see the v1.0.0 roadmap section below).

---

## The goal

SUCO is a distributed C/C++ compiler for a LAN. Target: **match IncrediBuild / Icecream on
speed, but be installable in 30 seconds via `apt`.**

**Status (re-measured on 0.11.0, 2026-07-24).** RocksDB, 365 compile steps, 4-node grid,
idle 8-core client — see `docs/BENCHMARK.md` for method and the mistakes it corrects:
- **Cold build: 2.43× faster than local `g++ -j8`** (149.5s vs 363.4s). Reproduced twice.
- **Warm rebuild: 19× faster** (19.1s) — Icecream has no cache; SUCO serves unchanged objects
  from a content-addressed cache. This remains the clearest structural advantage.
- Published publicly: `sudo apt install suco` from https://micbur.github.io/suco.

**The Icecream comparison is currently UNVERIFIED.** The earlier entry here claimed parity on a
cold build (SUCO 100.7s vs icecc 101.9s). That could not be reproduced: there is no Icecream
cluster on these machines — `icecc-scheduler`'s service is inactive and `iceccd` runs on exactly
one of the four hosts, so `icecc` here compiles locally. Whether the original figure was measured
against a real Icecream cluster or against local-only Icecream cannot be determined from the
current state of the machines. Do not repeat the parity claim until a cluster is stood up and the
comparison re-run. **What is measured is SUCO against a local build, not against Icecream.**

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

## Current state (2026-07-26)

- Public repo **github.com/MicBur/suco**, released **v0.12.0** (grid still on 0.11.0 until redeployed). APT
  repo signed + published on every `v*` tag → GitHub Pages; CI green. Docs: `docs/INSTALL.md`,
  `docs/INSTALL-apt.md`, `docs/BENCHMARK.md`.
- **The Windows→Linux cross-compile vision works out of the box.** A Windows dev runs
  `suco-cl++ -c foo.cpp -o foo.o`, a Linux worker cross-compiles with `x86_64-w64-mingw32-g++`,
  and a real `pe-x86-64` object comes back — no configuration. Verified independently by both
  agents. The client defaults to MinGW (not `cl.exe`) unless an MSVC env is active (#20).
- **Default grid = 4 Linux workers / 13 slots** (k3master, node1, node2, Brain-OS=node3). A local
  **Windows worker `WIN-DEV`** (192.168.0.24, 8 slots) is **opt-in**, toggled via Antigravity's
  `suco-gui.exe` ("Start WIN-DEV Worker") — normally OUT. With it in, the grid is 5 workers / 21
  slots and Windows-target TUs can compile natively there (e.g. for GCC-14+ headers).
- CI matrix: Linux Release, Linux Debug (ASan/UBSan), Windows MinGW (blocking smoke), Windows
  MSVC (build-only), GitGuardian. **Docs-only PRs skip the build matrix** (`paths-ignore`), so a
  markdown change no longer waits ~17 min on the MSVC job. `main` is **not** branch-protected.
- MSVC background (still true): the code was written for MinGW (POSIX names under `_WIN32`); MSVC
  needs `WIN32_LEAN_AND_MEAN`+`NOMINMAX`, a `platform_compat.h` shim, and a byte-identical rewrite
  of a greedy `\x1f` escape. Reproduce locally: vcpkg (`openssl zstd sqlite3 --triplet
  x64-windows`) + `cmake -G "Visual Studio 17 2022" -A x64 --toolchain vcpkg.cmake`. Build Tools
  under `C:\Program Files (x86)\...\2022\BuildTools`.
- **Open issue:** #24 (header-set splitting breaks cross-toolchain dispatch — why the split stays
  off by default). #26 (a supposed `<format>` cross-compile failure) was **closed invalid** — it
  was a test error (missing `-std=c++23`); `<format>` cross-compiles fine with the right `-std`.

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

10. **Winsock `SO_RCVTIMEO`/`SO_SNDTIMEO` take a DWORD of MILLISECONDS, not a `struct timeval`.**
    Passing a timeval makes Winsock read its `tv_sec` as milliseconds — a 30 s timeout becomes
    30 ms. This kept the Windows client off every remote coordinator for the entire port: recvs
    crossing the LAN aborted in ~30 ms as "handshake disconnect", while loopback (sub-30 ms) always
    passed, so local smoke tests were green and the real grid was unreachable. **When a Windows
    net path works on loopback but not across a LAN, suspect a timeout unit bug first.** A raw
    `TcpClient` from PowerShell that gets a correct reply proves the server is fine and the bug is
    client-side.
11. **The header-set split — reassembly defect FIXED in #15; still off by default (#24).**
   `header_set_hasher.cpp` classifies preprocessed output line-by-line into "system headers"
   (shipped once, cached, optionally PCH'd) and "stripped source". Concatenating the two back
   does NOT reproduce the input. Proven outside SUCO: the original `.ii` compiles clean, the
   reassembly yields **758 errors**. Two independent defects — (a) the `<built-in>`/
   `<command-line>` preamble is classified as non-header, so the predefined macros land AFTER
   the system headers that use them (`'__SIZE_TYPE__' does not name a type`); fixing only this
   gets 758 → 44; (b) markers of stripped system headers are dropped by design, but line
   markers NEST, so the remaining ones are unmatched (`linemarker ignored due to incorrect
   nesting`) and the content after them is misattributed (`expected declaration before '}'`,
   `field 'ip_dst' has incomplete type`). Consequence: **any project with a realistic header
   set failed to build on the grid**, and failed it outright — the worker returns exit 1, which
   the client treats as a real compile error, so invariant #3 never engaged. Mitigated by
   disabling the split by default; the switch `SUCO_HEADER_CACHE_ENABLED` had never actually
   gated the splitting, only the worker's PCH choice, which is why "turning it off" appeared to
   change nothing. Trivial TUs (system headers only) take a different path and compile fine —
   which is exactly why every smoke test passed for so long. **Both defects were fixed in #15
   (2026-07-25):** recognise indented line markers, and put the `<built-in>` preamble in the
   header set. Proven output-transparent — a full Linux SUCO build gives **102/102 byte-identical**
   objects with the split on vs off (`content_hash` is computed before the split, so invariant #1
   holds). It stays OFF by default only because it breaks CROSS-toolchain dispatch (#24): a Windows
   client's `C:/Qt` MinGW header set does not reconstitute on a Linux cross worker. Enable with
   `SUCO_HEADER_CACHE_ENABLED=1` on a same-toolchain grid for ~7.6%.
12. **An unreachable coordinator cost ~3 minutes per TU, not one second.** A single TU opens
   ~59 coordinator connections (cache query, header-set query, batch send, result upload, plus
   the backpressure re-query loop); each paid the full `connection_timeout_ms` (3000 ms). The
   build still produced a correct object via the local fallback — invariant #3 again masking a
   pathology. Fixed with a process-wide circuit breaker (2 consecutive failures → fail fast for
   30 s; any success resets it, which matters for the long-lived daemon). 181 s → 8 s.
   **Rule of thumb: SUCO must never make a build slower than compiling locally.**

## Test-harness traps that cost real time (2026-07-23)

These produced false bug reports before they were understood. All are MY errors, not SUCO's:

- **`SUCO_COORDINATOR` does not exist — it is `SUCO_COORDINATOR_HOST`.** Setting the wrong name
  silently leaves the client on the `127.0.0.1` default. It looks exactly like "the grid is
  broken", and it also explains a node that appeared unable to reach the coordinator.
- **On Windows the client defaults to `cl.exe`, not g++.** `suco-cl++ -c x.cpp` with no
  `SUCO_REAL_CXX` and no MSVC environment exits with almost no output. For the MinGW/grid path
  set `SUCO_REAL_CXX=g++`; for MSVC run inside `vcvars64.bat` (VS 2022 **BuildTools** here).
- **PowerShell swallows a native tool's stderr** when the tool is run through it — a failing
  `cmake --build` showed `FAILED` lines with zero diagnostics. Run such builds from bash with
  `> log 2>&1`.
- **`/dev/null` breaks the MinGW assembler** (`can't create nul`). Write to a real temp object.
- **A file in `/tmp` you cannot overwrite yields a stale `tail`.** A redirect failing made a
  successful command look like a failure and showed another session's log. Write logs into a
  directory you created.
- **Pass the `-std` a feature needs before calling a header "un-cross-compilable".** `std::format`
  does not exist at the `gnu++17` default — `suco-cl++ -c format.cpp` fails locally *and* on the
  grid for that reason alone. Testing cross-compile without `-std=c++23` produced a whole invalid
  "#26" that was closed as a test error. Reproduce the LOCAL compile with the identical flags
  first; if it fails locally, it is not a SUCO bug.

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

## Working with Antigravity — what it can do, and what it should own

**Antigravity** (Google's agentic dev platform; public preview, macOS/Windows/Linux, free) is the
second agent on this project. It maintains its own handoff doc, **`brain-ag.md`** (SSH inventory,
Windows paths, toolchain). It set up passwordless SSH to the grid — `ssh k3master`, `ssh node1`,
`ssh node2`, `ssh node3` (node3 = the box this file calls **Brain-OS**, 192.168.0.20).

**What it can do** here: an agent-first Manager surface (spawn/supervise several agents in
parallel, async Inbox); agents act across editor + terminal + browser in a single task (run the
loopback grid and verify the `:9001` dashboard in the browser); **Artifacts** as reviewable
deliverables (task lists, plans, screenshots, browser recordings) with inline review; a knowledge
base for cross-task memory. Models: Gemini 3, Claude Sonnet 4.5, GPT-OSS.

**What Antigravity should own** (division of labour, so the two agents stop colliding):

- **The Windows side.** Build and verify the `suco*.exe` on the Windows box (MinGW 13.1.0), keep
  `brain-ag.md`'s inventory current (SSH aliases, paths, toolchain), and own SSH/key management —
  it already generated and deployed the keys.
- **Browser-based verification.** Use its browser surface to check the dashboard live and capture
  Artifacts (screenshots/recordings) as grid-dispatch proof — `Direct dispatch OK`, per-node
  distribution, target-OS badges. This is where AG is strongest.
- **The Windows→Linux client path.** Exercise the flagship cross-dispatch from Windows and file
  what breaks there (this is how #24 surfaced; #26 turned out to be a test error, now closed).

Claude (this file) owns the **Linux/grid side**: coordinator/worker code, byte-identity and the
invariants above, releases, and the APT/Windows publish workflows.

**Antigravity's queue — DONE (2026-07-26).** All four items completed and evidenced in
`brain-ag.md` (walkthrough Artifacts live under `~/.gemini/antigravity/brain/...`, outside the repo):

1. **Windows installer** real-world tested — silent install, DLL audit, cross-dispatch from the
   *installed* binary, clean uninstall.
2. **Live dashboard proof** of the `target_os:"windows"` badge + job feed.
3. **C++23 header matrix.** With `-std=c++23`, `<format>`, `<expected>`, `<ranges>`,
   `<stacktrace>`, `<source_location>` (and more) cross-compile fine on the Linux workers;
   `<print>`, `<generator>`, `<mdspan>`, `<flat_map>`, `<flat_set>` do not — they need GCC 14+,
   which is not a SUCO issue. **This closed #26 as invalid:** an earlier "`<format>` fails cross"
   report (mine) was a test error — it omitted `-std=c++23`, and `std::format` does not exist at
   the `gnu++17` default *anywhere*, local included. Pass the `-std` a feature needs before calling
   a header "un-cross-compilable". AG's matrix was right all along.
4. **Circuit breaker (#14)** field-confirmed on Windows: ~8 s fail-fast on a dead IP → local
   fallback → exit 0.

**Antigravity's queue — status (2026-07-27).** AG's `brain-ag.md` (its LOCAL, gitignored doc — not in
the repo) now shows Task 8–13 done: WIN-DEV PATH-hardening, local-main reset, VS Code compile check,
VSIX scaffold, an 8-page GitHub **Wiki** (`wiki/` + `scripts/deploy_github_wiki.ps1`, on main), and
the **GPG-signed APT repo** (`release-apt.yml`, on main). AG worked on branches this time (good), but
twice left them **without opening a PR** — Claude filed + merged both: the WIN-DEV/VSIX branch (#40)
and a stranded 1-line PPA version fix (#49, `make_ppa_source.sh` was reading `cmake_minimum_required`
3.15 instead of `project(... VERSION 0.12.0)`). **Lesson to reinforce with AG: push the branch AND
open the PR** — an unpushed-PR branch is invisible and strands the work. The original queue below is
now essentially complete.

---

### ⟶ ANTIGRAVITY — UPDATED (strict, 2026-07-27 later)

Good work on Task 14 (real Windows→Linux PE/COFF cross-compile under V3 — dispatch confirmed, object
links + runs, `.text` byte-identical) and on keeping to branch+PR this round. Read the update below.

**⚠ ONE THING TO UNDO / NOT REPEAT: you flipped `SUCO_REMOTE_PREPROCESS` to default-ON (#74).**
That is a **release-level decision** and it touched **client-core** config — both explicitly outside
your lane (rule 3). It is NOT settled: Claude's recommendation to the owner is to ship #42 **opt-in for
one release**, gather real-world use, THEN flip — because default-on means every user's every build
silently uses V3, and a single silently-wrong object would violate the byte-identity invariant (#1).
The owner will decide. **Until the owner says otherwise: do NOT flip defaults, do NOT change
`client_config`, do NOT make release-level calls.** If you think a default should change, put it in
`brain-ag.md` as a proposal and let the owner/Claude decide — don't just merge it.

**What actually helps now — HARDEN THE EVIDENCE (this is your lane, and it's what the flip is missing):**

- **TASK A — BROAD cross-compile verification.** Your check was ONE file with one project header. Do a
  real sweep from the Windows client with `SUCO_REMOTE_PREPROCESS=1`: many `.cpp`, multiple/nested
  project headers, different flags (`-O0/-O2/-O3`, `-g`, `-DNDEBUG`), and for each compare the V3
  `pe-x86-64` object to `SUCO_REMOTE_PREPROCESS=0` (grid) and to a plain local MinGW compile
  (`fc /b`). Report the identical/differ counts + any DIFFER cases on **issue #42**. THIS breadth is
  what would actually justify default-on.
- **TASK B — a REAL Windows project under V3.** Build an actual multi-TU project (e.g. your 101-TU
  benchmark, or a real app) with `SUCO_REMOTE_PREPROCESS=1`, confirm it links + the binary runs
  correctly, and note any TU that fell back (time-macro/module/PCH). Real-world exercise is exactly
  the confidence the opt-in period is meant to gather.
- **TASK C — VS Code extension on real hardware** (F5 Dev-Host + Artifact) and **confirm WIN-DEV is on
  0.12.0 with DLLs bundled** — finish these if not already Artifact-proven.

Everything above stays Windows-side / verification. No grid code, no client-core, no default flips.

### ⟶ ANTIGRAVITY — DO THIS (strict, 2026-07-27) [Task 1 below = DONE, kept for context]

Read this and follow it exactly. These are Windows-side tasks only you can do; Claude has taken #42
(remote preprocessing) as far as the Linux grid allows.

**RULES — non-negotiable:**
1. **Sync first, every time:** `git fetch origin && git reset --hard origin/main` BEFORE any work.
   `main` is way ahead of you (through ~#70). Do NOT start on a stale tree.
2. **Branch + PR, always.** For ANY change: new branch off `origin/main`, push it, **AND OPEN THE PR**
   with `gh pr create`. You left #40 and #49 as pushed branches with no PR — that stranded the work
   and Claude had to file them. A branch without an open PR does not exist. No exceptions.
3. **Do NOT touch grid/Linux/client-core code.** File a GitHub issue if you find a grid bug; do not fix
   it. Your lane is the Windows side, the GUI, and browser/real-hardware verification.

**TASK 1 (highest priority) — verify V3 remote preprocessing on a REAL Windows→Linux cross-compile.**
This is the ONE #42 check Claude cannot do (on Linux the wrapper defaults to local g++ → ELF, not
PE/COFF — see `docs/remote_preprocessing_verification.md` §2b). On the Windows box, with the grid up
(WIN-DEV OUT, 4 Linux workers):
  - Compile a few real `.cpp` (with project headers) three ways and compare the resulting **`pe-x86-64`**
    objects: native `suco-cl++ -c foo.cpp` (RPP off), `SUCO_REMOTE_PREPROCESS=0 suco-cl++` (grid), and
    `SUCO_REMOTE_PREPROCESS=1 suco-cl++` (V3). Confirm the worker log shows `Compiling direct RPP job`.
  - Report: is the V3 object byte-identical to the normal-grid object? To a plain local MinGW compile?
    (`fc /b` on Windows, or `cmp` on the worker.) Capture it as an Artifact and comment the result on
    **issue #42**. This is the last thing gating `SUCO_REMOTE_PREPROCESS` from becoming the default.

**TASK 2 — VS Code extension on real hardware.** `extension/vscode/` is on main. `npm install && npm run
compile`, F5 → Extension Dev Host, point `suco.coordinatorHost` at `192.168.0.200`, Artifact the status
bar showing real numbers + the CMake-toggle. (Was queued; confirm done or do it.)

**TASK 3 — confirm WIN-DEV worker is on 0.12.0 with the DLLs bundled** (Task 8 in your brain-ag). Toggle
it in via the GUI, confirm it registers + takes a job, toggle it out. If the DLL fix isn't actually on
`main` via a PR, open one.

**Original NEXT queue (2026-07-26)** — the Task 1–7 backlog was done; these were the live items,
all squarely Windows-side / verification (AG's turf), none touching grid code:

1. **Fix + harden the WIN-DEV worker deployment (defect — highest priority).** This session the
   WIN-DEV `suco-worker.exe` on `C:\Users\micbu\Desktop\suco\` crash-looped with
   `STATUS_DLL_NOT_FOUND` (exit `-1073741515`) — `libssl-1_1-x64.dll` + `libcrypto-1_1-x64.dll`
   were missing next to the exe. Manually patched by copying from
   `C:\Qt\Tools\mingw1310_64\opt\bin\`, but AG's GUI "Start WIN-DEV Worker" bundle must **ship
   those two DLLs itself** so an attach can never crash-loop again. Also **rebuild the WIN-DEV
   worker at v0.12.0** (Desktop copy is stale). Verify: toggle WIN-DEV in via the GUI, confirm it
   registers on the dashboard and takes a job, toggle it out. Owner: AG (it owns the GUI + WIN-DEV
   deployment).
2. **Reset AG's local `main` to `origin/main`.** It may still carry the dropped `6deef07`
   (`fix(client): ship -E for cross`, the non-bug #26). `origin/main` is the source of truth; a
   local main that still has it will fork the two agents again on the next shared edit.
3. **Browser / Extension-Dev-Host verification of the new VS Code extension** (`extension/vscode/`,
   PR #38, on main). On the Windows box: `npm install && npm run compile`, press **F5** to launch
   the Extension Dev Host, point `suco.coordinatorHost` at `192.168.0.200`, and capture an Artifact
   of the status bar showing real numbers (`⚡ SUCO: 4w · cache …%`) + the "Toggle grid for CMake"
   action writing `CMAKE_CXX_COMPILER_LAUNCHER`. This validates Claude's PoC on the real Windows
   client — exactly the browser/verification role AG is strongest at.
4. **Scaffold the Visual Studio (VSIX) counterpart.** VSCode (TS) is Claude's; the **Visual Studio**
   extension is Windows-native MSVC tooling — AG's turf, so the two don't collide. Most useful in
   CMake *Open Folder* mode (inject `CMAKE_CXX_COMPILER_LAUNCHER=suco-cl++`, surface cache-hit rate
   from `/api/stats`); classic MSBuild `cl.exe` injection is the fiddly exception, park it. Land via
   branch + PR under `extension/visualstudio/`.

Optional, if capacity: an **MSVC-cache walkthrough Artifact** — capture the verified headline
("second identical MSVC compile = coordinator cache hit, byte-identical object, no `cl.exe`
recompile") as a reviewable terminal+dashboard recording, since that is the value prop for MSVC devs
who can't cross-dispatch.

AG also built, now **on `main`** (PR #34): a **Qt 6 desktop control center `suco-gui.exe`**
(system tray, worker toggle, one-click WIN-DEV attach/detach) and a **101-TU Windows benchmark**
harness (≈46 s native → ≈12 s grid, ≈3.8×; the hybrid figure INCLUDES WIN-DEV, unlike the
Linux-only numbers under "The goal"). Additive only — new `src/gui/` + PowerShell scripts + a
`find_package(Qt6 ... QUIET)`-guarded CMake target, so CI without Qt6 just skips it.

**The Windows worker `WIN-DEV` is opt-in, normally OUT.** Default grid = 4 Linux workers / 13
slots; the GUI's "Start WIN-DEV Worker" attaches the local Windows box (8 slots) on demand — a
native Windows worker for GCC-14+ headers or plain extra capacity, **not** a #26 escape hatch (#26
is closed as invalid). Grid-side tests should confirm WIN-DEV is OUT first, so they run on the
default 13 slots.

**Resolved (2026-07-26): the two-agent git tangle around #26.** AG had committed BOTH the GUI/bench
(`98ac658`) and an unnecessary `fix(client): ship -E for cross` (`6deef07`) directly onto local
main, unpushed, bypassing branch+PR and Claude's verify gate. Resolution: Claude cherry-picked only
`98ac658` onto a fresh branch from origin/main → PR #34 (landed); `6deef07` was **dropped** (it fixes
the non-bug #26 and moves the Windows-cross `content_hash`). **origin/main is the source of truth**;
a local main that still carries `6deef07` should be reset to origin/main. Lesson, the hard way:
shared code goes via branch+PR from origin/main — a direct commit to local main forks the two agents
and needs untangling. File grid-side bugs as issues; don't fix grid-side code (Claude's side). This
section lives here because Claude owns this file; AG mirrors what it needs into `brain-ag.md`.

**Coordination rules — learned the hard way (the two agents collided on THIS file):**

- Each agent owns its own handoff doc. Antigravity edits `brain-ag.md`; Claude edits
  `brain-claude.md`. Don't rewrite the other's doc — cross-reference it.
- Shared files (source, `CHANGELOG.md`, workflows, this file) change via a **git branch + PR**,
  never by overwriting another agent's work in the working tree. A local rewrite of a file the
  other agent has an open branch on silently loses work — that is exactly what happened here.
- Whoever acts on the grid follows the strict loop — *build → loopback → byte-identity → grid →
  deploy* — and the invariants. Autonomy does not exempt either agent from them.

Sources: [antigravity.google](https://antigravity.google/blog/introducing-google-antigravity).

---

## v1.0.0 roadmap — in progress (2026-07-27)

Milestone #1, issues #42–#46 (`docs/ROADMAP.md`). Started #42 **Remote Preprocessing** — the
highest-value item; built bottom-up in dormant, CI-verified slices so nothing can affect normal
builds until byte-identity is proven:

- **#48 (landed):** byte-deterministic bundle format + client `-MM` dependency scanner
  (`src/common/header_bundle_format.*`, `src/client/header_bundle.*`), self-test in ci_smoke_test.
- **#50 (landed):** worker-side `materialize()` (zip-slip-guarded unpack) + `remap_include_flags()`
  — the §6.3 include-path remap crux, pure/tested. Verified on MSVC + MinGW + Linux ASan/UBSan.
- **#51 (landed):** reserved `PACKET_DIRECT_COMPILE_REQ_V3 = 26`, added `SUCO_REMOTE_PREPROCESS`
  client flag (default off), and wrote **`docs/remote_preprocessing_impl.md`** — the exact wire
  framing, worker `execute_remote_preprocess()` steps, and the byte-identity gate procedure.

- **#53 (landed) — byte-identity EMPIRICALLY PROVEN** on node3 (g++ 15.2): the core approach
  (materialize → remap → compile RAW source in a fresh workspace) is byte-identical to native and
  deterministic across workspaces for `-O2`; `-g` needs exactly one flag, `-ffile-prefix-map=<ws>=.`
  (analogous to the existing `-fdebug-prefix-map`). Findings + the V3 cache-key derivation are in
  `docs/remote_preprocessing_impl.md` §4a.
- **#57 (landed) — the worker compile function is DONE + proven.**
  `JobExecutor::execute_remote_preprocess()` (job_executor.cpp) does the real remote compile
  (materialize + remap + `-x c++` + `-ffile-prefix-map`, honours `SUCO_SANDBOX`). Verified END-TO-END
  by calling the actual function on node3: `-O2` byte-identical to native; `-g` deterministic
  rpp↔rpp; identical under `SUCO_SANDBOX=1`. Dormant (no V3 receive branch yet), GCC/MinGW only.

- **#59 (landed) — the WIRE is DONE + grid-verified.** `SUCO_REMOTE_PREPROCESS=1` end-to-end:
  client builds the bundle + a V3 cache key (`enable_remote_preprocess`, shared by job_sender AND
  pipeline_orchestrator — the ACTIVE path is the latter/BatchSender, not JobSender), ships RAW source
  + bundle (V3 send in `try_compile_direct`), worker V3 receive branch → `handle_remote_preprocess_job`
  → `execute_remote_preprocess`. Verified on node3: worker logs `Compiling direct RPP job`, 7/7 remote,
  7/7 cache hits, binary runs; no-flag default path unchanged (regression-checked); all 8 CI checks
  green. **#42 correctness is COMPLETE.**

- **#63 (landed) — the client-CPU win.** Eligible V3 TUs now skip the local `-E` entirely: the
  V3 decision moved BEFORE preprocessing in `pipeline_orchestrator`, so on success the expensive
  `-E`/normalize/hash block is skipped and the job dispatches straight from raw source + bundle
  (cheap `-MM`). Correctness guards for the lost `needs_local` net: `enable_remote_preprocess`
  rejects `__DATE__`/`__TIME__`/`__TIMESTAMP__` + C++20 modules on the RAW source, and
  `build_header_bundle` flags `has_time_macros` while reading each PROJECT header (the header-
  introduced case). Rejected → local preprocessing. Verified on node3: skip-`-E` logged, 7/7 V3 +
  cache hits, and a `__DATE__`-in-header TU correctly falls back. CI runs both flag/no-flag smoke.

**#42 is FEATURE-COMPLETE** (correctness + the CPU win) and **byte-identity-VERIFIED** (#65,
`docs/remote_preprocessing_verification.md`): synthetic header-heavy corpus **240/240 byte-identical
to native** across -O0/-O2/-O3; real **fmt** (+gtest) **`.text` byte-identical**, delta only
relocatable paths (matches the existing grid's normalisation). The sweep also caught + fixed a real
V3 **cross-compile** bug (#66): base_cmd used `compiler_path` (local) not `get_remote_compiler_name()`
→ correct-by-construction now (V3 mirrors the normal dispatch path's compiler resolution). **Honest
correction:** my node3 "cross-compile" test was NOT a real cross-compile — on Linux the wrapper
defaults to local g++ (produces ELF, not PE/COFF), so the "drop-in verified" claim was a Linux
comparison; #67 was closed as not-a-bug (ELF-vs-PE test artifact). A TRUE Windows→Linux (PE/COFF) V3
verification needs a **Windows client** (AG's turf) — the one remaining cross-compile check before
default-on. Also remaining: bundle dedup — but analysis shows it's **low-value** (a bundle is one
TU's full -MM project-header set, so bundle hashes rarely repeat across TUs → poor dedup hit rate,
plus it adds cold-build round-trips; the real win would be header-LEVEL dedup, a bigger redesign). Other v1.0 items (#44 mTLS, #45 job stealing) are
Linux/grid = Claude; #46 macOS blocked on a build host. #43 sandbox = DONE (below).

**#43 sandbox compile-path — LANDED (#55, 2026-07-27).** Opt-in `SUCO_SANDBOX=1` now wraps the
worker's compiler invocation (`JobExecutor::sandbox_wrap_compile`, job_executor.cpp) in a read-only-fs
+ rw-job-dir/`/tmp` + no-net namespace sandbox — bwrap preferred, unshare fallback. Verified on node3:
compiling under BOTH backends is **byte-identical** to native, and `SUCO_SANDBOX=1 ci_smoke_test.sh`
passed end-to-end (7 TUs, valid objects, binary runs, 7/7 cache hits, no crashes). Default behaviour
is unchanged (no-op `cd <job_dir> && cmd` when off). `bwrap` IS installed on node3 and unprivileged
userns works (`kernel.apparmor_restrict_unprivileged_userns=0`). Remaining to fully close #43: per-job
`/tmp` isolation (currently binds all of `/tmp` rw) and a startup capability note; CI can't run
unprivileged userns so it stays grid-verified.

## Open items

- **⚠ Security (owner action):** the grid SSH/sudo password must be rotated — an old value was once
  exposed in a public repo. The deploy scripts no longer hardcode it.
- **PAT rotation (owner):** the GitHub PAT in the Brain-OS git remote should be rotated.
- **Sandboxing (#43): NO LONGER blocked** — `bwrap` is installed on node3 and unprivileged userns
  works there; both backends compile byte-identically (see the v1.0.0 section). Still needs the
  worker-side wiring + the writable-temp-path bind. ThinLTO (`clang lld`) may still want packages.
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
