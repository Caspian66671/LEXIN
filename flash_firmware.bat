@echo off
setlocal
cd /d "%~dp0"

where idf.py >nul 2>nul
if errorlevel 1 (
    echo ESP-IDF command line is not ready.
    echo Open this project in the ESP-IDF VSCode terminal first.
    echo.
    pause
    exit /b 1
)

echo Building and flashing LeXin firmware...
idf.py build flash
if errorlevel 1 goto failed

echo.
echo Flash OK. Press the board reset button if the screen does not restart automatically.
echo.
pause
exit /b 0

:failed
echo.
echo Flash failed. Check the USB cable and select the ESP32-P4 serial port.
echo.
pause
exit /b 1
