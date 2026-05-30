@echo off
setlocal
cd /d "%~dp0"
echo Starting WorkBuddy quick proxy...
echo.
node tools\workbuddy_proxy.js
echo.
echo Proxy stopped. Press any key to close this window.
pause >nul
