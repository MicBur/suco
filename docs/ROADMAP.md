# SUCO — Roadmap

**Goal:** Become the best free distributed C/C++ build system for Linux LANs — measurably
faster than Icecream/distcc, and closing the architectural gap to IncrediBuild step by step.
Linux first; Windows/MSVC polish follows later.

**Honest starting point (v2.0):** SUCO already beats Icecream/distcc feature-wise (grid-wide
object cache, zero-config discovery, zstd wire compression, fast `-fdirectives-only`
preprocessing, path-normalized team-wide cache hits). IncrediBuild still holds one structural
advantage: its process virtualization means the client machine does almost no work, while
SUCO still runs preprocessing locally (mitigated ~2× in v2.0, not eliminated — visible in
the Qt6 warm-build bottleneck). This roadmap is the plan to close that gap.

---

## v2.0 — Prove it (current)

- [x] Fast preprocessing (`-fdirectives-only` / `-frewrite-includes`), cache salt v3
- [x] zstd compression for all network payloads (~94% traffic reduction, measured)
- [x] Path normalization → team-wide cache hits across checkouts
- [x] Protocol version handshake (`PACKET_HELLO`)
- [x] CI (Linux Release, ASan/UBSan, Windows MSVC), MIT license, English docs
- [ ] **Honest comparative benchmark vs. Icecream/distcc** (real runs, raw logs published)
- [ ] Tag & publish v2.0.0

## v2.1 — Trust (make it safe to rely on)

- **Chaos test suite:** worker dies mid-job, corrupted packets, full cache, coordinator
  restart. The failover paths exist; prove them systematically.
- **Cache-correctness hardening:** full flag coverage in hash keys, `__DATE__`/`__TIME__`
  detection (skip caching, like ccache), `SOURCE_DATE_EPOCH` awareness.
- **Protocol fuzzing:** the hand-written network parser is network-exposed; fuzz it.
- **Packaging:** deb/rpm + systemd units, one-line install.
- **Prometheus metrics endpoint** on the coordinator (jobs/s, hit rate, worker load).

## v2.2 — Scale (beyond 3 nodes)

- **Direct client↔worker data path:** coordinator becomes scheduler + cache index only;
  object payloads no longer funnel through one machine. Removes the central bottleneck
  for 10+ workers.
- **Local machine as first-class worker:** a 16-core dev box should compile, not just
  preprocess and wait. Adaptive local/remote split under load.
- **Smarter scheduling:** job stealing, network-aware placement.

## v3.0 — Attack IncrediBuild's moat (Linux)

- **Remote preprocessing** (design: `docs/remote_preprocessing_design.md`): ship raw
  sources + content-addressed header bundles; preprocessing runs on workers. Eliminates
  the last big client-side cost — the one place IncrediBuild still wins — without
  syscall-interception, so it stays portable and robust.
- **Security for shared networks:** TLS + authentication, worker sandboxing
  (Linux namespaces). Icecream never solved this properly; enterprises care.

## v3.x — Where nobody else is good yet

- **Distributed ThinLTO:** once compilation scales, the linker is the next bottleneck.
- **C++20 modules in distributed builds:** unsolved in the free ecosystem; being early
  here is a durable differentiator.

## Later — Windows focus

- MSVC feature parity (PCH, `/Zc:preprocessor` paths), polished Windows workers,
  mixed Linux/Windows grids. Deliberately deferred: Linux is where the comparison
  targets (Icecream/distcc) live and where SUCO can win visibly first.

---

*Every performance claim on this roadmap ships with reproducible scripts and raw
measurement logs — no simulated competitor numbers, ever.*
