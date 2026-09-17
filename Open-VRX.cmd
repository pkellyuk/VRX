@echo off
setlocal
cd /d "%~dp0"
if not exist "desktop\VRX.Desktop\bin\Release\net10.0-windows\VRX.Desktop.exe" (
    echo Build the desktop app first using desktop\build-desktop.cmd.
    pause
    exit /b 1
)
start "" "desktop\VRX.Desktop\bin\Release\net10.0-windows\VRX.Desktop.exe"
