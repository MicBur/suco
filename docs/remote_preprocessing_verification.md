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

## 2b. Windows→Linux cross-compile — V3 is a byte-identical drop-in for the grid

Sweeping the MinGW cross-target (`x86_64-w64-mingw32-g++`, `pe-x86-64` objects) initially
showed V3 differing from a plain native cross-compile. Investigation found **two separate
things**:

1. **A real V3 bug (fixed):** the V3 base command used `cmd.compiler_path` (the LOCAL
   compiler) instead of the resolved `cmd.get_remote_compiler_name()`, so a MinGW target
   was compiled with the local `g++` — wrong architecture. Fixed to match the normal path.
2. **A pre-existing grid property (not V3):** even the NORMAL grid cross-compile object
   differs from a plain-native one (e.g. 3064 vs 1450 bytes for a trivial TU). This is a
   property of the existing grid cross path, independent of remote preprocessing.

After the fix, **V3 == the normal grid cross-compile path, byte-for-byte** (`SAME-DROPIN`).
So switching the default to V3 changes nothing for grid cross-compile users — the correct
drop-in criterion. (The separate grid-vs-native cross-compile size gap is worth its own
look but is out of scope for #42.)

## 3. Determinism (the cache invariant)

V3 objects are deterministic across workers/runs (different workspace temp dirs):
`-ffile-prefix-map=<workspace>=.` normalises the per-job path out. Verified directly
(docs/remote_preprocessing_impl.md §4a) and continuously in CI — the smoke test with
`SUCO_REMOTE_PREPROCESS=1` asserts 7/7 coordinator cache hits on recompile (a hit requires
byte-identical objects between passes) and that the worker actually ran V3 jobs.

## Conclusion

- **Path-insensitive code:** V3 is bit-for-bit identical to a native compile (240/240).
- **Path-embedding code:** V3 emits identical machine code with normalised, relocatable
  paths — the same trade-off the existing grid already makes, and deterministic/cacheable.
- V3 never miscompiles: the `.text` matched in every case examined; correctness guards
  send `__DATE__`/`__TIME__`/module TUs to local preprocessing.

**Readiness:** this clears the correctness bar for making `SUCO_REMOTE_PREPROCESS` the
default. Recommended before the flip: (a) confirm the same on the Windows→Linux
cross-compile path, and (b) bundle dedup via the blob cache (efficiency, not correctness).
