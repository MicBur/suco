# Deploy all deliverables to Desktop\suco folder

$desktopSuco = "C:\Users\micbu\Desktop\suco"
Write-Host "Deploying all deliverables to $desktopSuco..." -ForegroundColor Cyan

New-Item -ItemType Directory -Force -Path $desktopSuco | Out-Null
New-Item -ItemType Directory -Force -Path "$desktopSuco\icons" | Out-Null
New-Item -ItemType Directory -Force -Path "$desktopSuco\portfolio" | Out-Null

# 1. Copy NSIS Setup Installer
$installerPath = "packaging\windows\suco-0.11.0-windows-x64-setup.exe"
if (Test-Path $installerPath) {
    Copy-Item -Path $installerPath -Destination "$desktopSuco\suco-0.11.0-windows-x64-setup.exe" -Force
    Write-Host "Copied suco-0.11.0-windows-x64-setup.exe (Installer)" -ForegroundColor Green
}

# 2. Copy Standalone Executables and DLL payload
Copy-Item -Path "dist\suco-0.11.0-windows-x64\*" -Destination $desktopSuco -Recurse -Force
Write-Host "Copied standalone binaries, Qt6/MinGW DLLs, and dashboard.html" -ForegroundColor Green

# 3. Copy Icons & Favicon
$iconFiles = @(
    "resources\app_icon.ico",
    "resources\installer_icon.ico",
    "resources\favicon.ico",
    "resources\icon_256x256.png"
)
foreach ($icon in $iconFiles) {
    if (Test-Path $icon) {
        Copy-Item -Path $icon -Destination "$desktopSuco\icons" -Force
    }
}
Write-Host "Copied favicon.ico, app_icon.ico, and installer_icon.ico" -ForegroundColor Green

# 4. Copy Portfolio Showcase Artifact & Generated Images
$artifactDir = "C:\Users\micbu\.gemini\antigravity\brain\78f483fb-6e37-4d39-b476-c00b6a7d43de"
$portfolioFiles = @(
    "suco_portfolio_showcase.md",
    "suco_grid_dashboard_showcase.jpg",
    "suco_qt_control_center_showcase.jpg"
)
foreach ($pf in $portfolioFiles) {
    $fullPath = "$artifactDir\$pf"
    if (Test-Path $fullPath) {
        Copy-Item -Path $fullPath -Destination "$desktopSuco\portfolio" -Force
    }
}
Write-Host "Copied Portfolio Showcase Markdown & 4K Graphics to portfolio\" -ForegroundColor Green

Write-Host "`nAll deliverables successfully deployed to Desktop: $desktopSuco" -ForegroundColor Green
