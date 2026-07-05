@echo off
setlocal
cd /d "%~dp0"
echo Starting LeXin proxy...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start_lexin_proxy.ps1"
echo.
echo Proxy stopped. Press any key to close this window.
pause >nul
