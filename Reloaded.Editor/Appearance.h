#pragma once
#include <windows.h>
#include "AppearanceModel.h"

// RE+ Options > Appearance: Classic (the stock look, untouched), Modern Light (visual styles and
// Segoe UI 9 pt) or Modern Dark (that, in dark colours). Read once at start; a change applies the
// next time the editor starts.
namespace Appearance
{
    // Called first thing at DLL attach, before the editor's own thread runs. Classic installs nothing.
    void Initialize();

    Theme Current(); // in effect this session
    Theme Saved();   // in Reloaded_Editor.ini (what the next start uses)
    void Save(Theme theme);
    inline bool Dark() { return Current() == Theme::ModernDark; }

    // For RE+'s own painting in Modern Dark (COLORREFs).
    Palette Colors();
    HBRUSH Brush(COLORREF color); // kept for the session; never delete it

    // The height a multi-line static (an intro or status block) needs to show all its text, wrapped
    // at `width`, in its own font, and never less than `fixed`. Classic keeps `fixed` (the larger
    // modern font is what makes the stock heights too small); `most` caps it.
    int FitHeight(HWND text, int width, int fixed, int most = 100000);
}
