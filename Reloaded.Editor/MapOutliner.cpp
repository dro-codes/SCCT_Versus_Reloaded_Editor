#include "pch.h"
#undef min
#undef max
#include "MapOutliner.h"
#include "MapOutlinerModel.h"
#include "BudgetWindow.h"
#include "WindowPlacement.h"
#include "WorkflowEditor.h"
#include "logger.h"
#include <commctrl.h>
#include <chrono>
#include <stdexcept>
#include <unordered_set>
#pragma comment(lib, "comctl32.lib")

namespace MapOutliner
{
namespace
{
namespace Editor = Workflow::Editor;
using Outliner::Column;
using Outliner::GroupBy;

const char kTitle[] = "Map Outliner";
const char kSection[] = "MapOutliner";
const char kPlacementKey[] = "MapOutliner";

enum : int
{
    kSearchLabel = 100, kSearch, kGroupLabel, kGroupBy, kFollow, kList, kStatus,
    kHide = 110, kShow, kShowAll, kDelete, kFrame, kRefresh
};
constexpr UINT_PTR kTimer = 1;
constexpr UINT kTimerMs = 300;
constexpr UINT kApplySelection = WM_APP + 1;

struct State
{
    HWND window = nullptr, list = nullptr;
    HFONT bold = nullptr;
    std::vector<Outliner::Row> rows;
    std::vector<std::string> texts;
    Outliner::Options options;
    Outliner::View view;
    Editor::OutlinerKey key;
    bool known = false;      // rows were read from the open map
    bool follow = true;
    bool syncing = false;    // list selection being set by the window itself
    bool pending = false;    // a selection change waits to reach the editor
    bool asking = false;     // the Delete question is up: the timer leaves the rows alone
    std::string action;      // the last thing done, above the summary
} s;

void SetStatus()
{
    if (!IsWindow(s.window)) return;
    std::string text;
    if (s.known)
    {
        size_t selected = 0;
        for (const auto& row : s.rows) selected += row.selected;
        text = Outliner::Summary(s.rows, s.view, selected);
    }
    if (!s.action.empty()) text = s.action + (text.empty() ? "" : "\r\n" + text);
    SetDlgItemTextA(s.window, kStatus, text.c_str());
}
void Say(const std::string& text)
{
    s.action = text;
    SetStatus();
}

std::vector<int> SelectedListEntries()
{
    std::vector<int> entries;
    if (!s.list) return entries;
    for (int i = ListView_GetNextItem(s.list, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(s.list, i, LVNI_SELECTED))
        entries.push_back(i);
    return entries;
}
std::vector<int> ChosenRows() { return Outliner::ChosenRows(s.view, SelectedListEntries()); }

void SetListSelection(const std::vector<int>& entries, bool scroll)
{
    s.syncing = true;
    ListView_SetItemState(s.list, -1, 0, LVIS_SELECTED);
    for (int e : entries) ListView_SetItemState(s.list, e, LVIS_SELECTED, LVIS_SELECTED);
    if (scroll && !entries.empty())
    {
        ListView_SetItemState(s.list, entries.front(), LVIS_FOCUSED, LVIS_FOCUSED);
        ListView_EnsureVisible(s.list, entries.front(), FALSE);
    }
    s.syncing = false;
}

// Follow Selection: the list shows what the editor has selected.
void FollowEditor(bool scroll)
{
    if (s.follow && s.list) SetListSelection(Outliner::SelectedEntries(s.view, s.rows), scroll);
}

// Lays the rows out again (search, grouping, sort) without reading the map.
void Rebuild()
{
    if (!s.list) return;
    // Without Follow Selection the list keeps its own selection, by actor.
    std::unordered_set<uintptr_t> kept;
    std::vector<std::string> keptHeaders;
    if (!s.follow)
        for (int e : SelectedListEntries())
            if (e < static_cast<int>(s.view.entries.size()))
            {
                const auto& entry = s.view.entries[e];
                if (entry.Header()) keptHeaders.push_back(s.view.groups[entry.group].key);
                else if (entry.row < static_cast<int>(s.rows.size())) kept.insert(s.rows[entry.row].handle);
            }
    s.view = Outliner::Build(s.rows, s.texts, s.options);
    s.syncing = true;
    ListView_SetItemState(s.list, -1, 0, LVIS_SELECTED);
    ListView_SetItemCountEx(s.list, static_cast<int>(s.view.entries.size()), LVSICF_NOSCROLL);
    s.syncing = false;
    if (s.follow) FollowEditor(false);
    else if (!kept.empty() || !keptHeaders.empty())
    {
        std::vector<int> again;
        for (int e = 0; e < static_cast<int>(s.view.entries.size()); ++e)
        {
            const auto& entry = s.view.entries[e];
            if (entry.Header() ? std::find(keptHeaders.begin(), keptHeaders.end(), s.view.groups[entry.group].key) != keptHeaders.end()
                               : kept.count(s.rows[entry.row].handle) != 0)
                again.push_back(e);
        }
        SetListSelection(again, false);
    }
    InvalidateRect(s.list, nullptr, FALSE);
    SetStatus();
}

// Reads the map again: every actor in one pass.
void Read()
{
    const auto start = std::chrono::steady_clock::now();
    Editor::OutlinerRows(s.rows);
    s.key = Editor::OutlinerChangeKey();
    s.known = true;
    s.texts.clear();
    s.texts.reserve(s.rows.size());
    for (const auto& row : s.rows) s.texts.push_back(Outliner::SearchText(row));
    Rebuild();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (ms > 100) Logger::log("Map Outliner: read " + std::to_string(s.rows.size()) + " actors in " + std::to_string(ms) + " ms");
}

void Forget(const std::string& why)
{
    s.rows.clear();
    s.texts.clear();
    s.known = false;
    Rebuild();
    Say(why);
}

// The timer: the cheap key first; the map is read again only when it moved.
// With the key unchanged only the hidden and selected bits are looked at.
void Tick()
{
    if (s.asking) return;
    Editor::OutlinerKey key;
    try { key = Editor::OutlinerChangeKey(); }
    catch (const std::exception& e)
    {
        if (s.known) Forget(e.what());
        return;
    }
    if (!s.known || !(key == s.key))
    {
        // An edit that also changed the selection brings the selection into view.
        std::unordered_set<uintptr_t> before;
        for (const auto& row : s.rows) if (row.selected) before.insert(row.handle);
        Read();
        size_t same = 0, now = 0;
        for (const auto& row : s.rows) if (row.selected) { ++now; same += before.count(row.handle); }
        if (now && (now != before.size() || same != now)) FollowEditor(true);
        return;
    }
    const unsigned changed = Editor::OutlinerFlags(s.rows);
    if (!changed) return;
    if (changed & 2)
    {
        for (size_t i = 0; i < s.rows.size(); ++i) s.texts[i] = Outliner::SearchText(s.rows[i]);
        Rebuild();
    }
    else
    {
        FollowEditor(true);
        InvalidateRect(s.list, nullptr, FALSE);
        SetStatus();
    }
}

// Before acting on rows: they must be the open map's.
bool SameMap()
{
    try
    {
        if (!s.known || Editor::MapGeneration() != s.key.generation)
        {
            Read();
            Say("Another map was opened, so the list was read again. Choose the rows again.");
            return false;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        Forget(e.what());
        return false;
    }
}

// The rows chosen in the list, as the map is now: after an edit the timer
// has not caught up with yet, the map is read again and the same actors are
// looked up, leaving out any that went.
std::vector<int> CurrentChosen()
{
    auto chosen = ChosenRows();
    if (!SameMap()) return {};
    if (Editor::OutlinerChangeKey() == s.key) return chosen;
    std::unordered_set<uintptr_t> handles;
    for (int r : chosen) handles.insert(s.rows[r].handle);
    Read();
    std::vector<int> again;
    for (int r = 0; r < static_cast<int>(s.rows.size()); ++r) if (handles.count(s.rows[r].handle)) again.push_back(r);
    return again;
}

// The list's selection reaches the editor; focus frames it in the viewports.
void ApplySelection(bool focus)
{
    s.pending = false;
    if (!s.known) return;
    const auto choice = Outliner::Selectable(s.rows, CurrentChosen());
    std::vector<uintptr_t> actors;
    for (int r : choice.rows) actors.push_back(s.rows[r].handle);
    const size_t found = Editor::OutlinerSelect(actors, focus && !actors.empty());
    const std::unordered_set<int> on(choice.rows.begin(), choice.rows.end());
    for (int r = 0; r < static_cast<int>(s.rows.size()); ++r) s.rows[r].selected = on.count(r) != 0;
    std::string text = "Selected " + Outliner::Thousands(found) + " actor(s)" + (focus && found ? ", framed in the viewports." : ".");
    if (choice.hidden) text += " " + Outliner::Thousands(choice.hidden) + " hidden one(s) left out: Show them first.";
    InvalidateRect(s.list, nullptr, FALSE);
    Say(text);
}

Workflow::Json Identities(const std::vector<int>& rows)
{
    Workflow::Json members = Workflow::Json::array();
    for (int r : rows) members.push_back({{"path", s.rows[r].path}, {"class", s.rows[r].cls}});
    return members;
}

// Hide and Show go through the flags RE+: Visibility sets (one Undo step;
// hidden actors leave the selection).
void SetHidden(bool hidden, bool all)
{
    std::vector<int> rows = CurrentChosen();
    if (!s.known) return;
    if (all)
    {
        rows.clear();
        for (int r = 0; r < static_cast<int>(s.rows.size()); ++r) if (s.rows[r].hidden) rows.push_back(r);
    }
    if (rows.empty()) { Say(all ? "Nothing is hidden." : "Click the rows to " + std::string(hidden ? "hide" : "show") + " first."); return; }
    Editor::DesignSetFlags(Identities(rows), hidden ? 1 : 0, -1);
    Tick();
    Say(std::string(hidden ? "Hid " : "Showed ") + Outliner::Thousands(rows.size()) + " actor(s). Undo puts them back.");
}

void DeleteChosen()
{
    const auto chosen = CurrentChosen();
    if (!s.known) return;
    const auto plan = Outliner::PlanDelete(s.rows, chosen);
    if (plan.rows.empty()) { Say(plan.kept ? "The level info and the builder brush cannot be deleted." : "Click the rows to delete first."); return; }
    std::vector<uintptr_t> actors;
    for (int r : plan.rows) actors.push_back(s.rows[r].handle);
    const auto generation = s.key.generation;
    s.asking = true;
    const int answer = MessageBoxA(s.window, Outliner::DeleteQuestion(s.rows, plan).c_str(), kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    s.asking = false;
    if (answer != IDYES)
    {
        Say("Nothing was deleted.");
        return;
    }
    if (Editor::MapGeneration() != generation)
    {
        Read();
        Say("Another map was opened while asking, so nothing was deleted.");
        return;
    }
    const size_t deleted = Editor::OutlinerDelete(actors);
    Read();
    Say("Deleted " + Outliner::Thousands(deleted) + " actor(s) in one Undo step. Rebuild geometry if brushes went.");
}

void SaveSettings()
{
    const auto ini = Budget::IniPath();
    WritePrivateProfileStringA(kSection, "GroupBy", std::to_string(static_cast<int>(s.options.groupBy)).c_str(), ini.c_str());
    WritePrivateProfileStringA(kSection, "FollowSelection", s.follow ? "1" : "0", ini.c_str());
}

void ToggleGroup(int entry)
{
    s.options.collapsed = Outliner::Toggle(s.view, entry, s.options.collapsed);
    const std::string key = entry >= 0 && entry < static_cast<int>(s.view.entries.size()) && s.view.entries[entry].Header()
        ? s.view.groups[s.view.entries[entry].group].key : std::string();
    Rebuild();
    // Keep the header where the user is.
    for (int e = 0; e < static_cast<int>(s.view.entries.size()); ++e)
        if (s.view.entries[e].Header() && s.view.groups[s.view.entries[e].group].key == key)
        {
            s.syncing = true;
            ListView_SetItemState(s.list, e, LVIS_FOCUSED, LVIS_FOCUSED);
            ListView_EnsureVisible(s.list, e, FALSE);
            s.syncing = false;
            break;
        }
}

void Layout(HWND window)
{
    RECT r{};
    GetClientRect(window, &r);
    const int w = r.right - 24, h = r.bottom;
    MoveWindow(GetDlgItem(window, kSearchLabel), 12, 13, 46, 18, TRUE);
    MoveWindow(GetDlgItem(window, kSearch), 60, 10, std::max(80, w - 48 - 330), 22, TRUE);
    MoveWindow(GetDlgItem(window, kGroupLabel), r.right - 330, 13, 56, 18, TRUE);
    MoveWindow(GetDlgItem(window, kGroupBy), r.right - 272, 10, 110, 200, TRUE);
    MoveWindow(GetDlgItem(window, kFollow), r.right - 152, 10, 140, 22, TRUE);
    MoveWindow(GetDlgItem(window, kList), 12, 40, w, std::max(40, h - 40 - 84), TRUE);
    MoveWindow(GetDlgItem(window, kStatus), 12, h - 78, w, 34, TRUE);
    int x = 12;
    for (auto [id, width] : std::initializer_list<std::pair<int, int>>{{kHide, 64}, {kShow, 64}, {kShowAll, 76}, {kDelete, 72}, {kFrame, 64}, {kRefresh, 70}})
    {
        MoveWindow(GetDlgItem(window, id), x, h - 36, width, 26, TRUE);
        x += width + 8;
    }
    MoveWindow(GetDlgItem(window, IDCANCEL), std::max(x, static_cast<int>(r.right) - 92), h - 36, 80, 26, TRUE); // never over Refresh
}

// Esc clears the search; Down goes to the list.
LRESULT CALLBACK SearchProc(HWND edit, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
{
    if (message == WM_KEYDOWN && w == VK_ESCAPE) { SetWindowTextA(edit, ""); return 0; }
    if (message == WM_KEYDOWN && w == VK_DOWN && s.list) { SetFocus(s.list); return 0; }
    if (message == WM_CHAR && (w == VK_ESCAPE || w == VK_RETURN)) return 0;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(edit, SearchProc, 1);
    return DefSubclassProc(edit, message, w, l);
}

LRESULT ListNotify(NMHDR* header)
{
    switch (header->code)
    {
    case LVN_GETDISPINFOA:
    {
        auto info = reinterpret_cast<NMLVDISPINFOA*>(header);
        auto& item = info->item;
        if (!(item.mask & LVIF_TEXT) || !item.pszText || item.cchTextMax <= 0) return 0;
        item.pszText[0] = 0;
        if (item.iItem < 0 || item.iItem >= static_cast<int>(s.view.entries.size())) return 0;
        const auto& entry = s.view.entries[item.iItem];
        std::string text;
        if (entry.Header()) { if (item.iSubItem == 0) text = Outliner::HeaderText(s.view.groups[entry.group]); }
        else if (entry.row < static_cast<int>(s.rows.size()))
            text = Outliner::Cell(s.rows[entry.row], static_cast<Column>(item.iSubItem));
        if (entry.group >= 0 && !entry.Header() && item.iSubItem == 0) text = "      " + text;
        lstrcpynA(item.pszText, text.c_str(), item.cchTextMax);
        return 0;
    }
    case NM_CUSTOMDRAW:
    {
        auto draw = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
        {
            const int i = static_cast<int>(draw->nmcd.dwItemSpec);
            if (i < 0 || i >= static_cast<int>(s.view.entries.size())) return CDRF_DODEFAULT;
            const auto& entry = s.view.entries[i];
            LRESULT result = CDRF_DODEFAULT;
            // Selected rows stay plainly highlighted while the viewports have
            // the focus, so Follow Selection can be seen working.
            if (ListView_GetItemState(s.list, i, LVIS_SELECTED) & LVIS_SELECTED)
            {
                draw->nmcd.uItemState &= ~CDIS_SELECTED;
                draw->clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
                draw->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
                result = CDRF_NEWFONT;
            }
            else if (!entry.Header() && entry.row < static_cast<int>(s.rows.size()) && s.rows[entry.row].hidden)
            {
                draw->clrText = GetSysColor(COLOR_GRAYTEXT);
                result = CDRF_NEWFONT;
            }
            if (entry.Header() && s.bold)
            {
                SelectObject(draw->nmcd.hdc, s.bold);
                result = CDRF_NEWFONT;
            }
            return result;
        }
        return CDRF_DODEFAULT;
    }
    case LVN_ITEMCHANGED:
    {
        auto change = reinterpret_cast<NMLISTVIEW*>(header);
        if (!s.syncing && (change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED) && !s.pending)
        {
            s.pending = true;
            PostMessageA(s.window, kApplySelection, 0, 0);
        }
        return 0;
    }
    case LVN_ODSTATECHANGED:
    {
        auto change = reinterpret_cast<NMLVODSTATECHANGE*>(header);
        if (!s.syncing && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED) && !s.pending)
        {
            s.pending = true;
            PostMessageA(s.window, kApplySelection, 0, 0);
        }
        return 0;
    }
    case NM_DBLCLK:
    case NM_RETURN:
    {
        int item = header->code == NM_DBLCLK ? reinterpret_cast<NMITEMACTIVATE*>(header)->iItem : ListView_GetNextItem(s.list, -1, LVNI_FOCUSED);
        if (item < 0 || item >= static_cast<int>(s.view.entries.size())) return 0;
        if (s.view.entries[item].Header()) { ToggleGroup(item); return 0; }
        if (!(ListView_GetItemState(s.list, item, LVIS_SELECTED) & LVIS_SELECTED)) SetListSelection({item}, false);
        ApplySelection(true);
        return 0;
    }
    case LVN_KEYDOWN:
    {
        auto key = reinterpret_cast<NMLVKEYDOWN*>(header);
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const int focused = ListView_GetNextItem(s.list, -1, LVNI_FOCUSED);
        const bool onHeader = focused >= 0 && focused < static_cast<int>(s.view.entries.size()) && s.view.entries[focused].Header();
        if (key->wVKey == VK_DELETE) DeleteChosen();
        else if (key->wVKey == 'A' && ctrl) { ListView_SetItemState(s.list, -1, LVIS_SELECTED, LVIS_SELECTED); }
        else if (key->wVKey == 'F' && !ctrl) ApplySelection(true);
        else if (key->wVKey == 'H' && !ctrl) SetHidden(true, false);
        else if (key->wVKey == VK_LEFT && onHeader && !s.view.groups[s.view.entries[focused].group].collapsed) ToggleGroup(focused);
        else if (key->wVKey == VK_RIGHT && onHeader && s.view.groups[s.view.entries[focused].group].collapsed) ToggleGroup(focused);
        return 0;
    }
    case LVN_COLUMNCLICK:
    {
        const auto column = static_cast<Column>(reinterpret_cast<NMLISTVIEW*>(header)->iSubItem);
        s.options.descending = s.options.sortBy == column ? !s.options.descending : false;
        s.options.sortBy = column;
        Rebuild();
        Say(std::string("Sorted by ") + Outliner::ColumnName(column) + (s.options.descending ? ", descending." : "."));
        return 0;
    }
    }
    return 0;
}

LRESULT CALLBACK OutlinerProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            s.window = window;
            Budget::Control(window, "STATIC", "&Search:", 0, kSearchLabel, 0, 0, 0, 0);
            HWND search = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, window,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearch)), GetModuleHandle(nullptr), nullptr);
            SendMessage(search, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SendMessageA(search, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Name, class, group, Tag, Event... (several words narrow it)"));
            SetWindowSubclass(search, SearchProc, 1, 0);
            Budget::Control(window, "STATIC", "&Group by:", 0, kGroupLabel, 0, 0, 0, 0);
            auto groups = Budget::Control(window, "COMBOBOX", "", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, kGroupBy, 0, 0, 0, 0);
            for (const char* name : {"Nothing", "Class", "Group"}) SendMessageA(groups, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            SendMessageA(groups, CB_SETCURSEL, static_cast<int>(s.options.groupBy), 0);
            auto follow = Budget::Control(window, "BUTTON", "&Follow selection", WS_TABSTOP | BS_AUTOCHECKBOX, kFollow, 0, 0, 0, 0);
            SendMessageA(follow, BM_SETCHECK, s.follow ? BST_CHECKED : BST_UNCHECKED, 0);
            s.list = Budget::Control(window, WC_LISTVIEWA, "", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS, kList, 0, 0, 0, 0);
            ListView_SetExtendedListViewStyle(s.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
            Budget::Columns(s.list, {{"Name", 200}, {"Class", 140}, {"Group", 90}, {"Tag", 100}, {"Event", 100}, {"Location", 130}, {"Hidden", 52}, {"Locked", 52}},
                            {"Class", "Group", "Tag", "Event", "Location", "Hidden", "Locked"});
            LOGFONTA font{};
            GetObjectA(GetStockObject(DEFAULT_GUI_FONT), sizeof(font), &font);
            font.lfWeight = FW_BOLD;
            s.bold = CreateFontIndirectA(&font);
            Budget::Control(window, "STATIC", "", 0, kStatus, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "&Hide", WS_TABSTOP, kHide, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "Sho&w", WS_TABSTOP, kShow, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "Show &All", WS_TABSTOP, kShowAll, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "&Delete...", WS_TABSTOP, kDelete, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "F&rame", WS_TABSTOP, kFrame, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "Re&fresh", WS_TABSTOP, kRefresh, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "Close", WS_TABSTOP, IDCANCEL, 0, 0, 0, 0);
            Layout(window);
            SetTimer(window, kTimer, kTimerMs, nullptr);
            return 0;
        }
        case WM_SIZE:
            Layout(window);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {590, 320}; // the buttons in one row, Close clear of Refresh
            return 0;
        case WM_TIMER:
            if (w == kTimer) Tick();
            return 0;
        case kApplySelection:
            if (s.pending) ApplySelection(false);
            return 0;
        case WM_NOTIFY:
        {
            auto header = reinterpret_cast<NMHDR*>(l);
            if (header->idFrom == kList) return ListNotify(header);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case kSearch:
                if (HIWORD(w) == EN_CHANGE)
                {
                    char text[512] = {};
                    GetDlgItemTextA(window, kSearch, text, sizeof(text));
                    s.options.words = Outliner::SearchWords(text);
                    Rebuild();
                }
                return 0;
            case kGroupBy:
                if (HIWORD(w) == CBN_SELCHANGE)
                {
                    const auto chosen = SendDlgItemMessageA(window, kGroupBy, CB_GETCURSEL, 0, 0);
                    s.options.groupBy = chosen == 1 ? GroupBy::Class : chosen == 2 ? GroupBy::Group : GroupBy::None;
                    s.options.collapsed.clear();
                    SaveSettings();
                    Rebuild();
                }
                return 0;
            case kFollow:
                s.follow = SendDlgItemMessageA(window, kFollow, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SaveSettings();
                FollowEditor(true);
                return 0;
            case kHide: SetHidden(true, false); return 0;
            case kShow: SetHidden(false, false); return 0;
            case kShowAll: SetHidden(false, true); return 0;
            case kDelete: DeleteChosen(); return 0;
            case kFrame: ApplySelection(true); return 0;
            case kRefresh:
                Read();
                Say("Read the map again.");
                return 0;
            case IDCANCEL: DestroyWindow(window); return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kTimer);
            WindowPlacement::Remember(window, kPlacementKey);
            if (s.bold) { DeleteObject(s.bold); s.bold = nullptr; }
            s.window = nullptr;
            s.list = nullptr;
            s.rows.clear();
            s.texts.clear();
            s.view = {};
            s.known = false;
            s.pending = false;
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        // A modal box here would stall the in-editor tests; report in the status line.
        Say(e.what());
        if (message == WM_CREATE) return -1;
        return 0;
    }
    return DefWindowProcA(window, message, w, l);
}
} // namespace

void Open(HWND owner)
{
    if (IsWindow(s.window))
    {
        ShowWindow(s.window, SW_RESTORE);
        SetForegroundWindow(s.window);
        Tick();
        return;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const int group = Budget::IniInt(kSection, "GroupBy", 1);
    s.options = {};
    s.options.groupBy = group == 0 ? GroupBy::None : group == 2 ? GroupBy::Group : GroupBy::Class;
    s.follow = Budget::IniInt(kSection, "FollowSelection", 1) != 0;
    s.action.clear();
    Budget::RegisterClassOnce("ReloadedMapOutliner", OutlinerProc);
    HWND window = CreateWindowExA(WS_EX_TOOLWINDOW, "ReloadedMapOutliner", kTitle, WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 900, 620, owner, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!window) throw std::runtime_error("Could not open the Map Outliner.");
    WindowPlacement::Restore(window, kPlacementKey);
    Tick();
    if (!s.known && s.action.empty()) Say("Open a map to list its actors.");
    ShowWindow(window, SW_SHOW);
    SetFocus(GetDlgItem(window, kSearch));
}

bool HandleCommand(UINT command)
{
    if (command != kOpenCommand) return false;
    try { Open(Editor::MainWindow() ? Editor::MainWindow() : GetActiveWindow()); }
    catch (const std::exception& e) { MessageBoxA(GetActiveWindow(), e.what(), kTitle, MB_OK | MB_ICONINFORMATION); }
    return true;
}
}
