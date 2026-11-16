@echo off
rem Builds the port (build.ps1 next to this file): build.cmd [-Pgo use^|off] [ninja targets]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
