#include "pch.h"
#undef min
#undef max
#include "LightingBudget.h"
#include "WindowPlacement.h"
#include "Appearance.h"
#include "LightingBudgetModel.h"
#include "BudgetWindow.h"
#include "RebuildAllMaps.h"
#include "WorkflowEditor.h"
#include "logger.h"
#include <commctrl.h>
#include <filesystem>
#include <set>
#include <stdexcept>
#pragma comment(lib, "comctl32.lib")

namespace LightingBudget
{
namespace
{
const char kTitle[] = "Lighting Budget";
const char kSection[] = "LightingBudget";

enum : int
{
    kZoneList = 100, kHotspotList, kStatus, kRefresh, kSelect, kFrame, kIntro,
    kMakeStatic = 120, kTurnOff, kWeakestCount, kWeakestLabel, kApply, kCancelFix,
    kBuildAnyway = 201, kOpenBudget, kQuiet, kPromptText
};

// [LightingBudget] in Reloaded_Editor.ini; the defaults are the stock checks'
// own limits and are written once so the section can be found and edited.
Thresholds LoadThresholds()
{
    const Thresholds defaults;
    Thresholds t;
    t.leafLights = Budget::IniInt(kSection, "MaxLightsPerLeaf", defaults.leafLights);
    t.overlap = Budget::IniInt(kSection, "MaxOverlappingLights", defaults.overlap);
    t.warnBeforeBuild = Budget::IniInt(kSection, "WarnBeforeBuild", defaults.warnBeforeBuild ? 1 : 0) != 0;
    t.warnLeafLights = Budget::IniInt(kSection, "WarnLightsPerLeaf", defaults.warnLeafLights);
    t.warnOverlap = Budget::IniInt(kSection, "WarnOverlappingLights", defaults.warnOverlap);
    return ClampThresholds(t);
}

struct Snapshot
{
    Report report;
    std::vector<Light> lights;
    std::vector<std::string> paths, names;
    std::string map;
    unsigned generation = 0;
};

Snapshot Measure(const Thresholds& thresholds)
{
    const auto scene = Workflow::Editor::LightingBudgetScene();
    Snapshot snapshot;
    std::vector<Light> lights;
    for (const auto& item : scene.at("lights"))
    {
        Light light;
        light.name = item.at("name").get<std::string>();
        light.zone = item.at("zone").get<int>();
        light.flags.type = item.at("type").get<uint8_t>();
        light.flags.effect = item.at("effect").get<uint8_t>();
        light.flags.staticFlag = item.at("static").get<bool>();
        light.flags.inGameFlag = item.at("inGame").get<bool>();
        light.flags.dynamicFlag = item.at("dynamic").get<bool>();
        light.flags.zoneLimited = item.at("zoneLimited").get<bool>();
        light.flags.animA = item.at("animA").get<float>();
        light.flags.animB = item.at("animB").get<float>();
        if (item.contains("position"))
        {
            const auto& p = item.at("position");
            light.x = p.at(0).get<double>(); light.y = p.at(1).get<double>(); light.z = p.at(2).get<double>();
            light.radius = item.at("radius").get<double>();
        }
        light.brightness = item.value("brightness", 0.0);
        light.lightRadius = item.value("lightRadius", 0.0);
        snapshot.paths.push_back(item.at("path").get<std::string>());
        snapshot.names.push_back(light.name);
        lights.push_back(light);
    }
    std::vector<Leaf> leaves;
    for (const auto& item : scene.at("leaves"))
    {
        Leaf leaf;
        leaf.zone = item.at("zone").get<int>();
        leaf.index = item.at("index").get<int>();
        leaf.lights = item.at("lights").get<std::vector<int>>();
        leaves.push_back(std::move(leaf));
    }
    std::map<int, std::string> zones;
    for (const auto& [number, name] : scene.at("zones").items())
        zones[std::stoi(number)] = name.get<std::string>();
    snapshot.report = Analyse(lights, leaves, zones, thresholds, scene.at("leavesKnown").get<bool>());
    snapshot.lights = lights;
    snapshot.map = scene.at("map").get<std::string>();
    snapshot.generation = scene.at("generation").get<unsigned>();
    const auto& map = snapshot.report.map;
    size_t over = 0;
    for (const auto& zone : snapshot.report.zones) over += zone.Over();
    Logger::log("LightingBudget: " + std::to_string(map.total) + " lights (" + std::to_string(map.inGame) + " in game) in "
                + std::to_string(snapshot.report.zones.size()) + " zones, " + std::to_string(leaves.size())
                + " lit leaves; worst leaf " + std::to_string(map.worstLeaf) + ", largest overlap " + std::to_string(map.overlap)
                + "; " + std::to_string(over) + " zone(s) over");
    return snapshot;
}

std::string Intro(const Thresholds& t)
{
    return "Only in-game lights cost frame time: InGame and Dynamic lights are drawn every frame, Static ones are baked by "
           "Build Lighting. Worst leaf: the most in-game lights reaching one BSP leaf of the zone. Overlap: the largest "
           "group of the zone's own in-game lights that all overlap one another. The editor's Tools > Check InGame / "
           "Dynamic Lights checks flag more than 4 per leaf and groups of 3; the renderer only stops at 256, so these "
           "are guidelines, which every shipped Versus map exceeds (they reach 44 and 33). Zones are flagged above " + std::to_string(t.leafLights) + " per leaf or "
           + std::to_string(t.overlap) + " overlapping; builds warn above " + std::to_string(t.warnLeafLights) + " or "
           + std::to_string(t.warnOverlap) + ". Change them in Reloaded_Editor.ini [LightingBudget]. Click a row to "
           "select its lights, double-click to frame them too; right-click it, or use the buttons, to make its in-game "
           "lights static or turn off the weakest.";
}

std::string Status(const ZoneReport& zone)
{
    if (!zone.Over()) return "OK";
    std::string text = "Over:";
    if (zone.overLeaf) text += " leaf";
    if (zone.overLeaf && zone.overOverlap) text += ",";
    if (zone.overOverlap) text += " overlap";
    return text;
}

// A fix waiting for Apply: what it changes and how the row stood before.
struct Pending
{
    FixPlan plan;
    std::string what;
    std::vector<std::string> paths, rowPaths;
    int zone = -2;              // the zone row to select again afterwards; -1 the whole map
    unsigned generation = 0;
};

// The budget window. One at a time; it keeps the last measurement.
struct Window
{
    HWND window = nullptr;
    Snapshot snapshot;
    bool measured = false;
    int lastList = 0, lastRow = -1;
    bool hasPending = false;
    Pending pending;
} budget;

// The lights of a row (indices into the snapshot's light list) and its name.
std::vector<int> RowLights(int list, LPARAM data, std::string& what, int* zoneNumber = nullptr)
{
    const auto& report = budget.snapshot.report;
    if (list == kZoneList)
    {
        const auto& zone = data < 0 ? report.map : report.zones.at(static_cast<size_t>(data));
        what = zone.name;
        if (zoneNumber) *zoneNumber = data < 0 ? -1 : zone.zone;
        return zone.lights;
    }
    const auto& hotspot = report.hotspots.at(static_cast<size_t>(data));
    std::string zone = "Zone " + std::to_string(hotspot.zone);
    for (const auto& z : report.zones) if (z.zone == hotspot.zone) zone = z.name;
    what = HotspotLabel(hotspot) + " in " + zone;
    if (zoneNumber) *zoneNumber = -2;
    return hotspot.lights;
}

std::vector<std::string> PathsOf(const std::vector<int>& lights)
{
    std::vector<std::string> paths;
    for (int light : lights) paths.push_back(budget.snapshot.paths.at(static_cast<size_t>(light)));
    return paths;
}

std::vector<std::string> RowPaths(int list, LPARAM data, std::string& what)
{
    return PathsOf(RowLights(list, data, what));
}

// How many of these lights count in game in the current snapshot.
int InGameCount(const std::vector<std::string>& paths)
{
    std::set<std::string> wanted;
    for (const auto& path : paths) wanted.insert(Workflow::Fold(path));
    int count = 0;
    for (size_t i = 0; i < budget.snapshot.paths.size(); ++i)
        if (wanted.count(Workflow::Fold(budget.snapshot.paths[i])) && CountsInGame(budget.snapshot.lights[i].flags)) ++count;
    return count;
}

void SetStatus(const std::string& text)
{
    if (IsWindow(budget.window)) SetDlgItemTextA(budget.window, kStatus, text.c_str());
}

std::string ReportStatus()
{
    const auto& report = budget.snapshot.report;
    size_t over = 0;
    for (const auto& zone : report.zones) over += zone.Over();
    std::string text = std::to_string(report.map.total) + " lights, " + std::to_string(report.map.inGame)
        + " drawn in game, in " + std::to_string(report.zones.size()) + " zone(s); " + std::to_string(over)
        + " zone(s) over budget.";
    text += report.leavesKnown ? " Leaf counts use the BSP as last built." : " The BSP has not been built, so leaf counts are unavailable.";
    if (!report.overlapExact) text += " The overlap search stopped early on this dense map; overlap figures are lower bounds.";
    return text;
}

void Layout(HWND window);

void Fill()
{
    auto zones = GetDlgItem(budget.window, kZoneList), hotspots = GetDlgItem(budget.window, kHotspotList);
    ListView_DeleteAllItems(zones);
    ListView_DeleteAllItems(hotspots);
    const auto& report = budget.snapshot.report;
    auto count = [](const ZoneReport& zone, Usage usage)
    {
        auto found = zone.usage.find(usage);
        return std::to_string(found == zone.usage.end() ? 0 : found->second);
    };
    auto zoneRow = [&](int row, LPARAM data, const ZoneReport& zone)
    {
        Budget::Row(zones, row, data, {zone.name, std::to_string(zone.total), std::to_string(zone.inGame),
            count(zone, Usage::Static), count(zone, Usage::InGame), count(zone, Usage::StaticInGame),
            count(zone, Usage::Dynamic), count(zone, Usage::Unflagged), count(zone, Usage::Off),
            report.leavesKnown ? std::to_string(zone.worstLeaf) : "-", std::to_string(zone.overlap), Status(zone)});
    };
    zoneRow(0, -1, report.map);
    for (size_t i = 0; i < report.zones.size(); ++i) zoneRow(static_cast<int>(i) + 1, static_cast<LPARAM>(i), report.zones[i]);
    for (size_t i = 0; i < report.hotspots.size(); ++i)
    {
        const auto& hotspot = report.hotspots[i];
        std::string zone = "Zone " + std::to_string(hotspot.zone), names;
        for (const auto& z : report.zones) if (z.zone == hotspot.zone) zone = z.name;
        for (int light : hotspot.lights) names += (names.empty() ? "" : ", ") + budget.snapshot.names.at(static_cast<size_t>(light));
        Budget::Row(hotspots, static_cast<int>(i), static_cast<LPARAM>(i), {HotspotLabel(hotspot), zone, std::to_string(hotspot.lights.size()), names});
    }
    SetDlgItemTextA(budget.window, kIntro, Intro(report.thresholds).c_str());
    Layout(budget.window); // the intro's height follows its text
    SetStatus(ReportStatus());
}

void ShowFix(bool pending)
{
    if (!IsWindow(budget.window)) return;
    for (int id : {kApply, kCancelFix}) EnableWindow(GetDlgItem(budget.window, id), pending);
}

void CancelFix(bool quiet = false)
{
    const bool had = budget.hasPending;
    budget.hasPending = false;
    ShowFix(false);
    if (had && !quiet) SetStatus("Fix cancelled; nothing changed.");
}

void Refresh()
{
    budget.snapshot = Measure(LoadThresholds());
    budget.measured = true;
    budget.lastRow = -1;
    CancelFix(true);
    if (IsWindow(budget.window)) Fill();
}

void Choose(int list, int row, bool frame)
{
    if (row < 0 || !budget.measured) return;
    if (budget.snapshot.generation != Workflow::Editor::MapGeneration())
    {
        Refresh();
        SetStatus("Another map was opened, so the budget was measured again. Click the row again.");
        return;
    }
    LPARAM data = 0;
    if (!Budget::RowData(GetDlgItem(budget.window, list), row, data)) return;
    budget.lastList = list;
    budget.lastRow = row;
    CancelFix(true);
    std::string what;
    const auto paths = RowPaths(list, data, what);
    const auto found = Workflow::Editor::SelectActorPaths(paths, frame);
    std::string text = "Selected " + std::to_string(found) + " light(s) in " + what + (frame && found ? ", framed in the viewports." : ".");
    if (found < paths.size()) text += " " + std::to_string(paths.size() - found) + " no longer exist; Refresh.";
    SetStatus(text);
}

int WeakestCount()
{
    BOOL ok = FALSE;
    const int count = IsWindow(budget.window) ? static_cast<int>(GetDlgItemInt(budget.window, kWeakestCount, &ok, FALSE)) : 0;
    return ok ? std::clamp(count, 1, 255) : 1;
}

// Plans a fix for the chosen row and asks for it in the status line; Apply
// carries it out. Nothing changes until then.
void ProposeFix(Fix fix)
{
    if (!budget.measured || budget.lastRow < 0) { SetStatus("Click a zone or a hotspot first."); return; }
    if (budget.snapshot.generation != Workflow::Editor::MapGeneration())
    {
        Refresh();
        SetStatus("Another map was opened, so the budget was measured again. Choose the row again.");
        return;
    }
    LPARAM data = 0;
    if (!Budget::RowData(GetDlgItem(budget.window, budget.lastList), budget.lastRow, data)) return;
    Pending pending;
    const auto lights = RowLights(budget.lastList, data, pending.what, &pending.zone);
    pending.plan = fix == Fix::MakeStatic ? PlanMakeStatic(budget.snapshot.lights, lights)
                                          : PlanTurnOffWeakest(budget.snapshot.lights, lights, WeakestCount());
    pending.paths = PathsOf(pending.plan.lights);
    pending.rowPaths = PathsOf(lights);
    pending.generation = budget.snapshot.generation;
    const auto question = Confirmation(pending.plan, pending.what, budget.snapshot.lights);
    budget.hasPending = !pending.plan.Empty();
    budget.pending = std::move(pending);
    ShowFix(budget.hasPending);
    SetStatus(budget.hasPending ? question + " Apply or Cancel." : question);
}

void ApplyFix()
{
    if (!budget.hasPending) { SetStatus("Choose Make Lights Static or Turn Off Weakest first."); return; }
    const auto pending = budget.pending;
    CancelFix(true);
    if (pending.generation != Workflow::Editor::MapGeneration())
    {
        Refresh();
        SetStatus("Another map was opened, so nothing was changed. Choose the row again.");
        return;
    }
    const int before = InGameCount(pending.rowPaths);
    const auto changed = Workflow::Editor::LightingBudgetFix(pending.paths, pending.plan.fix == Fix::TurnOff ? 1 : 0);
    Refresh();
    if (pending.zone >= -1)
    {
        // Keep the zone row chosen so the next fix applies to it.
        LPARAM data = -1;
        const auto& zones = budget.snapshot.report.zones;
        for (size_t i = 0; i < zones.size(); ++i) if (zones[i].zone == pending.zone) data = static_cast<LPARAM>(i);
        auto list = GetDlgItem(budget.window, kZoneList);
        if ((pending.zone == -1 || data >= 0) && Budget::SelectRowByData(list, data))
        {
            budget.lastList = kZoneList;
            budget.lastRow = ListView_GetNextItem(list, -1, LVNI_SELECTED);
        }
    }
    SetStatus(Done(pending.plan, changed, pending.what, before, InGameCount(pending.rowPaths)));
}

// Right-click on a row: the same actions as the buttons.
void RowMenu(HWND window, int list, int row)
{
    if (row < 0) return;
    auto control = GetDlgItem(window, list);
    ListView_SetItemState(control, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    budget.lastList = list;
    budget.lastRow = row;
    HMENU menu = CreatePopupMenu();
    AppendMenuA(menu, MF_STRING, kSelect, "&Select Lights");
    AppendMenuA(menu, MF_STRING, kFrame, "Select and &Frame");
    AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(menu, MF_STRING, kMakeStatic, "Make These Lights S&tatic...");
    const std::string weakest = "Turn Off the &Weakest " + std::to_string(WeakestCount()) + "...";
    AppendMenuA(menu, MF_STRING, kTurnOff, weakest.c_str());
    POINT at{};
    GetCursorPos(&at);
    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, 0, window, nullptr);
    DestroyMenu(menu);
    if (choice == kSelect || choice == kFrame) Choose(list, row, choice == kFrame);
    else if (choice == kMakeStatic) ProposeFix(Fix::MakeStatic);
    else if (choice == kTurnOff) ProposeFix(Fix::TurnOff);
}

void Layout(HWND window)
{
    RECT r{};
    GetClientRect(window, &r);
    const int w = r.right - 24, h = r.bottom;
    // The intro keeps all its lines in the larger modern font (up to a third of the window).
    const int intro = Appearance::FitHeight(GetDlgItem(window, kIntro), w, 70, h / 3);
    const int top = intro + 12, listsHeight = h - top - 48 - 34 - 34;
    const int zonesHeight = listsHeight * 3 / 5, hotspotTop = top + zonesHeight + 22;
    MoveWindow(GetDlgItem(window, kIntro), 12, 8, w, intro, TRUE);
    MoveWindow(GetDlgItem(window, kZoneList), 12, top, w, zonesHeight, TRUE);
    MoveWindow(GetDlgItem(window, 110), 12, top + zonesHeight + 4, w, 16, TRUE);
    MoveWindow(GetDlgItem(window, kHotspotList), 12, hotspotTop, w, h - 116 - hotspotTop, TRUE);
    // The fixes for the chosen row.
    MoveWindow(GetDlgItem(window, kMakeStatic), 12, h - 108, 130, 26, TRUE);
    MoveWindow(GetDlgItem(window, kTurnOff), 150, h - 108, 120, 26, TRUE);
    MoveWindow(GetDlgItem(window, kWeakestCount), 274, h - 106, 40, 22, TRUE);
    MoveWindow(GetDlgItem(window, kWeakestLabel), 320, h - 103, 80, 18, TRUE);
    MoveWindow(GetDlgItem(window, kApply), 408, h - 108, 80, 26, TRUE);
    MoveWindow(GetDlgItem(window, kCancelFix), 496, h - 108, 80, 26, TRUE);
    MoveWindow(GetDlgItem(window, kStatus), 12, h - 76, w, 34, TRUE);
    MoveWindow(GetDlgItem(window, kRefresh), 12, h - 36, 90, 26, TRUE);
    MoveWindow(GetDlgItem(window, kSelect), 110, h - 36, 110, 26, TRUE);
    MoveWindow(GetDlgItem(window, kFrame), 228, h - 36, 130, 26, TRUE);
    MoveWindow(GetDlgItem(window, IDCANCEL), r.right - 102, h - 36, 90, 26, TRUE);
}

LRESULT CALLBACK BudgetProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            Budget::Control(window, "STATIC", "", 0, kIntro, 0, 0, 0, 0);
            Budget::Columns(Budget::ReportList(window, kZoneList),
                {{"Zone", 170}, {"Lights", 50}, {"In game", 58}, {"Static", 50}, {"InGame", 55}, {"Static/InGame", 84},
                 {"Dynamic", 58}, {"Unflagged", 64}, {"Off", 36}, {"Worst leaf", 66}, {"Overlap", 56}, {"Status", 100}},
                {"Zone", "Status"});
            Budget::Control(window, "STATIC", "Hotspots over budget (worst first): BSP leaves and groups of overlapping in-game lights", 0, 110, 0, 0, 0, 0);
            Budget::Columns(Budget::ReportList(window, kHotspotList), {{"Hotspot", 110}, {"Zone", 160}, {"Lights", 50}, {"Light names", 520}},
                {"Zone", "Light names"});
            Budget::Control(window, "STATIC", "", 0, kStatus, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "&Refresh", WS_TABSTOP, kRefresh, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "&Select Lights", WS_TABSTOP, kSelect, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "Select and &Frame", WS_TABSTOP, kFrame, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "Make Lights S&tatic...", WS_TABSTOP, kMakeStatic, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "Turn Off &Weakest...", WS_TABSTOP, kTurnOff, 0, 0, 0, 0);
            Budget::Control(window, "EDIT", "1", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_RIGHT, kWeakestCount, 0, 0, 0, 0);
            SendDlgItemMessageA(window, kWeakestCount, EM_LIMITTEXT, 3, 0);
            Budget::Control(window, "STATIC", "light(s)", 0, kWeakestLabel, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "&Apply", WS_TABSTOP | WS_DISABLED, kApply, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "&Cancel", WS_TABSTOP | WS_DISABLED, kCancelFix, 0, 0, 0, 0);
            Budget::Control(window, "BUTTON", "Close", WS_TABSTOP, IDCANCEL, 0, 0, 0, 0);
            Layout(window);
            return 0;
        }
        case WM_SIZE:
            Layout(window);
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {640, 460};
            return 0;
        case WM_NOTIFY:
        {
            auto header = reinterpret_cast<NMHDR*>(l);
            if ((header->idFrom == kZoneList || header->idFrom == kHotspotList) && (header->code == NM_CLICK || header->code == NM_DBLCLK))
            {
                auto activate = reinterpret_cast<NMITEMACTIVATE*>(l);
                Choose(static_cast<int>(header->idFrom), activate->iItem, header->code == NM_DBLCLK);
            }
            else if ((header->idFrom == kZoneList || header->idFrom == kHotspotList) && header->code == NM_RCLICK)
                RowMenu(window, static_cast<int>(header->idFrom), reinterpret_cast<NMITEMACTIVATE*>(l)->iItem);
            else if ((header->idFrom == kZoneList || header->idFrom == kHotspotList) && header->code == LVN_ITEMCHANGED)
            {
                // A row chosen from the keyboard is the one the buttons act on.
                auto change = reinterpret_cast<NMLISTVIEW*>(l);
                if ((change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED)
                    && !(budget.lastList == static_cast<int>(header->idFrom) && budget.lastRow == change->iItem))
                {
                    budget.lastList = static_cast<int>(header->idFrom);
                    budget.lastRow = change->iItem;
                    CancelFix(true);
                }
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case kRefresh: Refresh(); return 0;
            case kSelect: case kFrame:
                if (budget.lastRow < 0) SetStatus("Click a zone or a hotspot first.");
                else Choose(budget.lastList, budget.lastRow, LOWORD(w) == kFrame);
                return 0;
            case kMakeStatic: ProposeFix(Fix::MakeStatic); return 0;
            case kTurnOff: ProposeFix(Fix::TurnOff); return 0;
            case kApply: ApplyFix(); return 0;
            case kCancelFix: CancelFix(); return 0;
            case IDCANCEL: DestroyWindow(window); return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            WindowPlacement::Remember(window, "LightingBudget");
            budget.window = nullptr;
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
    if (IsWindow(budget.window))
    {
        ShowWindow(budget.window, IsIconic(budget.window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(budget.window);
        Fill();
        return;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    Budget::RegisterClassOnce("ReloadedLightingBudget", BudgetProc);
    budget.window = CreateWindowExA(WS_EX_TOOLWINDOW, "ReloadedLightingBudget", kTitle, WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX,
                                    CW_USEDEFAULT, CW_USEDEFAULT, 900, 680, owner, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!budget.window) throw std::runtime_error("Could not open the Lighting Budget window.");
    WindowPlacement::Restore(budget.window, "LightingBudget");
    Fill();
    ShowWindow(budget.window, SW_SHOW);
}

// The pre-build warning: Build anyway / Open Lighting Budget / Cancel, and a
// "don't warn again for this map this session" box.
struct Prompt
{
    std::string text;
    int choice = IDCANCEL;
    bool quiet = false, done = false;
};

LRESULT CALLBACK PromptProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    auto prompt = reinterpret_cast<Prompt*>(GetWindowLongPtr(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        prompt = static_cast<Prompt*>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams);
        SetWindowLongPtr(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(prompt));
    }
    if (!prompt) return DefWindowProcA(window, message, w, l);
    switch (message)
    {
    case WM_CREATE:
    {
        RECT r{};
        GetClientRect(window, &r);
        Budget::Control(window, "STATIC", prompt->text.c_str(), 0, kPromptText, 14, 12, r.right - 28, r.bottom - 92);
        Budget::Control(window, "BUTTON", "Don't warn again for this map this session", WS_TABSTOP | BS_AUTOCHECKBOX, kQuiet, 14, r.bottom - 74, r.right - 28, 20);
        Budget::Control(window, "BUTTON", "&Build anyway", WS_TABSTOP | BS_DEFPUSHBUTTON, kBuildAnyway, r.right - 384, r.bottom - 40, 110, 28);
        Budget::Control(window, "BUTTON", "&Open Lighting Budget", WS_TABSTOP, kOpenBudget, r.right - 266, r.bottom - 40, 150, 28);
        Budget::Control(window, "BUTTON", "Cancel", WS_TABSTOP, IDCANCEL, r.right - 108, r.bottom - 40, 94, 28);
        SetFocus(GetDlgItem(window, kBuildAnyway));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == kBuildAnyway || LOWORD(w) == kOpenBudget || LOWORD(w) == IDCANCEL)
        {
            prompt->choice = LOWORD(w);
            prompt->quiet = IsDlgButtonChecked(window, kQuiet) == BST_CHECKED;
            prompt->done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        prompt->choice = IDCANCEL;
        prompt->done = true;
        return 0;
    }
    return DefWindowProcA(window, message, w, l);
}

int Ask(HWND owner, Prompt& prompt)
{
    WNDCLASSA wc{};
    wc.lpfnWndProc = PromptProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "ReloadedLightingBudgetPrompt";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);
    // Size the text area for the summary.
    RECT text{0, 0, 520, 0};
    if (auto dc = GetDC(nullptr))
    {
        auto old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        DrawTextA(dc, prompt.text.c_str(), -1, &text, DT_CALCRECT | DT_WORDBREAK);
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    RECT rect{0, 0, 548, std::min(560L, text.bottom + 112)};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT ownerRect{};
    GetWindowRect(owner ? owner : GetDesktopWindow(), &ownerRect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    auto window = CreateWindowExA(WS_EX_DLGMODALFRAME, wc.lpszClassName, "Lighting Budget - Build", style,
                                  (ownerRect.left + ownerRect.right - width) / 2, (ownerRect.top + ownerRect.bottom - height) / 2,
                                  width, height, owner, nullptr, wc.hInstance, &prompt);
    if (!window) return kBuildAnyway; // never block a build on a UI failure
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    MSG message{};
    bool quit = false;
    while (!prompt.done)
    {
        const auto got = GetMessageA(&message, nullptr, 0, 0);
        if (got <= 0) { quit = got == 0; break; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && (message.hwnd == window || IsChild(window, message.hwnd)))
        {
            prompt.done = true;
            prompt.choice = IDCANCEL;
            break;
        }
        if (!IsDialogMessageA(window, &message))
        {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(window);
    if (owner) SetActiveWindow(owner);
    if (quit) PostQuitMessage(static_cast<int>(message.wParam));
    return prompt.choice;
}

std::set<std::string> quietMaps;
} // namespace

void Open(HWND owner)
{
    Refresh();
    Show(owner);
}

bool HandleCommand(UINT command)
{
    if (command == kMakeStaticCommand || command == kTurnOffCommand || command == kApplyFixCommand || command == kCancelFixCommand)
    {
        // The fixes act on the window's chosen row; they report in its
        // status line, never in a message box.
        if (!IsWindow(budget.window)) return true;
        try
        {
            if (command == kMakeStaticCommand) ProposeFix(Fix::MakeStatic);
            else if (command == kTurnOffCommand) ProposeFix(Fix::TurnOff);
            else if (command == kApplyFixCommand) ApplyFix();
            else CancelFix();
        }
        catch (const std::exception& e)
        {
            SetStatus(e.what());
        }
        return true;
    }
    if (command != kOpenCommand) return false;
    try
    {
        Open(GetActiveWindow());
    }
    catch (const std::exception& e)
    {
        MessageBoxA(GetActiveWindow(), e.what(), kTitle, MB_OK | MB_ICONINFORMATION);
    }
    return true;
}

bool AllowBuild(UINT command)
{
    // Rebuild All Maps calls the build directly, but never prompt during it.
    if (RebuildAllMaps::Running()) return true;
    try
    {
        const auto thresholds = LoadThresholds();
        if (!thresholds.warnBeforeBuild) return true;
        auto snapshot = Measure(ForBuildWarning(thresholds));
        if (!snapshot.report.Over()) return true;
        const auto key = snapshot.map.empty() ? "untitled#" + std::to_string(snapshot.generation) : FoldKey(snapshot.map);
        if (quietMaps.count(key)) return true;
        Prompt prompt;
        const char* build = command == kBuildAll ? "Build All" : command == kRebuildLighting ? "Rebuild Lighting Only" : "Rebuild Changed Lighting Only";
        prompt.text = Summary(snapshot.report) + "\r\n\r\nRun " + build + " anyway?";
        auto owner = GetActiveWindow();
        const int choice = Ask(owner, prompt);
        if (prompt.quiet) quietMaps.insert(key);
        Logger::log("LightingBudget: over budget before " + std::string(build) + "; "
                    + (choice == kBuildAnyway ? "built anyway" : choice == kOpenBudget ? "opened the budget" : "cancelled"));
        if (choice == kOpenBudget)
        {
            // The window flags zones against the guideline, not the warning's limits.
            Refresh();
            Show(owner);
            return false;
        }
        return choice == kBuildAnyway;
    }
    catch (const std::exception& e)
    {
        // The check must never stand in the way of a build.
        Logger::log(std::string("LightingBudget: build check skipped: ") + e.what());
        return true;
    }
}
}
