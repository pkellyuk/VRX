@echo off
setlocal enabledelayedexpansion
where cl >nul 2>&1
if not errorlevel 1 goto :ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS exit /b 1
call "!VCVARS!" >nul
:ready
if not exist "%~dp0out" mkdir "%~dp0out"
cl /nologo /std:c++20 /EHsc /O2 /W3 /I"%~dp0include" /Fo"%~dp0out\\" /Fe"%~dp0out\room_life_test.exe" "%~dp0room_life_test.cpp" /link ole32.lib
if errorlevel 1 exit /b 1
"%~dp0out\room_life_test.exe"
if errorlevel 1 exit /b 1
if "%~1"=="--cpu" exit /b 0
cl /nologo /std:c++20 /EHsc /O2 /W3 /I"%~dp0include" /Fo"%~dp0out\\" /Fe"%~dp0out\room_life_render_test.exe" "%~dp0room_life_render_test.cpp" /link d3d11.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
"%~dp0out\room_life_render_test.exe"
exit /b %errorlevel%
