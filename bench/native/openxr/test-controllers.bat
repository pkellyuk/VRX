@echo off
setlocal
rem Run from a Visual Studio developer prompt. No headset or models required.
if not exist "%~dp0out" mkdir "%~dp0out"
cl /nologo /std:c++20 /EHsc /W4 /I"%~dp0include" /Fo"%~dp0out\\" /Fe"%~dp0out\controller_menu_test.exe" "%~dp0controller_menu_test.cpp"
if errorlevel 1 exit /b 1
"%~dp0out\controller_menu_test.exe"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /W4 /DNOMINMAX /I"%~dp0include" /Fo"%~dp0out\\" /Fe"%~dp0out\controller_menu_compile_test.exe" "%~dp0controller_menu_compile_test.cpp" /link d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
"%~dp0out\controller_menu_compile_test.exe"
exit /b %errorlevel%
