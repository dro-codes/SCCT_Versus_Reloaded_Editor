#pragma once
#include <windows.h>

// Build > Map Check...: a report of actors in the void, outside the map or
// stuck in geometry, zones that leak into each other or to the outside, zone
// portals that seal nothing, and the stock Check Map for Errors entries, each
// row selecting (click) or framing (double-click) what it is about, with the
// leak path drawn in the viewports (MapCheckModel.h holds the checks).
namespace MapCheck
{
    struct Report; // MapCheckModel.h

    // Reserved block 41220-41239.
    constexpr UINT kOpenCommand = 41220;

    bool HandleCommand(UINT command);
    void Open(HWND owner);
    // The checks the window runs, on the map as last built, with the
    // [MapCheck] settings; logged like a Refresh. UI thread only. ToJson
    // (MapCheckModel.h) is the bridge's mapcheck.run reply.
    Report Measure();
}
