# Build script for SUCO Windows Installer setup executable

$version = "0.11.0"
$distDir = "dist\suco-$version-windows-x64"

Write-Host "Staging payload in $distDir..." -ForegroundColor Cyan
Remove-Item -Path "dist" -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $distDir | Out-Null

# 1. Copy Executables from build/
$exes = @("suco-gui.exe", "suco-worker.exe", "suco-cl++.exe", "suco-cl.exe", "suco.exe", "suco-coordinator.exe")
foreach ($exe in $exes) {
    if (Test-Path "build\$exe") {
        Copy-Item -Path "build\$exe" -Destination $distDir -Force
        Write-Host "Copied $exe to payload" -ForegroundColor Green
    } else {
        Write-Host "Warning: build\$exe not found!" -ForegroundColor Yellow
    }
}

# 2. Copy Dashboard HTML and LICENSE
Copy-Item -Path "dashboard.html" -Destination $distDir -Force -ErrorAction SilentlyContinue
Copy-Item -Path "LICENSE" -Destination $distDir -Force -ErrorAction SilentlyContinue

# 3. Deploy Qt 6 and MinGW runtime DLLs using windeployqt
Write-Host "Deploying Qt 6 and MinGW runtime DLLs into $distDir..." -ForegroundColor Cyan
$env:PATH = "C:\Qt\Tools\CMake_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\mingw1310_64\opt\bin;C:\Qt\6.11.1\mingw_64\bin;" + $env:PATH
& "C:\Qt\6.11.1\mingw_64\bin\windeployqt.exe" --no-compiler-runtime "$distDir\suco-gui.exe" | Out-Null

# Copy additional MinGW system runtime DLLs if present
$mingwBin = "C:\Qt\Tools\mingw1310_64\bin"
$dlls = @("libwinpthread-1.dll", "libstdc++-6.dll", "libgcc_s_seh-1.dll", "zstd.dll")
foreach ($dll in $dlls) {
    $dllPath = "$mingwBin\$dll"
    if (Test-Path $dllPath) {
        Copy-Item -Path $dllPath -Destination $distDir -Force
    }
}

# 4. Copy resources folder
if (Test-Path "resources") {
    Copy-Item -Path "resources" -Destination "$distDir\resources" -Recurse -Force
}

$absDistDir = (Resolve-Path $distDir).Path

# 5. Invoke makensis to build installer
Write-Host "`nBuilding installer setup executable with NSIS..." -ForegroundColor Yellow
$makensis = (Resolve-Path "tools\nsis\makensis.exe").Path
$nsiScript = (Resolve-Path "packaging\windows\suco.nsi").Path

& $makensis "-DVERSION=$version" "-DSRCDIR=$absDistDir" $nsiScript

if ($LASTEXITCODE -eq 0) {
    Write-Host "`nInstaller built successfully: suco-$version-windows-x64-setup.exe" -ForegroundColor Green
} else {
    Write-Host "`nInstaller build failed with exit code $LASTEXITCODE" -ForegroundColor Red
}
