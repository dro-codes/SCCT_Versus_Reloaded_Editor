#include "pch.h"
#include "EditorExtras.h"
#include "EditorConfigBits.h"
#include "CrashRecovery.h"
#include "CrashRecoveryModel.h"
#include "WorkflowEditor.h"
#include "GEKeybindSwap.h"
#include "LevelSnapshot.h"
#include "PlayLevelPlayers.h"
#include "SelectionModel.h"
#include "PropertyPaste.h"
#include "SaveBackupModel.h"
#include "SaveBackups.h"
#include "CompareBackup.h"
#include "UnsavedChangesModel.h"
#include "LightmapFix.h"
#include "SelfUpdater.h"
#include "CommandPalette.h"
#include "Version.h"
#include "logger.h"
#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

using Workflow::Json;
using Workflow::Vector;
using Workflow::Rotation;
using Workflow::Pose;
namespace Editor = Workflow::Editor;

namespace
{
    HWND frameWindow = nullptr;
    // The install runs on the frame's thread from a message hook. The hook stays
    // until an attempt puts every menu entry in place; `installed` says so,
    // independently of the hook handle, which the injection thread stores only
    // after the hook may already have run.
    std::atomic<HHOOK> startupHook{nullptr};
    std::atomic<bool> installed{false};
    unsigned installAttempts = 0;
    DWORD lastInstallAttempt = 0;
    const char* installStep = "";
    HMENU recentMenu = nullptr;
    HWND shortcutsWindow = nullptr;
    std::vector<std::string> recent;
    std::string lastMap;
    // Whether the open map has changes its file does not (UnsavedChangesModel.h).
    UnsavedChanges::Tracker changes;
    constexpr UINT_PTR kTimer = 0x5245;
    constexpr UINT_PTR kOfferTimer = 0x5246; // the crash-recovery offer, until it has been made
    constexpr UINT_PTR kConveyorTimer = 0x5247; // conveyor belt frames follow Length and Width
    constexpr int kRecentMax = 10;

    std::filesystem::path RecentFile() { return Editor::Directory() / "recent_maps.json"; }
    void LoadRecent()
    {
        recent.clear();
        try
        {
            std::ifstream in(RecentFile());
            if (!in) return;
            Json list; in >> list;
            for (const auto& entry : list) if (entry.is_string() && recent.size() < kRecentMax) recent.push_back(entry.get<std::string>());
        }
        catch (const std::exception&) { recent.clear(); }
    }
    void SaveRecent()
    {
        try
        {
            std::error_code error;
            std::filesystem::create_directories(RecentFile().parent_path(), error);
            std::ofstream out(RecentFile());
            out << Json(recent).dump(2);
        }
        catch (const std::exception&) { /* The list is a convenience; a failed save is not worth a dialog. */ }
    }
    void RebuildRecentMenu()
    {
        if (!recentMenu) return;
        while (GetMenuItemCount(recentMenu) > 0) DeleteMenu(recentMenu, 0, MF_BYPOSITION);
        if (recent.empty()) AppendMenuA(recentMenu, MF_STRING | MF_GRAYED, 0, "(no maps opened yet)");
        for (size_t i = 0; i < recent.size() && i < kRecentMax; ++i)
        {
            const std::filesystem::path path(recent[i]);
            const std::string label = "&" + std::to_string((i + 1) % 10) + "  " + path.filename().string() + "\t" + path.parent_path().filename().string();
            AppendMenuA(recentMenu, MF_STRING, EditorExtras::kRecentFirst + static_cast<UINT>(i), label.c_str());
        }
    }
    void NoteMap(const std::string& map)
    {
        if (map.empty()) return;
        const auto folded = Workflow::Fold(map);
        recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const std::string& entry) { return Workflow::Fold(entry) == folded; }), recent.end());
        recent.insert(recent.begin(), map);
        if (recent.size() > kRecentMax) recent.resize(kRecentMax);
        SaveRecent();
        RebuildRecentMenu();
    }

    bool InstallMenus();

    // The file a save wrote, in full: the engine opens a relative name from
    // the current directory (System), the frame's own map name from MapsEd.
    // The engine's name is ANSI; the result is UTF-8, as Editor::MapFile's is.
    std::string SavedFile(const char* path)
    {
        namespace fs = std::filesystem;
        fs::path file(path);
        std::error_code error;
        if (file.is_relative())
        {
            const auto here = fs::absolute(file, error);
            file = !error && fs::is_regular_file(here, error) ? here : Editor::Directory().parent_path().parent_path() / "Packages" / "MapsEd" / file;
        }
        const auto canonical = fs::weakly_canonical(file, error);
        const auto text = (error ? file : canonical).u8string();
        return std::string(text.begin(), text.end());
    }
    // After every save the engine finished (LightmapFix::SetSavedSink): a save
    // of the open map's own file makes it clean, whichever route it took.
    void __cdecl NoteSaved(const char* path)
    {
        if (!path || !*path) return;
        try { UnsavedChanges::Saved(changes, Editor::MapOpened(), Editor::ContentRevision(), SavedFile(path), Editor::MapFile()); }
        catch (...) { /* Inside the engine's save: nothing may escape, and a save not recorded only means one more warning. */ }
    }

    // Every few seconds on the frame's thread: a newly opened or saved-as
    // map goes to the top of the recent list. Whether it is clean was settled
    // when it was opened or saved, not when this notices it. Menu entries
    // lost to a rebuilt menu bar are put back.
    void Tick()
    {
        EditorConfigBits::Apply();
        PlayLevelPlayers::Attach(frameWindow);
        static bool reported = false;
        if (!InstallMenus() && !reported)
        {
            reported = true;
            Logger::log("Editor extras: some menu entries could not be put back on the timer");
        }
        // The first look at the editor starts the count of its edits and the
        // maps it opens, so a map opened soon after start-up is seen open.
        try { UnsavedChanges::Follow(changes, Editor::MapOpened()); }
        catch (const std::exception&) { /* No map yet. */ }
        try
        {
            const auto map = Editor::MapFile();
            if (map == lastMap) return;
            lastMap = map;
            CrashRecovery::NoteMap(map);
            // An opened autosave or backup is a moment, not a map to come back to.
            if (!Sessions::IsAutosaveName(map) && !SaveBackups::IsBackupPath(map)) NoteMap(map);
        }
        catch (const std::exception&) { /* The engine may not be ready yet. */ }
    }

    LRESULT CALLBACK FrameProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR)
    {
        if (message == WM_TIMER && w == kTimer) { Tick(); return 0; }
        if (message == WM_TIMER && w == kConveyorTimer)
        {
            try { Editor::SyncConveyorBelts(); }
            catch (const std::exception&) { /* No map open, or the editor is busy. */ }
            return 0;
        }
        if (message == WM_TIMER && w == kOfferTimer)
        {
            if (CrashRecovery::OfferWhenIdle(window)) KillTimer(window, kOfferTimer);
            return 0;
        }
        // The frame goes only when the editor is closing: the session ended
        // cleanly, unless the engine is going down with an error.
        if (message == WM_DESTROY) CrashRecovery::EndSession();
        if (message == WM_INITMENUPOPUP && reinterpret_cast<HMENU>(w) == recentMenu) { RebuildRecentMenu(); return 0; }
        if (message == WM_COMMAND)
        {
            // Rebuilding geometry makes every hidden actor visible again, so
            // whatever the Brush Visibility tool, the Scene panel or Hide
            // selected had hidden before the command is hidden again after a
            // command that turns out to have built.
            unsigned long before = 0;
            Json hidden;
            try
            {
                before = Editor::GeometryBuilds();
                hidden = Editor::HiddenActors();
            }
            catch (const std::exception&) { /* Not ready: nothing to restore. */ }
            const LRESULT result = DefSubclassProc(window, message, w, l);
            if (!hidden.empty())
                try
                {
                    if (Editor::GeometryBuilds() != before) Editor::RestoreHiddenActors(hidden);
                }
                catch (const std::exception&) { /* The map went away under us. */ }
            return result;
        }
        if (message == WM_NCDESTROY) { RemoveWindowSubclass(window, FrameProc, 1); frameWindow = nullptr; }
        return DefSubclassProc(window, message, w, l);
    }

    // Top-level submenu of the frame's bar that directly contains a command.
    HMENU MenuWithCommand(HMENU bar, UINT command)
    {
        for (int i = 0; i < GetMenuItemCount(bar); ++i)
            if (HMENU sub = GetSubMenu(bar, i))
                for (int j = 0; j < GetMenuItemCount(sub); ++j)
                    if (GetMenuItemID(sub, j) == command) return sub;
        return nullptr;
    }
    bool HasSubMenu(HMENU menu, HMENU sub)
    {
        for (int i = 0; i < GetMenuItemCount(menu); ++i)
            if (GetSubMenu(menu, i) == sub) return true;
        return false;
    }
    // Idempotent; true when every entry is on the frame's current menu bar.
    bool InstallMenus()
    {
        HMENU bar = GetMenu(frameWindow);
        if (!bar) return false;
        HMENU file = GetSubMenu(bar, 0);
        if (file && recentMenu && !HasSubMenu(file, recentMenu)) recentMenu = nullptr; // The bar was rebuilt.
        bool changed = false;
        if (file && !recentMenu)
        {
            changed = true;
            // After New and Open.
            recentMenu = CreatePopupMenu();
            InsertMenuA(file, 2, MF_BYPOSITION | MF_POPUP, reinterpret_cast<UINT_PTR>(recentMenu), "Open &Recent");
            RebuildRecentMenu();
        }
        if (file && recentMenu && GetMenuState(file, EditorExtras::kOpenLatestAutosave, MF_BYCOMMAND) == UINT(-1))
        {
            changed = true;
            // Right after Open Recent.
            int position = 0;
            while (position < GetMenuItemCount(file) && GetSubMenu(file, position) != recentMenu) ++position;
            InsertMenuA(file, position + 1, MF_BYPOSITION | MF_STRING, EditorExtras::kOpenLatestAutosave, "Open Latest A&utosave...");
        }
        if (file && GetMenuState(file, EditorExtras::kOpenLatestAutosave, MF_BYCOMMAND) != UINT(-1)
            && GetMenuState(file, EditorExtras::kOpenMapBackup, MF_BYCOMMAND) == UINT(-1))
        {
            changed = true;
            // Right after Open Latest Autosave.
            int position = 0;
            while (position < GetMenuItemCount(file) && GetMenuItemID(file, position) != EditorExtras::kOpenLatestAutosave) ++position;
            InsertMenuA(file, position + 1, MF_BYPOSITION | MF_STRING, EditorExtras::kOpenMapBackup, "Open Map &Backup...");
        }
        if (file && GetMenuState(file, EditorExtras::kOpenMapBackup, MF_BYCOMMAND) != UINT(-1)
            && GetMenuState(file, CompareBackup::kCompare, MF_BYCOMMAND) == UINT(-1))
        {
            changed = true;
            // Right after Open Map Backup.
            int position = 0;
            while (position < GetMenuItemCount(file) && GetMenuItemID(file, position) != EditorExtras::kOpenMapBackup) ++position;
            InsertMenuA(file, position + 1, MF_BYPOSITION | MF_STRING, CompareBackup::kCompare, "Compare &with Backup...");
        }
        if (HMENU build = MenuWithCommand(bar, 40038); build && GetMenuState(build, EditorExtras::kPlayFromCameraSpy, MF_BYCOMMAND) == UINT(-1))
        {
            changed = true;
            AppendMenuA(build, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(build, MF_STRING, EditorExtras::kPlayFromCameraSpy, "Play From Camera as &Spy");
            AppendMenuA(build, MF_STRING, EditorExtras::kPlayFromCameraMerc, "Play From Camera as &Merc");
            AppendMenuA(build, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(build, MF_STRING, EditorExtras::kLevelSnapshotViewport, "Set Level Snapshot from &Viewport");
            AppendMenuA(build, MF_STRING, EditorExtras::kLevelSnapshotFile, "Set Level Snapshot from &Image File...");
        }
        if (HMENU help = MenuWithCommand(bar, 40480))
        {
            if (GetMenuState(help, EditorExtras::kShortcuts, MF_BYCOMMAND) == UINT(-1))
            {
                changed = true;
                AppendMenuA(help, MF_STRING, EditorExtras::kShortcuts, "RE+ &Shortcuts...");
            }
            if (GetMenuState(help, SelfUpdater::kCheckNow, MF_BYCOMMAND) == UINT(-1))
            {
                changed = true;
                SelfUpdater::AppendHelpMenu(help, frameWindow);
            }
        }
        if (changed) DrawMenuBar(frameWindow);
        HMENU build = MenuWithCommand(bar, 40038), help = MenuWithCommand(bar, 40480);
        return file && recentMenu && HasSubMenu(file, recentMenu)
            && GetMenuState(file, EditorExtras::kOpenLatestAutosave, MF_BYCOMMAND) != UINT(-1)
            && GetMenuState(file, EditorExtras::kOpenMapBackup, MF_BYCOMMAND) != UINT(-1)
            && GetMenuState(file, CompareBackup::kCompare, MF_BYCOMMAND) != UINT(-1)
            && build && GetMenuState(build, EditorExtras::kPlayFromCameraSpy, MF_BYCOMMAND) != UINT(-1)
            && help && GetMenuState(help, EditorExtras::kShortcuts, MF_BYCOMMAND) != UINT(-1)
            && GetMenuState(help, SelfUpdater::kCheckNow, MF_BYCOMMAND) != UINT(-1);
    }

    // Every step is safe to repeat, so a failed attempt is simply retried.
    // installStep names the step for the log should one fault.
    bool InstallSteps(std::string& error)
    {
        static bool loaded = false;
        try
        {
            installStep = "LoadRecent";
            if (!loaded) { LoadRecent(); loaded = true; }
            installStep = "EditorConfigBits::Apply";
            EditorConfigBits::Apply();
            installStep = "LevelSnapshot::Attach";
            LevelSnapshot::Attach(frameWindow);
            installStep = "PlayLevelPlayers::Attach";
            PlayLevelPlayers::Attach(frameWindow); // retried on the tick if the bar is not there yet
            installStep = "CommandPalette::Attach";
            CommandPalette::Attach(frameWindow); // its key hook lives on this, the frame's, thread
            installStep = "SetWindowSubclass";
            if (!SetWindowSubclass(frameWindow, FrameProc, 1, 0)) { error = "SetWindowSubclass failed"; return false; }
            installStep = "SetTimer";
            if (!SetTimer(frameWindow, kTimer, 10000, nullptr)) { error = "SetTimer failed"; return false; }
            if (!SetTimer(frameWindow, kOfferTimer, 1000, nullptr)) { error = "SetTimer failed"; return false; }
            if (!SetTimer(frameWindow, kConveyorTimer, 500, nullptr)) { error = "SetTimer failed"; return false; }
            installStep = "InstallMenus";
            if (!InstallMenus()) { error = "menu bar not ready"; return false; }
            return true;
        }
        catch (const std::exception& e) { error = e.what(); }
        catch (...) { error = "unknown C++ exception"; }
        return false;
    }
    // Faults in a hook callback can vanish on WOW64 without a trace; catch
    // them here so the attempt is logged and retried.
    bool GuardedInstall(std::string& error, DWORD& fault)
    {
        __try { return InstallSteps(error); }
        __except (fault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    void TryInstall()
    {
        if (installed || !IsWindow(frameWindow)) return;
        const DWORD now = GetTickCount();
        if (installAttempts && now - lastInstallAttempt < 250) return;
        lastInstallAttempt = now;
        ++installAttempts;
        std::string error;
        DWORD fault = 0;
        if (GuardedInstall(error, fault))
        {
            installed = true;
            Logger::log("Editor extras: installed on attempt " + std::to_string(installAttempts));
            return;
        }
        // The first few failures, then every 40th (about every 10s).
        if (installAttempts <= 5 || installAttempts % 40 == 0)
        {
            char code[32] = "";
            if (fault) sprintf_s(code, "fault 0x%08lX", fault);
            Logger::log("Editor extras: attempt " + std::to_string(installAttempts) + " failed at " + installStep + ": "
                        + (fault ? std::string(code) : error));
        }
    }
    LRESULT CALLBACK StartupHook(int code, WPARAM w, LPARAM l)
    {
        // Runs on the frame's thread, on every message until the install
        // succeeds; then the hook removes itself.
        const LRESULT result = CallNextHookEx(nullptr, code, w, l);
        if (code >= 0) TryInstall();
        if (installed)
            if (HHOOK hook = startupHook.exchange(nullptr)) UnhookWindowsHookEx(hook);
        return result;
    }

    // The shortcut legend: everything Reloaded adds to the editor's keys and
    // mouse, with the Geometric Event keys as currently configured.
    std::string ShortcutText()
    {
        auto key = [](uint8_t k) { return std::string("Shift+") + static_cast<char>(k); };
        return "EVERYWHERE IN THE EDITOR\r\n"
               "Ctrl + mouse wheel over a viewport: change the grid size (the plan's snapping follows).\r\n"
               "Ctrl+D in a viewport: duplicate the selection in one Undo step (RE+ Options can drop the offset). Brush > Deintersect keeps its own menu entry.\r\n"
               "J in a viewport (no Ctrl, Shift or Alt): Game View, hiding editor icons and sprites the game does not draw.\r\n"
               "M in a viewport: measure from the last measured point to the mouse (in the 3D view, across the floor at its height).\r\n"
               "End in a viewport: drop the selection to the floor.   Shift+End: and turn meshes flat on the surface.   Ctrl+End: to the ceiling.\r\n"
               "F12: RE+ Options.   F7: disabled (the stock script compiler would crash).\r\n"
               "Ctrl+Shift+P: Command Palette (Help > Command Palette): type part of any menu command's name, Enter runs it.\r\n"
               "1 to 9 in a viewport: jump to camera bookmark N.   Ctrl+1 to Ctrl+9: store the viewport cameras as bookmark N (RE+ Tools > Camera Bookmarks).\r\n"
               "\r\n"
               "PROPERTIES WINDOWS (F4 Actor Properties, F6 Level Properties)\r\n"
               "Filter box: lists only properties whose name or category contains the typed words.   Esc in the box: clear it.\r\n"
               "Several actors, even of different classes: their shared properties, one value for all in one Undo step;\r\n"
               "(multiple values) where they differ.\r\n"
               "\r\n"
               "GEOMETRIC EVENTS (set in RE+ Options)\r\n"
               + key(g_KeyLedgeGrab) + ": ledge grab   " + key(g_KeyHandOverHand) + ": hand-over-hand   " + key(g_KeyPipe) + ": pipe\r\n"
               + key(g_KeyLadder) + ": ladder   " + key(g_KeyZipline) + ": zip line   " + key(g_KeyFence) + ": fence\r\n"
               "\r\n"
               "RIGHT-CLICK MENUS\r\n"
               "Actor: RE+: Select (all of this class, all with this Tag, same static mesh, invert) and\r\n"
               "RE+: Visibility (hide selected, isolate selected, unhide all); RE+: Properties (Copy Properties, then\r\n"
               "Paste Properties... onto the selection, ticking which ones; Paste Again pastes the same ones);\r\n"
               "RE+: Placement (drop to floor, ceiling or wall,\r\n"
               "push to a wall along X or Y, align to the last selected, distribute evenly, snap to the grid (pivots, box edges, yaw),\r\n"
               "copies in a line, round a centre or along a path); Save Selection as Assembly;\r\n"
               "Edit SMagicEvent; Add SObjective / triggers on a mission or objective; position the builder brush around meshes.\r\n"
               "Brush face or vertices: snap to the grid per axis.   Texture / mesh browser: Favorites, Find Usages and Find Usages in All Maps.\r\n"
               "Staircase brush or its faces: Smooth Staircase (an invisible blocking ramp over the steps) / Remove Staircase Smoothing.\r\n"
               "Any viewport: Builder Brush > Place Here; Measure > Start Here / To Here / Clear Measurement (distance,\r\n"
               "dX/dY/dZ, player heights and run time, drawn into every viewport).\r\n"
               "\r\n"
               "BROWSERS\r\n"
               "French asset names show their English: Porte_Metal01 (door metal 01). Hover a narrow list for the\r\n"
               "whole name; the Texture Browser's Filter box finds either language. RE+ Options turns it off.\r\n"
               "Static Mesh Browser: the Search box under the list finds meshes with every typed word in package.group.name\r\n"
               "or its English (All packages: every loaded mesh package). Esc in the box: clear it.   Down: to the list.\r\n"
               "\r\n"
               "MENUS\r\n"
               "Edit: Undo History lists every undo step, oldest first, with redo steps greyed below the current one;\r\n"
               "double-click a step (or select it and press Enter) to undo or redo to it. Ctrl+Z / Ctrl+Y step there too.\r\n"
               "View: Map Outliner lists every actor (search, group by class or Group, sort by a column); click selects, Ctrl/Shift add,\r\n"
               "double-click / Enter / F frames, H hides, Delete deletes (asks first, one Undo step), Ctrl+A all, Left/Right fold a group.\r\n"
               "File: Optimise Map Assets makes a release copy carrying only the assets it uses from big packs.\r\n"
               "File: Open Recent lists the last ten maps. The editor's own autosave (View > Advanced Options,\r\n"
               "Editor.EditorEngine: AutoSave, AutoSaveTimeMinutes) writes Auto0 to Auto9.sdc into MapsEd;\r\n"
               "File: Open Latest Autosave opens the newest of them. After a crash, the next start offers that session's autosave.\r\n"
               "File: Open Map Backup opens one of the last saved versions RE+ keeps of each map (System\\ReloadedEditor\\Backups); Save As over the map to restore it.\r\n"
               "File: Compare with Backup lists what differs from a backup, an autosave or any .sdc (actors added, deleted, moved,\r\n"
               "changed); click a row to select the actor; tick rows and Bring Back Selected restores the backup's version in one Undo step.\r\n"
               "Build: Play From Camera as Spy / Merc starts a playtest at the perspective viewport's camera.\r\n"
               "Build: Set Level Snapshot from Viewport / Image File puts the picture the game shows in map selection\r\n"
               "into the map's <Map>-i package (Packages\\Textures), backing up the old one under ReloadedEditor.\r\n"
               "Help: Check for RE+ Updates offers the newest release; Check for Updates at Startup turns the automatic check on or off;\r\n"
               "What's New in RE+ shows this version's release notes (also shown once after an update); Roll Back puts the version\r\n"
               "an update replaced back in place (and the newer one stays kept, to switch again); About RE+ shows the version.\r\n"
               "RE+ Tools: Map Design (its own Keys... window lists the plan's shortcuts), Brush Visibility, Placement,\r\n"
               "Gameplay Connections, SMagicEvent Workbench, SCamNetwork Manager, Working Views, Assemblies,\r\n"
               "Scatter Props (fill the builder brush or a circle round the last click with weighted static meshes,\r\n"
               "previewed in the viewports, placed as one Undo step), JSON.\r\n";
    }
    LRESULT CALLBACK ShortcutsProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_CREATE)
        {
            const auto text = ShortcutText();
            HWND edit = CreateWindowExA(0, "EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                        8, 8, 700, 420, window, reinterpret_cast<HMENU>(1), GetModuleHandle(nullptr), nullptr);
            SendMessage(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            return 0;
        }
        if (message == WM_SIZE) { MoveWindow(GetDlgItem(window, 1), 8, 8, (std::max)(1, LOWORD(l) - 16), (std::max)(1, HIWORD(l) - 16), TRUE); return 0; }
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (message == WM_NCDESTROY) shortcutsWindow = nullptr;
        return DefWindowProcA(window, message, w, l);
    }
    void ShowShortcuts()
    {
        if (!shortcutsWindow)
        {
            WNDCLASSA wc{};
            wc.hInstance = GetModuleHandle(nullptr);
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            wc.lpfnWndProc = ShortcutsProc;
            wc.lpszClassName = "ReloadedShortcuts";
            RegisterClassA(&wc);
            shortcutsWindow = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, "RE+ Shortcuts", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                              CW_USEDEFAULT, CW_USEDEFAULT, 740, 470, frameWindow, nullptr, wc.hInstance, nullptr);
            if (!shortcutsWindow) throw std::runtime_error("Cannot open the shortcut legend.");
        }
        else SetWindowTextA(GetDlgItem(shortcutsWindow, 1), ShortcutText().c_str());
        ShowWindow(shortcutsWindow, SW_SHOWNORMAL);
        SetForegroundWindow(shortcutsWindow);
    }

    // Selection helpers built on the editor's own actor list, so they work
    // for several classes or tags at once. The builder brush, the level info
    // and viewport cameras (not authorable) are never taken along, and
    // SelectionModel.h keeps hidden actors out of what is selected, and Hide
    // selected takes any hidden ones still selected out of the selection.
    namespace Selection = Workflow::Selection;
    template<class Rule> Json Chosen(Rule rule)
    {
        const auto actors = Editor::Actors();
        std::vector<Selection::Actor> listed;
        for (const auto& actor : actors) listed.push_back(Selection::FromJson(actor));
        Json chosen = Json::array();
        for (const auto i : rule(listed)) chosen.push_back(actors[i]);
        return chosen;
    }
    void SelectMatching(const char* field, const char* what)
    {
        const auto value = std::string(field) == "tag" ? &Selection::Actor::tag : &Selection::Actor::cls;
        Editor::Select(Chosen([&](const std::vector<Selection::Actor>& actors) { return Selection::Matching(actors, value, what); }), false);
    }
    void Exec(const char* command, const char* failure)
    {
        if (!Editor::Exec(command)) throw std::runtime_error(failure);
        Editor::Redraw();
    }
    // The rest of the selection, and hiding through the same flags the Scene
    // panel sets, so the plan, the panel and the viewports agree.
    void InvertSelection()
    {
        Editor::Select(Chosen(Selection::Inverted), false);
    }
    void Hide(bool selectedOnes)
    {
        const auto members = Chosen(selectedOnes ? Selection::ToHide : Selection::ToIsolate);
        if (members.empty()) return; // Isolating with the rest already hidden.
        Editor::DesignSetFlags(members, 1, -1);
        Editor::Redraw();
    }
    // From the actor list, not the Design scene: that reads every brush's
    // polygons and refuses a large map or a stray vertex.
    void UnhideAll()
    {
        Editor::DesignSetFlags(Chosen(Selection::ToUnhide), 0, -1);
        Editor::Redraw();
    }

    // A playtest from the perspective viewport's camera, as one team: from
    // that team's first start when it has one, else from a temporary start
    // that is removed again when Play Level returns.
    void PlayFromCamera(const std::string& team)
    {
        Pose pose;
        bool found = false;
        for (const auto& camera : Editor::CaptureView().at("cameras"))
        {
            const int mode = camera.at("mode");
            if (mode == 13 || mode == 14 || mode == 15) continue; // The orthographic views.
            pose = { camera.at("position").get<Vector>(), camera.at("rotation").get<Rotation>() };
            found = true;
            break;
        }
        if (!found) throw std::runtime_error("No perspective viewport to take the camera from.");
        pose.rotation[0] = 0; // A pawn stands level; only the camera's yaw carries over.
        pose.rotation[2] = 0;
        const auto starts = Editor::DesignSpawns();
        for (const auto& start : starts)
            if (start.at("team") == team) { Editor::DesignPlay(start, pose); return; }
        const std::string type = starts.empty() ? "Engine.PlayerStart" : starts.front().at("class").get<std::string>();
        const auto temporary = Editor::DesignTemporaryStart(type, team, pose);
        struct Remove
        {
            Json start;
            ~Remove() { try { Editor::DesignRemoveTemporaryStart(start); } catch (const std::exception&) {} }
        } remove{ temporary };
        Editor::DesignPlay(temporary, pose);
    }

    void OpenRecent(size_t index)
    {
        if (index >= recent.size()) return;
        const std::string path = recent[index];
        if (!std::filesystem::exists(path))
        {
            recent.erase(recent.begin() + index);
            SaveRecent();
            RebuildRecentMenu();
            throw std::runtime_error("That map is no longer there:\n" + path);
        }
        EditorExtras::OpenMap(path, frameWindow, "Open Recent");
    }
}

bool EditorExtras::OpenMap(const std::string& path, HWND owner, const char* title, bool confirmDiscard)
{
    if (confirmDiscard && UnsavedChanges::Unsaved(changes, Editor::MapOpened(), Editor::ContentRevision(), Editor::MapFile())
        && MessageBoxA(owner, ("Open " + std::filesystem::path(path).filename().string() + "?\n\nUnsaved changes in the current map will be lost.").c_str(),
                       title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return false;
    if (!Editor::Exec("MAP LOAD FILE=\"" + path + "\"")) throw std::runtime_error("The editor could not open\n" + path);
    // The map's settings, as the stock File > Open loads them: without this the
    // previous map's stay current, and the next save writes them (and that
    // map's interface package) over this map's <Map>-i. An autosave (Auto0-9)
    // is left out: it has no settings of its own, and blank ones would be
    // written over the real map's when it is saved back under that name. A
    // kept backup loads the settings of the map it was taken from, so they
    // are current when it is saved over that map to restore it.
    std::string stem = std::filesystem::path(path).stem().string();
    const bool backup = SaveBackups::IsBackupPath(path);
    if (backup)
    {
        const auto source = SaveBackups::ParseBackup(path);
        stem = source ? source->first : std::string();
    }
    if (!Sessions::IsAutosaveName(path) && !stem.empty() && stem.find_first_of(" \t\"'") == std::string::npos)
        Editor::Exec("LOADMAPPROP MAP=\"" + stem + "\"");
    Editor::SetMapFile(path);
    lastMap = path;
    // The settings and the Save target are part of opening it.
    UnsavedChanges::Settled(changes, Editor::MapOpened(), Editor::ContentRevision());
    CrashRecovery::NoteMap(path);
    if (!Sessions::IsAutosaveName(path) && !backup) NoteMap(path);
    Editor::Redraw();
    return true;
}

void EditorExtras::OpenMap(const std::string& path, bool confirmDiscard)
{
    OpenMap(path, frameWindow, "Open Recent", confirmDiscard);
}

void EditorExtras::Attach(HWND frame)
{
    if (!frame || frameWindow) return;
    frameWindow = frame;
    LightmapFix::SetSavedSink(NoteSaved);
    CrashRecovery::Start();
    const DWORD thread = GetWindowThreadProcessId(frame, nullptr);
    Logger::log("Editor extras: attaching to the frame's thread " + std::to_string(thread));
    // A hook on another thread belongs to the thread that set it and goes
    // away when that thread exits. Returning at once let the injection thread
    // end before the frame's thread, busy starting up, had looked at a
    // message: then nothing was installed at all. So this thread waits for
    // the install, hooking again if hooking failed and giving the hook
    // messages to see.
    for (int i = 0; i < 1200 && !installed; ++i) // up to ~10 minutes
    {
        if (!IsWindow(frame)) { Logger::log("Editor extras: the frame window went away before the install"); return; }
        if (!startupHook)
        {
            HHOOK hook = SetWindowsHookExA(WH_GETMESSAGE, StartupHook, nullptr, thread);
            if (!hook && i % 20 == 0) Logger::log("Editor extras: could not hook the frame's thread, error " + std::to_string(GetLastError()));
            startupHook = hook;
        }
        if (i == 60) Logger::log("Editor extras: not installed after 30s, still trying (" + std::to_string(installAttempts) + " attempts)");
        if (i % 4 == 0) PostMessage(frame, WM_NULL, 0, 0); // Something for the hook to see.
        Sleep(500);
    }
    if (!installed) Logger::log("Editor extras: gave up waiting for the install");
}

void EditorExtras::AppendActorMenu(HMENU menu)
{
    HMENU select = CreatePopupMenu(), visibility = CreatePopupMenu(), properties = CreatePopupMenu();
    if (!select || !visibility || !properties) return;
    AppendMenuA(select, MF_STRING, kSelectSameClass, "All of this &class");
    AppendMenuA(select, MF_STRING, kSelectSameTag, "All with this &Tag");
    AppendMenuA(select, MF_STRING, kSelectSameMesh, "Same static &mesh");
    AppendMenuA(select, MF_STRING, kInvertSelection, "&Invert selection");
    AppendMenuA(visibility, MF_STRING, kHideSelected, "&Hide selected");
    AppendMenuA(visibility, MF_STRING, kIsolateSelected, "&Isolate selected (hide the rest)");
    AppendMenuA(visibility, MF_STRING, kUnhideAll, "&Unhide all");
    const UINT pasteable = PropertyPaste::HasCopy() ? 0 : MF_GRAYED;
    AppendMenuA(properties, MF_STRING, PropertyPaste::kCopy, "&Copy Properties");
    AppendMenuA(properties, MF_STRING | pasteable, PropertyPaste::kPaste, "&Paste Properties...");
    AppendMenuA(properties, MF_STRING | pasteable, PropertyPaste::kPasteAgain, "Paste &Again");
    AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(select), "RE+: &Select");
    AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(visibility), "RE+: &Visibility");
    AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(properties), "RE+: P&roperties");
}

bool EditorExtras::HandleCommand(UINT command)
{
    if (SelfUpdater::HandleCommand(command)) return true;
    const bool ours = (command >= kSelectSameClass && command <= kLevelSnapshotFile) || (command >= kRecentFirst && command <= kRecentLast)
        || command == kOpenLatestAutosave || command == kOpenMapBackup;
    if (!ours) return false;
    try
    {
        if (command >= kRecentFirst && command <= kRecentLast) OpenRecent(command - kRecentFirst);
        else if (command == kOpenLatestAutosave) CrashRecovery::OpenLatestAutosave(frameWindow);
        else if (command == kOpenMapBackup) SaveBackups::OpenDialog(frameWindow);
        else if (command == kSelectSameClass) SelectMatching("class", "class");
        else if (command == kSelectSameTag) SelectMatching("tag", "Tag");
        else if (command == kSelectSameMesh) Exec("ACTOR SELECT MATCHINGSTATICMESH", "Select a static mesh actor first.");
        else if (command == kInvertSelection) InvertSelection();
        else if (command == kHideSelected) Hide(true);
        else if (command == kIsolateSelected) Hide(false);
        else if (command == kUnhideAll) UnhideAll();
        else if (command == kPlayFromCameraSpy) PlayFromCamera("0");
        else if (command == kPlayFromCameraMerc) PlayFromCamera("1");
        else if (command == kShortcuts) ShowShortcuts();
        else if (command == kLevelSnapshotViewport) LevelSnapshot::FromViewport();
        else if (command == kLevelSnapshotFile) LevelSnapshot::FromImageFile();
    }
    catch (const std::exception& e)
    {
        MessageBoxA(GetActiveWindow(), e.what(), RE_PLUS_NAME, MB_OK | MB_ICONINFORMATION);
    }
    return true;
}
