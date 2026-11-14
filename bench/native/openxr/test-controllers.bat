@echo off
setlocal enabledelayedexpansion
rem Use an existing developer prompt, or locate the installed Visual Studio C++ tools.
rem No headset or models required.
where cl >nul 2>&1
if not errorlevel 1 goto :vsready
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS (
  echo ERROR: Visual Studio C++ tools not found
  exit /b 1
)
call "!VCVARS!" >nul
if errorlevel 1 exit /b 1
:vsready
if not exist "%~dp0out" mkdir "%~dp0out"
cl /nologo /std:c++20 /EHsc /W4 /I"%~dp0include" /Fo"%~dp0out\\" /Fe"%~dp0out\controller_menu_test.exe" "%~dp0controller_menu_test.cpp"
if errorlevel 1 exit /b 1
"%~dp0out\controller_menu_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /DNOMINMAX /I"%~dp0include" /Fo"%~dp0out\\" /Fe"%~dp0out\controller_menu_compile_test.exe" "%~dp0controller_menu_compile_test.cpp" /link d3d12.lib dxgi.lib user32.lib
if errorlevel 1 exit /b 1
"%~dp0out\controller_menu_compile_test.exe"
exit /b %errorlevel%
