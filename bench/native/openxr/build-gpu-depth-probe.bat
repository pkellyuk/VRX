@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
  echo ERROR: Visual Studio C++ tools not found.
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
set "OUT=%~dp0out\gpu-depth-probe"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /EHsc /O2 /W3 /Fo"%OUT%\\" /Fe"%OUT%\gpu_depth_probe.exe" "%~dp0gpu_depth_probe.cpp" /link d3d12.lib dxgi.lib d3dcompiler.lib
if errorlevel 1 exit /b %errorlevel%
copy /y "%~dp0..\..\..\release\vendor\d3dcompiler_47.dll" "%OUT%\d3dcompiler_47.dll" >nul
exit /b %errorlevel%
