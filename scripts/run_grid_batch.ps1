# Generate and compile a 20-file C++ project in parallel across the live grid

$env:PATH = "C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\mingw1310_64\opt\bin;" + $env:PATH
$env:SUCO_COORDINATOR_HOST = "192.168.0.200"
$env:SUCO_REAL_CXX = "g++"
$env:SUCO_NO_DAEMON = "1"

$projectDir = "grid_batch_project"
New-Item -ItemType Directory -Force -Path $projectDir | Out-Null

# Generate 20 distinct C++ files with standard headers
$fileList = @()
for ($i = 1; $i -le 20; $i++) {
    $fileName = "batch_file_$i.cpp"
    $fileList += $fileName
    $filePath = "$projectDir\$fileName"
    
    $content = @"
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <map>
#include <cmath>

int func_$i(int val) {
    std::vector<int> data = {val, val * 2, val * 3, val + 10};
    std::sort(data.begin(), data.end());
    std::map<std::string, double> m;
    m["key_$i"] = std::sqrt(static_cast<double>(val + $i));
    return data[0] + static_cast<int>(m["key_$i"]);
}
"@
    Set-Content -Path $filePath -Value $content
}

# Generate main.cpp referencing all functions
$mainContent = "#include <iostream>`n"
for ($i = 1; $i -le 20; $i++) { $mainContent += "extern int func_$i(int);`n" }
$mainContent += @"
int main() {
    int sum = 0;
"@
for ($i = 1; $i -le 20; $i++) { $mainContent += "    sum += func_$i($i);`n" }
$mainContent += @"
    std::cout << "Grid Batch Test Executable Output: " << sum << std::endl;
    return 0;
}
"@
Set-Content -Path "$projectDir\main.cpp" -Value $mainContent
$fileList += "main.cpp"

# Generate CMakeLists.txt
$sourcesStr = $fileList -join " "
$cmakeContent = @"
cmake_minimum_required(VERSION 3.20)
project(GridBatchProject CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(grid_batch_app $sourcesStr)
"@
Set-Content -Path "$projectDir\CMakeLists.txt" -Value $cmakeContent

Write-Host "Generated 21 C++ source files in $projectDir." -ForegroundColor Green
Write-Host "Configuring CMake with suco-cl++.exe as CXX compiler..." -ForegroundColor Cyan

$sucoWrapper = (Resolve-Path "build\suco-cl++.exe").Path

cd $projectDir
cmake -B build_grid -G Ninja -DCMAKE_CXX_COMPILER="$sucoWrapper"
Write-Host "Starting parallel build with Ninja (-j 12)..." -ForegroundColor Yellow

$sw = [System.Diagnostics.Stopwatch]::StartNew()
cmake --build build_grid --config Release -- -j 12
$sw.Stop()

Write-Host "`n========================================================" -ForegroundColor Green
Write-Host " Parallel Grid Build Completed in $($sw.Elapsed.TotalSeconds) seconds!" -ForegroundColor Green
Write-Host "========================================================" -ForegroundColor Green

cd ..
