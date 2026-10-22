@echo off
setlocal
rem The MCP server's test suites. Run from an x86 Native Tools Command Prompt
rem for Visual Studio. tools/test_mcp.sh is the same list for g++; keep the two
rem in step. /utf-8: the server is built with it, and the tests compare UTF-8
rem text.
pushd "%~dp0.."
rem Embedded.gen.h is generated, not committed: write it from
rem Reloaded.Editor.Mcp\Resources the way the Visual Studio build does.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\mcp\embed_resources.ps1
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpBridgeModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpBridgeModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpBridgeModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpPlanModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpPlanModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpPlanModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpOpCoverageTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpOpCoverageTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpOpCoverageTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\MapCheckJsonModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MapCheckJsonModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\MapCheckJsonModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpProtocolModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpProtocolModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpProtocolModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\PromptsTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PromptsTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\PromptsTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpServerTests.cpp Reloaded.Editor.Mcp\McpServer.cpp Reloaded.Editor.Mcp\Resources.cpp Reloaded.Editor.Mcp\BridgeClient.cpp Reloaded.Editor.Mcp\Tools\SessionTools.cpp Reloaded.Editor.Mcp\Tools\QueryTools.cpp Reloaded.Editor.Mcp\Tools\EscapeTools.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpServerTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpServerTests.exe"
if not "%errorlevel%"=="0" goto failed
rem The whole server from the sources Reloaded.Editor.Mcp.vcxproj lists, which
rem also compiles main.cpp under /W4 /WX.
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 Reloaded.Editor.Mcp\main.cpp Reloaded.Editor.Mcp\McpServer.cpp Reloaded.Editor.Mcp\Resources.cpp Reloaded.Editor.Mcp\BridgeClient.cpp Reloaded.Editor.Mcp\Prompts.cpp Reloaded.Editor.Mcp\Tools\SessionTools.cpp Reloaded.Editor.Mcp\Tools\QueryTools.cpp Reloaded.Editor.Mcp\Tools\EscapeTools.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpServerSelfTest.exe" /link advapi32.lib shell32.lib
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpServerSelfTest.exe" --version
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpServerSelfTest.exe" --self-test
if not "%errorlevel%"=="0" goto failed
popd
exit /b 0
:failed
popd
exit /b 1
