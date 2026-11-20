@echo off
setlocal
cd /d "%~dp0"

where g++ >nul 2>nul
if errorlevel 1 (
    echo [build] g++ not found in PATH
    exit /b 1
)

g++ -std=c++17 -O2 -Wall -Wextra -Iinclude ^
    src\cloud_client.c src\album_store.c src\album_sync.c src\account_bind.c src\hal_pc.c ^
    examples\demo.c -o demo.exe -lws2_32
if errorlevel 1 (
    echo [build] failed
    exit /b 1
)

echo [build] ok: demo.exe
