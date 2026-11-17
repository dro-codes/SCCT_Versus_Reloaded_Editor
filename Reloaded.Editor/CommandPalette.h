#pragma once
#include <windows.h>

// Help > Command Palette... (Ctrl+Shift+P anywhere in the editor): a search
// box over the editor. Typing finds any command of the frame's menu bar, stock
// and RE+, by its menu path ("Build > Map Check..."); Enter or a click runs it
// as the menu would (WM_COMMAND to the frame), Esc closes, the arrows move.
// The commands run last are listed first. The matching is CommandPaletteModel.h.
namespace CommandPalette
{
    constexpr UINT kOpen = 41580; // block 41580-41599

    // On the frame's thread, once the frame exists: installs the
    // Ctrl+Shift+P key hook on that thread. Safe to call again.
    void Attach(HWND frame);
    bool HandleCommand(UINT command);
    void Open();
}
