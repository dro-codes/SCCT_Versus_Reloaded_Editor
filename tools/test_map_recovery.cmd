@echo off
setlocal
rem Run from an x86 Native Tools Command Prompt for Visual Studio.
pushd "%~dp0.."
rem An older checkout's vcpkg install is vcpkg_installed\<triplet>; a fresh
rem one nests the triplet twice.
set ZLIB=vcpkg_installed\x86-windows-static
if not exist %ZLIB%\include\zlib.h set ZLIB=vcpkg_installed\x86-windows-static\x86-windows-static
cl /nologo /std:c++17 /W4 /WX /EHsc /O2 tests\CollisionBoxTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\CollisionBoxTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\CollisionBoxTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++17 /W4 /WX /EHsc /O2 tests\RecoveredSoftBodySettingsTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\RecoveredSoftBodySettingsTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\RecoveredSoftBodySettingsTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /O2 tests\BspLeafLightTableTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\BspLeafLightTableTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\BspLeafLightTableTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++17 /W4 /WX /EHsc /O2 tests\RecoveredBspGeometryTests.cpp Reloaded.Editor\RecoveredBspGeometry.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\RecoveredBspGeometryTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\RecoveredBspGeometryTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /O2 tests\MapRecoveryModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MapRecoveryModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\MapRecoveryModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /O2 tests\RecoveredSurfaceMatchTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\RecoveredSurfaceMatchTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\RecoveredSurfaceMatchTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /O2 tests\RecoveredLightModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\RecoveredLightModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\RecoveredLightModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++17 /W4 /WX /EHsc /O2 tests\RecoveredActorImportTests.cpp Reloaded.Editor\RecoveredActorImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\RecoveredActorImportTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\RecoveredActorImportTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++17 /W4 /WX /EHsc /O2 tests\RecoveredSurfacePartitionTests.cpp Reloaded.Editor\RecoveredSurfacePartition.cpp Reloaded.Editor\RecoveredPolygonImport.cpp Reloaded.Editor\RecoveredBspGeometry.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\RecoveredSurfacePartitionTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\RecoveredSurfacePartitionTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++17 /W4 /WX /EHsc /O2 tests\RecoveredPolygonImportTests.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\RecoveredPolygonImportTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\RecoveredPolygonImportTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++17 /W4 /WX /EHsc /O2 tests\SdcBlockModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\SdcBlockModelTests.exe"
if not "%errorlevel%"=="0" goto failed
"%TEMP%\SdcBlockModelTests.exe"
if not "%errorlevel%"=="0" goto failed
cl /nologo /std:c++17 /W4 /WX /EHsc /O2 /I %ZLIB%\include tests\RecoveredAssetPackageTests.cpp Reloaded.Editor\RecoveredAssetPackage.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\RecoveredAssetPackageTests.exe" /link /LIBPATH:%ZLIB%\lib zlib.lib
if not "%errorlevel%"=="0" goto failed
"%TEMP%\RecoveredAssetPackageTests.exe"
if not "%errorlevel%"=="0" goto failed
popd
exit /b 0
:failed
popd
exit /b 1
