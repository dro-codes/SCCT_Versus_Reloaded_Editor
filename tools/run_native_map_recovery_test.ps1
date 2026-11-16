param(
    [Parameter(Mandatory = $true)][string]$EditorDll,
    [string]$SourceMap,
    [string]$GameSystem = $env:SCCT,
    [string]$StartupConfigSystem,
    [ValidatePattern('^[A-Za-z_][A-Za-z0-9_]*$')][string]$OutputName = 'NativeRecovered_Source',
    [int]$TimeoutSeconds = 300,
    [switch]$PrepareOnly,
    [switch]$GenerateFixture,
    [switch]$WorkflowTools,
    [switch]$BrushGridSnapOnly,
    [switch]$CharacterPreviewOnly,
    [switch]$StairSmoothOnly,
    [switch]$OptimiseAssetsOnly,
    # A map copied into the disposable MapsEd for the Optimise Map Assets full-size check.
    [string]$OptimiseExtraMap,
    # Only the MCP bridge section (tests/McpBridgeNativeNotes.md).
    [switch]$McpBridgeOnly,
    [switch]$ImportBaseline,
    [switch]$ReopenOnly,
    [switch]$RootOutside,
    [switch]$ExpectRecoveryFailure,
    [switch]$EditGeometry,
    [switch]$ImportTextOnly,
    [switch]$BuildImportedText,
    [switch]$RestoreTextNormals,
    [switch]$TraceSoftBodies,
    [switch]$VisibleEditor,
    [switch]$RecoveryMenu,
    [switch]$FpuBoundsProbe,
    [switch]$StepCookedSoftBodies,
    [switch]$TraceLeafLights,
    [switch]$TraceLighting,
    [switch]$VerifyPreservedLighting,
    [switch]$VerifySelectedLighting,
    [string]$LightingBrush,
    [switch]$MatchLightingAfterLoad,
    [switch]$GeometryOnlyBuild,
    [switch]$VerifyPlayMapSave,
    [switch]$RelightCookedMeshes,
    [switch]$AllLeafLightCandidates,
    [switch]$SkipMeshShadowOcclusion,
    [switch]$InspectCookedOnly,
    [switch]$CompactPoints,
    [string]$TracePoint,
    [string]$TraceActor,
    [string[]]$ExtraAssetPackage = @(),
    # Recover with File > Merge Recovered Geometry off.
    [switch]$NoMerge,
    # Act as if Cancel were pressed when this MapRecoveryModel::Stage starts
    # (0 Preparing ... 10 Paths), then check the editor was left clean.
    [ValidateRange(0, 10)][int]$CancelAtStage = -1
)
$ErrorActionPreference = 'Stop'
if ($VerifyPreservedLighting -and !$ReopenOnly) { throw 'VerifyPreservedLighting requires ReopenOnly and a recovered source with nontrivial lighting.' }
if ($VerifySelectedLighting -and (!$ReopenOnly -or !$VerifyPreservedLighting)) { throw 'VerifySelectedLighting requires ReopenOnly and VerifyPreservedLighting.' }
if ($MatchLightingAfterLoad -and (!$ReopenOnly -or !$LightingBrush)) { throw 'MatchLightingAfterLoad requires ReopenOnly and LightingBrush.' }
if ($GeometryOnlyBuild -and !$ReopenOnly) { throw 'GeometryOnlyBuild requires ReopenOnly.' }
if ($VerifyPlayMapSave -and !$ReopenOnly) { throw 'VerifyPlayMapSave requires ReopenOnly.' }
if ($StepCookedSoftBodies -and !$TraceSoftBodies) { throw 'StepCookedSoftBodies requires TraceSoftBodies.' }
if ($RecoveryMenu -and ($GenerateFixture -or $ReopenOnly -or $ImportTextOnly -or $ExpectRecoveryFailure -or $WorkflowTools -or $BrushGridSnapOnly)) { throw 'RecoveryMenu requires a standalone compiled map recovery test.' }
$repository = Split-Path -Parent $PSScriptRoot
$nativeDll = (Resolve-Path -LiteralPath $EditorDll).Path
if ($RelightCookedMeshes -and (!$InspectCookedOnly -or !$TraceLighting)) { throw 'RelightCookedMeshes requires InspectCookedOnly and TraceLighting.' }
if ($AllLeafLightCandidates -and !$RelightCookedMeshes) { throw 'AllLeafLightCandidates requires RelightCookedMeshes.' }
if ($SkipMeshShadowOcclusion -and !$RelightCookedMeshes) { throw 'SkipMeshShadowOcclusion requires RelightCookedMeshes.' }
if ($ImportBaseline -and !$GenerateFixture) { throw 'ImportBaseline requires GenerateFixture.' }
if ($ImportTextOnly -and ($GenerateFixture -or $ReopenOnly -or $ExpectRecoveryFailure -or $EditGeometry)) { throw 'ImportTextOnly requires a standalone T3D input.' }
if ($BuildImportedText -and !$ImportTextOnly) { throw 'BuildImportedText requires ImportTextOnly.' }
if ($ReopenOnly -and $GenerateFixture) { throw 'ReopenOnly requires an existing source map.' }
if ($EditGeometry -and ($ReopenOnly -or $ImportBaseline -or $ExpectRecoveryFailure)) { throw 'EditGeometry requires a successful recovery test.' }
if ($RootOutside -and !$GenerateFixture) { throw 'RootOutside requires GenerateFixture.' }
if ($ExpectRecoveryFailure -and ($ReopenOnly -or $ImportBaseline)) { throw 'ExpectRecoveryFailure requires a recovery test.' }
if ($ExpectRecoveryFailure -and !$GenerateFixture) { throw 'ExpectRecoveryFailure requires GenerateFixture for the normal-load follow-up.' }
if (!$GenerateFixture -and !$SourceMap) { throw 'Supply SourceMap or GenerateFixture.' }
if ($InspectCookedOnly -and ($GenerateFixture -or $ReopenOnly -or $ImportTextOnly -or $EditGeometry -or $ExpectRecoveryFailure)) { throw 'InspectCookedOnly requires a standalone compiled map.' }
$nativeSource = if ($GenerateFixture) { $null } else { (Resolve-Path -LiteralPath $SourceMap).Path }
$installedSystem = (Resolve-Path -LiteralPath $GameSystem).Path
$installedRoot = Split-Path -Parent $installedSystem
$testRoot = Join-Path $env:TEMP ('scct-source-recovery-' + [Guid]::NewGuid().ToString('N'))
$testSystem = Join-Path $testRoot 'System'
$testPackages = Join-Path $testRoot 'Packages'
$null = New-Item -ItemType Directory -Path $testSystem, $testPackages

# Every executable, DLL, configuration, source map and output map is a copy.
# Only immutable asset directories are shared; no editor command targets them.
Get-ChildItem -LiteralPath $installedSystem -File | Where-Object {
    $_.Extension -in '.exe', '.dll', '.ini', '.int', '.dat', '.config', '.bmp'
} | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $testSystem }
if ($StartupConfigSystem) {
    Get-ChildItem -LiteralPath (Resolve-Path -LiteralPath $StartupConfigSystem).Path -File -Filter '*.ini' |
        Where-Object { $_.Name -ne 'native_recovery_test.ini' } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $testSystem -Force }
}
foreach ($name in 'EditorRes', '_PC_') {
    if (Test-Path -LiteralPath (Join-Path $installedSystem $name)) {
        Copy-Item -LiteralPath (Join-Path $installedSystem $name) -Destination $testSystem -Recurse
    }
}
foreach ($name in 'Animations', 'Sounds', 'Textures') {
    $assetSource = Join-Path (Join-Path $installedRoot 'Packages') $name
    if (Test-Path -LiteralPath $assetSource) {
        $null = New-Item -ItemType Junction -Path (Join-Path $testPackages $name) -Target $assetSource
    }
}
# Recovery may emit a new external mesh package. Give that directory its own
# files so a test can neither replace nor add anything to the user's assets.
$meshSource = Join-Path (Join-Path $installedRoot 'Packages') 'StaticMeshes'
if (Test-Path -LiteralPath $meshSource) {
    Copy-Item -LiteralPath $meshSource -Destination $testPackages -Recurse
}
foreach ($assetPackage in $ExtraAssetPackage) {
    Copy-Item -LiteralPath (Resolve-Path -LiteralPath $assetPackage).Path -Destination (Join-Path $testPackages 'StaticMeshes')
}
if (Test-Path -LiteralPath (Join-Path $installedRoot 'Menus')) {
    $null = New-Item -ItemType Junction -Path (Join-Path $testRoot 'Menus') -Target (Join-Path $installedRoot 'Menus')
}
$inputMaps = Join-Path $testPackages 'Maps'
$outputMaps = Join-Path $testPackages 'MapsEd'
$null = New-Item -ItemType Directory -Path $inputMaps, $outputMaps
$inputMap = Join-Path $(if ($ReopenOnly) { $outputMaps } else { $inputMaps }) $(if ($GenerateFixture) { 'NativeFixture.sdc' } else { [IO.Path]::GetFileName($nativeSource) })
$outputMap = Join-Path $outputMaps ($OutputName + '.sdc')
if ($ExpectRecoveryFailure) {
    # Fail geometry interchange writing after cooked loading/reconstruction.
    # The input package itself remains a valid native map.
    $null = New-Item -ItemType Directory -Path (Join-Path $outputMaps ('Recovery\' + $OutputName + '\Geometry.t3d')) -Force
}
if (!$GenerateFixture) { Copy-Item -LiteralPath $nativeSource -Destination $inputMap }
if ($OptimiseExtraMap) { Copy-Item -LiteralPath (Resolve-Path -LiteralPath $OptimiseExtraMap).Path -Destination $outputMaps }
$injectedDll = Join-Path $testSystem 'Reloaded.Editor.dll'
Copy-Item -LiteralPath $nativeDll -Destination $injectedDll -Force

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw 'Visual Studio C++ tools are required for the native test.' }
$devCommand = Join-Path $visualStudio 'Common7/Tools/VsDevCmd.bat'
$compileScript = Join-Path $testSystem 'compile_test.cmd'
@"
@call "$devCommand" -arch=x86 -host_arch=x64 >nul
@if errorlevel 1 exit /b 1
@cl /nologo /W4 /EHsc /std:c++20 /MT /O2 "$repository\tests\NativeMapRecoveryDriver.cpp" /Fe:"$testSystem\NativeMapRecoveryDriver.exe" /Fo:"$testSystem\NativeMapRecoveryDriver.obj" /link user32.lib
@if errorlevel 1 exit /b 1
@cl /nologo /W4 /EHsc /std:c++20 /MT /O2 /LD "$repository\tests\NativeMapRecoveryProbe.cpp" /Fe:"$testSystem\NativeMapRecoveryProbe.dll" /Fo:"$testSystem\NativeMapRecoveryProbe.obj" /link user32.lib gdi32.lib comdlg32.lib
@exit /b %errorlevel%
"@ | Set-Content -LiteralPath $compileScript -Encoding ascii
& cmd.exe /d /c $compileScript
if ($LASTEXITCODE) { throw "Native test build failed; files retained at $testRoot" }
@"
[test]
source=$inputMap
destination=$outputMap
editor_dll=$injectedDll
generate_fixture=$([int][bool]$GenerateFixture)
workflow_tools=$([int][bool]($WorkflowTools -or $BrushGridSnapOnly -or $CharacterPreviewOnly -or $StairSmoothOnly -or $OptimiseAssetsOnly -or $McpBridgeOnly))
brush_grid_snap_only=$([int][bool]$BrushGridSnapOnly)
character_preview_only=$([int][bool]$CharacterPreviewOnly)
stair_smooth_only=$([int][bool]$StairSmoothOnly)
optimise_only=$([int][bool]$OptimiseAssetsOnly)
mcp_bridge_only=$([int][bool]$McpBridgeOnly)
import_baseline=$([int][bool]$ImportBaseline)
reopen_only=$([int][bool]$ReopenOnly)
root_outside=$([int][bool]$RootOutside)
expect_recovery_failure=$([int][bool]$ExpectRecoveryFailure)
edit_geometry=$([int][bool]$EditGeometry)
import_text_only=$([int][bool]$ImportTextOnly)
build_imported_text=$([int][bool]$BuildImportedText)
restore_text_normals=$([int][bool]$RestoreTextNormals)
trace_soft_bodies=$([int][bool]$TraceSoftBodies)
visible_editor=$([int][bool]$VisibleEditor)
recovery_menu=$([int][bool]$RecoveryMenu)
fpu_bounds_probe=$([int][bool]$FpuBoundsProbe)
step_cooked_soft_bodies=$([int][bool]$StepCookedSoftBodies)
trace_leaf_lights=$([int][bool]$TraceLeafLights)
trace_lighting=$([int][bool]$TraceLighting)
verify_preserved_lighting=$([int][bool]$VerifyPreservedLighting)
verify_selected_lighting=$([int][bool]$VerifySelectedLighting)
lighting_brush=$LightingBrush
match_lighting_after_load=$([int][bool]$MatchLightingAfterLoad)
geometry_only_build=$([int][bool]$GeometryOnlyBuild)
verify_play_map_save=$([int][bool]$VerifyPlayMapSave)
relight_cooked_meshes=$([int][bool]$RelightCookedMeshes)
all_leaf_light_candidates=$([int][bool]$AllLeafLightCandidates)
skip_mesh_shadow_occlusion=$([int][bool]$SkipMeshShadowOcclusion)
inspect_cooked_only=$([int][bool]$InspectCookedOnly)
compact_points=$([int][bool]$CompactPoints)
trace_point=$TracePoint
trace_actor=$TraceActor
recovery_merge=$([int](!$NoMerge))
"@ | Set-Content -LiteralPath (Join-Path $testSystem 'native_recovery_test.ini') -Encoding ascii
if ($CancelAtStage -ge 0) {
    Add-Content -LiteralPath (Join-Path $testSystem 'native_recovery_test.ini') -Value "recovery_cancel_stage=$CancelAtStage" -Encoding ascii
}
# The self-updater's start-up check would reach GitHub and could open a
# message box the probe cannot answer.
Add-Type -Namespace NativeRecoveryTest -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
'@
if (![NativeRecoveryTest.Ini]::WritePrivateProfileString('Updates', 'CheckOnStartup', '0', (Join-Path $testSystem 'Reloaded_Editor.ini'))) {
    throw 'Could not turn off the update check in the test installation.'
}
# The workflow suite stops the editor and starts it again; the crash-recovery
# offer that start would make is a message box the probe does not answer.
if (![NativeRecoveryTest.Ini]::WritePrivateProfileString('CrashRecovery', 'OfferAtStartup', '0', (Join-Path $testSystem 'Reloaded_Editor.ini'))) {
    throw 'Could not turn off the crash-recovery offer in the test installation.'
}
Write-Output "TestRoot=$testRoot"
if ($PrepareOnly) { return }

$testExecutable = Join-Path $testSystem 'ChaosTheory_Editor.exe'
$driver = Join-Path $testSystem 'NativeMapRecoveryDriver.exe'
$probe = Join-Path $testSystem 'NativeMapRecoveryProbe.dll'
$launchedId = & $driver $testExecutable $injectedDll $probe
if ($LASTEXITCODE) { throw "Isolated editor launch failed; files retained at $testRoot" }
$editorProcessId = [int]($launchedId | Select-Object -Last 1)
$report = Join-Path $testSystem 'native_recovery_report.txt'
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$workflowRestarted = $false
# Report lines are streamed as the probe writes them, so progress and every
# failed check show while the editor is still running.
$streamed = 0
try {
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $report) {
            $lines = @(Get-Content -LiteralPath $report)
            if ($lines.Count -gt $streamed) {
                for ($i = $streamed; $i -lt $lines.Count; $i++) {
                    $line = $lines[$i]
                    if ($line -match '^workflow_fail ') { Write-Output ('  !! FAILED CHECK: ' + $line.Substring(14)) }
                    elseif ($line -match '^workflow_check ') { Write-Output ('  ok ' + $line.Substring(15)) }
                    elseif ($line -match '^workflow ') { }
                    else { Write-Output $line }
                }
                $streamed = $lines.Count
            }
            $result = ($lines -join "`n")
            if ($result -match '(?m)^(PASS|FAIL) ') {
                $failed = @($lines | Where-Object { $_ -match '^workflow_fail ' })
                if ($failed.Count) { Write-Output ('' + $failed.Count + ' check(s) failed:'); $failed | ForEach-Object { Write-Output ('  ' + $_.Substring(14)) } }
                if ($result -match '(?m)^FAIL ') { throw 'Native map recovery test failed.' }
                if ($WorkflowTools -and !$BrushGridSnapOnly -and !$CharacterPreviewOnly -and !$StairSmoothOnly -and !$OptimiseAssetsOnly -and !$McpBridgeOnly -and !$workflowRestarted) {
                    $owned = Get-Process -Id $editorProcessId -ErrorAction Stop
                    if ($owned.Path -ne $testExecutable) { throw 'Unexpected isolated editor path.' }
                    Stop-Process -Id $editorProcessId -Force
                    $owned.WaitForExit(10000) | Out-Null
                    Move-Item -LiteralPath $report -Destination (Join-Path $testSystem 'workflow_first_process_report.txt')
                    $streamed = 0
                    $workflowConfig = Join-Path $testSystem 'native_recovery_test.ini'
                    $workflowText = (Get-Content -LiteralPath $workflowConfig -Raw).Replace('generate_fixture=1','generate_fixture=0')
                    Set-Content -LiteralPath $workflowConfig -Value ($workflowText + "`r`nworkflow_restart=1`r`n") -Encoding ascii
                    $launchedId = & $driver $testExecutable $injectedDll $probe
                    if ($LASTEXITCODE) { throw 'Workflow restart launch failed.' }
                    $editorProcessId = [int]($launchedId | Select-Object -Last 1)
                    $workflowRestarted = $true
                    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
                    continue
                }
                return
            }
        }
        if (!(Get-Process -Id $editorProcessId -ErrorAction SilentlyContinue)) {
            throw 'Isolated editor exited before finishing; inspect retained diagnostics.'
        }
        $crash = Get-ChildItem -LiteralPath (Join-Path $testSystem 'Diagnostics') -Filter 'EditorCrash_*.log' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($crash) {
            # The report is created before MiniDumpWriteDump runs. Allow the
            # diagnostic writer to finish before terminating this process.
            $dumpDeadline = [DateTime]::UtcNow.AddSeconds(30)
            while ([DateTime]::UtcNow -lt $dumpDeadline) {
                if ((Get-Content -LiteralPath $crash.FullName -Raw) -match '(?m)^MiniDump=') { break }
                if (!(Get-Process -Id $editorProcessId -ErrorAction SilentlyContinue)) { break }
                Start-Sleep -Milliseconds 500
            }
            Get-Content -LiteralPath $crash.FullName -TotalCount 10
            throw 'Isolated editor crashed; native report and dump retained.'
        }
        Start-Sleep -Milliseconds 500
    }
    if (Test-Path -LiteralPath $report) { Get-Content -LiteralPath $report }
    throw 'Native map recovery test timed out; inspect retained diagnostics.'
} finally {
    $remaining = Get-Process -Id $editorProcessId -ErrorAction SilentlyContinue
    # The process is disposable. Verify its actual executable before stopping it;
    # this never closes an already-running user editor or flushes its config.
    if ($remaining -and $remaining.Path -eq $testExecutable) {
        Stop-Process -Id $editorProcessId -Force
    }
}
