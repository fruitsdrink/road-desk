@echo off
setlocal
cd /d "%~dp0"
title Road Desk Mirror Installer
echo.
echo Road Desk Mirror - Win7 installer
echo.
if exist "%~dp0RoadDeskMirrorSetup.exe" (
  "%~dp0RoadDeskMirrorSetup.exe" %*
  set ERR=%ERRORLEVEL%
  exit /b %ERR%
)
echo RoadDeskMirrorSetup.exe not found - falling back to PowerShell script.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-RoadDeskMirror.ps1" %*
set ERR=%ERRORLEVEL%
if not "%ERR%"=="0" (
  echo.
  echo Install failed (exit %ERR%).
  pause
  exit /b %ERR%
)
exit /b 0
