@echo off
setlocal
rem The MCP server's test suites. Run from an x86 Native Tools Command Prompt
rem for Visual Studio. tools/test_mcp.sh is the same list for g++; keep the two
rem in step. /bigobj: the spec compiler's units exceed 65,279 sections; /utf-8:
rem the server is built with it, and the tests compare UTF-8 text.
pushd "%~dp0.."
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
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\LevelSpecModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\LevelSpecModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\LevelSpecModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\LevelSpecCompilerTests.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\LevelSpecCompilerTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\LevelSpecCompilerTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\LevelRulesModelTests.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\LevelRulesModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\LevelRulesModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\GeometricEventModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\GeometricEventModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\GeometricEventModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\ImageCodecTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ImageCodecTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\ImageCodecTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\PlanRenderTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PlanRenderTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\PlanRenderTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\GridPlanModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\GridPlanModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\GridPlanModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\BrushImportModelTests.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\BrushImportModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\BrushImportModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\ImportersTests.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ImportersTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\ImportersTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\PromptsTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PromptsTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\PromptsTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpServerTests.cpp Reloaded.Editor.Mcp\McpServer.cpp Reloaded.Editor.Mcp\Resources.cpp Reloaded.Editor.Mcp\BridgeClient.cpp Reloaded.Editor.Mcp\Tools\SessionTools.cpp Reloaded.Editor.Mcp\Tools\QueryTools.cpp Reloaded.Editor.Mcp\Tools\EscapeTools.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpServerTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpServerTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpRecoverToolsTests.cpp Reloaded.Editor.Mcp\McpServer.cpp Reloaded.Editor.Mcp\Resources.cpp Reloaded.Editor.Mcp\BridgeClient.cpp Reloaded.Editor.Mcp\Tools\SessionTools.cpp Reloaded.Editor.Mcp\Tools\QueryTools.cpp Reloaded.Editor.Mcp\Tools\EscapeTools.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpRecoverToolsTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpRecoverToolsTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpSpecToolsTests.cpp Reloaded.Editor.Mcp\Tools\SpecTools.cpp Reloaded.Editor.Mcp\Tools\IntentTools.cpp Reloaded.Editor.Mcp\Tools\ValidateTools.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpSpecToolsTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpSpecToolsTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpMediaToolsTests.cpp Reloaded.Editor.Mcp\Media.cpp Reloaded.Editor.Mcp\Tools\MediaTools.cpp Reloaded.Editor.Mcp\Tools\ImportTools.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpMediaToolsTests.exe" /link /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll delayimp.lib
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpMediaToolsTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 tests\McpReleaseToolsTests.cpp Reloaded.Editor.Mcp\Tools\ReleaseTools.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpReleaseToolsTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\McpReleaseToolsTests.exe"
if not "%errorlevel%"=="0" goto failed
rem The whole server from the sources Reloaded.Editor.Mcp.vcxproj lists, which
rem also compiles main.cpp and ValidateTools.cpp under /W4 /WX.
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 /bigobj /utf-8 Reloaded.Editor.Mcp\main.cpp Reloaded.Editor.Mcp\McpServer.cpp Reloaded.Editor.Mcp\Resources.cpp Reloaded.Editor.Mcp\BridgeClient.cpp Reloaded.Editor.Mcp\Prompts.cpp Reloaded.Editor.Mcp\Media.cpp Reloaded.Editor.Mcp\Tools\SessionTools.cpp Reloaded.Editor.Mcp\Tools\QueryTools.cpp Reloaded.Editor.Mcp\Tools\ValidateTools.cpp Reloaded.Editor.Mcp\Tools\SpecTools.cpp Reloaded.Editor.Mcp\Tools\IntentTools.cpp Reloaded.Editor.Mcp\Tools\EscapeTools.cpp Reloaded.Editor.Mcp\Tools\ImportTools.cpp Reloaded.Editor.Mcp\Tools\MediaTools.cpp Reloaded.Editor.Mcp\Tools\ReleaseTools.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\McpServerSelfTest.exe" /link /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll delayimp.lib ole32.lib windowscodecs.lib mfplat.lib mfreadwrite.lib mfuuid.lib shlwapi.lib advapi32.lib shell32.lib
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
