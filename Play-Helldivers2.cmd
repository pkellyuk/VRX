@echo off
setlocal
title VRX - Helldivers 2
cd /d "%~dp0"
if not exist "bench\native\openxr\out\xrapp5.exe" (
    echo VRX has not been built. Run bench\native\openxr\build.bat first.
    pause
    exit /b 1
)
echo Start SteamVR and Helldivers 2 before continuing.
echo Playing for up to 30 minutes. Equals recenters the screen; F8 closes the SteamVR menu.
"bench\native\openxr\out\xrapp5.exe" 1800 --exe=helldivers2.exe
if errorlevel 1 (
    echo.
    echo VRX could not continue. See the message above. Make sure the game is open.
    pause
    exit /b 1
)
