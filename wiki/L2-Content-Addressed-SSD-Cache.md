# 💾 L2 Content-Addressed SSD Cache

SUCO Grid features a **global, team-wide Content-Addressed L2 SSD Cache**.

Unlike Icecream or distcc, which re-run compiler passes on unchanged files, SUCO stores preprocessed SHA-256 object hashes in a central SQLite database backed by zstd compression.

---

## 🚀 Speedup Performance

- **Cold Build**: 3.83x Speedup vs Native Local Build
- **Warm Rebuild**: **19x Speedup** (342 translation units served in **24.7 seconds**)

---

## 🔒 Byte-Identity Invariants

SUCO enforces strict byte-identity rules:
1. **Source Normalization**: Strips comments, volatile whitespace, and line markers for hash keys.
2. **Compiler Flags**: Normalizes include paths while preserving execution order.
3. ** provenance Isolation**: Builds inside per-job temporary subdirectories to prevent path leakage in DWARF debug symbols.

---

## 🧼 Clearing Caches

To clear local and remote grid caches:

```bash
suco cache clear
```
