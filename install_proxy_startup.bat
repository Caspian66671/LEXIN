@echo off
setlocal
cd /d "%~dp0"
echo Installing WorkBuddy proxy startup task...
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install_proxy_startup.ps1"
echo.
pause
