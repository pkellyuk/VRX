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
cl /nologo /std:c++17 /EHsc /O2 /W3 /Fo"%OUT%\\" /Fe"%OUT%\me_probe.exe" "%~dp0me_probe.cpp" /link d3d12.lib dxgi.lib
if errorlevel 1 (
  echo BUILD FAILED: me_probe
  exit /b 1
)
cl /nologo /std:c++17 /EHsc /O2 /W3 /Fo"%OUT%\\" /Fe"%OUT%\sbs_render.exe" "%~dp0sbs_render.cpp"
if errorlevel 1 (
  echo BUILD FAILED: sbs_render
  exit /b 1
)
cl /nologo /std:c++17 /EHsc /O2 /W3 /Fo"%OUT%\\" /Fe"%OUT%\fusion_golden.exe" "%~dp0fusion_golden.cpp"
if errorlevel 1 (
  echo BUILD FAILED: fusion_golden
  exit /b 1
)
echo BUILD OK: %OUT%\me_probe.exe %OUT%\sbs_render.exe %OUT%\fusion_golden.exe
