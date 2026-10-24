#pragma once
#include <windows.h>

// RE+ windows open where they were last closed: [WindowPlacement] in
// Reloaded_Editor.ini, one key per window (the format and the rules are in
// WindowPlacementModel.h). Restore right after the window is created, before
// it is shown; Remember while it is being destroyed.
namespace WindowPlacement
{
    // Puts the window back where `key` last left it, when enough of its title
    // bar would be on a monitor. A window that cannot be resized keeps its
    // size, one that cannot be maximised comes back restored. A hidden window
    // stays hidden unless it comes back maximised. One that no longer fits
    // on its monitor is made to fit (WindowPlacementModel.h Fit). False when
    // nothing was stored or the place is off every monitor.
    bool Restore(HWND window, const char* key);
    // For a window created where Windows puts it, without Restore: Remember
    // stores it as it would a restored one.
    void Track(HWND window);
    // Stores where the window is under `key`: its place and size when not
    // maximised (where it is on screen, snapped or not), and whether it is
    // maximised. A window Restore never saw (its creation failed) is not stored.
    void Remember(HWND window, const char* key);
}
