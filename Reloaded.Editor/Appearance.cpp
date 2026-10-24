#include "pch.h"
#include "Appearance.h"
#include "logger.h"
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <shlwapi.h>
#include <map>
#include <mutex>
#include <string>

extern HINSTANCE g_hReloadedDll;

// How it works. Classic does nothing at all. The modern themes replace a few entries in the import
// tables of ChaosTheory_Editor.exe and of this DLL (never code), before the editor's thread starts:
//  - window, dialog, class-lookup and message box calls run with an activation context that names
//    Common Controls 6, so stock and RE+ controls are created from the visual-styles versions (the
//    stock controls are superclasses: GetClassInfoEx under the context hands them the v6 procs);
//    the editor's own comctl32 imports (toolbar, image lists) are pointed at version 6 too, since a
//    version 5 image list does not work in a version 6 control;
//  - dialog templates in MS Sans Serif 8 are rewritten to Segoe UI 9, fonts created in the legacy
//    dialog faces and the stock GUI font become Segoe UI 9;
//  - a CBT hook on each thread that makes windows subclasses them: list and tree views get the
//    Explorer look, and in Modern Dark every window gets dark colours: title bars (DWM), scroll
//    bars, lists, trees and edits (the system dark themes), WM_CTLCOLOR* answers, the menu bar
//    (the undocumented UAH menu messages), popup menus (uxtheme's app mode), tabs and toolbars;
//  - version 6 buttons notify their parent as they paint, which the stock windows would take for a
//    click (they answer any WM_NOTIFY with OnCommand), so those notifications stop at the parent;
//  - Modern Dark also maps the colours the stock editor and RE+ paint with: system colours
//    (GetSysColor, GetSysColorBrush, class backgrounds, FillRect with a colour index, DrawEdge),
//    fixed light greys used to fill and dark greys used for text, and the grey squares behind the
//    editorres button pictures.
// Engine-rendered viewports are left alone.

namespace
{
    using Appearance::Theme;

    Theme current = Theme::Classic;
    HANDLE context = INVALID_HANDLE_VALUE;
    HMODULE comctl6 = nullptr;
    HFONT uiFont = nullptr;

    std::string IniPath()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        if (char* slash = strrchr(path, '\\')) slash[1] = '\0';
        return std::string(path) + "Reloaded_Editor.ini";
    }

    // ---- the originals -----------------------------------------------------------------------------
    template <class F> F Real(const char* module, const char* name)
    {
        return reinterpret_cast<F>(GetProcAddress(GetModuleHandleA(module), name));
    }
    struct Originals
    {
        decltype(&CreateWindowExA) CreateWindowExA = nullptr;
        decltype(&CreateWindowExW) CreateWindowExW = nullptr;
        decltype(&DialogBoxParamA) DialogBoxParamA = nullptr;
        decltype(&DialogBoxParamW) DialogBoxParamW = nullptr;
        decltype(&DialogBoxIndirectParamA) DialogBoxIndirectParamA = nullptr;
        decltype(&DialogBoxIndirectParamW) DialogBoxIndirectParamW = nullptr;
        decltype(&CreateDialogParamA) CreateDialogParamA = nullptr;
        decltype(&CreateDialogParamW) CreateDialogParamW = nullptr;
        decltype(&CreateDialogIndirectParamA) CreateDialogIndirectParamA = nullptr;
        decltype(&CreateDialogIndirectParamW) CreateDialogIndirectParamW = nullptr;
        decltype(&MessageBoxA) MessageBoxA = nullptr;
        decltype(&MessageBoxW) MessageBoxW = nullptr;
        decltype(&GetClassInfoExA) GetClassInfoExA = nullptr;
        decltype(&GetClassInfoExW) GetClassInfoExW = nullptr;
        decltype(&GetClassInfoA) GetClassInfoA = nullptr;
        decltype(&GetClassInfoW) GetClassInfoW = nullptr;
        decltype(&RegisterClassExA) RegisterClassExA = nullptr;
        decltype(&RegisterClassExW) RegisterClassExW = nullptr;
        decltype(&RegisterClassA) RegisterClassA = nullptr;
        decltype(&RegisterClassW) RegisterClassW = nullptr;
        decltype(&GetSysColor) GetSysColor = nullptr;
        decltype(&GetSysColorBrush) GetSysColorBrush = nullptr;
        decltype(&CreateFontA) CreateFontA = nullptr;
        decltype(&CreateFontW) CreateFontW = nullptr;
        decltype(&CreateFontIndirectA) CreateFontIndirectA = nullptr;
        decltype(&CreateFontIndirectW) CreateFontIndirectW = nullptr;
        decltype(&GetStockObject) GetStockObject = nullptr;
        decltype(&CreateSolidBrush) CreateSolidBrush = nullptr;
        decltype(&CreatePen) CreatePen = nullptr;
        decltype(&SetTextColor) SetTextColor = nullptr;
        decltype(&SetBkColor) SetBkColor = nullptr;
        decltype(&FillRect) FillRect = nullptr;
        decltype(&DrawEdge) DrawEdge = nullptr;
        decltype(&LoadImageA) LoadImageA = nullptr;
        decltype(&LoadImageW) LoadImageW = nullptr;
        decltype(&GetOpenFileNameA) GetOpenFileNameA = nullptr;
        decltype(&GetOpenFileNameW) GetOpenFileNameW = nullptr;
        decltype(&GetSaveFileNameA) GetSaveFileNameA = nullptr;
        decltype(&GetSaveFileNameW) GetSaveFileNameW = nullptr;
        decltype(&ChooseColorA) ChooseColorA = nullptr;
        decltype(&ChooseColorW) ChooseColorW = nullptr;
        decltype(&SHBrowseForFolderA) SHBrowseForFolderA = nullptr;
        decltype(&SHBrowseForFolderW) SHBrowseForFolderW = nullptr;
    } real;

    decltype(&SetWindowSubclass) setSubclass = nullptr;
    decltype(&DefSubclassProc) defSubclass = nullptr;
    using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    AllowDarkModeForWindowFn allowDark = nullptr;
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    DwmSetWindowAttributeFn dwmSetAttribute = nullptr;

    // ---- per thread --------------------------------------------------------------------------------
    thread_local bool threadHooked = false;
    thread_local int foreignDialog = 0; // inside a common dialog: its windows keep their own look
    thread_local int messageBox = 0;    // inside MessageBox: its dialog is one
    LRESULT CALLBACK CbtProc(int code, WPARAM wParam, LPARAM lParam);

    void EnsureThread()
    {
        if (threadHooked) return;
        threadHooked = true;
        // Set by this thread, for this thread: it lives as long as the thread does.
        if (!SetWindowsHookExW(WH_CBT, CbtProc, nullptr, GetCurrentThreadId()))
            Logger::log("Appearance: no window hook on thread " + std::to_string(GetCurrentThreadId()) + ", error " + std::to_string(GetLastError()));
    }

    // Common Controls 6 for the calls inside the scope.
    struct Scope
    {
        ULONG_PTR cookie = 0;
        bool active = false;
        Scope()
        {
            EnsureThread();
            if (context != INVALID_HANDLE_VALUE) active = ActivateActCtx(context, &cookie) != FALSE;
        }
        ~Scope() { if (active) DeactivateActCtx(0, cookie); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
    struct Foreign
    {
        Foreign() { ++foreignDialog; }
        ~Foreign() { --foreignDialog; }
    };

    // ---- brushes and colours -----------------------------------------------------------------------
    std::mutex brushLock;
    std::map<COLORREF, HBRUSH> brushes;
    HBRUSH BrushFor(COLORREF color)
    {
        std::lock_guard<std::mutex> lock(brushLock);
        auto& brush = brushes[color];
        if (!brush) brush = real.CreateSolidBrush ? real.CreateSolidBrush(color) : CreateSolidBrush(color);
        return brush;
    }
    COLORREF DarkColor(int index) { return Appearance::DarkSysColor(index, real.GetSysColor ? real.GetSysColor(index) : 0); }

    // ---- fonts -------------------------------------------------------------------------------------
    void Modernise(LOGFONTW& font)
    {
        if (!Appearance::IsLegacyFace(font.lfFaceName)) return;
        wcscpy_s(font.lfFaceName, Appearance::kModernFace);
        font.lfHeight = Appearance::ModernHeight(font.lfHeight);
        font.lfWidth = 0;
        if (font.lfQuality == DEFAULT_QUALITY || font.lfQuality == DRAFT_QUALITY) font.lfQuality = CLEARTYPE_QUALITY;
    }
    LOGFONTW Wide(const LOGFONTA& font)
    {
        LOGFONTW wide{};
        memcpy(&wide, &font, offsetof(LOGFONTA, lfFaceName));
        MultiByteToWideChar(CP_ACP, 0, font.lfFaceName, -1, wide.lfFaceName, LF_FACESIZE);
        return wide;
    }

    HFONT WINAPI MyCreateFontIndirectW(const LOGFONTW* font)
    {
        if (!font || !Appearance::IsLegacyFace(font->lfFaceName)) return real.CreateFontIndirectW(font);
        LOGFONTW copy = *font;
        Modernise(copy);
        return real.CreateFontIndirectW(&copy);
    }
    HFONT WINAPI MyCreateFontIndirectA(const LOGFONTA* font)
    {
        if (!font || !Appearance::IsLegacyFace(font->lfFaceName)) return real.CreateFontIndirectA(font);
        LOGFONTW copy = Wide(*font);
        Modernise(copy);
        return real.CreateFontIndirectW(&copy);
    }
    HFONT WINAPI MyCreateFontA(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike, DWORD charset,
                               DWORD precision, DWORD clip, DWORD quality, DWORD pitch, LPCSTR face)
    {
        if (!Appearance::IsLegacyFace(face)) return real.CreateFontA(h, w, esc, orient, weight, italic, underline, strike, charset, precision, clip, quality, pitch, face);
        LOGFONTA font{h, w, esc, orient, weight, BYTE(italic), BYTE(underline), BYTE(strike), BYTE(charset), BYTE(precision), BYTE(clip), BYTE(quality), BYTE(pitch)};
        strncpy_s(font.lfFaceName, face, _TRUNCATE);
        return MyCreateFontIndirectA(&font);
    }
    HFONT WINAPI MyCreateFontW(int h, int w, int esc, int orient, int weight, DWORD italic, DWORD underline, DWORD strike, DWORD charset,
                               DWORD precision, DWORD clip, DWORD quality, DWORD pitch, LPCWSTR face)
    {
        if (!Appearance::IsLegacyFace(face)) return real.CreateFontW(h, w, esc, orient, weight, italic, underline, strike, charset, precision, clip, quality, pitch, face);
        LOGFONTW font{h, w, esc, orient, weight, BYTE(italic), BYTE(underline), BYTE(strike), BYTE(charset), BYTE(precision), BYTE(clip), BYTE(quality), BYTE(pitch)};
        wcsncpy_s(font.lfFaceName, face, _TRUNCATE);
        return MyCreateFontIndirectW(&font);
    }
    HGDIOBJ WINAPI MyGetStockObject(int which)
    {
        if ((which == DEFAULT_GUI_FONT || which == ANSI_VAR_FONT) && uiFont) return uiFont;
        if (current == Theme::ModernDark)
        {
            if (which == WHITE_BRUSH) return BrushFor(Appearance::DarkFill(RGB(255, 255, 255)));
            if (which == LTGRAY_BRUSH) return BrushFor(Appearance::DarkFill(RGB(192, 192, 192)));
            if (which == GRAY_BRUSH) return BrushFor(Appearance::DarkFill(RGB(128, 128, 128)));
        }
        return real.GetStockObject(which);
    }

    // ---- fixed colours (Modern Dark) ---------------------------------------------------------------
    HBRUSH WINAPI MyCreateSolidBrush(COLORREF color)
    {
        return real.CreateSolidBrush(Appearance::DarkFill(color));
    }
    HPEN WINAPI MyCreatePen(int style, int width, COLORREF color) { return real.CreatePen(style, width, Appearance::DarkFill(color)); }
    COLORREF WINAPI MySetTextColor(HDC dc, COLORREF color) { return real.SetTextColor(dc, Appearance::DarkText(color)); }
    COLORREF WINAPI MySetBkColor(HDC dc, COLORREF color) { return real.SetBkColor(dc, Appearance::DarkFill(color)); }
    int WINAPI MyFillRect(HDC dc, const RECT* rect, HBRUSH brush)
    {
        // (HBRUSH)(COLOR_xxx + 1): user32 would look the light system colour up itself.
        const auto value = reinterpret_cast<uintptr_t>(brush);
        if (value >= 1 && value <= 31) brush = BrushFor(DarkColor(static_cast<int>(value - 1)));
        return real.FillRect(dc, rect, brush);
    }
    // DrawEdge in the dark palette (the system's uses the light system colours).
    BOOL WINAPI MyDrawEdge(HDC dc, LPRECT rect, UINT edge, UINT flags)
    {
        if (!rect || (flags & (BF_DIAGONAL | BF_MONO | BF_FLAT))) return real.DrawEdge(dc, rect, edge, flags);
        const auto p = Appearance::DarkPalette();
        auto line = [&](const RECT& r, COLORREF topLeft, COLORREF bottomRight) {
            if (flags & BF_LEFT) { RECT e{r.left, r.top, r.left + 1, r.bottom}; real.FillRect(dc, &e, BrushFor(topLeft)); }
            if (flags & BF_TOP) { RECT e{r.left, r.top, r.right, r.top + 1}; real.FillRect(dc, &e, BrushFor(topLeft)); }
            if (flags & BF_RIGHT) { RECT e{r.right - 1, r.top, r.right, r.bottom}; real.FillRect(dc, &e, BrushFor(bottomRight)); }
            if (flags & BF_BOTTOM) { RECT e{r.left, r.bottom - 1, r.right, r.bottom}; real.FillRect(dc, &e, BrushFor(bottomRight)); }
        };
        RECT r = *rect;
        auto shrink = [&]() {
            if (flags & BF_LEFT) ++r.left;
            if (flags & BF_TOP) ++r.top;
            if (flags & BF_RIGHT) --r.right;
            if (flags & BF_BOTTOM) --r.bottom;
        };
        if (edge & (BDR_RAISEDOUTER | BDR_SUNKENOUTER))
        {
            if (edge & BDR_RAISEDOUTER) line(r, p.light, p.darkShadow); else line(r, p.shadow, p.border);
            shrink();
        }
        if (edge & (BDR_RAISEDINNER | BDR_SUNKENINNER))
        {
            if (edge & BDR_RAISEDINNER) line(r, p.border, p.shadow); else line(r, p.darkShadow, p.light);
            shrink();
        }
        if (flags & BF_MIDDLE) real.FillRect(dc, &r, BrushFor(p.face));
        if (flags & BF_ADJUST) *rect = r;
        return TRUE;
    }
    // Button and toolbar pictures (editorres\*.bmp) are drawn on a grey square.
    void RecolorBitmap(HBITMAP bitmap)
    {
        BITMAP info{};
        if (!GetObjectW(bitmap, sizeof(info), &info) || info.bmWidth <= 0 || info.bmHeight <= 0) return;
        if (static_cast<long long>(info.bmWidth) * info.bmHeight > 400000) return;
        BITMAPINFO header{};
        header.bmiHeader.biSize = sizeof(header.bmiHeader);
        header.bmiHeader.biWidth = info.bmWidth;
        header.bmiHeader.biHeight = -info.bmHeight;
        header.bmiHeader.biPlanes = 1;
        header.bmiHeader.biBitCount = 32;
        header.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> pixels(static_cast<size_t>(info.bmWidth) * info.bmHeight);
        HDC dc = GetDC(nullptr);
        if (GetDIBits(dc, bitmap, 0, info.bmHeight, pixels.data(), &header, DIB_RGB_COLORS))
        {
            // The corner pixel is the background the picture was drawn on; when it is a light or mid
            // grey, every pixel of exactly that grey takes the dark shade the editor's own fills of
            // that grey get (the bars they sit on).
            const uint32_t corner = pixels[0] & 0xFFFFFF;
            const uint32_t cornerRef = ((corner & 0xFF) << 16) | (corner & 0xFF00) | ((corner >> 16) & 0xFF);
            const uint32_t dark = Appearance::DarkFill(cornerRef);
            const uint32_t darkDib = ((dark & 0xFF) << 16) | (dark & 0xFF00) | ((dark >> 16) & 0xFF);
            bool changed = false;
            if (dark != cornerRef)
                for (auto& pixel : pixels)
                    if ((pixel & 0xFFFFFF) == corner) { pixel = (pixel & 0xFF000000) | darkDib; changed = true; }
            if (changed) SetDIBits(dc, bitmap, 0, info.bmHeight, pixels.data(), &header, DIB_RGB_COLORS);
        }
        ReleaseDC(nullptr, dc);
    }
    HANDLE WINAPI MyLoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy, UINT flags)
    {
        HANDLE image = real.LoadImageA(inst, name, type, cx, cy, flags);
        if (image && type == IMAGE_BITMAP) RecolorBitmap(static_cast<HBITMAP>(image));
        return image;
    }
    HANDLE WINAPI MyLoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags)
    {
        HANDLE image = real.LoadImageW(inst, name, type, cx, cy, flags);
        if (image && type == IMAGE_BITMAP) RecolorBitmap(static_cast<HBITMAP>(image));
        return image;
    }

    // ---- dialogs -----------------------------------------------------------------------------------
    // A generous bound for templates passed by pointer: TemplateSize stops at the last item.
    constexpr size_t kTemplateBound = 1 << 20;
    std::vector<uint8_t> ResourceTemplate(HINSTANCE instance, LPCSTR nameA, LPCWSTR nameW)
    {
        HRSRC found = nameW ? FindResourceW(instance, nameW, MAKEINTRESOURCEW(5)) : FindResourceA(instance, nameA, MAKEINTRESOURCEA(5));
        if (!found) return {};
        HGLOBAL loaded = LoadResource(instance, found);
        const void* data = loaded ? LockResource(loaded) : nullptr;
        return data ? Appearance::ModernTemplate(data, SizeofResource(instance, found)) : std::vector<uint8_t>{};
    }
    std::vector<uint8_t> MemoryTemplate(const void* data)
    {
        if (!data) return {};
        MEMORY_BASIC_INFORMATION info{};
        size_t bound = kTemplateBound;
        if (VirtualQuery(data, &info, sizeof(info)))
            bound = static_cast<size_t>(static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize - static_cast<const uint8_t*>(data));
        return Appearance::ModernTemplate(data, bound);
    }

    INT_PTR WINAPI MyDialogBoxParamA(HINSTANCE inst, LPCSTR name, HWND parent, DLGPROC proc, LPARAM param)
    {
        Scope scope;
        auto modern = ResourceTemplate(inst, name, nullptr);
        if (!modern.empty()) return real.DialogBoxIndirectParamA(inst, reinterpret_cast<LPCDLGTEMPLATEA>(modern.data()), parent, proc, param);
        return real.DialogBoxParamA(inst, name, parent, proc, param);
    }
    INT_PTR WINAPI MyDialogBoxParamW(HINSTANCE inst, LPCWSTR name, HWND parent, DLGPROC proc, LPARAM param)
    {
        Scope scope;
        auto modern = ResourceTemplate(inst, nullptr, name);
        if (!modern.empty()) return real.DialogBoxIndirectParamW(inst, reinterpret_cast<LPCDLGTEMPLATEW>(modern.data()), parent, proc, param);
        return real.DialogBoxParamW(inst, name, parent, proc, param);
    }
    INT_PTR WINAPI MyDialogBoxIndirectParamA(HINSTANCE inst, LPCDLGTEMPLATEA tmpl, HWND parent, DLGPROC proc, LPARAM param)
    {
        Scope scope;
        auto modern = MemoryTemplate(tmpl);
        return real.DialogBoxIndirectParamA(inst, modern.empty() ? tmpl : reinterpret_cast<LPCDLGTEMPLATEA>(modern.data()), parent, proc, param);
    }
    INT_PTR WINAPI MyDialogBoxIndirectParamW(HINSTANCE inst, LPCDLGTEMPLATEW tmpl, HWND parent, DLGPROC proc, LPARAM param)
    {
        Scope scope;
        auto modern = MemoryTemplate(tmpl);
        return real.DialogBoxIndirectParamW(inst, modern.empty() ? tmpl : reinterpret_cast<LPCDLGTEMPLATEW>(modern.data()), parent, proc, param);
    }
    HWND WINAPI MyCreateDialogParamA(HINSTANCE inst, LPCSTR name, HWND parent, DLGPROC proc, LPARAM param)
    {
        Scope scope;
        auto modern = ResourceTemplate(inst, name, nullptr);
        if (!modern.empty()) return real.CreateDialogIndirectParamA(inst, reinterpret_cast<LPCDLGTEMPLATEA>(modern.data()), parent, proc, param);
        return real.CreateDialogParamA(inst, name, parent, proc, param);
    }
    HWND WINAPI MyCreateDialogParamW(HINSTANCE inst, LPCWSTR name, HWND parent, DLGPROC proc, LPARAM param)
    {
        Scope scope;
        auto modern = ResourceTemplate(inst, nullptr, name);
        if (!modern.empty()) return real.CreateDialogIndirectParamW(inst, reinterpret_cast<LPCDLGTEMPLATEW>(modern.data()), parent, proc, param);
        return real.CreateDialogParamW(inst, name, parent, proc, param);
    }
    HWND WINAPI MyCreateDialogIndirectParamA(HINSTANCE inst, LPCDLGTEMPLATEA tmpl, HWND parent, DLGPROC proc, LPARAM param)
    {
        Scope scope;
        auto modern = MemoryTemplate(tmpl);
        return real.CreateDialogIndirectParamA(inst, modern.empty() ? tmpl : reinterpret_cast<LPCDLGTEMPLATEA>(modern.data()), parent, proc, param);
    }
    HWND WINAPI MyCreateDialogIndirectParamW(HINSTANCE inst, LPCDLGTEMPLATEW tmpl, HWND parent, DLGPROC proc, LPARAM param)
    {
        Scope scope;
        auto modern = MemoryTemplate(tmpl);
        return real.CreateDialogIndirectParamW(inst, modern.empty() ? tmpl : reinterpret_cast<LPCDLGTEMPLATEW>(modern.data()), parent, proc, param);
    }

    // ---- windows and classes -----------------------------------------------------------------------
    HWND WINAPI MyCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
    {
        Scope scope;
        return real.CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    }
    HWND WINAPI MyCreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
    {
        Scope scope;
        return real.CreateWindowExW(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    }
    struct InMessageBox
    {
        InMessageBox() { ++messageBox; }
        ~InMessageBox() { --messageBox; }
    };
    int WINAPI MyMessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type) { Scope scope; InMessageBox box; return real.MessageBoxA(owner, text, caption, type); }
    int WINAPI MyMessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type) { Scope scope; InMessageBox box; return real.MessageBoxW(owner, text, caption, type); }
    BOOL WINAPI MyGetClassInfoExA(HINSTANCE inst, LPCSTR name, LPWNDCLASSEXA info) { Scope scope; return real.GetClassInfoExA(inst, name, info); }
    BOOL WINAPI MyGetClassInfoExW(HINSTANCE inst, LPCWSTR name, LPWNDCLASSEXW info) { Scope scope; return real.GetClassInfoExW(inst, name, info); }
    BOOL WINAPI MyGetClassInfoA(HINSTANCE inst, LPCSTR name, LPWNDCLASSA info) { Scope scope; return real.GetClassInfoA(inst, name, info); }
    BOOL WINAPI MyGetClassInfoW(HINSTANCE inst, LPCWSTR name, LPWNDCLASSW info) { Scope scope; return real.GetClassInfoW(inst, name, info); }

    // A class painted in the light system colours paints in the dark ones.
    HBRUSH DarkBackground(HBRUSH brush)
    {
        const auto value = reinterpret_cast<uintptr_t>(brush);
        if (value >= 1 && value <= 31) return BrushFor(DarkColor(static_cast<int>(value - 1)));
        for (int index : {COLOR_BTNFACE, COLOR_WINDOW, COLOR_APPWORKSPACE})
            if (real.GetSysColorBrush && brush == real.GetSysColorBrush(index)) return BrushFor(DarkColor(index));
        return brush;
    }
    ATOM WINAPI MyRegisterClassExA(const WNDCLASSEXA* cls)
    {
        if (!cls || current != Theme::ModernDark) return real.RegisterClassExA(cls);
        WNDCLASSEXA copy = *cls;
        copy.hbrBackground = DarkBackground(copy.hbrBackground);
        return real.RegisterClassExA(&copy);
    }
    ATOM WINAPI MyRegisterClassExW(const WNDCLASSEXW* cls)
    {
        if (!cls || current != Theme::ModernDark) return real.RegisterClassExW(cls);
        WNDCLASSEXW copy = *cls;
        copy.hbrBackground = DarkBackground(copy.hbrBackground);
        return real.RegisterClassExW(&copy);
    }
    ATOM WINAPI MyRegisterClassA(const WNDCLASSA* cls)
    {
        if (!cls || current != Theme::ModernDark) return real.RegisterClassA(cls);
        WNDCLASSA copy = *cls;
        copy.hbrBackground = DarkBackground(copy.hbrBackground);
        return real.RegisterClassA(&copy);
    }
    ATOM WINAPI MyRegisterClassW(const WNDCLASSW* cls)
    {
        if (!cls || current != Theme::ModernDark) return real.RegisterClassW(cls);
        WNDCLASSW copy = *cls;
        copy.hbrBackground = DarkBackground(copy.hbrBackground);
        return real.RegisterClassW(&copy);
    }
    DWORD WINAPI MyGetSysColor(int index) { return current == Theme::ModernDark ? DarkColor(index) : real.GetSysColor(index); }
    HBRUSH WINAPI MyGetSysColorBrush(int index) { return current == Theme::ModernDark ? BrushFor(DarkColor(index)) : real.GetSysColorBrush(index); }

    BOOL WINAPI MyGetOpenFileNameA(LPOPENFILENAMEA o) { Foreign f; return real.GetOpenFileNameA(o); }
    BOOL WINAPI MyGetOpenFileNameW(LPOPENFILENAMEW o) { Foreign f; return real.GetOpenFileNameW(o); }
    BOOL WINAPI MyGetSaveFileNameA(LPOPENFILENAMEA o) { Foreign f; return real.GetSaveFileNameA(o); }
    BOOL WINAPI MyGetSaveFileNameW(LPOPENFILENAMEW o) { Foreign f; return real.GetSaveFileNameW(o); }
    BOOL WINAPI MyChooseColorA(LPCHOOSECOLORA c) { Foreign f; return real.ChooseColorA(c); }
    BOOL WINAPI MyChooseColorW(LPCHOOSECOLORW c) { Foreign f; return real.ChooseColorW(c); }
    PIDLIST_ABSOLUTE WINAPI MySHBrowseForFolderA(LPBROWSEINFOA b) { Foreign f; return real.SHBrowseForFolderA(b); }
    PIDLIST_ABSOLUTE WINAPI MySHBrowseForFolderW(LPBROWSEINFOW b) { Foreign f; return real.SHBrowseForFolderW(b); }

    // ---- what a window is --------------------------------------------------------------------------
    enum class Kind { Other, TopLevel, Dialog, MessageBox, Button, Edit, ComboBox, ComboList, ListBox, ListView, TreeView, Header, Toolbar, Tab, Status, Tooltip, ScrollBar, Static, Skip };

    bool Has(const std::wstring& text, const wchar_t* part)
    {
        std::wstring a = text, b = part;
        for (auto& c : a) c = towlower(c);
        for (auto& c : b) c = towlower(c);
        return a.find(b) != std::wstring::npos;
    }
    bool EndsWith(const std::wstring& text, const wchar_t* tail)
    {
        const size_t n = wcslen(tail);
        return text.size() >= n && !_wcsicmp(text.c_str() + text.size() - n, tail);
    }
    // By the system class underneath (RealGetWindowClass), or the end of the stock editor's
    // superclass names (SplinterCell2UnrealWButton and the like).
    Kind Classify(HWND window)
    {
        wchar_t name[128] = {}, base[128] = {};
        GetClassNameW(window, name, 127);
        RealGetWindowClassW(window, base, 127);
        const std::wstring n = name, b = base;
        auto is = [&](const wchar_t* cls) { return !_wcsicmp(b.c_str(), cls) || EndsWith(n, cls); };
        if (Has(n, L"Viewport") || Has(n, L"IME") || Has(n, L"DirectUIHWND") || Has(n, L"RichEdit") || Has(b, L"RichEdit")) return Kind::Skip;
        if (is(L"tooltips_class32")) return Kind::Tooltip;
        if (is(L"SysListView32") || is(L"ListView")) return Kind::ListView;
        if (is(L"SysTreeView32") || is(L"TreeView")) return Kind::TreeView;
        if (is(L"SysHeader32")) return Kind::Header;
        if (is(L"ToolbarWindow32")) return Kind::Toolbar;
        if (is(L"SysTabControl32") || is(L"TabControl")) return Kind::Tab;
        if (is(L"msctls_statusbar32")) return Kind::Status;
        if (is(L"ComboLBox")) return Kind::ComboList;
        if (is(L"ComboBox") || is(L"ComboBoxEx32")) return Kind::ComboBox;
        if (is(L"ListBox")) return Kind::ListBox;
        if (is(L"Edit")) return Kind::Edit;
        if (is(L"Button") || is(L"CheckBox")) return Kind::Button;
        if (is(L"ScrollBar")) return Kind::ScrollBar;
        if (is(L"Static") || is(L"Label")) return Kind::Static;
        if (b == L"#32770" || n == L"#32770") return Kind::Dialog;
        if (!(GetWindowLongW(window, GWL_STYLE) & WS_CHILD)) return Kind::TopLevel;
        return Kind::Other;
    }

    // ---- dark painting -----------------------------------------------------------------------------
    // The UAH menu messages (undocumented; the menu bar asks its window to draw it).
    constexpr UINT WM_UAHDRAWMENU = 0x0091;
    constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;
    struct UAHMENU { HMENU hmenu; HDC hdc; DWORD dwFlags; };
    struct UAHMENUITEM { int iPosition; DWORD metrics[8]; DWORD popup[5]; };
    struct UAHDRAWMENUITEM { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; };

    RECT MenuBarRect(HWND window)
    {
        MENUBARINFO bar{sizeof(bar)};
        RECT r{};
        if (!GetMenuBarInfo(window, OBJID_MENU, 0, &bar)) return r;
        RECT frame{};
        GetWindowRect(window, &frame);
        r = bar.rcBar;
        OffsetRect(&r, -frame.left, -frame.top);
        return r;
    }
    // The light line the system draws between the menu bar and the client area.
    void CoverMenuLine(HWND window)
    {
        if (!GetMenu(window) || (GetWindowLongW(window, GWL_STYLE) & WS_CHILD)) return;
        RECT client{};
        GetClientRect(window, &client);
        MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        RECT frame{};
        GetWindowRect(window, &frame);
        OffsetRect(&client, -frame.left, -frame.top);
        RECT line = client;
        line.bottom = line.top;
        line.top = line.bottom - 1;
        HDC dc = GetWindowDC(window);
        FillRect(dc, &line, BrushFor(Appearance::DarkPalette().menuBar));
        ReleaseDC(window, dc);
    }
    void DrawMenuItem(UAHDRAWMENUITEM* item)
    {
        const auto p = Appearance::DarkPalette();
        wchar_t text[256] = {};
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_STRING;
        info.dwTypeData = text;
        info.cch = 255;
        GetMenuItemInfoW(item->um.hmenu, item->umi.iPosition, TRUE, &info);
        const UINT state = item->dis.itemState;
        COLORREF background = p.menuBar, foreground = p.text;
        if (state & (ODS_HOTLIGHT | ODS_SELECTED)) background = p.hot;
        if (state & (ODS_GRAYED | ODS_DISABLED | ODS_INACTIVE)) foreground = p.grayText;
        FillRect(item->um.hdc, &item->dis.rcItem, BrushFor(background));
        SetBkMode(item->um.hdc, TRANSPARENT);
        SetTextColor(item->um.hdc, foreground);
        UINT flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
        if (state & ODS_NOACCEL) flags |= DT_HIDEPREFIX;
        DrawTextW(item->um.hdc, text, -1, &item->dis.rcItem, flags);
    }

    // A popup menu item (the frame's menus and the stock right-click menus are drawn by their owner
    // once it answers the UAH messages): check mark, text, shortcut and submenu arrow.
    void DrawPopupItem(UAHDRAWMENUITEM* item)
    {
        const auto p = Appearance::DarkPalette();
        HDC dc = item->um.hdc;
        RECT r = item->dis.rcItem;
        wchar_t text[512] = {};
        MENUITEMINFOW info{sizeof(info)};
        info.fMask = MIIM_STRING | MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU;
        info.dwTypeData = text;
        info.cch = 511;
        GetMenuItemInfoW(item->um.hmenu, item->umi.iPosition, TRUE, &info);
        const UINT state = item->dis.itemState;
        const bool hot = (state & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
        const bool grey = (state & (ODS_GRAYED | ODS_DISABLED)) != 0;
        real.FillRect(dc, &r, BrushFor(hot && !grey ? p.hot : p.face));
        if (info.fType & MFT_SEPARATOR)
        {
            RECT line{r.left + 4, (r.top + r.bottom) / 2, r.right - 4, (r.top + r.bottom) / 2 + 1};
            real.FillRect(dc, &line, BrushFor(p.border));
            return;
        }
        SetBkMode(dc, TRANSPARENT);
        real.SetTextColor(dc, grey ? p.grayText : p.text);
        const int height = r.bottom - r.top;
        if (info.fState & MFS_CHECKED)
        {
            RECT mark{r.left, r.top, r.left + height + 4, r.bottom};
            DrawTextW(dc, L"\x2713", 1, &mark, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        std::wstring label = text, shortcut;
        if (const size_t tab = label.find(L'\t'); tab != std::wstring::npos)
        {
            shortcut = label.substr(tab + 1);
            label.resize(tab);
        }
        const UINT prefix = (state & ODS_NOACCEL) ? DT_HIDEPREFIX : 0;
        RECT left{r.left + height + 8, r.top, r.right - 8, r.bottom};
        DrawTextW(dc, label.c_str(), -1, &left, DT_LEFT | DT_VCENTER | DT_SINGLELINE | prefix);
        RECT right{r.left, r.top, r.right - height, r.bottom};
        if (!shortcut.empty()) DrawTextW(dc, shortcut.c_str(), -1, &right, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (info.hSubMenu)
        {
            RECT arrow{r.right - height, r.top, r.right - 4, r.bottom};
            DrawTextW(dc, L"\x203A", 1, &arrow, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    void PaintTabs(HWND tab)
    {
        const auto p = Appearance::DarkPalette();
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(tab, &ps);
        RECT client{};
        GetClientRect(tab, &client);
        FillRect(dc, &client, BrushFor(p.face));
        HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(tab, WM_GETFONT, 0, 0)));
        SetBkMode(dc, TRANSPARENT);
        const int count = TabCtrl_GetItemCount(tab), selected = TabCtrl_GetCurSel(tab);
        for (int i = 0; i < count; ++i)
        {
            RECT r{};
            TabCtrl_GetItemRect(tab, i, &r);
            wchar_t text[128] = {};
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = text;
            item.cchTextMax = 127;
            SendMessageW(tab, TCM_GETITEMW, i, reinterpret_cast<LPARAM>(&item));
            FillRect(dc, &r, BrushFor(i == selected ? p.window : p.face));
            FrameRect(dc, &r, BrushFor(p.border));
            if (i == selected)
            {
                RECT accent = r;
                accent.bottom = accent.top + 2;
                FillRect(dc, &accent, BrushFor(p.highlight));
            }
            SetTextColor(dc, i == selected ? p.text : p.grayText);
            DrawTextW(dc, text, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        }
        if (!(GetWindowLongW(tab, GWL_STYLE) & TCS_BUTTONS))
        {
            RECT page = client;
            TabCtrl_AdjustRect(tab, FALSE, &page);
            InflateRect(&page, 2, 2);
            FrameRect(dc, &page, BrushFor(p.border));
        }
        SelectObject(dc, oldFont);
        EndPaint(tab, &ps);
    }

    void SetDarkTheme(HWND window, const wchar_t* name, const wchar_t* ids = nullptr)
    {
        if (allowDark) allowDark(window, true);
        SetWindowTheme(window, name, ids);
    }

    // Once the window exists (its WM_CREATE has run).
    void Created(HWND window, Kind kind)
    {
        const bool dark = current == Theme::ModernDark;
        if (!dark)
        {
            if (kind == Kind::ListView || kind == Kind::TreeView) SetWindowTheme(window, L"Explorer", nullptr);
            return;
        }
        const auto p = Appearance::DarkPalette();
        const DWORD style = GetWindowLongW(window, GWL_STYLE);
        if (!dwmSetAttribute)
            if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll"))
                dwmSetAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (!(style & WS_CHILD) && dwmSetAttribute)
        {
            const BOOL on = TRUE;
            dwmSetAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
        }
        switch (kind)
        {
        case Kind::Button:
        {
            DWORD type = style & BS_TYPEMASK;
            if (style & BS_PUSHLIKE) type = BS_PUSHBUTTON; // a toggle drawn as a push button
            // Themed check boxes and group boxes ignore WM_CTLCOLOR text colours; drawn classic, they
            // take them.
            if (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_SPLITBUTTON || type == BS_DEFSPLITBUTTON) SetDarkTheme(window, L"DarkMode_Explorer");
            else if (type != BS_OWNERDRAW) SetWindowTheme(window, L"", L"");
            break;
        }
        case Kind::Edit: SetDarkTheme(window, L"DarkMode_CFD"); break;
        case Kind::ComboBox:
        {
            SetDarkTheme(window, L"DarkMode_CFD");
            // Theming the combo re-themes its drop-down list (light scroll bar); themed again after it.
            COMBOBOXINFO info{sizeof(info)};
            if (GetComboBoxInfo(window, &info) && info.hwndList) SetWindowTheme(info.hwndList, L"DarkMode_Explorer", nullptr);
            break;
        }
        case Kind::ComboList:
        case Kind::ListBox:
            // Dark scroll bars from the theme alone: a list box allowed dark mode paints its selected
            // row white, so the row's white text cannot be read.
            SetWindowTheme(window, L"DarkMode_Explorer", nullptr);
            break;
        case Kind::ListView:
            SetDarkTheme(window, L"DarkMode_Explorer");
            ListView_SetBkColor(window, p.window);
            ListView_SetTextBkColor(window, p.window);
            ListView_SetTextColor(window, p.text);
            if (HWND header = ListView_GetHeader(window)) SetDarkTheme(header, L"DarkMode_ItemsView");
            break;
        case Kind::TreeView:
            SetDarkTheme(window, L"DarkMode_Explorer");
            TreeView_SetBkColor(window, p.window);
            TreeView_SetTextColor(window, p.text);
            TreeView_SetLineColor(window, p.border);
            break;
        case Kind::Header: SetDarkTheme(window, L"DarkMode_ItemsView"); break;
        default: SetDarkTheme(window, L"DarkMode_Explorer"); break;
        }
    }

    LRESULT CtlColor(UINT message, HDC dc, HWND control)
    {
        const auto p = Appearance::DarkPalette();
        COLORREF background = p.face;
        if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) background = p.window;
        if (message == WM_CTLCOLORSTATIC && control)
        {
            // A read-only edit asks as a static; give it the edit background.
            wchar_t cls[32] = {};
            RealGetWindowClassW(control, cls, 31);
            if (!_wcsicmp(cls, L"Edit")) background = p.window;
        }
        SetTextColor(dc, control && !IsWindowEnabled(control) ? p.grayText : p.text);
        SetBkColor(dc, background);
        return reinterpret_cast<LRESULT>(BrushFor(background));
    }

    // A disabled text static: the system draws its text embossed in the light theme's colours (a
    // white copy one pixel off), which smears on a dark background. Plain grey text instead.
    bool PaintDisabledStatic(HWND window)
    {
        const DWORD style = GetWindowLongW(window, GWL_STYLE);
        const DWORD type = style & SS_TYPEMASK;
        if (type != SS_LEFT && type != SS_CENTER && type != SS_RIGHT && type != SS_LEFTNOWORDWRAP && type != SS_SIMPLE) return false;
        const auto p = Appearance::DarkPalette();
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH brush = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(window), WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(window)));
        real.FillRect(dc, &client, brush ? brush : BrushFor(p.face));
        const int length = GetWindowTextLengthW(window);
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(window, text.data(), length + 1);
        HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0)));
        SetBkMode(dc, TRANSPARENT);
        real.SetTextColor(dc, p.grayText);
        UINT flags = DT_EXPANDTABS | ((style & SS_NOPREFIX) ? DT_NOPREFIX : 0);
        if (type == SS_CENTER) flags |= DT_CENTER | DT_WORDBREAK;
        else if (type == SS_RIGHT) flags |= DT_RIGHT | DT_WORDBREAK;
        else if (type == SS_LEFT) flags |= DT_WORDBREAK;
        else flags |= DT_SINGLELINE;
        DrawTextW(dc, text.c_str(), length, &client, flags);
        SelectObject(dc, old);
        EndPaint(window, &ps);
        return true;
    }

    LRESULT CALLBACK Subclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
    {
        const Kind kind = static_cast<Kind>(data);
        // Version 6 buttons send WM_NOTIFY (custom draw, hot item) as they paint; version 5 ones
        // never did. The stock editor's windows answer any WM_NOTIFY with OnCommand(control id),
        // so each repaint would press the button. Their parents never see these.
        if (message == WM_NOTIFY)
        {
            auto header = reinterpret_cast<NMHDR*>(lParam);
            if (header && header->hwndFrom && Classify(header->hwndFrom) == Kind::Button) return 0;
        }
        if (message == WM_CREATE)
        {
            const LRESULT result = defSubclass(window, message, wParam, lParam);
            if (result != -1) Created(window, kind);
            return result;
        }
        if (message == WM_WINDOWPOSCHANGED && kind == Kind::Static)
        {
            // A wrapped text static that changes size keeps its old lines under the new ones (version 6
            // statics do not repaint the whole of themselves): an intro sized to its text then shows
            // both wraps at once. Repainted whole, with its background.
            const auto pos = reinterpret_cast<const WINDOWPOS*>(lParam);
            const DWORD type = GetWindowLongW(window, GWL_STYLE) & SS_TYPEMASK;
            if (pos && !(pos->flags & SWP_NOSIZE) && (type == SS_LEFT || type == SS_CENTER || type == SS_RIGHT)) InvalidateRect(window, nullptr, TRUE);
        }
        if (current != Theme::ModernDark) return defSubclass(window, message, wParam, lParam);
        switch (message)
        {
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSCROLLBAR:
        case WM_CTLCOLORMSGBOX:
            defSubclass(window, message, wParam, lParam);
            return CtlColor(message, reinterpret_cast<HDC>(wParam), reinterpret_cast<HWND>(lParam));
        case WM_UAHDRAWMENU:
        {
            auto menu = reinterpret_cast<UAHMENU*>(lParam);
            if (!menu || menu->hmenu != GetMenu(window)) break; // only the bar: popups are themed
            RECT r = MenuBarRect(window);
            FillRect(menu->hdc, &r, BrushFor(Appearance::DarkPalette().menuBar));
            return 0;
        }
        case WM_UAHDRAWMENUITEM:
        {
            auto item = reinterpret_cast<UAHDRAWMENUITEM*>(lParam);
            if (!item) break;
            if (item->um.hmenu == GetMenu(window)) DrawMenuItem(item); else DrawPopupItem(item);
            return 0;
        }
        case WM_NCPAINT:
        case WM_NCACTIVATE:
        {
            const LRESULT result = defSubclass(window, message, wParam, lParam);
            CoverMenuLine(window);
            return result;
        }
        case WM_ERASEBKGND:
            if (kind == Kind::Toolbar || kind == Kind::Status)
            {
                RECT r{};
                GetClientRect(window, &r);
                FillRect(reinterpret_cast<HDC>(wParam), &r, BrushFor(Appearance::DarkPalette().face));
                return 1;
            }
            if (kind == Kind::Tab) return 1;
            break;
        case WM_PAINT:
            if (kind == Kind::Tab) { PaintTabs(window); return 0; }
            if (kind == Kind::Static && !IsWindowEnabled(window) && PaintDisabledStatic(window)) return 0;
            if (kind == Kind::MessageBox)
            {
                // The system paints a light band behind the buttons; the icon and text are child
                // windows, so the dark background (WM_CTLCOLORDLG) is all there is to draw.
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(window, &ps);
                RECT client{};
                GetClientRect(window, &client);
                real.FillRect(dc, &client, BrushFor(Appearance::DarkPalette().face));
                EndPaint(window, &ps);
                return 0;
            }
            break;
        case WM_NOTIFY:
        {
            // Toolbar button text and list view column titles in the dark text colour.
            auto header = reinterpret_cast<NMHDR*>(lParam);
            if (header && header->code == NM_CUSTOMDRAW)
            {
                wchar_t cls[64] = {};
                GetClassNameW(header->hwndFrom, cls, 63);
                if (Has(cls, L"SysHeader32"))
                {
                    auto draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
                    if (draw->dwDrawStage == CDDS_PREPAINT) return defSubclass(window, message, wParam, lParam) | CDRF_NOTIFYITEMDRAW;
                    if (draw->dwDrawStage == CDDS_ITEMPREPAINT)
                    {
                        const LRESULT result = defSubclass(window, message, wParam, lParam);
                        SetTextColor(draw->hdc, Appearance::DarkPalette().text);
                        return result;
                    }
                }
                if (Has(cls, L"ToolbarWindow32"))
                {
                    auto draw = reinterpret_cast<NMTBCUSTOMDRAW*>(lParam);
                    if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
                    {
                        const LRESULT result = defSubclass(window, message, wParam, lParam);
                        return result | CDRF_NOTIFYITEMDRAW;
                    }
                    if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                    {
                        draw->clrText = Appearance::DarkPalette().text;
                        return defSubclass(window, message, wParam, lParam) | TBCDRF_USECDCOLORS;
                    }
                }
            }
            break;
        }
        }
        return defSubclass(window, message, wParam, lParam);
    }

    LRESULT CALLBACK CbtProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code == HCBT_CREATEWND && !foreignDialog && setSubclass)
        {
            HWND window = reinterpret_cast<HWND>(wParam);
            Kind kind = Classify(window);
            if (kind == Kind::Dialog && messageBox) kind = Kind::MessageBox;
            if (kind != Kind::Skip)
                setSubclass(window, Subclass, 0x4150, static_cast<DWORD_PTR>(kind));
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // ---- import tables -----------------------------------------------------------------------------
    // Points `module`'s imports from `dll` at what `pick` returns (null: leave that one).
    template <class Pick> int PatchImports(HMODULE module, const char* dll, Pick pick)
    {
        auto base = reinterpret_cast<uint8_t*>(module);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!directory.VirtualAddress) return 0;
        int patched = 0;
        for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress); d->Name; ++d)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) || !d->OriginalFirstThunk) continue;
            auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
            auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
            for (; names->u1.AddressOfData; ++names, ++slots)
            {
                const char* name = nullptr;
                WORD ordinal = 0;
                if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) ordinal = static_cast<WORD>(IMAGE_ORDINAL(names->u1.Ordinal));
                else name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name;
                void* replacement = pick(name, ordinal);
                if (!replacement) continue;
                DWORD old = 0;
                if (!VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old)) continue;
                slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
                VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
                ++patched;
            }
        }
        return patched;
    }

    struct Hook { const char* dll; const char* name; void* replacement; };

    void LoadOriginals()
    {
#define ORIGINAL(module, name) real.name = Real<decltype(real.name)>(module, #name)
        ORIGINAL("user32.dll", CreateWindowExA); ORIGINAL("user32.dll", CreateWindowExW);
        ORIGINAL("user32.dll", DialogBoxParamA); ORIGINAL("user32.dll", DialogBoxParamW);
        ORIGINAL("user32.dll", DialogBoxIndirectParamA); ORIGINAL("user32.dll", DialogBoxIndirectParamW);
        ORIGINAL("user32.dll", CreateDialogParamA); ORIGINAL("user32.dll", CreateDialogParamW);
        ORIGINAL("user32.dll", CreateDialogIndirectParamA); ORIGINAL("user32.dll", CreateDialogIndirectParamW);
        ORIGINAL("user32.dll", MessageBoxA); ORIGINAL("user32.dll", MessageBoxW);
        ORIGINAL("user32.dll", GetClassInfoExA); ORIGINAL("user32.dll", GetClassInfoExW);
        ORIGINAL("user32.dll", GetClassInfoA); ORIGINAL("user32.dll", GetClassInfoW);
        ORIGINAL("user32.dll", RegisterClassExA); ORIGINAL("user32.dll", RegisterClassExW);
        ORIGINAL("user32.dll", RegisterClassA); ORIGINAL("user32.dll", RegisterClassW);
        ORIGINAL("user32.dll", GetSysColor); ORIGINAL("user32.dll", GetSysColorBrush);
        ORIGINAL("gdi32.dll", CreateFontA); ORIGINAL("gdi32.dll", CreateFontW);
        ORIGINAL("gdi32.dll", CreateFontIndirectA); ORIGINAL("gdi32.dll", CreateFontIndirectW);
        ORIGINAL("gdi32.dll", GetStockObject);
        ORIGINAL("gdi32.dll", CreateSolidBrush); ORIGINAL("gdi32.dll", CreatePen);
        ORIGINAL("gdi32.dll", SetTextColor); ORIGINAL("gdi32.dll", SetBkColor);
        ORIGINAL("user32.dll", FillRect); ORIGINAL("user32.dll", DrawEdge);
        ORIGINAL("user32.dll", LoadImageA); ORIGINAL("user32.dll", LoadImageW);
#undef ORIGINAL
        HMODULE comdlg = LoadLibraryA("comdlg32.dll");
        HMODULE shell = LoadLibraryA("shell32.dll");
        real.GetOpenFileNameA = reinterpret_cast<decltype(real.GetOpenFileNameA)>(GetProcAddress(comdlg, "GetOpenFileNameA"));
        real.GetOpenFileNameW = reinterpret_cast<decltype(real.GetOpenFileNameW)>(GetProcAddress(comdlg, "GetOpenFileNameW"));
        real.GetSaveFileNameA = reinterpret_cast<decltype(real.GetSaveFileNameA)>(GetProcAddress(comdlg, "GetSaveFileNameA"));
        real.GetSaveFileNameW = reinterpret_cast<decltype(real.GetSaveFileNameW)>(GetProcAddress(comdlg, "GetSaveFileNameW"));
        real.ChooseColorA = reinterpret_cast<decltype(real.ChooseColorA)>(GetProcAddress(comdlg, "ChooseColorA"));
        real.ChooseColorW = reinterpret_cast<decltype(real.ChooseColorW)>(GetProcAddress(comdlg, "ChooseColorW"));
        real.SHBrowseForFolderA = reinterpret_cast<decltype(real.SHBrowseForFolderA)>(GetProcAddress(shell, "SHBrowseForFolderA"));
        real.SHBrowseForFolderW = reinterpret_cast<decltype(real.SHBrowseForFolderW)>(GetProcAddress(shell, "SHBrowseForFolderW"));
    }

    std::vector<Hook> Hooks()
    {
        std::vector<Hook> hooks = {
            {"user32.dll", "CreateWindowExA", MyCreateWindowExA}, {"user32.dll", "CreateWindowExW", MyCreateWindowExW},
            {"user32.dll", "DialogBoxParamA", MyDialogBoxParamA}, {"user32.dll", "DialogBoxParamW", MyDialogBoxParamW},
            {"user32.dll", "DialogBoxIndirectParamA", MyDialogBoxIndirectParamA}, {"user32.dll", "DialogBoxIndirectParamW", MyDialogBoxIndirectParamW},
            {"user32.dll", "CreateDialogParamA", MyCreateDialogParamA}, {"user32.dll", "CreateDialogParamW", MyCreateDialogParamW},
            {"user32.dll", "CreateDialogIndirectParamA", MyCreateDialogIndirectParamA}, {"user32.dll", "CreateDialogIndirectParamW", MyCreateDialogIndirectParamW},
            {"user32.dll", "MessageBoxA", MyMessageBoxA}, {"user32.dll", "MessageBoxW", MyMessageBoxW},
            {"user32.dll", "GetClassInfoExA", MyGetClassInfoExA}, {"user32.dll", "GetClassInfoExW", MyGetClassInfoExW},
            {"user32.dll", "GetClassInfoA", MyGetClassInfoA}, {"user32.dll", "GetClassInfoW", MyGetClassInfoW},
            {"gdi32.dll", "CreateFontA", MyCreateFontA}, {"gdi32.dll", "CreateFontW", MyCreateFontW},
            {"gdi32.dll", "CreateFontIndirectA", MyCreateFontIndirectA}, {"gdi32.dll", "CreateFontIndirectW", MyCreateFontIndirectW},
            {"gdi32.dll", "GetStockObject", MyGetStockObject},
            {"comdlg32.dll", "GetOpenFileNameA", MyGetOpenFileNameA}, {"comdlg32.dll", "GetOpenFileNameW", MyGetOpenFileNameW},
            {"comdlg32.dll", "GetSaveFileNameA", MyGetSaveFileNameA}, {"comdlg32.dll", "GetSaveFileNameW", MyGetSaveFileNameW},
            {"comdlg32.dll", "ChooseColorA", MyChooseColorA}, {"comdlg32.dll", "ChooseColorW", MyChooseColorW},
            {"shell32.dll", "SHBrowseForFolderA", MySHBrowseForFolderA}, {"shell32.dll", "SHBrowseForFolderW", MySHBrowseForFolderW},
        };
        if (current == Theme::ModernDark)
        {
            hooks.push_back({"user32.dll", "RegisterClassExA", MyRegisterClassExA});
            hooks.push_back({"user32.dll", "RegisterClassExW", MyRegisterClassExW});
            hooks.push_back({"user32.dll", "RegisterClassA", MyRegisterClassA});
            hooks.push_back({"user32.dll", "RegisterClassW", MyRegisterClassW});
            hooks.push_back({"user32.dll", "GetSysColor", MyGetSysColor});
            hooks.push_back({"user32.dll", "GetSysColorBrush", MyGetSysColorBrush});
            hooks.push_back({"user32.dll", "FillRect", MyFillRect});
            hooks.push_back({"user32.dll", "DrawEdge", MyDrawEdge});
            hooks.push_back({"user32.dll", "LoadImageA", MyLoadImageA});
            hooks.push_back({"user32.dll", "LoadImageW", MyLoadImageW});
            hooks.push_back({"gdi32.dll", "CreateSolidBrush", MyCreateSolidBrush});
            hooks.push_back({"gdi32.dll", "CreatePen", MyCreatePen});
            hooks.push_back({"gdi32.dll", "SetTextColor", MySetTextColor});
            hooks.push_back({"gdi32.dll", "SetBkColor", MySetBkColor});
        }
        return hooks;
    }

    int Patch(HMODULE module, const std::vector<Hook>& hooks)
    {
        int patched = 0;
        for (const char* dll : {"user32.dll", "gdi32.dll", "comdlg32.dll", "shell32.dll"})
            patched += PatchImports(module, dll, [&](const char* name, WORD) -> void* {
                if (!name) return nullptr;
                for (auto& hook : hooks)
                    if (!_stricmp(hook.dll, dll) && !strcmp(hook.name, name)) return hook.replacement;
                return nullptr;
            });
        // Version 6 for the editor's own Common Controls calls (toolbars, image lists).
        if (comctl6)
            patched += PatchImports(module, "comctl32.dll", [&](const char* name, WORD ordinal) -> void* {
                return reinterpret_cast<void*>(GetProcAddress(comctl6, name ? name : MAKEINTRESOURCEA(ordinal)));
            });
        return patched;
    }

    bool CreateContext()
    {
        // Our manifest resource (id 3: the loader never applies it to this DLL's own imports).
        ACTCTXW actx{sizeof(actx)};
        actx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        actx.hModule = g_hReloadedDll;
        actx.lpResourceName = MAKEINTRESOURCEW(3);
        context = CreateActCtxW(&actx);
        if (context == INVALID_HANDLE_VALUE)
        {
            Logger::log("Appearance: no Common Controls 6 context, error " + std::to_string(GetLastError()));
            return false;
        }
        ULONG_PTR cookie = 0;
        if (ActivateActCtx(context, &cookie))
        {
            comctl6 = LoadLibraryW(L"comctl32.dll");
            DeactivateActCtx(0, cookie);
        }
        if (comctl6 == GetModuleHandleW(L"comctl32.dll") && comctl6)
        {
            // The handle outside the context is the same module: not version 6.
            DLLVERSIONINFO version{sizeof(version)};
            auto getVersion = reinterpret_cast<DLLGETVERSIONPROC>(GetProcAddress(comctl6, "DllGetVersion"));
            if (getVersion && SUCCEEDED(getVersion(&version)) && version.dwMajorVersion < 6) comctl6 = nullptr;
        }
        if (!comctl6) { Logger::log("Appearance: Common Controls 6 did not load"); return false; }
        setSubclass = reinterpret_cast<decltype(setSubclass)>(GetProcAddress(comctl6, "SetWindowSubclass"));
        defSubclass = reinterpret_cast<decltype(defSubclass)>(GetProcAddress(comctl6, "DefSubclassProc"));
        return setSubclass && defSubclass;
    }

    void PrepareDark()
    {
        HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
        allowDark = reinterpret_cast<AllowDarkModeForWindowFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(133)));
        auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
        auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(136)));
        if (setMode) setMode(2); // ForceDark: popup menus and scroll bars in the dark theme
        if (flush) flush();
    }
}

Appearance::Theme Appearance::Saved()
{
    char value[64] = {};
    GetPrivateProfileStringA("Appearance", "Theme", "Classic", value, sizeof(value), IniPath().c_str());
    return ParseTheme(value);
}

void Appearance::Save(Theme theme)
{
    WritePrivateProfileStringA("Appearance", "Theme", ThemeKey(theme), IniPath().c_str());
}

Appearance::Theme Appearance::Current() { return current; }

Appearance::Palette Appearance::Colors() { return DarkPalette(); }

HBRUSH Appearance::Brush(COLORREF color) { return BrushFor(color); }

int Appearance::FitHeight(HWND text, int width, int fixed, int most)
{
    if (current == Theme::Classic || !text || width <= 0) return fixed;
    const int length = GetWindowTextLengthW(text);
    if (length <= 0) return fixed;
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(text, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    HDC dc = GetDC(text);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(text, WM_GETFONT, 0, 0)));
    RECT r{0, 0, width, 0};
    DrawTextW(dc, value.c_str(), length, &r, DT_CALCRECT | DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(text, dc);
    return (std::max)(fixed, (std::min)(most, static_cast<int>(r.bottom) + 2));
}

void Appearance::Initialize()
{
    const Theme wanted = Saved();
    if (wanted == Theme::Classic) return; // the stock look: nothing is touched
    if (!CreateContext()) return;
    current = wanted;
    LoadOriginals();
    LOGFONTW font{};
    font.lfHeight = kModernHeight;
    font.lfWeight = FW_NORMAL;
    font.lfCharSet = DEFAULT_CHARSET;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, kModernFace);
    uiFont = real.CreateFontIndirectW(&font);
    if (current == Theme::ModernDark) PrepareDark();
    const auto hooks = Hooks();
    const int editor = Patch(GetModuleHandleW(nullptr), hooks);
    const int ours = Patch(g_hReloadedDll, hooks);
    Logger::log(std::string("Appearance: ") + ThemeLabel(current) + " (" + std::to_string(editor) + " editor and " + std::to_string(ours) + " RE+ imports)");
}
