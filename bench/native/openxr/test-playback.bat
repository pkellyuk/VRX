@echo off
setlocal
where cl >nul 2>&1
if not errorlevel 1 goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
:build
if not exist "%~dp0out" mkdir "%~dp0out"
cl /nologo /std:c++17 /EHsc /O2 /W4 /WX /I"%~dp0include" /Fo"%~dp0out\playback_test.obj" /Fe"%~dp0out\playback_test.exe" "%~dp0playback_test.cpp" /link d3d11.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
"%~dp0out\playback_test.exe"
exit /b %errorlevel%
