@echo off
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\pythonw.exe" (
    echo Run the setup commands in README.md first to create .venv and install the app.
    pause
    exit /b 1
)
start "Apple Music Presence" ".venv\Scripts\pythonw.exe" -m apple_music_presence
