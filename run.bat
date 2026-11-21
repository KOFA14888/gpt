@echo off
setlocal
title ScreenMate AI
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 goto nopython

if not exist ".venv\Scripts\python.exe" (
  echo [1/3] Creating virtual environment...
  python -m venv .venv
  if errorlevel 1 goto fail
)

echo [2/3] Checking dependencies...
".venv\Scripts\python.exe" -m pip install --disable-pip-version-check -q -r requirements.txt
if errorlevel 1 goto fail

if not exist ".env" copy /y ".env.example" ".env" >nul

echo [3/3] Starting ScreenMate AI...
echo Chat: bottom-right corner. Tray icon: right click for settings and exit.
".venv\Scripts\python.exe" main.py
if errorlevel 1 goto fail
exit /b 0

:nopython
echo Python not found. Install Python 3.10+ and enable "Add Python to PATH".
echo https://www.python.org/downloads/
pause
exit /b 1

:fail
echo.
echo Startup failed. See the messages above.
pause
exit /b 1
