@echo off
chcp 65001 >nul
title PC port live view
mode con cols=92 lines=38 >nul 2>&1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0threads.ps1"
