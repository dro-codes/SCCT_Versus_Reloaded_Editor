#pragma once
#include <windows.h>
#include "WorkflowModel.h"

// Build > Lighting Budget...: in-game lights per zone, the worst BSP leaf and
// the largest overlapping group, against the editor's own light-check limits
// (LightingBudgetModel.h). Also checked before Build All and the lighting
// rebuilds, with a warning when the map is over budget.
namespace LightingBudget
{
    constexpr UINT kOpenCommand = 41150;
    // Fixes for the open window's chosen row: plan one (shown for Apply or
    // Cancel in the window), carry it out as one undoable step, or drop it.
    constexpr UINT kMakeStaticCommand = 41151, kTurnOffCommand = 41152, kApplyFixCommand = 41153, kCancelFixCommand = 41154;
    // Stock Build menu commands the warning runs before.
    constexpr UINT kBuildAll = 40038, kRebuildLighting = 40162, kRebuildChangedLighting = 30000;

    bool HandleCommand(UINT command);
    void Open(HWND owner);
    // Runs on the UI thread before a stock build command. False cancels it.
    // Never prompts while Rebuild All Maps runs or when warnings are off.
    bool AllowBuild(UINT command);
    // The budget as the window measures it, against the [LightingBudget]
    // guideline, with the pre-build warning's verdict beside it (the bridge's
    // lighting.budget). Lights are named by actor path. UI thread only.
    Workflow::Json Measure();
}
