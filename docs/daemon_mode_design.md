# Design: SUCO Daemon Mode (T12)

**Status:** Draft for review — do not implement before the implementation plan is reviewed.
**Author:** Claude (design), implementation: Antigravity, review: Claude.

## 1. Problem

The T8b analysis identified SUCO's remaining structural overhead: CMake/make invoke
`suco-cl++` **once per source file** (501 process spawns for the 500-class project).
Every invocation pays for: process startup (~5ms), config load + toolchain-info read,
a fresh TCP connection to the coordinator, and a batch size of exactly 1 (the batch
sender exists, but with one file per process there is nothing to batch).

A subtle correctness gap follows from the same structure: the local-slot semaphore
(T8b.2) lives **per process**. Under `make -j12`, twelve independent `suco-cl++`
processes each own their private semaphore, so local slots cannot actually limit
global local concurrency. Only a shared daemon can account slots correctly.

## 2. Architecture

One per-user background process, `suco-daemon`, owning all shared state:

```
make -j12 ──► suco-cl++ (thin client, ~0.1ms IPC) ─┐
          ──► suco-cl++ ───────────────────────────┼─► suco-daemon ──► coordinator (1 persistent
          ──► suco-cl++ ───────────────────────────┘        │              TCP conn, multiplexed)
                                                            └──► local compile slots (global)
```

- **IPC:** Unix domain socket at `$XDG_RUNTIME_DIR/suco/daemon.sock` (fallback
  `/tmp/suco-$UID/daemon.sock`), permissions 0600, same-user only.
  Windows: named pipe — deferred (Windows focus comes later).
- **Wrapper protocol:** length-prefixed frames, first frame carries a protocol
  version (lesson from T5). Request = argv + cwd + relevant env (`SUCO_*`, `CCACHE_*`-like
  overrides). Response = exit code + captured stdout/stderr, streamed.
- **Daemon owns:** persistent coordinator connection (heartbeat reuses it), toolchain
  info + version cache (in memory), local prep-cache index, **global** local-slot
  accounting, batch aggregation queue.

## 3. What gets faster, concretely

| Per-file cost today (standalone)         | With daemon               |
|------------------------------------------|---------------------------|
| TCP connect + HELLO to coordinator       | 0 (one persistent conn)   |
| Config parse + toolchain cache file read | 0 (in memory)             |
| Batch size 1                             | aggregation window (~10ms) → real batches |
| Local slots per process (ineffective)    | global slot accounting    |
| Process spawn of wrapper                 | remains (thin, no work)   |

Combined with the posix_spawnp change (2026-07-05), the remaining per-file overhead
target is < 10ms cold, ~1–2ms on prep-cache hits.

## 4. Lifecycle & robustness

- **Auto-start:** first wrapper invocation starts the daemon if the socket is dead
  (lockfile to prevent thundering herd); `SUCO_NO_DAEMON=1` forces standalone mode.
- **Fallback = current code path.** If the daemon is unreachable or answers with a
  wrong protocol version, the wrapper silently runs the existing standalone logic.
  Zero regression risk; daemon is purely an accelerator.
- **Idle shutdown** after 5 min; `suco daemon status|stop|restart` subcommands.
- Stale socket (daemon crashed): connect fails fast → unlink + restart via lockfile.

## 5. Rollout phases

1. **Phase 1:** daemon + wrapper IPC, persistent coordinator connection, global local
   slots, in-memory config/toolchain cache. (Measure: j1/j12 vs. standalone.)
2. **Phase 2:** batch aggregation window (collect requests for ~10ms or until N=16,
   send as one PACKET_COMPILE_BATCH_REQ). (Measure again, separately.)
3. **Phase 3:** pipelining (preprocess next file while previous transfers) and, later,
   integration point for remote preprocessing (T13) — the daemon is the natural place
   for the header-bundle index.

## 6. Open questions (answer in implementation plan, before coding)

1. Response streaming vs. buffered (large compiler stderr)?
2. How does the daemon pick up config changes (mtime check per request vs. SIGHUP)?
3. Crash isolation: one bad job must not kill the daemon (job = task in thread pool,
   exceptions caught per job).
4. How do integration tests run both modes (matrix: standalone + daemon)?
5. Interaction with `suco doctor` (T10): doctor should check daemon health.
