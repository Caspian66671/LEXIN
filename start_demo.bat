@echo off
setlocal
cd /d "%~dp0"
echo Starting WorkBuddy demo helper...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start_workbuddy_proxy.ps1"
echo.
if errorlevel 1 (
    echo Proxy check failed. Keep this window open and send the error to Codex.
) else (
    echo Ready. Now use the ESP32-P4 touch screen.
)
echo.
pause
