# Script to launch real live CMD windows and record real desktop screen activity into an MP4 video

$ffmpeg = (Resolve-Path "thirdparty/tools/ffmpeg.exe").Path
$buildDir = (Resolve-Path "build").Path
$outMp4 = (Join-Path $PWD "docs/suco_live_cmd_demo.mp4")

# Kill any existing coordinator/worker processes first
Get-Process -Name "suco-coordinator", "suco-worker" -ErrorAction SilentlyContinue | Stop-Process -Force

Write-Host "=== 1. Starting FFmpeg Screen Recorder (22s) ==="
$ffmpegArgs = "-y -f gdigrab -framerate 25 -t 22 -i desktop -vf `"scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p`" -c:v libx264 -preset ultrafast -crf 20 `"$outMp4`""
$recProc = Start-Process -FilePath $ffmpeg -ArgumentList $ffmpegArgs -PassThru -NoNewWindow

Start-Sleep -Seconds 2

Write-Host "=== 2. Opening Live CMD Windows on Screen ==="

# Launch Coordinator CMD Window (Top Left)
$cmdCoord = "title SUCO Coordinator Hub && cd /d `"$buildDir`" && suco-coordinator.exe"
Start-Process -FilePath "cmd.exe" -ArgumentList "/k", $cmdCoord

Start-Sleep -Seconds 3

# Launch Worker CMD Window (Top Right)
$cmdWorker = "title SUCO Worker Node (Worker-Alpha) && cd /d `"$buildDir`" && suco-worker.exe --coordinator 127.0.0.1:9000 --slots 4 --direct-port 9005 --name Worker-Alpha"
Start-Process -FilePath "cmd.exe" -ArgumentList "/k", $cmdWorker

Start-Sleep -Seconds 4

# Launch Client CMD Window (Bottom) & Execute Compilations
$cmdClient = "title SUCO Client Terminal && cd /d `"$PWD`" && timeout /t 2 && echo --- PASS 1: COLD COMPILATION --- && build\suco-cl++.exe -O2 -std=c++20 -c src/common/hash_util.cpp -o build/hash_util.o && timeout /t 3 && echo --- PASS 2: L2 GRID CACHE HIT --- && build\suco-cl++.exe -O2 -std=c++20 -c src/common/hash_util.cpp -o build/hash_util.o"
Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $cmdClient

Write-Host "=== 3. Waiting for Live Recording to Complete ==="
$recProc.WaitForExit()

Write-Host "=== 4. Cleaning up CMD Windows ==="
Get-Process -Name "cmd", "suco-coordinator", "suco-worker" -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like "*SUCO*" } | Stop-Process -Force

if (Test-Path $outMp4) {
    $item = Get-Item $outMp4
    Write-Host "SUCCESS: Real Live CMD Recording saved at $($item.FullName) (Size: $($item.Length) bytes)"
} else {
    throw "Failed to record live video!"
}
