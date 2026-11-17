#include "pch.h"
#undef min
#undef max
#include "MapUsagesWindow.h"
#include "WindowPlacement.h"
#include "MapUsages.h"
#include "EditorExtras.h"
#include "WorkflowEditor.h"
#include <commctrl.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace MapUsagesWindow
{
namespace
{
    constexpr int kObject = 300, kStop = 301, kGameMaps = 302, kAutosaves = 303, kNote = 304, kList = 305, kOpen = 306, kCopy = 307, kStatus = 308, kLabel = 309;
    constexpr UINT_PTR kTimer = 1;

    // What the scan thread shares with the window. The window polls it on a
    // timer, so nothing is posted to a window that may already be gone.
    struct Job
    {
        std::mutex lock;
        std::atomic<bool> cancel{ false };
        size_t index = 0, total = 0;
        std::string current, error, query;
        std::vector<MapUsages::Row> rows;
        double seconds = 0;
        bool done = false;
    };
    struct State
    {
        HWND window{}, list{};
        std::shared_ptr<Job> job;
        std::thread worker;
        std::vector<MapUsages::Row> rows;
        std::string query;
        size_t scanned = 0;
    };
    HWND openWindow = nullptr;

    HWND Control(HWND parent, const char* type, const char* title, DWORD style, int id)
    {
        HWND child = CreateWindowExA(std::string(type) == "EDIT" ? WS_EX_CLIENTEDGE : 0, type, title, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10, parent,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
        SendMessage(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return child;
    }
    std::string Text(HWND control)
    {
        const int count = GetWindowTextLengthA(control);
        std::string text(count + 1, '\0');
        GetWindowTextA(control, text.data(), count + 1);
        text.resize(count);
        return text;
    }
    void Status(State& s, const std::string& text) { SetWindowTextA(GetDlgItem(s.window, kStatus), text.c_str()); }
    bool Checked(State& s, int id) { return SendDlgItemMessage(s.window, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }
    bool Busy(const State& s) { return s.worker.joinable(); }
    std::filesystem::path Packages() { return Workflow::Editor::Directory().parent_path().parent_path() / "Packages"; }

    void Layout(State& s)
    {
        RECT r{};
        GetClientRect(s.window, &r);
        const int width = r.right;
        MoveWindow(GetDlgItem(s.window, kLabel), 12, 15, 50, 20, TRUE);
        MoveWindow(GetDlgItem(s.window, kObject), 64, 12, std::max(120, width - 64 - 12 - 2 * 96), 24, TRUE);
        MoveWindow(GetDlgItem(s.window, IDOK), width - 12 - 2 * 96 + 6, 11, 90, 26, TRUE);
        MoveWindow(GetDlgItem(s.window, kStop), width - 12 - 90, 11, 90, 26, TRUE);
        MoveWindow(GetDlgItem(s.window, kGameMaps), 12, 44, 330, 22, TRUE);
        MoveWindow(GetDlgItem(s.window, kAutosaves), 350, 44, 260, 22, TRUE);
        MoveWindow(GetDlgItem(s.window, kNote), 12, 70, width - 24, 46, TRUE);
        MoveWindow(s.list, 12, 120, width - 24, std::max(40, static_cast<int>(r.bottom) - 120 - 86), TRUE);
        MoveWindow(GetDlgItem(s.window, kStatus), 12, r.bottom - 80, width - 24, 40, TRUE);
        MoveWindow(GetDlgItem(s.window, kOpen), 12, r.bottom - 36, 130, 26, TRUE);
        MoveWindow(GetDlgItem(s.window, kCopy), 150, r.bottom - 36, 130, 26, TRUE);
        const int rest = std::max(100, width - 24 - 170 - 80 - 90 - 24);
        ListView_SetColumnWidth(s.list, 0, 170);
        ListView_SetColumnWidth(s.list, 1, 80);
        ListView_SetColumnWidth(s.list, 2, 90);
        ListView_SetColumnWidth(s.list, 3, rest);
    }
    void Buttons(State& s)
    {
        const bool busy = Busy(s);
        EnableWindow(GetDlgItem(s.window, IDOK), !busy);
        EnableWindow(GetDlgItem(s.window, kStop), busy);
        EnableWindow(GetDlgItem(s.window, kObject), !busy);
        EnableWindow(GetDlgItem(s.window, kGameMaps), !busy);
        EnableWindow(GetDlgItem(s.window, kAutosaves), !busy);
        EnableWindow(GetDlgItem(s.window, kCopy), !busy && !s.rows.empty());
        EnableWindow(GetDlgItem(s.window, kOpen), !busy && !s.rows.empty());
    }
    void Populate(State& s)
    {
        ListView_DeleteAllItems(s.list);
        int index = 0;
        for (const auto& row : s.rows)
        {
            std::string values[4] = { row.file, row.folder, row.error.empty() ? std::to_string(row.result.references) : "-",
                                      row.error.empty() ? MapUsages::Summary(row.result) : "Could not read: " + row.error };
            LVITEMA item{};
            item.mask = LVIF_TEXT;
            item.iItem = index;
            item.pszText = values[0].data();
            SendMessageA(s.list, LVM_INSERTITEMA, 0, reinterpret_cast<LPARAM>(&item));
            for (int column = 1; column < 4; ++column)
            {
                item.iSubItem = column;
                item.pszText = values[column].data();
                SendMessageA(s.list, LVM_SETITEMTEXTA, index, reinterpret_cast<LPARAM>(&item));
            }
            ++index;
        }
    }
    void Finish(State& s)
    {
        if (s.worker.joinable()) s.worker.join();
        const auto job = std::move(s.job); // The thread is gone: nothing else holds it.
        s.rows = std::move(job->rows);
        // A stopped search finishes the map it was on, then stops.
        s.scanned = job->cancel && job->total ? std::min(job->index + 1, job->total) : job->total;
        Populate(s);
        size_t found = 0, unreadable = 0;
        for (const auto& row : s.rows) (row.error.empty() ? found : unreadable) += 1;
        char seconds[32];
        snprintf(seconds, sizeof(seconds), "%.1f", job->seconds);
        std::string text;
        if (!job->error.empty()) text = job->error;
        else
        {
            text = job->cancel ? "Stopped. " : "";
            text += job->query + " is used by " + std::to_string(found) + " of " + std::to_string(s.scanned) + " maps (" + seconds + " s).";
            if (unreadable) text += " " + std::to_string(unreadable) + " file(s) could not be read; they are listed last.";
            if (found) text += " Double-click a map to open it.";
        }
        Status(s, text);
        Buttons(s);
    }
    void Start(State& s)
    {
        if (Busy(s)) return;
        const auto query = Text(GetDlgItem(s.window, kObject));
        std::vector<std::string> parts;
        try { parts = MapUsages::SplitPath(query); }
        catch (const std::exception& e) { Status(s, e.what()); return; }
        std::vector<MapUsages::Folder> folders = { { Packages() / "MapsEd", "MapsEd" } };
        if (Checked(s, kGameMaps)) folders.push_back({ Packages() / "Maps", "Maps" });
        const bool autosaves = Checked(s, kAutosaves);
        s.rows.clear();
        ListView_DeleteAllItems(s.list);
        s.query = MapUsages::Join(parts);
        auto job = std::make_shared<Job>();
        job->query = s.query;
        s.job = job;
        Status(s, "Listing maps...");
        s.worker = std::thread([job, folders, autosaves, parts]
        {
            const auto start = std::chrono::steady_clock::now();
            try
            {
                const auto files = MapUsages::MapFiles(folders, autosaves);
                { std::lock_guard<std::mutex> hold(job->lock); job->total = files.size(); }
                auto rows = MapUsages::Scan(files, parts, job->cancel, [&](size_t index, size_t, const std::string& file)
                {
                    std::lock_guard<std::mutex> hold(job->lock);
                    job->index = index;
                    job->current = file;
                });
                std::lock_guard<std::mutex> hold(job->lock);
                job->rows = std::move(rows);
                if (files.empty()) job->error = "No .sdc maps found in " + folders.front().path.string() + ".";
            }
            catch (const std::exception& e)
            {
                std::lock_guard<std::mutex> hold(job->lock);
                job->error = std::string("The search failed: ") + e.what();
            }
            std::lock_guard<std::mutex> hold(job->lock);
            job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            job->done = true;
        });
        Buttons(s);
    }
    void Progress(State& s)
    {
        if (!s.job) return;
        bool done = false;
        std::string text;
        {
            std::lock_guard<std::mutex> hold(s.job->lock);
            done = s.job->done;
            if (!done && s.job->total)
                text = "Searching " + std::to_string(s.job->index + 1) + " of " + std::to_string(s.job->total) + ": " + s.job->current;
        }
        if (done) Finish(s);
        else if (!text.empty()) Status(s, text);
    }
    MapUsages::Row* Selected(State& s)
    {
        const int row = ListView_GetNextItem(s.list, -1, LVNI_SELECTED);
        return row >= 0 && row < static_cast<int>(s.rows.size()) ? &s.rows[row] : nullptr;
    }
    void OpenSelected(State& s)
    {
        auto row = Selected(s);
        if (!row) { Status(s, "Select a map in the list first."); return; }
        std::filesystem::path path = row->path;
        // The editor opens source maps; a playable copy's source has its name in MapsEd.
        if (MapUsages::Fold(row->folder) != "mapsed")
        {
            path = Packages() / "MapsEd" / path.filename();
            if (!std::filesystem::exists(path))
            {
                Status(s, row->file + " is a playable map in Maps with no source of the same name in MapsEd; open its source map instead.");
                return;
            }
        }
        if (EditorExtras::OpenMap(path.string(), s.window, "Find Usages in All Maps"))
            Status(s, "Opened " + path.filename().string() + ". Find Usages in the texture or static mesh browser lists each use in the map.");
    }
    void Copy(State& s)
    {
        const auto text = MapUsages::Report(s.query, s.rows, s.scanned);
        if (!OpenClipboard(s.window)) { Status(s, "The clipboard is in use; try again."); return; }
        EmptyClipboard();
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1))
        {
            memcpy(GlobalLock(memory), text.c_str(), text.size() + 1);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_TEXT, memory)) GlobalFree(memory);
        }
        CloseClipboard();
        Status(s, "Copied " + std::to_string(s.rows.size()) + " rows as tab-separated text.");
    }

    LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        auto s = reinterpret_cast<State*>(GetWindowLongPtr(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            s = static_cast<State*>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams);
            s->window = window;
            SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
        }
        if (!s) return DefWindowProcA(window, message, w, l);
        try
        {
            if (message == WM_CREATE)
            {
                Control(window, "STATIC", "Object:", 0, kLabel);
                Control(window, "EDIT", "", ES_AUTOHSCROLL, kObject);
                Control(window, "BUTTON", "Search", BS_DEFPUSHBUTTON, IDOK);
                Control(window, "BUTTON", "Stop", 0, kStop);
                Control(window, "BUTTON", "Also the game's Maps folder (playable copies)", BS_AUTOCHECKBOX, kGameMaps);
                Control(window, "BUTTON", "Include autosaves (Auto0-Auto9)", BS_AUTOCHECKBOX, kAutosaves);
                Control(window, "STATIC",
                        "Reads each map's package tables in Packages\\MapsEd without opening it, and lists the maps that use the object from its package: "
                        "what to check before changing a shared package. Objects stored inside a map's own package (myLevel) are not found this way. "
                        "A texture used only by a static mesh is not named by the map: search the mesh. A package or group name finds everything in it.",
                        0, kNote);
                s->list = Control(window, WC_LISTVIEWA, "", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER, kList);
                ListView_SetExtendedListViewStyle(s->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
                const char* columns[] = { "Map", "Folder", "References", "Used by" };
                for (int i = 0; i < 4; ++i)
                {
                    LVCOLUMNA column{};
                    column.mask = LVCF_TEXT | LVCF_WIDTH;
                    column.cx = 100;
                    column.pszText = const_cast<char*>(columns[i]);
                    SendMessageA(s->list, LVM_INSERTCOLUMNA, i, reinterpret_cast<LPARAM>(&column));
                }
                Control(window, "STATIC", "Enter Package.Group.Name, or right-click a texture or static mesh in its browser.", 0, kStatus);
                Control(window, "BUTTON", "Open Map", 0, kOpen);
                Control(window, "BUTTON", "Copy Results", 0, kCopy);
                Layout(*s);
                Buttons(*s);
                SetTimer(window, kTimer, 100, nullptr);
                return 0;
            }
            if (message == WM_SIZE) { if (s->list) Layout(*s); return 0; }
            if (message == WM_GETMINMAXINFO) { reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = { 640, 420 }; return 0; }
            if (message == WM_TIMER && w == kTimer) { Progress(*s); return 0; }
            if (message == WM_COMMAND)
            {
                switch (LOWORD(w))
                {
                case IDOK: Start(*s); return 0;
                case kStop: if (s->job) s->job->cancel = true; return 0;
                case kOpen: OpenSelected(*s); return 0;
                case kCopy: Copy(*s); return 0;
                }
            }
            if (message == WM_NOTIFY)
            {
                auto header = reinterpret_cast<NMHDR*>(l);
                if (header->hwndFrom == s->list && header->code == NM_DBLCLK) { OpenSelected(*s); return 0; }
            }
            if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        }
        catch (const std::exception& e)
        {
            Status(*s, e.what());
            return message == WM_CREATE ? -1 : 0;
        }
        if (message == WM_NCDESTROY)
        {
            WindowPlacement::Remember(window, "FindUsagesInAllMaps");
            KillTimer(window, kTimer);
            if (s->job) s->job->cancel = true;
            if (s->worker.joinable()) s->worker.join(); // At most one map's read.
            SetWindowLongPtr(window, GWLP_USERDATA, 0);
            if (openWindow == window) openWindow = nullptr;
            delete s;
        }
        return DefWindowProcA(window, message, w, l);
    }
}

void Open(HWND owner, const std::string& object)
{
    if (!IsWindow(openWindow))
    {
        WNDCLASSA wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "ReloadedMapUsages";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassA(&wc);
        auto state = new State;
        openWindow = CreateWindowExA(WS_EX_CONTROLPARENT, wc.lpszClassName, "Find Usages in All Maps", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 560,
                                     owner, nullptr, wc.hInstance, state);
        if (!openWindow) throw std::runtime_error("Could not open Find Usages in All Maps."); // WM_NCDESTROY freed the state.
        WindowPlacement::Restore(openWindow, "FindUsagesInAllMaps");
        ShowWindow(openWindow, SW_SHOW);
    }
    else
    {
        ShowWindow(openWindow, IsIconic(openWindow) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(openWindow);
    }
    auto s = reinterpret_cast<State*>(GetWindowLongPtr(openWindow, GWLP_USERDATA));
    if (!object.empty() && s && !Busy(*s))
    {
        SetDlgItemTextA(openWindow, kObject, object.c_str());
        Start(*s);
    }
}
}
