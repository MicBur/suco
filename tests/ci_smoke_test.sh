#!/bin/bash
# SUCO CI smoke test — fully self-contained, repo-relative, no external grid needed.
# Starts coordinator + worker on localhost, compiles a small generated project
# through suco-cl++ (grid loopback), verifies objects, then proves a coordinator
# cache hit on the second pass. Designed for GitHub Actions runners.
#
# Usage: tests/ci_smoke_test.sh [BUILD_DIR]   (default: <repo>/build)
set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$(cd "${1:-$REPO_ROOT/build}" && pwd)"
WORK_DIR="$(mktemp -d)"
COORD_PID=""
WORKER_PID=""

# Per-compilation timeout (seconds). GH Actions runners are slow; allow generous time.
COMPILE_TIMEOUT=120

cleanup() {
    # Use SIGKILL — graceful SIGTERM can hang in CI and block the step forever.
    [ -n "$WORKER_PID" ] && kill -9 "$WORKER_PID" 2>/dev/null || true
    [ -n "$COORD_PID" ] && kill -9 "$COORD_PID" 2>/dev/null || true
    # Wait only on our own PIDs (not all children), with a 5s fallback.
    { wait "$WORKER_PID" "$COORD_PID" 2>/dev/null; } &
    WAIT_PID=$!
    ( sleep 5; kill -9 $WAIT_PID 2>/dev/null ) &
    wait $WAIT_PID 2>/dev/null || true
    echo "--- coordinator.log (tail) ---"; tail -30 "$WORK_DIR/coordinator.log" 2>/dev/null || true
    echo "--- worker.log (tail) ---"; tail -30 "$WORK_DIR/worker.log" 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

for bin in suco-coordinator suco-worker suco-cl++; do
    if [ ! -x "$BUILD_DIR/$bin" ]; then
        echo "FAIL: $BUILD_DIR/$bin not found or not executable"; exit 1
    fi
done

# Deterministic environment: explicit coordinator, no daemon, no local slots
# (forces the grid + cache path so the cache-hit assertion is meaningful).
export SUCO_COORDINATOR_HOST=127.0.0.1
export SUCO_NO_DAEMON=1
export SUCO_LOCAL_SLOTS=0
export SUCO_CACHE_DIR="$WORK_DIR/cache"
export SUCO_LOG_LEVEL=INFO

echo "=== Starting coordinator + worker (localhost) ==="
"$BUILD_DIR/suco-coordinator" > "$WORK_DIR/coordinator.log" 2>&1 &
COORD_PID=$!
sleep 2

# Verify coordinator is listening before starting worker
kill -0 "$COORD_PID" || { echo "FAIL: coordinator died on startup"; exit 1; }

"$BUILD_DIR/suco-worker" --coordinator 127.0.0.1:9000 --slots 2 > "$WORK_DIR/worker.log" 2>&1 &
WORKER_PID=$!

# Wait for worker registration — poll coordinator log for up to 30 seconds
echo "Waiting for worker registration..."
for attempt in $(seq 1 30); do
    if grep -q -i "Worker registered" "$WORK_DIR/coordinator.log" 2>/dev/null; then
        echo "Worker registered after ${attempt}s"
        break
    fi
    if [ "$attempt" -eq 30 ]; then
        echo "FAIL: worker did not register within 30s"
        echo "--- coordinator.log ---"; cat "$WORK_DIR/coordinator.log"
        echo "--- worker.log ---"; cat "$WORK_DIR/worker.log"
        exit 1
    fi
    sleep 1
done

kill -0 "$WORKER_PID" || { echo "FAIL: worker died after startup"; exit 1; }

echo "=== Generating test project (5 classes) ==="
SRC_DIR="$WORK_DIR/proj"
mkdir -p "$SRC_DIR"
for i in 1 2 3 4 5; do
    cat > "$SRC_DIR/class$i.cpp" <<EOF
#include <string>
#include <vector>
namespace t { int value$i(const std::vector<std::string>& v) { return static_cast<int>(v.size()) + $i; } }
EOF
done
cat > "$SRC_DIR/main.cpp" <<'EOF'
#include <iostream>
#include <string>
#include <vector>
namespace t {
    int value1(const std::vector<std::string>&); int value2(const std::vector<std::string>&);
    int value3(const std::vector<std::string>&); int value4(const std::vector<std::string>&);
    int value5(const std::vector<std::string>&);
}
int main() {
    std::vector<std::string> v{"a","b"};
    std::cout << t::value1(v)+t::value2(v)+t::value3(v)+t::value4(v)+t::value5(v) << std::endl;
    return 0;
}
EOF

echo "=== Pass 1: compile via suco-cl++ (expect cache misses) ==="
cd "$SRC_DIR"
for f in class1 class2 class3 class4 class5 main; do
    timeout "$COMPILE_TIMEOUT" "$BUILD_DIR/suco-cl++" g++ -std=c++20 -O1 -c "$f.cpp" -o "$f.o" 2>> "$WORK_DIR/pass1.log" \
        || { echo "FAIL: suco-cl++ timed out or failed for $f.cpp (exit $?)"; cat "$WORK_DIR/pass1.log"; exit 1; }
    [ -s "$f.o" ] || { echo "FAIL: $f.o missing/empty in pass 1"; exit 1; }
done
echo "Pass 1 OK (all objects produced)"

echo "=== Link & run ==="
g++ class1.o class2.o class3.o class4.o class5.o main.o -o smoke_app
OUT="$(./smoke_app)"
[ "$OUT" = "25" ] || { echo "FAIL: expected output 25, got '$OUT' (wrong/corrupt objects?)"; exit 1; }
echo "Binary runs correctly (output: $OUT)"

echo "=== Pass 2: recompile (expect coordinator cache hits) ==="
rm -f ./*.o
for f in class1 class2 class3 class4 class5 main; do
    timeout "$COMPILE_TIMEOUT" "$BUILD_DIR/suco-cl++" g++ -std=c++20 -O1 -c "$f.cpp" -o "$f.o" 2>> "$WORK_DIR/pass2.log" \
        || { echo "FAIL: suco-cl++ timed out or failed for $f.cpp in pass 2 (exit $?)"; cat "$WORK_DIR/pass2.log"; exit 1; }
    [ -s "$f.o" ] || { echo "FAIL: $f.o missing/empty in pass 2"; exit 1; }
done
HITS="$(grep -c -i "cache hit" "$WORK_DIR/pass2.log" || true)"
echo "Cache hits in pass 2: $HITS / 6"
[ "$HITS" -ge 1 ] || { echo "FAIL: no cache hits on identical recompile"; cat "$WORK_DIR/pass2.log"; exit 1; }

echo "=== Services still alive? ==="
kill -0 "$COORD_PID" || { echo "FAIL: coordinator crashed during test"; exit 1; }
kill -0 "$WORKER_PID" || { echo "FAIL: worker crashed during test"; exit 1; }

echo "============================================="
echo "SMOKE TEST PASSED (objects OK, binary OK, $HITS cache hits, no crashes)"
echo "============================================="
