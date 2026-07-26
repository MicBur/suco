# Script to generate ICO and PNG icons from the source image

Add-Type -AssemblyName System.Drawing

$srcPath = "C:\Users\micbu\.gemini\antigravity\brain\78f483fb-6e37-4d39-b476-c00b6a7d43de\suco_app_icon_1785060721710.jpg"
if (-not (Test-Path $srcPath)) {
    Write-Host "Source image not found at $srcPath" -ForegroundColor Red
    exit 1
}

$img = [System.Drawing.Image]::FromFile($srcPath)

# Ensure resources dir
New-Item -ItemType Directory -Force -Path "resources" | Out-Null

# Save PNGs in multiple sizes
$sizes = @(16, 32, 48, 64, 128, 256)
foreach ($sz in $sizes) {
    $bmp = New-Object System.Drawing.Bitmap($sz, $sz)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($img, 0, 0, $sz, $sz)
    $g.Dispose()
    
    $pngPath = "resources\icon_${sz}x${sz}.png"
    $bmp.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Create a master ICO file
# Convert 256x256 bitmap to ICO handle
$bmp256 = New-Object System.Drawing.Bitmap(256, 256)
$g256 = [System.Drawing.Graphics]::FromImage($bmp256)
$g256.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g256.DrawImage($img, 0, 0, 256, 256)
$g256.Dispose()

$hIcon = $bmp256.GetHicon()
$icon = [System.Drawing.Icon]::FromHandle($hIcon)

# Save ICO files
$icoFiles = @(
    "resources\app_icon.ico",
    "resources\installer_icon.ico",
    "resources\favicon.ico",
    "src\gui\app_icon.ico"
)

foreach ($target in $icoFiles) {
    $parent = Split-Path -Parent $target
    if ($parent -and -not (Test-Path $parent)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    $stream = [System.IO.File]::Create($target)
    $icon.Save($stream)
    $stream.Close()
    Write-Host "Created ICO file: $target" -ForegroundColor Green
}

$bmp256.Dispose()
$img.Dispose()

Write-Host "All icons generated successfully!" -ForegroundColor Cyan
