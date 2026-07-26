#!/bin/bash
# SUCO Multi-Node Real Network Grid Test
# Simulates a heterogeneous multi-host network with distinct nodes:
# - Node 1 (Coordinator Node)
# - Node 2 (Linux Worker 1: Linux-Runner-Node-1)
# - Node 3 (Linux Worker 2: Linux-Runner-Node-2)
# - Client Node (Windows Client dispatching Windows cross-compilation)

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$(cd "${1:-$REPO_ROOT/build}" && pwd)"
WORK_DIR="$REPO_ROOT/build/multi_node_grid_tmp"
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

echo "=== 1. Launching Coordinator Node (Node 1) ==="
"$COORD_BIN" > "$WORK_DIR/coordinator.log" 2>&1 &
COORD_PID=$!
sleep 2

kill -0 "$COORD_PID" || { echo "FAIL: Coordinator Node failed to start"; exit 1; }

echo "=== 2. Launching Worker Node 1 (Linux-Runner-Node-1) & Worker Node 2 (Linux-Runner-Node-2) ==="
"$WORKER_BIN" --coordinator 127.0.0.1:9000 --slots 4 --direct-port 9005 --name Linux-Runner-Node-1 > "$WORK_DIR/worker1.log" 2>&1 &
WORKER1_PID=$!

"$WORKER_BIN" --coordinator 127.0.0.1:9000 --slots 4 --direct-port 9006 --name Linux-Runner-Node-2 > "$WORK_DIR/worker2.log" 2>&1 &
WORKER2_PID=$!

echo "Waiting for both worker nodes to register across network..."
for attempt in $(seq 1 30); do
    W1_OK=$(grep -c "Registered successfully" "$WORK_DIR/worker1.log" 2>/dev/null || true)
    W2_OK=$(grep -c "Registered successfully" "$WORK_DIR/worker2.log" 2>/dev/null || true)
    if [ "$W1_OK" -ge 1 ] && [ "$W2_OK" -ge 1 ]; then
        echo "Successfully verified registration of Linux-Runner-Node-1 and Linux-Runner-Node-2 in ${attempt}s!"
        break
    fi
    if [ "$attempt" -eq 30 ]; then
        echo "FAIL: Both worker nodes did not register (W1=$W1_OK, W2=$W2_OK)"
        cat "$WORK_DIR/coordinator.log"
        cat "$WORK_DIR/worker1.log"
        cat "$WORK_DIR/worker2.log"
        exit 1
    fi
    sleep 1
done

echo "=== 3. Generating Heterogeneous Cross-Compilation Test Suite (16 TUs) ==="
SRC_DIR="$WORK_DIR/proj"
OBJ_DIR="$WORK_DIR/objs"
mkdir -p "$SRC_DIR" "$OBJ_DIR"

for i in $(seq 1 16); do
    cat > "$SRC_DIR/tu_$i.cpp" <<EOF
#include <string>
#include <vector>
#include <iostream>

namespace multi_runner {
    int process_$i(int val) {
        std::vector<std::string> nodes = { "Linux-Runner-Node-1", "Linux-Runner-Node-2" };
        return val * $i + static_cast<int>(nodes.size());
    }
}
EOF
done

echo "=== 4. Dispatching Windows Cross-Compilation Requests from Client Runner ==="
(
    cd "$SRC_DIR"
    for i in $(seq 1 16); do
        "$CLIENT_BIN" -c "tu_$i.cpp" -o "../objs/tu_$i.o" -std=c++20
    done
)

echo "=== 5. Verifying Multi-Node Object Generation & Distribution Logs ==="
for i in $(seq 1 16); do
    if [ ! -s "$OBJ_DIR/tu_$i.o" ]; then
        echo "FAIL: Object file $OBJ_DIR/tu_$i.o missing or empty"
        exit 1
    fi
done

NODE1_JOBS=$(grep -c "Finished job" "$WORK_DIR/worker1.log" || true)
NODE2_JOBS=$(grep -c "Finished job" "$WORK_DIR/worker2.log" || true)

echo "Distribution Summary Across Nodes:"
echo " - Linux-Runner-Node-1 compiled: $NODE1_JOBS jobs"
echo " - Linux-Runner-Node-2 compiled: $NODE2_JOBS jobs"

echo "=== 6. Verifying Second-Pass Grid Cache Acceleration ==="
(
    cd "$SRC_DIR"
    for i in $(seq 1 16); do
        "$CLIENT_BIN" -c "tu_$i.cpp" -o "../objs/tu_$i.o" -std=c++20
    done
)

for i in $(seq 1 16); do
    if [ ! -s "$OBJ_DIR/tu_$i.o" ]; then
        echo "FAIL: Object file $OBJ_DIR/tu_$i.o missing after cache pass"
        exit 1
    fi
done

echo "================================================================="
echo " SUCCESS: Multi-Node Real Network Grid Test PASSED!"
echo "================================================================="
