@echo off
echo ============================================================
echo   Starting Smart Traffic Multithreaded Simulation...
echo ============================================================
echo.
echo Opening Web Dashboard at http://localhost:8080 ...
start http://localhost:8080
python "%~dp0traffic_sim.py" --port 8080
pause
