#pragma once
#include <windows.h>

// View > Map Outliner (also RE+ Tools): a window listing every actor of the
// open map with its Name, Class, Group, Tag, Event, location, hidden and
// locked state, grouped by class or by the map's Group field, with a search
// over every column. Click selects in the editor, double-click frames, Hide /
// Show use the same flags as RE+: Visibility, Delete deletes in one Undo
// step. The list and grouping logic is MapOutlinerModel.h.
namespace MapOutliner
{
    // 41560-41579 are the outliner's (General.cpp's MenuBarDispatch).
    constexpr UINT kOpenCommand = 41560;

    bool HandleCommand(UINT command);
    void Open(HWND owner);
}
