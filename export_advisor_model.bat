@echo off
setlocal
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo Python was not found. Install Python 3.11+ or run this from an ESP-IDF terminal with Python on PATH.
  pause
  exit /b 1
)

if not exist ".advisor_venv\Scripts\python.exe" (
  python -m venv .advisor_venv
  if errorlevel 1 goto :fail
)

".advisor_venv\Scripts\python.exe" -m pip install --upgrade pip
if errorlevel 1 goto :fail
".advisor_venv\Scripts\python.exe" -m pip install torch --index-url https://download.pytorch.org/whl/cpu
if errorlevel 1 goto :fail
".advisor_venv\Scripts\python.exe" -m pip install esp-ppq
if errorlevel 1 goto :fail
".advisor_venv\Scripts\python.exe" tools\export_lexin_advisor_espdl.py
if errorlevel 1 goto :fail

echo.
echo ESP-DL advisor model exported to main\models\lexin_advisor.espdl
pause
exit /b 0

:fail
echo.
echo Failed to export ESP-DL advisor model.
pause
exit /b 1
