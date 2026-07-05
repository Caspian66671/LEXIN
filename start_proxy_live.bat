@echo off
setlocal
cd /d "%~dp0"
echo Starting LeXin proxy with live logs...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start_lexin_proxy_live.ps1"
echo.
echo Live log stopped. Press any key to close this window.
pause >nul
