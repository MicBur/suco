# Script to download portable NSIS zip and extract it into tools/nsis

$nsisUrl = "https://downloads.sourceforge.net/project/nsis/NSIS%203/3.10/nsis-3.10.zip"
$nsisZip = "tools\nsis.zip"
$nsisDir = "tools\nsis"

New-Item -ItemType Directory -Force -Path "tools" | Out-Null

if (-not (Test-Path "$nsisDir\makensis.exe")) {
    Write-Host "Downloading portable NSIS..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri $nsisUrl -OutFile $nsisZip -UserAgent "Mozilla/5.0"
    
    Write-Host "Extracting portable NSIS..." -ForegroundColor Cyan
    Expand-Archive -Path $nsisZip -DestinationPath "tools\nsis_temp" -Force
    
    $extracted = Get-ChildItem -Path "tools\nsis_temp" -Directory | Select-Object -First 1
    if ($extracted) {
        Move-Item -Path "$($extracted.FullName)\*" -Destination $nsisDir -Force
        Remove-Item -Path "tools\nsis_temp" -Recurse -Force
    }
    Remove-Item -Path $nsisZip -Force -ErrorAction SilentlyContinue
}

if (Test-Path "$nsisDir\makensis.exe") {
    Write-Host "NSIS is ready at $nsisDir\makensis.exe" -ForegroundColor Green
} else {
    Write-Host "Failed to prepare portable NSIS." -ForegroundColor Red
}
