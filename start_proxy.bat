@echo off
setlocal
cd /d "%~dp0"
echo Starting WorkBuddy quick proxy...
echo.
where node >nul 2>nul
if errorlevel 1 (
    echo Node.js was not found.
    echo Install Node.js 18 or newer, then run this file again.
    echo.
    pause
    exit /b 1
)
node "%~dp0tools\workbuddy_proxy.js" --port 8787
echo.
echo Proxy stopped. Press any key to close this window.
pause >nul
