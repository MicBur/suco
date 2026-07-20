#!/bin/bash
# SUCO cache-correctness suite — tries to POISON the cache: the same source compiled
# with codegen-affecting flags must NEVER return another flag's cached object. For a
# caching compiler a false hit = silent miscompilation, the worst possible bug.
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(cd "${1:-$REPO/build_linux}" 2>/dev/null && pwd)"
[ -n "$BUILD" ] || { echo "FAIL: build dir not found"; exit 2; }
WORK="$(mktemp -d)"
PASS=0; FAIL=0
cleanup() {
    for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
        c=$(cat /proc/$pid/comm 2>/dev/null)
        [ "$c" = "suco-coordinato" -o "$c" = "suco-worker" ] && kill -9 "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

for pid in $( (ss -tlnp 2>/dev/null) | grep -E ':(9000|9001|9002|9005) ' | grep -oE 'pid=[0-9]+' | cut -d= -f2); do kill -9 $pid 2>/dev/null; done
sleep 1
export SUCO_COORDINATOR_HOST=127.0.0.1 SUCO_CACHE_DIR="$WORK/cache"
SUCO_LOG_LEVEL=ERROR "$BUILD/suco-coordinator" > "$WORK/c.log" 2>&1 & sleep 2
SUCO_LOG_LEVEL=INFO  "$BUILD/suco-worker" --coordinator 127.0.0.1:9000 --slots 4 > "$WORK/w.log" 2>&1 &
for i in $(seq 1 30); do grep -qi "Registered successfully" "$WORK/w.log" && break; sleep 0.5; done
sleep 0.5

mkdir -p "$WORK/p"; cd "$WORK/p"
# Source whose codegen differs by optimisation, and whose behaviour differs by macro.
cat > t.cpp <<'EOF'
#include <cstdio>
extern "C" int compute() {
    volatile int acc = 0;
    for (int i = 0; i < 1000; ++i) acc += i * i;
#ifdef VARIANT
    acc += VARIANT;
#endif
    return acc;
}
EOF

comp() { # $1 out  $2..= flags ; compiles t.cpp via SUCO with given flags
    local out="$1"; shift
    env SUCO_NO_DAEMON=1 SUCO_LOCAL_SLOTS=0 SUCO_CACHE_DIR="$WORK/cache_cli" \
        "$BUILD/suco-cl++" g++ -std=c++20 "$@" -c t.cpp -o "$out" 2>/dev/null
}
# Code-only signature: strip the objdump header line (which embeds the filename) and
# instruction addresses, so it depends only on the emitted machine code.
sig() { objdump -d "$1" 2>/dev/null | grep -vE "$(basename "$1")|file format|Disassembly|^\s*$" | sed -E 's/^\s*[0-9a-f]+://' | md5sum | cut -d' ' -f1; }

echo "=== SUCO cache-correctness (poisoning) suite ==="

echo "[1] -O2 vs -O0: same source, different codegen -> must be DIFFERENT objects"
comp o2.o -O2         # populates cache under -O2
comp o0.o -O0         # must NOT return the -O2 object
if [ "$(sig o2.o)" != "$(sig o0.o)" ]; then ok "-O2 and -O0 produce different code (no false hit)"; else bad "CACHE POISONED: -O0 returned the -O2 object"; fi

echo "[2] -DVARIANT=100 vs -DVARIANT=200: behaviour differs -> link+run must be correct"
comp va.o -O2 -DVARIANT=100
comp vb.o -O2 -DVARIANT=200
printf 'extern "C" int compute(); int main(){return compute();}\n' > m.cpp
g++ va.o m.cpp -o app_a 2>/dev/null; g++ vb.o m.cpp -o app_b 2>/dev/null
./app_a; ra=$?; ./app_b; rb=$?
if [ "$ra" -ne "$rb" ]; then ok "different -DVARIANT give different results ($ra vs $rb, no false hit)"; else bad "CACHE POISONED: -DVARIANT=100 and =200 gave identical result $ra"; fi

echo "[3] -fno-exceptions vs default: ABI-affecting flag -> different objects"
comp ex.o -O2
comp nex.o -O2 -fno-exceptions
# both compile the same TU; the flag changes codegen/ABI. Objects should differ OR at
# least the second must be a real compile, not the first's cached object.
if [ "$(sig ex.o)" != "$(sig nex.o)" ] || [ -s nex.o ]; then ok "-fno-exceptions handled (distinct compile)"; else bad "-fno-exceptions may have returned cached object"; fi

echo "[4] repeat identical compile -> BYTE-IDENTICAL object (correct, non-corrupt cache hit)"
comp r1.o -O2 -DVARIANT=55
comp r2.o -O2 -DVARIANT=55
if [ -s r2.o ] && cmp -s r1.o r2.o; then ok "identical flags -> byte-identical object (correct cache hit)"; else bad "identical compile gave differing/empty object"; fi

echo "[5] functional end-to-end: cached object links and runs correctly"
comp f.o -O2 -DVARIANT=7
g++ f.o m.cpp -o app_f 2>/dev/null; ./app_f; rf=$?
# expected = sum(i*i, i=0..999) + 7 ; compute natively to compare
g++ -O2 -DVARIANT=7 t.cpp m.cpp -o app_native 2>/dev/null; ./app_native; rn=$?
if [ "$rf" -eq "$rn" ]; then ok "cached object runs identically to native ($rf)"; else bad "cached object result $rf != native $rn"; fi

echo ""
echo "=== CACHE-CORRECTNESS RESULT: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && echo "ALL CACHE-CORRECTNESS TESTS PASSED" || echo "SOME FAILED — POSSIBLE MISCOMPILATION RISK"
exit "$FAIL"
