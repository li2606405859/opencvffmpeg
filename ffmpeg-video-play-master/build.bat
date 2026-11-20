@echo off
REM ============================================================
REM 1main.cpp 编译脚本 (FFmpeg + SDL + swresample)
REM 用法: 双击或 cmd 里运行 build.bat
REM ============================================================
cd /d "%~dp0"

g++ mainaudio.cpp -o mainaudio.exe ^
  -I"lib/ffmpeg-4.2.1/include" ^
  -I"lib/SDL2/include" ^
  -L"dll/x64" ^
  -L"lib/SDL2/lib/x64" ^
  -lavformat -lavcodec -lavutil -lswresample -lSDL2

if %ERRORLEVEL%==0 (
  echo.
  echo [BUILD OK] 1main.exe ^(146KB^)
  echo.
  echo 运行: 1main.exe  (按 ESC 退出)
) else (
  echo.
  echo [BUILD FAILED] exit code %ERRORLEVEL%
)
pause
