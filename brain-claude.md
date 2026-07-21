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
  "same state" for the project.
- Write your side into `brain-win.md`. Same rule as here: **no secrets — the repo is public.**
- Antigravity's own conversations/settings are not synced through this file; that's tied to the
  Antigravity account, not the git repo.
