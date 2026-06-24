# SUCO Lite - Massiver Benchmark-Runner
# Dieses Skript simuliert ein Projekt mit 30 großen C++-Dateien.
# Ohne Cache dauert die Kompilierung ca. 8 - 10 Minuten.
# Mit Cache (Zweitdurchlauf) dauert sie nur wenige Sekunden.

$ErrorActionPreference = "Stop"

# Prüfen, ob wir in der MSVC Developer-Umgebung sind (cl.exe muss im Pfad sein)
if (-not (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
    Write-Host "FEHLER: cl.exe wurde nicht im Pfad gefunden!" -ForegroundColor Red
    Write-Host "Bitte starte dieses Skript aus einer Visual Studio Developer PowerShell" -ForegroundColor Yellow
    Write-Host "oder rufe vorher vcvarsall.bat auf." -ForegroundColor Yellow
    exit 1
}

$buildDir = "$PSScriptRoot\massive_benchmark"
if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}
New-Item -ItemType Directory -Path $buildDir | Out-Null

$sucoPath = "$PSScriptRoot\build\Release\suco.exe"
if (-not (Test-Path $sucoPath)) {
    Write-Host "FEHLER: suco.exe nicht gefunden unter $sucoPath" -ForegroundColor Red
    exit 1
}

# 1. Generiere 30 C++-Dateien
Write-Host "Erzeuge 30 komplexe C++-Dateien im Verzeichnis '$buildDir'..." -ForegroundColor Cyan

for ($fileIdx = 1; $fileIdx -le 30; $fileIdx++) {
    $filePath = "$buildDir\file_$fileIdx.cpp"
    $sb = [System.Text.StringBuilder]::new()
    
    # Umfangreiche Standard-Header inkludieren, um die Parsing-Zeit zu erhöhen
    $sb.AppendLine("#include <iostream>") | Out-Null
    $sb.AppendLine("#include <cmath>") | Out-Null
    $sb.AppendLine("#include <vector>") | Out-Null
    $sb.AppendLine("#include <string>") | Out-Null
    $sb.AppendLine("#include <map>") | Out-Null
    $sb.AppendLine("") | Out-Null
    
    # 1200 komplexe Funktionen pro Datei
    for ($i = 0; $i -lt 1200; $i++) {
        $sb.AppendLine("double func_${fileIdx}_${i}(double x) {") | Out-Null
        $sb.AppendLine("    double y = x;") | Out-Null
        for ($j = 0; $j -lt 30; $j++) {
            $sb.AppendLine("    y = std::sin(y) * $($j).5 + std::cos(y) * $($i).2;") | Out-Null
            $sb.AppendLine("    y = std::sqrt(std::abs(y)) + std::pow(y, 1.1);") | Out-Null
        }
        $sb.AppendLine("    return y;") | Out-Null
        $sb.AppendLine("}") | Out-Null
        $sb.AppendLine("") | Out-Null
    }
    
    # Helfer-Funktion, die alle aufruft
    $sb.AppendLine("double run_all_funcs_$fileIdx() {") | Out-Null
    $sb.AppendLine("    double sum = 0;") | Out-Null
    for ($i = 0; $i -lt 1200; $i++) {
        $sb.AppendLine("    sum += func_${fileIdx}_${i}(1.5);") | Out-Null
    }
    $sb.AppendLine("    return sum;") | Out-Null
    $sb.AppendLine("}") | Out-Null
    
    [System.IO.File]::WriteAllText($filePath, $sb.ToString())
    Write-Host "  -> file_$fileIdx.cpp generiert" -ForegroundColor Gray
}

Write-Host "`nGenerierung abgeschlossen. Starte Benchmark-Kompilierung..." -ForegroundColor Green

# Hilfsfunktion zur Durchführung der Kompilierung
function Run-CompileBatch($label) {
    Write-Host "--------------------------------------------------" -ForegroundColor Cyan
    Write-Host "Starte Durchlauf: $label" -ForegroundColor Yellow
    Write-Host "--------------------------------------------------" -ForegroundColor Cyan
    
    $startTime = [DateTime]::Now
    
    for ($fileIdx = 1; $fileIdx -le 30; $fileIdx++) {
        $src = "$buildDir\file_$fileIdx.cpp"
        $obj = "$buildDir\file_$fileIdx.obj"
        
        Write-Host "Kompiliere file_$fileIdx.cpp (mittels suco.exe)... " -NoNewline -ForegroundColor White
        
        $compileStart = [DateTime]::Now
        
        # Aufruf mit Optimierung /O2 und C++-Exceptions /EHsc
        & $sucoPath cl.exe /c /O2 /EHsc $src "/Fo$obj"
        $exitCode = $LASTEXITCODE
        
        $compileEnd = [DateTime]::Now
        $diff = ($compileEnd - $compileStart).TotalSeconds
        
        if ($exitCode -eq 0) {
            Write-Host "OK ($($diff.ToString('F1'))s)" -ForegroundColor Green
        } else {
            Write-Host "FEHLER (ExitCode: $exitCode)" -ForegroundColor Red
            exit 1
        }
    }
    
    $endTime = [DateTime]::Now
    $totalDuration = ($endTime - $startTime).TotalSeconds
    return $totalDuration
}

# 2. Erster Durchlauf: Cache Misses (alles wird frisch auf dem Grid/Worker kompiliert)
# Wir leeren vorher das Cache-Verzeichnis des Coordinators, um echte Messergebnisse zu garantieren!
# (Das Standard-Cache-Verzeichnis liegt unter %LOCALAPPDATA%\suco\cache\)
$localCache = "$env:LOCALAPPDATA\suco\cache"
if (Test-Path $localCache) {
    Write-Host "`nBereinige lokalen Coordinator-Cache unter '$localCache' für sauberen Benchmark..." -ForegroundColor Yellow
    Remove-Item -Recurse -Force $localCache\* -ErrorAction SilentlyContinue
}

$durationMiss = Run-CompileBatch -label "DURCHLAUF 1: CACHE-MISS (Frische Grid-Kompilierung)"

# 3. Zweiter Durchlauf: Cache Hits (alles wird direkt aus dem Coordinator-Cache geladen)
$durationHit = Run-CompileBatch -label "DURCHLAUF 2: CACHE-HIT (Direktes Laden aus dem Cache)"

# 4. Auswertung
$reduction = ((1 - ($durationHit / $durationMiss)) * 100).ToString("F1")
Write-Host "`n==================================================" -ForegroundColor Cyan
Write-Host "              BENCHMARK AUSWERTUNG                " -ForegroundColor Cyan
Write-Host "==================================================" -ForegroundColor Cyan
Write-Host "Dauer Cache-Miss (Durchlauf 1): $($durationMiss.ToString('F1')) Sekunden (~$(([Math]::Round($durationMiss / 60, 1))) Minuten)" -ForegroundColor White
Write-Host "Dauer Cache-Hit  (Durchlauf 2): $($durationHit.ToString('F1')) Sekunden" -ForegroundColor Green
Write-Host "Zeit-Ersparnis:                 $reduction %" -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Cyan

# Cleanup
if (Test-Path $buildDir) {
    Remove-Item -Recurse -Force $buildDir
}
