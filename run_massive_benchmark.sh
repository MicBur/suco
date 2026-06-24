#!/bin/bash
set -e
export SUCO_COORDINATOR_HOST="192.168.0.200"
BUILD_DIR="massive_benchmark"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "=================================================="
echo "SUCO Massive Distributed Benchmark (Phase 1 Cache)"
echo "=================================================="

# 1. Generiere 30 C++-Dateien
echo "Generiere 30 komplexe C++-Dateien in '$BUILD_DIR'..."
for fileIdx in {1..30}; do
  filePath="$BUILD_DIR/file_$fileIdx.cpp"
  (
    echo "#include <cmath>"
    echo ""
    
    for i in {1..800}; do
      echo "double func_${fileIdx}_${i}(double x) {"
      echo "    double y = x;"
      for j in {1..15}; do
        echo "    y = std::sin(y) * $j.5 + std::cos(y) * $i.2;"
        echo "    y = std::sqrt(std::abs(y)) + std::pow(y, 1.1);"
      done
      echo "    return y;"
      echo "}"
      echo ""
    done
    
    echo "double run_all_funcs_$fileIdx() {"
    echo "    double sum = 0;"
    for i in {1..800}; do
      echo "    sum += func_${fileIdx}_${i}(1.5);"
    done
    echo "    return sum;"
    echo "}"
  ) > "$filePath"
  echo "  -> file_$fileIdx.cpp generiert"
done

echo "Generierung abgeschlossen."

# 2. Durchlauf 1: Cache Misses (Verteiltes Kompilieren auf 3x HP Minis)
echo -e "\n--------------------------------------------------"
echo "Durchlauf 1: Cache Miss (Verteilter Build im Grid)"
echo "--------------------------------------------------"
start_time=$(date +%s.%N)

# Wir kompilieren alle 30 Dateien parallel über xargs, um die 12 Slots der Worker voll auszunutzen!
# -P 12 startet 12 Compiler-Jobs parallel!
seq 1 30 | xargs -I {} -P 12 sh -c '
  fileIdx={}
  src="massive_benchmark/file_$fileIdx.cpp"
  obj="massive_benchmark/file_$fileIdx.o"
  echo "Starte Kompilierung von file_$fileIdx.cpp..."
  ./build_linux/suco g++ -w -O3 -std=c++17 -c "$src" -o "$obj"
'

end_time=$(date +%s.%N)
duration1=$(python3 -c "print($end_time - $start_time)")
echo -e "\n>> Dauer Durchlauf 1 (Miss): ${duration1} Sekunden. <<"

# 3. Durchlauf 2: Cache Hits (Sekundenschnelle Cache-Rückgabe)
echo -e "\n--------------------------------------------------"
echo "Durchlauf 2: Cache Hit (Direkt aus dem SSD Cache)"
echo "--------------------------------------------------"
start_time2=$(date +%s.%N)

seq 1 30 | xargs -I {} -P 12 sh -c '
  fileIdx={}
  src="massive_benchmark/file_$fileIdx.cpp"
  obj="massive_benchmark/file_$fileIdx.o"
  ./build_linux/suco g++ -w -O3 -std=c++17 -c "$src" -o "$obj"
'

end_time2=$(date +%s.%N)
duration2=$(python3 -c "print($end_time2 - $start_time2)")
echo -e "\n>> Dauer Durchlauf 2 (Hit): ${duration2} Sekunden. <<"

# Ersparnis berechnen
savings=$(python3 -c "print(100 - ($duration2 / $duration1 * 100))")

echo -e "\n=============================================="
echo "BENCHMARK ERGEBNIS:"
echo "----------------------------------------------"
echo "Durchlauf 1 (Miss): $(python3 -c "print(f'{float($duration1):.1f}')")s"
echo "Durchlauf 2 (Hit) : $(python3 -c "print(f'{float($duration2):.1f}')")s"
echo "Performance-Gewinn: $(python3 -c "print(f'{float($savings):.1f}')")%"
echo "=============================================="

# Aufräumen
rm -rf "$BUILD_DIR"
