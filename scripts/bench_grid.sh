#!/usr/bin/env bash
# SUCO grid benchmark — native-Linux vs Windows-cross-compile throughput.
#
#   sudo scripts/bench_grid.sh                 # defaults: 48 TUs, 3 rounds
#   sudo N=96 ROUNDS=5 scripts/bench_grid.sh
#   sudo COORD=192.168.0.200 scripts/bench_grid.sh
#
# Must run as root: it sources /etc/suco/secret.env for the grid handshake.
# The secret is never printed.
#
# Method (see docs/BENCHMARK.md for the reasoning):
#   * A discarded warm-up round primes each target's worker-side header caches,
#     so round 1 does not pay cold-header costs that later rounds skip.
#   * Linux and Windows rounds are INTERLEAVED and the median is reported —
#     a single run of each is dominated by run-to-run variance (~20%).
#   * Every round uses freshly generated, uniquely-named sources and clears the
#     client cache, so each measurement is a genuine cold-cache compile.
#   * The local baseline runs at -j$(nproc); oversubscribing it costs ~7% and
#     would flatter the grid.
set -u

N="${N:-48}"                 # translation units per round
PAR="${PAR:-16}"             # client parallelism for grid rounds
ROUNDS="${ROUNDS:-3}"
COORD="${COORD:-127.0.0.1}"
WORK="${WORK:-/tmp/suco-bench-$$}"
LOCAL_PAR="${LOCAL_PAR:-$(nproc)}"

[ "$(id -u)" -eq 0 ] || { echo "must run as root (needs /etc/suco/secret.env)"; exit 2; }
set -a; . /etc/suco/secret.env; set +a
export SUCO_COORDINATOR="$COORD" SUCO_NO_DAEMON=1

rm -rf "$WORK"; mkdir -p "$WORK"; cd "$WORK"
trap 'cd /; rm -rf "$WORK"' EXIT

# A template/STL-heavy TU: compile time dominates network overhead, which is
# what a real C++ build looks like. Trivial TUs would only measure dispatch.
gen() {
    local tok="$1" i
    for i in $(seq 1 "$N"); do
        cat > "tu_${tok}_${i}.cpp" <<EOF
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <numeric>
template<class T> struct Wr { T v; T get() const { return v + T{}; } };
template<class T> static T red_${tok}(std::vector<T> a){
    std::sort(a.begin(), a.end());
    return std::accumulate(a.begin(), a.end(), T{});
}
static int c_${tok}_${i}(){
    std::vector<int> a(200); std::iota(a.begin(), a.end(), ${i});
    std::map<std::string,int> m;
    for (int k : a) m[std::to_string(k*${i})] = k;
    Wr<int> w{ red_${tok}(a) };
    return w.get() + (int)m.size();
}
int main(){ return c_${tok}_${i}(); }
EOF
    done
}

# timed <glob> <parallelism> <command...>   -> wall seconds on stdout
timed() {
    local glob="$1" par="$2"; shift 2
    local s e
    s=$(date +%s.%N)
    ls $glob | xargs -P "$par" -I{} "$@" >/dev/null 2>&1
    e=$(date +%s.%N)
    awk -v a="$s" -v b="$e" 'BEGIN{printf "%.3f", b-a}'
}
clear_client_cache() { rm -rf /root/.cache/suco /root/.suco 2>/dev/null || true; }
median() { sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'; }

GRID_LIN=(env SUCO_LOCAL_SLOTS=0 suco-cl++ -O2 -c {} -o {}.o)
GRID_WIN=(env SUCO_LOCAL_SLOTS=0 SUCO_REAL_CXX=x86_64-w64-mingw32-g++ suco-cl++ -O2 -c {} -o {}.o)

echo "=== SUCO grid benchmark ==="
echo "  TUs/round=$N  grid parallelism=$PAR  rounds=$ROUNDS  coordinator=$COORD"
echo "  client: $(nproc) cores, local baseline at -j$LOCAL_PAR"
echo "  native: $(g++ --version | head -1)"
echo "  mingw : $(x86_64-w64-mingw32-g++ --version | head -1)"
echo

# Warm-up (discarded): primes worker header caches for BOTH targets.
gen wu; timed "tu_wu_*.cpp" "$PAR" "${GRID_LIN[@]}" >/dev/null
gen wv; timed "tu_wv_*.cpp" "$PAR" "${GRID_WIN[@]}" >/dev/null

: > .l; : > .v; : > .b
printf "  %-6s %10s %10s %10s\n" round linux windows local
for r in $(seq 1 "$ROUNDS"); do
    clear_client_cache; gen "l$r"
    L=$(timed "tu_l${r}_*.cpp" "$PAR" "${GRID_LIN[@]}")
    clear_client_cache; gen "v$r"
    V=$(timed "tu_v${r}_*.cpp" "$PAR" "${GRID_WIN[@]}")
    gen "b$r"
    B=$(timed "tu_b${r}_*.cpp" "$LOCAL_PAR" g++ -O2 -c {} -o {}.o)
    printf "  %-6s %10s %10s %10s\n" "$r" "$L" "$V" "$B"
    echo "$L" >> .l; echo "$V" >> .v; echo "$B" >> .b
done

# Warm cache: re-run the last Linux round; the cache should serve every TU.
WARM=$(timed "tu_l${ROUNDS}_*.cpp" "$PAR" "${GRID_LIN[@]}")

ML=$(median < .l); MV=$(median < .v); MB=$(median < .b)
echo
awk -v l="$ML" -v v="$MV" -v b="$MB" -v w="$WARM" -v n="$N" 'BEGIN{
    printf "  MEDIAN local g++     : %6.3fs  (%5.1f TU/s)\n", b, n/b;
    printf "  MEDIAN grid Linux    : %6.3fs  (%5.1f TU/s)  %.2fx vs local\n", l, n/l, b/l;
    printf "  MEDIAN grid Windows  : %6.3fs  (%5.1f TU/s)  %.2fx vs local\n", v, n/v, b/v;
    printf "  grid Linux WARM cache: %6.3fs  (%5.1f TU/s)  %.2fx vs cold grid\n", w, n/w, l/w;
    printf "  Windows vs Linux     : %.2fx  (NOTE: confounded by compiler version, see docs)\n", v/l;
}'
# Sanity: MinGW emits COFF objects, native g++ emits ELF. Guards against a
# silently-failed cross-compile being reported as a fast run.
echo "  sanity: linux=$(file tu_l${ROUNDS}_*.o 2>/dev/null | grep -c ELF)/$N ELF, windows=$(file tu_v${ROUNDS}_*.o 2>/dev/null | grep -c COFF)/$N COFF"
