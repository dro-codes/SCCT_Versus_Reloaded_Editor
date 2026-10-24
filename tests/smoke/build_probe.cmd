@echo off
rem Builds SmokeProbe.dll from an x86 Native Tools Command Prompt for Visual Studio.
rem Usage: build_probe.cmd [output folder]   (default %TEMP%\scct-smoke)
setlocal
set OUT=%~1
if "%OUT%"=="" set OUT=%TEMP%\scct-smoke
if not exist "%OUT%" mkdir "%OUT%"
pushd "%~dp0"
cl /nologo /W4 /EHsc /std:c++20 /MT /O2 /LD /I "%~dp0..\..\Reloaded.Editor\Include" SmokeProbe.cpp /Fe:"%OUT%\SmokeProbe.dll" /Fo:"%OUT%\SmokeProbe.obj" /link user32.lib gdi32.lib comdlg32.lib comctl32.lib gdiplus.lib
set RESULT=%errorlevel%
popd
exit /b %RESULT%
