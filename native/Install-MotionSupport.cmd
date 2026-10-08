@echo off
title Apple Music Presence - Motion Support
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-MotionSupport.ps1"
set "motion_exit=%errorlevel%"
if not "%motion_exit%"=="0" (
    echo.
    echo Motion support was not installed. Your app still supports normal covers.
)
echo.
pause
exit /b %motion_exit%
