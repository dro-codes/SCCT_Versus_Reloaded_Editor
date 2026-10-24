#include "pch.h"
#undef min
#undef max
#include "CommandPalette.h"
#include "CommandPaletteModel.h"
#include "WorkflowEditor.h"
#include "logger.h"
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace CommandPalette
{
namespace
{
using Model::Entry;

const char kClass[] = "ReloadedCommandPalette";
enum : int { kEdit = 100, kList, kHint };
constexpr int kVisibleRows = 14;
constexpr int kMaxDepth = 8;

struct State
{
    HWND frame = nullptr;
    HWND window = nullptr, edit = nullptr, list = nullptr, hint = nullptr;
    HWND previousFocus = nullptr;
    HFONT font = nullptr, marks = nullptr;
    int rowHeight = 20;
    WNDPROC editProc = nullptr, listProc = nullptr;
    std::vector<Entry> entries;
    std::vector<size_t> shown;
    std::vector<std::string> recent;
};
State g;
HHOOK keyHook = nullptr;
bool swallowP = false; // the P of a Ctrl+Shift+P press, so its key-up is kept too

// ---- the commands of the menu bar ----------------------------------------------------------------
std::string ItemText(HMENU menu, int position)
{
    MENUITEMINFOA info{ sizeof(info) };
    info.fMask = MIIM_STRING;
    if (!GetMenuItemInfoA(menu, position, TRUE, &info) || !info.cch) return {};
    std::string text(info.cch + 1, '\0');
    info.dwTypeData = text.data();
    info.cch = static_cast<UINT>(text.size());
    if (!GetMenuItemInfoA(menu, position, TRUE, &info)) return {};
    text.resize(strlen(text.c_str()));
    return text;
}

// Every command of the menu, depth first in menu order. Each submenu is told
// it is about to open first (WM_INITMENUPOPUP, as Windows does), so a menu
// filled or updated on opening (File > Open Recent) is listed as it would show.
void Walk(HMENU menu, const std::string& parent, int depth, std::vector<Entry>& out)
{
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
    {
        MENUITEMINFOA info{ sizeof(info) };
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoA(menu, i, TRUE, &info) || (info.fType & MFT_SEPARATOR)) continue;
        const auto text = Model::SplitMenuText(ItemText(menu, i));
        if (text.label.empty()) continue; // a bitmap or owner-drawn item without text
        const auto path = Model::JoinPath(parent, text.label);
        if (info.hSubMenu)
        {
            if (depth >= kMaxDepth) continue;
            SendMessageA(g.frame, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(info.hSubMenu), MAKELPARAM(i, FALSE));
            Walk(info.hSubMenu, path, depth + 1, out);
            continue;
        }
        if (!info.wID || info.wID == kOpen) continue;
        Entry entry;
        entry.path = path;
        entry.shortcut = text.shortcut;
        entry.id = info.wID;
        entry.enabled = !(info.fState & (MFS_DISABLED | MFS_GRAYED));
        entry.checked = (info.fState & MFS_CHECKED) != 0;
        out.push_back(entry);
    }
}

std::vector<Entry> Commands()
{
    std::vector<Entry> entries;
    HMENU bar = GetMenu(g.frame);
    if (!bar) return entries;
    SendMessageA(g.frame, WM_INITMENU, reinterpret_cast<WPARAM>(bar), 0);
    Walk(bar, "", 0, entries);
    return entries;
}

// ---- the commands run last -----------------------------------------------------------------------
std::filesystem::path RecentFile() { return Workflow::Editor::Directory() / "command_palette_recent.json"; }
std::vector<std::string> LoadRecent()
{
    std::vector<std::string> recent;
    try
    {
        std::ifstream in(RecentFile());
        if (!in) return recent;
        Workflow::Json list;
        in >> list;
        for (const auto& item : list)
            if (item.is_string() && recent.size() < Model::kRecentMax) recent.push_back(item.get<std::string>());
    }
    catch (const std::exception&) { recent.clear(); }
    return recent;
}
void SaveRecent(const std::vector<std::string>& recent)
{
    try
    {
        std::error_code error;
        std::filesystem::create_directories(RecentFile().parent_path(), error);
        std::ofstream out(RecentFile());
        out << Workflow::Json(recent).dump(2);
    }
    catch (const std::exception&) { /* Only the order of the list is lost. */ }
}

// ---- the window ----------------------------------------------------------------------------------
std::string WindowString(HWND window)
{
    const int length = GetWindowTextLengthA(window);
    std::string text(length + 1, '\0');
    GetWindowTextA(window, text.data(), length + 1);
    text.resize(length);
    return text;
}
void SetHint(const std::string& text) { if (g.hint) SetWindowTextA(g.hint, text.c_str()); }
void ShowCount()
{
    SetHint(std::to_string(g.shown.size()) + " of " + std::to_string(g.entries.size())
            + " commands.   Enter or click: run   Esc: close   Up/Down: move");
}

void Filter()
{
    g.shown = Model::Rank(g.entries, WindowString(g.edit), g.recent);
    SendMessageA(g.list, WM_SETREDRAW, FALSE, 0);
    SendMessageA(g.list, LB_RESETCONTENT, 0, 0);
    for (size_t row = 0; row < g.shown.size(); ++row)
    {
        const auto& entry = g.entries[g.shown[row]];
        const auto at = SendMessageA(g.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Model::RowText(entry).c_str()));
        if (at >= 0) SendMessageA(g.list, LB_SETITEMDATA, at, static_cast<LPARAM>(g.shown[row]));
    }
    if (!g.shown.empty()) SendMessageA(g.list, LB_SETCURSEL, 0, 0);
    SendMessageA(g.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g.list, nullptr, TRUE);
    ShowCount();
}

void Close(bool restoreFocus)
{
    if (!IsWindow(g.window)) return;
    const HWND focus = g.previousFocus;
    DestroyWindow(g.window);
    if (restoreFocus && focus && IsWindow(focus) && IsWindowEnabled(focus)) SetFocus(focus);
}

void Run(int row)
{
    if (row < 0 || row >= static_cast<int>(g.shown.size())) { MessageBeep(MB_OK); return; }
    const Entry entry = g.entries[g.shown[row]];
    if (!entry.enabled)
    {
        MessageBeep(MB_ICONWARNING);
        SetHint(entry.path + " is greyed in its menu just now, so it cannot run.");
        SetFocus(g.edit);
        return;
    }
    g.recent = Model::NoteRun(g.recent, entry.path);
    SaveRecent(g.recent);
    const HWND frame = g.frame;
    Close(true);
    Logger::log("Command Palette: " + entry.path + " (" + std::to_string(entry.id) + ")");
    // As a click on the menu item: the frame gets the command after the palette is gone.
    PostMessageA(frame, WM_COMMAND, MAKEWPARAM(entry.id, 0), 0);
}

void Move(int delta)
{
    const int count = static_cast<int>(SendMessageA(g.list, LB_GETCOUNT, 0, 0));
    if (count <= 0) return;
    int row = static_cast<int>(SendMessageA(g.list, LB_GETCURSEL, 0, 0));
    row = std::clamp(row < 0 ? 0 : row + delta, 0, count - 1);
    SendMessageA(g.list, LB_SETCURSEL, row, 0);
}

// Shared by the box and the list: what the keys do.
bool Key(WPARAM key)
{
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (key)
    {
    case VK_ESCAPE: Close(true); return true;
    case VK_RETURN: Run(static_cast<int>(SendMessageA(g.list, LB_GETCURSEL, 0, 0))); return true;
    case VK_UP: Move(-1); return true;
    case VK_DOWN: Move(1); return true;
    case VK_PRIOR: Move(-(kVisibleRows - 1)); return true;
    case VK_NEXT: Move(kVisibleRows - 1); return true;
    case VK_HOME: if (!ctrl) return false; Move(-1000000); return true;
    case VK_END: if (!ctrl) return false; Move(1000000); return true;
    default: return false;
    }
}

LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_KEYDOWN && Key(w)) return 0;
    if (message == WM_CHAR && (w == '\r' || w == 27 || w == '\n')) return 0; // no beep
    return CallWindowProcA(g.editProc, window, message, w, l);
}

LRESULT CALLBACK ListProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_KEYDOWN && Key(w)) return 0;
    if (message == WM_CHAR)
    {
        // Typing over the list goes on into the box.
        if (w >= 32 || w == 8) { SetFocus(g.edit); SendMessageA(g.edit, WM_CHAR, w, l); }
        return 0;
    }
    const LRESULT result = CallWindowProcA(g.listProc, window, message, w, l);
    if (message == WM_LBUTTONUP)
    {
        const auto hit = SendMessageA(window, LB_ITEMFROMPOINT, 0, l);
        if (!HIWORD(hit)) Run(LOWORD(hit));
        else SetFocus(g.edit);
    }
    return result;
}

void DrawRow(const DRAWITEMSTRUCT& item)
{
    if (item.itemID == static_cast<UINT>(-1)) return;
    const auto index = static_cast<size_t>(item.itemData);
    if (index >= g.entries.size()) return;
    const auto& entry = g.entries[index];
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    HDC dc = item.hDC;
    RECT r = item.rcItem;
    FillRect(dc, &r, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    SetBkMode(dc, TRANSPARENT);
    const COLORREF strong = GetSysColor(!entry.enabled ? COLOR_GRAYTEXT : selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
    const COLORREF weak = GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_GRAYTEXT);
    const UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    HGDIOBJ oldFont = SelectObject(dc, g.font);

    // The tick of a checked command, Marlett's check mark.
    RECT mark{ r.left, r.top, r.left + g.rowHeight, r.bottom };
    if (entry.checked && g.marks)
    {
        SelectObject(dc, g.marks);
        SetTextColor(dc, strong);
        DrawTextA(dc, "a", 1, &mark, flags | DT_CENTER);
        SelectObject(dc, g.font);
    }
    RECT text{ mark.right, r.top, r.right - 8, r.bottom };

    // The shortcut on the right.
    if (!entry.shortcut.empty())
    {
        RECT size{ 0, 0, 0, 0 };
        DrawTextA(dc, entry.shortcut.c_str(), -1, &size, flags | DT_CALCRECT);
        RECT keys{ std::max(text.left, text.right - (size.right - size.left)), r.top, text.right, r.bottom };
        SetTextColor(dc, weak);
        DrawTextA(dc, entry.shortcut.c_str(), -1, &keys, flags | DT_RIGHT);
        text.right = keys.left - 16;
    }
    // The menus above it, quieter, then the command.
    const size_t split = Model::LabelStart(entry.path);
    if (split > 0)
    {
        const std::string menus = entry.path.substr(0, split);
        RECT size{ 0, 0, 0, 0 };
        DrawTextA(dc, menus.c_str(), -1, &size, flags | DT_CALCRECT);
        SetTextColor(dc, entry.enabled ? weak : strong);
        DrawTextA(dc, menus.c_str(), -1, &text, flags | DT_END_ELLIPSIS);
        text.left = std::min(text.right, text.left + (size.right - size.left));
    }
    SetTextColor(dc, strong);
    const std::string label = entry.path.substr(split);
    DrawTextA(dc, label.c_str(), -1, &text, flags | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
    if (item.itemState & ODS_FOCUS) { /* the selection colour is enough */ }
}

void Layout(HWND window)
{
    RECT client;
    GetClientRect(window, &client);
    const int pad = 6, width = client.right - 2 * pad;
    const int editHeight = g.rowHeight + 6, hintHeight = g.rowHeight;
    MoveWindow(g.edit, pad, pad, width, editHeight, TRUE);
    const int listTop = pad + editHeight + pad;
    MoveWindow(g.list, pad, listTop, width, client.bottom - listTop - hintHeight - pad, TRUE);
    MoveWindow(g.hint, pad, client.bottom - hintHeight - pad / 2, width, hintHeight, TRUE);
}

LRESULT CALLBACK PaletteProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    switch (message)
    {
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT*>(l)->itemHeight = g.rowHeight;
        return TRUE;
    case WM_DRAWITEM:
        if (w == kList) { DrawRow(*reinterpret_cast<DRAWITEMSTRUCT*>(l)); return TRUE; }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == kEdit && HIWORD(w) == EN_CHANGE) { Filter(); return 0; }
        break;
    case WM_ACTIVATE:
        // A popup: it goes as soon as anything else is clicked.
        if (LOWORD(w) == WA_INACTIVE) PostMessageA(window, WM_CLOSE, 0, 0);
        else SetFocus(g.edit);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_SIZE:
        Layout(window);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(w), GetSysColor(COLOR_GRAYTEXT));
        SetBkColor(reinterpret_cast<HDC>(w), GetSysColor(COLOR_BTNFACE));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    case WM_DESTROY:
        if (g.font) DeleteObject(g.font);
        if (g.marks) DeleteObject(g.marks);
        g.font = g.marks = nullptr;
        g.window = g.edit = g.list = g.hint = nullptr;
        g.entries.clear();
        g.shown.clear();
        return 0;
    }
    return DefWindowProcA(window, message, w, l);
}

void RegisterOnce()
{
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXA type{ sizeof(type) };
    type.style = CS_DROPSHADOW;
    type.lpfnWndProc = PaletteProc;
    type.hInstance = GetModuleHandleA(nullptr);
    type.hCursor = LoadCursor(nullptr, IDC_ARROW);
    type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    type.lpszClassName = kClass;
    if (!RegisterClassExA(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Could not register the Command Palette window.");
    registered = true;
}

HWND FrameWindow()
{
    if (g.frame && IsWindow(g.frame)) return g.frame;
    return nullptr;
}

// Whether a key press may open the palette now: the editor is in front, no
// modal box has the frame disabled and no menu is open.
bool CanOpenFromKey()
{
    const HWND frame = FrameWindow();
    if (!frame || !IsWindowEnabled(frame)) return false;
    GUITHREADINFO info{ sizeof(info) };
    if (GetGUIThreadInfo(GetCurrentThreadId(), &info) && (info.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE)))
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

void CALLBACK OpenFromKey(HWND, UINT, UINT_PTR timer, DWORD)
{
    KillTimer(nullptr, timer);
    try
    {
        if (IsWindow(g.window)) Close(true);
        else Open();
    }
    catch (const std::exception& e) { Logger::log(std::string("Command Palette: ") + e.what()); }
}

// Ctrl+Shift+P on the frame's thread, whichever of its windows has the keys:
// taken as the message leaves the queue, before the window (and the stock
// accelerators) see it. The P's key-up goes with it.
LRESULT CALLBACK KeyProc(int code, WPARAM removal, LPARAM parameter)
{
    auto* message = reinterpret_cast<MSG*>(parameter);
    if (code == HC_ACTION && removal == PM_REMOVE && message && message->wParam == 'P'
        && (message->message == WM_KEYDOWN || message->message == WM_KEYUP))
    {
        const bool release = message->message == WM_KEYUP, repeat = (message->lParam & 0x40000000) != 0;
        if (release && swallowP) { swallowP = false; message->message = WM_NULL; }
        else if (!release && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0 && GetKeyState(VK_MENU) >= 0
                 && (swallowP || CanOpenFromKey()))
        {
            swallowP = true;
            message->message = WM_NULL;
            // Opened from a timer, outside the hook.
            if (!repeat) SetTimer(nullptr, 0, 0, OpenFromKey);
        }
    }
    return CallNextHookEx(keyHook, code, removal, parameter);
}
} // namespace

void Attach(HWND frame)
{
    g.frame = frame;
    if (keyHook) return;
    keyHook = SetWindowsHookExA(WH_GETMESSAGE, KeyProc, nullptr, GetCurrentThreadId());
    Logger::log(keyHook ? "Command Palette: Ctrl+Shift+P installed" : "Command Palette: the Ctrl+Shift+P hook failed");
}

void Open()
{
    const HWND frame = FrameWindow();
    if (!frame) throw std::runtime_error("The editor window is not ready yet.");
    if (IsWindow(g.window))
    {
        SetForegroundWindow(g.window);
        SetFocus(g.edit);
        return;
    }
    RegisterOnce();
    g.previousFocus = GetFocus();
    g.entries = Commands();
    g.recent = LoadRecent();

    NONCLIENTMETRICSA metrics{ sizeof(metrics) };
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    LOGFONTA look = metrics.lfMessageFont;
    g.font = CreateFontIndirectA(&look);
    LOGFONTA marlett{};
    marlett.lfHeight = look.lfHeight;
    marlett.lfCharSet = SYMBOL_CHARSET;
    strcpy_s(marlett.lfFaceName, "Marlett");
    g.marks = CreateFontIndirectA(&marlett);
    {
        HDC dc = GetDC(frame);
        HGDIOBJ old = SelectObject(dc, g.font);
        TEXTMETRICA tm{};
        GetTextMetricsA(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(frame, dc);
        g.rowHeight = std::max(16, static_cast<int>(tm.tmHeight + 6));
    }

    // Near the top of the editor, across the middle half of it.
    RECT area{};
    GetWindowRect(frame, &area);
    const int frameWidth = area.right - area.left;
    const int width = std::clamp(frameWidth / 2, 480, 820);
    const int height = 6 + (g.rowHeight + 6) + 6 + kVisibleRows * g.rowHeight + 4 + g.rowHeight + 6;
    const int x = area.left + (frameWidth - width) / 2;
    const int y = area.top + std::min(90, std::max(0, static_cast<int>(area.bottom - area.top) / 8));

    g.window = CreateWindowExA(WS_EX_TOOLWINDOW, kClass, "Command Palette", WS_POPUP | WS_BORDER | WS_CLIPCHILDREN,
                               x, y, width, height, frame, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!g.window) throw std::runtime_error("Could not open the Command Palette.");
    g.edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
                             g.window, reinterpret_cast<HMENU>(kEdit), nullptr, nullptr);
    g.list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                             0, 0, 0, 0, g.window, reinterpret_cast<HMENU>(kList), nullptr, nullptr);
    g.hint = CreateWindowExA(0, "STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE, 0, 0, 0, 0,
                             g.window, reinterpret_cast<HMENU>(kHint), nullptr, nullptr);
    for (HWND child : { g.edit, g.list, g.hint }) SendMessageA(child, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), FALSE);
    SendMessageA(g.list, LB_SETITEMHEIGHT, 0, g.rowHeight);
    SendMessageA(g.edit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Type part of a menu command, e.g. map check"));
    g.editProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
    g.listProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g.list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ListProc)));
    Layout(g.window);
    Filter();
    ShowWindow(g.window, SW_SHOW);
    SetForegroundWindow(g.window);
    SetFocus(g.edit);
}

bool HandleCommand(UINT command)
{
    if (command != kOpen) return false;
    try { Open(); }
    catch (const std::exception& e) { MessageBoxA(GetActiveWindow(), e.what(), "Command Palette", MB_OK | MB_ICONINFORMATION); }
    return true;
}
}
