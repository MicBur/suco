# PowerShell Script to Deploy SSH Public Key to Grid Nodes
# Run this script in PowerShell to install your key on all remote machines.

$pubKey = (Get-Content "$env:USERPROFILE\.ssh\id_ed25519.pub" -Raw).Trim()

if (-not $pubKey) {
    Write-Error "Could not read $env:USERPROFILE\.ssh\id_ed25519.pub"
    exit 1
}

$nodes = @("k3master", "node1", "node2", "node3")

Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "  SUCO SSH Key Deployment to Cluster Nodes" -ForegroundColor Cyan
Write-Host "========================================================" -ForegroundColor Cyan
Write-Host "Public Key to deploy:" -ForegroundColor Yellow
Write-Host $pubKey -ForegroundColor Gray
Write-Host "--------------------------------------------------------" -ForegroundColor Cyan

foreach ($node in $nodes) {
    Write-Host "`n>>> Deploying SSH Key to node: $node" -ForegroundColor Green
    Write-Host "Enter the SSH password for $node if prompted:" -ForegroundColor Yellow
    
    $remoteCmd = "mkdir -p ~/.ssh && chmod 700 ~/.ssh && echo '$pubKey' >> ~/.ssh/authorized_keys && sort -u ~/.ssh/authorized_keys -o ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys && echo 'SUCCESS: Key installed on \$(hostname)'"
    
    & ssh.exe -o StrictHostKeyChecking=accept-new $node $remoteCmd
}

Write-Host "`n========================================================" -ForegroundColor Cyan
Write-Host "  SSH Key deployment finished!" -ForegroundColor Green
Write-Host "  Test passwordless connection with: ssh k3master" -ForegroundColor Green
Write-Host "========================================================" -ForegroundColor Cyan
