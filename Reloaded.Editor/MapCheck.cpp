#include "pch.h"
#undef min
#undef max
#include "MapCheck.h"
#include "WindowPlacement.h"
#include "Appearance.h"
#include "MapCheckModel.h"
#include "MeasureTool.h"
#include "Reachability.h"
#include "ReachabilityModel.h"
#include "WorkflowEditor.h"
#include "logger.h"
#include <commctrl.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#pragma comment(lib, "comctl32.lib")

namespace MapCheck
{
namespace
{
const char kTitle[] = "Map Check";
const char kSection[] = "MapCheck";
const char kOverlay[] = "Map Check";

enum : int { kList = 100, kStatus, kRefresh, kSelect, kFrame, kIntro, kShowPath };

// Leak path colours (FColor, 0xAARRGGBB): the path, its inside end, its far end.
constexpr uint32_t kPathColour = 0xFFFF3030, kFromColour = 0xFF40FF40, kToColour = 0xFFFFE000;

std::string IniPath()
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    return (std::filesystem::path(exe).parent_path() / "Reloaded_Editor.ini").string();
}

// [MapCheck] in Reloaded_Editor.ini; the defaults are written once so the
// section can be found and edited.
Settings LoadSettings()
{
    const auto ini = IniPath();
    const Settings defaults;
    const struct { const char* key; std::string value; } keys[] = {
        {"GeometryTolerance", Units(defaults.geometryTolerance)},
        {"FarOutsideDistance", std::to_string(static_cast<int>(defaults.farOutside))},
        {"LeakGridCells", std::to_string(defaults.leakGridCells)},
        {"ShowZonesWithoutZoneInfo", defaults.zonesWithoutZoneInfo ? "1" : "0"},
        {"StockChecks", defaults.stockChecks ? "1" : "0"},
        {"FloatingMinGap", Units(defaults.floatMinGap)},
        {"FloatingMaxGap", Units(defaults.floatMaxGap)},
        {"CollisionTriangleLimit", std::to_string(defaults.collisionTriangleLimit)},
        {"CollisionBspNodeLimit", std::to_string(defaults.collisionNodeLimit)},
        {"PerPolygonTriangleCap", std::to_string(defaults.polyTriangleCap)},
        {"MalformedHulls", defaults.malformedHulls ? "1" : "0"}};
    for (const auto& key : keys)
    {
        char value[32] = {};
        GetPrivateProfileStringA(kSection, key.key, "", value, sizeof(value), ini.c_str());
        if (!value[0])
            WritePrivateProfileStringA(kSection, key.key, key.value.c_str(), ini.c_str());
    }
    auto number = [&](const char* key, double fallback)
    {
        char value[32] = {};
        GetPrivateProfileStringA(kSection, key, "", value, sizeof(value), ini.c_str());
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        return end != value && std::isfinite(parsed) ? parsed : fallback;
    };
    Settings s;
    s.geometryTolerance = number("GeometryTolerance", defaults.geometryTolerance);
    s.farOutside = number("FarOutsideDistance", defaults.farOutside);
    s.leakGridCells = static_cast<int>(number("LeakGridCells", defaults.leakGridCells));
    s.zonesWithoutZoneInfo = GetPrivateProfileIntA(kSection, "ShowZonesWithoutZoneInfo", 1, ini.c_str()) != 0;
    s.stockChecks = GetPrivateProfileIntA(kSection, "StockChecks", 1, ini.c_str()) != 0;
    // FloatingMaxGap 0 turns the floating mesh check off.
    s.floatMinGap = number("FloatingMinGap", defaults.floatMinGap);
    s.floatMaxGap = number("FloatingMaxGap", defaults.floatMaxGap);
    // Static mesh collision limits; 0 turns a check off.
    s.collisionTriangleLimit = static_cast<int>(number("CollisionTriangleLimit", defaults.collisionTriangleLimit));
    s.collisionNodeLimit = static_cast<int>(number("CollisionBspNodeLimit", defaults.collisionNodeLimit));
    s.polyTriangleCap = static_cast<int>(number("PerPolygonTriangleCap", defaults.polyTriangleCap));
    s.malformedHulls = GetPrivateProfileIntA(kSection, "MalformedHulls", 1, ini.c_str()) != 0;
    return ClampSettings(s);
}

struct Snapshot
{
    Report report;
    unsigned generation = 0;
    double seconds = 0;
};

Snapshot Measure()
{
    const auto settings = LoadSettings();
    const auto started = std::chrono::steady_clock::now();
    Snapshot snapshot;
    auto scene = Workflow::Editor::MapCheckScene(settings);
    if (settings.floatMaxGap > 0 && scene.bsp.Built()) scene.floors = Workflow::Editor::MapCheckFloorGaps(settings.floatMaxGap);
    snapshot.report = Analyse(scene, settings);
    // Where each team can walk (Reachability.h), from its last run on the map as it is.
    for (const auto& row : Reachability::MapCheckRows())
    {
        Problem p;
        p.severity = row.severity == 0 ? Severity::Error : row.severity == 1 ? Severity::Warning : Severity::Note;
        p.group = "Reachability";
        p.check = row.check;
        p.message = row.message;
        p.actors = row.actors;
        snapshot.report.problems.push_back(std::move(p));
    }
    std::stable_sort(snapshot.report.problems.begin(), snapshot.report.problems.end(), [](const Problem& a, const Problem& b) { return a.severity < b.severity; });
    snapshot.generation = Workflow::Editor::MapGeneration();
    snapshot.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto& report = snapshot.report;
    Logger::log("MapCheck: " + std::to_string(report.problems.size()) + " problem(s) (" + std::to_string(report.Count(Severity::Error)) + " errors, "
                + std::to_string(report.Count(Severity::Warning)) + " warnings) in " + std::to_string(scene.actors.size()) + " actors, "
                + std::to_string(scene.bsp.nodes.size()) + " nodes, " + std::to_string(report.zones) + " zones, " + std::to_string(scene.portals.size())
                + " portal surfaces, " + std::to_string(scene.meshes.size()) + " static meshes; stock entries " + std::to_string(scene.stock.size()));
    for (const auto& p : report.problems)
        Logger::log(std::string("MapCheck: ") + SeverityName(p.severity) + " [" + p.check + "] " + p.message);
    return snapshot;
}

HWND Control(HWND window, const char* type, const char* text, DWORD style, int id)
{
    auto control = CreateWindowExA(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    SendMessage(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}

void Row(HWND list, int row, LPARAM data, const std::vector<std::string>& cells)
{
    LVITEMA item{};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = row;
    item.lParam = data;
    item.pszText = const_cast<char*>(cells[0].c_str());
    SendMessageA(list, LVM_INSERTITEMA, 0, reinterpret_cast<LPARAM>(&item));
    for (int i = 1; i < static_cast<int>(cells.size()); ++i)
    {
        LVITEMA cell{};
        cell.iSubItem = i;
        cell.pszText = const_cast<char*>(cells[i].c_str());
        SendMessageA(list, LVM_SETITEMTEXTA, row, reinterpret_cast<LPARAM>(&cell));
    }
}

const char kIntroText[] =
    "Checks the map as last built: actors in the void (solid space, outside every zone), far outside the geometry or with their "
    "collision cut into BSP or a static mesh; zones that leak into each other or to the outside; zone portals that seal nothing; "
    "static meshes floating a little above the floor; static meshes whose collision crashes the game or does not work in it; "
    "objectives and pickups a team cannot walk to (Build > Reachability Map); and the editor's own Check Map for Errors. "
    "Build geometry first. "
    "Click a row to select what it is about, double-click to frame it; a leak path is drawn in the viewports. "
    "Settings: Reloaded_Editor.ini [MapCheck].";

struct Window
{
    HWND window = nullptr;
    Snapshot snapshot;
    bool measured = false;
    int lastRow = -1;
} check;

void SetStatus(const std::string& text)
{
    if (IsWindow(check.window)) SetDlgItemTextA(check.window, kStatus, text.c_str());
}

std::string ReportStatus()
{
    const auto& report = check.snapshot.report;
    char took[48] = "";
    if (check.snapshot.seconds >= 0.5) snprintf(took, sizeof(took), " in %.1f s", check.snapshot.seconds);
    const auto checked = std::to_string(report.actorsChecked) + " actors and " + std::to_string(report.zones) + " zone(s) checked" + took + ".";
    if (report.problems.empty()) return "No problems found; " + checked;
    return std::to_string(report.Count(Severity::Error)) + " error(s), " + std::to_string(report.Count(Severity::Warning)) + " warning(s), "
        + std::to_string(report.Count(Severity::Note)) + " note(s); " + checked;
}

std::string Selects(const Problem& p)
{
    std::string text;
    // A mesh used hundreds of times would make the cell unreadable.
    constexpr size_t kNamed = 6;
    for (size_t i = 0; i < p.actors.size() && i < kNamed; ++i)
    {
        const auto& path = p.actors[i];
        const auto dot = path.find_last_of('.');
        text += (text.empty() ? "" : ", ") + (dot == std::string::npos ? path : path.substr(dot + 1));
    }
    if (p.actors.size() > kNamed) text += " and " + std::to_string(p.actors.size() - kNamed) + " more";
    for (int surface : p.surfaces) text += (text.empty() ? "" : ", ") + std::string("surface ") + std::to_string(surface);
    if (!p.path.empty()) text += (text.empty() ? "" : "; ") + std::string("leak path");
    return text.empty() ? "-" : text;
}

void ClearPath() { MeasureTool::SetOverlay(kOverlay, {}); }

void ShowPath(const Problem& p)
{
    if (p.path.size() < 2 || !IsWindow(check.window) || IsDlgButtonChecked(check.window, kShowPath) != BST_CHECKED)
    {
        ClearPath();
        return;
    }
    std::vector<MeasureTool::OverlayLine> lines;
    auto line = [&](const Vec3& a, const Vec3& b, uint32_t colour)
    {
        lines.push_back({{a[0], a[1], a[2]}, {b[0], b[1], b[2]}, colour});
    };
    for (size_t i = 1; i < p.path.size(); ++i) line(p.path[i - 1], p.path[i], kPathColour);
    auto cross = [&](const Vec3& at, uint32_t colour)
    {
        for (size_t axis = 0; axis < 3; ++axis)
        {
            Vec3 a = at, b = at;
            a[axis] -= 24; b[axis] += 24;
            line(a, b, colour);
        }
    };
    cross(p.path.front(), kFromColour);
    cross(p.path.back(), kToColour);
    MeasureTool::SetOverlay(kOverlay, lines);
}

void Fill()
{
    auto list = GetDlgItem(check.window, kList);
    ListView_DeleteAllItems(list);
    const auto& report = check.snapshot.report;
    for (size_t i = 0; i < report.problems.size(); ++i)
    {
        const auto& p = report.problems[i];
        Row(list, static_cast<int>(i), static_cast<LPARAM>(i), {SeverityName(p.severity), p.group, p.check, p.message, Selects(p)});
    }
    SetStatus(ReportStatus());
}

void Refresh()
{
    ClearPath();
    auto old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    try
    {
        check.snapshot = Measure();
    }
    catch (...)
    {
        SetCursor(old);
        throw;
    }
    SetCursor(old);
    check.measured = true;
    check.lastRow = -1;
    if (IsWindow(check.window)) Fill();
}

void Choose(int row, bool frame)
{
    if (row < 0 || !check.measured) return;
    if (check.snapshot.generation != Workflow::Editor::MapGeneration())
    {
        Refresh();
        SetStatus("Another map was opened, so the map was checked again. Click the row again.");
        return;
    }
    LVITEMA item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!SendMessageA(GetDlgItem(check.window, kList), LVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&item))) return;
    const auto index = static_cast<size_t>(item.lParam);
    if (index >= check.snapshot.report.problems.size()) return;
    check.lastRow = row;
    const auto& p = check.snapshot.report.problems[index];
    ShowPath(p);
    if (p.actors.empty() && p.surfaces.empty())
    {
        SetStatus(p.path.empty() ? "This row has nothing to select." : "The leak path is drawn in the viewports.");
        return;
    }
    const auto found = Workflow::Editor::SelectMapCheckRow(p.actors, p.surfaces, frame);
    const auto wanted = p.actors.size() + p.surfaces.size();
    std::string text = "Selected " + Selects(p) + (frame && found ? ", framed in the viewports." : ".");
    if (found < wanted) text += " " + std::to_string(wanted - found) + " no longer exist; Refresh.";
    if (frame && p.actors.empty()) text += " Surfaces cannot be framed; look for the selected portal surface.";
    SetStatus(text);
}

void Layout(HWND window)
{
    RECT r{};
    GetClientRect(window, &r);
    const int w = r.right - 24, h = r.bottom;
    // The intro keeps all its lines in the larger modern font (up to a third of the window).
    const int intro = Appearance::FitHeight(GetDlgItem(window, kIntro), w, 62, h / 3);
    const int top = intro + 12;
    MoveWindow(GetDlgItem(window, kIntro), 12, 8, w, intro, TRUE);
    MoveWindow(GetDlgItem(window, kList), 12, top, w, h - top - 82, TRUE);
    MoveWindow(GetDlgItem(window, kStatus), 12, h - 76, w, 34, TRUE);
    MoveWindow(GetDlgItem(window, kRefresh), 12, h - 36, 90, 26, TRUE);
    MoveWindow(GetDlgItem(window, kSelect), 110, h - 36, 90, 26, TRUE);
    MoveWindow(GetDlgItem(window, kFrame), 208, h - 36, 130, 26, TRUE);
    MoveWindow(GetDlgItem(window, kShowPath), 352, h - 34, 200, 22, TRUE);
    MoveWindow(GetDlgItem(window, IDCANCEL), r.right - 102, h - 36, 90, 26, TRUE);
}

LRESULT CALLBACK CheckProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            Control(window, "STATIC", kIntroText, 0, kIntro);
            auto list = Control(window, WC_LISTVIEWA, "", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, kList);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
            const std::pair<const char*, int> columns[] = {{"Severity", 62}, {"Group", 84}, {"Check", 160}, {"Problem", 560}, {"Selects", 180}};
            for (int i = 0; i < 5; ++i)
            {
                LVCOLUMNA column{};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.cx = columns[i].second;
                column.pszText = const_cast<char*>(columns[i].first);
                SendMessageA(list, LVM_INSERTCOLUMNA, i, reinterpret_cast<LPARAM>(&column));
            }
            Control(window, "STATIC", "", 0, kStatus);
            Control(window, "BUTTON", "&Refresh", WS_TABSTOP, kRefresh);
            Control(window, "BUTTON", "&Select", WS_TABSTOP, kSelect);
            Control(window, "BUTTON", "Select and &Frame", WS_TABSTOP, kFrame);
            Control(window, "BUTTON", "Show &leak path in viewports", WS_TABSTOP | BS_AUTOCHECKBOX, kShowPath);
            CheckDlgButton(window, kShowPath, BST_CHECKED);
            Control(window, "BUTTON", "Close", WS_TABSTOP, IDCANCEL);
            Layout(window);
            return 0;
        }
        case WM_SIZE:
            Layout(window);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {640, 360};
            return 0;
        case WM_NOTIFY:
        {
            auto header = reinterpret_cast<NMHDR*>(l);
            if (header->idFrom == kList && (header->code == NM_CLICK || header->code == NM_DBLCLK))
            {
                auto activate = reinterpret_cast<NMITEMACTIVATE*>(l);
                Choose(activate->iItem, header->code == NM_DBLCLK);
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case kRefresh: Refresh(); return 0;
            case kSelect: case kFrame:
                if (check.lastRow < 0) SetStatus("Click a row first.");
                else Choose(check.lastRow, LOWORD(w) == kFrame);
                return 0;
            case kShowPath:
                if (check.lastRow >= 0) Choose(check.lastRow, false);
                else ClearPath();
                return 0;
            case IDCANCEL: DestroyWindow(window); return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            WindowPlacement::Remember(window, "MapCheck");
            ClearPath();
            check.window = nullptr;
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        // A modal box here would stall the in-editor tests; report in the status line.
        SetStatus(e.what());
        if (message == WM_CREATE) return -1;
        return 0;
    }
    return DefWindowProcA(window, message, w, l);
}

void Show(HWND owner)
{
    if (IsWindow(check.window))
    {
        ShowWindow(check.window, IsIconic(check.window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(check.window);
        Fill();
        return;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSA wc{};
    wc.lpfnWndProc = CheckProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "ReloadedMapCheck";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);
    check.window = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 1000, 560, owner, nullptr, wc.hInstance, nullptr);
    if (!check.window) throw std::runtime_error("Could not open the Map Check window.");
    WindowPlacement::Restore(check.window, "MapCheck");
    Fill();
    ShowWindow(check.window, SW_SHOW);
}
} // namespace

void Open(HWND owner)
{
    Refresh();
    Show(owner);
}

void ReachabilityChanged()
{
    if (!IsWindow(check.window)) return;
    try
    {
        Refresh();
    }
    catch (const std::exception& e)
    {
        SetStatus(e.what());
    }
}

bool HandleCommand(UINT command)
{
    if (command != kOpenCommand) return false;
    try
    {
        Open(GetActiveWindow());
    }
    catch (const std::exception& e)
    {
        // Open the window anyway so the reason shows in its status line, not a modal box.
        Logger::log(std::string("MapCheck: ") + e.what());
        try
        {
            check.measured = false;
            check.snapshot = {};
            Show(GetActiveWindow());
            SetStatus(e.what());
        }
        catch (const std::exception&) {}
    }
    return true;
}
}
