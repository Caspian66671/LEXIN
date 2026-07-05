@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem esp-ppq requires Python >=3.8,<3.13. Python 3.13/3.14 will NOT work.
rem Find a compatible interpreter (prefer 3.12, then 3.11) via the py launcher.
set "PYLAUNCH="
py -3.12 --version >nul 2>nul && set "PYLAUNCH=py -3.12"
if not defined PYLAUNCH (
  py -3.11 --version >nul 2>nul && set "PYLAUNCH=py -3.11"
)
if not defined PYLAUNCH (
  py -3.10 --version >nul 2>nul && set "PYLAUNCH=py -3.10"
)

if not defined PYLAUNCH (
  echo.
  echo Could not find Python 3.10, 3.11, or 3.12.
  echo esp-ppq does NOT support Python 3.13+ ^(you appear to have 3.14^).
  echo Please install Python 3.12 from:
  echo   https://www.python.org/downloads/release/python-3127/
  echo Tick "Add Python to PATH" during install, then re-run this script.
  pause
  exit /b 1
)
echo Using interpreter: %PYLAUNCH%
%PYLAUNCH% --version

rem If a venv already exists but was built with an incompatible Python
rem (e.g. the earlier 3.14 attempt), recreate it.
if exist ".emotion_venv\Scripts\python.exe" (
  ".emotion_venv\Scripts\python.exe" -c "import sys; sys.exit(0 if sys.version_info[:2] < (3,13) else 1)" >nul 2>nul
  if errorlevel 1 (
    echo Existing .emotion_venv uses an incompatible Python; recreating it...
    rmdir /s /q ".emotion_venv"
  )
)

if not exist ".emotion_venv\Scripts\python.exe" (
  %PYLAUNCH% -m venv .emotion_venv
  if errorlevel 1 goto :fail
)

set PY=.emotion_venv\Scripts\python.exe

"%PY%" -m pip install --upgrade pip
if errorlevel 1 goto :fail
"%PY%" -m pip install torch --index-url https://download.pytorch.org/whl/cpu
if errorlevel 1 goto :fail
"%PY%" -m pip install esp-ppq numpy pillow datasets
if errorlevel 1 goto :fail

echo.
echo === Step 1/2: training the FER2013 emotion CNN ===
"%PY%" tools\train_emotion_fer2013.py
if errorlevel 1 goto :fail

echo.
echo === Step 2/2: quantising + exporting to ESP-DL ===
"%PY%" tools\export_emotion_espdl.py
if errorlevel 1 goto :fail

echo.
echo Emotion model exported to main\models\expression.espdl
echo Now rebuild + flash the firmware (build_firmware.bat / flash_firmware.bat).
pause
exit /b 0

:fail
echo.
echo Failed to train/export the emotion model.
pause
exit /b 1
