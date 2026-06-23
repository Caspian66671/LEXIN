@echo off
setlocal
cd /d "%~dp0"

where idf.py >nul 2>nul
if errorlevel 1 (
    echo ESP-IDF command line is not ready.
    echo Open this project in the ESP-IDF VSCode terminal, or install ESP-IDF 5.x first.
    echo.
    pause
    exit /b 1
)

echo Setting target to esp32p4...
idf.py set-target esp32p4
if errorlevel 1 goto failed

echo Building firmware...
idf.py build
if errorlevel 1 goto failed

echo.
echo Build OK. Run flash_firmware.bat to auto-detect and flash the board.
echo.
pause
exit /b 0

:failed
echo.
echo Build failed. Keep this window open and check the error above.
echo.
pause
exit /b 1
