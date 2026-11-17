#include "pch.h"
#undef min
#undef max
#include "ScatterTool.h"
#include "ScatterModel.h"
#include "MeasureTool.h"
#include "StaticMeshBrowserFavorites.h"
#include "WindowPlacement.h"
#include "WorkflowEditor.h"
#include "logger.h"
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace ScatterTool
{
namespace
{
    using Workflow::Json;
    using Workflow::Vector;
    namespace Scatter = Workflow::Scatter;
    namespace Place = Workflow::Placement;
    namespace Editor = Workflow::Editor;

    enum : int
    {
        kList = 100, kAddBrowser, kFavorites, kAddFavorite, kRemove, kWeight,
        kByCount = 110, kCount, kByDensity, kDensity,
        kArea = 120, kRadius,
        kRandomYaw = 130, kRandomTilt, kAlign,
        kScaleMin = 140, kScaleMax, kSpacing,
        kSeed = 150, kReroll, kPreview,
        kStatus = 160, kPlace = 170, kClose
    };
    constexpr UINT_PTR kTimer = 1;
    constexpr const char* kOverlay = "scatter";
    constexpr const char* kPlacementKey = "ScatterProps";
    // FColor as a number, 0xAARRGGBB.
    constexpr uint32_t kPropColour = 0xFF40E0FF, kAreaColour = 0xFFFF9020;
    constexpr size_t kMaxLines = 6400;

    struct Mesh { std::string path; double weight = 1; Vector lo{}, hi{}; };
    std::vector<Mesh> meshes; // kept while the editor runs, so the window opens with the last list
    HWND dialog = nullptr;
    bool updating = false;      // fields written by the window itself
    bool previewPaused = false; // after Place, until something is edited
    std::string sticky;         // a result or error shown until something is edited
    DWORD lastEdit = 0;
    struct Plan { std::string key; Json request, area, result; };
    Plan plan;
    std::vector<MeasureTool::OverlayLine> shown;

    HWND Item(int id) { return GetDlgItem(dialog, id); }
    bool Checked(int id) { return SendMessageA(Item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    void SetChecked(int id, bool on) { SendMessageA(Item(id), BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }
    std::string Text(int id)
    {
        char text[1024] = {};
        GetWindowTextA(Item(id), text, sizeof(text));
        return text;
    }
    void SetText(int id, const std::string& text) { updating = true; SetWindowTextA(Item(id), text.c_str()); updating = false; }
    double Number(int id, const char* what)
    {
        const auto text = Text(id);
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        while (end && *end == ' ') ++end;
        if (text.empty() || !end || *end || !std::isfinite(value)) throw std::runtime_error(std::string("Enter a number for ") + what + ".");
        return value;
    }
    long long Whole(int id, const char* what)
    {
        const double value = Number(id, what);
        if (value != std::floor(value) || value < 0 || value > 4000000000.0) throw std::runtime_error(std::string("Enter a whole number for ") + what + ".");
        return static_cast<long long>(value);
    }
    std::string Format(double value)
    {
        char text[64];
        snprintf(text, sizeof(text), "%.4g", std::abs(value) < 0.00005 ? 0.0 : value);
        return text;
    }
    std::string Count(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }
    void SetStatus(const std::string& text) { if (dialog) SetWindowTextA(Item(kStatus), text.c_str()); }

    // ------------------------------------------------------------ mesh list

    void FillList()
    {
        HWND list = Item(kList);
        updating = true;
        ListView_DeleteAllItems(list);
        for (size_t i = 0; i < meshes.size(); ++i)
        {
            LVITEMA item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            std::string name = meshes[i].path;
            item.pszText = name.data();
            SendMessageA(list, LVM_INSERTITEMA, 0, reinterpret_cast<LPARAM>(&item));
            std::string weight = Format(meshes[i].weight);
            LVITEMA cell{};
            cell.iSubItem = 1;
            cell.pszText = weight.data();
            SendMessageA(list, LVM_SETITEMTEXTA, i, reinterpret_cast<LPARAM>(&cell));
        }
        updating = false;
    }
    std::vector<int> SelectedRows()
    {
        std::vector<int> rows;
        for (int i = -1; (i = ListView_GetNextItem(Item(kList), i, LVNI_SELECTED)) >= 0;) rows.push_back(i);
        return rows;
    }
    void AddMesh(const std::string& path)
    {
        if (path.empty()) throw std::runtime_error("Pick a mesh in the Static Mesh Browser first.");
        const auto info = Editor::ScatterMesh(path);
        const auto own = info.at("path").get<std::string>();
        for (const auto& m : meshes)
            if (_stricmp(m.path.c_str(), own.c_str()) == 0) throw std::runtime_error(own + " is already in the list.");
        meshes.push_back({ own, 1, info.at("lo").get<Vector>(), info.at("hi").get<Vector>() });
        FillList();
        ListView_SetItemState(Item(kList), static_cast<int>(meshes.size() - 1), LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    void FillFavorites()
    {
        HWND combo = Item(kFavorites);
        const auto previous = Text(kFavorites);
        SendMessageA(combo, CB_RESETCONTENT, 0, 0);
        for (const auto& path : StaticMeshBrowserFavorites::FavoritePaths())
            SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
        const auto at = SendMessageA(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(previous.c_str()));
        SendMessageA(combo, CB_SETCURSEL, at >= 0 ? at : 0, 0);
    }
    std::string ChosenFavorite()
    {
        HWND combo = Item(kFavorites);
        const auto at = SendMessageA(combo, CB_GETCURSEL, 0, 0);
        if (at < 0) throw std::runtime_error("There are no favourite meshes: star some in the Static Mesh Browser's Favorites first.");
        char text[1024] = {};
        if (SendMessageA(combo, CB_GETLBTEXTLEN, at, 0) >= static_cast<LRESULT>(sizeof(text))) throw std::runtime_error("That favourite's name is too long.");
        SendMessageA(combo, CB_GETLBTEXT, at, reinterpret_cast<LPARAM>(text));
        return text;
    }

    // ------------------------------------------------------------- the plan

    bool Circle() { return SendMessageA(Item(kArea), CB_GETCURSEL, 0, 0) == 1; }
    Json Request()
    {
        if (meshes.empty()) throw std::runtime_error("Add a static mesh: pick one in the Static Mesh Browser and press Add Browser Mesh, or add a favourite.");
        Json items = Json::array();
        for (const auto& m : meshes) items.push_back({ { "path", m.path }, { "weight", m.weight }, { "lo", m.lo }, { "hi", m.hi } });
        Json request = { { "items", items } };
        const bool byDensity = Checked(kByDensity);
        request["byDensity"] = byDensity;
        if (byDensity) request["density"] = Number(kDensity, "the density");
        else request["count"] = static_cast<int>(std::min<long long>(Whole(kCount, "the count"), 1000000));
        request["randomYaw"] = Checked(kRandomYaw);
        request["randomTilt"] = Checked(kRandomTilt);
        request["align"] = Checked(kAlign);
        request["scaleMin"] = Number(kScaleMin, "the smallest scale");
        request["scaleMax"] = Number(kScaleMax, "the largest scale");
        request["spacing"] = Number(kSpacing, "the spacing");
        request["seed"] = static_cast<uint64_t>(Whole(kSeed, "the seed"));
        request["circle"] = Circle();
        if (Circle()) request["radius"] = Number(kRadius, "the radius");
        // The model's own checks, so a typing slip shows before any tracing.
        Scatter::Settings settings;
        settings.byDensity = byDensity;
        settings.count = request.value("count", 20);
        settings.density = request.value("density", 1.0);
        settings.scaleMin = request["scaleMin"].get<double>(); settings.scaleMax = request["scaleMax"].get<double>(); settings.spacing = request["spacing"].get<double>();
        std::vector<Scatter::Item> check;
        for (const auto& m : meshes) check.push_back({ m.weight, { m.lo, m.hi } });
        Scatter::Validate(settings, check);
        return request;
    }
    // The plan for the window's values, worked out again when they, the area
    // or the map change; while values are being typed, once they settle.
    // Returns false while waiting for that.
    bool Refresh(bool now)
    {
        auto request = Request();
        auto area = Editor::ScatterArea(request.at("circle").get<bool>(), request.value("radius", 0.0));
        const std::string key = request.dump() + area.dump() + std::to_string(Editor::ContentRevision());
        if (key == plan.key && !plan.result.is_null()) return true;
        if (!now && GetTickCount() - lastEdit < 300) return false;
        plan = Plan{};
        request["area"] = area;
        auto result = Editor::ScatterPlan(request);
        plan = { key, request, area, result };
        Logger::log("Scatter Props: " + std::to_string(result.at("props").size()) + " of " + std::to_string(result.at("target").get<int>()) + " props, "
                    + std::to_string(result.at("tried").get<int>()) + " tried, " + std::to_string(result.at("obstacles").get<size_t>()) + " small meshes near, " + std::to_string(result.at("rays").get<size_t>()) + " rays, "
                    + std::to_string(result.at("ms").get<long long>()) + " ms");
        return true;
    }
    std::string Summary(const Json& result)
    {
        const auto placed = result.at("props").size();
        const int target = result.at("target"), noFloor = result.at("noFloor"), walls = result.at("walls"), crowded = result.at("crowded"), blocked = result.at("blocked"), outside = result.at("outside");
        std::string text = placed == static_cast<size_t>(target) ? Count(placed, "prop fits.", "props fit.")
                         : std::to_string(placed) + " of " + std::to_string(target) + " props fit: the room is full at this spacing or has little free floor.";
        std::vector<std::string> left;
        if (crowded) left.push_back(std::to_string(crowded) + " too close to another prop");
        if (blocked) left.push_back(std::to_string(blocked) + " on a mesh already there");
        if (walls) left.push_back(std::to_string(walls) + " in a wall");
        if (noFloor) left.push_back(std::to_string(noFloor) + " not resting on a floor");
        if (outside) left.push_back(std::to_string(outside) + " over the edge of the area");
        if (!left.empty())
        {
            text += " Tries left out: ";
            for (size_t i = 0; i < left.size(); ++i) text += (i ? ", " : "") + left[i];
            text += ".";
        }
        return text;
    }

    // ----------------------------------------------------------- preview

    void Show(std::vector<MeasureTool::OverlayLine> lines)
    {
        auto same = [&] {
            if (lines.size() != shown.size()) return false;
            for (size_t i = 0; i < lines.size(); ++i)
                for (int k = 0; k < 3; ++k)
                    if (lines[i].from[k] != shown[i].from[k] || lines[i].to[k] != shown[i].to[k] || lines[i].colour != shown[i].colour) return false;
            return true;
        };
        if (same()) return;
        shown = lines;
        try { MeasureTool::SetOverlay(kOverlay, lines); } catch (const std::exception&) {}
    }
    void Line(std::vector<MeasureTool::OverlayLine>& lines, const Vector& a, const Vector& b, uint32_t colour)
    {
        if (lines.size() >= kMaxLines) return;
        lines.push_back({ { a[0], a[1], a[2] }, { b[0], b[1], b[2] }, colour });
    }
    std::vector<MeasureTool::OverlayLine> Lines()
    {
        std::vector<MeasureTool::OverlayLine> lines;
        const auto& area = plan.area;
        if (area.at("circle").get<bool>())
        {
            const auto centre = area.at("centre").get<Scatter::Point>();
            const double r = area.at("radius"), z = area.at("z");
            for (int i = 0; i < 48; ++i)
            {
                const double a = 2 * Place::kPi * i / 48, b = 2 * Place::kPi * (i + 1) / 48;
                Line(lines, { centre[0] + r * std::cos(a), centre[1] + r * std::sin(a), z }, { centre[0] + r * std::cos(b), centre[1] + r * std::sin(b), z }, kAreaColour);
            }
            Line(lines, { centre[0] - 32, centre[1], z }, { centre[0] + 32, centre[1], z }, kAreaColour);
            Line(lines, { centre[0], centre[1] - 32, z }, { centre[0], centre[1] + 32, z }, kAreaColour);
        }
        // The builder brush draws its own outline.
        for (const auto& prop : plan.result.at("props"))
        {
            Place::Box box{ prop.at("lo").get<Vector>(), prop.at("hi").get<Vector>() };
            const auto c = Place::Corners(box);
            static const int edges[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
            for (const auto& e : edges) Line(lines, c[e[0]], c[e[1]], kPropColour);
        }
        return lines;
    }
    void Preview(bool now = false)
    {
        if (!dialog) return;
        try
        {
            const bool wanted = Checked(kPreview) && !previewPaused;
            if (!wanted && !now)
            {
                Show({});
                if (sticky.empty()) SetStatus(previewPaused ? "Change a value or Re-roll to scatter more." : "Preview is off: Place works out where the props go.");
                return;
            }
            if (!Refresh(now)) { if (sticky.empty()) SetStatus("Working out where the props go..."); return; }
            Show(wanted ? Lines() : std::vector<MeasureTool::OverlayLine>{});
            if (sticky.empty()) SetStatus(Summary(plan.result));
        }
        catch (const std::exception& e)
        {
            Show({});
            plan = Plan{};
            SetStatus(!sticky.empty() ? sticky : e.what());
        }
    }
    void PlaceProps()
    {
        Preview(true);
        if (plan.result.is_null()) throw std::runtime_error(Text(kStatus));
        const auto& props = plan.result.at("props");
        if (props.empty()) throw std::runtime_error("No prop found a free spot on the floor: " + Summary(plan.result));
        const auto summary = Summary(plan.result);
        const auto created = Editor::ScatterPlace(props);
        previewPaused = true;
        Show({});
        sticky = "Placed " + Count(created.size(), "prop", "props") + " in the Scatter group; they are selected, and one Undo removes them. Change a value or Re-roll to scatter more.";
        Logger::log("Scatter Props: " + sticky + " (" + summary + ")");
        SetStatus(sticky);
    }
    void Reroll()
    {
        uint64_t seed = 1;
        try { seed = static_cast<uint64_t>(Whole(kSeed, "the seed")); } catch (const std::exception&) {}
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        SetText(kSeed, std::to_string(Scatter::NextSeed(seed ^ static_cast<uint64_t>(now.QuadPart))));
    }
    void ApplyWeight()
    {
        const auto rows = SelectedRows();
        if (rows.empty()) return;
        const double weight = Number(kWeight, "the weight");
        if (!std::isfinite(weight) || weight < 0 || weight > 1000000) throw std::runtime_error("A weight is 0 or more.");
        for (int row : rows) if (row < static_cast<int>(meshes.size())) meshes[row].weight = weight;
        updating = true;
        for (int row : rows)
        {
            std::string text = Format(weight);
            LVITEMA cell{};
            cell.iSubItem = 1;
            cell.pszText = text.data();
            SendMessageA(Item(kList), LVM_SETITEMTEXTA, row, reinterpret_cast<LPARAM>(&cell));
        }
        updating = false;
    }
    void Enable()
    {
        const bool density = Checked(kByDensity);
        EnableWindow(Item(kCount), !density);
        EnableWindow(Item(kDensity), density);
        EnableWindow(Item(kRadius), Circle());
        EnableWindow(Item(kAlign), !Checked(kRandomTilt));
        const bool selected = !SelectedRows().empty();
        EnableWindow(Item(kRemove), selected);
        EnableWindow(Item(kWeight), selected);
    }
    void Edited()
    {
        if (updating) return;
        previewPaused = false;
        sticky.clear();
        lastEdit = GetTickCount();
        Enable();
        Preview();
    }

    // ------------------------------------------------------------- window

    HWND Control(HWND parent, const char* type, const char* text, DWORD style, int id, int x, int y, int width, int height, DWORD exStyle = 0)
    {
        HWND control = CreateWindowExA(exStyle, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
        SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return control;
    }
    HWND Edit(HWND parent, int id, const char* text, int x, int y, int width)
    {
        return Control(parent, "EDIT", text, WS_TABSTOP | ES_AUTOHSCROLL, id, x, y, width, 21, WS_EX_CLIENTEDGE);
    }
    void Build(HWND window)
    {
        int y = 8;
        Control(window, "STATIC", "Meshes to scatter (a mesh with a higher weight is picked more often)", 0, -1, 12, y, 400, 16);
        y += 18;
        HWND list = Control(window, WC_LISTVIEWA, "", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER, kList, 12, y, 396, 112);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT);
        LVCOLUMNA column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.cx = 316; column.pszText = const_cast<char*>("Static mesh");
        SendMessageA(list, LVM_INSERTCOLUMNA, 0, reinterpret_cast<LPARAM>(&column));
        column.cx = 56; column.pszText = const_cast<char*>("Weight");
        SendMessageA(list, LVM_INSERTCOLUMNA, 1, reinterpret_cast<LPARAM>(&column));
        y += 118;
        Control(window, "BUTTON", "Add &Browser Mesh", BS_PUSHBUTTON | WS_TABSTOP, kAddBrowser, 12, y, 120, 23);
        Control(window, "BUTTON", "&Remove", BS_PUSHBUTTON | WS_TABSTOP, kRemove, 138, y, 70, 23);
        Control(window, "STATIC", "Weight", 0, -1, 268, y + 4, 44, 18);
        Edit(window, kWeight, "1", 316, y + 1, 60);
        y += 30;
        Control(window, "STATIC", "Favorite", 0, -1, 12, y + 4, 50, 18);
        Control(window, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kFavorites, 66, y, 276, 240);
        Control(window, "BUTTON", "&Add", BS_PUSHBUTTON | WS_TABSTOP, kAddFavorite, 348, y - 1, 60, 23);
        y += 34;
        Control(window, "BUTTON", "&Count", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, kByCount, 12, y + 1, 60, 20);
        Edit(window, kCount, "20", 76, y, 56);
        Control(window, "BUTTON", "&Density", BS_AUTORADIOBUTTON, kByDensity, 150, y + 1, 70, 20);
        Edit(window, kDensity, "1", 222, y, 56);
        Control(window, "STATIC", "per 256 x 256 units", WS_GROUP, -1, 284, y + 4, 124, 18);
        y += 30;
        Control(window, "STATIC", "Area", 0, -1, 12, y + 4, 50, 18);
        HWND area = Control(window, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kArea, 66, y, 212, 120);
        SendMessageA(area, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Builder brush footprint"));
        SendMessageA(area, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Circle round the last click"));
        SendMessageA(area, CB_SETCURSEL, 0, 0);
        Control(window, "STATIC", "Radius", 0, -1, 290, y + 4, 44, 18);
        Edit(window, kRadius, "512", 336, y, 72);
        y += 32;
        Control(window, "BUTTON", "Random &yaw", BS_AUTOCHECKBOX | WS_TABSTOP, kRandomYaw, 12, y, 110, 20);
        Control(window, "BUTTON", "Random &pitch and roll too", BS_AUTOCHECKBOX | WS_TABSTOP, kRandomTilt, 130, y, 180, 20);
        y += 24;
        Control(window, "BUTTON", "A&lign to slope (sit flat on the floor under each prop)", BS_AUTOCHECKBOX | WS_TABSTOP, kAlign, 12, y, 340, 20);
        y += 30;
        Control(window, "STATIC", "Scale from", 0, -1, 12, y + 4, 60, 18);
        Edit(window, kScaleMin, "1", 76, y, 50);
        Control(window, "STATIC", "to", 0, -1, 132, y + 4, 16, 18);
        Edit(window, kScaleMax, "1", 150, y, 50);
        Control(window, "STATIC", "Spacing", 0, -1, 230, y + 4, 50, 18);
        Edit(window, kSpacing, "16", 284, y, 60);
        Control(window, "STATIC", "units", 0, -1, 350, y + 4, 40, 18);
        y += 30;
        Control(window, "STATIC", "Seed", 0, -1, 12, y + 4, 50, 18);
        Edit(window, kSeed, "1", 76, y, 90);
        Control(window, "BUTTON", "R&e-roll", BS_PUSHBUTTON | WS_TABSTOP, kReroll, 172, y - 1, 70, 23);
        y += 32;
        Control(window, "BUTTON", "Pre&view in the viewports", BS_AUTOCHECKBOX | WS_TABSTOP, kPreview, 12, y, 200, 20);
        y += 26;
        Control(window, "STATIC", "", SS_LEFT, kStatus, 12, y, 396, 58);
        y += 64;
        Control(window, "BUTTON", "&Place", BS_DEFPUSHBUTTON | WS_TABSTOP, kPlace, 232, y, 84, 26);
        Control(window, "BUTTON", "Close", BS_PUSHBUTTON | WS_TABSTOP, kClose, 324, y, 84, 26);
        SetChecked(kByCount, true);
        SetChecked(kRandomYaw, true);
        SetChecked(kPreview, true);
        updating = true;
        SetWindowTextA(GetDlgItem(window, kSeed), std::to_string(Scatter::NextSeed(GetTickCount())).c_str());
        updating = false;
    }
    void Fail(const std::exception& e)
    {
        sticky = e.what();
        SetStatus(sticky);
        MessageBeep(MB_ICONWARNING);
        Logger::log(std::string("Scatter Props: ") + e.what());
    }
    LRESULT CALLBACK DialogProc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        try
        {
            switch (message)
            {
            case WM_CREATE:
                dialog = window;
                Build(window);
                FillList();
                FillFavorites();
                Enable();
                SetTimer(window, kTimer, 400, nullptr);
                return 0;
            case WM_COMMAND:
            {
                const int id = LOWORD(w), code = HIWORD(w);
                if (id == kClose || id == IDCANCEL) { DestroyWindow(window); return 0; }
                if (id == kPlace || id == IDOK) { PlaceProps(); return 0; }
                if (id == kAddBrowser) { AddMesh(Editor::ScatterBrowserMesh()); Edited(); return 0; }
                if (id == kAddFavorite) { AddMesh(ChosenFavorite()); Edited(); return 0; }
                if (id == kRemove)
                {
                    auto rows = SelectedRows();
                    for (auto it = rows.rbegin(); it != rows.rend(); ++it) if (*it < static_cast<int>(meshes.size())) meshes.erase(meshes.begin() + *it);
                    FillList(); Edited();
                    return 0;
                }
                if (id == kReroll) { Reroll(); Edited(); return 0; }
                if (id == kFavorites)
                {
                    if (code == CBN_DROPDOWN) FillFavorites();
                    return 0;
                }
                if (id == kWeight && code == EN_CHANGE)
                {
                    if (updating) return 0;
                    try { ApplyWeight(); } catch (const std::exception& e) { SetStatus(e.what()); return 0; }
                    Edited();
                    return 0;
                }
                if (code == EN_CHANGE || code == CBN_SELCHANGE || code == BN_CLICKED) { Edited(); return 0; }
                break;
            }
            case WM_NOTIFY:
            {
                const auto* header = reinterpret_cast<const NMHDR*>(l);
                if (static_cast<int>(header->idFrom) == kList && header->code == LVN_ITEMCHANGED && !updating)
                {
                    const auto rows = SelectedRows();
                    if (!rows.empty() && rows.front() < static_cast<int>(meshes.size())) SetText(kWeight, Format(meshes[rows.front()].weight));
                    Enable();
                }
                if (static_cast<int>(header->idFrom) == kList && header->code == LVN_KEYDOWN && reinterpret_cast<const NMLVKEYDOWN*>(l)->wVKey == VK_DELETE)
                    SendMessageA(window, WM_COMMAND, kRemove, 0);
                break;
            }
            case WM_TIMER:
                if (w == kTimer) { Preview(); return 0; }
                break;
            case WM_CLOSE:
                DestroyWindow(window);
                return 0;
            case WM_DESTROY:
                KillTimer(window, kTimer);
                WindowPlacement::Remember(window, kPlacementKey);
                Show({});
                plan = Plan{};
                return 0;
            case WM_NCDESTROY:
                dialog = nullptr;
                break;
            }
        }
        catch (const std::exception& e) { Fail(e); return 0; }
        return DefWindowProcA(window, message, w, l);
    }
    void Open()
    {
        if (!dialog)
        {
            static bool registered = false;
            if (!registered)
            {
                INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES };
                InitCommonControlsEx(&controls);
                WNDCLASSA wc{};
                wc.lpfnWndProc = DialogProc; wc.hInstance = GetModuleHandle(nullptr);
                wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
                wc.lpszClassName = "ReloadedScatterProps";
                registered = RegisterClassA(&wc) != 0;
            }
            RECT size{ 0, 0, 420, 548 };
            AdjustWindowRectEx(&size, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
            HWND owner = Editor::MainWindow();
            CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, "ReloadedScatterProps", "Scatter Props", WS_CAPTION | WS_SYSMENU | WS_POPUP,
                            CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top, owner ? owner : GetActiveWindow(), nullptr, GetModuleHandle(nullptr), nullptr);
            if (!dialog) throw std::runtime_error("Cannot open the Scatter Props window.");
            if (!WindowPlacement::Restore(dialog, kPlacementKey))
            {
                POINT at{}; GetCursorPos(&at);
                SetWindowPos(dialog, nullptr, std::max(0L, at.x - 120), std::max(0L, at.y - 60), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
        }
        previewPaused = false;
        sticky.clear();
        lastEdit = 0;
        Enable();
        ShowWindow(dialog, SW_SHOW);
        SetForegroundWindow(dialog);
        Preview();
    }
}

bool HandleCommand(UINT command)
{
    if (command != kOpen) return false;
    try { Open(); }
    catch (const std::exception& e) { MessageBeep(MB_ICONWARNING); Logger::log(std::string("Scatter Props: ") + e.what()); }
    return true;
}
}
