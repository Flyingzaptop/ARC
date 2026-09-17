@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\stage4-validate.ps1" -PublishResults %*
exit /b %ERRORLEVEL%
