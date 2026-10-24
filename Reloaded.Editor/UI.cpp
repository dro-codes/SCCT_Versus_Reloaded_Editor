#include "pch.h"
#include "UI.h"

#include "Hooks.h"
#include "MapRecovery.h"
#include "WindowDriftFix.h"
#include "RebuildAllMaps.h"
#include "LightingBudget.h"
#include "MapCheck.h"
#include "LightShadowMap.h"
#include "RenderBudget.h"
#include "Reachability.h"
#include "MapOutliner.h"
#include "MoverWorkbench.h"
#include "MinimapWindow.h"
#include "WorkflowTools.h"
#include "CharacterSkinsWindow.h"
#include "ObjImportWindow.h"
#include "CharacterSkinPresetsWindow.h"
#include "EmitterLibraryWindow.h"
#include "StoreyFilter.h"
#include "UndoHistory.h"
#include "FavoritesWindow.h"
#include "MapPackageDialog.h"
#include "MapOptimiseDialog.h"
#include "ObjExportWindow.h"
#include "EditorExtras.h"
#include "PlacementTools.h"
#include "CommandPalette.h"
#include "ScatterTool.h"
#include "Appearance.h"

#include "Version.h"

INIT_HOOKS;

// The frame's own title, "RE+ 2.0.0 - Chaos Theory Editor"; with a map open the
// frame adds " - [<map>]" after it. It is passed to sprintf as a format.
static const char editor_title[] = RE_PLUS_WINDOW_TITLE;
// The map window's title, just the map name.
static const char map_title[] = "%s";

// The stock map title is sprintf(buffer, "...Version %.1f - [%s", version, map),
// with the version a float pushed as a double. RE+ versions are not floats, so
// the map name (the third argument) is copied over the double's first half and
// a "%s" format reads it from there. The call still pops all its arguments.
// edx is free here: the call right after clobbers it.
JMP_HOOK(0x10E4319C, SetTitle) {
    static int Return = 0x10E431A1;
    __asm {
        mov edx, dword ptr[esp + 8]
        mov dword ptr[esp], edx
        push offset map_title
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E3C138, SetTitle2) {
    static int Return = 0x10E3C13D;
    __asm {
        mov edx, dword ptr[esp + 8]
        mov dword ptr[esp], edx
        push offset map_title
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E2E49F, SetTitleEditorName) {
    static int Return = 0x10E2E4A4;
    __asm {
        push offset editor_title
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E4B285, SetTitleEditorName2) {
    static int Return = 0x10E4B28A;
    __asm {
        push offset editor_title
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E3EBE0, SkipPointlessSecondPopup) {
    static int Return = 0x10E3EC26;
    __asm {
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E3E071, VerboseSavePopup) {
    static int Return = 0x10E3E076;
    __asm {
        push offset verbose_save_message
        jmp dword ptr[Return]
    }
}

JMP_HOOK(0x10E3E0B8, SkipPointlessSecondPopupOnExit) {
    static int Return = 0x10E3ED8F;
    __asm {
        jmp dword ptr[Return]
    }
}

// Add RE+ menu items to the main menu
// Command IDs are handled by MenuBarDispatch (General.cpp)
static int MenuPosByCommand(HMENU menu, UINT cmd)
{
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
        if (GetMenuItemID(menu, i) == cmd)
            return i;
    return -1;
}

static int MenuPosByText(HMENU menu, const char* text)
{
    const int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i)
    {
        char label[128] = {};
        GetMenuStringA(menu, i, label, sizeof(label), MF_BYPOSITION);
        if (!strcmp(label, text)) return i;
    }
    return -1;
}

// Top-level submenu of the bar that directly contains 'cmd'.
static HMENU SubMenuWithCommand(HMENU bar, UINT cmd)
{
    const int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i)
    {
        HMENU sub = GetSubMenu(bar, i);
        if (sub && MenuPosByCommand(sub, cmd) >= 0)
            return sub;
    }
    return nullptr;
}

static void InjectReloadedMenuItems(HWND frame)
{
    HMENU bar = GetMenu(frame);
    if (!bar) return;

    HMENU reTools = nullptr;
    const int reToolsPos = MenuPosByText(bar, "RE+ &Tools");
    if (reToolsPos >= 0)
        reTools = GetSubMenu(bar, reToolsPos);

    if (!reTools)
    {
        reTools = CreatePopupMenu();
        AppendMenuA(reTools, MF_STRING, 40948, "&Map Design...");
        AppendMenuA(reTools, MF_STRING, WorkflowTools::kBrushVisibility, "&Brush Visibility...");
        AppendMenuA(reTools, MF_STRING, MapOutliner::kOpenCommand, "Map &Outliner...");
        AppendMenuA(reTools, MF_STRING, StoreyFilter::kOpen, "S&toreys (one floor at a time)...");
        AppendMenuA(reTools, MF_STRING, LightShadowMap::kToggle, "Li&ght and Shadow Map");
        AppendMenuA(reTools, MF_STRING, LightShadowMap::kSettings, "Light and Shadow Settings...");
        AppendMenuA(reTools, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(reTools, MF_STRING, WorkflowTools::kConnections, "Gameplay &Connections...");
        AppendMenuA(reTools, MF_STRING, 40927, "SMagicEvent &Workbench...");
        AppendMenuA(reTools, MF_STRING, 40932, "SCamNetwork Ma&nager...");
        AppendMenuA(reTools, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(reTools, MF_STRING, WorkflowTools::kViews, "Working &Views...");
        // The keys work over a viewport (GridSizeShortcut.cpp); the labels only show them.
        if (HMENU bookmarks = CreatePopupMenu())
        {
            const UINT count = WorkflowTools::kBookmarkSetFirst - WorkflowTools::kBookmarkGoFirst;
            for (UINT slot = 1; slot <= count; ++slot)
            {
                const std::string n = std::to_string(slot);
                AppendMenuA(bookmarks, MF_STRING, WorkflowTools::kBookmarkGoFirst + slot - 1, ("Go to Bookmark &" + n + "\t" + n).c_str());
            }
            AppendMenuA(bookmarks, MF_SEPARATOR, 0, nullptr);
            for (UINT slot = 1; slot <= count; ++slot)
            {
                const std::string n = std::to_string(slot);
                AppendMenuA(bookmarks, MF_STRING, WorkflowTools::kBookmarkSetFirst + slot - 1, ("Set Bookmark " + n + "\tCtrl+" + n).c_str());
            }
            AppendMenuA(reTools, MF_POPUP, reinterpret_cast<UINT_PTR>(bookmarks), "Camera Boo&kmarks");
        }
        AppendMenuA(reTools, MF_STRING, WorkflowTools::kAssemblies, "Actor &Assemblies...");
        AppendMenuA(reTools, MF_STRING, WorkflowTools::kSaveAssembly, "&Save Selection as Assembly...");
        if (HMENU prefabs = CreatePopupMenu())
        {
            AppendMenuA(prefabs, MF_STRING, WorkflowTools::kPrefabSelectInstance, "Select &Instance");
            AppendMenuA(prefabs, MF_STRING, WorkflowTools::kPrefabSelectAll, "Select &All Instances");
            AppendMenuA(prefabs, MF_STRING, WorkflowTools::kPrefabUpdate, "&Update Prefab from This Instance...");
            AppendMenuA(prefabs, MF_STRING, WorkflowTools::kPrefabBreakLink, "&Break Link");
            AppendMenuA(reTools, MF_POPUP, reinterpret_cast<UINT_PTR>(prefabs), "Linked Pre&fabs");
        }
        AppendMenuA(reTools, MF_STRING, EmitterLibraryWindow::Command, "Emitter &Library...");
        AppendMenuA(reTools, MF_STRING, WorkflowTools::kSaveToEmitterLibrary, "Save Selected Emitters to Emitter L&ibrary...");
        AppendMenuA(reTools, MF_STRING, CharacterSkinsWindow::Command, "C&haracter Skins...");
        AppendMenuA(reTools, MF_STRING, CharacterSkinPresetsWindow::Command, "Character Skin &Presets...");
        AppendMenuA(reTools, MF_STRING, WorkflowTools::kAddConveyorBelt, "Add Con&veyor Belt");
        AppendMenuA(reTools, MF_STRING, ScatterTool::kOpen, "Scatter P&rops...");
        AppendMenuA(reTools, MF_STRING, MoverWorkbench::kOpen, "M&over Workbench...");
        AppendMenuA(reTools, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(reTools, MF_STRING, 40934, "&Export Map to JSON...");
        AppendMenuA(reTools, MF_STRING, 40935, "&Import Map from JSON...");
        AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(reTools), "RE+ &Tools");
    }

    UndoHistory::InstallMenu(bar); // Edit > Undo History, after Redo
    PlacementTools::InstallMenu(bar); // RE+ Tools > Placement

    HMENU view = SubMenuWithCommand(bar, 40065); // "Advanced Options" lives in View
    if (view)
    {
        // Remove the previous release's View > Reloaded Tools submenu if a
        // menu was rebuilt in-place. The commands now live under RE+ Tools.
        const int oldToolsPos = MenuPosByText(view, "Reloaded &Tools");
        if (oldToolsPos >= 0)
        {
            HMENU oldTools = GetSubMenu(view, oldToolsPos);
            RemoveMenu(view, oldToolsPos, MF_BYPOSITION);
            if (oldTools) DestroyMenu(oldTools);
        }
        int pos = MenuPosByCommand(view, 19004); // after "Show Actor Class Browser"
        if (pos >= 0 && MenuPosByCommand(view, 40067) < 0)
            InsertMenuA(view, pos + 1, MF_BYPOSITION | MF_STRING, 40067, "Show &Animation Browser");

        pos = MenuPosByCommand(view, 40065);     // after "Advanced Options"
        if (pos >= 0 && MenuPosByCommand(view, 40066) < 0)
            InsertMenuA(view, pos + 1, MF_BYPOSITION | MF_STRING, 40066, "RE+ Options\tF12");

        pos = MenuPosByCommand(view, 40066);     // after "RE+ Options"
        if (pos >= 0
            && MenuPosByCommand(view,
                                WindowDriftFix::kResetPropertyWindowsCommandId) < 0)
            InsertMenuA(view, pos + 1, MF_BYPOSITION | MF_STRING,
                        WindowDriftFix::kResetPropertyWindowsCommandId,
                        "Reset &Property Window Positions");

        pos = MenuPosByCommand(view, 19004);     // after "Show Actor Class Browser"
        if (MenuPosByCommand(view, MapOutliner::kOpenCommand) < 0)
            InsertMenuA(view, pos >= 0 ? pos + 1 : -1, MF_BYPOSITION | MF_STRING, MapOutliner::kOpenCommand, "Map &Outliner...");
    }
    FavoritesWindow::InstallMenu(bar); // View > Favorites, after the browsers


    HMENU file = GetSubMenu(bar, 0);
    if (file)
    {
        if (MenuPosByCommand(file, MapPackageDialog::Command) < 0)
            AppendMenuA(file, MF_STRING, MapPackageDialog::Command, "Package Map for &Sharing...");
        if (MenuPosByCommand(file, MapOptimiseDialog::Command) < 0)
        {
            const int sharing = MenuPosByCommand(file, MapPackageDialog::Command);
            InsertMenuA(file, sharing >= 0 ? sharing : -1, MF_BYPOSITION | MF_STRING, MapOptimiseDialog::Command,
                        "&Optimise Map Assets...");
        }
        // File starts with New, Open, then a separator in the stock editor.
        if (MenuPosByCommand(file, MapRecovery::kCommandId) < 0)
            InsertMenuA(file, 2, MF_BYPOSITION | MF_STRING,
                        MapRecovery::kCommandId,
                        "&Recover Compiled Map...");

        const int recoveryPos =
            MenuPosByCommand(file, MapRecovery::kCommandId);
        if (recoveryPos >= 0
            && MenuPosByCommand(file, MapRecovery::kOpenRecoveredCommandId) < 0)
            InsertMenuA(file, recoveryPos + 1,
                        MF_BYPOSITION | MF_STRING,
                        MapRecovery::kOpenRecoveredCommandId,
                        "Convert &Legacy Recovered Map...");
        MapRecovery::InstallMenu(file);
        ObjExportWindow::InstallMenu(file); // File > Export > Map..., Map to OBJ...

        // After the stock Import... (40074).
        const int importPos = MenuPosByCommand(file, 40074);
        if (MenuPosByCommand(file, ObjImportWindow::Command) < 0)
            InsertMenuA(file, importPos >= 0 ? importPos + 1 : -1, MF_BYPOSITION | MF_STRING, ObjImportWindow::Command, "Import O&BJ Mesh...");
    }

    HMENU lightingBuild = SubMenuWithCommand(bar, 40038);
    if (lightingBuild && MenuPosByCommand(lightingBuild, MapRecovery::kRecalculateLightingCommandId) < 0)
    {
        AppendMenuA(lightingBuild, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(lightingBuild, MF_STRING, MapRecovery::kRecalculateLightingCommandId, "&Recalculate Lighting...");
    }
    if (lightingBuild && MenuPosByCommand(lightingBuild, MapRecovery::kRecalculateSelectedLightingCommandId) < 0)
        AppendMenuA(lightingBuild, MF_STRING, MapRecovery::kRecalculateSelectedLightingCommandId, "Recalculate &Selected Lighting...");
    if (lightingBuild && MenuPosByCommand(lightingBuild, MapRecovery::kMatchSelectedLightingCommandId) < 0)
        AppendMenuA(lightingBuild, MF_STRING, MapRecovery::kMatchSelectedLightingCommandId, "Match Selected &BSP Lighting...");
    if (lightingBuild && MenuPosByCommand(lightingBuild, LightingBudget::kOpenCommand) < 0)
        AppendMenuA(lightingBuild, MF_STRING, LightingBudget::kOpenCommand, "Lighting B&udget...");
    if (lightingBuild && MenuPosByCommand(lightingBuild, MapCheck::kOpenCommand) < 0)
        AppendMenuA(lightingBuild, MF_STRING, MapCheck::kOpenCommand, "&Map Check...");
    if (lightingBuild && MenuPosByCommand(lightingBuild, Reachability::kToggle) < 0)
        AppendMenuA(lightingBuild, MF_STRING, Reachability::kToggle, "Reac&hability Map");
    if (lightingBuild && MenuPosByCommand(lightingBuild, RenderBudget::kOpenCommand) < 0)
        AppendMenuA(lightingBuild, MF_STRING, RenderBudget::kOpenCommand, "Re&nder Budget...");
    if (lightingBuild && MenuPosByCommand(lightingBuild, MinimapWindow::Command) < 0)
        AppendMenuA(lightingBuild, MF_STRING, MinimapWindow::Command, "Generate &Minimap...");
    // -UnlockPackages is what lets it save over the stock maps.
    if (RebuildAllMaps::Available())
    {
        HMENU build = SubMenuWithCommand(bar, 40038); // "&Build All" lives in Build
        if (build && MenuPosByCommand(build, 40902) < 0)
        {
            AppendMenuA(build, MF_SEPARATOR, 0, nullptr);
            AppendMenuA(build, MF_STRING, 40902, "Rebuild &All Maps...");
        }
    }

    HMENU brush = SubMenuWithCommand(bar, 40403); // "Scale..." lives in Brush
    if (brush && MenuPosByCommand(brush, 40911) < 0)
    {
        // Keep the direct action beside the stock Reset submenu.
        const int scalePos = MenuPosByCommand(brush, 40403);
        if (scalePos >= 0)
            InsertMenuA(brush, scalePos, MF_BYPOSITION | MF_STRING,
                        40911, "Rebuild Builder Brush as Default &Cube");
    }

    HMENU help = SubMenuWithCommand(bar, 40480); // "Unreal Developer Network" lives in Help
    if (help && MenuPosByCommand(help, 40900) < 0)
    {
        AppendMenuA(help, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(help, MF_STRING, 40900, RE_PLUS_NAME " on &GitHub");
        AppendMenuA(help, MF_STRING, 40901, "&Reloaded Editor Wiki (AllyPal)");
    }
    // At the top of Help: the search over every menu command (CommandPalette.cpp).
    if (help && MenuPosByCommand(help, CommandPalette::kOpen) < 0)
    {
        InsertMenuA(help, 0, MF_BYPOSITION | MF_STRING, CommandPalette::kOpen, "&Command Palette...\tCtrl+Shift+P");
        InsertMenuA(help, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
    }

    DrawMenuBar(frame);
}

static HWND g_reloadedFrame;
static BOOL CALLBACK FindFrameProc(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    HMENU bar = GetMenu(hwnd);
    if (bar && SubMenuWithCommand(bar, 40065)) // the frame that owns the View menu
    {
        g_reloadedFrame = hwnd;
        return FALSE;
    }
    return TRUE;
}

// The editor frame doesn't exist yet at DLL attach; wait for it, then inject once.
static DWORD WINAPI MenuInjectThread(LPVOID)
{
    for (int i = 0; i < 600; ++i) // up to ~60s
    {
        g_reloadedFrame = nullptr;
        EnumWindows(FindFrameProc, 0);
        if (g_reloadedFrame)
        {
            InjectReloadedMenuItems(g_reloadedFrame);
            // The menus were changed from this thread while the frame's own thread may be drawing
            // its menu bar. The modern looks draw the bar themselves (Modern Dark item by item), so
            // have the frame's thread measure and draw it again now that every menu is in.
            if (Appearance::Current() != Appearance::Theme::Classic)
                SetWindowPos(g_reloadedFrame, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            EditorExtras::Attach(g_reloadedFrame);
            return 0;
        }
        Sleep(100);
    }
    return 0;
}

void UI::Initialize()
{
    INSTALL_HOOKS;

    HANDLE h = CreateThread(nullptr, 0, MenuInjectThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}
