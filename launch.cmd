@echo off
setlocal
cd /d "%~dp0"
set "presence_exe=%~dp0AppleMusicPresenceNative.exe"
if not exist "%presence_exe%" set "presence_exe=%~dp0build\native\Release\AppleMusicPresenceNative.exe"
if not exist "%presence_exe%" (
    echo Download and extract the Windows ZIP from the GitHub releases page,
    echo or build the app with native\build.ps1. See README.md for instructions.
    pause
    exit /b 1
)
start "Apple Music Presence" "%presence_exe%" %*
