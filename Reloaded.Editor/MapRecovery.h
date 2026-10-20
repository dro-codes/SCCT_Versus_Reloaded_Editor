#pragma once

#include "MapRecoveryModel.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace MapRecovery
{
    constexpr UINT kCommandId = 40903;
    // Retained command identifiers; 40904 now converts legacy recovery files.
    constexpr UINT kOpenRecoveredCommandId = 40904;
    constexpr UINT kExportBrushesCommandId = 40905;
    constexpr UINT kRecoverEditableCommandId = 40906;
    // File > Merge Recovered Geometry (checked): block 41300-41319.
    constexpr UINT kMergeGeometryCommandId = 41300;

    void Run(HWND owner);
    void RunEditable(HWND owner);
    void OpenRecovered(HWND owner);
    void ExportRecoveredBrushes(HWND owner);
    // Adds File > Merge Recovered Geometry after the recovery commands.
    void InstallMenu(HMENU file);
    bool HandleCommand(UINT command);
    // [MapRecovery] MergeGeometry in Reloaded_Editor.ini; on by default.
    bool MergeGeometryEnabled();
    void SetMergeGeometry(bool enabled);

    struct RecoveryOptions
    {
        // Join and grow the compiled BSP's convex cells into fewer brushes and
        // combine matching surface pieces. Off: only cells sharing whole
        // faces are joined (recovery's earlier behaviour).
        bool mergeGeometry = true;
        // Keep the compiled map's Xbox-only actors as common content (the PC
        // game then loads them too) instead of leaving them out; references
        // to them stay. The report lists each one. Off for the menu command.
        bool keepXboxOnly = false;
        // Called at every checkpoint on the UI thread; false cancels while
        // MapRecoveryModel::CancelAllowed(stage). May be empty.
        std::function<bool(MapRecoveryModel::Stage, double)> progress;
        // Test hook: behave as if Cancel was pressed when this stage starts.
        MapRecoveryModel::Stage cancelAt = MapRecoveryModel::Stage::Count;
    };

    struct RecoveryOutcome
    {
        bool cancelled = false;
        MapRecoveryModel::Counts counts;
        std::vector<MapRecoveryModel::ReportRow> rows;
        std::filesystem::path details;  // Recovery.txt
        std::filesystem::path folder;
    };

    // One-time conversion; the output uses the normal editor package layout.
    // No dialogs. The caller must supply a new destination filename.
    // A cancelled recovery resets the editor to a new empty map and deletes
    // the files it had written; error then says it was cancelled.
    bool RecoverToSource(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         std::string& error);
    bool RecoverToSource(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         std::string& error, const RecoveryOptions& options,
                         RecoveryOutcome* outcome);
    // A recovery is running (the menu's, map.recover's or a test's).
    bool Running();
    bool HandleSaveCommand(UINT commandId);
    constexpr UINT kRecalculateLightingCommandId = 40929;
    constexpr UINT kRecalculateSelectedLightingCommandId = 40930;
    void RecalculateSelectedLighting(HWND owner, bool matchSurroundings = false);
    constexpr UINT kMatchSelectedLightingCommandId = 40931;
    void InitializeLightingProtection();
    void RecalculateLighting(HWND owner);
    void* ResolveBuilderBrushActor(void* level);
    bool IsRecoveredMapActive();
    bool UsesRecoveredBspLayout();
    bool ShouldSkipRecoveredEditorPoly();
    bool IsRecoveredBspModel(void* model);
    void RepairZoneInfoAssignment(void* actor);

    // The actor snapshot can contain native runtime actors whose cooked-only
    // state is not valid after T3D reconstruction.  These calls bracket the
    // engine's two actor Tick call sites so a first-chance exception can name
    // the exact reconstructed actor instead of reporting only TickAllActors.
    void ArmActorTickDiagnostic();
    void BeginActorTickLoop(void* level);
    void EndActorTickLoop();
    void RecordActorTick(void* actor, int actorIndex);
    void ClearActorTick();
    bool LogActorTickException(EXCEPTION_POINTERS* exceptionInfo);

    // Armed after the recovered-map confirmation closes. The vectored
    // exception handler records the first UI-thread fault because Unreal's
    // guarded call chain consumes it before the process-level crash logger.
    void ArmViewportExceptionDiagnostic();
    bool LogViewportException(EXCEPTION_POINTERS* exceptionInfo);

    // Temporary cooked extraction levels require the cooked serializer layout.
    // Finished source maps use the ordinary save path with these guards off.
    // These calls are paired by the SavePackage hook.
    void BeginSavePackage(uintptr_t returnAddress);
    uintptr_t EndSavePackage();
}
