# Test script for Prometheus /metrics endpoint on Port 9001

Write-Host "Starting local test coordinator..." -ForegroundColor Cyan
$coordProc = Start-Process -FilePath ".\build\suco-coordinator.exe" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3

try {
    $response = Invoke-WebRequest -Uri "http://127.0.0.1:9001/metrics" -UseBasicParsing
    Write-Host "`n========================================================" -ForegroundColor Green
    Write-Host " PROMETHEUS TELEMETRY ENDPOINT RESPONSE (HTTP $($response.StatusCode)):" -ForegroundColor Green
    Write-Host "========================================================" -ForegroundColor Green
    Write-Host $response.Content
} catch {
    Write-Host "Failed to query /metrics: $_" -ForegroundColor Red
} finally {
    if ($coordProc -and -not $coordProc.HasExited) {
        Stop-Process -Id $coordProc.Id -Force -ErrorAction SilentlyContinue
    }
}
