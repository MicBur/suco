# SUCO grid benchmark — native Linux vs. Windows cross-compile

Reproduce with [`scripts/bench_grid.sh`](../scripts/bench_grid.sh):

```sh
sudo scripts/bench_grid.sh              # 48 TUs, 3 rounds
sudo N=96 ROUNDS=5 scripts/bench_grid.sh
```

## Results

Grid: 4 workers / 13 slots (k3master, node1, node2, Brain-OS). Client, coordinator
and one worker share k3master (4 cores). 48 STL/template-heavy TUs per round,
client parallelism 16, three interleaved rounds, medians reported.

| Scenario | Wall | Throughput | vs. local |
|---|---|---|---|
| local `g++ -j4` (no grid) | 8.516 s | 5.6 TU/s | 1.00x |
| grid, native Linux (`g++`) | 2.551 s | 18.8 TU/s | **3.34x** |
| grid, Windows cross (`x86_64-w64-mingw32-g++`) | 2.257 s | 21.3 TU/s | **3.77x** |
| grid, Linux, warm cache | 1.543 s | 31.1 TU/s | 5.52x |

Per-round wall times (linux / windows / local):
`2.535/2.089/8.450 · 2.551/2.257/8.642 · 3.086/2.908/8.516`

Object sanity every run: 48/48 ELF for Linux, 48/48 COFF for Windows — a
silently-failed cross-compile cannot masquerade as a fast round.

## Real project: SUCO building itself

The synthetic figures above use uniform TUs with only system headers. A real
build also has a dependency graph, mixed TU sizes, serial link steps, and TUs
that bypass the grid entirely. Building SUCO through SUCO (46 C++ TUs, 8-core
client, four-node grid):

| Scenario | Wall | vs. local |
|---|---|---|
| local `g++ -j8` | 88.6 s | 1.00x |
| SUCO grid `-j16`, cold | 57.6 s | **1.54x** |
| SUCO grid `-j16`, warm cache | 57.5 s | 1.54x |

**1.54x, not the synthetic 3.34x.** The gap is the honest part. A real build
cannot parallelise past its dependency graph, the link steps are serial and
local, and SUCO's own sources use `__DATE__`/`__TIME__`, which forces those TUs
to compile locally and bypass the cache ("uses `__DATE__`/`__TIME__` — compiling
locally, bypassing cache"). That last point also explains why the warm run does
not beat the cold one here.

Treat the synthetic number as the grid's ceiling and this one as what a real
project sees.

> This benchmark was only measurable after #15: until then the header-set split
> produced a translation unit the worker could not compile, so a real project
> failed to build through the grid at all.

## RocksDB — the head-to-head, re-validated on 0.11.0

365 compile steps (`-DWITH_TESTS=OFF -DWITH_TOOLS=OFF -DWITH_GFLAGS=OFF`), 8-core
client, four-node grid, measured on an idle machine:

| Scenario | Wall | vs. local |
|---|---|---|
| local `g++ -j8` | 363.4 s | 1.00x |
| grid, cold, header-sets **off** (0.11.0 default) | 149.5 s | **2.43x** |
| grid, cold, header-sets **on** | 138.9 s | 2.62x |
| grid, warm cache | 19.1 s | **19.0x** |

Reproduced: a second independent series gave 152.1 / 140.1 / 18.9 s.

**RocksDB does not trigger the #15 header-set defect.** It builds cleanly with the
split enabled, while SUCO's own sources do not. The defect is therefore
content-dependent — which is why it survived so long, and why the earlier RocksDB
figures were not invalidated by it.

**Disabling the split costs ~7.6% here** (149.5 s vs 138.9 s). That is the honest
price of the 0.11.0 correctness mitigation: projects whose headers happen to split
cleanly give up a small amount of speed so that projects whose headers do not still
build at all.

### What is NOT verified: the "parity with Icecream" claim

The project has claimed parity with Icecream on a RocksDB cold build. **That claim
could not be reproduced, because there is no Icecream cluster to compare against:**
`icecc-scheduler` is installed but its service is inactive, and `iceccd` runs only
on one of the four machines. An `icecc` build in this environment compiles locally.

Whether the original comparison ran against a real Icecream cluster or against
local-only Icecream is not something the current state of the machines can answer.
Until an Icecream cluster is stood up and the comparison re-run, treat the parity
claim as unverified.

### Three measurement mistakes worth repeating

The first three attempts at this benchmark were all wrong, in ways that flattered
or distorted the result:

1. **The warm run measured nothing** (0.0 s) — the build directory was not wiped, so
   ninja found everything up to date.
2. **Run ordering leaked cold costs**: the first configuration measured paid all the
   cold worker-cache costs, making the second look 2.8x faster than it was.
3. **Only the client cache was cleared, never the coordinator's L2.** Every "cold"
   run after the first was served from it — `cache_hits: 2526` against
   `cache_misses: 366`, where 366 is exactly one full cold build. Cold numbers
   dropped from ~150 s to ~26 s and looked spectacular; they were cache hits.

A cold grid measurement must clear the client cache **and** the coordinator's cache,
and must not reuse a populated build directory.

## Proof the work actually spreads across the grid

A speedup number alone does not prove multi-node execution — a fast round could
in principle come from one strong node. Measured job distribution for one
48-TU round (`/api/timeline`):

| Node | Jobs | Share | Cores |
|---|---|---|---|
| node1 | 17 | 28.3% | 4 |
| node2 | 17 | 28.3% | 4 |
| k3master | 9 | 15.0% | 4 (also client + coordinator) |
| Brain-OS | 5 | 8.3% | 2 |

17+17+9+5 = 48, i.e. every TU accounted for, and the split tracks each node's
capacity. k3master takes less than its core count suggests because it is also
running the client and coordinator.

The stronger check is slot utilisation: median worker-side compile time was
**661 ms** (min 433, max 938), so 48 TUs represent ~31.7 s of serial compile
work — delivered in ~2.5 s wall. That is **~12.7x effective parallelism across
13 slots (~98% utilisation)**, which is only reachable if all four nodes are
genuinely compiling at once.

## Reading the numbers

**The grid is worth ~3.3x on a cold build** for this workload, and a warm cache
another ~1.65x on top (5.5x vs. a cold local build). Scale matters: at 16 TUs the
speedup drops to ~2.1x because fixed per-build overhead amortises over fewer
jobs. Bigger builds benefit more.

**Windows cross-compiles are *not* slower than native Linux jobs.** The grid
turns them around ~12% *faster* (0.88x), reproducibly — the factor came out
0.88, 0.88 and 0.89 across three independent measurement series.

**But that 12% is not a property of the target OS.** It is confounded by the
compiler version installed on the workers:

- native: `g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0`
- cross: `x86_64-w64-mingw32-g++ (GCC) 13-posix`

GCC 13 compiles this workload faster than GCC 15, and its libstdc++ headers are
smaller. The honest conclusion is the *negative* one, which is the useful one:
**cross-compiling for Windows costs the grid nothing.** Dispatch, header-set
handling and object transfer behave the same for both targets. Anyone wanting a
true target-OS comparison would need matching GCC major versions on both sides.

## Method — and why each part is there

The first naive run reported Windows as *slower* (5.99 s vs. 4.21 s); a second
reported it *faster*. Both were single runs, and both were noise. The script
therefore does the following:

- **Discarded warm-up round per target.** The first batch of a target pays
  cold worker-side header-cache costs that later batches skip. Without this,
  whichever target ran first looked worse.
- **Interleaved rounds + medians.** Run-to-run variance is ~20%, larger than the
  effect being measured. Alternating Linux/Windows within each round cancels
  drift, and the median resists a single slow round.
- **Fresh, uniquely-named sources every round** and a cleared client cache, so
  each measurement is a genuine cold-cache compile rather than a cache hit.
- **Local baseline at `-j$(nproc)`.** Oversubscribing it (`-j16` on 4 cores)
  costs ~7% and would have flattered the grid — the first draft did exactly
  that and overstated the speedup.
- **`SUCO_LOCAL_SLOTS=0` for grid rounds.** Otherwise SUCO's icecc-style
  local-first scheduling hands a single small TU to a free client core and never
  dispatches it, so "grid" rounds would silently measure local compilation.
- **Template/STL-heavy TUs**, so compile time dominates. Trivial TUs would only
  measure dispatch overhead.

## Caveats

- Client, coordinator and one worker share k3master, so client preprocessing
  competes with a worker for the same 4 cores. A dedicated client would improve
  the grid figures.
- k3s runs on all four boxes (benchmark hygiene, invariant #7). Load average was
  ~0.07 at measurement time, but the machines are not dedicated.
- Results are for one workload shape and one grid. Treat the ratios as
  indicative, not as a spec.
