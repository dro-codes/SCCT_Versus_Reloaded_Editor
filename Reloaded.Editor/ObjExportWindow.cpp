#include "pch.h"
#undef min
#undef max
#include "ObjExportWindow.h"
#include "WindowPlacement.h"
#include "Appearance.h"
#include "WorkflowEditor.h"
#include <filesystem>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <string>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace ObjExportWindow
{
namespace
{
constexpr char kTitle[] = "Export Map to OBJ";
constexpr char kSection[] = "ObjExport";
HWND window = nullptr;

std::string IniPath()
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    return (std::filesystem::path(exe).parent_path() / "Reloaded_Editor.ini").string();
}
std::string Setting(const char* key, const std::string& fallback)
{
    char text[1024] = {};
    GetPrivateProfileStringA(kSection, key, fallback.c_str(), text, sizeof(text), IniPath().c_str());
    return text;
}
void Store(const char* key, const std::string& value) { WritePrivateProfileStringA(kSection, key, value.c_str(), IniPath().c_str()); }

std::string MapStem()
{
    const auto file = Workflow::Editor::MapFile();
    return file.empty() ? std::string("Map") : std::filesystem::path(file).stem().string();
}
// The folder last exported to is offered again. When it was named after the
// map then open and another map is open now, the new map's name takes its
// place; the first time it is Documents\SCCT Map Exports\<Map>.
std::string StartFolder()
{
    const auto map = MapStem();
    std::filesystem::path folder(Setting("Folder", ""));
    const auto last = Setting("Map", "");
    if (folder.empty())
    {
        PWSTR documents = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))) folder = std::filesystem::path(documents) / "SCCT Map Exports" / map;
        CoTaskMemFree(documents);
    }
    else if (!last.empty() && _stricmp(last.c_str(), map.c_str()) && !_stricmp(folder.filename().string().c_str(), last.c_str()))
        folder = folder.parent_path() / map;
    return folder.string();
}

HWND Control(const char* type, const char* text, DWORD style, int id, int x, int y, int w, int h)
{
    auto control = CreateWindowExA(0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    SendMessage(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}
std::string Text(int id)
{
    char text[1024] = {};
    GetDlgItemTextA(window, id, text, sizeof(text));
    return text;
}
bool Ticked(int id) { return IsDlgButtonChecked(window, id) == BST_CHECKED; }
void Status(const std::string& text) { SetDlgItemTextA(window, kStatus, text.c_str()); }

void SaveSettings()
{
    Store("Folder", Text(kFolder));
    Store("Map", MapStem());
    Store("Meshes", Ticked(kMeshes) ? "1" : "0");
    Store("Textures", Ticked(kTextures) ? "1" : "0");
    Store("SelectionOnly", Ticked(kSelection) ? "1" : "0");
    Store("Centimetres", Text(kScale));
}

void Browse()
{
    auto initialized = OleInitialize(nullptr);
    BROWSEINFOW browse{};
    browse.hwndOwner = window;
    browse.lpszTitle = L"Choose the folder to write the map's .obj, .mtl and textures into.";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    auto selected = SHBrowseForFolderW(&browse);
    wchar_t path[MAX_PATH]{};
    const bool picked = selected && SHGetPathFromIDListW(selected, path);
    CoTaskMemFree(selected);
    if (SUCCEEDED(initialized)) OleUninitialize();
    if (picked) SetDlgItemTextW(window, kFolder, path);
}

std::string Plural(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

void Export()
{
    double centimetres = 0;
    try { centimetres = std::stod(Text(kScale)); }
    catch (const std::exception&) { centimetres = 0; }
    if (!(centimetres > 0)) { Status("The scale must be a positive number: how many centimetres one Unreal unit is (1 by default)."); return; }
    const auto folder = Text(kFolder);
    if (folder.empty()) { Status("Choose a folder to export to."); return; }
    SaveSettings();
    Status("Exporting...");
    UpdateWindow(window);
    auto cursor = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    Workflow::Json result;
    try
    {
        result = Workflow::Editor::ExportMapObj({{"folder", folder}, {"meshes", Ticked(kMeshes)}, {"textures", Ticked(kTextures)},
                                                 {"selectedOnly", Ticked(kSelection)}, {"centimetres", centimetres}});
    }
    catch (const std::exception& e)
    {
        SetCursor(cursor);
        Status(std::string("Not exported: ") + e.what());
        return;
    }
    SetCursor(cursor);
    std::ostringstream text;
    text.precision(1);
    text << std::fixed << "Wrote " << std::filesystem::path(result.at("obj").get<std::string>()).filename().string() << ": "
         << Plural(result.at("polygons").get<size_t>(), "BSP polygon", "BSP polygons") << ", "
         << Plural(result.at("actors").get<size_t>(), "static mesh actor", "static mesh actors") << " ("
         << Plural(result.at("meshes").get<size_t>(), "mesh", "meshes") << "), "
         << Plural(result.at("materials").get<size_t>(), "material", "materials") << ", "
         << Plural(result.at("textures").get<size_t>(), "texture", "textures") << " in " << result.at("seconds").get<double>() << " s.";
    if (const auto failed = result.at("texturesFailed").get<size_t>())
        text << " " << Plural(failed, "texture", "textures") << " could not be read and show grey (listed in the editor log).";
    Status(text.str());
}

LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            window = hwnd;
            RECT r{};
            GetClientRect(hwnd, &r);
            const int width = r.right - 24;
            Control("STATIC",
                    "Writes the built level's BSP and its static meshes as <Map>.obj and <Map>.mtl, with the textures as PNGs, "
                    "into the folder below. The file is Z up: in Blender, File > Import > Wavefront (.obj) with Forward Axis Y "
                    "and Up Axis Z. Rebuild the geometry first so the BSP is current.",
                    0, kIntro, 12, 10, width, 56);
            int y = 74;
            Control("STATIC", "Folder:", 0, 0, 12, y + 3, 50, 20);
            Control("EDIT", StartFolder().c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, kFolder, 64, y, width - 150, 22);
            Control("BUTTON", "Browse...", WS_TABSTOP, kBrowse, r.right - 92, y - 1, 80, 25);
            y += 34;
            Control("BUTTON", "Static meshes", WS_TABSTOP | BS_AUTOCHECKBOX, kMeshes, 12, y, 150, 20);
            Control("BUTTON", "Textures (PNG)", WS_TABSTOP | BS_AUTOCHECKBOX, kTextures, 170, y, 150, 20);
            Control("BUTTON", "Selection only", WS_TABSTOP | BS_AUTOCHECKBOX, kSelection, 328, y, 150, 20);
            CheckDlgButton(hwnd, kMeshes, Setting("Meshes", "1") == "1" ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, kTextures, Setting("Textures", "1") == "1" ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hwnd, kSelection, Setting("SelectionOnly", "0") == "1" ? BST_CHECKED : BST_UNCHECKED);
            y += 30;
            Control("STATIC", "1 Unreal unit =", 0, 0, 12, y + 3, 90, 20);
            Control("EDIT", Setting("Centimetres", "1").c_str(), WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, kScale, 104, y, 60, 22);
            HWND metres = Control("STATIC", "cm (written in metres; 1 matches Blender's metres and the stock exports)", 0, 0, 170, y + 3, width - 158, 20);
            // Two lines in the larger modern font.
            const int metresHeight = Appearance::FitHeight(metres, width - 158, 20);
            MoveWindow(metres, 170, y + 3, width - 158, metresHeight, FALSE);
            y += 12 + metresHeight;
            Control("STATIC", "Selection only takes the selected static mesh actors and BSP surfaces.", 0, kStatus, 12, y, width, 48);
            Control("BUTTON", "&Export", WS_TABSTOP | BS_DEFPUSHBUTTON, kExport, r.right - 300, r.bottom - 36, 90, 27);
            Control("BUTTON", "&Open Folder", WS_TABSTOP, kOpenFolder, r.right - 202, r.bottom - 36, 92, 27);
            Control("BUTTON", "Close", WS_TABSTOP, IDCANCEL, r.right - 102, r.bottom - 36, 90, 27);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case kBrowse: Browse(); return 0;
            case kExport: case IDOK: Export(); return 0;
            case kOpenFolder:
            {
                const auto folder = std::filesystem::path(Text(kFolder));
                if (!std::filesystem::is_directory(folder)) { Status("The folder does not exist yet: export first."); return 0; }
                ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            case IDCANCEL: DestroyWindow(hwnd); return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            SaveSettings();
            WindowPlacement::Remember(hwnd, "ObjExport");
            window = nullptr;
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        // A modal box here would stall the in-editor tests; report in the status line.
        if (window) Status(e.what());
        if (message == WM_CREATE) return -1;
        return 0;
    }
    return DefWindowProcA(hwnd, message, w, l);
}
} // namespace

void Open(HWND owner)
{
    Workflow::Editor::LevelPath(); // A map must be open.
    if (IsWindow(window))
    {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        return;
    }
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSA wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "ReloadedObjExport";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassA(&wc);
        registered = true;
    }
    RECT rect{0, 0, 560, 270};
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
    auto created = CreateWindowExA(WS_EX_TOOLWINDOW, "ReloadedObjExport", kTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                   CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, owner, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!created) throw std::runtime_error("Could not open the Export Map to OBJ window.");
    WindowPlacement::Restore(created, "ObjExport");
    ShowWindow(created, SW_SHOW);
}

bool HandleCommand(UINT command)
{
    if (command != Command) return false;
    try { Open(GetActiveWindow()); }
    catch (const std::exception& e) { MessageBoxA(GetActiveWindow(), e.what(), kTitle, MB_OK | MB_ICONINFORMATION); }
    return true;
}

void InstallMenu(HMENU file)
{
    if (!file) return;
    const int count = GetMenuItemCount(file);
    for (int i = 0; i < count; ++i)
        if (HMENU sub = GetSubMenu(file, i))
            if (GetMenuState(sub, Command, MF_BYCOMMAND) != UINT(-1)) return; // installed already
    int stock = -1;
    for (int i = 0; i < count; ++i)
        if (GetMenuItemID(file, i) == 40017) stock = i;
    HMENU exportMenu = CreatePopupMenu();
    if (!exportMenu) return;
    if (stock >= 0)
    {
        // The stock File > Export... moves into the submenu, unchanged.
        AppendMenuA(exportMenu, MF_STRING, 40017, "&Map...");
        RemoveMenu(file, stock, MF_BYPOSITION);
    }
    AppendMenuA(exportMenu, MF_STRING, Command, "Map to &OBJ...");
    InsertMenuA(file, stock >= 0 ? stock : -1, MF_BYPOSITION | MF_POPUP, reinterpret_cast<UINT_PTR>(exportMenu), "&Export");
}
} // namespace ObjExportWindow
