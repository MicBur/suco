$ffmpeg = (Resolve-Path "thirdparty/tools/ffmpeg.exe").Path

$vidCmd = "C:\Users\micbu\Videos\2026-07-26 14-59-34.mp4"
$vidWeb = "C:\Users\micbu\Videos\2026-07-26 15-01-28.mp4"

$thumb = (Resolve-Path "C:\Users\micbu\.gemini\antigravity\brain\78f483fb-6e37-4d39-b476-c00b6a7d43de\suco_youtube_thumb_1785068376136.jpg").Path
$chart = (Resolve-Path "C:\Users\micbu\.gemini\antigravity\brain\78f483fb-6e37-4d39-b476-c00b6a7d43de\suco_bench_chart_1785068392621.jpg").Path

Write-Host "=== 1. Rendering Sequential YouTube Video Edit ==="

# Render Intro PNG to 4s MP4
& $ffmpeg -y -loop 1 -i $thumb -t 4 -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p" -c:v libx264 -r 60 docs/part1_intro.mp4

# Re-encode CMD video to standard 1080p60
& $ffmpeg -y -i $vidCmd -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p" -c:v libx264 -r 60 docs/part2_cmd.mp4

# Re-encode Browser video to standard 1080p60
& $ffmpeg -y -i $vidWeb -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p" -c:v libx264 -r 60 docs/part3_web.mp4

# Render Outro PNG to 6s MP4
& $ffmpeg -y -loop 1 -i $chart -t 6 -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,format=yuv420p" -c:v libx264 -r 60 docs/part4_outro.mp4

# Concatenate all parts
$concatList = "docs/obs_concat_list.txt"
"file 'part1_intro.mp4'`nfile 'part2_cmd.mp4'`nfile 'part3_web.mp4'`nfile 'part4_outro.mp4'" | Out-File -Encoding ascii $concatList

$finalMp4 = "docs/SUCO_YouTube_Showcase_Final.mp4"
& $ffmpeg -y -f concat -safe 0 -i $concatList -c copy $finalMp4

Write-Host "=== 2. Rendering Side-by-Side Split-Screen Video ==="
$splitMp4 = "docs/suco_youtube_splitscreen.mp4"
& $ffmpeg -y -i $vidCmd -i $vidWeb -filter_complex "[0:v]scale=960:1080[left];[1:v]scale=960:1080[right];[left][right]hstack[v]" -map "[v]" -c:v libx264 -preset fast -crf 18 -r 60 $splitMp4

# Cleanup temp parts
Remove-Item docs/part1_intro.mp4, docs/part2_cmd.mp4, docs/part3_web.mp4, docs/part4_outro.mp4, docs/obs_concat_list.txt -ErrorAction SilentlyContinue

if ((Test-Path $finalMp4) -and (Test-Path $splitMp4)) {
    $f1 = Get-Item $finalMp4
    $f2 = Get-Item $splitMp4
    Write-Host "SUCCESS: Generated Final YouTube Video ($($f1.Length) bytes) and Split-Screen Video ($($f2.Length) bytes)"
} else {
    throw "Failed to render video edits!"
}
