#include "../Reloaded.Editor/AppearanceModel.h"
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace Appearance;

namespace
{
    // A dialog template written the way the resource compiler and RE+'s in-memory dialogs write them.
    struct Builder
    {
        std::vector<uint8_t> b;
        void Align2() { while (b.size() & 1) b.push_back(0); }
        void Align4() { while (b.size() & 3) b.push_back(0); }
        void W(uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
        void D(uint32_t v) { W(uint16_t(v)); W(uint16_t(v >> 16)); }
        void S(const wchar_t* s) { for (; *s; ++s) W(uint16_t(*s)); W(0); }
        void Atom(uint16_t a) { W(0xFFFF); W(a); }
    };

    std::vector<uint8_t> Classic(const wchar_t* face, uint16_t points, bool font = true)
    {
        Builder t;
        t.D(WS_POPUP | WS_CAPTION | (font ? DS_SETFONT : 0));
        t.D(0);
        t.W(3);
        t.W(0); t.W(0); t.W(200); t.W(100);
        t.W(0); t.W(0); t.S(L"Title");
        if (font) { t.W(points); t.S(face); }
        // A static with a text title, an edit with creation data, a button with an ordinal title.
        t.Align4(); t.D(WS_CHILD | WS_VISIBLE); t.D(0); t.W(5); t.W(5); t.W(60); t.W(8); t.W(100); t.Atom(0x0082); t.S(L"Hello there"); t.W(0);
        t.Align4(); t.D(WS_CHILD | WS_VISIBLE | WS_BORDER); t.D(0); t.W(5); t.W(20); t.W(60); t.W(12); t.W(101); t.Atom(0x0081); t.S(L""); t.W(4); t.W(0xABCD); t.W(0x1234);
        t.Align4(); t.D(WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON); t.D(0); t.W(5); t.W(40); t.W(50); t.W(14); t.W(IDOK); t.S(L"Button"); t.S(L"OK"); t.W(0);
        return t.b;
    }

    std::vector<uint8_t> Extended(const wchar_t* face)
    {
        Builder t;
        t.W(1); t.W(0xFFFF); t.D(0); t.D(0);
        t.D(WS_POPUP | WS_CAPTION | DS_SETFONT | DS_FIXEDSYS);
        t.W(2);
        t.W(0); t.W(0); t.W(180); t.W(90);
        t.W(0); t.W(0); t.S(L"Ex");
        t.W(8); t.W(FW_NORMAL); t.b.push_back(0); t.b.push_back(1); t.S(face);
        t.Align4(); t.D(0); t.D(0); t.D(WS_CHILD | WS_VISIBLE); t.W(5); t.W(5); t.W(80); t.W(8); t.D(200); t.Atom(0x0082); t.S(L"Extended static"); t.W(0);
        t.Align4(); t.D(0); t.D(0); t.D(WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX); t.W(5); t.W(20); t.W(80); t.W(10); t.D(201); t.S(L"Button"); t.S(L"Check"); t.W(0);
        return t.b;
    }

    // The template made into a real (hidden) dialog: its controls' text and its font face.
    struct Made { std::wstring face; int points = 0; std::vector<std::wstring> texts; };
    INT_PTR CALLBACK Quiet(HWND, UINT message, WPARAM, LPARAM) { return message == WM_INITDIALOG; }
    Made Make(const std::vector<uint8_t>& tmpl)
    {
        Made made;
        HWND dialog = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()), nullptr, Quiet, 0);
        assert(dialog);
        LOGFONTW font{};
        GetObjectW(reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0)), sizeof(font), &font);
        made.face = font.lfFaceName;
        HDC dc = GetDC(nullptr);
        made.points = MulDiv(-font.lfHeight, 72, GetDeviceCaps(dc, LOGPIXELSY));
        ReleaseDC(nullptr, dc);
        for (HWND c = GetWindow(dialog, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        {
            wchar_t text[64] = {};
            GetWindowTextW(c, text, 63);
            made.texts.push_back(text);
        }
        DestroyWindow(dialog);
        return made;
    }
}

int main()
{
    // ---- the setting
    assert(ParseTheme("Classic") == Theme::Classic);
    assert(ParseTheme("") == Theme::Classic);
    assert(ParseTheme("nonsense") == Theme::Classic);
    assert(ParseTheme("ModernLight") == Theme::ModernLight);
    assert(ParseTheme("modern light") == Theme::ModernLight);
    assert(ParseTheme("Modern-Dark") == Theme::ModernDark);
    assert(ParseTheme("DARK") == Theme::ModernDark);
    for (Theme t : {Theme::Classic, Theme::ModernLight, Theme::ModernDark})
    {
        assert(ParseTheme(ThemeKey(t)) == t);
        assert(ParseTheme(ThemeLabel(t)) == t);
    }
    assert(static_cast<int>(Theme::Classic) == 0 && static_cast<int>(Theme::ModernLight) == 1 && static_cast<int>(Theme::ModernDark) == 2);

    // ---- fonts
    assert(IsLegacyFace(L"MS Sans Serif") && IsLegacyFace("ms shell dlg") && IsLegacyFace(L"MS Shell Dlg 2") && IsLegacyFace("Tahoma"));
    assert(!IsLegacyFace(L"Courier New") && !IsLegacyFace("Segoe UI") && !IsLegacyFace("Arial") && !IsLegacyFace(static_cast<const char*>(nullptr)));
    assert(ModernHeight(0) == kModernHeight);
    assert(ModernHeight(-11) == kModernHeight && ModernHeight(-10) == kModernHeight && ModernHeight(13) == kModernHeight);
    assert(ModernHeight(-16) == -16 && ModernHeight(-8) == -8 && ModernHeight(24) == 24);
    assert(ModernPoints(8) == 9 && ModernPoints(10) == 10);

    // ---- templates: sizes
    const auto classic = Classic(L"MS Sans Serif", 8);
    assert(TemplateSize(classic.data(), classic.size()) == classic.size());
    assert(TemplateSize(classic.data(), classic.size() - 1) == 0); // cut short: unreadable
    const auto ex = Extended(L"MS Shell Dlg");
    assert(TemplateSize(ex.data(), ex.size()) == ex.size());
    // A bound past the end (an in-memory template) still stops at the last item.
    auto padded = classic;
    padded.resize(padded.size() + 64, 0xCC);
    assert(TemplateSize(padded.data(), padded.size()) == classic.size());

    // ---- templates: rewriting
    assert(ModernTemplate(Classic(L"Courier New", 8).data(), 4096).empty());       // not a dialog face
    auto unfonted = Classic(L"", 0, false);
    assert(TemplateSize(unfonted.data(), unfonted.size()) == unfonted.size());
    assert(ModernTemplate(unfonted.data(), unfonted.size()).empty());               // the system font: left alone
    const auto modern = ModernTemplate(padded.data(), padded.size());
    assert(!modern.empty());
    assert(TemplateSize(modern.data(), modern.size()) == modern.size());
    const auto before = Make(classic), after = Make(modern);
    assert(before.face == L"MS Sans Serif" && after.face == L"Segoe UI");
    assert(after.points == 9);
    assert(before.texts == after.texts && after.texts.size() == 3 && after.texts[0] == L"Hello there" && after.texts[2] == L"OK");
    // Rewriting is idempotent: a Segoe UI template is not a legacy one.
    assert(ModernTemplate(modern.data(), modern.size()).empty());

    const auto modernEx = ModernTemplate(ex.data(), ex.size());
    assert(!modernEx.empty() && TemplateSize(modernEx.data(), modernEx.size()) == modernEx.size());
    const auto exBefore = Make(ex), exAfter = Make(modernEx);
    assert(exAfter.face == L"Segoe UI" && exAfter.points == 9);
    assert(exBefore.texts == exAfter.texts && exAfter.texts.size() == 2 && exAfter.texts[1] == L"Check");
    // A ten point dialog keeps its size.
    assert(Make(ModernTemplate(Classic(L"Tahoma", 10).data(), 4096)).points == 10);

    // ---- colours
    const Palette p = DarkPalette();
    auto luminance = [](uint32_t c) { return (c & 0xFF) * 3 + ((c >> 8) & 0xFF) * 6 + ((c >> 16) & 0xFF); };
    assert(luminance(p.text) > 4 * luminance(p.face));
    assert(luminance(p.window) < luminance(p.face));
    assert(DarkSysColor(COLOR_BTNFACE, 0x123456) == p.face);
    assert(DarkSysColor(COLOR_WINDOW, 0x123456) == p.window);
    assert(DarkSysColor(COLOR_WINDOWTEXT, 0) == p.text && DarkSysColor(COLOR_BTNTEXT, 0) == p.text);
    assert(DarkSysColor(COLOR_ACTIVECAPTION, 0x123456) == 0x123456); // captions: the system's
    assert(DarkSysColor(COLOR_DESKTOP, 0x654321) == 0x654321);

    std::puts("AppearanceModelTests passed");
    return 0;
}
