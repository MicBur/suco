#!/bin/bash
# Unattended cold-vs-warm RocksDB benchmark for an IDLE machine (run overnight).
# Usage: SUCO_SECRET=... scripts/bench_cold.sh <rocksdb_build_dir> [runs]
# Appends results to ~/suco-bench.log. Refuses to start if the machine is busy.
set -u
BUILD_DIR="${1:?rocksdb build dir (configured with suco-cl++ launcher)}"
RUNS="${2:-3}"
LOG=~/suco-bench.log
LOAD=$(awk '{print int($1)}' /proc/loadavg)
if [ "$LOAD" -gt 2 ]; then
    echo "$(date '+%F %T') SKIPPED: load $LOAD > 2 — machine not idle" >> "$LOG"
    exit 1
fi
echo "=== $(date '+%F %T') cold benchmark, $RUNS runs, dir=$BUILD_DIR ===" >> "$LOG"
for i in $(seq 1 "$RUNS"); do
    suco cache clear > /dev/null 2>&1
    ninja -C "$BUILD_DIR" -t clean > /dev/null 2>&1
    W=$( { /usr/bin/time -f "%e" ninja -C "$BUILD_DIR" -j32 rocksdb > /dev/null; } 2>&1 | tail -1 )
    N=$(find "$BUILD_DIR" -name '*.o' | wc -l)
    echo "  cold run $i: wall=${W}s objects=$N" >> "$LOG"
done
ninja -C "$BUILD_DIR" -t clean > /dev/null 2>&1
W=$( { /usr/bin/time -f "%e" ninja -C "$BUILD_DIR" -j32 rocksdb > /dev/null; } 2>&1 | tail -1 )
echo "  warm run: wall=${W}s" >> "$LOG"
echo "=== done $(date '+%F %T') ===" >> "$LOG"
