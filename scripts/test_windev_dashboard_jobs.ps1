# Test script to force job assignment to WIN-DEV worker node

$env:PATH = "C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\mingw1310_64\opt\bin;" + $env:PATH
$env:SUCO_COORDINATOR_HOST = "192.168.0.200"
$env:SUCO_REAL_CXX = "g++"
$env:SUCO_NO_DAEMON = "1"
$env:SUCO_SECRET = [Environment]::GetEnvironmentVariable('SUCO_SECRET', 'User')

Write-Host "Starting local WIN-DEV worker (8 slots)..." -ForegroundColor Cyan
$workerProc = Start-Process -FilePath ".\build\suco-worker.exe" -ArgumentList "--coordinator 192.168.0.200:9000 --slots 8" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3

$testDir = "scratch_windev_test"
Remove-Item -Path $testDir -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $testDir | Out-Null

$fileList = @()
for ($i = 1; $i -le 30; $i++) {
    $fileName = "windev_job_$i.cpp"
    $fileList += $fileName
    $filePath = "$testDir\$fileName"
    $content = "#include <iostream>`n#include <vector>`n#include <string>`nint windev_func_$i() { return $i; }"
    Set-Content -Path $filePath -Value $content
}

$sourcesStr = $fileList -join " "
$cmakeContent = @"
cmake_minimum_required(VERSION 3.20)
project(WinDevTest CXX)
set(CMAKE_CXX_STANDARD 20)
add_executable(windev_app $sourcesStr)
"@
Set-Content -Path "$testDir\CMakeLists.txt" -Value $cmakeContent

$sucoWrapper = (Resolve-Path "build\suco-cl++.exe").Path

Write-Host "Configuring CMake with suco-cl++.exe..." -ForegroundColor Cyan
cmake -S $testDir -B "$testDir\build" -G Ninja -DCMAKE_CXX_COMPILER="$sucoWrapper" | Out-Null

Write-Host "Compiling 30 jobs in parallel with Ninja (-j 30)..." -ForegroundColor Yellow
cmake --build "$testDir\build" --config Release -- -j 30

Start-Sleep -Seconds 2

# Query Dashboard API for jobs handled by WIN-DEV
$stats = Invoke-RestMethod -Uri "http://192.168.0.200:9001/api/stats"
$winDevJobs = $stats.recent_jobs | Where-Object { $_.worker_name -eq "WIN-DEV" }

Write-Host "`n========================================================" -ForegroundColor Green
Write-Host " JOBS DISPATCHED TO WIN-DEV WORKER ON DASHBOARD:" -ForegroundColor Green
Write-Host "========================================================" -ForegroundColor Green
if ($winDevJobs) {
    $winDevJobs | Format-Table filename, exit_code, worker_name, target_os -AutoSize
} else {
    Write-Host "No WIN-DEV jobs found in recent_jobs. Total recent jobs: $($stats.recent_jobs.Count)" -ForegroundColor Red
}

if ($workerProc -and -not $workerProc.HasExited) {
    Stop-Process -Id $workerProc.Id -Force -ErrorAction SilentlyContinue
}

Remove-Item -Path $testDir -Recurse -Force -ErrorAction SilentlyContinue
