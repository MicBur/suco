# SUCO Lite Windows Installer
# Muss als Administrator ausgeführt werden!

$ErrorActionPreference = "Stop"

# 1. Administrator-Rechte prüfen
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "Bitte starte dieses Skript als Administrator (z. B. in einer administrativen PowerShell)!"
    Exit 1
}

Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "      SUCO Lite - Windows Installer" -ForegroundColor Cyan
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host ""

# 2. Installationsmodus abfragen
$title = "SUCO Modus auswählen"
$message = "Soll dieser PC als SUCO-Coordinator (Hauptrechner) eingerichtet werden?`nWenn Nein, wird er nur als Worker (Kompilierungs-Knoten) eingerichtet."
$yes = New-Object System.Management.Automation.Host.ChoiceDescription "&Ja (Coordinator + Worker)", "Richtet Coordinator und Worker ein."
$no = New-Object System.Management.Automation.Host.ChoiceDescription "&Nein (Nur Worker)", "Richtet nur einen Worker-Knoten ein."
$options = [System.Management.Automation.Host.ChoiceDescription[]]($yes, $no)
$result = $host.ui.PromptForChoice($title, $message, $options, 0)

$installCoordinator = ($result -eq 0)

# 3. Installationsverzeichnis erstellen
$installDir = "C:\Program Files\suco"
if (-not (Test-Path $installDir)) {
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    Write-Host "Installationsverzeichnis erstellt: $installDir" -ForegroundColor Green
}

# 4. Binaries kopieren
$buildDir = "$PSScriptRoot\build\Release"
if (-not (Test-Path "$buildDir\suco.exe")) {
    $buildDir = "$PSScriptRoot\build" # Fallback
}

if (-not (Test-Path "$buildDir\suco.exe")) {
    Write-Error "Kompilierte Binärdateien nicht in $buildDir gefunden. Bitte baue das Projekt zuerst!"
    Exit 1
}

# Dateien kopieren
Copy-Item "$buildDir\suco.exe" -Destination "$installDir\" -Force
Copy-Item "$PSScriptRoot\dashboard.html" -Destination "$installDir\" -Force

if ($installCoordinator) {
    Copy-Item "$buildDir\suco-coordinator.exe" -Destination "$installDir\" -Force
    Copy-Item "$buildDir\suco-worker.exe" -Destination "$installDir\" -Force
    Write-Host "Coordinator-, Worker- und Client-Binärdateien kopiert." -ForegroundColor Green
} else {
    Copy-Item "$buildDir\suco-worker.exe" -Destination "$installDir\" -Force
    Write-Host "Worker-Binärdatei kopiert." -ForegroundColor Green
}

# 5. PATH erweitern
$pathEnv = [Environment]::GetEnvironmentVariable("Path", "Machine")
if ($pathEnv -notlike "*C:\Program Files\suco*") {
    [Environment]::SetEnvironmentVariable("Path", $pathEnv + ";C:\Program Files\suco", "Machine")
    $env:Path += ";C:\Program Files\suco"
    Write-Host "C:\Program Files\suco wurde zur Systemvariable PATH hinzugefügt." -ForegroundColor Green
}

# 6. Firewall-Regeln erstellen
Write-Host "Richte Windows-Firewall-Regeln ein..." -ForegroundColor Yellow
Remove-NetFirewallRule -DisplayName "SUCO *" -ErrorAction SilentlyContinue | Out-Null

New-NetFirewallRule -DisplayName "SUCO Compile Service (TCP 9000)" -Direction Inbound -LocalPort 9000 -Protocol TCP -Action Allow -Enabled True | Out-Null
New-NetFirewallRule -DisplayName "SUCO Dashboard (TCP 9001)" -Direction Inbound -LocalPort 9001 -Protocol TCP -Action Allow -Enabled True | Out-Null
New-NetFirewallRule -DisplayName "SUCO Discovery (UDP 9002)" -Direction Inbound -LocalPort 9002 -Protocol UDP -Action Allow -Enabled True | Out-Null
Write-Host "Firewall-Regeln für Ports 9000 (TCP), 9001 (TCP) und 9002 (UDP) erfolgreich eingerichtet." -ForegroundColor Green

# 7. Start-Skripte im Installationsverzeichnis erstellen
if ($installCoordinator) {
    # Startskript für Coordinator + Worker
    $slots = [Math]::Max(1, $env:NUMBER_OF_PROCESSORS - 2) # 2 Slots weniger auf Coordinator
    $startScript = @"
@echo off
title SUCO Grid Service
cd /d "$installDir"
echo Starte SUCO Coordinator...
start "SUCO Coordinator" /Min suco-coordinator.exe
echo Starte lokalen Worker mit $slots Slots...
start "SUCO Local Worker" /Min suco-worker.exe --slots $slots
echo SUCO Grid erfolgreich gestartet!
echo Dashboard erreichbar unter: http://localhost:9001
pause
"@
    $startScript | Out-File -FilePath "$installDir\start-grid.bat" -Encoding ASCII -Force
    Write-Host "Startskript erstellt: $installDir\start-grid.bat" -ForegroundColor Green
} else {
    # Startskript für reinen Worker
    $slots = $env:NUMBER_OF_PROCESSORS
    $startScript = @"
@echo off
title SUCO Worker node
cd /d "$installDir"
echo Starte SUCO Worker mit $slots Slots...
echo Suche Coordinator automatisch per UDP Broadcast...
suco-worker.exe --slots $slots
pause
"@
    $startScript | Out-File -FilePath "$installDir\start-worker.bat" -Encoding ASCII -Force
    Write-Host "Startskript erstellt: $installDir\start-worker.bat" -ForegroundColor Green
}

Write-Host ""
Write-Host "=============================================" -ForegroundColor Cyan
Write-Host "     Installation erfolgreich abgeschlossen!" -ForegroundColor Cyan
if ($installCoordinator) {
    Write-Host " Starte das Grid mit: C:\Program Files\suco\start-grid.bat" -ForegroundColor Yellow
} else {
    Write-Host " Starte den Worker mit: C:\Program Files\suco\start-worker.bat" -ForegroundColor Yellow
}
Write-Host "=============================================" -ForegroundColor Cyan
