@echo off
setlocal
cd /d "%~dp0.."
call bench\native\openxr\build.bat --desktop
if errorlevel 1 exit /b 1
rem Stale C++ LIB paths are irrelevant to the managed desktop build.
set LIB=
dotnet build desktop\VRX.Desktop\VRX.Desktop.csproj -c Release --nologo
exit /b %errorlevel%
