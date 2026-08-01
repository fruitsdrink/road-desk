@echo off
setlocal
cd /d "%~dp0"
title Road Desk Mirror Uninstall
echo.
echo Road Desk Mirror - uninstall
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-RoadDeskMirror.ps1" -UninstallOnly %*
set ERR=%ERRORLEVEL%
echo.
pause
exit /b %ERR%