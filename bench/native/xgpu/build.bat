@echo off
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at %VCVARS%
  exit /b 1
)
call "%VCVARS%" >nul
set OUT=%~dp0out
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++17 /EHsc /O2 /W3 /Fo"%OUT%\\" /Fe"%OUT%\xgpu_probe.exe" "%~dp0xgpu_probe.cpp" /link d3d12.lib dxgi.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo BUILD OK: %OUT%\xgpu_probe.exe
