# Smart Traffic Multithreaded Simulation Launcher
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  Starting Smart Traffic Multithreaded Simulation...       " -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan

$url = "http://localhost:8080"
Write-Host "Opening Web Dashboard at $url..." -ForegroundColor Green
Start-Process $url

python "$PSScriptRoot\traffic_sim.py" --port 8080
