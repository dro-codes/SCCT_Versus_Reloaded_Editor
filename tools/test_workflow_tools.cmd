@echo off
setlocal
rem Run from an x86 Native Tools Command Prompt for Visual Studio.
pushd "%~dp0.."
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MapDesignModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MapDesignModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MapDesignModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MeasureModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MeasureModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MeasureModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\PlacementModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PlacementModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\PlacementModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\ScatterModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ScatterModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\ScatterModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\LevelSnapshotModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\LevelSnapshotModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\LevelSnapshotModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\EntryThumbnailModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\EntryThumbnailModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\EntryThumbnailModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\WindowPlacementModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\WindowPlacementModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\WindowPlacementModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\ViewBookmarkModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ViewBookmarkModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\ViewBookmarkModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\PrefabModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PrefabModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\PrefabModelTests.exe"
if errorlevel 1 goto failed
rem Not named *Update*: Windows would demand elevation for it as an installer.
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MapUsagesModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MapUsagesModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MapUsagesModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\SelfUpdaterModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ReleaseModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\ReleaseModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\CrashRecoveryModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\CrashRecoveryModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\CrashRecoveryModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\SaveBackupModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\SaveBackupModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\SaveBackupModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\UnsavedChangesModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\UnsavedChangesModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\UnsavedChangesModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MeshFavoritesModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MeshFavoritesModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MeshFavoritesModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\FavoritesModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\FavoritesModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\FavoritesModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\FavoritesWindowModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\FavoritesWindowModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\FavoritesWindowModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\UndoHistoryModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\UndoHistoryModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\UndoHistoryModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\AnimImportOptionsModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\AnimImportOptionsModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\AnimImportOptionsModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\PropertyFilterModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PropertyFilterModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\PropertyFilterModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\PropertyPasteModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PropertyPasteModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\PropertyPasteModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\AssetNameGlossModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\AssetNameGlossModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\AssetNameGlossModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\SoundFavoritesModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\SoundFavoritesModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\SoundFavoritesModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\BrushGridSnapTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\BrushGridSnapTests.exe"
if errorlevel 1 goto failed
"%TEMP%\BrushGridSnapTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\ViewportKeysModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ViewportKeysModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\ViewportKeysModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\GridSizeShortcutTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\GridSizeShortcutTests.exe"
if errorlevel 1 goto failed
"%TEMP%\GridSizeShortcutTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\LocalLightingMatchTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\LocalLightingMatchTests.exe"
if errorlevel 1 goto failed
"%TEMP%\LocalLightingMatchTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\PlayLevelCommandTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PlayLevelCommandTests.exe"
if errorlevel 1 goto failed
"%TEMP%\PlayLevelCommandTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\PlayLevelConfigTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PlayLevelConfigTests.exe" /link user32.lib
if errorlevel 1 goto failed
"%TEMP%\PlayLevelConfigTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MapAuthoringModelTests.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MapAuthoringModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MapAuthoringModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\CompareBackupModelTests.cpp Reloaded.Editor\WorkflowModel.cpp Reloaded.Editor\RecoveredPolygonImport.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\CompareBackupModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\CompareBackupModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\SecurityModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\SecurityModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\SecurityModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\StageModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\StageModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\StageModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\CameraNetworkModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\CameraNetworkModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\CameraNetworkModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MagicEventModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MagicEventModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MagicEventModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\WorkflowModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\WorkflowModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\WorkflowModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\UsageMarksModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\UsageMarksModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\UsageMarksModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\EmitterLibraryModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\EmitterLibraryModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\EmitterLibraryModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\EmitterPreviewModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\EmitterPreviewModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\EmitterPreviewModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\EmitterEditModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\EmitterEditModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\EmitterEditModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\WorkflowGraphTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\WorkflowGraphTests.exe"
if errorlevel 1 goto failed
"%TEMP%\WorkflowGraphTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\SelectionModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\SelectionModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\SelectionModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\LightingBudgetModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\LightingBudgetModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\LightingBudgetModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MapCheckModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MapCheckModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MapCheckModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\LightShadowModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\LightShadowModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\LightShadowModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\ReachabilityModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ReachabilityModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\ReachabilityModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\RenderBudgetModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\RenderBudgetModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\RenderBudgetModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\CharacterSkinsModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\CharacterSkinsModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\CharacterSkinsModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\ObjectivePlayersModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ObjectivePlayersModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\ObjectivePlayersModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\ConveyorModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ConveyorModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MoverModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MoverModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MoverModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\ConveyorModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\LargeAddressModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\LargeAddressModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\LargeAddressModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\PackageLoadFixModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\PackageLoadFixModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\PackageLoadFixModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\CharacterSkinPresetsModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\CharacterSkinPresetsModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\CharacterSkinPresetsModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MinimapModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MinimapModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MinimapModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\CharacterPreviewModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\CharacterPreviewModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\CharacterPreviewModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\StairSmoothModelTests.cpp Reloaded.Editor\WorkflowModel.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\StairSmoothModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\StairSmoothModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\SoftBodyStepModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\SoftBodyStepModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\SoftBodyStepModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\MapOutlinerModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\MapOutlinerModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\MapOutlinerModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\AppearanceModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\AppearanceModelTests.exe" /link user32.lib gdi32.lib
if errorlevel 1 goto failed
"%TEMP%\AppearanceModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\CommandPaletteModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\CommandPaletteModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\CommandPaletteModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\ObjExportModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ObjExportModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\ObjExportModelTests.exe"
if errorlevel 1 goto failed
cl /nologo /std:c++20 /W4 /WX /EHsc /MT /O2 tests\ObjImportModelTests.cpp /Fo"%TEMP%\\" /Fe"%TEMP%\ObjImportModelTests.exe"
if errorlevel 1 goto failed
"%TEMP%\ObjImportModelTests.exe"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
