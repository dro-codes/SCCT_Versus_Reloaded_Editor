// Integrated RE+ 2.7 smoke steps. The map from smoke.ini (a big map) is loaded first; map2 (ShipD) is used
// for the Map JSON round trip. Everything happens in a disposable install.

bool IsLocked(unsigned char* actor)
{
    auto p = FindProperty(actor, "bLockLocation");
    if (!p) return false;
    return (*reinterpret_cast<unsigned*>(actor + *reinterpret_cast<unsigned*>(p + 0x3C)) & *reinterpret_cast<unsigned*>(p + 0x64)) != 0;
}
std::vector<unsigned char*> FreeMeshes(size_t count, size_t skip = 0)
{
    std::vector<unsigned char*> result;
    for (auto a : OfClass("StaticMeshActor"))
    {
        if (IsLocked(a)) continue;
        if (skip) { --skip; continue; }
        result.push_back(a);
        if (result.size() == count) break;
    }
    return result;
}
void SelectOnly(const std::vector<unsigned char*>& actors)
{
    Exec("SELECT NONE");
    for (auto a : actors) SetSelected(a, true);
    Pump(100);
}
std::string VecText(Vec v) { return "(" + Fixed(v.x, 3) + "," + Fixed(v.y, 3) + "," + Fixed(v.z, 3) + ")"; }
bool Same(Vec a, Vec b) { return std::fabs(a.x - b.x) < 1e-3 && std::fabs(a.y - b.y) < 1e-3 && std::fabs(a.z - b.z) < 1e-3; }
int LargestGrid(const std::vector<Vec>& points)
{
    for (int g : {256, 128, 64, 32, 16, 8, 4, 2, 1})
    {
        bool all = true;
        for (auto p : points)
            for (float v : {p.x, p.y})
                if (std::fabs(v / g - std::round(v / g)) > 1e-3) all = false;
        if (all) return g;
    }
    return 0;
}
// Every column of every row, joined with " | ".
std::vector<std::string> ListRows(HWND list)
{
    std::vector<std::string> rows;
    if (!list) return rows;
    HWND header = reinterpret_cast<HWND>(SendMessageA(list, LVM_GETHEADER, 0, 0));
    const int columns = header ? std::max(1, static_cast<int>(SendMessageA(header, HDM_GETITEMCOUNT, 0, 0))) : 1;
    const int count = static_cast<int>(SendMessageA(list, LVM_GETITEMCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
    {
        std::string row;
        for (int c = 0; c < columns; ++c)
        {
            char text[2048] = {};
            LVITEMA item{};
            item.iSubItem = c;
            item.pszText = text;
            item.cchTextMax = sizeof(text);
            SendMessageA(list, LVM_GETITEMTEXTA, i, reinterpret_cast<LPARAM>(&item));
            if (c) row += " | ";
            row += text;
        }
        rows.push_back(row);
    }
    return rows;
}
float ReadFloat(unsigned char* actor, const char* name)
{
    auto p = FindProperty(actor, name);
    return p ? *reinterpret_cast<float*>(actor + *reinterpret_cast<unsigned*>(p + 0x3C)) : -1.0f;
}
unsigned char* ActorNamed(const std::string& name)
{
    for (auto a : Actors()) if (name == Name(a)) return a;
    return nullptr;
}
HWND FirstChildOfClass(HWND parent, const char* cls)
{
    struct S { const char* c; HWND r; } s{cls, nullptr};
    EnumChildWindows(parent, [](HWND w, LPARAM p) -> BOOL {
        auto s = reinterpret_cast<S*>(p);
        char c[64] = {};
        GetClassNameA(w, c, sizeof(c));
        if (!_stricmp(c, s->c)) { s->r = w; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.r;
}
HWND FindControlAnywhere(int id)
{
    struct S { int id; HWND r; } s{id, nullptr};
    EnumThreadWindows(GetCurrentThreadId(), [](HWND top, LPARAM p) -> BOOL {
        auto s = reinterpret_cast<S*>(p);
        EnumChildWindows(top, [](HWND w, LPARAM q) -> BOOL {
            auto s = reinterpret_cast<S*>(q);
            if (GetDlgCtrlID(w) == s->id) { char c[64] = {}; GetClassNameA(w, c, sizeof(c)); if (!_stricmp(c, "Edit")) { s->r = w; return FALSE; } }
            return TRUE;
        }, p);
        return s->r ? FALSE : TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.r;
}
// Answers an Explorer-style common file dialog with this path on its second tick.
bool AnswerFile(HWND w, const std::wstring& path, int& ticks)
{
    ++ticks;
    if (ticks == 40) { Info("cancel-file-dialog", WindowText(w)); PostMessage(w, WM_COMMAND, IDCANCEL, 0); }
    if (ticks != 2 || !IsWindowEnabled(w)) return true;
    SendMessageW(w, CDM_SETCONTROLTEXT, edt1, reinterpret_cast<LPARAM>(path.c_str()));
    EnumChildWindows(w, [](HWND child, LPARAM p) -> BOOL {
        wchar_t cls[64]{};
        GetClassNameW(child, cls, 64);
        if (!wcscmp(cls, L"Edit") && (GetDlgCtrlID(child) == 1001 || GetDlgCtrlID(child) == edt1 || GetDlgCtrlID(GetParent(child)) == cmb13 || GetDlgCtrlID(GetParent(GetParent(child))) == cmb13))
            SetWindowTextW(child, reinterpret_cast<const wchar_t*>(p));
        return TRUE;
    }, reinterpret_cast<LPARAM>(path.c_str()));
    PostMessage(w, WM_COMMAND, IDOK, 0);
    return true;
}
size_t CountFiles(const std::filesystem::path& folder)
{
    std::error_code e;
    size_t n = 0;
    if (std::filesystem::is_directory(folder, e))
        for (auto& f : std::filesystem::directory_iterator(folder, e)) if (f.is_regular_file()) ++n;
    return n;
}

void RunSteps()
{
    const auto root = directory.parent_path();
    const int total = static_cast<int>(Actors().size());
    const int hiddenAtStart = Hidden();
    Info("actors", std::to_string(total) + " hidden=" + std::to_string(hiddenAtStart));

    // ---- select-visibility: Isolate with nothing selected must refuse.
    Exec("SELECT NONE");
    Pump(200);
    double ms = Command(40958);
    Check(BoxContains("Select what should stay visible first") && Hidden() == hiddenAtStart, "isolate-needs-selection",
          "ms=" + Fixed(ms) + " hidden=" + std::to_string(Hidden()) + " boxes=" + std::to_string(boxes.size()));

    // ---- actor-index: Invert on a big map (2.6.1 took 8.4 s on a 4,800-actor map).
    auto one = FreeMeshes(1);
    SelectOnly(one);
    ms = Command(40956);
    const int inverted = Selected();
    Check(ms < 3000 && inverted > total / 2, "invert-fast", "ms=" + Fixed(ms) + " selected=" + std::to_string(inverted));

    // ---- Isolate three meshes, once and again (the second changes nothing: no new Undo step).
    auto three = FreeMeshes(3, 10);
    SelectOnly(three);
    auto before = Undo();
    ms = Command(40958);
    auto after = Undo();
    const int hiddenIsolated = Hidden();
    Check(after.count == before.count + 1 && hiddenIsolated > total / 2, "isolate-one-step",
          "ms=" + Fixed(ms) + " hidden=" + std::to_string(hiddenIsolated) + " undo='" + after.current + "'");
    SelectOnly(three);
    before = Undo();
    ms = Command(40958);
    after = Undo();
    Check(after.count == before.count, "isolate-again-no-step", "ms=" + Fixed(ms) + " undo " + std::to_string(before.count) + "->" + std::to_string(after.count));
    ms = Command(40959);
    Check(ms < 3000 && Hidden() == hiddenAtStart, "unhide-all-fast", "ms=" + Fixed(ms) + " hidden=" + std::to_string(Hidden()) + " boxes=" + std::to_string(boxes.size()));

    // ---- placement-snap-push: Snap pivots (X and Y), again (nothing to do), Undo; yaw to 90.
    auto meshes = FreeMeshes(3, 40);
    std::vector<Vec> start;
    for (auto a : meshes) start.push_back(ReadVector(a, "Location"));
    SelectOnly(meshes);
    before = Undo();
    ms = Command(41420);
    after = Undo();
    std::vector<Vec> snapped;
    for (auto a : meshes) snapped.push_back(ReadVector(a, "Location"));
    const int grid = LargestGrid(snapped);
    std::string moves;
    for (size_t i = 0; i < meshes.size(); ++i) moves += VecText(start[i]) + "->" + VecText(snapped[i]) + " ";
    // The working copy's editor grid is 1 unit, so whole-unit pivots are the expected result there.
    bool movedAny = false;
    for (size_t i = 0; i < meshes.size(); ++i) movedAny = movedAny || !Same(start[i], snapped[i]);
    Check(after.count == before.count + 1 && grid >= 1 && movedAny, "snap-pivots", "grid>=" + std::to_string(grid) + " undo='" + after.current + "' " + moves);
    before = Undo();
    Command(41420);
    after = Undo();
    Check(after.count == before.count, "snap-again-nothing", "undo " + std::to_string(before.count) + "->" + std::to_string(after.count));
    Exec("TRANSACTION UNDO");
    Pump(300);
    bool restored = true;
    for (size_t i = 0; i < meshes.size(); ++i) restored = restored && Same(ReadVector(meshes[i], "Location"), start[i]);
    Check(restored, "snap-undo-restores");
    // Turn them off the 90-degree grid first (yaw 9000, a little under 50 degrees), then Snap Yaw to 90.
    std::vector<Rot> turns;
    for (auto a : meshes)
    {
        turns.push_back(ReadRotation(a));
        if (auto p = FindProperty(a, "Rotation")) reinterpret_cast<Rot*>(a + *reinterpret_cast<unsigned*>(p + 0x3C))->yaw = 9000;
    }
    SelectOnly(meshes);
    before = Undo();
    Command(41424);
    after = Undo();
    bool square = true;
    std::string yaws;
    for (auto a : meshes) { auto r = ReadRotation(a); yaws += std::to_string(r.yaw) + " "; square = square && r.yaw == 16384; }
    Check(square && after.count == before.count + 1, "snap-yaw-90", "yaws=" + yaws + " undo='" + after.current + "'");
    if (after.count > before.count) { Exec("TRANSACTION UNDO"); Pump(300); }
    bool turnedBack = true;
    for (auto a : meshes) turnedBack = turnedBack && ReadRotation(a).yaw == 9000;
    Check(turnedBack, "snap-yaw-undo");
    for (size_t i = 0; i < meshes.size(); ++i)
        if (auto p = FindProperty(meshes[i], "Rotation")) *reinterpret_cast<Rot*>(meshes[i] + *reinterpret_cast<unsigned*>(p + 0x3C)) = turns[i];

    // ---- copy-paste-props: copy DrawScale3D from one mesh onto two others, one Undo step.
    auto trio = FreeMeshes(3, 80);
    if (trio.size() == 3)
    {
        WriteVector(trio[0], "DrawScale3D", Vec{2, 3, 4});
        const Vec b0 = ReadVector(trio[1], "DrawScale3D"), c0 = ReadVector(trio[2], "DrawScale3D");
        SelectOnly({trio[0]});
        Command(41075);
        const bool copyBox = !boxes.empty();
        SelectOnly({trio[1], trio[2]});
        int pasteTicks = 0;
        size_t pasteRows = 0;
        std::string pasteTitle;
        dialogHandler = [&](HWND w, const std::string& title) {
            if (title.find("Paste Properties from") == std::string::npos) return false;
            pasteTitle = title;
            if (auto list = FirstChildOfClass(w, "SysListView32")) pasteRows = ListRows(list).size();
            if (++pasteTicks == 3) PostMessage(w, WM_COMMAND, IDOK, 0);
            return true;
        };
        before = Undo();
        Command(41076);
        dialogHandler = nullptr;
        after = Undo();
        const Vec b1 = ReadVector(trio[1], "DrawScale3D"), c1 = ReadVector(trio[2], "DrawScale3D");
        Check(!copyBox && pasteRows > 0 && Same(b1, Vec{2, 3, 4}) && Same(c1, Vec{2, 3, 4}) && after.count == before.count + 1, "paste-properties",
              "title='" + pasteTitle + "' rows=" + std::to_string(pasteRows) + " b=" + VecText(b1) + " c=" + VecText(c1) + " undo='" + after.current + "' boxes=" + std::to_string(boxes.size()));
        Exec("TRANSACTION UNDO");
        Pump(300);
        Check(Same(ReadVector(trio[1], "DrawScale3D"), b0) && Same(ReadVector(trio[2], "DrawScale3D"), c0), "paste-undo-restores");
    }

    // ---- map-check-floating: run Map Check, lift five meshes 20 units, refresh.
    Exec("SELECT NONE");
    ms = Command(41220);
    HWND check = FindTopWindow("Map Check");
    auto rowsBefore = check ? ListRows(GetDlgItem(check, 100)) : std::vector<std::string>{};
    int floatingBefore = 0;
    for (auto& r : rowsBefore) if (r.find("float") != std::string::npos) ++floatingBefore;
    Check(check != nullptr, "map-check-opens", "ms=" + Fixed(ms) + " rows=" + std::to_string(rowsBefore.size()) + " floating=" + std::to_string(floatingBefore));
    if (check)
    {
        auto lift = FreeMeshes(5, 120);
        std::vector<Vec> lifted;
        for (auto a : lift) { auto v = ReadVector(a, "Location"); lifted.push_back(v); v.z += 20; WriteVector(a, "Location", v); }
        ms = Command(102, check);
        Pump(500);
        auto rowsAfter = ListRows(GetDlgItem(check, 100));
        int floatingAfter = 0, named = 0;
        for (auto& r : rowsAfter)
            if (r.find("float") != std::string::npos)
            {
                ++floatingAfter;
                for (auto a : lift) if (r.find(std::string(Name(a)) + " ") != std::string::npos) ++named;
            }
        Check(floatingAfter > floatingBefore && named > 0, "map-check-floating", "ms=" + Fixed(ms) + " floating " + std::to_string(floatingBefore) + "->" + std::to_string(floatingAfter) + " lifted-listed=" + std::to_string(named));
        for (size_t i = 0; i < lift.size(); ++i) WriteVector(lift[i], "Location", lifted[i]);
        for (size_t i = 0; i < rowsAfter.size() && i < 12; ++i) Info("map-check-row", rowsAfter[i]);
        PostMessage(check, WM_CLOSE, 0, 0);
        Pump(300);
    }

    // ---- camera-bookmarks: Set 1 writes a slot into library.json; Go 5 (empty) explains; Go 1 works.
    Command(41449);
    std::string library;
    {
        std::ifstream in(directory / "ReloadedEditor" / "library.json", std::ios::binary);
        library.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    Check(library.find("\"slot\":1") != std::string::npos || library.find("\"slot\": 1") != std::string::npos, "bookmark-set", "library bytes=" + std::to_string(library.size()) + " boxes=" + std::to_string(boxes.size()));
    Command(41444);
    Check(BoxContains("No camera bookmark 5"), "bookmark-empty-slot", "boxes=" + std::to_string(boxes.size()));
    Command(41440);
    Check(boxes.empty(), "bookmark-go", "boxes=" + std::to_string(boxes.size()));

    // ---- save-backups: two saves keep two earlier versions; Open Map Backup can be cancelled.
    std::error_code removeErrorIgnored;
    const auto mapName = std::filesystem::path(Ini("map")).stem().string();
    const auto backups = directory / "ReloadedEditor" / "Backups" / mapName;
    // Opened from Open Recent, the map has its file, so File > Save saves it in place (as Ctrl+S does).
    // Opened by command, File > Save would ask where; then save with the command File > Save runs.
    const bool viaMenu = Ini("open") == "recent";
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    if (viaMenu) Command(40007); else Exec("MAP SAVE FILE=\"" + Ini("map") + "\"");
    Pump(300);
    const size_t afterFirst = CountFiles(backups);
    Pump(1500);
    if (viaMenu) Command(40007); else Exec("MAP SAVE FILE=\"" + Ini("map") + "\"");
    Pump(300);
    QueryPerformanceCounter(&b);
    ms = 1000.0 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
    const size_t afterSecond = CountFiles(backups);
    for (auto& entry : std::filesystem::directory_iterator(backups, removeErrorIgnored)) Info("backup", entry.path().filename().string());
    Check(afterFirst == 1 && afterSecond == 2, "save-backups", "ms=" + Fixed(ms) + " files " + std::to_string(afterFirst) + "," + std::to_string(afterSecond) + " boxes=" + std::to_string(boxes.size()));
    bool sawOpen = false;
    dialogHandler = [&](HWND w, const std::string& title) {
        if (title != "Open Map Backup") return false;
        if (!sawOpen) { sawOpen = true; PostMessage(w, WM_COMMAND, IDCANCEL, 0); }
        return true;
    };
    Command(40988);
    dialogHandler = nullptr;
    Check(sawOpen && boxes.empty(), "open-backup-cancel", "boxes=" + std::to_string(boxes.size()));

    // ---- sm-search: type into the Static Mesh Browser's search box, if the browser exists.
    if (HWND search = FindControlAnywhere(41016))
    {
        HWND parent = GetParent(search);
        HWND list = nullptr;
        std::string classes;
        for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        {
            char cls[64] = {};
            GetClassNameA(c, cls, sizeof(cls));
            classes += std::string(cls) + "#" + std::to_string(GetDlgCtrlID(c)) + " ";
            std::string lower(cls);
            for (auto& ch : lower) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (!list && lower.find("list") != std::string::npos) list = c;
        }
        Info("mesh-search-children", classes);
        auto count = [&]() -> int {
            if (!list) return -1;
            char cls[64] = {};
            GetClassNameA(list, cls, sizeof(cls));
            return strstr(cls, "ListView") ? static_cast<int>(SendMessageA(list, LVM_GETITEMCOUNT, 0, 0)) : static_cast<int>(SendMessageA(list, LB_GETCOUNT, 0, 0));
        };
        const int plain = count();
        SetWindowTextA(search, "door");
        SendMessageA(parent, WM_COMMAND, MAKEWPARAM(41016, EN_CHANGE), reinterpret_cast<LPARAM>(search));
        Pump(1200);
        const int found = count();
        SetWindowTextA(search, "");
        SendMessageA(parent, WM_COMMAND, MAKEWPARAM(41016, EN_CHANGE), reinterpret_cast<LPARAM>(search));
        Pump(1200);
        Check(found >= 0, "mesh-search", "plain=" + std::to_string(plain) + " door=" + std::to_string(found) + " cleared=" + std::to_string(count()));
    }
    else Info("mesh-search", "Static Mesh Browser search box not found (browser not created)");

    // ---- selection-not-edit: Map JSON import applies after a preview row was clicked (ShipD).
    const auto map2 = Ini("map2");
    bool second = false;
    if (!map2.empty() && viaMenu)
    {
        // Open Recent's second entry; the map has unsaved test edits, so answer No to saving them.
        boxAnswer = IDNO;
        Command(40971);
        boxAnswer = IDOK;
        second = std::filesystem::path(map2).stem().string() == std::filesystem::path(Ini("map")).stem().string() ? false : Actors().size() > 2;
        Info("second-map", "boxes=" + std::to_string(boxes.size()) + " actors=" + std::to_string(Actors().size()));
    }
    else if (!map2.empty()) second = Exec("MAP LOAD FILE=\"" + map2 + "\"") != 0;
    if (second)
    {
        Pump(1500);
        const auto exportPath = (directory / "smoke_export.json").wstring();
        const auto changePath = (directory / "smoke_changes.json").wstring();
        std::error_code removeError;
        std::filesystem::remove(exportPath, removeError);
        std::filesystem::remove(changePath, removeError);
        int ticks = 0;
        dialogHandler = [&](HWND w, const std::string& title) {
            if (title != "Export Map to JSON") return false;
            return AnswerFile(w, exportPath, ticks);
        };
        ms = Command(40934);
        dialogHandler = nullptr;
        Check(std::filesystem::exists(exportPath), "json-export", "ms=" + Fixed(ms));
        // An update of an existing light: its preview row selects that light in the map.
        std::string lightName;
        float brightness = -1;
        try
        {
            std::ifstream in(exportPath, std::ios::binary);
            auto snapshot = nlohmann::json::parse(in);
            auto changes = snapshot.at("changes");
            nlohmann::json update;
            for (auto& entry : snapshot.at("actors"))
            {
                if (entry.at("actor").at("class") != "Engine.Light" || !entry.contains("values") || !entry["values"].contains("LightBrightness")) continue;
                const auto path = entry["actor"]["path"].get<std::string>();
                lightName = path.substr(path.find_last_of('.') + 1);
                brightness = std::stof(entry["values"]["LightBrightness"].get<std::string>()) + 7.0f;
                update = {{"op", "update"}, {"actor", entry["actor"]}, {"before", entry["values"]}, {"properties", {{"LightBrightness", std::to_string(brightness)}}}};
                break;
            }
            changes["description"] = "Smoke test brightness";
            changes["operations"] = nlohmann::json::array({update});
            std::ofstream(changePath, std::ios::binary) << changes.dump(1);
        }
        catch (const std::exception& e) { Fail("json-change-file", e.what()); }
        Info("json-light", lightName + " -> " + Fixed(brightness));
        const size_t lightsBefore = OfClass("Light").size();
        int openTicks = 0, previewTicks = 0;
        bool previewSeen = false;
        dialogHandler = [&](HWND w, const std::string& title) {
            if (title == "Import Map from JSON") return AnswerFile(w, changePath, openTicks);
            if (title != "Preview Map JSON Import") return false;
            previewSeen = true;
            ++previewTicks;
            if (previewTicks == 2)
            {
                LVITEMA item{};
                item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                item.state = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageA(GetDlgItem(w, 11), LVM_SETITEMSTATE, 0, reinterpret_cast<LPARAM>(&item));
            }
            if (previewTicks == 6) PostMessage(w, WM_COMMAND, IDOK, 0);
            return true;
        };
        before = Undo();
        ms = Command(40935);
        dialogHandler = nullptr;
        after = Undo();
        const size_t lightsAfter = OfClass("Light").size();
        auto light = ActorNamed(lightName);
        const float now = light ? ReadFloat(light, "LightBrightness") : -1.0f;
        Check(previewSeen && !BoxContains("Map changed during preview") && std::fabs(now - brightness) < 0.01f && lightsAfter == lightsBefore, "json-import-after-row-click",
              "ms=" + Fixed(ms) + " brightness=" + Fixed(now) + " undo='" + after.current + "' boxes=" + std::to_string(boxes.size()) + " selected=" + std::to_string(light ? (Flags(light) & 0x40) != 0 : 0));

        // ---- conveyor belts (with map2, on the second map): Add Conveyor Belt at the viewport's last click, then Undo.
        if (Ini("conveyor") == "1")
        {
            const size_t actorsBefore = Actors().size();
            before = Undo();
            ms = Command(41395);
            Pump(1500);
            after = Undo();
            const size_t actorsAfter = Actors().size();
            std::string text;
            for (auto& box : boxes) text += box + " ";
            Check(actorsAfter > actorsBefore && after.count > before.count, "conveyor-add",
                  "ms=" + Fixed(ms) + " actors " + std::to_string(actorsBefore) + "->" + std::to_string(actorsAfter) + " undo='" + after.current + "' " + text);
            Exec("TRANSACTION UNDO");
            Pump(800);
            Info("conveyor-undo", "actors=" + std::to_string(Actors().size()) + " undo='" + Undo().current + "'");
        }
    }
}
