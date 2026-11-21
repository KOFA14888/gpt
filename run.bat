@echo off
chcp 65001 >nul
setlocal
title ScreenMate AI
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
    echo [!] Python не найден. Установи Python 3.10+ с https://www.python.org/downloads/
    echo     При установке обязательно отметь "Add Python to PATH".
    pause
    exit /b 1
)

if not exist ".venv\Scripts\python.exe" (
    echo [1/3] Создаю виртуальное окружение .venv ...
    python -m venv .venv
    if errorlevel 1 goto :fail
)

echo [2/3] Проверяю зависимости...
".venv\Scripts\python.exe" -m pip install --disable-pip-version-check -q -r requirements.txt
if errorlevel 1 goto :fail

if not exist ".env" (
    copy /y ".env.example" ".env" >nul
    echo [!] Создан файл .env - вставь туда API-ключ, либо введи его в настройках приложения.
)

echo [3/3] Запуск ScreenMate AI...
echo     Чат в правом нижнем углу. Трей: ПКМ по иконке - настройки / выход.
".venv\Scripts\python.exe" main.py
if errorlevel 1 goto :fail
exit /b 0

:fail
echo.
echo [!] Запуск не удался. Проверь вывод выше.
pause
exit /b 1
