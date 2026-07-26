# ==============================================================================
# SUCO Large Windows Benchmark Harness (100 C++ Translation Units)
# ==============================================================================

$env:PATH = "C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\mingw1310_64\opt\bin;" + $env:PATH
$env:SUCO_COORDINATOR_HOST = "192.168.0.200"
$env:SUCO_REAL_CXX = "g++"
$env:SUCO_NO_DAEMON = "1"

$benchDir = "large_bench_project"
Remove-Item -Path $benchDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $benchDir | Out-Null

Write-Host "==================================================================" -ForegroundColor Cyan
Write-Host " Generating Large C++ Benchmark Project (100 Translation Units)" -ForegroundColor Cyan
Write-Host "==================================================================" -ForegroundColor Cyan

$fileList = @()
for ($i = 1; $i -le 100; $i++) {
    $idxStr = $i.ToString("D3")
    $fileName = "bench_tu_$idxStr.cpp"
    $fileList += $fileName
    $filePath = "$benchDir\$fileName"
    
    $content = @"
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <map>
#include <cmath>
#include <chrono>

int compute_work_$idxStr(int seed) {
    std::vector<double> vals;
    vals.reserve(150);
    for (int k = 0; k < 150; ++k) {
        vals.push_back(std::sin(static_cast<double>(seed + k + $i)) * 100.0);
    }
    std::sort(vals.begin(), vals.end());
    std::map<std::string, double> m;
    for (size_t k = 0; k < vals.size(); ++k) {
        m["key_" + std::to_string(k)] = std::cos(vals[k]) * std::sqrt(std::abs(vals[k]) + 1.0);
    }
    double total = 0.0;
    for (const auto& pair : m) {
        total += pair.second;
    }
    return static_cast<int>(std::abs(total)) + $i;
}
"@
    Set-Content -Path $filePath -Value $content
}

# Main file
$mainContent = "#include <iostream>`n#include <chrono>`n"
for ($i = 1; $i -le 100; $i++) { 
    $idxStr = $i.ToString("D3")
    $mainContent += "extern int compute_work_$idxStr(int);`n" 
}
$mainContent += @"
int main() {
    auto start = std::chrono::high_resolution_clock::now();
    int sum = 0;
"@
for ($i = 1; $i -le 100; $i++) { 
    $idxStr = $i.ToString("D3")
    $mainContent += "    sum += compute_work_$idxStr($i);`n" 
}
$mainContent += @"
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;
    std::cout << "SUCO Large Benchmark App Result: " << sum << " (computed in " << duration.count() << " ms)" << std::endl;
    return 0;
}
"@
Set-Content -Path "$benchDir\main.cpp" -Value $mainContent
$fileList += "main.cpp"

# CMakeLists.txt
$sourcesStr = $fileList -join " "
$cmakeContent = @"
cmake_minimum_required(VERSION 3.20)
project(LargeBenchProject CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(suco_large_bench_app $sourcesStr)
"@
Set-Content -Path "$benchDir\CMakeLists.txt" -Value $cmakeContent

Write-Host "Generated 101 C++ source files in $benchDir." -ForegroundColor Green
$sucoWrapper = (Resolve-Path "build\suco-cl++.exe").Path

# ==============================================================================
# RUN BENCHMARKS
# ==============================================================================
$results = @()

# ------------------------------------------------------------------------------
# 1. Native Local Build (-j 24)
# ------------------------------------------------------------------------------
Write-Host "`n[1/4] Running Native Local Build (MinGW g++ -j 24)..." -ForegroundColor Yellow
$nativeBuildDir = "$benchDir\build_native"
cmake -S $benchDir -B $nativeBuildDir -G Ninja -DCMAKE_CXX_COMPILER="g++" | Out-Null

$sw = [System.Diagnostics.Stopwatch]::StartNew()
cmake --build $nativeBuildDir --config Release -- -j 24 | Out-Null
$sw.Stop()
$t1 = $sw.Elapsed.TotalSeconds
Write-Host "Native Local Build Time: $([math]::Round($t1, 2)) seconds" -ForegroundColor Green
$results += [PSCustomObject]@{ Configuration = "1. Native Local Build (g++ -j 24)"; TimeSec = [math]::Round($t1, 2); Speedup = "1.00x (Baseline)"; Throughput = "$([math]::Round(101/$t1, 1)) TUs/s" }

# ------------------------------------------------------------------------------
# 2. SUCO Remote Grid Only (WIN-DEV Off - 13 slots)
# ------------------------------------------------------------------------------
Write-Host "`n[2/4] Running SUCO Remote Grid Only (13 Remote Linux Slots)..." -ForegroundColor Yellow
$remoteBuildDir = "$benchDir\build_suco_remote"
cmake -S $benchDir -B $remoteBuildDir -G Ninja -DCMAKE_CXX_COMPILER="$sucoWrapper" | Out-Null

$sw = [System.Diagnostics.Stopwatch]::StartNew()
cmake --build $remoteBuildDir --config Release -- -j 16 | Out-Null
$sw.Stop()
$t2 = $sw.Elapsed.TotalSeconds
$s2 = [math]::Round($t1 / $t2, 2)
Write-Host "SUCO Remote Grid Build Time: $([math]::Round($t2, 2)) seconds (Speedup: ${s2}x)" -ForegroundColor Green
$results += [PSCustomObject]@{ Configuration = "2. SUCO Remote Grid Only (13 Slots)"; TimeSec = [math]::Round($t2, 2); Speedup = "${s2}x"; Throughput = "$([math]::Round(101/$t2, 1)) TUs/s" }

# ------------------------------------------------------------------------------
# 3. SUCO Full Hybrid Grid (WIN-DEV On - 21 slots)
# ------------------------------------------------------------------------------
Write-Host "`n[3/4] Running SUCO Full Hybrid Grid (21 Slots: Local + Remote)..." -ForegroundColor Yellow
$env:SUCO_SECRET = [Environment]::GetEnvironmentVariable('SUCO_SECRET', 'User')
$workerProc = Start-Process -FilePath ".\build\suco-worker.exe" -ArgumentList "--coordinator 192.168.0.200:9000 --slots 8" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3

$hybridBuildDir = "$benchDir\build_suco_hybrid"
cmake -S $benchDir -B $hybridBuildDir -G Ninja -DCMAKE_CXX_COMPILER="$sucoWrapper" | Out-Null

$sw = [System.Diagnostics.Stopwatch]::StartNew()
cmake --build $hybridBuildDir --config Release -- -j 24 | Out-Null
$sw.Stop()
$t3 = $sw.Elapsed.TotalSeconds
$s3 = [math]::Round($t1 / $t3, 2)

if ($workerProc -and -not $workerProc.HasExited) {
    Stop-Process -Id $workerProc.Id -Force -ErrorAction SilentlyContinue
}

Write-Host "SUCO Full Hybrid Grid Build Time: $([math]::Round($t3, 2)) seconds (Speedup: ${s3}x)" -ForegroundColor Green
$results += [PSCustomObject]@{ Configuration = "3. SUCO Full Hybrid Grid (21 Slots)"; TimeSec = [math]::Round($t3, 2); Speedup = "${s3}x"; Throughput = "$([math]::Round(101/$t3, 1)) TUs/s" }

# ------------------------------------------------------------------------------
# 4. SUCO Warm Rebuild (L2 Cache Hits)
# ------------------------------------------------------------------------------
Write-Host "`n[4/4] Running SUCO Warm Rebuild (L2 Cache Hits)..." -ForegroundColor Yellow
cd $hybridBuildDir
ninja -t clean | Out-Null
$sw = [System.Diagnostics.Stopwatch]::StartNew()
ninja -j 24 | Out-Null
$sw.Stop()
cd ..\..
$t4 = $sw.Elapsed.TotalSeconds
$s4 = [math]::Round($t1 / $t4, 2)
Write-Host "SUCO Warm Rebuild Time: $([math]::Round($t4, 2)) seconds (Speedup: ${s4}x)" -ForegroundColor Green
$results += [PSCustomObject]@{ Configuration = "4. SUCO Warm Rebuild (L2 Cache Hit)"; TimeSec = [math]::Round($t4, 2); Speedup = "${s4}x"; Throughput = "$([math]::Round(101/$t4, 1)) TUs/s" }

# ------------------------------------------------------------------------------
# Executive Summary Table
# ------------------------------------------------------------------------------
Write-Host "`n==================================================================" -ForegroundColor Green
Write-Host "             LARGE WINDOWS BENCHMARK SUMMARY (101 TUs)" -ForegroundColor Green
Write-Host "==================================================================" -ForegroundColor Green
$results | Format-Table -AutoSize
