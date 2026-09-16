@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\stage2-final-validate.ps1" %*
set "ARC_EXIT=%ERRORLEVEL%"
if not "%ARC_EXIT%"=="0" echo ARC Stage 2 validation exited with code %ARC_EXIT%.
exit /b %ARC_EXIT%
