# Script to deploy/sync the wiki/ directory directly to GitHub Wiki repository (https://github.com/MicBur/suco.wiki.git)

$wikiRepo = "https://github.com/MicBur/suco.wiki.git"
$tmpDir = Join-Path $env:TEMP "suco_wiki_deploy"

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
Copy-Item (Join-Path $PWD "wiki/*.md") -Destination $tmpDir -Force

Write-Host "=== 3. Committing and Pushing Wiki Pages ==="
cd $tmpDir
git add .
git commit -m "docs(wiki): update SUCO Grid wiki documentation pages"
git push origin master 2>$null; git push origin main 2>$null
cd $PWD

Write-Host "SUCCESS: SUCO Grid Wiki published at https://github.com/MicBur/suco/wiki"
