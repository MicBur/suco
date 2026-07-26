$ffmpeg = (Resolve-Path "thirdparty/tools/ffmpeg.exe").Path
if (-not (Test-Path $ffmpeg)) { throw "ffmpeg.exe not found at $ffmpeg" }

$img1 = (Resolve-Path "C:\Users\micbu\.gemini\antigravity\brain\78f483fb-6e37-4d39-b476-c00b6a7d43de\suco_youtube_thumb_1785068376136.jpg").Path
$img2 = (Resolve-Path "C:\Users\micbu\.gemini\antigravity\brain\78f483fb-6e37-4d39-b476-c00b6a7d43de\suco_slide_arch_1785068581358.jpg").Path
$img3 = (Resolve-Path "C:\Users\micbu\.gemini\antigravity\brain\78f483fb-6e37-4d39-b476-c00b6a7d43de\suco_slide_cross_1785068595797.jpg").Path
$img4 = (Resolve-Path "C:\Users\micbu\.gemini\antigravity\brain\78f483fb-6e37-4d39-b476-c00b6a7d43de\suco_slide_cache_1785068607482.jpg").Path
$img5 = (Resolve-Path "C:\Users\micbu\.gemini\antigravity\brain\78f483fb-6e37-4d39-b476-c00b6a7d43de\suco_bench_chart_1785068392621.jpg").Path

# Create input list file using forward slashes for FFmpeg compatibility
$listContent = @"
file '$($img1 -replace '\\', '/').jpg'
duration 8
file '$($img2 -replace '\\', '/').jpg'
duration 8
file '$($img3 -replace '\\', '/').jpg'
duration 8
file '$($img4 -replace '\\', '/').jpg'
duration 8
file '$($img5 -replace '\\', '/').jpg'
duration 8
file '$($img5 -replace '\\', '/').jpg'
"@

# Fix input filenames
$f1 = $img1 -replace '\\', '/'
$f2 = $img2 -replace '\\', '/'
$f3 = $img3 -replace '\\', '/'
$f4 = $img4 -replace '\\', '/'
$f5 = $img5 -replace '\\', '/'

$listContent = "file '$f1'`nduration 8`nfile '$f2'`nduration 8`nfile '$f3'`nduration 8`nfile '$f4'`nduration 8`nfile '$f5'`nduration 8`nfile '$f5'"

$listFile = (Join-Path $PWD "docs/ffmpeg_concat_list.txt") -replace '\\', '/'
Set-Content -Path "docs/ffmpeg_concat_list.txt" -Value $listContent -Encoding ascii

$outMp4 = "docs/suco_showcase_video.mp4"

Write-Host "Rendering 1080p MP4 Video with FFmpeg..."
& $ffmpeg -y -f concat -safe 0 -i docs/ffmpeg_concat_list.txt -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p" -c:v libx264 -preset fast -crf 18 $outMp4

if (Test-Path $outMp4) {
    $item = Get-Item $outMp4
    Write-Host "SUCCESS: Generated MP4 Video File at $($item.FullName) (Size: $($item.Length) bytes)"
} else {
    throw "Failed to render MP4 video file!"
}
