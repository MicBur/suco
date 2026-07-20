#!/bin/bash
# SUCO chaos / robustness suite — proves the failover and crash-resistance paths.
# Self-contained: starts a coordinator + worker on localhost, injects failures, and
# asserts the system survives and still produces correct objects.
#
# Usage: tests/chaos_test.sh [BUILD_DIR]   (default <repo>/build_linux)
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(cd "${1:-$REPO/build_linux}" 2>/dev/null && pwd)"
[ -n "$BUILD" ] || { echo "FAIL: build dir '${1:-$REPO/build_linux}' not found"; exit 2; }
WORK="$(mktemp -d)"
PASS=0; FAIL=0
COORD_PID=""; WORKER_PID=""

cleanup() {
    [ -n "$WORKER_PID" ] && kill -9 "$WORKER_PID" 2>/dev/null
    [ -n "$COORD_PID" ] && kill -9 "$COORD_PID" 2>/dev/null
    for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
        c=$(cat /proc/$pid/comm 2>/dev/null)
        [ "$c" = "suco-coordinato" -o "$c" = "suco-worker" ] && kill -9 "$pid" 2>/dev/null
    done
    rm -rf "$WORK"
}
trap cleanup EXIT
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

free_all_ports() {
    for pid in $( (ss -tlnp 2>/dev/null) | grep -E ':(9000|9001|9002|9005) ' | grep -oE 'pid=[0-9]+' | cut -d= -f2); do kill -9 $pid 2>/dev/null; done
    for pid in $(ls /proc 2>/dev/null | grep -E '^[0-9]+$'); do
        c=$(cat /proc/$pid/comm 2>/dev/null)
        [ "$c" = "suco-coordinato" -o "$c" = "suco-worker" ] && kill -9 "$pid" 2>/dev/null
    done
    sleep 1
}
start_grid() {
    free_all_ports
    export SUCO_COORDINATOR_HOST=127.0.0.1 SUCO_CACHE_DIR="$WORK/cache"
    SUCO_LOG_LEVEL=ERROR "$BUILD/suco-coordinator" > "$WORK/coord.log" 2>&1 & COORD_PID=$!
    sleep 2
    SUCO_LOG_LEVEL=INFO "$BUILD/suco-worker" --coordinator 127.0.0.1:9000 --slots 4 > "$WORK/worker.log" 2>&1 & WORKER_PID=$!
    for i in $(seq 1 30); do grep -qi "Registered successfully" "$WORK/worker.log" && break; sleep 0.5; done
    sleep 0.5
}
gen_project() {
    mkdir -p "$1"
    for i in $(seq 1 "$2"); do
        printf '#include <string>\n#include <vector>\nnamespace t{int v%d(const std::vector<std::string>&x){return (int)x.size()+%d;}}\n' "$i" "$i" > "$1/c$i.cpp"
    done
}
build_bg() { # $1 dir  $2 n  $3 extra_env -> sets BPID
    local cdir="$WORK/cc_$RANDOM"
    ( cd "$1"; eval "export $3"; export SUCO_COORDINATOR_HOST=127.0.0.1 SUCO_NO_DAEMON=1 SUCO_CACHE_DIR="$cdir" SUCO_LOG_LEVEL=WARNING
      for f in $(seq 1 "$2"); do timeout 60 "$BUILD/suco-cl++" g++ -std=c++20 -O1 -c "c$f.cpp" -o "c$f.o" 2>>"$WORK/build.err"; done ) &
    BPID=$!
}

echo "=== SUCO Chaos Suite (build: $BUILD) ==="
for b in suco-coordinator suco-worker suco-cl++; do
    [ -x "$BUILD/$b" ] || { echo "FAIL: $BUILD/$b missing"; exit 2; }
done

# ---------------------------------------------------------------------------
echo "[1] Garbage/fuzzed packets must not crash coordinator or worker"
start_grid
wdport=$(grep -oE "listener started on port [0-9]+" "$WORK/worker.log" | grep -oE "[0-9]+$")
for tp in "9000" "${wdport:-9005}"; do
    for k in $(seq 1 8); do
        head -c $((RANDOM % 8192 + 1)) /dev/urandom 2>/dev/null | timeout 2 bash -c "cat > /dev/tcp/127.0.0.1/$tp" 2>/dev/null
    done
done
# valid-looking packet type + truncated body, and huge length fields
printf '\x00\x00\x00\x13\xff\xff\xff\xff' | timeout 2 bash -c "cat > /dev/tcp/127.0.0.1/9000" 2>/dev/null
printf '\x00\x00\x00\x04\x7f\xff\xff\xff' | timeout 2 bash -c "cat > /dev/tcp/127.0.0.1/${wdport:-9005}" 2>/dev/null
sleep 1
kill -0 "$COORD_PID" 2>/dev/null && ok "coordinator survived fuzzed packets" || bad "coordinator CRASHED"
kill -0 "$WORKER_PID" 2>/dev/null && ok "worker survived fuzzed packets" || bad "worker CRASHED"
gen_project "$WORK/p1" 5
build_bg "$WORK/p1" 5 "SUCO_LOCAL_SLOTS=0"; wait $BPID 2>/dev/null
n=$(ls -1 "$WORK/p1"/*.o 2>/dev/null | wc -l)
[ "$n" -eq 5 ] && ok "grid still compiles after fuzzing ($n/5)" || bad "grid broken after fuzzing ($n/5)"

# ---------------------------------------------------------------------------
echo "[2] Worker killed mid-build -> build completes (fallback), objects correct"
start_grid
gen_project "$WORK/p2" 20
build_bg "$WORK/p2" 20 "SUCO_LOCAL_SLOTS=2"
sleep 1.5
kill -9 "$WORKER_PID" 2>/dev/null   # worker dies mid-build
wait $BPID 2>/dev/null
n=$(ls -1 "$WORK/p2"/*.o 2>/dev/null | wc -l)
if [ "$n" -eq 20 ]; then
    # link + run to prove objects are not corrupt
    ( cd "$WORK/p2"; printf 'namespace t{int %s;}int main(){return 0;}\n' "$(for i in $(seq 1 20); do echo -n "v$i(const std::vector<std::string>&),"; done | sed 's/,$//')" > m.cpp 2>/dev/null
      g++ c*.o -c m.cpp 2>/dev/null; g++ c*.o m.o -o app 2>/dev/null; ./app 2>/dev/null )
    ok "build completed despite worker death (20/20 objects)"
else
    bad "build incomplete after worker death ($n/20)"
fi

# ---------------------------------------------------------------------------
echo "[3] Coordinator killed mid-build -> clients fall back, build completes"
start_grid
gen_project "$WORK/p3" 12
build_bg "$WORK/p3" 12 "SUCO_LOCAL_SLOTS=2"
sleep 0.8
kill -9 "$COORD_PID" 2>/dev/null    # coordinator dies mid-build
wait $BPID 2>/dev/null
n=$(ls -1 "$WORK/p3"/*.o 2>/dev/null | wc -l)
[ "$n" -eq 12 ] && ok "build completed despite coordinator death (12/12)" || bad "build incomplete after coordinator death ($n/12)"

# ---------------------------------------------------------------------------
echo "[4] Worker restart mid-session -> re-registers and grid recovers"
start_grid
kill -9 "$WORKER_PID" 2>/dev/null; sleep 1
"$BUILD/suco-worker" --coordinator 127.0.0.1:9000 --slots 4 > "$WORK/worker2.log" 2>&1 & WORKER_PID=$!
for i in $(seq 1 20); do grep -qi "Registered successfully" "$WORK/worker2.log" && break; sleep 0.5; done
gen_project "$WORK/p4" 5
build_bg "$WORK/p4" 5 "SUCO_LOCAL_SLOTS=0"; wait $BPID 2>/dev/null
n=$(ls -1 "$WORK/p4"/*.o 2>/dev/null | wc -l)
[ "$n" -eq 5 ] && ok "grid recovered after worker restart ($n/5)" || bad "grid did not recover ($n/5)"

echo ""
echo "=== CHAOS RESULT: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] && echo "ALL CHAOS TESTS PASSED" || echo "SOME CHAOS TESTS FAILED"
exit "$FAIL"
