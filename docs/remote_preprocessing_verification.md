# Remote Preprocessing — byte-identity verification (v1.0.0 #42)

Evidence that the opt-in remote-preprocessing path (`SUCO_REMOTE_PREPROCESS=1`, the V3
packet) produces correct, deterministic objects. Run on grid node3 (g++ 15.2), 2026-07-27,
comparing V3-grid objects against a plain local `g++` compile of the same TU.

## 1. Synthetic header-heavy corpus — byte-identical to native

A generated project with 3 nested project headers (templates, inline functions, macros)
and N translation units, each pulling the project headers + system headers (`<map>`,
`<numeric>`, `<cmath>`, `<algorithm>`, …). Compiled from a **relative** source path (as
real build systems do), native vs V3, then `cmp`'d:

| Optimisation | TUs | Byte-identical to native | Went via V3 |
| :--- | :--- | :--- | :--- |
| `-O2` | 100 | **100 / 100** | 100 |
| `-O2` | 40 | **40 / 40** | 40 |
| `-O0` | 50 | **50 / 50** | 50 |
| `-O3` | 50 | **50 / 50** | 50 |

**240 / 240 byte-identical to native**, every TU confirmed on the V3 path. For code that
doesn't embed absolute paths, remote-preprocessed objects are bit-for-bit what a local
compile produces.

## 2. Real library (fmtlib/fmt) — identical code, relocatable paths

The fmt sources and its gtest-based tests (heavy templates, `<chrono>`, ranges, mocks).
The gtest `EXPECT`/`ASSERT` macros embed `__FILE__` **from headers**, so those TUs differ
from a plain native compile — investigated in full:

```
format-test.cc:  native 1007209 B  vs  V3 1007182 B   (Δ 27 bytes)
  .text (machine code): BYTE-IDENTICAL
  difference is ENTIRELY embedded paths:
    native: /tmp/fmt/include/fmt/format.h   (absolute)
    V3:     ./include/fmt/format.h          (normalised, relocatable)
  + a benign rodata string-merge alignment section name (.str1.8 vs .str1.1)
```

The **compiled code is byte-identical**; only the embedded debug/assert path strings
differ, and V3's are the **relocatable** (relative) form. This is exactly the path
normalisation the existing preprocessed grid path already performs (via
`-ffile-prefix-map`), so a V3 object is as relocatable and deterministic as any grid
object today — it is not a miscompile.

## 2c. Windows→Linux cross-compile — VERIFIED on real PE/COFF (Antigravity, 2026-07-27)

Done from a real Windows 11 client (`suco-cl++.exe`, MinGW 13.1.0) cross-dispatching to Linux
worker node3 (`x86_64-w64-mingw32-g++` GCC 13.2) — the check the Linux side cannot perform
(see §2b). Results reported on issue #42:

**Broad sweep:** 15 compilations — 3 TUs (`math_utils.cpp`, `string_utils.cpp`,
`complex_template.cpp`) × 5 flag sets (`-O0`, `-O2`, `-O3`, `-g`, `-DNDEBUG -O2`).
**13/15 byte-identical to a plain native MinGW compile**; all 15 valid `PE-x86-64` objects.

**The 2 non-identical cases, fully explained** (`math_utils.cpp` at `-O2` and at `-DNDEBUG -O2`):

| Check | Result |
| :--- | :--- |
| `.text` machine code (V3 vs baseline) | **byte-identical** (544 B, both cases) |
| Object size | exact match (1676 B / 1700 B) |
| V3 vs normal grid (`SUCO_REMOTE_PREPROCESS=0`) | exact match (case 2, compared directly) |
| V3 determinism (pass 1 vs pass 2) | **100 % identical** — the cache-critical property |
| Cause of the byte delta | path-string normalisation only |

Same pattern as the Linux/fmt result in §2: **identical machine code, delta confined to
relocatable path strings, fully deterministic.** Two agents, two platforms, one conclusion.

**Real project:** the 101-TU Windows benchmark suite built with `SUCO_REMOTE_PREPROCESS=1`
via Ninja, linked natively into `suco_large_bench_app.exe`, and **ran correctly**
(`Result: 14630 in 13.34 ms`).

*(Reporting nit for the record: AG's summary framed a "24-byte delta (1700 B vs 1676 B)" as the
V3-vs-native difference; those two numbers are actually the `-O2` vs `-DNDEBUG -O2` objects — two
different compilations. The per-case data above is what matters and is internally consistent:
each case matched its baseline in size, with `.text` identical.)*

## 2b. How the earlier Linux-side "cross-compile" attempt was wrong (kept as a lesson)

**Correction (honest):** an initial attempt to sweep the MinGW cross-target on the Linux
grid host was flawed. Invoking `suco-cl++ x86_64-w64-mingw32-g++ …` on **Linux** is not a
recognised cross-compile — the wrapper defaults to the local `g++` (Linux target), so both
the grid and V3 produced **ELF** objects, while a plain `x86_64-w64-mingw32-g++` produced
**PE/COFF**. Section analysis (`.eh_frame`/`.note.GNU-stack` vs `.pdata`/`.xdata`) and `file`
(ELF vs PE) confirmed this. So "V3 == grid" there was a *Linux* comparison, not a
Windows-target one, and the earlier "grid differs from native" was just ELF-vs-PE (see
closed issue #67).

What IS true:
- **A real V3 bug was found and fixed (#66):** the V3 base command used `cmd.compiler_path`
  (the local compiler) instead of `cmd.get_remote_compiler_name()` — the target-qualified
  name the normal path uses. For a real MinGW cross-target these differ, and using the
  resolved remote name is correct **by construction** (V3 now mirrors the proven normal
  dispatch path).
- V3 and the normal grid path produce identical objects for the same client invocation
  (verified on Linux) — the shared cache-store/response tail is unchanged.

**Since verified:** the true Windows→Linux PE/COFF cross-compile was done from a real Windows
client by Antigravity — see §2c above.

## 3. Determinism (the cache invariant)

V3 objects are deterministic across workers/runs (different workspace temp dirs):
`-ffile-prefix-map=<workspace>=.` normalises the per-job path out. Verified directly
(docs/remote_preprocessing_impl.md §4a) and continuously in CI — the smoke test with
`SUCO_REMOTE_PREPROCESS=1` asserts 7/7 coordinator cache hits on recompile (a hit requires
byte-identical objects between passes) and that the worker actually ran V3 jobs.

## 4. The client-CPU benefit (the point of the feature)

Measured aggregate client CPU (`user+sys`, `/usr/bin/time`) over 35 header-heavy TUs,
loopback grid, normal path vs V3:

| Path | Client CPU (35 TUs) |
| :--- | :--- |
| Normal (local `-E` preprocessing) | **4.79 s** |
| V3 (skip `-E`; cheap `-MM` scan + bundle) | **2.79 s** |
| | **−41.8 % (1.72× less client CPU)** |

The client offloads the expensive `-E` expansion to the worker and only runs the light
`-MM` dependency scan + bundle pack locally. The win **scales with header weight** — the
heavier the includes (templates, big STL/third-party headers, PCH-style umbrella headers),
the larger the `-E` this avoids; light TUs benefit less. ~42 % here is a representative
mid-weight figure, not a floor or ceiling.

## Conclusion

- **Path-insensitive code:** V3 is bit-for-bit identical to a native compile (240/240).
- **Path-embedding code:** V3 emits identical machine code with normalised, relocatable
  paths — the same trade-off the existing grid already makes, and deterministic/cacheable.
- V3 never miscompiles: the `.text` matched in every case examined; correctness guards
  send `__DATE__`/`__TIME__`/module TUs to local preprocessing.

**Readiness — the bar is met; `SUCO_REMOTE_PREPROCESS` defaults ON as of #74.** Both
verification tracks agree, independently:

| Track | Evidence |
| :--- | :--- |
| Linux (Claude) | 240/240 byte-identical native (`-O0/-O2/-O3`); fmt `.text` identical |
| Windows→Linux PE/COFF (Antigravity) | 13/15 identical, the 2 explained (`.text` identical, == normal grid, deterministic) |
| Real projects | 101-TU Windows suite builds/links/**runs**; loopback smoke 7/7 cache hits |
| CI | both paths pinned — `SUCO_REMOTE_PREPROCESS=1` **and** `=0` (#79) |
| Safety net | time-macro / C++20-module / unreadable-header TUs fall back to local preprocessing |

Escape hatch: `SUCO_REMOTE_PREPROCESS=0` restores the classic local-preprocessing path
(CI-tested, so it stays working).

Not required for correctness, still open: **bundle dedup** via the blob cache. Analysis
suggests it is low-value as designed — a bundle is one TU's full `-MM` project-header set, so
bundle hashes rarely repeat across TUs (poor hit rate) and dedup adds cold-build round-trips;
header-*level* dedup would be the real win, but that is a larger redesign.
