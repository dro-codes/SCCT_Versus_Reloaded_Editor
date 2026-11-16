#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace Reach { struct Patch; struct Finding; }

// Build > Reachability Map: where a spy and where a merc can walk from their
// team's player starts, worked out with the engine's own collision queries
// (ReachabilityModel.h) a few steps at a time, so the editor stays usable and
// the menu item cancels it. The cells are drawn in every level viewport (green
// both teams, blue spies only, red mercs only) with a legend; Map Design draws
// them as a plan layer (Tools > Show reachability map) and Map Check lists the
// objectives and pickups a team cannot get to and the starts that reach
// nothing. Settings live in Reloaded_Editor.ini [Reachability].
namespace Reachability
{
    // Reserved block 41480-41499.
    constexpr UINT kToggle = 41480;

    bool HandleCommand(UINT command);
    // From Measure's UUnrealEdEngine::Draw hook, beside the Light and Shadow map.
    void DrawViewportOverlay(uintptr_t viewport, uintptr_t sceneNode);
    // Map Check's rows: the findings of a run on the map as it is, or a note
    // that it is being worked out (a run is started when none is fresh, unless
    // [Reachability] RunWithMapCheck=0); Map Check refreshes when it ends.
    std::vector<Reach::Finding> MapCheckRows();
    // Map Design's plan layer: the cells so far, their size, and whether the
    // layer is in use (turning it on starts a run when none is fresh). Version
    // changes whenever the cells do.
    const std::vector<Reach::Patch>& Patches();
    double CellSize();
    void SetPlanLayer(bool on);
    unsigned Version();
    // One line for a status bar: what is shown, or how far the run has got.
    std::string Status();
}
