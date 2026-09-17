@echo off
setlocal enabledelayedexpansion

set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat
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

rem The runtime loader lives with SteamVR; copy it next to the exe so the
rem LoadLibraryW fallback also works.
copy /y "C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\openxr_loader.dll" "%OUT%\" >nul 2>&1

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
cl /nologo /std:c++20 /EHsc /O2 /W3 /I"%INC%" /I"%ORTINC%" /Fo"%OUT%\\" /Fe"%OUT%\%APP5_NAME%.exe" "%~dp0xrapp5.cpp" ^
   /link /LIBPATH:"%ORTLIB%" onnxruntime.lib d3d12.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib ole32.lib windowscodecs.lib windowsapp.lib user32.lib
if errorlevel 1 (
  echo BUILD FAILED ^(xrapp5^)
  exit /b 1
)

copy /y "%ORTLIB%\onnxruntime.dll" "%OUT%\" >nul
for /d %%D in ("%USERPROFILE%\.nuget\packages\microsoft.ai.directml\*") do set DMLPKG2=%%D
if defined DMLPKG2 if exist "!DMLPKG2!\runtimes\win-x64\native\DirectML.dll" copy /y "!DMLPKG2!\runtimes\win-x64\native\DirectML.dll" "%OUT%\" >nul
echo BUILD OK: %OUT%\xrapp3.exe
