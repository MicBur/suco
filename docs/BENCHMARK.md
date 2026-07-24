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

## The Windows case: a Windows developer on a Linux cross-compiling grid

This is the configuration SUCO on Windows exists for. The client preprocesses on
Windows, the Linux workers cross-compile with `x86_64-w64-mingw32-g++`, and real
`pe-x86-64` Windows objects come back. Building SUCO itself from the Windows box:

| Scenario | Wall | vs. local |
|---|---|---|
| local MinGW `g++ -j24` | 99.5 s | 1.00x |
| grid only, `-j16` (`SUCO_LOCAL_SLOTS=0`) | 96.9 s | 1.03x |
| **grid hybrid, `-j24` (local cores + grid)** | **64.4 s** | **1.55x** |

**The grid alone does not beat a strong workstation — it ties it.** The client here
has 24 cores; the grid has 13 slots spread over 2- to 4-core Linux machines. Pure
grid dispatch (96.9 s) is within noise of building locally (99.5 s).

**The win comes from using both**, which is the default mode: local-first
scheduling races free client cores against grid slots, and that combination is
1.55x faster than either alone. Anyone benchmarking SUCO by forcing
`SUCO_LOCAL_SLOTS=0` is measuring the grid in isolation, not what a user gets.

The corollary is that SUCO's value on Windows scales with how weak the client is
relative to the grid, not with the grid's absolute size. A laptop gains far more
than this 24-core box does.

> Requires the client to resolve to MinGW rather than MSVC. Since #20 that is the
> default outside a Developer Command Prompt — an MSVC job cannot be dispatched at
> all, because no Linux worker can run `cl.exe`.

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
