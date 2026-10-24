// ---- Mover Workbench (RE+ Tools > Mover Workbench...), on a map with movers (ShipD).
// Included by SmokeSuites.inl after the Workflow() bridge helper.

// A window's picture, as PrintWindow draws it, into a 24-bit BMP.
bool CaptureWindow(HWND w, const std::filesystem::path& file)
{
    RECT r{};
    GetWindowRect(w, &r);
    const int width = r.right - r.left, height = r.bottom - r.top;
    if (width <= 0 || height <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    auto old = SelectObject(dc, bitmap);
    const bool drawn = PrintWindow(w, dc, 2) != 0;
    BITMAPINFOHEADER header{sizeof(header), width, height, 1, 24, BI_RGB};
    const int stride = (width * 3 + 3) & ~3;
    std::vector<unsigned char> pixels(static_cast<size_t>(stride) * height);
    GetDIBits(dc, bitmap, 0, height, pixels.data(), reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    BITMAPFILEHEADER fileHeader{0x4d42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(header) + pixels.size()), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(header)};
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return drawn;
}

namespace MoverSmoke
{
constexpr UINT kOpen = 41520;
// MoverWorkbench.h's controls.
enum : int { kList = 100, kStatus, kInfo, kAddKey, kUpdateKey, kDeleteKey, kMoveUp, kMoveDown, kGoToKey, kPreview,
             kGhosts, kApply, kRenameTag, kWire, kWireList, kPlayers, kPlayersText, kTriggeredBy, kRefresh, kFieldFirst = 200 };
constexpr int kMoveTimeField = 0, kInitialStateField = 4;

nlohmann::json Identity(unsigned char* a) { return {{"path", ObjectPathOf(a)}, {"class", ObjectPathOf(*reinterpret_cast<unsigned char**>(a + 0x24))}}; }
nlohmann::json State(const nlohmann::json& id)
{
    auto r = Workflow({{"op", "mover.state"}, {"actor", id}});
    return r.value("ok", false) ? r["result"] : nlohmann::json();
}
int Steps() { auto u = Undo(); return u.count - u.undone; }
std::string Status(HWND w) { return WindowText(GetDlgItem(w, kStatus)); }
void Press(HWND w, int id) { Command(static_cast<UINT>(id), w); Pump(200); }
void Pick(HWND w, int row)
{
    HWND list = GetDlgItem(w, kList);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Pump(100);
}
std::string Key(const nlohmann::json& state, int k)
{
    if (state.is_null() || k >= static_cast<int>(state["path"]["keys"].size())) return "-";
    const auto& p = state["path"]["keys"][k]["position"];
    return "(" + Fixed(p[0].get<double>()) + "," + Fixed(p[1].get<double>()) + "," + Fixed(p[2].get<double>()) + ")";
}
bool Near(const nlohmann::json& a, double x, double y, double z)
{
    return std::fabs(a[0].get<double>() - x) < 0.01 && std::fabs(a[1].get<double>() - y) < 0.01 && std::fabs(a[2].get<double>() - z) < 0.01;
}
int Keys(const nlohmann::json& state) { return state.is_null() ? -1 : static_cast<int>(state["path"]["keys"].size()); }
int Current(const nlohmann::json& state) { return state.is_null() ? -1 : state["path"]["current"].get<int>(); }
}

// The pictures for the report (picture = '1' in -Extra): a three-key path with a turn, the mover
// framed in every viewport, the workbench open with ghosts drawn, captured once the editor's own
// loop has repainted the viewports. Nothing is undone: the run ends there.
void MoverPicture(unsigned char* mover, const nlohmann::json& id)
{
    using namespace MoverSmoke;
    auto path = State(id)["path"];
    path["keys"] = nlohmann::json::array({{{"position", {0, 0, 0}}, {"rotation", {0, 0, 0}}},
                                          {{"position", {-166, 0, 0}}, {"rotation", {0, 0, 0}}},
                                          {{"position", {-166, -170, 0}}, {"rotation", {0, 8192, 0}}}});
    path["current"] = 0;
    auto r = Workflow({{"op", "mover.path"}, {"actor", id}, {"path", path}, {"label", "picture"}});
    Check(r.value("ok", false), "mover-picture-path", r.value("error", std::string()));
    {
        auto outline = Workflow({{"op", "mover.outline"}, {"actor", id}});
        Info("picture-outline", std::to_string(outline["result"]["edges"].size()) + " edges " + outline.dump().substr(0, 400));
    }
    const auto at = ReadVector(mover, "Location");
    Workflow({{"op", "select"}, {"actors", nlohmann::json::array({id})}, {"focus", true}});
    Pump(300);
    auto view = Workflow({{"op", "view.capture"}})["result"];
    Info("picture-cameras", view["cameras"].dump().substr(0, 600));
    const double target[3] = {at.x - 110, at.y - 90, at.z + 40};
    for (auto& c : view["cameras"])
    {
        if (c.value("mode", 0) != 5 && c.value("mode", 0) != 9 && c.contains("OrthoZoom")) { c["OrthoZoom"] = c["OrthoZoom"].get<double>() / 4; continue; }
        const double eye[3] = {at.x + 260, at.y + 330, at.z + 230};
        const double dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
        const double units = 32768.0 / 3.14159265358979;
        c["position"] = {eye[0], eye[1], eye[2]};
        c["rotation"] = {static_cast<int>(std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * units), static_cast<int>(std::atan2(dy, dx) * units), 0};
    }
    view["actors"] = nlohmann::json::array();
    view.erase("groups");
    Info("picture-view", Workflow({{"op", "view.restore"}, {"view", view}}).dump().substr(0, 200));
    SelectOnly({mover});
    Command(MoverSmoke::kOpen);
    Pump(500);
    if (HWND w = OpenedWindow("Mover Workbench")) { Pick(w, 2); SetWindowPos(w, nullptr, 40, 40, 1000, 720, SWP_NOZORDER); }
    afterRun = [] {
        HWND w = OpenedWindow("Mover Workbench");
        Check(w && CaptureWindow(frameWindow, directory / "mover_editor.bmp") && CaptureWindow(w, directory / "mover_window.bmp"), "mover-pictures");
    };
}

void MoverSuite()
{
    using namespace MoverSmoke;
    // The first mover with two keys, the rest of the map unselected.
    unsigned char* mover = nullptr;
    for (auto a : Actors())
        if (IsKindOf(a, "Mover") && !(Flags(a) & 0x10))
        {
            auto st = State(Identity(a));
            if (Keys(st) == 2 && Current(st) == 0) { mover = a; break; }
        }
    if (!mover) { Fail("mover-found", "no two-key mover in this map"); return; }
    const auto id = Identity(mover);
    Info("mover", id.dump());
    SelectOnly({mover});
    Check(Workflow({{"op", "mover.selected"}})["result"] == id, "mover-selected", Workflow({{"op", "mover.selected"}}).dump());

    // The actor right-click menu offers it (LoadMenu through the editor's import, which RE+ hooks).
    using LoadMenuFn = HMENU(WINAPI*)(HINSTANCE, LPCSTR);
    auto load = *reinterpret_cast<LoadMenuFn*>(0x11AF23F0);
    if (HMENU menu = load(GetModuleHandleA(nullptr), MAKEINTRESOURCEA(107)))
    {
        HMENU sub = GetSubMenu(menu, 0);
        Check(sub && GetMenuState(sub, kOpen, MF_BYCOMMAND) != UINT(-1), "mover-actor-menu");
        DestroyMenu(menu);
    }
    else Fail("mover-actor-menu", "no menu 107");

    Command(kOpen);
    Pump(800);
    HWND w = OpenedWindow("Mover Workbench");
    if (!w) { Fail("mover-open", "boxes=" + std::to_string(boxes.size())); return; }
    HWND list = GetDlgItem(w, kList);
    Check(ListView_GetItemCount(list) == 2, "mover-lists-keys", std::to_string(ListView_GetItemCount(list)) + " rows; " + WindowText(GetDlgItem(w, kInfo)));
    for (const auto& row : ListRows(list)) Info("mover-row", row);

    const auto start = State(id);
    const auto base = start["path"]["base"];
    const auto startLocation = ReadVector(mover, "Location");
    const auto key1 = start["path"]["keys"][1]["position"];
    const int steps0 = Steps();

    // Moved off its key (as a viewport move that leaves the path alone), then Add Key Here.
    WriteVector(mover, "Location", {startLocation.x, startLocation.y, startLocation.z + 200});
    Press(w, kAddKey);
    auto st = State(id);
    Check(Keys(st) == 3 && Current(st) == 2 && Near(st["path"]["keys"][2]["position"], 0, 0, 200) && Near(st["path"]["base"], base[0], base[1], base[2])
              && std::fabs(ReadVector(mover, "Location").z - (startLocation.z + 200)) < 0.01 && Steps() == steps0 + 1,
          "mover-add-key-here", "keys=" + std::to_string(Keys(st)) + " key2=" + Key(st, 2) + " undo '" + Undo().current + "' status '" + Status(w) + "'");
    Check(ListView_GetItemCount(list) == 3, "mover-list-follows", std::to_string(ListView_GetItemCount(list)));

    // Go to Key 0 (as KeyNum does): the mover goes back to the base.
    Pick(w, 0);
    Press(w, kGoToKey);
    st = State(id);
    auto at = ReadVector(mover, "Location");
    Check(Current(st) == 0 && std::fabs(at.x - base[0].get<double>()) < 0.01 && std::fabs(at.z - base[2].get<double>()) < 0.01 && Steps() == steps0 + 2,
          "mover-go-to-key", "at " + VecText(at) + " undo '" + Undo().current + "'");

    // The stock editor's move at key 0 takes the base along (PostEditMove); Add Key Here keeps the path.
    {
        auto b = ReadVector(mover, "BasePos");
        WriteVector(mover, "BasePos", {b.x, b.y + 64, b.z});
        WriteVector(mover, "Location", {at.x, at.y + 64, at.z});
        Pump(700); // the window's follow timer sees it
        Info("mover-moved-status", Status(w));
        Press(w, kAddKey);
        st = State(id);
        Check(Keys(st) == 4 && Current(st) == 3 && Near(st["path"]["keys"][3]["position"], 0, 64, 0) && Near(st["path"]["base"], base[0], base[1], base[2])
                  && Near(st["path"]["keys"][1]["position"], key1[0], key1[1], key1[2]),
              "mover-add-after-drag", "key3=" + Key(st, 3) + " base " + st["path"]["base"].dump());
    }

    // Update Key 1 to where the mover is (key 3's place), Move Up key 3, Delete it.
    Pick(w, 1);
    Press(w, kUpdateKey);
    st = State(id);
    Check(Near(st["path"]["keys"][1]["position"], 0, 64, 0) && Current(st) == 1 && Steps() == steps0 + 4, "mover-update-key", "key1=" + Key(st, 1) + " undo '" + Undo().current + "'");
    Pick(w, 3);
    Press(w, kMoveUp);
    st = State(id);
    Check(Near(st["path"]["keys"][2]["position"], 0, 64, 0) && Near(st["path"]["keys"][3]["position"], 0, 0, 200) && Steps() == steps0 + 5, "mover-move-up",
          "key2=" + Key(st, 2) + " key3=" + Key(st, 3));
    Pick(w, 3);
    Press(w, kDeleteKey);
    st = State(id);
    Check(Keys(st) == 3 && Steps() == steps0 + 6, "mover-delete-key", "keys=" + std::to_string(Keys(st)) + " undo '" + Undo().current + "'");

    // Every edit was one step: six Undos put the mover back as it was.
    for (int i = 0; i < 6; ++i) { Exec("TRANSACTION UNDO"); Pump(150); }
    st = State(id);
    at = ReadVector(mover, "Location");
    Check(Keys(st) == 2 && Current(st) == 0 && Near(st["path"]["keys"][1]["position"], key1[0], key1[1], key1[2]) && Steps() == steps0
              && std::fabs(at.x - startLocation.x) < 0.01 && std::fabs(at.y - startLocation.y) < 0.01,
          "mover-undo-all", "keys=" + std::to_string(Keys(st)) + " at " + VecText(at) + " steps " + std::to_string(Steps() - steps0));
    Pump(700);

    // Settings: MoveTime and the trigger state in words, one Undo step.
    SetWindowTextA(GetDlgItem(w, kFieldFirst + kMoveTimeField), "3.5");
    HWND states = GetDlgItem(w, kFieldFirst + kInitialStateField);
    int toggle = -1;
    std::string labels;
    for (int i = 0; i < static_cast<int>(SendMessageA(states, CB_GETCOUNT, 0, 0)); ++i)
    {
        char text[256] = {};
        SendMessageA(states, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(text));
        labels += std::string(text) + "; ";
        if (strstr(text, "(TriggerToggle)")) toggle = i;
    }
    Info("mover-states", labels);
    SendMessageA(states, CB_SETCURSEL, toggle, 0);
    Press(w, kApply);
    st = State(id);
    Check(toggle >= 0 && st["values"]["MoveTime"] == "3.500000" && st["values"]["InitialState"] == "TriggerToggle" && Steps() == steps0 + 1,
          "mover-settings", st["values"]["MoveTime"].dump() + " " + st["values"]["InitialState"].dump() + " status '" + Status(w) + "' undo '" + Undo().current + "'");
    SetWindowTextA(GetDlgItem(w, kFieldFirst + kMoveTimeField), "abc");
    Press(w, kApply);
    Check(Status(w).find("seconds") != std::string::npos && Steps() == steps0 + 1, "mover-settings-refuses", Status(w));
    Exec("TRANSACTION UNDO");
    Pump(800);

    // Wire the first candidate trigger: its Event names the mover's Tag, one Undo step.
    HWND wires = GetDlgItem(w, kWireList);
    const int candidates = static_cast<int>(SendMessageA(wires, CB_GETCOUNT, 0, 0));
    auto list2 = Workflow({{"op", "mover.candidates"}, {"actor", id}});
    Info("mover-candidates", std::to_string(candidates));
    if (candidates > 0 && list2.value("ok", false))
    {
        int pick = 0;
        for (int i = 0; i < candidates; ++i)
            if (list2["result"][i]["class"].get<std::string>().find("MagicEvent") == std::string::npos) { pick = i; break; }
        SendMessageA(wires, CB_SETCURSEL, pick, 0);
        const auto trigger = list2["result"][pick];
        const int before = Steps();
        Press(w, kWire);
        st = State(id);
        const auto tag = st["values"]["Tag"].get<std::string>();
        bool wired = false;
        for (const auto& link : st["triggeredBy"]) if (link["actor"] == trigger["path"]) wired = true;
        Check(wired && Steps() == before + 1 && Status(w).find("now triggers") != std::string::npos, "mover-wire", trigger["path"].get<std::string>() + " -> " + tag + " status '" + Status(w) + "'");
        Exec("TRANSACTION UNDO");
        Pump(800);
    }
    else Fail("mover-wire", "no candidates");

    // Preview plays it in the viewports and puts it back, with no Undo step.
    const int beforePreview = Steps();
    const auto beforeLocation = ReadVector(mover, "Location");
    Press(w, kPreview);
    Pump(900);
    auto moving = ReadVector(mover, "Location");
    Check(!Same(moving, beforeLocation), "mover-preview-moves", VecText(moving) + " status '" + Status(w) + "'");
    Press(w, kPreview);
    Pump(300);
    Check(Same(ReadVector(mover, "Location"), beforeLocation) && Steps() == beforePreview, "mover-preview-restores",
          VecText(ReadVector(mover, "Location")) + " steps " + std::to_string(Steps() - beforePreview));

    // Players in Match is offered for a mover.
    Check(IsWindowEnabled(GetDlgItem(w, kPlayers)) != 0, "mover-players", WindowText(GetDlgItem(w, kPlayersText)));
    CloseWindowAndWait(w);
    Check(!OpenedWindow("Mover Workbench"), "mover-closed");
    if (Ini("picture") == "1") MoverPicture(mover, id);
}
