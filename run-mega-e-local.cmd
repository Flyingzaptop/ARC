@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\mega-e-local-check.ps1"
exit /b %ERRORLEVEL%
