# `suco run` — Generic Distributed Task Execution

Generalises SUCO from a C/C++ compiler grid into a **generic build-step grid**: distribute *any*
command (code generators, IDL/protobuf, shader compilers, asset pipelines, test runners, linters)
to grid workers — the core capability IncrediBuild offers beyond compilation. Combined with SUCO's
content-addressed cache, it also *caches task outputs by input hash team-wide*, which IncrediBuild
does not do.

## Model

```
suco run [--in FILE]... [--out FILE]... [--worker IP:PORT] -- COMMAND ARGS...
```

The client ships the declared `--in` files to a worker, the worker runs `COMMAND` in a fresh
isolated temp dir populated with those inputs, and the declared `--out` files are written back to
the client. `COMMAND` runs under `sh -c` in the temp dir, so shell redirects/pipes work; each argv
token is shell-quoted so the reconstructed command is equivalent to the original.

Wire protocol (new): `PACKET_RUN_REQ` (client→worker) = command + N inputs (path+content) + declared
output paths; `PACKET_RUN_RESP` (worker→client) = exit code + merged stdout/stderr log + produced
output files. Handled in the worker's direct listener next to `PACKET_DIRECT_COMPILE_REQ`.

## Phase status

- **Phase 1 — DONE (prototype, verified):** explicit `--worker IP:PORT`, remote execution in an
  isolated temp dir, input→output round-trip, graceful **local fallback** when no worker is
  reachable. Verified: `tr`/`wc`/`pwd` run remotely (log shows the worker's `/tmp/suco_run_*` cwd),
  outputs returned correctly, fallback works.
- **Phase 2 — the IncrediBuild-and-beyond part:**
  1. **Result caching — LOCAL DONE (verified):** `task_hash = sha256(command + sorted(input
     path+content hashes))`; hit under `<cache_dir>/runcache/<hash>.bundle` restores the declared
     outputs and skips execution entirely (verified: same inputs → hit, worker not run; changed
     input → re-run). Only successful runs are cached. This is the "ccache for arbitrary tasks"
     mechanic that goes beyond IncrediBuild. **Open:** make it team-wide via the coordinator blob
     cache (query/store the output bundle by task_hash, reusing PACKET_CACHE_QUERY/STORE).
  2. **Auto-discovery + scheduling — open:** pick a worker via the coordinator (expose `direct_port`
     in `/api/stats`, or a light PACKET_RUN_SCHEDULE) instead of `--worker`.
  3. **Auth on the direct listener — DONE:** compile + run now require HMAC auth when SUCO_SECRET is
     set (closes the RCE this feature opened).
- **Phase 3 — later:** auto I/O tracking (run under `fanotify`/`LD_PRELOAD`, capture read/written
  files → no `--in`/`--out` declaration needed; approaches IncrediBuild's transparency, portably);
  distributed test execution (parallelise + cache test results, re-run only changed tests).

## Security (IMPORTANT — before production use)

`suco run` executes **arbitrary commands** on workers. The worker's direct listener currently has
**no authentication** (unlike the coordinator handshake, see `docs/security_auth.md`). On an
untrusted or shared LAN this is remote code execution. **Before enabling `suco run` beyond a trusted
network, the direct listener (compile AND run) must require the shared-secret auth** (or the grid
must be network-isolated). This is tracked as the top follow-up.

## Files

- Protocol: `src/common/protocol.h` (`PACKET_RUN_REQ` = 21, `PACKET_RUN_RESP` = 22).
- Worker handler: `handle_run_request()` in `src/worker/worker.cpp`.
- Client: `RunCommand` in `src/suco_wrapper/main.cpp` (`suco run`).
