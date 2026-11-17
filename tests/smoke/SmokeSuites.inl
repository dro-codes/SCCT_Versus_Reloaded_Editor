// Smaller suites, chosen with suite=<name>[,<name>...] in -Extra (run_smoke.ps1 -Extra @{ suite = 'storeys' }).
// Each one expects the map from smoke.ini to be open, from Open Recent.

HWND TopOfClass(const char* cls)
{
    struct S { const char* c; HWND r; } s{cls, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM p) -> BOOL {
        auto s = reinterpret_cast<S*>(p);
        char c[128] = {};
        GetClassNameA(w, c, sizeof(c));
        if (!_stricmp(c, s->c)) { s->r = w; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.r;
}
// File > Open Recent's first entry again, keeping (Yes) or not the open map's changes when asked.
void ReopenRecent(int answer = IDYES)
{
    boxAnswer = answer;
    Command(40970);
    boxAnswer = IDOK;
    Pump(2500);
}

// ---- Storeys: stepping to a floor hides the others in the viewports only. A save must not write
// that view into the map: reopened, nothing is hidden that was not hidden before.
void StoreysSuite()
{
    const auto map = Ini("map");
    const auto folder = std::filesystem::path(map).parent_path();
    const int hiddenAtStart = Hidden();
    Command(40966);
    Pump(2000);
    HWND palette = TopOfClass("ReloadedStoreyFilter");
    Check(palette != nullptr, "storeys-open", "hidden=" + std::to_string(hiddenAtStart));
    if (!palette) return;
    SendMessageA(palette, WM_KEYDOWN, VK_NEXT, 0);
    Pump(500);
    const int onFloor = Hidden();
    Check(onFloor > hiddenAtStart, "storeys-floor-hides", "hidden " + std::to_string(hiddenAtStart) + "->" + std::to_string(onFloor));

    // File > Save, in place (the map came from Open Recent), then the map opened again from Open Recent.
    Command(40007);
    Pump(500);
    Check(Hidden() == onFloor, "storeys-save-keeps-floor", "hidden after save=" + std::to_string(Hidden()) + " boxes=" + std::to_string(boxes.size()));
    ReopenRecent(IDYES);
    const int reopened = Hidden();
    Check(reopened == hiddenAtStart, "storeys-saved-map-not-hidden", "hidden=" + std::to_string(reopened) + " (at start " + std::to_string(hiddenAtStart) + ")");

    // The palette follows the reopened map; step down a floor again.
    Command(40966);
    Pump(1500);
    SendMessageA(palette, WM_KEYDOWN, VK_NEXT, 0);
    Pump(500);
    const int again = Hidden();
    Info("storeys-floor-again", "hidden=" + std::to_string(again));

    // The stock autosave's own command, into a spare slot.
    const auto autosave = folder / "Auto7.sdc";
    std::error_code ignored;
    std::filesystem::remove(autosave, ignored);
    Exec("MAP SAVE AUTOSAVE=1 FILE=\"" + autosave.string() + "\"");
    Pump(500);
    Check(Hidden() == again && std::filesystem::exists(autosave), "storeys-autosave-keeps-floor", "hidden after autosave=" + std::to_string(Hidden()));

    // Save As to a copy.
    const auto copy = folder / "StoreysCopy.sdc";
    std::filesystem::remove(copy, ignored);
    int ticks = 0;
    dialogHandler = [&](HWND w, const std::string& title) {
        char cls[64] = {};
        GetClassNameA(w, cls, sizeof(cls));
        if (strcmp(cls, "#32770") || IsMessageBox(w)) return false;
        Info("save-as-dialog", title);
        return AnswerFile(w, copy.wstring(), ticks);
    };
    Command(40008);
    dialogHandler = nullptr;
    Pump(500);
    Check(std::filesystem::exists(copy) && Hidden() == again, "storeys-save-as-keeps-floor", "hidden after save as=" + std::to_string(Hidden()) + " boxes=" + std::to_string(boxes.size()));

    // Reopened, each file shows every storey.
    auto hiddenIn = [&](const std::filesystem::path& file) {
        Exec("MAP LOAD FILE=\"" + file.string() + "\"");
        Pump(2500);
        return Hidden();
    };
    if (std::filesystem::exists(autosave))
    {
        const int n = hiddenIn(autosave);
        Check(n == hiddenAtStart, "storeys-autosave-file-not-hidden", "hidden=" + std::to_string(n) + " (at start " + std::to_string(hiddenAtStart) + ")");
    }
    if (std::filesystem::exists(copy))
    {
        const int n = hiddenIn(copy);
        Check(n == hiddenAtStart, "storeys-save-as-file-not-hidden", "hidden=" + std::to_string(n) + " (at start " + std::to_string(hiddenAtStart) + ")");
    }
    if ((palette = TopOfClass("ReloadedStoreyFilter")) != nullptr) { PostMessage(palette, WM_CLOSE, 0, 0); Pump(300); }
}

// ---- Unsaved changes: Open Recent warns exactly when the open map has edits its file does not.
// Every probe of the warning answers No, so a warning keeps the map open and no warning opens it.
bool WarnsBeforeOpenRecent()
{
    ReopenRecent(IDNO);
    return BoxContains("Unsaved changes");
}
void ExpectWarning(bool expected, const std::string& name, bool also = true, const std::string& detail = "")
{
    const bool warned = WarnsBeforeOpenRecent();
    Check(also && warned == expected, name, std::string(warned ? "warned" : "no warning") + " boxes=" + std::to_string(boxes.size()) + (detail.empty() ? "" : " " + detail));
}
// One edit: Hide selected on the first visible free mesh, an Undo step of its own.
void EditOnce()
{
    SelectOnly(FreeMeshes(1));
    Command(40957);
    Exec("SELECT NONE");
    Pump(200);
}
// The frame's File > Open, found by its text.
UINT FileOpenCommand()
{
    HMENU bar = GetMenu(frameWindow);
    HMENU file = bar ? GetSubMenu(bar, 0) : nullptr;
    for (int i = 0; file && i < GetMenuItemCount(file); ++i)
    {
        char text[256] = {};
        GetMenuStringA(file, i, text, sizeof(text), MF_BYPOSITION);
        std::string plain;
        for (char* c = text; *c && *c != '\t'; ++c) if (*c != '&') plain += *c;
        const UINT id = GetMenuItemID(file, i);
        if (plain.rfind("Open", 0) == 0 && plain.find("Recent") == std::string::npos && plain.find("Backup") == std::string::npos
            && plain.find("Autosave") == std::string::npos && id != UINT(-1))
        {
            Info("file-open", plain + " id=" + std::to_string(id));
            return id;
        }
    }
    return 0;
}
// Answers the next file dialog with this path (Save As, File > Open), or cancels it.
void AnswerFileDialogs(const std::filesystem::path& path, bool cancel)
{
    static int ticks;
    ticks = 0;
    dialogHandler = [path, cancel](HWND w, const std::string& title) {
        char cls[64] = {};
        GetClassNameA(w, cls, sizeof(cls));
        if (strcmp(cls, "#32770") || IsMessageBox(w)) return false;
        if (cancel) { if (++ticks == 2) { Info("cancel-file-dialog", title); PostMessage(w, WM_COMMAND, IDCANCEL, 0); } return true; }
        return AnswerFile(w, path.wstring(), ticks);
    };
}

void UnsavedSuite()
{
    const auto map = std::filesystem::path(Ini("map"));
    const auto other = std::filesystem::path(Ini("map2"));
    ExpectWarning(false, "unsaved-open-recent-clean");

    EditOnce();
    ExpectWarning(true, "unsaved-edit-warns");
    ExpectWarning(true, "unsaved-still-warns-after-no");

    // The stock autosave writes a copy: still unsaved.
    Exec("MAP SAVE AUTOSAVE=1 FILE=\"" + (map.parent_path() / "Auto6.sdc").string() + "\"");
    Pump(300);
    ExpectWarning(true, "unsaved-autosave-is-not-a-save");

    // File > Save As, cancelled.
    AnswerFileDialogs({}, true);
    Command(40008);
    dialogHandler = nullptr;
    ExpectWarning(true, "unsaved-save-as-cancelled");

    // File > Save refused: the map file is read-only.
    SetFileAttributesW(map.c_str(), FILE_ATTRIBUTE_READONLY);
    boxAnswer = IDNO;
    Command(40007);
    boxAnswer = IDOK;
    std::string refusal;
    for (auto& b : boxes) refusal += b + " ";
    SetFileAttributesW(map.c_str(), FILE_ATTRIBUTE_NORMAL);
    Info("unsaved-refused-save", refusal);
    ExpectWarning(true, "unsaved-save-refused");

    // Ctrl+L's route: the stock FileSave, called directly rather than from the menu.
    EditOnce();
    using FileSave = int(__cdecl*)(HWND);
    boxes.clear();
    const UINT_PTR answering = SetTimer(nullptr, 0, 100, AnswerBoxes);
    const int saved = reinterpret_cast<FileSave>(0x10E03EFA)(frameWindow);
    KillTimer(nullptr, answering);
    Pump(500);
    ExpectWarning(false, "unsaved-file-save-direct-is-a-save", saved != 0, "saved=" + std::to_string(saved));

    // Open Recent opened the map again (no warning); edit, then File > Save from the menu.
    EditOnce();
    Command(40007);
    Pump(500);
    ExpectWarning(false, "unsaved-menu-save-is-a-save");

    // Save As under a new name: clean under that name.
    EditOnce();
    const auto copy = map.parent_path() / "UnsavedCopy.sdc";
    std::error_code ignored;
    std::filesystem::remove(copy, ignored);
    AnswerFileDialogs(copy, false);
    Command(40008);
    dialogHandler = nullptr;
    Pump(500);
    ExpectWarning(false, "unsaved-save-as-is-a-save", std::filesystem::exists(copy), "copy=" + std::to_string(std::filesystem::exists(copy)));

    // The stock File > Open (its "Save changes?" answered No): clean as opened, and an edit straight
    // after it counts even once the ten-second check has seen the new map.
    const UINT open = FileOpenCommand();
    if (!open || other.empty()) { Fail("unsaved-stock-open", "no File > Open or no map2"); return; }
    auto stockOpen = [&](const std::filesystem::path& file) {
        AnswerFileDialogs(file, false);
        boxAnswer = IDNO;
        Command(open);
        boxAnswer = IDOK;
        dialogHandler = nullptr;
        Pump(500);
        const bool now = WindowText(frameWindow).find(file.filename().string()) != std::string::npos;
        Info("unsaved-stock-opened", WindowText(frameWindow) + " actors=" + std::to_string(Actors().size()));
        return now;
    };
    const bool opened = stockOpen(other);
    Pump(11000);
    ExpectWarning(false, "unsaved-stock-open-clean", opened);
    const bool back = stockOpen(map);
    EditOnce();
    Pump(11000);
    ExpectWarning(true, "unsaved-stock-open-edit-counts", back);
}

// ---- Window placement: each RE+ window opens where it was closed, maximised if it was, and a place
// off every monitor is ignored.
std::string PlacementEntry(const char* key)
{
    char text[256] = {};
    GetPrivateProfileStringA("WindowPlacement", key, "", text, sizeof(text), (directory / "Reloaded_Editor.ini").string().c_str());
    return text;
}
std::string RectText(const RECT& r)
{
    return std::to_string(r.left) + "," + std::to_string(r.top) + " " + std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top);
}
bool SameRect(const RECT& a, const RECT& b) { return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom; }
// The newest visible top-level window of this thread whose title starts with `title`.
HWND OpenedWindow(const char* title)
{
    struct S { const char* t; HWND r; } s{title, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM p) -> BOOL {
        auto s = reinterpret_cast<S*>(p);
        if (IsWindowVisible(w) && WindowText(w).rfind(s->t, 0) == 0) { s->r = w; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.r;
}
RECT NormalRect(HWND w)
{
    WINDOWPLACEMENT p{sizeof(p)};
    GetWindowPlacement(w, &p);
    RECT r = p.rcNormalPosition;
    // Workspace coordinates for a window that is not a tool window.
    if (!(GetWindowLongA(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
    {
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoA(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
        OffsetRect(&r, info.rcWork.left - info.rcMonitor.left, info.rcWork.top - info.rcMonitor.top);
    }
    return r;
}
void CloseWindowAndWait(HWND w)
{
    PostMessage(w, WM_CLOSE, 0, 0);
    for (int i = 0; i < 30 && IsWindow(w); ++i) Pump(100);
}

struct PlacedWindow { const char* name; UINT command; const char* title; const char* key; HWND (*open)(); };
HWND OpenFromConnections()
{
    HWND list = OpenedWindow("Gameplay Connections");
    if (!list) { Command(40920); list = OpenedWindow("Gameplay Connections"); }
    if (!list) return nullptr;
    // Graph... The graph is owned by the list, so the list stays open until the graph is done.
    Command(203, list);
    return OpenedWindow("Gameplay Connection Graph");
}

// Find Usages, as the Static Mesh Browser's right-click menu opens it, for a mesh in the map.
HWND OpenFindUsages()
{
    auto meshes = FreeMeshes(1);
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    auto property = meshes.empty() ? nullptr : FindProperty(meshes[0], "StaticMesh");
    if (!editor || !property) return nullptr;
    // The browser's current mesh (GEditor+0x13c), as a click in it sets it.
    *reinterpret_cast<void**>(editor + 0x13c) = *reinterpret_cast<void**>(meshes[0] + *reinterpret_cast<unsigned*>(property + 0x3C));
    HWND search = FindControlAnywhere(41016);
    for (HWND w = search ? GetParent(search) : nullptr; w; w = GetParent(w))
    {
        Command(40925, w);
        Pump(300);
        if (HWND found = OpenedWindow("Asset Find and Replace")) return found;
    }
    return nullptr;
}

void PlacementSuite()
{
    // A free mesh selected, so Find Usages has a mesh to look for.
    SelectOnly(FreeMeshes(1));
    const PlacedWindow windows[] = {
        {"map-check", 41220, "Map Check", "MapCheck", nullptr},
        {"lighting-budget", 41150, "Lighting Budget", "LightingBudget", nullptr},
        {"render-budget", 41280, "Render Budget", "RenderBudget", nullptr},
        {"undo-history", 41105, "Undo History", "UndoHistory", nullptr},
        {"storeys", 40966, "Storeys", "Storeys", nullptr},
        {"emitter-library", 40967, "Emitter Library", "EmitterLibrary", nullptr},
        {"magic-event-workbench", 40927, "SMagicEvent Workbench", "MagicEventWorkbench", nullptr},
        {"camera-networks", 40932, "SCamNetwork Manager", "CameraNetworks", nullptr},
        {"gameplay-connections", 40920, "Gameplay Connections", "GameplayConnections", nullptr},
        {"working-views", 40921, "Working Views", "WorkingViews", nullptr},
        {"actor-assemblies", 40922, "Actor Assemblies", "ActorAssemblies", nullptr},
        {"connection-graph", 0, "Gameplay Connection Graph", "ConnectionGraph", OpenFromConnections},
        {"find-usages", 0, "Asset Find and Replace", "FindUsages", OpenFindUsages},
        {"find-usages-all-maps", 41061, "Find Usages in All Maps", "FindUsagesInAllMaps", nullptr},
        {"light-shadow", 41201, "Light and Shadow", "LightShadow", nullptr},
    };
    MONITORINFO main{sizeof(main)};
    GetMonitorInfoA(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &main);
    int index = 0;
    for (const auto& tool : windows)
    {
        ++index;
        auto open = [&]() -> HWND {
            if (tool.open) return tool.open();
            Command(tool.command);
            Pump(300);
            return OpenedWindow(tool.title);
        };
        HWND w = open();
        if (!w) { Fail("placement-" + std::string(tool.name), "did not open; boxes=" + std::to_string(boxes.size()) + (boxes.empty() ? "" : " " + boxes[0])); continue; }
        const bool resizable = (GetWindowLongA(w, GWL_STYLE) & WS_THICKFRAME) != 0;
        RECT before{};
        GetWindowRect(w, &before);
        // Somewhere new on the main monitor, and a size of its own when it can be resized.
        const int x = main.rcWork.left + 60 + index * 23, y = main.rcWork.top + 40 + index * 17;
        const int width = (before.right - before.left) + (resizable ? 31 : 0), height = (before.bottom - before.top) + (resizable ? 19 : 0);
        SetWindowPos(w, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        Pump(150);
        const RECT moved = NormalRect(w);
        CloseWindowAndWait(w);
        const auto entry = PlacementEntry(tool.key);
        HWND again = open();
        const RECT back = again ? NormalRect(again) : RECT{};
        Check(again && moved.right > moved.left && SameRect(back, moved) && !entry.empty(), "placement-" + std::string(tool.name),
              "moved " + RectText(moved) + " back " + RectText(back) + " entry '" + entry + "'" + (resizable ? "" : " (fixed size)"));
        if (again) CloseWindowAndWait(again);
    }
    if (HWND list = OpenedWindow("Gameplay Connections")) CloseWindowAndWait(list);

    // Maximised, and back to its own size when restored.
    if (HWND w = (Command(41220), Pump(300), OpenedWindow("Map Check")))
    {
        const RECT normal = NormalRect(w);
        ShowWindow(w, SW_MAXIMIZE);
        Pump(200);
        CloseWindowAndWait(w);
        const auto entry = PlacementEntry("MapCheck");
        Command(41220);
        Pump(300);
        HWND again = OpenedWindow("Map Check");
        const bool zoomed = again && IsZoomed(again);
        if (again) { ShowWindow(again, SW_RESTORE); Pump(200); }
        const RECT restored = again ? NormalRect(again) : RECT{};
        Check(zoomed && SameRect(restored, normal), "placement-maximised", "entry '" + entry + "' zoomed=" + std::to_string(zoomed) + " restored " + RectText(restored) + " was " + RectText(normal));
        if (again) CloseWindowAndWait(again);
    }

    // A place on no monitor (one unplugged since) is ignored.
    WritePrivateProfileStringA("WindowPlacement", "RenderBudget", "-30000,-30000,900,600,0", (directory / "Reloaded_Editor.ini").string().c_str());
    Command(41280);
    Pump(300);
    if (HWND w = OpenedWindow("Render Budget"))
    {
        RECT r{};
        GetWindowRect(w, &r);
        Check(MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr && r.left > -20000, "placement-off-screen-ignored", RectText(r));
        CloseWindowAndWait(w);
    }
    else Fail("placement-off-screen-ignored", "Render Budget did not open");
    // A damaged entry is ignored too.
    WritePrivateProfileStringA("WindowPlacement", "LightingBudget", "nonsense", (directory / "Reloaded_Editor.ini").string().c_str());
    Command(41150);
    Pump(300);
    if (HWND w = OpenedWindow("Lighting Budget")) { Pass("placement-damaged-entry-ignored"); CloseWindowAndWait(w); }
    else Fail("placement-damaged-entry-ignored", "Lighting Budget did not open");
}

// ---- Render Budget, Meshes by cost: one row per static mesh, heaviest first, adding up to the
// zone list's mesh triangles; a click selects every copy, a zone row narrows the list to that zone.
std::vector<std::string> Cells(const std::string& row)
{
    std::vector<std::string> cells;
    size_t at = 0;
    for (size_t next; (next = row.find(" | ", at)) != std::string::npos; at = next + 3) cells.push_back(row.substr(at, next - at));
    cells.push_back(row.substr(at));
    return cells;
}
long long Number(const std::string& text)
{
    std::string digits;
    for (char c : text) if (c >= '0' && c <= '9') digits += c;
    return digits.empty() ? -1 : std::stoll(digits);
}
std::string ObjectPath(unsigned char* object)
{
    std::string path;
    for (int depth = 0; object && depth < 16; ++depth, object = *reinterpret_cast<unsigned char**>(object + 0x18))
        path = std::string(ObjectName(object)) + (path.empty() ? "" : "." + path);
    return path;
}
void ClickRow(HWND window, int id, int row, bool twice)
{
    NMITEMACTIVATE click{};
    click.hdr.hwndFrom = GetDlgItem(window, id);
    click.hdr.idFrom = static_cast<UINT_PTR>(id);
    click.hdr.code = twice ? NM_DBLCLK : NM_CLICK;
    click.iItem = row;
    SendMessageA(window, WM_NOTIFY, static_cast<WPARAM>(id), reinterpret_cast<LPARAM>(&click));
    Pump(300);
}

void MeshCostSuite()
{
    Command(41280);
    Pump(500);
    HWND budget = OpenedWindow("Render Budget");
    if (!budget) { Fail("meshcost-open", "Render Budget did not open; boxes=" + std::to_string(boxes.size())); return; }
    HWND views = GetDlgItem(budget, 110), meshList = GetDlgItem(budget, 111), zoneList = GetDlgItem(budget, 100);
    Check(views && meshList && !IsWindowVisible(meshList), "meshcost-hotspots-first", "views=" + std::to_string(views != nullptr));
    SendMessageA(views, CB_SETCURSEL, 1, 0);
    SendMessageA(budget, WM_COMMAND, MAKEWPARAM(110, CBN_SELCHANGE), reinterpret_cast<LPARAM>(views));
    Pump(300);
    const auto rows = ListRows(meshList);
    const auto zones = ListRows(zoneList);
    long long sum = 0, previous = -1;
    bool ordered = true, shares = true;
    for (const auto& row : rows)
    {
        const auto cells = Cells(row);
        if (cells.size() < 5) { ordered = false; continue; }
        const long long copies = Number(cells[1]), each = Number(cells[2]), total = Number(cells[3]);
        if (previous >= 0 && total > previous) ordered = false;
        if (copies * each < total || total <= 0) ordered = false;
        if (cells[4].empty() || cells[4].back() != '%') shares = false;
        previous = total;
        sum += total;
    }
    const long long mapMeshTriangles = zones.empty() ? -1 : Number(Cells(zones[0])[4]);
    Check(IsWindowVisible(meshList) && !rows.empty() && ordered && shares && sum == mapMeshTriangles, "meshcost-whole-map",
          std::to_string(rows.size()) + " meshes, sum " + std::to_string(sum) + " vs map mesh tris " + std::to_string(mapMeshTriangles)
          + " ordered=" + std::to_string(ordered) + " shares=" + std::to_string(shares) + " scope '" + WindowText(GetDlgItem(budget, 112)) + "'");
    for (size_t i = 0; i < rows.size() && i < 5; ++i) Info("meshcost-row", rows[i]);

    // A click selects every copy; a double-click frames them too.
    if (!rows.empty())
    {
        const auto first = Cells(rows[0]);
        Exec("SELECT NONE");
        ClickRow(budget, 111, 0, false);
        int copies = 0, same = 0;
        for (auto a : Actors())
            if (Flags(a) & 0x40)
            {
                ++copies;
                if (auto p = FindProperty(a, "StaticMesh"))
                    if (ObjectPath(*reinterpret_cast<unsigned char**>(a + *reinterpret_cast<unsigned*>(p + 0x3C))) == first[0]) ++same;
            }
        Check(copies == Number(first[1]) && same == copies, "meshcost-click-selects-copies",
              first[0] + ": selected " + std::to_string(copies) + " of " + first[1] + ", same mesh " + std::to_string(same)
              + " status '" + WindowText(GetDlgItem(budget, 102)) + "'");
        ClickRow(budget, 111, 0, true);
        Check(WindowText(GetDlgItem(budget, 102)).find("framed") != std::string::npos, "meshcost-double-click-frames", WindowText(GetDlgItem(budget, 102)));
    }

    // A zone row: only that zone counts. The busiest zone by mesh triangles, so the list differs.
    int best = -1;
    long long bestTriangles = -1;
    for (size_t i = 1; i < zones.size(); ++i)
    {
        const auto cells = Cells(zones[i]);
        if (cells[0].rfind("From camera", 0) == 0) continue;
        const long long triangles = Number(cells[4]);
        if (triangles > bestTriangles && triangles < mapMeshTriangles) { bestTriangles = triangles; best = static_cast<int>(i); }
    }
    if (best > 0)
    {
        const auto zoneName = Cells(zones[best])[0];
        ClickRow(budget, 100, best, false);
        const auto zoneRows = ListRows(meshList);
        long long zoneSum = 0;
        for (const auto& row : zoneRows) zoneSum += Number(Cells(row)[3]);
        const auto scope = WindowText(GetDlgItem(budget, 112));
        Check(zoneSum == bestTriangles && zoneRows.size() <= rows.size() && scope.find(zoneName) != std::string::npos, "meshcost-zone-only",
              zoneName + ": " + std::to_string(zoneRows.size()) + " meshes, sum " + std::to_string(zoneSum) + " vs " + std::to_string(bestTriangles) + " scope '" + scope + "'");
        // Back to the whole map.
        ClickRow(budget, 100, 0, false);
        long long again = 0;
        for (const auto& row : ListRows(meshList)) again += Number(Cells(row)[3]);
        Check(again == mapMeshTriangles, "meshcost-whole-map-again", std::to_string(again));
    }
    else Fail("meshcost-zone-only", "no zone with meshes");

    // Back to hotspots: the hotspot list shows again.
    SendMessageA(views, CB_SETCURSEL, 0, 0);
    SendMessageA(budget, WM_COMMAND, MAKEWPARAM(110, CBN_SELCHANGE), reinterpret_cast<LPARAM>(views));
    Pump(200);
    Check(IsWindowVisible(GetDlgItem(budget, 101)) && !IsWindowVisible(meshList), "meshcost-back-to-hotspots");
    CloseWindowAndWait(budget);
}

// ---- Command Palette: Help > Command Palette lists the menu bar's commands; typing finds one, Enter or a
// click runs it, and the command run last is listed first the next time. Ctrl+Shift+P opens and closes it.
constexpr UINT kPalette = 41580;
std::vector<std::string> PaletteRows(HWND palette)
{
    std::vector<std::string> rows;
    HWND list = GetDlgItem(palette, 101);
    const auto count = SendMessageA(list, LB_GETCOUNT, 0, 0);
    for (LRESULT i = 0; i < count; ++i)
    {
        std::string text(static_cast<size_t>(SendMessageA(list, LB_GETTEXTLEN, i, 0)) + 1, '\0');
        SendMessageA(list, LB_GETTEXT, i, reinterpret_cast<LPARAM>(text.data()));
        text.resize(strlen(text.c_str()));
        rows.push_back(text);
    }
    return rows;
}
HWND OpenPalette()
{
    Command(kPalette);
    Pump(300);
    HWND palette = TopOfClass("ReloadedCommandPalette");
    return palette && IsWindowVisible(palette) ? palette : nullptr;
}
void TypeInPalette(HWND palette, const char* text)
{
    SetWindowTextA(GetDlgItem(palette, 100), text);
    Pump(100);
}
// Pump with the message box answerer running: a command the palette posted runs inside the pump.
void PumpAnswering(DWORD ms)
{
    boxes.clear();
    UINT_PTR timer = SetTimer(nullptr, 0, 100, AnswerBoxes);
    Pump(ms);
    KillTimer(nullptr, timer);
}
bool HasRow(const std::vector<std::string>& rows, const std::string& start)
{
    for (const auto& row : rows) if (row.rfind(start, 0) == 0) return true;
    return false;
}
// The editor frame with the palette drawn over it, as a 24-bit BMP (window contents only).
void SavePaletteShot(HWND palette, const std::string& file)
{
    RECT frame{}, popup{};
    GetWindowRect(frameWindow, &frame);
    GetWindowRect(palette, &popup);
    const int w = frame.right - frame.left, h = frame.bottom - frame.top;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen), part = CreateCompatibleDC(screen);
    HBITMAP image = CreateCompatibleBitmap(screen, w, h);
    HBITMAP piece = CreateCompatibleBitmap(screen, popup.right - popup.left, popup.bottom - popup.top);
    HGDIOBJ old = SelectObject(dc, image), oldPart = SelectObject(part, piece);
    PrintWindow(frameWindow, dc, 2 /* PW_RENDERFULLCONTENT */);
    PrintWindow(palette, part, 2);
    BitBlt(dc, popup.left - frame.left, popup.top - frame.top, popup.right - popup.left, popup.bottom - popup.top, part, 0, 0, SRCCOPY);
    BITMAPINFOHEADER header{sizeof(header), w, h, 1, 24, BI_RGB};
    const int stride = (w * 3 + 3) & ~3;
    std::vector<char> pixels(static_cast<size_t>(stride) * h);
    GetDIBits(dc, image, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
    BITMAPFILEHEADER fileHeader{0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(header) + pixels.size()), 0, 0,
                                sizeof(BITMAPFILEHEADER) + sizeof(header)};
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
    SelectObject(dc, old);
    SelectObject(part, oldPart);
    DeleteObject(image);
    DeleteObject(piece);
    DeleteDC(dc);
    DeleteDC(part);
    ReleaseDC(nullptr, screen);
    Info("palette-shot", file);
}
// Ctrl+Shift+P as the keyboard would deliver it to the frame's thread: the key state says Ctrl and Shift
// are down while the P press and release are fetched from the queue.
void PressCtrlShiftP()
{
    BYTE keys[256] = {}, held[256] = {};
    GetKeyboardState(keys);
    memcpy(held, keys, sizeof(held));
    held[VK_CONTROL] = held[VK_LCONTROL] = held[VK_SHIFT] = held[VK_LSHIFT] = 0x80;
    SetKeyboardState(held);
    PostMessageA(frameWindow, WM_KEYDOWN, 'P', 0x00190001);
    Pump(150);
    PostMessageA(frameWindow, WM_KEYUP, 'P', 0xC0190001);
    Pump(150);
    SetKeyboardState(keys);
    Pump(200);
}
void PaletteSuite()
{
    std::error_code ignored;
    std::filesystem::remove(directory / "ReloadedEditor" / "command_palette_recent.json", ignored);

    HWND palette = OpenPalette();
    Check(palette != nullptr, "palette-open", "boxes=" + std::to_string(boxes.size()));
    if (!palette) return;
    auto rows = PaletteRows(palette);
    int withKeys = 0;
    for (const auto& row : rows) if (row.find('\t') != std::string::npos) ++withKeys;
    for (size_t i = 0; i < rows.size() && i < 4; ++i) Info("palette-row", rows[i]);
    // Stock commands, RE+ ones and a submenu's, each with its menu path; the palette itself is not listed.
    Check(rows.size() > 100 && HasRow(rows, "File > ") && HasRow(rows, "Build > Map Check...") && HasRow(rows, "Help > RE+ Shortcuts...")
              && HasRow(rows, "RE+ Tools > Camera Bookmarks > Go to Bookmark 1\t1") && !HasRow(rows, "Help > Command Palette"),
          "palette-lists-menus", std::to_string(rows.size()) + " commands, " + std::to_string(withKeys) + " with a shortcut");
    Check(withKeys > 10, "palette-shows-shortcuts", std::to_string(withKeys));
    const auto hint = WindowText(GetDlgItem(palette, 102));
    Check(hint.find(std::to_string(rows.size()) + " of " + std::to_string(rows.size())) == 0, "palette-count", hint);

    TypeInPalette(palette, "rendr budg");
    rows = PaletteRows(palette);
    Check(!rows.empty() && rows[0].rfind("Build > Render Budget...", 0) == 0, "palette-fuzzy-first",
          std::to_string(rows.size()) + " rows, first '" + (rows.empty() ? std::string() : rows[0]) + "'");
    TypeInPalette(palette, "zzqqxx");
    Check(PaletteRows(palette).empty(), "palette-no-match", WindowText(GetDlgItem(palette, 102)));
    // shot=<file.bmp> and shotquery=<typed text> in -Extra: a picture of the palette over the editor.
    if (const auto shot = Ini("shot"); !shot.empty())
    {
        TypeInPalette(palette, Ini("shotquery", "light").c_str());
        Pump(300);
        SavePaletteShot(palette, shot);
    }
    TypeInPalette(palette, "map check");
    rows = PaletteRows(palette);
    Check(!rows.empty() && rows[0].rfind("Build > Map Check...", 0) == 0, "palette-words-first", rows.empty() ? "" : rows[0]);

    // Enter runs the selected row: the palette goes and Map Check opens.
    SendMessageA(GetDlgItem(palette, 100), WM_KEYDOWN, VK_RETURN, 0);
    PumpAnswering(1500);
    HWND check = OpenedWindow("Map Check");
    Check(!IsWindow(palette) && check != nullptr, "palette-enter-runs", "palette open=" + std::to_string(IsWindow(palette) != 0) + " boxes=" + std::to_string(boxes.size()));
    if (check) CloseWindowAndWait(check);

    // A click runs a row too.
    palette = OpenPalette();
    if (!palette) { Fail("palette-reopen"); return; }
    rows = PaletteRows(palette);
    Check(!rows.empty() && rows[0].rfind("Build > Map Check...", 0) == 0, "palette-recent-first", rows.empty() ? "" : rows[0]);
    TypeInPalette(palette, "render budget");
    HWND list = GetDlgItem(palette, 101);
    RECT item{};
    SendMessageA(list, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&item));
    const LPARAM at = MAKELPARAM((item.left + item.right) / 2, (item.top + item.bottom) / 2);
    SendMessageA(list, WM_LBUTTONDOWN, MK_LBUTTON, at);
    SendMessageA(list, WM_LBUTTONUP, 0, at);
    PumpAnswering(1500);
    HWND budget = OpenedWindow("Render Budget");
    Check(!IsWindow(palette) && budget != nullptr, "palette-click-runs", "boxes=" + std::to_string(boxes.size()));
    if (budget) CloseWindowAndWait(budget);

    // Most recent first, then the one before; the arrows move; Esc closes.
    palette = OpenPalette();
    if (!palette) { Fail("palette-reopen"); return; }
    rows = PaletteRows(palette);
    Check(rows.size() > 2 && rows[0].rfind("Build > Render Budget...", 0) == 0 && rows[1].rfind("Build > Map Check...", 0) == 0,
          "palette-recent-order", rows.size() > 1 ? rows[0] + " | " + rows[1] : "");
    std::ifstream saved(directory / "ReloadedEditor" / "command_palette_recent.json");
    Check(saved.good(), "palette-recent-saved");
    HWND edit = GetDlgItem(palette, 100);
    SendMessageA(edit, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageA(edit, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageA(edit, WM_KEYDOWN, VK_UP, 0);
    Check(SendMessageA(GetDlgItem(palette, 101), LB_GETCURSEL, 0, 0) == 1, "palette-arrows");
    SendMessageA(edit, WM_KEYDOWN, VK_ESCAPE, 0);
    Pump(200);
    Check(!IsWindow(palette) && !OpenedWindow("Render Budget"), "palette-escape-closes");

    // Ctrl+Shift+P: only while the editor is the foreground application.
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    if (process != GetCurrentProcessId()) { Info("palette-hotkey", "skipped: the editor is not in front"); return; }
    PressCtrlShiftP();
    palette = TopOfClass("ReloadedCommandPalette");
    Check(palette && IsWindowVisible(palette), "palette-hotkey-opens");
    PressCtrlShiftP();
    Check(!TopOfClass("ReloadedCommandPalette"), "palette-hotkey-closes");
}

// ---- Linked prefabs: two meshes (one Tagged, the other's Event pointing at it) saved as an
// assembly, placed linked twice; one instance edited and the prefab updated from it rebuilds the
// other where it stands, in one Undo step that Undo takes back; the link survives save and reopen;
// Break Link drops an instance.
using Json = nlohmann::json;
Json Request(const Json& request)
{
    using Fn = int(__cdecl*)(const char*, char*, unsigned);
    static Fn call = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("Reloaded.Editor.dll"), "ReloadedWorkflowRequest"));
    if (!call) throw std::runtime_error("no workflow bridge");
    std::vector<char> out(16 * 1024 * 1024);
    const auto text = request.dump();
    call(text.c_str(), out.data(), static_cast<unsigned>(out.size()));
    auto reply = Json::parse(out.data());
    if (!reply.value("ok", false)) throw std::runtime_error(request.value("op", std::string()) + ": " + reply.value("error", std::string()));
    return reply.at("result");
}
unsigned char* ActorByPath(const std::string& path)
{
    const auto name = path.substr(path.find_last_of('.') + 1);
    for (auto a : Actors()) if (!_stricmp(Name(a).c_str(), name.c_str())) return a;
    return nullptr;
}
Json Instances() { return Request({{"op", "prefab.instances"}}); }
Json LiveOf(const Json& instances, size_t i) { return i < instances.size() ? instances[i].at("live") : Json::array(); }
// The member of a live list whose name ends in `stem` (the placement prefix differs per copy).
unsigned char* MemberNamed(const Json& live, const std::string& stem)
{
    for (const auto& m : live)
    {
        const auto path = m.at("path").get<std::string>();
        if (path.size() >= stem.size() && !_stricmp(path.substr(path.size() - stem.size()).c_str(), stem.c_str())) return ActorByPath(path);
    }
    return nullptr;
}
std::string TagOf(const std::string& path, const char* key)
{
    for (const auto& a : Request({{"op", "actors"}})) if (a.at("path") == path) return a.value(key, std::string());
    return {};
}
// The editor frame and a tool window over it, for the report (PrintWindow of each, never the desktop).
void SaveWindowBitmap(HWND w, const std::filesystem::path& file)
{
    RECT r{};
    GetWindowRect(w, &r);
    const int width = r.right - r.left, height = r.bottom - r.top;
    if (width <= 0 || height <= 0) return;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(dc, bitmap);
    PrintWindow(w, dc, 2);
    BITMAPFILEHEADER header{0x4d42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + width * height * 4), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    out.write(static_cast<const char*>(bits), static_cast<std::streamsize>(width) * height * 4);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}
void PrefabsSuite()
{
    auto pair = FreeMeshes(2, 5);
    if (pair.size() < 2) { Fail("prefab-meshes", "fewer than two free meshes"); return; }
    const std::string level = Request({{"op", "map"}}).at("level");
    const std::string nameA = Name(pair[0]), nameB = Name(pair[1]);
    const Json idA = {{"path", level + "." + nameA}, {"class", "Engine.StaticMeshActor"}}, idB = {{"path", level + "." + nameB}, {"class", "Engine.StaticMeshActor"}};
    Request({{"op", "light.set"}, {"actor", idA}, {"properties", {{"Tag", "PfSmokeLamp"}}}});
    Request({{"op", "light.set"}, {"actor", idB}, {"properties", {{"Event", "PfSmokeLamp"}}}});
    SelectOnly(pair);
    const auto entry = Request({{"op", "prefab.save"}, {"name", "Smoke Prefab"}});
    Check(entry.at("actors").size() == 2 && entry.at("bindings").empty(), "prefab-saved", "actors=" + std::to_string(entry.at("actors").size()) + " bindings=" + std::to_string(entry.at("bindings").size()));
    const auto id = entry.at("id").get<std::string>();
    const auto a = ReadVector(pair[0], "Location");

    // Two linked copies, the second turned a quarter, one Undo step each.
    auto before = Undo();
    Request({{"op", "prefab.place"}, {"assembly", id}, {"position", {a.x, a.y + 2048, a.z}}, {"rotation", {0, 0, 0}}});
    auto after = Undo();
    Check(after.count == before.count + 1 && after.current == "Place linked prefab", "prefab-place-one-step", "undo '" + after.current + "'");
    Request({{"op", "prefab.place"}, {"assembly", id}, {"position", {a.x + 2048, a.y + 2048, a.z}}, {"rotation", {0, 16384, 0}}});
    auto instances = Instances();
    Check(instances.size() == 2 && LiveOf(instances, 0).size() == 2 && LiveOf(instances, 1).size() == 2, "prefab-two-instances", instances.dump().substr(0, 300));
    if (instances.size() != 2) return;
    const auto counts = Request({{"op", "prefab.counts"}});
    Check(counts.value(id, 0) == 2, "prefab-count", counts.dump());

    // Tags are fresh per copy, and each copy's Event follows its own Tag.
    auto lamp1 = MemberNamed(LiveOf(instances, 0), nameA), lamp2 = MemberNamed(LiveOf(instances, 1), nameA);
    auto switch1 = MemberNamed(LiveOf(instances, 0), nameB), switch2 = MemberNamed(LiveOf(instances, 1), nameB);
    if (!lamp1 || !lamp2 || !switch1 || !switch2) { Fail("prefab-members", "members not found"); return; }
    const auto tag1 = TagOf(level + "." + Name(lamp1), "tag"), tag2 = TagOf(level + "." + Name(lamp2), "tag");
    const auto event1 = TagOf(level + "." + Name(switch1), "event"), event2 = TagOf(level + "." + Name(switch2), "event");
    Check(tag1 != tag2 && !_stricmp(tag1.c_str(), event1.c_str()) && !_stricmp(tag2.c_str(), event2.c_str()) && tag1.find("PfSmokeLamp") != std::string::npos,
          "prefab-fresh-tags", tag1 + "/" + event1 + " " + tag2 + "/" + event2);

    // Select Instance and Select All Instances, from the frame menu.
    SelectOnly({lamp1});
    Command(41540);
    Check(Selected() == 2, "prefab-select-instance", "selected=" + std::to_string(Selected()));
    SelectOnly({lamp1});
    Command(41541);
    Check(Selected() == 4, "prefab-select-all", "selected=" + std::to_string(Selected()));

    // Edit the first copy: its lamp up 64 (an Undo step of its own), then update from it.
    Request({{"op", "design.translate"}, {"members", Json::array({{{"path", level + "." + Name(lamp1)}, {"class", "Engine.StaticMeshActor"}}})}, {"delta", {0, 0, 64}}});
    // The second copy moved as a whole: it is rebuilt where it now stands.
    Request({{"op", "design.translate"}, {"members", Json::array({{{"path", level + "." + Name(lamp2)}, {"class", "Engine.StaticMeshActor"}},
                                                                  {{"path", level + "." + Name(switch2)}, {"class", "Engine.StaticMeshActor"}}})}, {"delta", {128, -64, 0}}});
    const auto lamp2Before = ReadVector(lamp2, "Location"), switch2Before = ReadVector(switch2, "Location");
    const std::string oldLamp2 = Name(lamp2), oldSwitch2 = Name(switch2);
    SelectOnly({switch1});
    before = Undo();
    Command(41542);
    after = Undo();
    Check(after.count == before.count + 1 && after.current == "Update linked prefab" && BoxContains("Update the prefab"), "prefab-update-one-step",
          "undo '" + after.current + "' boxes=" + std::to_string(boxes.size()));
    instances = Instances();
    auto newLamp2 = MemberNamed(LiveOf(instances, 1), nameA), newSwitch2 = MemberNamed(LiveOf(instances, 1), nameB);
    if (!newLamp2 || !newSwitch2) { Fail("prefab-rebuilt", "rebuilt members not found " + instances.dump().substr(0, 300)); return; }
    const auto lampNow = ReadVector(newLamp2, "Location"), switchNow = ReadVector(newSwitch2, "Location");
    Check(Name(newLamp2) != oldLamp2 && std::fabs(lampNow.z - lamp2Before.z - 64) < 0.5f && std::fabs(lampNow.x - lamp2Before.x) < 0.5f && std::fabs(lampNow.y - lamp2Before.y) < 0.5f
          && std::fabs(switchNow.x - switch2Before.x) + std::fabs(switchNow.y - switch2Before.y) + std::fabs(switchNow.z - switch2Before.z) < 0.5f && !ActorByPath(oldLamp2), "prefab-rebuilt-in-place",
          VecText(lamp2Before) + "->" + VecText(lampNow) + " switch " + VecText(switch2Before) + "->" + VecText(switchNow));
    const auto tagNew = TagOf(level + "." + Name(newLamp2), "tag"), eventNew = TagOf(level + "." + Name(newSwitch2), "event");
    Check(tagNew != tag1 && tagNew != tag2 && !_stricmp(tagNew.c_str(), eventNew.c_str()), "prefab-rebuilt-fresh-tags", tagNew + "/" + eventNew);

    // Undo puts the old copy back: its own actors, where they were.
    const std::string newLampName = Name(newLamp2);
    before = Undo();
    Command(40019);
    Pump(500);
    after = Undo();
    instances = Instances();
    auto back = ActorByPath(oldLamp2);
    std::string live;
    for (const auto& m : LiveOf(instances, 1)) live += m.at("path").get<std::string>() + " ";
    Check(back && Same(ReadVector(back, "Location"), lamp2Before) && !ActorByPath(newLampName) && LiveOf(instances, 1).size() == 2, "prefab-undo-restores",
          "undid '" + before.current + "' now '" + after.current + "' back=" + (back ? VecText(ReadVector(back, "Location")) : std::string("none")) + " new still there=" + (ActorByPath(newLampName) ? "yes" : "no") + " live=" + live);

    // Actor Assemblies shows the count; a picture for the report.
    Command(40922);
    Pump(800);
    if (HWND assemblies = OpenedWindow("Actor Assemblies"))
    {
        HWND list = FindWindowExA(assemblies, nullptr, WC_LISTVIEWA, nullptr);
        bool shown = false;
        for (const auto& row : ListRows(list)) if (row.find("Smoke Prefab") != std::string::npos && row.find("2 linked instances in this map") != std::string::npos) shown = true;
        Check(shown, "prefab-list-count");
        SelectOnly({switch1});
        Command(41541);
        Exec("CAMERA ALIGN");
        Pump(800);
        SaveWindowBitmap(frameWindow, directory / "prefabs-frame.bmp");
        SaveWindowBitmap(assemblies, directory / "prefabs-window.bmp");
        CloseWindowAndWait(assemblies);
    }
    else Fail("prefab-list-count", "Actor Assemblies did not open");

    // The link is in the map: saved and opened again, both copies are still instances.
    Command(40007);
    Pump(500);
    ReopenRecent(IDYES);
    instances = Instances();
    Check(instances.size() == 2 && LiveOf(instances, 0).size() == 2 && LiveOf(instances, 1).size() == 2, "prefab-survives-reopen", instances.dump().substr(0, 300));

    // Break Link: the copy's actors stay, unlinked.
    if (auto lamp = MemberNamed(LiveOf(instances, 1), nameA))
    {
        SelectOnly({lamp});
        before = Undo();
        Command(41543);
        after = Undo();
        instances = Instances();
        Check(instances.size() == 1 && ActorByPath(level + "." + Name(lamp)) && after.count == before.count + 1, "prefab-break-link",
              "instances=" + std::to_string(instances.size()) + " undo '" + after.current + "' " + std::to_string(before.count) + "->" + std::to_string(after.count));
        SelectOnly({lamp});
        Command(41542);
        Check(BoxContains("Select an actor of a linked prefab"), "prefab-unlinked-refuses-update");
    }
}

// ---- Light and Shadow Map: sampling with static meshes, kept work after edits, the settings
// switches, and a picture of the map and its settings window for the report.
std::uintmax_t LogSize()
{
    std::error_code ignored;
    return std::filesystem::file_size(directory / "Reloaded.Editor.dll.log", ignored);
}
// The last "LightShadowMap: ... patches" line the editor wrote after a point in its log.
std::string LastShadowLine(std::uintmax_t from)
{
    std::ifstream log(directory / "Reloaded.Editor.dll.log", std::ios::binary);
    log.seekg(static_cast<std::streamoff>(from));
    std::string line, last;
    while (std::getline(log, line))
        if (line.find("LightShadowMap: ") != std::string::npos && line.find(" patches") != std::string::npos) last = line;
    return last;
}
long long NumberBefore(const std::string& text, const std::string& word)
{
    const auto at = text.find(word);
    if (at == std::string::npos) return -1;
    size_t start = at;
    while (start > 0 && isdigit(static_cast<unsigned char>(text[start - 1]))) --start;
    return start < at ? std::stoll(text.substr(start, at - start)) : -1;
}
// The window as drawn (viewports included), as a 24-bit BMP.
bool SaveWindowPicture(HWND w, const std::filesystem::path& file)
{
    RECT r{};
    GetWindowRect(w, &r);
    const int width = r.right - r.left, height = r.bottom - r.top;
    if (width <= 0 || height <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(dc, bitmap);
    BOOL printed = PrintWindow(w, dc, 2 /* PW_RENDERFULLCONTENT */);
    GdiFlush();
    const DWORD stride = (DWORD(width) * 3 + 3) & ~3u, size = stride * DWORD(height);
    // Some windows print nothing; then copy what the window itself shows.
    bool blank = true;
    for (DWORD i = 0; i < size && blank; i += 97) blank = static_cast<const unsigned char*>(bits)[i] == 0;
    if (blank)
    {
        HDC own = GetWindowDC(w);
        printed = BitBlt(dc, 0, 0, width, height, own, 0, 0, SRCCOPY);
        ReleaseDC(w, own);
        GdiFlush();
    }
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + size;
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    out.write(static_cast<const char*>(bits), size);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return printed && out.good();
}
void LightShadowSuite()
{
    // On: sampled from nothing.
    auto from = LogSize();
    const double onMs = Command(41200);
    Pump(500);
    auto line = LastShadowLine(from);
    Info("light-shadow-on", "command ms=" + Fixed(onMs, 0) + " | " + line);
    Check(NumberBefore(line, " patches") > 0, "light-shadow-sampled", std::to_string(NumberBefore(line, " patches")) + " patches");
    Check(NumberBefore(line, " on static meshes") > 0, "light-shadow-mesh-floors", std::to_string(NumberBefore(line, " on static meshes")) + " patches on static meshes");

    // An in-game light moved: sampled again a moment later, keeping the other lights.
    // One with a static mesh near it, for the step after.
    unsigned char *light = nullptr, *mesh = nullptr;
    const auto meshes = OfClass("StaticMeshActor");
    for (auto a : Actors())
    {
        auto type = FindProperty(a, "LightType");
        if (!type || !a[*reinterpret_cast<unsigned*>(type + 0x3C)]) continue;
        const unsigned flagsA = *reinterpret_cast<unsigned*>(a + 0x2e8), flagsB = *reinterpret_cast<unsigned*>(a + 0x2ec);
        if (!(flagsB & 0x800000) && !(flagsA & 0x20000)) continue;
        if (!light) light = a;
        const auto lamp = ReadVector(a, "Location");
        for (auto m : meshes)
        {
            const auto at = ReadVector(m, "Location");
            const double dx = at.x - lamp.x, dy = at.y - lamp.y, dz = at.z - lamp.z;
            if (dx * dx + dy * dy + dz * dz < 600.0 * 600.0) { light = a; mesh = m; break; }
        }
        if (mesh) break;
    }
    if (light)
    {
        auto at = ReadVector(light, "Location");
        at.x += 48;
        WriteVector(light, "Location", at);
        *reinterpret_cast<unsigned*>(light + 0x2e8) |= 0x100; // bLightChanged: its render data follows
        from = LogSize();
        Exec("TRANSACTION REDO"); // nothing to redo: only marks the map changed
        Pump(4000);
        line = LastShadowLine(from);
        Info("light-shadow-light-moved", Name(light) + " | " + line);
        Check(line.find(" kept ") != std::string::npos, "light-shadow-keeps-other-lights", line.substr(line.find(" in ") == std::string::npos ? 0 : line.find(" in ")));
    }
    else Info("light-shadow-light-moved", "no in-game light in this map");

    // A static mesh near a light moved: only the lights it is near are worked out again.
    if (mesh)
    {
        auto at = ReadVector(mesh, "Location");
        at.x += 40;
        WriteVector(mesh, "Location", at);
        from = LogSize();
        Exec("TRANSACTION REDO");
        Pump(4000);
        line = LastShadowLine(from);
        Info("light-shadow-mesh-moved", Name(mesh) + " | " + line);
        Check(line.find(" kept ") != std::string::npos || line.find("from nothing") == std::string::npos, "light-shadow-mesh-move-incremental", line.substr(line.find(" in ") == std::string::npos ? 0 : line.find(" in ")));
    }

    // Settings: static mesh floors off leaves only BSP patches; on again brings them back.
    Command(41201);
    Pump(500);
    HWND settings = OpenedWindow("Light and Shadow");
    Check(settings != nullptr, "light-shadow-settings-open");
    if (settings)
    {
        Check(IsDlgButtonChecked(settings, 111) == BST_CHECKED && IsDlgButtonChecked(settings, 112) == BST_CHECKED && IsDlgButtonChecked(settings, 113) == BST_CHECKED,
              "light-shadow-switches-on");
        CheckDlgButton(settings, 112, BST_UNCHECKED);
        SendMessageA(settings, WM_COMMAND, 106, 0); // Apply
        Pump(300);
        const auto off = WindowText(GetDlgItem(settings, 109));
        CheckDlgButton(settings, 112, BST_CHECKED);
        SendMessageA(settings, WM_COMMAND, 106, 0);
        Pump(300);
        const auto on = WindowText(GetDlgItem(settings, 109));
        Check(off.find("on static meshes") == std::string::npos && on.find("on static meshes") != std::string::npos, "light-shadow-mesh-floors-switch",
              off.substr(0, off.find('\r')) + " / " + on.substr(0, on.find('\r')));
        char ini[64] = {};
        GetPrivateProfileStringA("LightShadowMap", "MeshFloors", "", ini, sizeof(ini), (directory / "Reloaded_Editor.ini").string().c_str());
        Check(std::string(ini) == "1", "light-shadow-switch-saved", std::string("MeshFloors=") + ini);
        // Pictures: the editor with the map on, and the settings window. The disposable editor
        // starts hidden; it is shown (without taking the focus) only while they are taken.
        const bool wasVisible = IsWindowVisible(frameWindow) != 0, wasIconic = IsIconic(frameWindow) != 0;
        ShowWindow(frameWindow, wasIconic ? SW_SHOWNOACTIVATE : SW_SHOWNA);
        if (IsIconic(frameWindow)) ShowWindow(frameWindow, SW_RESTORE);
        RECT frame{};
        GetWindowRect(frameWindow, &frame);
        SetWindowPos(settings, HWND_TOP, frame.right - 520, frame.top + 80, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        RedrawWindow(frameWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        Pump(2500);
        const bool first = SaveWindowPicture(frameWindow, directory / "lightshadow-1.bmp");
        const bool second = SaveWindowPicture(settings, directory / "lightshadow-2.bmp");
        if (!wasVisible) ShowWindow(frameWindow, SW_HIDE);
        Info("light-shadow-pictures", (directory / "lightshadow-1.bmp").string() + " printed " + std::to_string(first) + std::to_string(second)
             + " visible " + std::to_string(wasVisible) + " iconic " + std::to_string(wasIconic) + " rect " + std::to_string(frame.left) + ","
             + std::to_string(frame.top) + "," + std::to_string(frame.right) + "," + std::to_string(frame.bottom));
        CloseWindowAndWait(settings);
    }
}

// ---- Map to OBJ: File > Export holds the stock Export... and Map to OBJ...; an export writes an OBJ
// whose faces all point at vertices it wrote, materials the MTL defines and textures that are PNGs on
// disk; Selection only keeps to the selected static mesh actors; the folder is offered again.
struct ObjFile
{
    size_t v = 0, vt = 0, vn = 0, f = 0, badCorners = 0;
    std::vector<std::string> objects;
    std::set<std::string> used;
};
ObjFile ReadObj(const std::filesystem::path& path)
{
    ObjFile o;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
    {
        if (line.rfind("v ", 0) == 0) ++o.v;
        else if (line.rfind("vt ", 0) == 0) ++o.vt;
        else if (line.rfind("vn ", 0) == 0) ++o.vn;
        else if (line.rfind("o ", 0) == 0) o.objects.push_back(line.substr(2));
        else if (line.rfind("usemtl ", 0) == 0) o.used.insert(line.substr(7));
        else if (line.rfind("f ", 0) == 0)
        {
            ++o.f;
            std::istringstream corners(line.substr(2));
            int n = 0;
            for (std::string c; corners >> c; ++n)
            {
                long long a = 0, b = 0, d = 0;
                if (sscanf_s(c.c_str(), "%lld/%lld/%lld", &a, &b, &d) != 3 || a < 1 || a > (long long)o.v || b < 1 || b > (long long)o.vt || d < 1 || d > (long long)o.vn) ++o.badCorners;
            }
            if (n < 3) ++o.badCorners;
        }
    }
    return o;
}
// The frame with a tool window drawn over it where it sits, as a 24-bit BMP (PrintWindow, so only these
// two windows are captured, never the desktop).
void SaveWindowShot(HWND tool, const std::string& file)
{
    RECT frame{}, over{};
    GetWindowRect(frameWindow, &frame);
    GetWindowRect(tool, &over);
    const int w = frame.right - frame.left, h = frame.bottom - frame.top;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen), toolDc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h), toolBitmap = CreateCompatibleBitmap(screen, over.right - over.left, over.bottom - over.top);
    auto old = SelectObject(dc, bitmap);
    auto oldTool = SelectObject(toolDc, toolBitmap);
    PrintWindow(frameWindow, dc, PW_RENDERFULLCONTENT);
    PrintWindow(tool, toolDc, PW_RENDERFULLCONTENT);
    BitBlt(dc, over.left - frame.left, over.top - frame.top, over.right - over.left, over.bottom - over.top, toolDc, 0, 0, SRCCOPY);
    BITMAPINFOHEADER info{sizeof(info), w, h, 1, 24, BI_RGB};
    const int stride = ((w * 3 + 3) / 4) * 4;
    std::vector<char> pixels(static_cast<size_t>(stride) * h);
    SelectObject(dc, old);
    GetDIBits(dc, bitmap, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS);
    BITMAPFILEHEADER header{0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(info) + pixels.size()), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(info)};
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    out.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
    SelectObject(toolDc, oldTool);
    DeleteObject(bitmap);
    DeleteObject(toolBitmap);
    DeleteDC(dc);
    DeleteDC(toolDc);
    ReleaseDC(nullptr, screen);
}
void ObjExportSuite()
{
    // The menu.
    HMENU file = GetSubMenu(GetMenu(frameWindow), 0);
    HMENU exportMenu = nullptr;
    for (int i = 0; file && i < GetMenuItemCount(file); ++i)
        if (HMENU sub = GetSubMenu(file, i))
            if (GetMenuState(sub, 41660, MF_BYCOMMAND) != UINT(-1)) exportMenu = sub;
    Check(exportMenu && GetMenuState(exportMenu, 40017, MF_BYCOMMAND) != UINT(-1) && GetMenuState(file, 40017, MF_BYCOMMAND) != UINT(-1),
          "objexport-menu", exportMenu ? "File > Export holds Map... and Map to OBJ..." : "no File > Export submenu");

    Command(41660);
    Pump(300);
    HWND w = OpenedWindow("Export Map to OBJ");
    if (!w) { Fail("objexport-open", "the window did not open; boxes=" + std::to_string(boxes.size())); return; }
    const auto root = directory.parent_path() / "ObjExportTest";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    auto run = [&](const std::filesystem::path& folder, bool selection) {
        SetDlgItemTextA(w, 301, folder.string().c_str());
        CheckDlgButton(w, 303, BST_CHECKED);
        CheckDlgButton(w, 304, BST_CHECKED);
        CheckDlgButton(w, 305, selection ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextA(w, 306, "1");
        const double ms = Command(307, w);
        const auto status = WindowText(GetDlgItem(w, 309));
        Info("objexport-status", status + " (" + Fixed(ms / 1000.0) + " s)");
        return status;
    };
    const auto map = std::filesystem::path(Ini("map")).stem().string();
    const auto full = root / "full";
    const auto status = run(full, false);
    const auto obj = ReadObj(full / (map + ".obj"));
    Check(status.rfind("Wrote", 0) == 0 && obj.f > 1000 && obj.badCorners == 0, "objexport-obj",
          std::to_string(obj.v) + " v, " + std::to_string(obj.vt) + " vt, " + std::to_string(obj.vn) + " vn, " + std::to_string(obj.f) + " faces, "
          + std::to_string(obj.objects.size()) + " objects, bad corners " + std::to_string(obj.badCorners));
    Check(!obj.objects.empty() && obj.objects[0] == "BSP" && obj.objects.size() > 10, "objexport-objects", obj.objects.empty() ? "none" : obj.objects[0]);
    if (const auto shot = Ini("shot"); !shot.empty()) { SaveWindowShot(w, shot); Info("objexport-shot", shot); }

    // Every material used is defined, every texture named is a PNG.
    std::set<std::string> defined;
    size_t maps = 0, missing = 0, notPng = 0;
    {
        std::ifstream in(full / (map + ".mtl"));
        for (std::string line; std::getline(in, line);)
        {
            if (line.rfind("newmtl ", 0) == 0) defined.insert(line.substr(7));
            if (line.rfind("map_Kd ", 0) == 0)
            {
                ++maps;
                std::ifstream png(full / line.substr(7), std::ios::binary);
                char signature[8] = {};
                if (!png) ++missing;
                else if (!png.read(signature, 8) || memcmp(signature, "\x89PNG\r\n\x1a\n", 8)) ++notPng;
            }
        }
    }
    size_t undefined = 0;
    for (const auto& m : obj.used) if (!defined.count(m)) ++undefined;
    Check(undefined == 0 && maps > 10 && missing == 0 && notPng == 0, "objexport-mtl",
          std::to_string(obj.used.size()) + " used, " + std::to_string(defined.size()) + " defined, undefined " + std::to_string(undefined) + ", "
          + std::to_string(maps) + " textures, missing " + std::to_string(missing) + ", not PNG " + std::to_string(notPng));

    // Selection only: three visible static mesh actors, no surfaces.
    Exec("SELECT NONE");
    Exec("POLY SELECT NONE");
    std::vector<unsigned char*> picked;
    for (auto a : OfClass("StaticMeshActor"))
    {
        auto hidden = FindProperty(a, "bHidden");
        auto mesh = FindProperty(a, "StaticMesh");
        if (!hidden || !mesh) continue;
        if (*reinterpret_cast<unsigned*>(a + *reinterpret_cast<unsigned*>(hidden + 0x3C)) & *reinterpret_cast<unsigned*>(hidden + 0x64)) continue;
        if (!*reinterpret_cast<void**>(a + *reinterpret_cast<unsigned*>(mesh + 0x3C))) continue;
        SetSelected(a, true);
        picked.push_back(a);
        if (picked.size() == 3) break;
    }
    const auto some = root / "selection";
    const auto selected = run(some, true);
    const auto part = ReadObj(some / (map + ".obj"));
    bool bsp = false;
    for (const auto& o : part.objects) if (o.rfind("BSP", 0) == 0) bsp = true;
    Check(picked.size() == 3 && part.objects.size() == 3 && !bsp && part.badCorners == 0, "objexport-selection-only",
          std::to_string(part.objects.size()) + " objects, " + std::to_string(part.f) + " faces: " + selected);
    for (auto a : picked) SetSelected(a, false);

    // A scale that is no number is refused in the status line.
    SetDlgItemTextA(w, 306, "abc");
    Command(307, w);
    Check(WindowText(GetDlgItem(w, 309)).find("scale") != std::string::npos, "objexport-bad-scale", WindowText(GetDlgItem(w, 309)));
    SetDlgItemTextA(w, 306, "1");

    // Closed and opened again: the last folder is offered.
    SetDlgItemTextA(w, 301, full.string().c_str());
    CloseWindowAndWait(w);
    Command(41660);
    Pump(300);
    w = OpenedWindow("Export Map to OBJ");
    Check(w && WindowText(GetDlgItem(w, 301)) == full.string(), "objexport-folder-remembered", w ? WindowText(GetDlgItem(w, 301)) : "not open");
    if (w) CloseWindowAndWait(w);
}

// ---- Import OBJ: a small OBJ with an MTL, a BMP that is not a power of two and a TGA that is, imported with
// hulls and placed; per-polygon and no collision; a mesh over the game's limit split into parts placed together;
// the window from File and from the Static Mesh Browser.
nlohmann::json ImportRequest(const nlohmann::json& request)
{
    using Fn = int(__cdecl*)(const char*, char*, unsigned);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("Reloaded.Editor.dll"), "ReloadedWorkflowRequest"));
    if (!fn) return {{"ok", false}, {"error", "no ReloadedWorkflowRequest"}};
    std::vector<char> out(1 << 20);
    const int r = fn(request.dump().c_str(), out.data(), static_cast<unsigned>(out.size()));
    if (r < 0) { out.resize(static_cast<size_t>(-r)); fn(request.dump().c_str(), out.data(), static_cast<unsigned>(out.size())); }
    return nlohmann::json::parse(out.data());
}
void WriteFile(const std::filesystem::path& file, const std::string& bytes)
{
    std::ofstream out(file, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}
// A box's OBJ faces (counter-clockwise from outside), its corners from vertex `base` + 1.
std::string ObjBox(double x0, double y0, double z0, double x1, double y1, double z1, int base)
{
    std::ostringstream o;
    for (int i = 0; i < 8; ++i) o << "v " << ((i & 1) ? x1 : x0) << " " << ((i & 2) ? y1 : y0) << " " << ((i & 4) ? z1 : z0) << "\n";
    const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& q : quads)
        o << "f " << q[0] + 1 + base << "/1 " << q[1] + 1 + base << "/2 " << q[2] + 1 + base << "/3 " << q[3] + 1 + base << "/4\n";
    return o.str();
}
std::string Bmp24(int w, int h)
{
    const int row = (w * 3 + 3) & ~3;
    std::string b(54 + static_cast<size_t>(row) * h, '\0');
    auto u32 = [&](size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) b[at + k] = static_cast<char>((v >> (8 * k)) & 0xff); };
    b[0] = 'B'; b[1] = 'M';
    u32(2, static_cast<uint32_t>(b.size())); u32(10, 54); u32(14, 40); u32(18, static_cast<uint32_t>(w)); u32(22, static_cast<uint32_t>(h));
    b[26] = 1; b[28] = 24;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            char* p = &b[54 + static_cast<size_t>(y) * row + static_cast<size_t>(x) * 3];
            p[0] = static_cast<char>(40); p[1] = static_cast<char>(90 + (x * 7 % 60)); p[2] = static_cast<char>(140 + (y * 5 % 80));
        }
    return b;
}
std::string Tga32(int w, int h)
{
    std::string t = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, static_cast<char>(w & 0xff), static_cast<char>(w >> 8), static_cast<char>(h & 0xff), static_cast<char>(h >> 8), 32, 0x28};
    for (int i = 0; i < w * h; ++i) { t += static_cast<char>(150); t += static_cast<char>(150); t += static_cast<char>(160); t += static_cast<char>(255); }
    return t;
}
// The window's picture (PrintWindow), with `over` drawn on top where it sits, as a 24-bit BMP.
void CaptureWindows(HWND base, HWND over, const std::filesystem::path& file)
{
    RECT b{}, o{};
    GetWindowRect(base, &b);
    const int w = b.right - b.left, h = b.bottom - b.top;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 24, BI_RGB};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(dc, bitmap);
    {
        // PrintWindow draws into a screen-compatible bitmap, not straight into a DIB section.
        HDC bdc = CreateCompatibleDC(screen);
        HBITMAP bb = CreateCompatibleBitmap(screen, w, h);
        auto bold = SelectObject(bdc, bb);
        PrintWindow(base, bdc, 2);
        BitBlt(dc, 0, 0, w, h, bdc, 0, 0, SRCCOPY);
        SelectObject(bdc, bold);
        DeleteObject(bb);
        DeleteDC(bdc);
    }
    if (over)
    {
        GetWindowRect(over, &o);
        HDC odc = CreateCompatibleDC(screen);
        HBITMAP ob = CreateCompatibleBitmap(screen, o.right - o.left, o.bottom - o.top);
        auto oold = SelectObject(odc, ob);
        PrintWindow(over, odc, 2);
        BitBlt(dc, o.left - b.left, o.top - b.top, o.right - o.left, o.bottom - o.top, odc, 0, 0, SRCCOPY);
        SelectObject(odc, oold);
        DeleteObject(ob);
        DeleteDC(odc);
    }
    GdiFlush();
    const int row = (w * 3 + 3) & ~3;
    std::string out(54, '\0');
    auto u32 = [&](size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) out[at + k] = static_cast<char>((v >> (8 * k)) & 0xff); };
    out[0] = 'B'; out[1] = 'M';
    u32(2, static_cast<uint32_t>(54 + row * h)); u32(10, 54); u32(14, 40); u32(18, static_cast<uint32_t>(w)); u32(22, static_cast<uint32_t>(-h));
    out[26] = 1; out[28] = 24;
    out.append(static_cast<const char*>(bits), static_cast<size_t>(row) * h);
    WriteFile(file, out);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}
// The window whose menu bar (or one of its popups) holds the command.
HWND MenuOwner(UINT command)
{
    struct S { UINT c; HWND r; } s{command, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM p) -> BOOL {
        auto s = reinterpret_cast<S*>(p);
        std::function<bool(HMENU)> has = [&](HMENU m) {
            for (int i = 0; m && i < GetMenuItemCount(m); ++i)
            {
                if (GetMenuItemID(m, i) == s->c) return true;
                if (HMENU sub = GetSubMenu(m, i); sub && has(sub)) return true;
            }
            return false;
        };
        if (IsWindowVisible(w) && has(GetMenu(w)) && w != frameWindow) { s->r = w; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.r;
}
bool MenuHas(HWND w, UINT command)
{
    std::function<bool(HMENU)> has = [&](HMENU m) {
        for (int i = 0; m && i < GetMenuItemCount(m); ++i)
        {
            if (GetMenuItemID(m, i) == command) return true;
            if (HMENU sub = GetSubMenu(m, i); sub && has(sub)) return true;
        }
        return false;
    };
    return has(GetMenu(w));
}
// A crate (wood, from a BMP 100 x 60), a lid on it (metal, a 64 x 64 TGA) and a sign (a material with no
// image), written to System\objimport test\crate.obj (returned).
std::filesystem::path WriteObjCrate()
{
    const auto folder = directory / "objimport test";
    std::filesystem::create_directories(folder);
    WriteFile(folder / "wood.bmp", Bmp24(100, 60));
    WriteFile(folder / "metal.tga", Tga32(64, 64));
    WriteFile(folder / "crate.mtl", "newmtl CrateWood\nKd 0.6 0.4 0.2\nmap_Kd wood.bmp\nnewmtl CrateMetal\nmap_Kd -s 1 1 1 metal.tga\nnewmtl Plain\nKd 1 1 1\n");
    std::string obj = "# Import OBJ smoke test\nmtllib crate.mtl\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\no Crate\nusemtl CrateWood\ns 1\n" + ObjBox(0, 0, 0, 1, 0.8, 1, 0) +
                      "o Lid\nusemtl CrateMetal\n" + ObjBox(0.1, 0.8, 0.1, 0.9, 1.0, 0.9, 8) +
                      "o Sign\nusemtl Plain\nv 0 1.1 1.05\nv 1 1.1 1.05\nv 1 1.2 1.05\nv 0 1.2 1.05\nf 17/1 18/2 19/3 20/4\n";
    WriteFile(folder / "crate.obj", obj);
    return folder / "crate.obj";
}
void ObjImportSuite()
{
    namespace fs = std::filesystem;
    const auto folder = WriteObjCrate().parent_path();
    // A wall of 12000 triangles: three parts.
    {
        std::ostringstream o;
        o << "o Wall\nusemtl Plain\n";
        for (int y = 0; y <= 60; ++y)
            for (int x = 0; x <= 100; ++x) o << "v " << x * 0.1 << " " << y * 0.1 << " 0\n";
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 100; ++x)
            {
                const int a = y * 101 + x + 1;
                o << "f " << a << " " << a + 1 << " " << a + 102 << " " << a + 101 << "\n";
            }
        WriteFile(folder / "wall.obj", "mtllib crate.mtl\n" + o.str());
    }
    const auto crate = (folder / "crate.obj").u8string();
    const std::string crateFile(crate.begin(), crate.end());
    const auto wallPath = (folder / "wall.obj").u8string();
    const std::string wallFile(wallPath.begin(), wallPath.end());

    // What it will make.
    auto preview = ImportRequest({{"op", "objimport.preview"}, {"file", crateFile}, {"package", "MyLevel"}, {"group", "ObjTest"}, {"name", "SmokeCrate"}, {"scale", 100},
                            {"up", "y"}, {"collision", "parts"}});
    Info("objimport-preview", preview.dump().substr(0, 1500));
    if (!preview.value("ok", false)) { Fail("objimport-preview", preview.value("error", std::string())); return; }
    const auto& p = preview.at("result");
    const auto& slots = p.at("slots");
    Check(p.at("triangles") == 26 && p.at("hulls") == 3 && p.at("hullTriangles") == 36, "objimport-preview-counts",
          "triangles " + p.at("triangles").dump() + " hulls " + p.at("hulls").dump() + " hull triangles " + p.at("hullTriangles").dump());
    Check(slots.size() == 3 && slots[0].at("kind") == "image" && slots[1].at("kind") == "image" && slots[2].at("kind") == "default", "objimport-preview-materials", slots.dump());

    // The import, placed where the viewport was last clicked: one Undo step.
    auto unrealEd = *reinterpret_cast<unsigned char**>(0x117a59b0);
    if (unrealEd) { auto click = reinterpret_cast<float*>(unrealEd + 0x1bc); click[0] = 128; click[1] = -256; click[2] = 64; }
    const auto before = Undo();
    auto imported = ImportRequest({{"op", "objimport.import"}, {"file", crateFile}, {"package", "MyLevel"}, {"group", "ObjTest"}, {"name", "SmokeCrate"}, {"scale", 100},
                             {"up", "y"}, {"collision", "parts"}, {"place", true}});
    Info("objimport-import", imported.dump().substr(0, 3000));
    if (!imported.value("ok", false)) { Fail("objimport-import", imported.value("error", std::string())); return; }
    const auto& r = imported.at("result");
    const auto& part = r.at("parts").at(0);
    Check(r.at("parts").size() == 1 && part.at("path").get<std::string>().find(".ObjTest.SmokeCrate") != std::string::npos, "objimport-mesh", part.at("path").dump());
    Check(part.at("hullTriangles") == 36 && part.at("hullNodes").get<int>() > 0 && part.at("hullNodes").get<int>() < 32767, "objimport-hulls-in-editor",
          "hull triangles " + part.at("hullTriangles").dump() + " nodes " + part.at("hullNodes").dump());
    bool boundsWarning = false;
    for (const auto& w : r.at("warnings")) boundsWarning = boundsWarning || w.get<std::string>().find("bounds") != std::string::npos;
    const auto& b = part.at("bounds");
    Check(!boundsWarning && std::abs(b[0].get<double>()) < 0.5 && std::abs(b[3].get<double>() - 100) < 0.5 && std::abs(b[5].get<double>() - 120) < 0.5,
          "objimport-bounds-hulls-on-the-mesh", b.dump());
    const auto& mats = part.at("materials");
    // The engine keeps the case a name was first seen with (a Textures group may read textures).
    auto material = [&](size_t i) { auto m = mats[i].at("material").get<std::string>(); for (auto& c : m) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); return m; };
    Check(mats.size() == 3 && material(0).find("textures.cratewood") != std::string::npos && material(1).find("textures.cratemetal") != std::string::npos &&
          material(2).find("defaulttexture") != std::string::npos, "objimport-materials", mats.dump());
    const auto after = Undo();
    Check(r.contains("placed") && r.at("placed").size() == 1 && after.count == before.count + 1, "objimport-placed-one-undo",
          "undo " + std::to_string(before.count) + " -> " + std::to_string(after.count) + " '" + after.current + "'");
    if (r.contains("placed") && !r.at("placed").empty())
    {
        const auto path = r.at("placed")[0].at("path").get<std::string>();
        auto actor = ActorNamed(path.substr(path.rfind('.') + 1));
        const auto at = actor ? ReadVector(actor, "Location") : Vec{};
        std::string mesh;
        if (actor)
            if (auto prop = FindProperty(actor, "StaticMesh")) mesh = ObjectName(*reinterpret_cast<unsigned char**>(actor + *reinterpret_cast<unsigned*>(prop + 0x3C)));
        Check(actor && std::abs(at.x - 128) < 0.01 && std::abs(at.y + 256) < 0.01 && std::abs(at.z - 64) < 0.01 && mesh == "SmokeCrate", "objimport-actor",
              path + " at " + Fixed(at.x) + "," + Fixed(at.y) + "," + Fixed(at.z) + " mesh " + mesh);
    }
    auto again = ImportRequest({{"op", "objimport.import"}, {"file", crateFile}, {"package", "MyLevel"}, {"group", "ObjTest"}, {"name", "SmokeCrate"}, {"scale", 100}});
    Check(!again.value("ok", true) && again.value("error", std::string()).find("exists already") != std::string::npos, "objimport-name-taken", again.dump());

    // Per polygon and no collision.
    auto poly = ImportRequest({{"op", "objimport.import"}, {"file", crateFile}, {"package", "MyLevel"}, {"group", "ObjTest"}, {"name", "SmokePoly"}, {"scale", 100},
                         {"collision", "polygons"}});
    Info("objimport-polygons", poly.dump().substr(0, 1500));
    if (poly.value("ok", false))
    {
        const auto& q = poly.at("result").at("parts").at(0);
        Check(q.at("hullTriangles") == 0 && q.at("polygonCollision").get<int>() > 0, "objimport-per-polygon",
              "hull " + q.at("hullTriangles").dump() + " polygon " + q.at("polygonCollision").dump());
    }
    else Fail("objimport-per-polygon", poly.value("error", std::string()));
    auto none = ImportRequest({{"op", "objimport.import"}, {"file", crateFile}, {"package", "MyLevel"}, {"group", "ObjTest"}, {"name", "SmokeNone"}, {"scale", 100},
                         {"collision", "none"}});
    Info("objimport-none", none.dump().substr(0, 1500));
    if (none.value("ok", false))
    {
        const auto& q = none.at("result").at("parts").at(0);
        bool off = !q.at("materials").empty();
        for (const auto& m : q.at("materials")) off = off && m.contains("collision") && !m.at("collision").get<bool>();
        Check(q.at("hullTriangles") == 0 && q.at("polygonCollision") == 0 && off, "objimport-no-collision",
              "hull " + q.at("hullTriangles").dump() + " polygon " + q.at("polygonCollision").dump() + " " + q.at("materials").dump());
    }
    else Fail("objimport-no-collision", none.value("error", std::string()));

    // Over the limit: split into parts, placed together, one Undo step.
    const auto beforeWall = Undo();
    auto wall = ImportRequest({{"op", "objimport.import"}, {"file", wallFile}, {"package", "MyLevel"}, {"group", "ObjTest"}, {"name", "SmokeWall"}, {"scale", 100},
                         {"collision", "polygons"}, {"split", true}, {"place", true}});
    Info("objimport-split", wall.dump().substr(0, 3000));
    if (wall.value("ok", false))
    {
        const auto& w = wall.at("result");
        bool within = true;
        size_t sum = 0;
        for (const auto& q : w.at("parts"))
        {
            within = within && q.at("triangles").get<size_t>() <= 5512 && q.at("polygonCollision").get<int>() <= 5512 && q.at("polygonNodes").get<int>() < 32767;
            sum += q.at("triangles").get<size_t>();
        }
        const auto afterWall = Undo();
        Check(w.at("parts").size() == 3 && within && sum == 12000 && w.at("placed").size() == 3 && afterWall.count == beforeWall.count + 1, "objimport-split-placed",
              std::to_string(w.at("parts").size()) + " parts, " + std::to_string(sum) + " triangles, placed " + std::to_string(w.at("placed").size()) + ", undo " +
              std::to_string(beforeWall.count) + " -> " + std::to_string(afterWall.count));
    }
    else Fail("objimport-split-placed", wall.value("error", std::string()));
    auto refused = ImportRequest({{"op", "objimport.preview"}, {"file", wallFile}, {"name", "SmokeWall2"}, {"collision", "polygons"}, {"split", false}});
    Check(refused.value("ok", false) && refused.at("result").at("error").get<std::string>().find("crashes the game") != std::string::npos, "objimport-limit-without-split",
          refused.dump().substr(0, 400));

    // The window, from File > Import OBJ Mesh...: the file typed in, its summary read, a picture taken.
    Check(MenuHas(frameWindow, 41640), "objimport-file-menu");
    std::string summary, status;
    int ticks = 0;
    bool opened = false;
    dialogHandler = [&](HWND w, const std::string& title) {
        if (title != "Import OBJ") return false;
        opened = true;
        ++ticks;
        if (ticks == 2)
        {
            SetDlgItemTextA(w, 200, std::string(crate.begin(), crate.end()).c_str());
            SetDlgItemTextA(w, 205, "100");
        }
        if (ticks == 12)
        {
            char text[4096] = {};
            GetDlgItemTextA(w, 210, text, sizeof(text));
            summary = text;
            CaptureWindows(w, nullptr, directory / "objimport-1.bmp");
            // Import from the window too, under another name and not placed.
            SetDlgItemTextA(w, 204, "SmokeDialog");
            CheckDlgButton(w, 209, BST_UNCHECKED);
            PostMessage(w, WM_COMMAND, IDOK, 0);
        }
        if (ticks == 20)
        {
            char text[4096] = {};
            GetDlgItemTextA(w, 211, text, sizeof(text));
            status = text;
            CaptureWindows(w, nullptr, directory / "objimport-2.bmp");
        }
        if (ticks == 21) PostMessage(w, WM_COMMAND, IDCANCEL, 0);
        return true;
    };
    Command(41640);
    dialogHandler = nullptr;
    Check(opened && summary.find("26 triangles") != std::string::npos && summary.find("CrateWood: loaded") != std::string::npos, "objimport-window-summary", summary);
    Check(status.find(".SmokeDialog (26 triangles") != std::string::npos, "objimport-window-import", status);

    // The Static Mesh Browser's File menu has it too, and opens the same window.
    Command(40468);
    Pump(800);
    HWND browser = MenuOwner(41640);
    Check(browser != nullptr, "objimport-browser-menu", browser ? WindowText(browser) : "no window with the command");
    if (browser)
    {
        CaptureWindows(browser, nullptr, directory / "objimport-3.bmp");
        opened = false;
        dialogHandler = [&](HWND w, const std::string& title) {
            if (title != "Import OBJ") return false;
            opened = true;
            PostMessage(w, WM_COMMAND, IDCANCEL, 0);
            return true;
        };
        Command(41640, browser);
        dialogHandler = nullptr;
        Check(opened, "objimport-browser-opens-window");
    }
}

// ---- Reachability: Build > Reachability Map works out where each team walks, a few steps at a
// time; Map Check shows that it is being worked out and refreshes with its rows when it ends; the
// menu item cancels a run going on.
// A window as it is drawn, saved as a 24-bit BMP (shot=<folder> in -Extra).
void SaveShot(HWND w, const std::string& name)
{
    const auto folder = Ini("shot");
    if (folder.empty() || !w) return;
    RECT r{};
    GetWindowRect(w, &r);
    const int width = r.right - r.left, height = r.bottom - r.top;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, height, 1, 24, BI_RGB};
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    auto old = SelectObject(dc, bitmap);
    // The full content first (the Direct3D viewports); a plain print when that comes back black.
    const DWORD stride = (static_cast<DWORD>(width) * 3 + 3) & ~3u, size = stride * static_cast<DWORD>(height);
    auto black = [&] { GdiFlush(); for (DWORD i = 0; i < size; i += 97) if (static_cast<unsigned char*>(bits)[i]) return false; return true; };
    if (!PrintWindow(w, dc, 2) || black()) PrintWindow(w, dc, 0);
    Info("shot", name + (black() ? " black" : " ok"));
    BITMAPFILEHEADER file{0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + size), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    FILE* f = nullptr;
    if (!fopen_s(&f, (folder + "\\" + name + ".bmp").c_str(), "wb") && f)
    {
        fwrite(&file, sizeof(file), 1, f);
        fwrite(&info.bmiHeader, sizeof(info.bmiHeader), 1, f);
        fwrite(bits, 1, size, f);
        fclose(f);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}
std::vector<std::string> ReachRows(HWND check)
{
    std::vector<std::string> rows;
    for (const auto& row : ListRows(GetDlgItem(check, 100)))
        if (row.find("| Reachability |") != std::string::npos) rows.push_back(row);
    return rows;
}
bool ReachChecked()
{
    return (GetMenuState(GetMenu(frameWindow), 41480, MF_BYCOMMAND) & MF_CHECKED) != 0;
}
void ReachSuite()
{
    Info("command", "41220 " + Fixed(Command(41220)) + " ms");
    Pump(500);
    HWND check = OpenedWindow("Map Check");
    if (!check) { Fail("reach-mapcheck-open", "Map Check did not open"); return; }
    auto rows = ReachRows(check);
    Check(!rows.empty() && rows[0].find("being worked out") != std::string::npos, "reach-mapcheck-working", rows.empty() ? "no rows" : rows[0]);
    Info("command", "41480 " + Fixed(Command(41480)) + " ms");
    Check(ReachChecked(), "reach-menu-checked");
    const DWORD started = GetTickCount();
    bool done = false;
    while (GetTickCount() - started < 600000)
    {
        Pump(1000);
        rows = ReachRows(check);
        if (!rows.empty() && rows.back().find("Worked out on foot") != std::string::npos) { done = true; break; }
    }
    Check(done, "reach-finished", Fixed((GetTickCount() - started) / 1000.0) + " s; " + std::to_string(rows.size()) + " rows");
    for (const auto& row : rows) Info("reach-row", row);
    if (done)
    {
        const auto& summary = rows.back();
        const auto spies = summary.find("spies "), mercs = summary.find("mercs ");
        Check(spies != std::string::npos && mercs != std::string::npos && Number(summary.substr(spies, 16)) > 100 && Number(summary.substr(mercs, 16)) > 100,
              "reach-both-teams", summary.substr(spies == std::string::npos ? 0 : spies, 60));
        // A row selects what it is about.
        for (size_t i = 0; i + 1 < rows.size(); ++i)
        {
            if (Cells(rows[i]).back() == "-") continue;
            const auto all = ListRows(GetDlgItem(check, 100));
            const auto at = std::find(all.begin(), all.end(), rows[i]) - all.begin();
            Exec("SELECT NONE");
            ClickRow(check, 100, static_cast<int>(at), false);
            Check(Selected() >= 1, "reach-row-selects", Cells(rows[i]).back() + " selected=" + std::to_string(Selected()));
            Exec("SELECT NONE");
            break;
        }
        Pump(1500);
        SaveShot(frameWindow, "reach-viewports");
        SaveShot(check, "reach-mapcheck");
    }
    // Off hides it; on again with the map unchanged shows the same result at once.
    Info("command", "41480 " + Fixed(Command(41480)) + " ms");
    Check(!ReachChecked(), "reach-menu-unchecked");
    Info("command", "41480 " + Fixed(Command(41480)) + " ms");
    Pump(300);
    rows = ReachRows(check);
    Check(ReachChecked() && !rows.empty(), "reach-on-again");
    // The second map (ShipD): on starts a run, off cancels it and Map Check says so; on again
    // works it out, and Map Check refreshes with its rows.
    if (Ini("map2").empty()) { Info("reach-map2", "no map2: cancel and second map skipped"); CloseWindowAndWait(check); return; }
    boxAnswer = IDNO;
    Command(40971);
    boxAnswer = IDOK;
    Pump(1500);
    Info("reach-map2", "actors=" + std::to_string(Actors().size()));
    if (ReachChecked()) Command(41480);
    Info("command", "41480 on " + Fixed(Command(41480)) + " ms");
    Pump(1500);
    Info("command", "41480 off " + Fixed(Command(41480)) + " ms");
    Info("command", "41220 " + Fixed(Command(41220)) + " ms");
    Pump(300);
    rows = ReachRows(check);
    Check(!rows.empty() && rows.back().find("Cancelled") != std::string::npos, "reach-cancel", rows.empty() ? "no rows" : rows.back());
    Info("command", "41480 on " + Fixed(Command(41480)) + " ms");
    Info("command", "41220 " + Fixed(Command(41220)) + " ms");
    const DWORD second = GetTickCount();
    done = false;
    while (GetTickCount() - second < 600000)
    {
        Pump(1000);
        rows = ReachRows(check);
        if (!rows.empty() && rows.back().find("Worked out on foot") != std::string::npos) { done = true; break; }
    }
    Check(done, "reach-map2-finished", Fixed((GetTickCount() - second) / 1000.0) + " s; " + std::to_string(rows.size()) + " rows");
    for (const auto& row : rows) Info("reach-map2-row", row);
    Pump(2000);
    SaveShot(frameWindow, "reach-map2-viewports");
    SaveShot(check, "reach-map2-mapcheck");
    // Map Design's plan layer: Tools > Show reachability map (its window's command 777).
    Info("command", "40948 " + Fixed(Command(40948)) + " ms");
    Pump(1500);
    if (HWND design = OpenedWindow("Map Design"))
    {
        SendMessageA(design, WM_COMMAND, 777, 0);
        Pump(1500);
        const auto status = WindowText(GetDlgItem(design, 720));
        const bool checked = (GetMenuState(GetMenu(design), 777, MF_BYCOMMAND) & MF_CHECKED) != 0;
        Check(checked && status.find("Reachability map: green both teams") != std::string::npos, "reach-map-design-layer", status.substr(0, 160));
        SaveShot(design, "reach-map2-design");
    }
    else Fail("reach-map-design-layer", "Map Design did not open");
    CloseWindowAndWait(check);
}

// ---- Map Outliner: every actor listed in a virtual list, a search over all columns, grouping, a
// click selecting in the editor and the list following the editor's selection, Hide / Show in one
// Undo step each, Delete asking first and deleting in one Undo step that Undo brings back, and the
// window reopening where it was.
namespace OutlinerIds { enum : int { kSearch = 101, kGroupBy = 103, kFollow = 104, kList = 105, kStatus = 106, kHide = 110, kShow, kShowAll, kDelete, kFrame, kRefresh }; }
int ListCount(HWND list) { return static_cast<int>(SendMessageA(list, LVM_GETITEMCOUNT, 0, 0)); }
std::vector<int> ListSelection(HWND list)
{
    std::vector<int> rows;
    for (int i = static_cast<int>(SendMessageA(list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED)); i >= 0;
         i = static_cast<int>(SendMessageA(list, LVM_GETNEXTITEM, static_cast<WPARAM>(i), LVNI_SELECTED)))
        rows.push_back(i);
    return rows;
}
void SelectListRows(HWND list, const std::vector<int>& rows)
{
    LVITEMA clear{};
    clear.stateMask = LVIS_SELECTED;
    SendMessageA(list, LVM_SETITEMSTATE, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&clear));
    for (int row : rows)
    {
        LVITEMA item{};
        item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        item.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageA(list, LVM_SETITEMSTATE, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
    }
    Pump(400);
}
// The list row whose Name column is exactly this, or -1.
int RowNamed(const std::vector<std::string>& rows, const std::string& name)
{
    for (size_t i = 0; i < rows.size(); ++i)
    {
        auto cell = Cells(rows[i])[0];
        cell.erase(0, cell.find_first_not_of(' '));
        if (cell == name) return static_cast<int>(i);
    }
    return -1;
}
void Search(HWND outliner, const std::string& text)
{
    HWND edit = GetDlgItem(outliner, OutlinerIds::kSearch);
    SetWindowTextA(edit, text.c_str());
    SendMessageA(outliner, WM_COMMAND, MAKEWPARAM(OutlinerIds::kSearch, EN_CHANGE), reinterpret_cast<LPARAM>(edit));
    Pump(200);
}
void GroupOutliner(HWND outliner, int by)
{
    HWND combo = GetDlgItem(outliner, OutlinerIds::kGroupBy);
    SendMessageA(combo, CB_SETCURSEL, by, 0);
    SendMessageA(outliner, WM_COMMAND, MAKEWPARAM(OutlinerIds::kGroupBy, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    Pump(200);
}
std::string Lowered(std::string text)
{
    for (auto& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}

void OutlinerSuite()
{
    // On the View menu and RE+ Tools.
    std::string menus;
    HMENU bar = GetMenu(frameWindow);
    for (int i = 0; bar && i < GetMenuItemCount(bar); ++i)
        if (HMENU sub = GetSubMenu(bar, i); sub && GetMenuState(sub, 41560, MF_BYCOMMAND) != UINT(-1))
        {
            char text[64] = {};
            GetMenuStringA(bar, i, text, sizeof(text), MF_BYPOSITION);
            menus += std::string(menus.empty() ? "" : ", ") + text;
        }
    Check(menus.find("View") != std::string::npos && menus.find("RE+") != std::string::npos, "outliner-menus", menus);
    Exec("SELECT NONE");
    Pump(200);
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    Command(41560);
    Pump(500);
    QueryPerformanceCounter(&b);
    HWND outliner = OpenedWindow("Map Outliner");
    if (!outliner) { Fail("outliner-open", "did not open; boxes=" + std::to_string(boxes.size())); return; }
    HWND list = GetDlgItem(outliner, OutlinerIds::kList);
    const bool virtualList = (GetWindowLongA(list, GWL_STYLE) & LVS_OWNERDATA) != 0;
    int cameras = 0;
    for (auto actor : Actors()) cameras += ClassName(actor) == "Camera";
    const int actors = static_cast<int>(Actors().size()) - cameras;
    Info("outliner-open", "ms=" + Fixed(1000.0 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart)) + " actors=" + std::to_string(actors)
         + " status '" + WindowText(GetDlgItem(outliner, OutlinerIds::kStatus)) + "'");

    // Grouped by nothing: one line per actor, the viewport cameras left out.
    GroupOutliner(outliner, 0);
    auto rows = ListRows(list);
    Check(virtualList && static_cast<int>(rows.size()) == actors, "outliner-lists-every-actor",
          std::to_string(rows.size()) + " rows for " + std::to_string(actors) + " actors (" + std::to_string(cameras) + " cameras) virtual=" + std::to_string(virtualList));
    for (size_t i = 0; i < rows.size() && i < 4; ++i) Info("outliner-row", rows[i]);

    // Grouped by class: a header per class with its count, the rows under it.
    GroupOutliner(outliner, 1);
    auto grouped = ListRows(list);
    size_t headers = 0;
    long long counted = 0;
    for (const auto& row : grouped)
        if (row.rfind("[-] ", 0) == 0) { ++headers; counted += Number(row.substr(row.rfind('('))); }
    Check(headers > 1 && static_cast<int>(grouped.size()) == actors + static_cast<int>(headers) && counted == actors, "outliner-group-by-class",
          std::to_string(headers) + " classes, " + std::to_string(counted) + " counted, " + std::to_string(grouped.size()) + " lines");
    // Collapsing a class (double-click its header) keeps the header and hides its rows.
    ClickRow(outliner, OutlinerIds::kList, 0, true);
    const int collapsed = ListCount(list);
    const auto firstHeader = ListRows(list).empty() ? std::string() : ListRows(list)[0];
    Check(firstHeader.rfind("[+] ", 0) == 0 && collapsed < static_cast<int>(grouped.size()), "outliner-collapse", firstHeader + " lines " + std::to_string(collapsed));
    ClickRow(outliner, OutlinerIds::kList, 0, true);
    Check(ListCount(list) == static_cast<int>(grouped.size()), "outliner-expand", std::to_string(ListCount(list)));
    GroupOutliner(outliner, 0);

    // Search: every word must appear in some column.
    auto meshes = FreeMeshes(6, 30);
    if (meshes.size() < 6) { Fail("outliner-meshes", "the map needs six free static meshes"); CloseWindowAndWait(outliner); return; }
    Search(outliner, "staticmeshactor");
    rows = ListRows(list);
    bool all = !rows.empty();
    for (const auto& row : rows) all = all && Lowered(row).find("staticmeshactor") != std::string::npos;
    Check(all && static_cast<int>(rows.size()) < actors, "outliner-search", std::to_string(rows.size()) + " rows");
    const std::string named = Name(meshes[0]);
    Search(outliner, "  StaticMeshActor   " + named + " ");
    rows = ListRows(list);
    all = RowNamed(rows, named) >= 0;
    for (const auto& row : rows) all = all && row.find(named) != std::string::npos;
    Check(all, "outliner-search-words-narrow", named + ": " + std::to_string(rows.size()) + " rows");
    Search(outliner, "");
    rows = ListRows(list);
    Check(static_cast<int>(rows.size()) == actors, "outliner-search-cleared", std::to_string(rows.size()));

    // A click on three rows selects those three actors in the editor.
    std::vector<int> three;
    for (int i = 0; i < 3; ++i) three.push_back(RowNamed(rows, Name(meshes[i])));
    SelectListRows(list, three);
    int on = 0;
    for (int i = 0; i < 3; ++i) on += (Flags(meshes[i]) & 0x40) != 0;
    Check(on == 3 && Selected() == 3, "outliner-click-selects", "selected " + std::to_string(Selected()) + " of them " + std::to_string(on)
          + " status '" + WindowText(GetDlgItem(outliner, OutlinerIds::kStatus)) + "'");

    // Follow selection: a mesh selected in the editor becomes the list's selection.
    Exec("SELECT NONE");
    SetSelected(meshes[3], true);
    Pump(800);
    auto chosen = ListSelection(list);
    Check(chosen.size() == 1 && chosen[0] == RowNamed(rows, Name(meshes[3])), "outliner-follows-selection", std::to_string(chosen.size()) + " row(s)");

    // Double-click frames.
    ClickRow(outliner, OutlinerIds::kList, RowNamed(rows, Name(meshes[4])), true);
    const auto framed = WindowText(GetDlgItem(outliner, OutlinerIds::kStatus));
    Check(framed.find("framed") != std::string::npos && (Flags(meshes[4]) & 0x40), "outliner-double-click-frames", framed);

    // Hide two, one Undo step; the rows say Hidden; Show brings them back.
    SelectListRows(list, {RowNamed(rows, Name(meshes[0])), RowNamed(rows, Name(meshes[1]))});
    auto before = Undo();
    Command(OutlinerIds::kHide, outliner);
    Pump(500);
    auto after = Undo();
    rows = ListRows(list);
    const int hiddenRow = RowNamed(rows, Name(meshes[0]));
    const bool marked = hiddenRow >= 0 && rows[hiddenRow].find("Hidden") != std::string::npos;
    Check((Flags(meshes[0]) & 0x10) && (Flags(meshes[1]) & 0x10) && after.count == before.count + 1 && marked, "outliner-hide",
          "undo '" + after.current + "' row '" + (hiddenRow >= 0 ? rows[hiddenRow] : std::string()) + "'");
    // A hidden row is never selected in the editor.
    SelectListRows(list, {RowNamed(rows, Name(meshes[0]))});
    Check(!(Flags(meshes[0]) & 0x40), "outliner-hidden-not-selected", WindowText(GetDlgItem(outliner, OutlinerIds::kStatus)));
    SelectListRows(list, {RowNamed(rows, Name(meshes[0])), RowNamed(rows, Name(meshes[1]))});
    before = Undo();
    Command(OutlinerIds::kShow, outliner);
    Pump(500);
    after = Undo();
    Check(!(Flags(meshes[0]) & 0x10) && !(Flags(meshes[1]) & 0x10) && after.count == before.count + 1, "outliner-show", "undo '" + after.current + "'");

    // Delete: answered No, nothing goes; through the list's Delete key.
    const std::string gone0 = Name(meshes[0]), gone1 = Name(meshes[1]);
    rows = ListRows(list);
    SelectListRows(list, {RowNamed(rows, gone0), RowNamed(rows, gone1)});
    const size_t actorsBefore = Actors().size();
    before = Undo();
    boxAnswer = IDNO;
    boxes.clear();
    {
        const UINT_PTR answering = SetTimer(nullptr, 0, 100, AnswerBoxes);
        NMLVKEYDOWN key{};
        key.hdr.hwndFrom = list;
        key.hdr.idFrom = OutlinerIds::kList;
        key.hdr.code = LVN_KEYDOWN;
        key.wVKey = VK_DELETE;
        SendMessageA(outliner, WM_NOTIFY, OutlinerIds::kList, reinterpret_cast<LPARAM>(&key));
        KillTimer(nullptr, answering);
    }
    boxAnswer = IDOK;
    Pump(300);
    Check(BoxContains("Delete these 2 actors?") && Actors().size() == actorsBefore && Undo().count == before.count, "outliner-delete-asks",
          "boxes=" + std::to_string(boxes.size()) + (boxes.empty() ? "" : " " + boxes[0]));
    // Answered Yes: both go in one Undo step, and Undo brings them back.
    boxAnswer = IDYES;
    Command(OutlinerIds::kDelete, outliner);
    boxAnswer = IDOK;
    Pump(500);
    after = Undo();
    rows = ListRows(list);
    Check(!ActorNamed(gone0) && !ActorNamed(gone1) && Actors().size() == actorsBefore - 2 && after.count == before.count + 1
              && RowNamed(rows, gone0) < 0 && static_cast<int>(rows.size()) == actors - 2,
          "outliner-delete-one-step", "undo '" + after.current + "' actors " + std::to_string(actorsBefore) + "->" + std::to_string(Actors().size())
          + " rows " + std::to_string(rows.size()) + " boxes=" + std::to_string(boxes.size()));
    Command(40019); // Edit > Undo
    Pump(800);
    rows = ListRows(list);
    Check(ActorNamed(gone0) && ActorNamed(gone1) && Actors().size() == actorsBefore && RowNamed(rows, gone0) >= 0, "outliner-delete-undo",
          "actors " + std::to_string(Actors().size()) + " rows " + std::to_string(rows.size()));

    // The level info cannot be deleted.
    GroupOutliner(outliner, 0);
    Search(outliner, "levelinfo");
    rows = ListRows(list);
    if (!rows.empty())
    {
        SelectListRows(list, {0});
        Command(OutlinerIds::kDelete, outliner);
        Pump(200);
        Check(boxes.empty() && WindowText(GetDlgItem(outliner, OutlinerIds::kStatus)).find("cannot be deleted") != std::string::npos, "outliner-keeps-level-info",
              WindowText(GetDlgItem(outliner, OutlinerIds::kStatus)));
    }
    Search(outliner, "");

    // Reading the whole map again is quick.
    const double ms = Command(OutlinerIds::kRefresh, outliner);
    Check(ms < 2000, "outliner-refresh-fast", "ms=" + Fixed(ms) + " for " + std::to_string(actors) + " actors");

    // It reopens where it was left.
    MONITORINFO main{sizeof(main)};
    GetMonitorInfoA(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &main);
    SetWindowPos(outliner, nullptr, main.rcWork.left + 97, main.rcWork.top + 61, 811, 533, SWP_NOZORDER | SWP_NOACTIVATE);
    Pump(150);
    const RECT moved = NormalRect(outliner);
    CloseWindowAndWait(outliner);
    Command(41560);
    Pump(300);
    HWND again = OpenedWindow("Map Outliner");
    Check(again && SameRect(NormalRect(again), moved), "outliner-placement", "moved " + RectText(moved) + " back " + (again ? RectText(NormalRect(again)) : std::string("none")));
    // shot=1 in -Extra: grouped by class and left open for a while, for a picture taken from outside.
    if (again && Ini("shot") == "1")
    {
        GroupOutliner(again, 1);
        Exec("SELECT NONE");
        for (int i = 2; i < 6; ++i) SetSelected(meshes[i], true);
        Info("outliner-shot", "window " + std::to_string(reinterpret_cast<uintptr_t>(again)));
        Pump(15000);
    }
    if (again) CloseWindowAndWait(again);
}

// ---- Scatter Props: a circle round a player start and the builder brush's footprint are filled with a small
// mesh from the map, placed as one Undo step in the Scatter group, and one Undo takes them all away again.
namespace scatter
{
constexpr int kList = 100, kAddBrowser = 101, kWeight = 105, kCount = 111, kArea = 120, kRadius = 121, kAlign = 132,
              kScaleMin = 140, kScaleMax = 141, kSpacing = 142, kSeed = 150, kReroll = 151, kStatus = 160, kPlace = 170;
unsigned char* MeshOf(unsigned char* actor)
{
    auto p = FindProperty(actor, "StaticMesh");
    return p ? *reinterpret_cast<unsigned char**>(actor + *reinterpret_cast<unsigned*>(p + 0x3C)) : nullptr;
}
float Extent(unsigned char* mesh)
{
    auto b = reinterpret_cast<float*>(mesh + 0x28);
    return (std::max)({b[3] - b[0], b[4] - b[1], b[5] - b[2]});
}
std::string GroupOf(unsigned char* actor)
{
    auto p = FindProperty(actor, "Group");
    return p ? NameText(*reinterpret_cast<int*>(actor + *reinterpret_cast<unsigned*>(p + 0x3C))) : "";
}
void Click(HWND w, int id) { SendMessageA(w, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(w, id))); Pump(200); }
void Type(HWND w, int id, const std::string& text) { SetWindowTextA(GetDlgItem(w, id), text.c_str()); Pump(50); }
void Choose(HWND w, int id, int index)
{
    SendMessageA(GetDlgItem(w, id), CB_SETCURSEL, index, 0);
    SendMessageA(w, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(GetDlgItem(w, id)));
    Pump(50);
}
std::vector<unsigned char*> Props()
{
    std::vector<unsigned char*> result;
    for (auto a : Actors()) if (ClassName(a) == "StaticMeshActor" && Name(a).rfind("ScatterProp", 0) == 0) result.push_back(a);
    return result;
}
// Place, then check the new props and that one Undo removes them (and Redo brings them back, Undo again).
void PlaceAndUndo(HWND w, const std::string& label, const std::function<bool(Vec)>& inside, unsigned char* mesh)
{
    Pump(1500);
    const auto status = WindowText(GetDlgItem(w, kStatus));
    Info(label + "-preview", status);
    const auto before = Props().size();
    const int actors = static_cast<int>(Actors().size());
    const auto undo = Undo();
    Click(w, kPlace);
    Pump(500);
    const auto after = Props();
    const auto placed = after.size() - before;
    const auto now = Undo();
    bool grouped = true, within = true, meshes = true, shown = true;
    std::string where;
    for (size_t i = before; i < after.size(); ++i)
    {
        grouped = grouped && GroupOf(after[i]) == "Scatter";
        const auto at = ReadVector(after[i], "Location");
        within = within && inside(at);
        meshes = meshes && MeshOf(after[i]) == mesh;
        shown = shown && !(Flags(after[i]) & 0x10);
        if (i < before + 3) where += VecText(at) + " ";
    }
    Check(placed > 0 && static_cast<int>(Actors().size()) == actors + static_cast<int>(placed) && Selected() == static_cast<int>(placed), label + "-placed",
          std::to_string(placed) + " props, selected " + std::to_string(Selected()) + " at " + where + "status '" + WindowText(GetDlgItem(w, kStatus)) + "'");
    Check(grouped && meshes, label + "-group-and-mesh", "grouped=" + std::to_string(grouped) + " mesh=" + std::to_string(meshes));
    Check(within, label + "-inside-area");
    Check(shown, label + "-not-hidden");
    Check(now.current == "Scatter props" && now.count - now.undone == undo.count - undo.undone + 1, label + "-one-undo-step",
          "'" + now.current + "' steps " + std::to_string(undo.count - undo.undone) + "->" + std::to_string(now.count - now.undone));
    Exec("TRANSACTION UNDO");
    Pump(300);
    Check(Props().size() == before && static_cast<int>(Actors().size()) == actors, label + "-undo-removes-all",
          std::to_string(Props().size()) + " props left, actors " + std::to_string(Actors().size()) + " (was " + std::to_string(actors) + ")");
    Exec("TRANSACTION REDO");
    Pump(300);
    Check(Props().size() == before + placed, label + "-redo-brings-back", std::to_string(Props().size()));
    Exec("TRANSACTION UNDO");
    Pump(300);
    Check(Props().size() == before, label + "-undo-again", std::to_string(Props().size()));
}
}

void ScatterSuite()
{
    using namespace scatter;
    // A small mesh from the map becomes the Static Mesh Browser's current one.
    unsigned char* mesh = nullptr;
    for (auto a : OfClass("StaticMeshActor"))
        if (auto m = MeshOf(a); m && Extent(m) >= 16 && Extent(m) <= 96) { mesh = m; break; }
    if (!mesh) { Fail("scatter-mesh", "no small static mesh in the map"); return; }
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    *reinterpret_cast<unsigned char**>(editor + 0x13C) = mesh;
    // A player start's spot is open floor: the last click goes there.
    unsigned char* start = nullptr;
    for (auto a : Actors()) if (ClassName(a).find("PlayerStart") != std::string::npos) { start = a; break; }
    if (!start) { Fail("scatter-start", "no player start in the map"); return; }
    auto spot = ReadVector(start, "Location");
    auto unrealEd = *reinterpret_cast<unsigned char**>(0x117a59b0);
    *reinterpret_cast<Vec*>(unrealEd + 0x1bc) = spot;
    Info("scatter-setup", ObjectName(mesh) + std::string(" extent ") + Fixed(Extent(mesh)) + " at " + ClassName(start) + " " + VecText(spot));

    Exec("SELECT NONE");
    Command(41500);
    Pump(500);
    HWND w = OpenedWindow("Scatter Props");
    if (!w) { Fail("scatter-open", "the window did not open; boxes=" + std::to_string(boxes.size())); return; }
    Pass("scatter-open");
    // An empty list explains itself.
    Pump(600);
    const int rowsAtStart = static_cast<int>(SendMessageA(GetDlgItem(w, kList), LVM_GETITEMCOUNT, 0, 0));
    Info("scatter-empty-status", WindowText(GetDlgItem(w, kStatus)));
    Click(w, kAddBrowser);
    const int rows = static_cast<int>(SendMessageA(GetDlgItem(w, kList), LVM_GETITEMCOUNT, 0, 0));
    Check(rowsAtStart == 0 && rows == 1, "scatter-add-browser-mesh", std::to_string(rowsAtStart) + "->" + std::to_string(rows));
    // The same mesh again is refused.
    Click(w, kAddBrowser);
    Check(SendMessageA(GetDlgItem(w, kList), LVM_GETITEMCOUNT, 0, 0) == 1 && WindowText(GetDlgItem(w, kStatus)).find("already") != std::string::npos,
          "scatter-no-duplicate", WindowText(GetDlgItem(w, kStatus)));

    // Re-roll changes the seed.
    Type(w, kSeed, "12345");
    Click(w, kReroll);
    Check(WindowText(GetDlgItem(w, kSeed)) != "12345", "scatter-reroll", WindowText(GetDlgItem(w, kSeed)));

    // A circle of 384 round the start, 12 props, scaled 0.8 to 1.2, 8 apart.
    Choose(w, kArea, 1);
    Type(w, kRadius, "384");
    Type(w, kCount, "12");
    Type(w, kScaleMin, "0.8");
    Type(w, kScaleMax, "1.2");
    Type(w, kSpacing, "8");
    Type(w, kSeed, "777");
    PlaceAndUndo(w, "scatter-circle", [&](Vec v) { return std::hypot(v.x - spot.x, v.y - spot.y) <= 384 + 1; }, mesh);

    // The same with align to slope.
    SendMessageA(GetDlgItem(w, kAlign), BM_SETCHECK, BST_CHECKED, 0);
    Click(w, kAlign);
    SendMessageA(GetDlgItem(w, kAlign), BM_SETCHECK, BST_CHECKED, 0);
    PlaceAndUndo(w, "scatter-align", [&](Vec v) { return std::hypot(v.x - spot.x, v.y - spot.y) <= 384 + 1; }, mesh);
    SendMessageA(GetDlgItem(w, kAlign), BM_SETCHECK, BST_UNCHECKED, 0);
    Click(w, kAlign);

    // The builder brush's footprint: the default cube, moved over the start.
    auto builder = Actors().size() > 1 ? Actors()[1] : nullptr;
    if (builder && ClassName(builder) == "Brush")
    {
        Command(40911);
        WriteVector(builder, "Location", spot);
        Pump(200);
        Choose(w, kArea, 0);
        Type(w, kCount, "6");
        Type(w, kSpacing, "4");
        Info("scatter-builder", VecText(ReadVector(builder, "Location")));
        PlaceAndUndo(w, "scatter-builder", [&](Vec v) { return std::fabs(v.x - spot.x) <= 300 && std::fabs(v.y - spot.y) <= 300; }, mesh);
    }
    else Fail("scatter-builder", "no builder brush");

    // A count past the limit says so and places nothing.
    Type(w, kCount, "5000");
    Pump(800);
    const auto before = Props().size();
    Click(w, kPlace);
    Check(Props().size() == before && WindowText(GetDlgItem(w, kStatus)).find("between 1 and 500") != std::string::npos, "scatter-count-limit", WindowText(GetDlgItem(w, kStatus)));
    Type(w, kCount, "12");
    Choose(w, kArea, 1);
    Pump(1500);
    // hold=<ms> in -Extra: the viewport cameras look down on the start and the editor keeps running that long
    // after the report with the window open, for a picture (a new seed a second later redraws the preview).
    const auto hold = Ini("hold");
    if (!hold.empty())
    {
        // The player start with the most free floor round it.
        Type(w, kRadius, "448");
        Type(w, kCount, "24");
        int best = -1;
        Vec chosen = spot;
        for (auto a : Actors())
        {
            if (ClassName(a).find("PlayerStart") == std::string::npos) continue;
            const auto at = ReadVector(a, "Location");
            *reinterpret_cast<Vec*>(unrealEd + 0x1bc) = at;
            Type(w, kSeed, std::to_string(500 + best));
            Pump(900);
            const int fit = static_cast<int>(Number(WindowText(GetDlgItem(w, kStatus)).substr(0, 3)));
            if (fit > best) { best = fit; chosen = at; }
        }
        *reinterpret_cast<Vec*>(unrealEd + 0x1bc) = chosen;
        spot = chosen;
        for (auto a : Actors())
            if (ClassName(a) == "Camera")
            {
                WriteVector(a, "Location", {spot.x - 450, spot.y, spot.z + 380});
                if (auto p = FindProperty(a, "Rotation")) *reinterpret_cast<Rot*>(a + *reinterpret_cast<unsigned*>(p + 0x3C)) = {-7000, 0, 0};
            }
        Type(w, kRadius, "448");
        Type(w, kCount, "24");
        Pump(1500);
        Info("scatter-hold", hold + " " + WindowText(GetDlgItem(w, kStatus)));
        static HWND held;
        held = w;
        SetTimer(nullptr, 0, 1000, [](HWND, UINT, UINT_PTR id, DWORD) { KillTimer(nullptr, id); SetWindowTextA(GetDlgItem(held, kSeed), "778"); });
        // Then Place, for a picture of the meshes themselves.
        SetTimer(nullptr, 0, 9000, [](HWND, UINT, UINT_PTR id, DWORD) { KillTimer(nullptr, id); SendMessageA(held, WM_COMMAND, MAKEWPARAM(kPlace, BN_CLICKED), 0); });
        finishAfter = static_cast<DWORD>(std::stoul(hold));
        return;
    }
    // Closed and opened again: where it was left, with its mesh list, and no preview left behind.
    SetWindowPos(w, nullptr, 180, 140, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    RECT was{};
    GetWindowRect(w, &was);
    CloseWindowAndWait(w);
    Check(!IsWindow(w), "scatter-close");
    Command(41500);
    Pump(500);
    if (HWND again = OpenedWindow("Scatter Props"))
    {
        RECT now{};
        GetWindowRect(again, &now);
        Check(now.left == was.left && now.top == was.top && SendMessageA(GetDlgItem(again, kList), LVM_GETITEMCOUNT, 0, 0) == 1, "scatter-reopen",
              std::to_string(now.left) + "," + std::to_string(now.top) + " was " + std::to_string(was.left) + "," + std::to_string(was.top));
        CloseWindowAndWait(again);
    }
    else Fail("scatter-reopen", "did not open again");
}

// ---- Compare with Backup: File > Save keeps a backup; then a mesh is moved, another deleted and a light's
// brightness changed. The comparison lists all three, with the backup's value beside the map's; one ticked row
// (the deleted mesh) comes back as one Undo step and Undo takes it away again; then Tick All brings back the
// rest too.
constexpr UINT CompareBackupCommand = 41600; // File > Compare with Backup...
// With shot=<file.bmp> in -Extra: the editor frame with a tool window drawn over it where it is, both by
// PrintWindow (never the screen).
void SaveFrameShot(HWND tool, const std::string& file)
{
    RECT frameRect{}, toolRect{};
    GetWindowRect(frameWindow, &frameRect);
    GetWindowRect(tool, &toolRect);
    const int w = frameRect.right - frameRect.left, h = frameRect.bottom - frameRect.top;
    HDC screen = GetDC(nullptr);
    HDC canvas = CreateCompatibleDC(screen), part = CreateCompatibleDC(screen);
    HBITMAP image = CreateCompatibleBitmap(screen, w, h);
    HBITMAP toolImage = CreateCompatibleBitmap(screen, toolRect.right - toolRect.left, toolRect.bottom - toolRect.top);
    auto oldCanvas = SelectObject(canvas, image);
    auto oldPart = SelectObject(part, toolImage);
    PrintWindow(frameWindow, canvas, 2);
    PrintWindow(tool, part, 2);
    BitBlt(canvas, toolRect.left - frameRect.left, toolRect.top - frameRect.top, toolRect.right - toolRect.left, toolRect.bottom - toolRect.top, part, 0, 0, SRCCOPY);
    SelectObject(part, oldPart);
    SelectObject(canvas, oldCanvas);
    BITMAPINFOHEADER header{sizeof(header), w, -h, 1, 24, BI_RGB};
    const int stride = (w * 3 + 3) & ~3;
    std::vector<unsigned char> pixels(static_cast<size_t>(stride) * h);
    GetDIBits(canvas, image, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
    BITMAPFILEHEADER fileHeader{0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(header) + pixels.size()), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(header)};
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    DeleteObject(image);
    DeleteObject(toolImage);
    DeleteDC(canvas);
    DeleteDC(part);
    ReleaseDC(nullptr, screen);
    Info("shot", file);
}
std::filesystem::path NewestBackup(const std::filesystem::path& folder)
{
    std::filesystem::path newest;
    std::filesystem::file_time_type when{};
    std::error_code e;
    for (auto& f : std::filesystem::directory_iterator(folder, e))
        if (f.is_regular_file() && f.path().extension() == ".sdc" && (newest.empty() || f.last_write_time() >= when)) { newest = f.path(); when = f.last_write_time(); }
    return newest;
}
struct CompareRun { bool listed = false; std::vector<std::string> rows; std::string status; int ticked = -1; };
// Opens File > Compare with Backup on this file; tick(rows, list) ticks rows in the list and returns whether to bring back.
CompareRun RunCompare(const std::filesystem::path& backup, const std::function<bool(const std::vector<std::string>&, HWND)>& tick)
{
    CompareRun run;
    int fileTicks = 0, windowTicks = 0;
    bool bring = false, busy = false;
    dialogHandler = [&](HWND w, const std::string& title) {
        if (busy) return true; // tick() pumps messages; the timer comes back in meanwhile.
        if (title == "Choose a Backup to Compare") return AnswerFile(w, backup.wstring(), fileTicks);
        char cls[64] = {};
        GetClassNameA(w, cls, sizeof(cls));
        if (title != "Compare with Backup" || strcmp(cls, "ReloadedMapAuthoringPreview")) return false;
        ++windowTicks;
        HWND list = GetDlgItem(w, 11);
        if (windowTicks == 3)
        {
            run.listed = true;
            run.rows = ListRows(list);
            busy = true;
            bring = tick(run.rows, list);
            busy = false;
            Pump(200);
            run.status = WindowText(GetDlgItem(w, 12));
        }
        if (windowTicks == 6) PostMessage(w, WM_COMMAND, bring ? IDOK : IDCANCEL, 0);
        return true;
    };
    Command(CompareBackupCommand);
    dialogHandler = nullptr;
    return run;
}
void TickRow(HWND list, int row)
{
    LVITEMA item{};
    item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    item.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageA(list, LVM_SETITEMSTATE, row, reinterpret_cast<LPARAM>(&item));
    Pump(100);
    SendMessageA(list, WM_KEYDOWN, VK_SPACE, 0);
    SendMessageA(list, WM_KEYUP, VK_SPACE, 0);
}
int RowIndex(const std::vector<std::string>& rows, const std::string& change, const std::string& target, const std::string& property = "")
{
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto cells = Cells(rows[i]);
        if (cells.size() >= 5 && cells[0] == change && cells[1] == target && (property.empty() || cells[2].rfind(property, 0) == 0)) return static_cast<int>(i);
    }
    return -1;
}
void CompareSuite()
{
    const auto mapName = std::filesystem::path(Ini("map")).stem().string();
    const auto folder = directory / "ReloadedEditor" / "Backups" / mapName;
    Command(40007);
    Pump(500);
    const auto backup = NewestBackup(folder);
    Check(!backup.empty(), "compare-backup-kept", backup.string() + " boxes=" + std::to_string(boxes.size()));
    if (backup.empty()) return;

    auto meshes = OfClass("StaticMeshActor", 50);
    auto lights = OfClass("Light", 5);
    if (meshes.size() < 2 || lights.empty()) { Fail("compare-fixture", "needs two static meshes and a light"); return; }
    unsigned char* mover = meshes[0];
    const std::string moverName = Name(mover), goneName = Name(meshes[1]), lightName = Name(lights[0]);
    const Vec moverAt = ReadVector(mover, "Location"), goneAt = ReadVector(meshes[1], "Location");
    const float brightness = ReadFloat(lights[0], "LightBrightness");
    WriteVector(mover, "Location", {moverAt.x + 256, moverAt.y, moverAt.z});
    if (auto p = FindProperty(lights[0], "LightBrightness")) *reinterpret_cast<float*>(lights[0] + *reinterpret_cast<unsigned*>(p + 0x3C)) = brightness + 7;
    for (auto a : Actors()) SetSelected(a, false);
    SetSelected(meshes[1], true);
    Exec("ACTOR DELETE");
    Pump(300);
    const size_t actorsEdited = Actors().size();
    Check(!ActorNamed(goneName), "compare-edits", "moved " + moverName + ", deleted " + goneName + ", light " + lightName + " " + Fixed(brightness) + "->" + Fixed(brightness + 7));

    // One row: the deleted mesh.
    auto before = Undo();
    auto run = RunCompare(backup, [&](const std::vector<std::string>& rows, HWND list) {
        const int gone = RowIndex(rows, "Deleted", goneName);
        if (gone >= 0) TickRow(list, gone);
        return gone >= 0;
    });
    for (size_t i = 0; i < run.rows.size() && i < 12; ++i) Info("compare-row", run.rows[i]);
    const int movedRow = RowIndex(run.rows, "Moved", moverName, "Location"), lightRow = RowIndex(run.rows, "Changed", lightName, "LightBrightness");
    Check(run.listed && RowIndex(run.rows, "Deleted", goneName) >= 0 && movedRow >= 0 && lightRow >= 0 && run.rows.size() == 3, "compare-lists-differences",
          std::to_string(run.rows.size()) + " rows; status '" + run.status + "' boxes=" + std::to_string(boxes.size()));
    if (lightRow >= 0) Info("compare-light-row", run.rows[lightRow]);
    Check(run.status.find("1 row(s) ticked") != std::string::npos, "compare-tick-one", run.status);
    // The copy the comparison loaded is unloaded and its file removed.
    Check(CountFiles(directory / "ReloadedEditor" / "compare") == 0, "compare-copy-removed");
    auto after = Undo();
    auto back = ActorNamed(goneName);
    if (!back) back = ActorNamed(goneName + "_2");
    const Vec backAt = back ? ReadVector(back, "Location") : Vec{};
    std::string text;
    for (auto& b : boxes) text += b + " ";
    Check(back && std::fabs(backAt.x - goneAt.x) < 0.01f && std::fabs(backAt.y - goneAt.y) < 0.01f && Actors().size() == actorsEdited + 1
              && std::fabs(ReadVector(mover, "Location").x - (moverAt.x + 256)) < 0.01f && after.count == before.count + 1 && after.current == "Bring back from backup",
          "compare-bring-back-one", "back=" + (back ? Name(back) : std::string("none")) + " actors " + std::to_string(actorsEdited) + "->" + std::to_string(Actors().size())
              + " undo '" + after.current + "' " + text);
    Exec("TRANSACTION UNDO");
    Pump(500);
    Check(!ActorNamed(goneName) && !ActorNamed(goneName + "_2") && Actors().size() == actorsEdited, "compare-undo", "actors=" + std::to_string(Actors().size()) + " undo '" + Undo().current + "'");

    // More to bring back: an emitter with its particle components deleted, and a copy of a mesh added.
    unsigned char* emitter = nullptr;
    int emitterParts = 0;
    for (auto a : Actors())
        if (auto p = FindProperty(a, "Emitters"); p && !(Flags(a) & 0x10))
            if (const int n = *reinterpret_cast<int*>(a + *reinterpret_cast<unsigned*>(p + 0x3C) + 4); n > 0) { emitter = a; emitterParts = n; break; }
    const std::string emitterName = emitter ? Name(emitter) : std::string();
    if (emitter)
        if (auto p = FindProperty(emitter, "Emitters"))
            Info("compare-emitter-component", ObjectPath(**reinterpret_cast<unsigned char***>(emitter + *reinterpret_cast<unsigned*>(p + 0x3C))));
    for (auto a : Actors()) SetSelected(a, false);
    if (emitter) { SetSelected(emitter, true); Exec("ACTOR DELETE"); Pump(300); }
    std::string copyName;
    if (meshes.size() > 2)
    {
        for (auto a : Actors()) SetSelected(a, false);
        SetSelected(meshes[2], true);
        std::set<std::string> names;
        for (auto a : Actors()) names.insert(Name(a));
        Exec("ACTOR DUPLICATE");
        Pump(300);
        for (auto a : Actors()) if (!names.count(Name(a))) copyName = Name(a);
    }
    Info("compare-more-edits", "deleted emitter " + emitterName + " (" + std::to_string(emitterParts) + " components), added copy " + copyName);

    // Everything: Tick All.
    run = RunCompare(backup, [&](const std::vector<std::string>& rows, HWND list) {
        SendMessageA(GetParent(list), WM_COMMAND, 13, 0);
        if (const auto shot = Ini("shot"); !shot.empty())
        {
            // The moved mesh's row clicked, so the picture shows it selected in the viewports.
            if (const int moved = RowIndex(rows, "Moved", moverName); moved >= 0)
            {
                LVITEMA item{};
                item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                item.state = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageA(list, LVM_SETITEMSTATE, moved, reinterpret_cast<LPARAM>(&item));
            }
            Pump(1500);
            SaveFrameShot(GetParent(list), shot);
        }
        return true;
    });
    for (size_t i = 0; i < run.rows.size() && i < 12; ++i) Info("compare-row", run.rows[i]);
    unsigned char* emitterBack = nullptr;
    for (auto a : Actors()) if (!emitterName.empty() && (Name(a) == emitterName || Name(a).rfind(emitterName + "_", 0) == 0)) emitterBack = a;
    int partsBack = 0;
    if (emitterBack)
        if (auto p = FindProperty(emitterBack, "Emitters")) partsBack = *reinterpret_cast<int*>(emitterBack + *reinterpret_cast<unsigned*>(p + 0x3C) + 4);
    Check((emitterName.empty() || (emitterBack && partsBack == emitterParts)) && (copyName.empty() || !ActorNamed(copyName)), "compare-bring-back-emitter-and-delete-copy",
          "emitter back=" + (emitterBack ? Name(emitterBack) : std::string("none")) + " components " + std::to_string(partsBack) + "/" + std::to_string(emitterParts)
              + " copy " + copyName + (ActorNamed(copyName) ? " still there" : " gone"));
    auto light = ActorNamed(lightName);
    mover = reinterpret_cast<unsigned char*>(ActorNamed(moverName));
    // Brought back again, it is named _3: _2 is kept for Undo too.
    back = nullptr;
    for (auto a : Actors()) if (Name(a) == goneName || Name(a).rfind(goneName + "_", 0) == 0) back = a;
    text.clear();
    for (auto& b : boxes) text += b + " ";
    Check(back && light && std::fabs(ReadFloat(light, "LightBrightness") - brightness) < 0.01f
              && mover && std::fabs(ReadVector(mover, "Location").x - moverAt.x) < 0.01f,
          "compare-bring-back-all", "status '" + run.status + "' back=" + (back ? Name(back) : std::string("none")) + " brightness=" + (light ? Fixed(ReadFloat(light, "LightBrightness")) : "-")
              + " undo '" + Undo().current + "' " + text);
    // Compared again, nothing differs.
    run = RunCompare(backup, [&](const std::vector<std::string>&, HWND) { return false; });
    for (const auto& row : run.rows) Info("compare-left", row);
    Check(!run.listed && BoxContains("match"), "compare-nothing-left", "rows=" + std::to_string(run.rows.size()) + " boxes=" + (boxes.empty() ? std::string("none") : boxes[0]));
    Exec("TRANSACTION UNDO");
    Pump(500);
    Check(ActorNamed(lightName) && std::fabs(ReadFloat(ActorNamed(lightName), "LightBrightness") - (brightness + 7)) < 0.01f, "compare-undo-all", "undo '" + Undo().current + "'");
}

// ---- Map Check, static mesh collision: the rows match the meshes' own counts read here, and a row
// selects every actor using its mesh (a double-click frames them). Run it on a map with heavy meshes.
struct MeshCounts { int poly = 0, polyNodes = 0, hull = 0, hullNodes = 0; std::vector<unsigned char*> users; };
std::map<std::string, MeshCounts> MapMeshes()
{
    std::map<std::string, MeshCounts> meshes;
    for (auto a : Actors())
    {
        auto p = FindProperty(a, "StaticMesh");
        auto mesh = p ? *reinterpret_cast<unsigned char**>(a + *reinterpret_cast<unsigned*>(p + 0x3C)) : nullptr;
        if (!mesh || ClassName(mesh) != "StaticMesh") continue;
        auto& m = meshes[ObjectName(mesh)];
        m.poly = *reinterpret_cast<int*>(mesh + 0x108);
        m.polyNodes = *reinterpret_cast<int*>(mesh + 0x118);
        m.hull = *reinterpret_cast<int*>(mesh + 0x14C);
        m.hullNodes = *reinterpret_cast<int*>(mesh + 0x154);
        m.users.push_back(a);
    }
    return meshes;
}

// Counts the open map's meshes over each limit, runs Map Check and compares its Collision rows with them.
// Returns the Map Check window (left open) and its rows.
HWND CollisionRows(const std::string& label, const std::map<std::string, MeshCounts>& meshes, std::vector<std::string>& rows, std::vector<int>& collisionRows)
{
    size_t crashes = 0, polyFull = 0, hullFull = 0, capped = 0;
    for (const auto& [name, m] : meshes)
    {
        crashes += m.poly > 5512 || m.hull > 5512;
        polyFull += m.polyNodes >= 32767;
        hullFull += m.hullNodes >= 32767;
        capped += m.poly >= 15000;
    }
    Info("collision-meshes", label + ": " + std::to_string(meshes.size()) + " meshes used; over 5512: " + std::to_string(crashes) + ", per-polygon BSP full: " + std::to_string(polyFull)
         + ", hull BSP full: " + std::to_string(hullFull) + ", at 15000: " + std::to_string(capped));
    Exec("SELECT NONE");
    const double ms = Command(41220);
    HWND check = OpenedWindow("Map Check");
    if (!check) { Fail("collision-open", label + ": Map Check did not open; boxes=" + std::to_string(boxes.size())); return nullptr; }
    rows = ListRows(GetDlgItem(check, 100));
    std::map<std::string, size_t> byCheck;
    collisionRows.clear();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const auto cells = Cells(rows[i]);
        if (cells.size() < 4 || cells[1] != "Collision") continue;
        ++byCheck[cells[2]];
        collisionRows.push_back(static_cast<int>(i));
    }
    std::string summary;
    for (const auto& [name, n] : byCheck) summary += (summary.empty() ? "" : ", ") + name + " " + std::to_string(n);
    Info("collision-rows", label + ": " + std::to_string(collisionRows.size()) + " of " + std::to_string(rows.size()) + " rows in " + Fixed(ms) + " ms: " + summary);
    Check(byCheck["Crashes the game"] == crashes && byCheck["Collision BSP full"] == polyFull && byCheck["Hull BSP full"] == hullFull
          && byCheck["Per-polygon collision cut short"] == capped, "collision-counts-match",
          label + ": rows crash/bsp/hull/cap " + std::to_string(byCheck["Crashes the game"]) + "/" + std::to_string(byCheck["Collision BSP full"]) + "/"
          + std::to_string(byCheck["Hull BSP full"]) + "/" + std::to_string(byCheck["Per-polygon collision cut short"]) + " vs meshes "
          + std::to_string(crashes) + "/" + std::to_string(polyFull) + "/" + std::to_string(hullFull) + "/" + std::to_string(capped));
    for (size_t i = 0; i < collisionRows.size() && i < 60; ++i) Info("collision-row", rows[static_cast<size_t>(collisionRows[i])]);
    return check;
}

void CollisionSuite()
{
    const auto meshes = MapMeshes();
    std::vector<std::string> rows;
    std::vector<int> collisionRows;
    HWND check = CollisionRows(std::filesystem::path(Ini("map")).stem().string(), meshes, rows, collisionRows);
    if (!check) return;

    // The first row selects every actor using its mesh, and only those.
    if (!collisionRows.empty())
    {
        const int row = collisionRows.front();
        const auto message = Cells(rows[static_cast<size_t>(row)])[3];
        const MeshCounts* mesh = nullptr;
        std::string meshName;
        for (const auto& [name, m] : meshes)
            if (message.rfind(name + " ", 0) == 0 || message.rfind(name + "'s ", 0) == 0)
                if (name.size() > meshName.size()) { meshName = name; mesh = &m; }
        if (!mesh) { Fail("collision-row-mesh", message.substr(0, 80)); }
        else
        {
            Exec("SELECT NONE");
            ClickRow(check, 100, row, false);
            size_t selected = 0, right = 0;
            for (auto a : Actors())
                if (Flags(a) & 0x40)
                {
                    ++selected;
                    right += std::find(mesh->users.begin(), mesh->users.end(), a) != mesh->users.end();
                }
            Check(selected == mesh->users.size() && right == selected, "collision-click-selects-users",
                  meshName + ": selected " + std::to_string(selected) + " of " + std::to_string(mesh->users.size()) + " status '" + WindowText(GetDlgItem(check, 101)) + "'");
            ClickRow(check, 100, row, true);
            Check(WindowText(GetDlgItem(check, 101)).find("framed") != std::string::npos, "collision-double-click-frames", WindowText(GetDlgItem(check, 101)));
        }
    }
    // sweep=<folder>: every other map there (not the stock autosaves), opened in turn.
    const auto sweep = Ini("sweep");
    if (!sweep.empty())
    {
        std::vector<std::filesystem::path> maps;
        for (const auto& entry : std::filesystem::directory_iterator(sweep))
            if (entry.path().extension() == ".sdc" && entry.path().stem().string().rfind("Auto", 0) != 0 && entry.path() != std::filesystem::path(Ini("map")))
                maps.push_back(entry.path());
        std::sort(maps.begin(), maps.end());
        const auto from = Ini("sweepfrom");   // resume a sweep at this map name
        for (const auto& map : maps)
        {
            if (map.stem().string() < from) continue;
            if (check) CloseWindowAndWait(check);
            // Message boxes a load shows (missing packages) are answered, as Command does.
            const UINT_PTR timer = SetTimer(nullptr, 0, 100, AnswerBoxes);
            bool loaded = false;
            try { loaded = Exec("MAP LOAD FILE=\"" + map.string() + "\"") != 0; }
            catch (...) { Info("collision-sweep-load", "the editor threw loading " + map.string()); }
            KillTimer(nullptr, timer);
            if (!loaded || Actors().size() < 2) { Fail("collision-sweep-load", map.string()); check = nullptr; continue; }
            Pump(300);
            check = CollisionRows(map.stem().string(), MapMeshes(), rows, collisionRows);
            if (!check) return;
        }
    }
    if (check && Ini("keepopen") != "1") CloseWindowAndWait(check);
}

// ---- RE+'s JSON bridge (ReloadedWorkflowRequest), on the UI thread like the windows call it.
nlohmann::json Workflow(const nlohmann::json& request)
{
    using Fn = int(__cdecl*)(const char*, char*, unsigned);
    static Fn call = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("Reloaded.Editor.dll"), "ReloadedWorkflowRequest"));
    if (!call) return {{"ok", false}, {"error", "no bridge"}};
    std::vector<char> out(64u << 20);
    const auto text = request.dump();
    const int r = call(text.c_str(), out.data(), static_cast<unsigned>(out.size()));
    if (r < 0) return {{"ok", false}, {"error", "reply too large"}};
    try { return nlohmann::json::parse(out.data()); }
    catch (const std::exception& e) { return {{"ok", false}, {"error", e.what()}}; }
}
std::string ObjectPathOf(void* object)
{
    std::string path;
    for (auto o = static_cast<unsigned char*>(object); o; o = *reinterpret_cast<unsigned char**>(o + 0x18))
        path = std::string(ObjectName(o)) + (path.empty() ? "" : "." + path);
    return path;
}
bool IsKindOf(unsigned char* object, const char* cls)
{
    for (auto c = *reinterpret_cast<unsigned char**>(object + 0x24); c; c = *reinterpret_cast<unsigned char**>(c + 0x28))
        if (!strcmp(ObjectName(c), cls)) return true;
    return false;
}

#include "SmokeMover.inl"

// ---- Appearance: a picture of the whole editor (frame, browsers, a property window), for comparing the
// Classic / Modern Light / Modern Dark settings. Every window of the editor is captured on its own
// (PrintWindow) and drawn where it sits on screen, onto a canvas the size of the frame: nothing else on
// the desktop is in the picture.
void MenuItems(HMENU menu, const std::string& path, std::vector<std::pair<std::string, UINT>>& out)
{
    for (int i = 0, n = GetMenuItemCount(menu); i < n; ++i)
    {
        char text[256] = {};
        GetMenuStringA(menu, i, text, sizeof(text), MF_BYPOSITION);
        std::string label;
        for (const char* c = text; *c; ++c) if (*c != '&') label += *c;
        if (HMENU sub = GetSubMenu(menu, i)) MenuItems(sub, path + label + " > ", out);
        else if (const UINT id = GetMenuItemID(menu, i); id && id != UINT(-1)) out.push_back({path + label, id});
    }
}
UINT MenuCommand(const char* text)
{
    std::vector<std::pair<std::string, UINT>> items;
    if (HMENU bar = GetMenu(frameWindow)) MenuItems(bar, "", items);
    for (auto& [label, id] : items)
        if (label.find(text) != std::string::npos) return id;
    return 0;
}
bool SavePng(HBITMAP bitmap, const std::wstring& path)
{
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return false;
    bool ok = false;
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        UINT count = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&count, &size);
        std::vector<unsigned char> buffer(size);
        auto encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
        Gdiplus::GetImageEncoders(count, size, encoders);
        for (UINT i = 0; i < count; ++i)
            if (!wcscmp(encoders[i].MimeType, L"image/png"))
                ok = image.Save(path.c_str(), &encoders[i].Clsid, nullptr) == Gdiplus::Ok;
    }
    Gdiplus::GdiplusShutdown(token);
    return ok;
}
// Windows 10 and 11 give a resizable window an invisible border (about 7 pixels left, right and below)
// that PrintWindow draws black; the visible part is DWM's extended frame bounds. Those are in screen
// pixels while this DPI-unaware editor works in scaled ones, so the border widths are converted.
using SetAwarenessFn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
SetAwarenessFn SetAwareness()
{
    return reinterpret_cast<SetAwarenessFn>(GetProcAddress(GetModuleHandleA("user32.dll"), "SetThreadDpiAwarenessContext"));
}
bool FrameBounds(HWND window, RECT& bounds)
{
    using GetAttribute = HRESULT(WINAPI*)(HWND, DWORD, PVOID, DWORD);
    static auto get = reinterpret_cast<GetAttribute>(GetProcAddress(LoadLibraryA("dwmapi.dll"), "DwmGetWindowAttribute"));
    return get && SUCCEEDED(get(window, 9 /* DWMWA_EXTENDED_FRAME_BOUNDS */, &bounds, sizeof(bounds)));
}
RECT VisibleRect(HWND window)
{
    RECT logical{};
    GetWindowRect(window, &logical);
    auto setAwareness = SetAwareness();
    if (!setAwareness) return logical;
    DPI_AWARENESS_CONTEXT previous = setAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    RECT physical{}, visible{};
    GetWindowRect(window, &physical);
    const bool known = FrameBounds(window, visible);
    if (previous) setAwareness(previous);
    if (!known || physical.right <= physical.left || physical.bottom <= physical.top) return logical;
    const double sx = double(logical.right - logical.left) / (physical.right - physical.left);
    const double sy = double(logical.bottom - logical.top) / (physical.bottom - physical.top);
    RECT r = logical;
    r.left += static_cast<LONG>(std::lround((visible.left - physical.left) * sx));
    r.top += static_cast<LONG>(std::lround((visible.top - physical.top) * sy));
    r.right -= static_cast<LONG>(std::lround((physical.right - visible.right) * sx));
    r.bottom -= static_cast<LONG>(std::lround((physical.bottom - visible.bottom) * sy));
    return r.right > r.left && r.bottom > r.top ? r : logical;
}
std::vector<HWND> EditorWindows() // visible top-level windows of the editor, top first
{
    std::vector<HWND> windows;
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        DWORD process = 0;
        GetWindowThreadProcessId(w, &process);
        if (process == GetCurrentProcessId() && IsWindowVisible(w) && !IsIconic(w)) reinterpret_cast<std::vector<HWND>*>(p)->push_back(w);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}
// What is really on screen (smoke.ini screen=1): the editor's windows brought to the top for a moment,
// in their own order, and only the frame's visible rectangle copied, in screen pixels.
int ScreenAttempt(const std::wstring& path);
bool CaptureScreen(const std::wstring& path)
{
    // Another editor taking its own picture the same way can push these windows down again, or keep
    // them from going topmost: then the picture is taken again, up to five times.
    for (int attempt = 1;; ++attempt)
    {
        const int covered = ScreenAttempt(path);
        if (covered == 0) return true;
        if (covered < 0) return false;
        if (attempt == 5)
        {
            Info("screen-covered", std::to_string(covered) + " of 81 points show another window");
            return true;
        }
        Pump(1500);
    }
}
// One try: the picture saved; returns -1 if it could not be saved, else how many of 81 points of the
// frame show another program's window, or one of the editor's windows that is not on top of the others.
int ScreenAttempt(const std::wstring& path)
{
    const auto windows = EditorWindows();
    std::string refused;
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
        if (!SetWindowPos(*it, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) refused += "'" + WindowText(*it) + "' error " + std::to_string(GetLastError()) + " ";
    RedrawWindow(frameWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    Pump(600);
    int notOnTop = 0;
    for (HWND w : windows)
        if (IsWindow(w) && !(GetWindowLongA(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) { ++notOnTop; refused += "'" + WindowText(w).substr(0, 40) + "' "; }
    if (!refused.empty() || notOnTop) Info("screen-not-topmost", std::to_string(notOnTop) + " window(s); " + refused);
    auto setAwareness = SetAwareness();
    DPI_AWARENESS_CONTEXT previous = setAwareness ? setAwareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
    RECT frame{};
    if (!FrameBounds(frameWindow, frame)) GetWindowRect(frameWindow, &frame);
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    HDC screen = GetDC(nullptr);
    HDC canvas = CreateCompatibleDC(screen);
    HBITMAP picture = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(canvas, picture);
    BitBlt(canvas, 0, 0, width, height, screen, frame.left, frame.top, SRCCOPY | CAPTUREBLT);
    // Another topmost window (another editor, say) over the frame would be in the picture: say so.
    int covered = 0;
    for (int gx = 1; gx < 10; ++gx)
        for (int gy = 1; gy < 10; ++gy)
        {
            DWORD process = 0;
            GetWindowThreadProcessId(WindowFromPhysicalPoint({frame.left + width * gx / 10, frame.top + height * gy / 10}), &process);
            if (process != GetCurrentProcessId()) ++covered;
        }
    if (setAwareness && previous) setAwareness(previous);
    for (HWND w : windows)
        if (IsWindow(w)) SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SelectObject(canvas, old);
    const bool ok = SavePng(picture, path);
    DeleteObject(picture);
    DeleteDC(canvas);
    ReleaseDC(nullptr, screen);
    return ok ? covered + notOnTop * 9 : -1;
}
// Every window of the editor captured on its own (PrintWindow) and drawn where it sits, without its
// invisible border, onto a canvas the size of the frame's visible rectangle.
bool CaptureEditor(const std::wstring& path)
{
    if (Ini("screen") == "1") return CaptureScreen(path);
    const RECT frame = VisibleRect(frameWindow);
    const int width = frame.right - frame.left, height = frame.bottom - frame.top;
    const auto windows = EditorWindows();
    HDC screen = GetDC(nullptr);
    HDC canvas = CreateCompatibleDC(screen);
    HBITMAP picture = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(canvas, picture);
    RECT all{0, 0, width, height};
    FillRect(canvas, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    for (auto it = windows.rbegin(); it != windows.rend(); ++it)
    {
        RECT r{};
        GetWindowRect(*it, &r);
        const RECT visible = VisibleRect(*it);
        RECT overlap{};
        if (!IntersectRect(&overlap, &visible, &frame)) continue;
        const int w = r.right - r.left, h = r.bottom - r.top;
        HDC one = CreateCompatibleDC(screen);
        HBITMAP shot = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ previous = SelectObject(one, shot);
        PrintWindow(*it, one, PW_RENDERFULLCONTENT);
        BitBlt(canvas, visible.left - frame.left, visible.top - frame.top, visible.right - visible.left, visible.bottom - visible.top,
               one, visible.left - r.left, visible.top - r.top, SRCCOPY);
        SelectObject(one, previous);
        DeleteObject(shot);
        DeleteDC(one);
    }
    SelectObject(canvas, old);
    const bool ok = SavePng(picture, path);
    DeleteObject(picture);
    DeleteDC(canvas);
    ReleaseDC(nullptr, screen);
    return ok;
}
HWND TopWithClassPart(const char* part)
{
    struct S { const char* p; HWND r; } s{part, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM p) -> BOOL {
        auto s = reinterpret_cast<S*>(p);
        char c[128] = {};
        GetClassNameA(w, c, sizeof(c));
        if (IsWindowVisible(w) && strstr(c, s->p)) { s->r = w; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.r;
}
// The 2.8 windows (-new1 to -new6 after the picture's name): Map Outliner, Scatter Props and Export Map to
// OBJ; the Command Palette; Compare with Backup; Mover Workbench with Actor Assemblies (a linked prefab
// placed and its row picked); Import OBJ with a file's summary; Map Check's reachability rows with the
// Reachability Map's legend in the viewports. Needs a map with a mover, static meshes, a light and a
// player start (ShipD). It saves the map.
std::filesystem::path NewShot(const std::string& shot, int n)
{
    auto path = std::filesystem::path(shot);
    path.replace_filename(path.stem().string() + "-new" + std::to_string(n) + ".png");
    return path;
}
void PlaceWindow(HWND w, int x, int y, int width, int height)
{
    const bool resizable = (GetWindowLongA(w, GWL_STYLE) & WS_THICKFRAME) != 0;
    RECT r{};
    GetWindowRect(w, &r);
    if (!resizable) x = (std::min)(x, static_cast<int>(1590 - (r.right - r.left))); // inside the 1600-wide frame
    SetWindowPos(w, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | (resizable ? 0 : SWP_NOSIZE));
}
void AppearanceNewWindows(const std::string& shot)
{
    // 1: Map Outliner, Scatter Props (with a mesh from the map in its list) and Export Map to OBJ.
    std::vector<HWND> opened;
    {
        unsigned char* mesh = nullptr;
        for (auto a : OfClass("StaticMeshActor"))
            if (auto m = scatter::MeshOf(a); m && scatter::Extent(m) >= 16 && scatter::Extent(m) <= 96) { mesh = m; break; }
        if (mesh) *reinterpret_cast<unsigned char**>(*reinterpret_cast<unsigned char**>(kEditor) + 0x13C) = mesh;
        Exec("SELECT NONE");
        Command(41560);
        Pump(800);
        if (HWND w = OpenedWindow("Map Outliner")) { PlaceWindow(w, 20, 90, 520, 880); opened.push_back(w); }
        else Info("appearance-tool-missing", "Map Outliner");
        Command(41500);
        Pump(800);
        if (HWND w = OpenedWindow("Scatter Props"))
        {
            if (mesh && SendMessageA(GetDlgItem(w, scatter::kList), LVM_GETITEMCOUNT, 0, 0) == 0) scatter::Click(w, scatter::kAddBrowser);
            PlaceWindow(w, 550, 90, 520, 880);
            opened.push_back(w);
        }
        else Info("appearance-tool-missing", "Scatter Props");
        Command(41660);
        Pump(800);
        if (HWND w = OpenedWindow("Export Map to OBJ")) { PlaceWindow(w, 1080, 90, 500, 600); opened.push_back(w); }
        else Info("appearance-tool-missing", "Export Map to OBJ");
        Pump(1200);
        Check(opened.size() == 3 && CaptureEditor(NewShot(shot, 1).wstring()), "appearance-new1-shot", std::to_string(opened.size()) + " windows");
        for (HWND w : opened) if (IsWindow(w)) CloseWindowAndWait(w);
    }

    // 2: the Command Palette, with a few letters typed.
    if (HWND palette = OpenPalette())
    {
        TypeInPalette(palette, "light");
        Pump(400);
        Check(CaptureEditor(NewShot(shot, 2).wstring()), "appearance-new2-shot", "Command Palette");
        SendMessageA(GetDlgItem(palette, 100), WM_KEYDOWN, VK_ESCAPE, 0);
        Pump(300);
        if (IsWindow(palette) && IsWindowVisible(palette)) ShowWindow(palette, SW_HIDE);
    }
    else Info("appearance-tool-missing", "Command Palette");

    // 3: Compare with Backup, after a save and three edits (a mesh moved, one deleted, a light changed).
    {
        const auto folder = directory / "ReloadedEditor" / "Backups" / std::filesystem::path(Ini("map")).stem();
        Command(40007);
        Pump(500);
        const auto backup = NewestBackup(folder);
        auto meshes = OfClass("StaticMeshActor", 50);
        auto lights = OfClass("Light", 5);
        if (backup.empty() || meshes.size() < 2 || lights.empty()) Info("appearance-tool-missing", "Compare with Backup: no backup, meshes or light");
        else
        {
            const Vec at = ReadVector(meshes[0], "Location");
            WriteVector(meshes[0], "Location", {at.x + 256, at.y, at.z});
            if (auto p = FindProperty(lights[0], "LightBrightness")) *reinterpret_cast<float*>(lights[0] + *reinterpret_cast<unsigned*>(p + 0x3C)) += 7;
            SelectOnly({meshes[1]});
            Exec("ACTOR DELETE");
            Pump(300);
            bool captured = false;
            RunCompare(backup, [&](const std::vector<std::string>&, HWND list) {
                HWND w = GetParent(list);
                PlaceWindow(w, 200, 150, 1100, 600);
                Pump(600);
                captured = CaptureEditor(NewShot(shot, 3).wstring());
                return false;
            });
            Check(captured, "appearance-new3-shot", "Compare with Backup");
            Exec("SELECT NONE");
        }
    }

    // 4: Mover Workbench for the first mover, with Actor Assemblies (a linked prefab placed, its row picked).
    {
        unsigned char* mover = nullptr;
        for (auto a : Actors()) if (IsKindOf(a, "Mover") && !(Flags(a) & 0x10)) { mover = a; break; }
        opened.clear();
        if (mover)
        {
            SelectOnly({mover});
            Command(MoverSmoke::kOpen);
            Pump(800);
        }
        if (HWND w = mover ? OpenedWindow("Mover Workbench") : nullptr) { PlaceWindow(w, 20, 90, 900, 880); opened.push_back(w); }
        else Info("appearance-tool-missing", "Mover Workbench");
        auto pair = FreeMeshes(2, 5);
        if (pair.size() == 2)
        {
            SelectOnly(pair);
            const auto entry = Request({{"op", "prefab.save"}, {"name", "Appearance Prefab"}});
            const auto a = ReadVector(pair[0], "Location");
            if (entry.contains("id"))
                Request({{"op", "prefab.place"}, {"assembly", entry.at("id")}, {"position", {a.x, a.y + 2048, a.z}}, {"rotation", {0, 0, 0}}});
        }
        Command(40922);
        Pump(800);
        if (HWND w = OpenedWindow("Actor Assemblies"))
        {
            PlaceWindow(w, 930, 90, 650, 880);
            HWND list = FindWindowExA(w, nullptr, WC_LISTVIEWA, nullptr);
            const auto rows = ListRows(list);
            for (size_t i = 0; i < rows.size(); ++i)
                if (rows[i].find("Appearance Prefab") != std::string::npos)
                {
                    LVITEMA item{};
                    item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                    item.state = LVIS_SELECTED | LVIS_FOCUSED;
                    SendMessageA(list, LVM_SETITEMSTATE, i, reinterpret_cast<LPARAM>(&item));
                }
            opened.push_back(w);
        }
        else Info("appearance-tool-missing", "Actor Assemblies");
        Pump(1200);
        Check(opened.size() == 2 && CaptureEditor(NewShot(shot, 4).wstring()), "appearance-new4-shot", std::to_string(opened.size()) + " windows");
        for (HWND w : opened) if (IsWindow(w)) CloseWindowAndWait(w);
        Exec("SELECT NONE");
    }

    // 5: Import OBJ (modal) with a file typed in and its summary shown, then cancelled.
    {
        const auto crate = WriteObjCrate().u8string();
        int ticks = 0;
        bool captured = false;
        dialogHandler = [&](HWND w, const std::string& title) {
            if (title != "Import OBJ") return false;
            ++ticks;
            if (ticks == 2)
            {
                SetWindowPos(w, HWND_TOP, 300, 120, 0, 0, SWP_NOSIZE);
                SetDlgItemTextA(w, 200, std::string(crate.begin(), crate.end()).c_str());
            }
            if (ticks == 12)
            {
                captured = CaptureEditor(NewShot(shot, 5).wstring());
                PostMessage(w, WM_COMMAND, IDCANCEL, 0);
            }
            return true;
        };
        Command(41640);
        dialogHandler = nullptr;
        Check(captured, "appearance-new5-shot", "Import OBJ");
    }

    // 6: the Reachability Map worked out, its legend in the viewports and its rows in Map Check.
    {
        Command(41220);
        Pump(500);
        HWND check = OpenedWindow("Map Check");
        if (!ReachChecked()) Command(41480);
        bool done = false;
        for (const DWORD started = GetTickCount(); check && GetTickCount() - started < 300000;)
        {
            Pump(1000);
            const auto rows = ReachRows(check);
            if (!rows.empty() && rows.back().find("Worked out on foot") != std::string::npos) { done = true; break; }
        }
        Info("appearance-reach", done ? "worked out" : "not finished");
        if (!check) { Fail("appearance-new6-shot", "Map Check did not open"); return; }
        PlaceWindow(check, 820, 520, 760, 460);
        // The last rows (the reachability ones) in view.
        HWND list = GetDlgItem(check, 100);
        const int rows = static_cast<int>(SendMessageA(list, LVM_GETITEMCOUNT, 0, 0));
        if (rows > 0) SendMessageA(list, LVM_ENSUREVISIBLE, rows - 1, FALSE);
        // The viewports (and the legend in them) only repaint from the editor's own loop: the picture
        // is taken once the suite has returned.
        static std::wstring lastShot;
        lastShot = NewShot(shot, 6).wstring();
        afterRun = [] { Check(CaptureEditor(lastShot), "appearance-new6-shot", "Map Check and Reachability Map"); };
    }
}
// How many pixels of a list box's selected row stand well apart from the row's own background: its
// text, or none when the row is painted in its text colour.
int SelectedRowContrast(HWND list)
{
    const int index = static_cast<int>(SendMessageA(list, LB_GETCURSEL, 0, 0));
    RECT row{}, client{};
    if (index < 0 || SendMessageA(list, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&row)) == LB_ERR) return -1;
    GetClientRect(list, &client);
    IntersectRect(&row, &row, &client);
    const int width = client.right, height = client.bottom;
    if (row.right <= row.left || row.bottom <= row.top) return -1;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP image = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(dc, image);
    PrintWindow(list, dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
    SelectObject(dc, old);
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    GetDIBits(dc, image, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    DeleteObject(image);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    auto light = [](uint32_t c) { return (int((c >> 16) & 0xFF) * 299 + int((c >> 8) & 0xFF) * 587 + int(c & 0xFF) * 114) / 1000; };
    std::map<uint32_t, int> counts;
    for (int y = row.top; y < row.bottom; ++y)
        for (int x = row.left; x < row.right; ++x) ++counts[pixels[static_cast<size_t>(y) * width + x] & 0xFFFFFF];
    uint32_t background = 0;
    int most = 0;
    for (auto& [colour, n] : counts)
        if (n > most) { most = n; background = colour; }
    int text = 0;
    for (auto& [colour, n] : counts)
        if (std::abs(light(colour) - light(background)) > 96) text += n;
    return text;
}
// RE+ Options over the editor (`-options`), then with its Appearance list dropped (`-options-list`):
// the current look's row must be readable (2.8.0's Modern Dark painted it white, under white text).
void AppearanceOptions(const std::string& shot)
{
    auto optionsShot = std::filesystem::path(shot), listShot = optionsShot;
    optionsShot.replace_filename(optionsShot.stem().string() + "-options.png");
    listShot.replace_filename(listShot.stem().string() + "-options-list.png");
    bool captured = false;
    dialogHandler = [&](HWND w, const std::string& title) {
        if (title != "RE+ Options") return false;
        if (!captured)
        {
            captured = true;
            SetWindowPos(w, HWND_TOP, 560, 300, 0, 0, SWP_NOSIZE);
            UpdateWindow(w);
            Check(CaptureEditor(optionsShot.wstring()), "appearance-options-shot", optionsShot.string());
            HWND combo = GetDlgItem(w, 1012); // IDC_COMBO_APPEARANCE
            SendMessageA(combo, CB_SHOWDROPDOWN, TRUE, 0);
            COMBOBOXINFO info{sizeof(info)};
            if (GetComboBoxInfo(combo, &info) && info.hwndList && IsWindowVisible(info.hwndList))
            {
                UpdateWindow(info.hwndList);
                Check(CaptureEditor(listShot.wstring()), "appearance-options-list-shot", listShot.string());
                const int text = SelectedRowContrast(info.hwndList);
                Check(text >= 20, "appearance-options-list-row", std::to_string(text) + " text pixels in the selected row");
            }
            else Fail("appearance-options-list-row", "the Appearance list did not open");
            SendMessageA(combo, CB_SHOWDROPDOWN, FALSE, 0);
            PostMessage(w, WM_COMMAND, IDCANCEL, 0);
        }
        return true;
    };
    Command(40066);
    dialogHandler = nullptr;
}
void AppearanceSuite()
{
    ShowWindow(frameWindow, SW_RESTORE);
    SetWindowPos(frameWindow, nullptr, 0, 0, 1600, 1000, SWP_NOZORDER | SWP_NOACTIVATE);
    Pump(500);
    for (const char* name : {"View > Show Texture Browser", "View > Show Static Mesh Browser"})
    {
        const UINT id = MenuCommand(name);
        Check(id != 0, "appearance-menu", name);
        if (id) { Command(id); Pump(1000); }
    }
    SelectOnly(FreeMeshes(1));
    const UINT properties = MenuCommand("Actor Properties");
    if (properties) { Command(properties); Pump(1000); }
    HWND browser = TopWithClassPart("WBrowserMaster"), props = TopWithClassPart("WObjectProperties");
    Check(browser && props, "appearance-windows", std::string(browser ? "browser " : "") + (props ? "properties" : ""));
    if (HWND log = TopWithClassPart("WLog")) ShowWindow(log, SW_HIDE);
    if (browser) SetWindowPos(browser, HWND_TOP, 1010, 150, 570, 830, SWP_NOACTIVATE);
    if (props) SetWindowPos(props, HWND_TOP, 30, 330, 380, 650, SWP_NOACTIVATE);
    // Open a category or two of the property list.
    Pump(1500);
    // Every window, its class and its font, for the report.
    EnumThreadWindows(GetCurrentThreadId(), [](HWND w, LPARAM) -> BOOL {
        if (!IsWindowVisible(w)) return TRUE;
        char cls[128] = {};
        GetClassNameA(w, cls, sizeof(cls));
        std::string children;
        EnumChildWindows(w, [](HWND c, LPARAM p) -> BOOL {
            char k[64] = {};
            GetClassNameA(c, k, sizeof(k));
            LOGFONTA font{};
            if (HFONT f = reinterpret_cast<HFONT>(SendMessageA(c, WM_GETFONT, 0, 0))) GetObjectA(f, sizeof(font), &font);
            auto& out = *reinterpret_cast<std::string*>(p);
            if (out.size() < 1500) out += std::string(k) + "(" + font.lfFaceName + " " + std::to_string(font.lfHeight) + ") ";
            return TRUE;
        }, reinterpret_cast<LPARAM>(&children));
        Info("window", std::string(cls) + " '" + WindowText(w) + "': " + children);
        return TRUE;
    }, 0);
    if (Ini("dump") == "1")
    {
        // Every child of the frame, browser and property window: class, place and size.
        for (HWND top : {frameWindow, browser, props})
            EnumChildWindows(top, [](HWND c, LPARAM) -> BOOL {
                char k[96] = {};
                GetClassNameA(c, k, sizeof(k));
                RECT r{};
                GetWindowRect(c, &r);
                if (IsWindowVisible(c))
                    Info("child", std::string(k) + " id=" + std::to_string(GetDlgCtrlID(c)) + " " + std::to_string(r.left) + "," + std::to_string(r.top) + " " + std::to_string(r.right - r.left) + "x" + std::to_string(r.bottom - r.top) + " style=" + std::to_string(GetWindowLongA(c, GWL_STYLE)) + " '" + WindowText(c).substr(0, 30) + "'");
                return TRUE;
            }, 0);
    }
    const auto shot = Ini("shot");
    if (shot.empty()) return;
    if (Ini("part") == "new") { AppearanceNewWindows(shot); return; } // only the 2.8 windows
    if (Ini("part") == "options") { AppearanceOptions(shot); return; } // only RE+ Options
    Check(CaptureEditor(std::filesystem::path(shot).wstring()), "appearance-shot", shot);
    AppearanceOptions(shot);
    // A message box (Isolate with nothing selected refuses), captured the same way, then answered.
    auto boxShot = std::filesystem::path(shot);
    boxShot.replace_filename(boxShot.stem().string() + "-box.png");
    bool captured = false;
    dialogHandler = [&](HWND w, const std::string&) {
        char cls[64] = {};
        GetClassNameA(w, cls, sizeof(cls));
        if (strcmp(cls, "#32770") || !IsMessageBox(w)) return false;
        if (!captured)
        {
            captured = true;
            SetWindowPos(w, nullptr, 560, 400, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            UpdateWindow(w);
            Check(CaptureEditor(boxShot.wstring()), "appearance-box-shot", DialogText(w));
        }
        else PostMessage(w, WM_COMMAND, GetDlgItem(w, IDOK) ? IDOK : IDCANCEL, 0); // a lone OK button is IDCANCEL
        return true;
    };
    Exec("SELECT NONE");
    Command(40958);
    dialogHandler = nullptr;

    // The frame's File menu, open, captured from a timer inside the menu's loop and then closed.
    static std::wstring menuShot;
    auto menuPath = std::filesystem::path(shot);
    menuPath.replace_filename(menuPath.stem().string() + "-menu.png");
    menuShot = menuPath.wstring();
    static bool menuCaptured;
    menuCaptured = false;
    UINT_PTR menuTimer = SetTimer(nullptr, 0, 2000, [](HWND, UINT, UINT_PTR, DWORD) {
        if (menuCaptured) return;
        menuCaptured = true;
        Check(CaptureEditor(menuShot), "appearance-menu-shot");
        EndMenu();
    });
    if (HMENU bar = GetMenu(frameWindow))
    {
        RECT item{};
        GetMenuItemRect(frameWindow, bar, 0, &item);
        TrackPopupMenu(GetSubMenu(bar, 0), TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, item.left, item.bottom, 0, frameWindow, nullptr);
    }
    KillTimer(nullptr, menuTimer);

    // RE+'s own tool windows: Map Check, Render Budget, Emitter Library and Gameplay Connections.
    struct Tool { UINT command; const char* title; RECT place; };
    const Tool tools[] = {
        {41220, "Map Check", {20, 90, 790, 520}},
        {41280, "Render Budget", {800, 90, 1580, 520}},
        {40967, "Emitter Library", {20, 530, 790, 980}},
        {40920, "Gameplay Connections", {800, 530, 1580, 980}},
    };
    std::vector<HWND> opened;
    for (auto& tool : tools)
    {
        Command(tool.command);
        Pump(800);
        if (HWND w = OpenedWindow(tool.title))
        {
            SetWindowPos(w, HWND_TOP, tool.place.left, tool.place.top, tool.place.right - tool.place.left, tool.place.bottom - tool.place.top, SWP_NOACTIVATE);
            opened.push_back(w);
        }
        else Info("appearance-tool-missing", tool.title);
    }
    Pump(1500);
    auto toolsShot = std::filesystem::path(shot);
    toolsShot.replace_filename(toolsShot.stem().string() + "-tools.png");
    Check(opened.size() == std::size(tools) && CaptureEditor(toolsShot.wstring()), "appearance-tools-shot", std::to_string(opened.size()) + " windows");
    for (HWND w : opened) if (IsWindow(w)) CloseWindowAndWait(w);

    // More of them: Undo History, Lighting Budget, Find Usages, Working Views, SMagicEvent Workbench
    // and Storeys, three across and two down.
    struct More { UINT command; const char* title; HWND (*open)(); };
    const More more[] = {
        {41105, "Undo History", nullptr}, {41150, "Lighting Budget", nullptr}, {0, "Asset Find and Replace", OpenFindUsages},
        {40921, "Working Views", nullptr}, {40927, "SMagicEvent Workbench", nullptr}, {40966, "Storeys", nullptr},
    };
    SelectOnly(FreeMeshes(1));
    opened.clear();
    int slot = 0;
    for (auto& tool : more)
    {
        HWND w = nullptr;
        if (tool.open) w = tool.open();
        else { Command(tool.command); Pump(800); w = OpenedWindow(tool.title); }
        const int x = 20 + (slot % 3) * 520, y = 90 + (slot / 3) * 450;
        ++slot;
        if (!w) { Info("appearance-tool-missing", tool.title); continue; }
        const bool resizable = (GetWindowLongA(w, GWL_STYLE) & WS_THICKFRAME) != 0;
        SetWindowPos(w, HWND_TOP, x, y, 510, 440, SWP_NOACTIVATE | (resizable ? 0 : SWP_NOSIZE));
        opened.push_back(w);
    }
    Pump(1500);
    auto moreShot = std::filesystem::path(shot);
    moreShot.replace_filename(moreShot.stem().string() + "-tools2.png");
    Check(opened.size() == std::size(more) && CaptureEditor(moreShot.wstring()), "appearance-tools2-shot", std::to_string(opened.size()) + " windows");
    for (HWND w : opened) if (IsWindow(w)) CloseWindowAndWait(w);

    // Light and Shadow Settings, with File > Optimise Map Assets (modal) open beside it.
    Command(41201);
    Pump(800);
    HWND lightShadow = OpenedWindow("Light and Shadow");
    if (lightShadow) SetWindowPos(lightShadow, HWND_TOP, 20, 90, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    else Info("appearance-tool-missing", "Light and Shadow");
    auto thirdShot = std::filesystem::path(shot);
    thirdShot.replace_filename(thirdShot.stem().string() + "-tools3.png");
    bool optimiseShown = false;
    dialogHandler = [&](HWND w, const std::string& title) {
        if (title != "Optimise Map Assets") return false;
        if (!optimiseShown)
        {
            optimiseShown = true;
            SetWindowPos(w, HWND_TOP, 560, 90, 0, 0, SWP_NOSIZE);
            UpdateWindow(w);
            Pump(300);
            Check(CaptureEditor(thirdShot.wstring()), "appearance-tools3-shot", lightShadow ? "with Light and Shadow" : "");
            PostMessage(w, WM_COMMAND, IDCANCEL, 0);
        }
        return true;
    };
    Command(41390);
    Pump(500);
    dialogHandler = nullptr;
    if (!optimiseShown) Info("appearance-tool-missing", "Optimise Map Assets");
    if (lightShadow && IsWindow(lightShadow)) CloseWindowAndWait(lightShadow);

    // Map Design, Brush Visibility, SCamNetwork Manager and Actor Assemblies (-tools4) with Character
    // Skins open over them, then Character Skin Presets on its own (-tools5); both of those are modal.
    struct Free { UINT command; const char* title; };
    const Free others[] = {{40948, "Map Design"}, {40953, "Brush Visibility"}, {40932, "SCamNetwork Manager"}, {40922, "Actor Assemblies"}};
    opened.clear();
    slot = 0;
    for (auto& tool : others)
    {
        Command(tool.command);
        Pump(800);
        const int x = 20 + (slot % 2) * 780, y = 90 + (slot / 2) * 450;
        ++slot;
        HWND w = OpenedWindow(tool.title);
        if (!w) { Info("appearance-tool-missing", tool.title); continue; }
        const bool resizable = (GetWindowLongA(w, GWL_STYLE) & WS_THICKFRAME) != 0;
        SetWindowPos(w, HWND_TOP, x, y, 770, 440, SWP_NOACTIVATE | (resizable ? 0 : SWP_NOSIZE));
        opened.push_back(w);
    }
    Pump(1000);
    for (const auto& [command, suffix] : {std::pair<UINT, const char*>{41180, "-tools4.png"}, {41181, "-tools5.png"}})
    {
        auto modalShot = std::filesystem::path(shot);
        modalShot.replace_filename(modalShot.stem().string() + suffix);
        bool shown = false;
        dialogHandler = [&](HWND w, const std::string& title) {
            if (title.rfind("Character Skin", 0) != 0) return false;
            if (!shown)
            {
                shown = true;
                SetWindowPos(w, HWND_TOP, 400, 200, 0, 0, SWP_NOSIZE);
                UpdateWindow(w);
                Pump(300);
                Check(CaptureEditor(modalShot.wstring()), std::string("appearance") + suffix, title);
                PostMessage(w, WM_COMMAND, IDCANCEL, 0);
            }
            return true;
        };
        Command(command);
        Pump(500);
        dialogHandler = nullptr;
        if (!shown) Info("appearance-tool-missing", std::to_string(command));
        for (HWND w : opened) if (IsWindow(w)) CloseWindowAndWait(w);
        opened.clear();
    }
    AppearanceNewWindows(shot);
}

void RunSuites(const std::string& list)
{
    std::stringstream names(list);
    for (std::string name; std::getline(names, name, ',');)
    {
        Info("suite", name);
        if (name == "storeys") StoreysSuite();
        else if (name == "unsaved") UnsavedSuite();
        else if (name == "placement") PlacementSuite();
        else if (name == "meshcost") MeshCostSuite();
        else if (name == "palette") PaletteSuite();
        else if (name == "prefabs") PrefabsSuite();
        else if (name == "lightshadow") LightShadowSuite();
        else if (name == "objexport") ObjExportSuite();
        else if (name == "objimport") ObjImportSuite();
        else if (name == "reach") ReachSuite();
        else if (name == "outliner") OutlinerSuite();
        else if (name == "scatter") ScatterSuite();
        else if (name == "compare") CompareSuite();
        else if (name == "collision") CollisionSuite();
        else if (name == "mover") MoverSuite();
        else if (name == "appearance") AppearanceSuite();
        else Fail("suite", "no suite called " + name);
    }
}
