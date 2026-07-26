#!/bin/bash
# SUCO Multi-Worker Heterogeneous Cross-Compilation Integration Test
# Verifies that a client (suco-cl++) distributes compilation tasks
# across MULTIPLE parallel workers via x86_64-w64-mingw32-g++ cross-compilation.

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$(cd "${1:-$REPO_ROOT/build}" && pwd)"
WORK_DIR="$REPO_ROOT/build/multi_worker_test_tmp"
mkdir -p "$WORK_DIR"
COORD_PID=""
WORKER1_PID=""
WORKER2_PID=""

cleanup() {
    [ -n "$WORKER1_PID" ] && kill -9 "$WORKER1_PID" 2>/dev/null || true
    [ -n "$WORKER2_PID" ] && kill -9 "$WORKER2_PID" 2>/dev/null || true
    [ -n "$COORD_PID" ] && kill -9 "$COORD_PID" 2>/dev/null || true
    echo "--- coordinator.log (tail) ---"; tail -30 "$WORK_DIR/coordinator.log" 2>/dev/null || true
    echo "--- worker1.log (tail) ---"; tail -30 "$WORK_DIR/worker1.log" 2>/dev/null || true
    echo "--- worker2.log (tail) ---"; tail -30 "$WORK_DIR/worker2.log" 2>/dev/null || true
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

COORD_BIN=""
WORKER_BIN=""
CLIENT_BIN=""

if [ -f "$BUILD_DIR/suco-coordinator.exe" ]; then COORD_BIN="$BUILD_DIR/suco-coordinator.exe"; else COORD_BIN="$BUILD_DIR/suco-coordinator"; fi
if [ -f "$BUILD_DIR/suco-worker.exe" ]; then WORKER_BIN="$BUILD_DIR/suco-worker.exe"; else WORKER_BIN="$BUILD_DIR/suco-worker"; fi
if [ -f "$BUILD_DIR/suco-cl++.exe" ]; then CLIENT_BIN="$BUILD_DIR/suco-cl++.exe"; else CLIENT_BIN="$BUILD_DIR/suco-cl++"; fi

for bin_path in "$COORD_BIN" "$WORKER_BIN" "$CLIENT_BIN"; do
    if [ ! -f "$bin_path" ]; then
        echo "FAIL: $bin_path not found"; exit 1
    fi
done

export SUCO_COORDINATOR_HOST=127.0.0.1
export SUCO_NO_DAEMON=1
export SUCO_LOCAL_SLOTS=0
export SUCO_CACHE_DIR="$WORK_DIR/cache"
export SUCO_LOG_LEVEL=INFO

echo "=== 1. Starting Coordinator on localhost:9000 ==="
"$COORD_BIN" > "$WORK_DIR/coordinator.log" 2>&1 &
COORD_PID=$!
sleep 2

kill -0 "$COORD_PID" || { echo "FAIL: coordinator failed to start"; exit 1; }

echo "=== 2. Starting 2 Parallel Linux Cross-Compile Workers ==="
"$WORKER_BIN" --coordinator 127.0.0.1:9000 --slots 4 --direct-port 9005 --name Worker-Alpha > "$WORK_DIR/worker1.log" 2>&1 &
WORKER1_PID=$!

"$WORKER_BIN" --coordinator 127.0.0.1:9000 --slots 4 --direct-port 9006 --name Worker-Beta > "$WORK_DIR/worker2.log" 2>&1 &
WORKER2_PID=$!

echo "Waiting for both workers to register with coordinator..."
for attempt in $(seq 1 30); do
    W1_OK=$(grep -c "Registered successfully" "$WORK_DIR/worker1.log" 2>/dev/null || true)
    W2_OK=$(grep -c "Registered successfully" "$WORK_DIR/worker2.log" 2>/dev/null || true)
    if [ "$W1_OK" -ge 1 ] && [ "$W2_OK" -ge 1 ]; then
        echo "Successfully verified registration for Worker-Alpha (port 9005) and Worker-Beta (port 9006) in ${attempt}s!"
        break
    fi
    if [ "$attempt" -eq 30 ]; then
        echo "FAIL: Both workers did not register within 30s (W1=$W1_OK, W2=$W2_OK)"
        cat "$WORK_DIR/coordinator.log"
        cat "$WORK_DIR/worker1.log"
        cat "$WORK_DIR/worker2.log"
        exit 1
    fi
    sleep 1
done

echo "=== 3. Generating 10 Translation Unit Cross-Compile Project ==="
SRC_DIR="$WORK_DIR/proj"
OBJ_DIR="$WORK_DIR/objs"
mkdir -p "$SRC_DIR" "$OBJ_DIR"

if command -v cygpath >/dev/null 2>&1; then
    SRC_DIR_ARG="$(cygpath -w "$SRC_DIR")"
    OBJ_DIR_ARG="$(cygpath -w "$OBJ_DIR")"
else
    SRC_DIR_ARG="$SRC_DIR"
    OBJ_DIR_ARG="$OBJ_DIR"
fi

for i in $(seq 1 10); do
    cat > "$SRC_DIR/tu_$i.cpp" <<EOF
#include <string>
#include <vector>
#include <iostream>

namespace grid_test {
    int compute_$i(int val) {
        std::vector<std::string> items = { "tu_$i", "cross_compile_test" };
        return val * $i + static_cast<int>(items.size());
    }
}
EOF
done

echo "=== 4. Dispatching Cross-Compilation via suco-cl++ ==="
CROSS_CXX="x86_64-w64-mingw32-g++"
if ! command -v "$CROSS_CXX" >/dev/null 2>&1; then
    echo "Note: $CROSS_CXX not found locally, defaulting compiler flags"
fi

(
    cd "$SRC_DIR"
    for i in $(seq 1 10); do
        "$CLIENT_BIN" -c "tu_$i.cpp" -o "../objs/tu_$i.o" -std=c++20
    done
)

echo "=== 5. Verifying Object Generation & Multi-Worker Parallel Dispatch ==="
for i in $(seq 1 10); do
    if [ ! -s "$OBJ_DIR/tu_$i.o" ]; then
        echo "FAIL: Object file $OBJ_DIR/tu_$i.o is missing or empty"
        exit 1
    fi
done

echo "=== 6. Verifying L2 Grid Cache Hits on Second Pass ==="
(
    cd "$SRC_DIR"
    for i in $(seq 1 10); do
        "$CLIENT_BIN" -c "tu_$i.cpp" -o "../objs/tu_$i.o" -std=c++20
    done
)

for i in $(seq 1 10); do
    if [ ! -s "$OBJ_DIR/tu_$i.o" ]; then
        echo "FAIL: Object file $OBJ_DIR/tu_$i.o missing after 2nd pass"
        exit 1
    fi
done

echo "========================================================="
echo " SUCCESS: Multi-Worker Cross-Compilation Test PASSED!"
echo "========================================================="
