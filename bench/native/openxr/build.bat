@echo off
setlocal enabledelayedexpansion

rem The newest Visual Studio with the C++ tools (vswhere), else the developer PC's VS 18.
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
echo === Visual Studio: %VCVARS%
if not exist "%VCVARS%" (
  echo ERROR: vcvars64.bat not found at %VCVARS%
  exit /b 1
)
call "%VCVARS%" >nul

set INC=%~dp0include
set OUT=%~dp0out
if defined VRX_NATIVE_OUT set "OUT=%VRX_NATIVE_OUT%"
set APP5_NAME=xrapp5
if "%~1"=="--desktop" set APP5_NAME=xrplayer
if not exist "%OUT%" mkdir "%OUT%"

echo === include: %INC%

cl /nologo /std:c++17 /EHsc /O2 /W3 /I"%INC%" /Fo"%OUT%\\" /Fe"%OUT%\xrprobe.exe" "%~dp0xrprobe.cpp" ^
   /link d3d11.lib dxgi.lib ole32.lib
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)

rem The OpenXR loader goes next to the exe (the engine loads it from there): the pinned,
rem Valve-signed copy in release\vendor, else SteamVR's own.
set "LOADER=%~dp0..\..\..\release\vendor\openxr_loader.dll"
if not exist "%LOADER%" set "LOADER=C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openxr_loader.dll"
copy /y "%LOADER%" "%OUT%\" >nul 2>&1

echo BUILD OK: %OUT%\xrprobe.exe

echo.
echo === building xrapp (OpenXR + D3D12 quad layer) ===
cl /nologo /std:c++17 /EHsc /O2 /W3 /I"%INC%" /Fo"%OUT%\\" /Fe"%OUT%\xrapp.exe" "%~dp0xrapp.cpp" ^
   /link d3d12.lib dxgi.lib dxguid.lib ole32.lib
if errorlevel 1 (
  echo BUILD FAILED ^(xrapp^)
  exit /b 1
)
echo BUILD OK: %OUT%\xrapp.exe

echo.
echo === building xrapp3 (depth model + projection layer + depth attachment) ===
set ORTPKG=%USERPROFILE%\.nuget\packages\microsoft.ml.onnxruntime.directml\1.24.4
set ORTINC=%ORTPKG%\build\native\include
set ORTLIB=%ORTPKG%\runtimes\win-x64\native

cl /nologo /std:c++17 /EHsc /O2 /W3 /I"%INC%" /I"%ORTINC%" /Fo"%OUT%\\" /Fe"%OUT%\xrapp3.exe" "%~dp0xrapp3.cpp" ^
   /link /LIBPATH:"%ORTLIB%" onnxruntime.lib d3d12.lib dxgi.lib dxguid.lib ole32.lib windowscodecs.lib
if errorlevel 1 (
  echo BUILD FAILED ^(xrapp3^)
  exit /b 1
)

echo.
echo === building xrapp4 (worker-thread depth + compute-shader warp) ===
cl /nologo /std:c++17 /EHsc /O2 /W3 /I"%INC%" /I"%ORTINC%" /Fo"%OUT%\\" /Fe"%OUT%\xrapp4.exe" "%~dp0xrapp4.cpp" ^
   /link /LIBPATH:"%ORTLIB%" onnxruntime.lib d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib ole32.lib windowscodecs.lib
if errorlevel 1 (
  echo BUILD FAILED ^(xrapp4^)
  exit /b 1
)

echo.
echo === building xrapp5 (desktop capture + GPU-resident model input + colour-res warp) ===
rem Version information (product name and version), required for code signing.
rc /nologo /fo "%OUT%\xrplayer.res" "%~dp0xrplayer.rc"
if errorlevel 1 (
  echo BUILD FAILED ^(xrplayer.rc^)
  exit /b 1
)
cl /nologo /std:c++20 /EHsc /O2 /W3 /I"%INC%" /I"%ORTINC%" /Fo"%OUT%\\" /Fe"%OUT%\%APP5_NAME%.exe" "%~dp0xrapp5.cpp" "%OUT%\xrplayer.res" ^
   /link /LIBPATH:"%ORTLIB%" onnxruntime.lib d3d12.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib ole32.lib windowscodecs.lib windowsapp.lib user32.lib dwmapi.lib shcore.lib
if errorlevel 1 (
  echo BUILD FAILED ^(xrapp5^)
  exit /b 1
)

copy /y "%ORTLIB%\onnxruntime.dll" "%OUT%\" >nul
for /d %%D in ("%USERPROFILE%\.nuget\packages\microsoft.ai.directml\*") do set DMLPKG2=%%D
if defined DMLPKG2 if exist "!DMLPKG2!\runtimes\win-x64\native\DirectML.dll" copy /y "!DMLPKG2!\runtimes\win-x64\native\DirectML.dll" "%OUT%\" >nul
rem The Windows SDK's redistributable shader compiler, next to the exe so that the
rem engine loads it instead of System32's (which changes with Windows updates): the
rem shipped shader cache is keyed by the compiler's version (xrapp5.cpp, CompileCs).
set "D3DCOMPILER=%WindowsSdkDir%Redist\D3D\x64\d3dcompiler_47.dll"
if not exist "%D3DCOMPILER%" set "D3DCOMPILER=%ProgramFiles(x86)%\Windows Kits\10\Redist\D3D\x64\d3dcompiler_47.dll"
if not exist "%D3DCOMPILER%" (
  echo BUILD FAILED ^(Windows SDK d3dcompiler_47.dll redistributable not found^)
  exit /b 1
)
copy /y "%D3DCOMPILER%" "%OUT%\" >nul
if errorlevel 1 (
  echo BUILD FAILED ^(cannot copy d3dcompiler_47.dll^)
  exit /b 1
)
echo BUILD OK: %OUT%\xrapp3.exe
