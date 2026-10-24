#pragma once
// Appearance: the editor's look (Classic, Modern Light, Modern Dark). Pure logic, unit tested in
// tests/AppearanceModelTests.cpp: the setting's names, which fonts are replaced and at what size,
// rewriting a dialog template's font, and the dark palette.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace Appearance
{
    enum class Theme { Classic, ModernLight, ModernDark };

    inline std::string Lower(std::string text)
    {
        for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return text;
    }

    // Reloaded_Editor.ini [Appearance] Theme=. Anything unknown is Classic.
    inline Theme ParseTheme(const std::string& value)
    {
        std::string key;
        for (char c : Lower(value)) if (c != ' ' && c != '-' && c != '_') key += c;
        if (key == "modernlight" || key == "light") return Theme::ModernLight;
        if (key == "moderndark" || key == "dark") return Theme::ModernDark;
        return Theme::Classic;
    }
    inline const char* ThemeKey(Theme theme)
    {
        return theme == Theme::ModernLight ? "ModernLight" : theme == Theme::ModernDark ? "ModernDark" : "Classic";
    }
    inline const char* ThemeLabel(Theme theme)
    {
        return theme == Theme::ModernLight ? "Modern Light" : theme == Theme::ModernDark ? "Modern Dark" : "Classic";
    }

    // ---- fonts ------------------------------------------------------------------------------------
    constexpr const wchar_t* kModernFace = L"Segoe UI";
    constexpr int kModernPoints = 9;
    constexpr int kModernHeight = -12; // 9 pt at 96 dpi

    // The 1990s dialog faces the stock editor asks for. Fixed-pitch faces (the log, script text) and
    // anything else keep theirs.
    inline bool IsLegacyFace(const wchar_t* face)
    {
        if (!face) return false;
        static const wchar_t* const legacy[] = {L"MS Sans Serif", L"MS Shell Dlg", L"MS Shell Dlg 2", L"Microsoft Sans Serif", L"Tahoma"};
        for (auto name : legacy)
            if (!_wcsicmp(face, name)) return true;
        return false;
    }
    inline bool IsLegacyFace(const char* face)
    {
        if (!face) return false;
        std::wstring wide;
        for (const char* c = face; *c; ++c) wide += static_cast<wchar_t>(static_cast<unsigned char>(*c));
        return IsLegacyFace(wide.c_str());
    }

    // A LOGFONT height for the modern face: the 8 pt sizes dialog text uses (and 0, "the default")
    // become 9 pt; headings and small print keep their size.
    inline int ModernHeight(int height)
    {
        if (height == 0) return kModernHeight;
        if (height < 0) return (-height == 10 || -height == 11) ? kModernHeight : height;
        return (height >= 12 && height <= 14) ? kModernHeight : height; // a cell height of 8 pt text
    }
    inline int ModernPoints(int points) { return points == 8 ? kModernPoints : points; }

    // ---- dialog templates -------------------------------------------------------------------------
    namespace Detail
    {
        struct Reader
        {
            const uint8_t* data; size_t size; size_t at = 0; bool bad = false;
            bool Need(size_t n) { if (at + n > size) bad = true; return !bad; }
            uint16_t Word() { if (!Need(2)) return 0; uint16_t v; std::memcpy(&v, data + at, 2); at += 2; return v; }
            uint32_t Dword() { if (!Need(4)) return 0; uint32_t v; std::memcpy(&v, data + at, 4); at += 4; return v; }
            void Align4() { at = (at + 3) & ~size_t(3); }
            // sz_Or_Ord: 0 (none), 0xFFFF + ordinal, or a 0-terminated UTF-16 string.
            void SzOrOrd()
            {
                const uint16_t first = Word();
                if (bad || first == 0) return;
                if (first == 0xFFFF) { Word(); return; }
                while (!bad && Word() != 0) {}
            }
            std::wstring Sz()
            {
                std::wstring text;
                for (uint16_t c = Word(); !bad && c; c = Word()) text += static_cast<wchar_t>(c);
                return text;
            }
        };
        constexpr uint32_t kSetFont = 0x40; // DS_SETFONT
    }

    // The whole template's size in bytes (header and every item), or 0 when it cannot be read.
    // `limit` bounds the read (a resource's size, or a generous bound for an in-memory template).
    inline size_t TemplateSize(const void* tmpl, size_t limit)
    {
        Detail::Reader r{static_cast<const uint8_t*>(tmpl), limit};
        const bool extended = limit >= 4 && r.data[2] == 0xFF && r.data[3] == 0xFF && r.data[0] == 1 && r.data[1] == 0;
        uint32_t style = 0;
        uint16_t count = 0;
        if (extended) { r.Word(); r.Word(); r.Dword(); r.Dword(); style = r.Dword(); }
        else { style = r.Dword(); r.Dword(); }
        count = r.Word();
        r.Word(); r.Word(); r.Word(); r.Word();
        r.SzOrOrd(); r.SzOrOrd(); r.Sz();
        if (style & Detail::kSetFont)
        {
            r.Word();
            if (extended) { r.Word(); r.Word(); }
            r.Sz();
        }
        for (uint16_t i = 0; i < count && !r.bad; ++i)
        {
            r.Align4();
            if (extended) { r.Dword(); r.Dword(); r.Dword(); r.Word(); r.Word(); r.Word(); r.Word(); r.Dword(); }
            else { r.Dword(); r.Dword(); r.Word(); r.Word(); r.Word(); r.Word(); r.Word(); }
            r.SzOrOrd(); r.SzOrOrd();
            const uint16_t extra = r.Word();
            if (extra && r.Need(extra & ~1u)) r.at += extra & ~1u; // as user32 skips it
        }
        return r.bad ? 0 : r.at;
    }

    // A copy of the template with its font set to `face` at ModernPoints(size), or empty when it has
    // no font of its own (DS_SETFONT) or one that is not a legacy dialog face. Items keep their
    // DWORD alignment from the template's start.
    inline std::vector<uint8_t> ModernTemplate(const void* tmpl, size_t limit, const wchar_t* face = kModernFace)
    {
        const size_t total = TemplateSize(tmpl, limit);
        if (!total) return {};
        Detail::Reader r{static_cast<const uint8_t*>(tmpl), total};
        const bool extended = r.data[2] == 0xFF && r.data[3] == 0xFF && r.data[0] == 1 && r.data[1] == 0;
        uint32_t style = 0;
        if (extended) { r.Word(); r.Word(); r.Dword(); r.Dword(); style = r.Dword(); }
        else { style = r.Dword(); r.Dword(); }
        if (!(style & Detail::kSetFont)) return {};
        const uint16_t count = r.Word();
        r.Word(); r.Word(); r.Word(); r.Word();
        r.SzOrOrd(); r.SzOrOrd(); r.Sz();
        const size_t fontAt = r.at;
        const uint16_t points = r.Word();
        uint16_t weight = 0, italicCharset = 0;
        if (extended) { weight = r.Word(); italicCharset = r.Word(); }
        const std::wstring oldFace = r.Sz();
        if (r.bad || !IsLegacyFace(oldFace.c_str())) return {};

        std::vector<uint8_t> out(r.data, r.data + fontAt);
        auto word = [&](uint16_t v) { out.push_back(static_cast<uint8_t>(v)); out.push_back(static_cast<uint8_t>(v >> 8)); };
        word(static_cast<uint16_t>(ModernPoints(points)));
        if (extended) { word(weight); word(italicCharset); }
        for (const wchar_t* c = face; *c; ++c) word(static_cast<uint16_t>(*c));
        word(0);
        for (uint16_t i = 0; i < count; ++i)
        {
            r.Align4();
            const size_t start = r.at;
            if (extended) { r.Dword(); r.Dword(); r.Dword(); r.Word(); r.Word(); r.Word(); r.Word(); r.Dword(); }
            else { r.Dword(); r.Dword(); r.Word(); r.Word(); r.Word(); r.Word(); r.Word(); }
            r.SzOrOrd(); r.SzOrOrd();
            const uint16_t extra = r.Word();
            if (extra) r.at += extra & ~1u;
            while (out.size() & 3) out.push_back(0);
            out.insert(out.end(), r.data + start, r.data + r.at);
        }
        return out;
    }

    // ---- colours ----------------------------------------------------------------------------------
    // COLORREF values (0x00BBGGRR).
    constexpr uint32_t Rgb(int r, int g, int b) { return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16); }
    struct Palette
    {
        uint32_t face, window, text, grayText, border, light, shadow, darkShadow, highlight, highlightText, hot, menuBar;
    };
    inline Palette DarkPalette()
    {
        return {Rgb(43, 43, 43), Rgb(30, 30, 30), Rgb(232, 232, 232), Rgb(140, 140, 140), Rgb(75, 75, 75), Rgb(70, 70, 70),
                Rgb(24, 24, 24), Rgb(12, 12, 12), Rgb(0, 95, 184), Rgb(255, 255, 255), Rgb(62, 62, 62), Rgb(36, 36, 36)};
    }

    // The colour a GetSysColor(index) call sees in Modern Dark. Indices are COLOR_* values; those
    // the editor does not paint with (captions, the desktop) keep `original`.
    inline uint32_t DarkSysColor(int index, uint32_t original)
    {
        const Palette p = DarkPalette();
        switch (index)
        {
        case 0:  return Rgb(50, 50, 50);   // COLOR_SCROLLBAR
        case 4:  return p.face;            // COLOR_MENU
        case 5:  return p.window;          // COLOR_WINDOW
        case 6:  return p.border;          // COLOR_WINDOWFRAME
        case 7:  return p.text;            // COLOR_MENUTEXT
        case 8:  return p.text;            // COLOR_WINDOWTEXT
        case 12: return Rgb(24, 24, 24);   // COLOR_APPWORKSPACE
        case 13: return p.highlight;       // COLOR_HIGHLIGHT
        case 14: return p.highlightText;   // COLOR_HIGHLIGHTTEXT
        case 15: return p.face;            // COLOR_BTNFACE / 3DFACE
        case 16: return p.shadow;          // COLOR_BTNSHADOW
        case 17: return p.grayText;        // COLOR_GRAYTEXT
        case 18: return p.text;            // COLOR_BTNTEXT
        case 20: return p.border;          // COLOR_BTNHIGHLIGHT
        case 21: return p.darkShadow;      // COLOR_3DDKSHADOW
        case 22: return p.light;           // COLOR_3DLIGHT
        case 23: return p.text;            // COLOR_INFOTEXT
        case 24: return Rgb(50, 50, 50);   // COLOR_INFOBK
        case 26: return Rgb(96, 170, 255); // COLOR_HOTLIGHT
        case 30: return p.menuBar;         // COLOR_MENUBAR
        default: return original;
        }
    }

    // The stock editor and RE+'s own windows paint with fixed colours (white list rows, grey bars,
    // black text). In Modern Dark, light greys used to fill turn into a narrow band of dark greys
    // (lighter ones darker, so the order of shades is kept), and dark greys used for text turn
    // light. Coloured values (selection blue, warnings in red) are left as they are.
    inline bool Greyish(uint32_t color, int& level)
    {
        const int r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF;
        const int high = r > g ? (r > b ? r : b) : (g > b ? g : b), low = r < g ? (r < b ? r : b) : (g < b ? g : b);
        level = (r + g + b) / 3;
        return high - low <= 24 || low >= 200; // grey, or a pale tint (tooltip yellow)
    }
    inline uint32_t DarkFill(uint32_t color)
    {
        int level = 0;
        if ((color >> 24) || !Greyish(color, level) || level < 96) return color;
        const int dark = 28 + (255 - level) / 4;
        return Rgb(dark, dark, dark);
    }
    inline uint32_t DarkText(uint32_t color)
    {
        int level = 0;
        if ((color >> 24) || !Greyish(color, level) || level > 128) return color;
        const int light = 232 - level * 6 / 10;
        return Rgb(light, light, light);
    }
}
