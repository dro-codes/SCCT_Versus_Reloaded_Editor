#pragma once
#include <windows.h>

// RE+ Tools > Mover Workbench... (also on a mover's right-click menu): the
// selected mover's keys as a list, edited a key at a time, its timing and
// trigger settings in plain words, wiring to a trigger or button, a preview
// that plays it in the viewports, and ghost outlines of it at every key.
// MoverModel.h holds the key list and its maths; MoverNative.inl the editor side.
namespace MoverWorkbench
{
    // Reserved block 41520-41539.
    constexpr UINT kOpen = 41520;

    // The window's controls, for the smoke suite: buttons take WM_COMMAND.
    enum Control : int
    {
        kList = 100, kStatus, kInfo, kAddKey, kUpdateKey, kDeleteKey, kMoveUp, kMoveDown, kGoToKey, kPreview,
        kGhosts, kApply, kRenameTag, kWire, kWireList, kPlayers, kPlayersText, kTriggeredBy, kRefresh,
        kFieldFirst = 200, // the setting fields, in MoverWorkbench.cpp's kFields order
    };

    bool HandleCommand(UINT command);
    // Opens the window on the selected mover (or the last one shown).
    void Open(HWND owner);
}
