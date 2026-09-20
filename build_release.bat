@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul || (
  echo ERROR: CMake is not on PATH.
  exit /b 1
)

cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release || exit /b 1

echo.
echo Built: build\Release\version.dll
endlocal

