@echo off
setlocal enabledelayedexpansion

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at %VCVARS%
  exit /b 1
)
call "%VCVARS%" >nul

set PKG=%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime.directml\1.24.4
set INC=%PKG%\build\native\include
set LIBDIR=%PKG%\runtimes\win-x64\native

set OUT=%~dp0out
if not exist "%OUT%" mkdir "%OUT%"

echo === include: %INC%
echo === libdir : %LIBDIR%

cl /nologo /std:c++17 /EHsc /O2 /W3 /I"%INC%" /Fo"%OUT%\\" /Fe"%OUT%\dmlgpu.exe" "%~dp0dmlgpu.cpp" ^
   /link /LIBPATH:"%LIBDIR%" onnxruntime.lib d3d12.lib dxgi.lib dxguid.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

copy /y "%LIBDIR%\onnxruntime.dll" "%OUT%\" >nul

rem DirectML.dll comes from the Microsoft.AI.DirectML package
for /d %%D in ("%USERPROFILE%\.nuget\packages\microsoft.ai.directml\*") do set DMLPKG=%%D
if defined DMLPKG (
  if exist "!DMLPKG!\runtimes\win-x64\native\DirectML.dll" (
    copy /y "!DMLPKG!\runtimes\win-x64\native\DirectML.dll" "%OUT%\" >nul
    echo === DirectML.dll from !DMLPKG!
  )
)

echo BUILD OK: %OUT%\dmlgpu.exe
