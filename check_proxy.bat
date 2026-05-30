@echo off
setlocal
cd /d "%~dp0"
echo Checking WorkBuddy quick proxy...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { Invoke-WebRequest -UseBasicParsing http://127.0.0.1:8787/health | Select-Object -ExpandProperty Content; Invoke-WebRequest -UseBasicParsing http://127.0.0.1:8787/time | Select-Object -ExpandProperty Content; Invoke-WebRequest -UseBasicParsing http://127.0.0.1:8787/weather | Select-Object -ExpandProperty Content } catch { Write-Host $_.Exception.Message; exit 1 }"
echo.
pause
