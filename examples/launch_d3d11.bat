@echo off
setlocal

set SM86_ENABLE_D3D11_BRIDGE=1
set SM86_ENABLE_OSD=1
set SM86_LOW_LATENCY=1
set SM86_DIAGNOSTICS=0

if "%~1"=="" (
  echo Usage: %~nx0 ^<game.exe^> [arguments]
  exit /b 2
)

start "" %*
