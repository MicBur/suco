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
- The old best-effort **Windows MSVC CI job was already red on main** before this branch existed
  (vcpkg configure failure, `continue-on-error` so nothing noticed). Not touched here; either fix
  it or retire it in favour of the MinGW job, which actually tests the grid.

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
- **To serve Windows clients, a Linux node needs BOTH `apt install mingw-w64` AND a worker build
  that advertises it** (probe added 2026-07-21; ships with the next release). The scheduler now
  matches Windows jobs by a target-qualified dispatch id (`x86_64-w64-mingw32-g++`) against the
  worker's toolchain map — no protocol change, the field was always a free-form string, so old
  workers are simply never selected and the client compiles locally. Version gate applies: the
  node's cross-g++ major must match the client's local g++ major (13 here; Debian bookworm ships
  12 → skipped safely). Linux→Linux jobs are unaffected, they still dispatch as plain `g++`.
- **Header sets / PCH work on Windows now** (2026-07-21): the system-header predicate also accepts
  paths containing `mingw`. Invariant #1 held by construction — additive predicate (no `/usr/`
  path changes membership) and Windows had zero existing header-set keys. Verified end-to-end on
  the loopback grid incl. PCH build/HIT and a `__LINE__` provenance probe byte-identical to
  native. Cosmetic: `linemarker ignored due to incorrect nesting` warnings on the Windows worker
  (push markers stripped with header text, return markers kept) — provenance proven unaffected;
  worth checking whether Linux workers log the same.
- Antigravity's own conversations/settings are not synced through this file; that's tied to the
  Antigravity account, not the git repo.
