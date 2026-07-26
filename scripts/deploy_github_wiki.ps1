# Script to deploy/sync the wiki/ directory directly to GitHub Wiki repository (https://github.com/MicBur/suco.wiki.git)

$wikiRepo = "https://github.com/MicBur/suco.wiki.git"
$tmpDir = Join-Path $env:TEMP "suco_wiki_deploy"
$srcWiki = Join-Path (Get-Location) "wiki"

Remove-Item -Recurse -Force $tmpDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

Write-Host "=== 1. Cloning GitHub Wiki Repository ==="
git clone $wikiRepo $tmpDir 2>$null
if (-not (Test-Path "$tmpDir/.git")) {
    Write-Host "Initializing new Wiki repository..."
    cd $tmpDir
    git init
    git remote add origin $wikiRepo
}

Write-Host "=== 2. Copying Wiki Pages ==="
Get-ChildItem "$srcWiki/*.md" | Copy-Item -Destination $tmpDir -Force

Write-Host "=== 3. Committing and Pushing Wiki Pages ==="
cd $tmpDir
git add .
git commit -m "docs(wiki): update SUCO Grid wiki documentation pages"
git push -u origin master 2>$null; git push -u origin main 2>$null
cd (Get-Location)

Write-Host "SUCCESS: SUCO Grid Wiki published at https://github.com/MicBur/suco/wiki"
