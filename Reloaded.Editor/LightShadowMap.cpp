#include "pch.h"
#undef min
#undef max
#include "LightShadowMap.h"
#include "WindowPlacement.h"
#include "Appearance.h"
#include "LightShadowModel.h"
#include "MeasureModel.h"
#include "WorkflowEditor.h"
#include "GridSizeShortcutState.h"
#include "logger.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace LightShadowMap
{
namespace
{
using Address = uintptr_t;
using LightShadow::Band;
using LightShadow::Cell;
using LightShadow::Settings;
using LightShadow::Source;
namespace Measure = Workflow::Measure;

const char kTitle[] = "Light and Shadow Map";
const char kSection[] = "LightShadowMap";

constexpr Address kLevelViewportConfigs = 0x1165e8d4, kLevelViewportCount = 0x1165e8d8;
// FSceneNode: WorldToScreen and ScreenToWorld, as Measure reads them.
constexpr Address kWorldToScreen = 0x114, kScreenToWorld = 0x154;
// FLineBatcher(RI, ZTest), DrawLine, ~FLineBatcher (flushes); UCanvas SetClip,
// WrappedPrintf and WrappedStrLenf. The same calls MeasureTool.cpp documents.
constexpr Address kLineBatcher = 0x11176c80, kLineBatcherDraw = 0x11177480, kLineBatcherEnd = 0x11179ae0;
constexpr Address kCanvasSetClip = 0x110b3860, kCanvasPrint = 0x110b6a30, kCanvasTextSize = 0x110b6970;

// FColor is B,G,R,A in memory.
constexpr uint32_t kBandColour[3] = { 0xFF3C82FF, 0xFFFFD21E, 0xFFFF3C3C };
constexpr uint32_t kTextColour = 0xFFF0F0F0, kShadowColour = 0xFF000000;
// The 3D view draws patches this close to its camera; further ones are specks.
constexpr double kViewDistance = 8192;

struct State
{
    Settings settings;
    bool loaded = false;
    bool shown = false, planLayer = false;
    std::vector<Cell> cells;
    // The in-game lights' shares at each patch, kept so the next sample works
    // out only what an edit changed.
    LightShadow::LightCache cache;
    bool truncated = false, sampled = false, attempted = false;
    unsigned generation = 0, revision = 0;
    // When the map last changed under the patches; they are sampled again
    // once it has been still for a moment.
    bool stale = false;
    std::chrono::steady_clock::time_point changed{};
    UINT_PTR timer = 0;
    std::string status;
    bool overlayFaulted = false;
    HWND window = nullptr;
} state;

std::string IniPath()
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    return (std::filesystem::path(exe).parent_path() / "Reloaded_Editor.ini").string();
}
const struct { const char* key; int Settings::* field; } kKeys[] = {
    { "HiddenBelow", &Settings::hiddenBelow }, { "ExposedFrom", &Settings::exposedFrom },
    { "PatchSize", &Settings::cell }, { "ChestHeight", &Settings::chest } };
const struct { const char* key; bool Settings::* field; } kSwitches[] = {
    { "MeshShadows", &Settings::meshShadows }, { "MeshFloors", &Settings::meshFloors }, { "SpotCones", &Settings::spotCones } };

// Written once with the defaults so the section can be found and edited.
void Load()
{
    if (state.loaded) return;
    state.loaded = true;
    const auto ini = IniPath();
    Settings s;
    for (const auto& key : kKeys)
    {
        char value[32] = {};
        GetPrivateProfileStringA(kSection, key.key, "", value, sizeof(value), ini.c_str());
        if (!value[0]) WritePrivateProfileStringA(kSection, key.key, std::to_string(s.*key.field).c_str(), ini.c_str());
        else s.*key.field = GetPrivateProfileIntA(kSection, key.key, s.*key.field, ini.c_str());
    }
    for (const auto& key : kSwitches)
    {
        char value[32] = {};
        GetPrivateProfileStringA(kSection, key.key, "", value, sizeof(value), ini.c_str());
        if (!value[0]) WritePrivateProfileStringA(kSection, key.key, s.*key.field ? "1" : "0", ini.c_str());
        else s.*key.field = GetPrivateProfileIntA(kSection, key.key, 1, ini.c_str()) != 0;
    }
    char value[32] = {};
    GetPrivateProfileStringA(kSection, "Source", "", value, sizeof(value), ini.c_str());
    if (!value[0]) WritePrivateProfileStringA(kSection, "Source", "0", ini.c_str());
    else s.source = static_cast<Source>(GetPrivateProfileIntA(kSection, "Source", 0, ini.c_str()));
    state.settings = LightShadow::Clamp(s);
}
void Save()
{
    const auto ini = IniPath();
    for (const auto& key : kKeys)
        WritePrivateProfileStringA(kSection, key.key, std::to_string(state.settings.*key.field).c_str(), ini.c_str());
    for (const auto& key : kSwitches)
        WritePrivateProfileStringA(kSection, key.key, state.settings.*key.field ? "1" : "0", ini.c_str());
    WritePrivateProfileStringA(kSection, "Source", std::to_string(static_cast<int>(state.settings.source)).c_str(), ini.c_str());
}

// The editor's main frame: this process's top-level UnrealEd frame window.
HWND Frame()
{
    for (HWND window = FindWindowExA(nullptr, nullptr, "SplinterCell2UnrealWEditorFrame", nullptr); window;
         window = FindWindowExA(nullptr, window, "SplinterCell2UnrealWEditorFrame", nullptr))
    {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId()) return window;
    }
    return nullptr;
}
void Redraw()
{
    try { Workflow::Editor::Redraw(); }
    catch (const std::exception&) { /* No map: nothing to draw into. */ }
}
void SetStatus(const std::string& text);

// The 10th/50th/90th percentile of one part of the brightness, for the log.
std::string Parts(bool lights)
{
    auto cells = state.cells;
    for (auto& cell : cells) cell.value = lights ? cell.lights : cell.baked;
    return std::to_string(int(LightShadow::Percentile(cells, 0.1))) + "/" + std::to_string(int(LightShadow::Percentile(cells, 0.5)))
        + "/" + std::to_string(int(LightShadow::Percentile(cells, 0.9)));
}
// Samples the open map's floor. Throws with a plain message when it cannot.
// Starting afresh drops everything kept from the last sample (after a lighting
// build, say); otherwise only what changed is worked out again.
void Sample(bool afresh = false)
{
    Load();
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b) { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count()); };
    const auto started = Clock::now();
    if (afresh)
    {
        Workflow::Editor::LightShadowForget();
        state.cache = LightShadow::LightCache{};
    }
    LightShadow::Scene scene;
    const auto read = Workflow::Editor::LightShadowScene(scene, state.settings);
    const auto readDone = Clock::now();
    auto result = LightShadow::SampleCells(scene, state.settings);
    const auto cellsDone = Clock::now();
    LightShadow::RelightStats lit;
    try { LightShadow::Relight(scene, result.cells, state.settings, state.cache, &lit); }
    catch (...) { state.cache = LightShadow::LightCache{}; throw; }
    const auto done = Clock::now();
    state.cells = std::move(result.cells);
    state.truncated = result.truncated;
    state.sampled = state.attempted = true;
    state.stale = false;
    state.generation = Workflow::Editor::MapGeneration();
    state.revision = Workflow::Editor::ContentRevision();
    const auto summary = LightShadow::Summarise(state.cells);
    Logger::log("LightShadowMap: " + std::to_string(read.bspFloors) + " BSP and " + std::to_string(read.meshFloors) + " static mesh floor triangles, "
                + std::to_string(scene.atlases.size()) + " lightmap atlases, " + std::to_string(scene.instances.size()) + " static meshes ("
                + std::to_string(scene.shapes.size()) + " shapes, " + std::to_string(read.meshTriangles) + " triangles), "
                + std::to_string(scene.lights.size()) + " in-game lights -> " + std::to_string(summary.total) + " patches ("
                + std::to_string(summary.mesh) + " on static meshes; " + std::to_string(summary.bands[0]) + " hidden, "
                + std::to_string(summary.bands[1]) + " partly, " + std::to_string(summary.bands[2]) + " exposed, " + std::to_string(summary.unbaked)
                + " without baked lighting) in " + ms(started, done) + " ms (read " + ms(started, readDone) + ", patches " + ms(readDone, cellsDone)
                + ", lights " + ms(cellsDone, done) + (lit.full ? ", from nothing" : ", kept " + std::to_string(lit.reused) + " of " + std::to_string(lit.lights) + " lights")
                + ", " + std::to_string(lit.tested) + " lines of sight); brightness at the 10th/50th/90th percentile "
                + std::to_string(int(LightShadow::Percentile(state.cells, 0.1))) + "/" + std::to_string(int(LightShadow::Percentile(state.cells, 0.5)))
                + "/" + std::to_string(int(LightShadow::Percentile(state.cells, 0.9))) + " (baked " + Parts(false) + ", lights " + Parts(true) + ")");
}
bool TrySample(bool afresh = false)
{
    try { Sample(afresh); SetStatus(""); return true; }
    catch (const std::exception& e)
    {
        state.cells.clear();
        state.sampled = false;
        state.stale = false;
        // Not again until the map changes.
        try { state.generation = Workflow::Editor::MapGeneration(); state.revision = Workflow::Editor::ContentRevision(); state.attempted = true; }
        catch (const std::exception&) { state.attempted = false; }
        SetStatus(e.what());
        Logger::log(std::string("LightShadowMap: not sampled: ") + e.what());
        return false;
    }
}

// While the map is shown: sample again once an edit has settled, or at once
// for another map.
void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD)
{
    if (!state.shown && !state.planLayer) return;
    try
    {
        const auto generation = Workflow::Editor::MapGeneration(), revision = Workflow::Editor::ContentRevision();
        const auto now = std::chrono::steady_clock::now();
        if (!state.attempted || generation != state.generation)
        {
            TrySample();
            Redraw();
            return;
        }
        if (revision != state.revision)
        {
            state.revision = revision;
            state.stale = true;
            state.changed = now;
            return;
        }
        if (state.stale && now - state.changed > std::chrono::milliseconds(1500))
        {
            TrySample();
            Redraw();
        }
    }
    catch (const std::exception&)
    {
        // No map open: drop the old map's patches.
        if (state.sampled) { state.cells.clear(); state.sampled = false; }
        state.attempted = false;
    }
}
void UpdateTimer()
{
    const bool wanted = state.shown || state.planLayer;
    if (wanted && !state.timer) state.timer = SetTimer(nullptr, 0, 700, Tick);
    else if (!wanted && state.timer) { KillTimer(nullptr, state.timer); state.timer = 0; }
}
void UpdateMenu()
{
    if (auto frame = Frame())
        if (auto menu = GetMenu(frame))
            CheckMenuItem(menu, kToggle, MF_BYCOMMAND | (state.shown ? MF_CHECKED : MF_UNCHECKED));
    if (IsWindow(state.window)) CheckDlgButton(state.window, 110, state.shown ? BST_CHECKED : BST_UNCHECKED);
}
void Show(bool on)
{
    state.shown = on;
    // Turning it on samples afresh, which also picks up a lighting build.
    if (on) TrySample(true);
    UpdateTimer();
    UpdateMenu();
    Redraw();
}

// ---- Viewport overlay ------------------------------------------------------

bool Copy(void* destination, const void* source, size_t size)
{
    __try { memcpy(destination, source, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template<class T> T Read(Address address)
{
    T value{};
    if (!address || !Copy(&value, reinterpret_cast<const void*>(address), sizeof(value)))
        throw std::runtime_error("Editor state changed or is unavailable.");
    return value;
}
bool IsLevelViewport(Address viewport)
{
    void* configs = nullptr; int count = 0;
    if (!Copy(&configs, reinterpret_cast<const void*>(kLevelViewportConfigs), 4)
        || !Copy(&count, reinterpret_cast<const void*>(kLevelViewportCount), 4))
        return false;
    return GridSizeShortcut::Detail::IsLevelViewport(reinterpret_cast<void*>(viewport), configs, count);
}

struct FVec { float x, y, z; };
FVec ToF(const LightShadow::Point& p) { return { float(p.x), float(p.y), float(p.z) }; }

// Text lines in a viewport corner, each in its own colour, with a shadow.
void DrawText(Address viewport, int height, const std::vector<std::pair<std::string, uint32_t>>& text, bool bottom)
{
    if (text.empty()) return;
    const auto canvas = Read<Address>(viewport + 0x68);
    const auto font = Read<Address>(canvas + 0x64);
    if (!font) return;
    using SetClipFn = void(__thiscall*)(Address, int, int, int, int);
    using PrintFn = void(__cdecl*)(Address, Address, int, const char*, ...);
    using SizeFn = void(__cdecl*)(Address, Address, int*, int*, const char*, ...);
    int width = 0, lineHeight = 0;
    for (const auto& line : text)
    {
        int x = 0, y = 0;
        reinterpret_cast<SizeFn>(kCanvasTextSize)(canvas, font, &x, &y, "%s", line.first.c_str());
        width = std::max(width, x); lineHeight = std::max(lineHeight, y);
    }
    if (lineHeight <= 0) lineHeight = 12;
    const int total = lineHeight * static_cast<int>(text.size());
    const int left = 6, top = bottom ? std::max(2, height - total - 6) : 6;
    unsigned char saved[0x18] = {}; uint32_t savedColour = 0;
    if (!Copy(saved, reinterpret_cast<const void*>(canvas + 0x2c), sizeof(saved)) || !Copy(&savedColour, reinterpret_cast<const void*>(canvas + 0x50), 4)) return;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < text.size(); ++i)
        {
            const uint32_t colour = pass ? text[i].second : kShadowColour;
            Copy(reinterpret_cast<void*>(canvas + 0x50), &colour, 4);
            const int shadow = pass ? 0 : 1;
            reinterpret_cast<SetClipFn>(kCanvasSetClip)(canvas, left + shadow, top + shadow + static_cast<int>(i) * lineHeight, width + 8, lineHeight + 2);
            reinterpret_cast<PrintFn>(kCanvasPrint)(canvas, font, 0, "%s", text[i].first.c_str());
        }
    Copy(reinterpret_cast<void*>(canvas + 0x2c), saved, sizeof(saved));
    Copy(reinterpret_cast<void*>(canvas + 0x50), &savedColour, 4);
}

// The patch under the mouse in a top view, for the readout.
const Cell* UnderMouse(Address viewport, const Measure::Matrix& screenToWorld, int width, int height, int hidden)
{
    if (hidden != 2) return nullptr;
    const auto window = Read<Address>(viewport + 0x1b4);
    const HWND hwnd = window ? Read<HWND>(window + 4) : nullptr;
    POINT mouse{};
    if (!hwnd || !GetCursorPos(&mouse) || WindowFromPoint(mouse) != hwnd || !ScreenToClient(hwnd, &mouse)) return nullptr;
    if (mouse.x < 0 || mouse.y < 0 || mouse.x >= width || mouse.y >= height) return nullptr;
    const auto at = Measure::Deproject(screenToWorld, mouse.x + 0.5, mouse.y + 0.5, 0.5, width, height);
    return LightShadow::At(state.cells, at[0], at[1], state.settings.cell);
}

void DrawChecked(Address viewport, Address node)
{
    if (!state.shown || !IsLevelViewport(viewport)) return;
    const int width = Read<int>(viewport + 0xa0), height = Read<int>(viewport + 0xa4);
    if (width <= 0 || height <= 0) return;
    const auto worldToScreen = Read<Measure::Matrix>(node + kWorldToScreen);
    const auto screenToWorld = Read<Measure::Matrix>(node + kScreenToWorld);
    const auto camera = Read<Address>(viewport + 0x30);
    const int hidden = Measure::DepthAxis(Read<int>(camera + 0x4fc));
    const auto location = Read<std::array<float, 3>>(camera + 0x80);

    // Front and side views would show the patches edge-on: legend only.
    if (!state.cells.empty() && hidden != 0 && hidden != 1)
    {
        const auto ri = Read<Address>(viewport + 0x164);
        alignas(16) unsigned char batcher[0x40] = {};
        // Depth-tested in the 3D view, so walls hide the floor behind them;
        // 2D views draw every storey.
        reinterpret_cast<void*(__thiscall*)(void*, Address, int)>(kLineBatcher)(batcher, ri, hidden < 0 ? 1 : 0);
        const double size = state.settings.cell;
        for (const auto& cell : state.cells)
        {
            if (hidden < 0)
            {
                const double dx = cell.at.x - location[0], dy = cell.at.y - location[1], dz = cell.at.z - location[2];
                if (dx * dx + dy * dy + dz * dz > kViewDistance * kViewDistance) continue;
            }
            const auto p = Measure::Transform(worldToScreen, { cell.at.x, cell.at.y, cell.at.z, 1 });
            if (!(p[3] > 1e-6)) continue;
            const double margin = 1.25;
            if (std::abs(p[0] / p[3]) > margin || std::abs(p[1] / p[3]) > margin) continue;
            const uint32_t colour = kBandColour[static_cast<int>(cell.band)];
            for (const auto& [a, b] : LightShadow::Outline(cell, size))
                reinterpret_cast<void(__thiscall*)(void*, FVec, FVec, uint32_t)>(kLineBatcherDraw)(batcher, ToF(a), ToF(b), colour);
        }
        reinterpret_cast<void(__thiscall*)(void*)>(kLineBatcherEnd)(batcher);
    }

    std::vector<std::pair<std::string, uint32_t>> text;
    if (!state.sampled)
        text.push_back({ "Light and Shadow: " + (state.status.empty() ? std::string("not sampled") : state.status), kTextColour });
    else
    {
        const auto lines = LightShadow::Legend(LightShadow::Summarise(state.cells), state.settings, state.truncated, state.stale);
        for (size_t i = 0; i < lines.size(); ++i)
            text.push_back({ lines[i], i >= 1 && i <= 3 ? kBandColour[i - 1] : kTextColour });
        if (const auto* cell = UnderMouse(viewport, screenToWorld, width, height, hidden))
            text.push_back({ "Under the mouse: " + LightShadow::Describe(*cell), kBandColour[static_cast<int>(cell->band)] });
    }
    DrawText(viewport, height, text, true);
}
void DrawGuarded(Address viewport, Address node)
{
    try { DrawChecked(viewport, node); }
    catch (const std::exception& e)
    {
        if (!state.overlayFaulted) Logger::log(std::string("LightShadowMap: overlay skipped: ") + e.what());
        state.overlayFaulted = true;
    }
}
void LogFault() { Logger::log("LightShadowMap: overlay faulted and was skipped"); }

// ---- Settings window ---------------------------------------------------------

enum : int { kIntro = 100, kHiddenEdit, kExposedEdit, kPatchEdit, kChestEdit, kSourceCombo, kApply, kRefreshButton, kStatusText, kSummary, kShowCheck = 110,
             kMeshShadowsCheck, kMeshFloorsCheck, kSpotConesCheck };

void SetStatus(const std::string& text)
{
    state.status = text;
    if (IsWindow(state.window)) SetDlgItemTextA(state.window, kStatusText, text.c_str());
}
std::string SummaryText()
{
    if (!state.sampled) return "Not sampled yet.";
    std::string text;
    for (const auto& line : LightShadow::Legend(LightShadow::Summarise(state.cells), state.settings, state.truncated, state.stale))
        text += (text.empty() ? "" : "\r\n") + line;
    return text;
}
void FillWindow()
{
    if (!IsWindow(state.window)) return;
    const auto& s = state.settings;
    SetDlgItemInt(state.window, kHiddenEdit, s.hiddenBelow, FALSE);
    SetDlgItemInt(state.window, kExposedEdit, s.exposedFrom, FALSE);
    SetDlgItemInt(state.window, kPatchEdit, s.cell, FALSE);
    SetDlgItemInt(state.window, kChestEdit, s.chest, FALSE);
    SendDlgItemMessageA(state.window, kSourceCombo, CB_SETCURSEL, static_cast<int>(s.source), 0);
    CheckDlgButton(state.window, kShowCheck, state.shown ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(state.window, kMeshShadowsCheck, s.meshShadows ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(state.window, kMeshFloorsCheck, s.meshFloors ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(state.window, kSpotConesCheck, s.spotCones ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextA(state.window, kSummary, SummaryText().c_str());
}
void Apply()
{
    Settings s = state.settings;
    BOOL ok = TRUE;
    auto number = [&](int id, int& field) { BOOL good = FALSE; const UINT v = GetDlgItemInt(state.window, id, &good, FALSE); if (good) field = static_cast<int>(v); else ok = FALSE; };
    number(kHiddenEdit, s.hiddenBelow);
    number(kExposedEdit, s.exposedFrom);
    number(kPatchEdit, s.cell);
    number(kChestEdit, s.chest);
    const auto source = SendDlgItemMessageA(state.window, kSourceCombo, CB_GETCURSEL, 0, 0);
    if (source >= 0) s.source = static_cast<Source>(source);
    s.meshShadows = IsDlgButtonChecked(state.window, kMeshShadowsCheck) == BST_CHECKED;
    s.meshFloors = IsDlgButtonChecked(state.window, kMeshFloorsCheck) == BST_CHECKED;
    s.spotCones = IsDlgButtonChecked(state.window, kSpotConesCheck) == BST_CHECKED;
    s = LightShadow::Clamp(s);
    const bool resample = s.cell != state.settings.cell || s.chest != state.settings.chest || s.meshShadows != state.settings.meshShadows
        || s.meshFloors != state.settings.meshFloors || s.spotCones != state.settings.spotCones;
    state.settings = s;
    Save();
    if (resample || !state.sampled) TrySample();
    else LightShadow::ApplyBands(state.cells, state.settings);
    FillWindow();
    if (state.status.empty())
        SetStatus(ok ? "Saved to Reloaded_Editor.ini [LightShadowMap]." : "Saved; a field that was not a whole number kept its old value.");
    Redraw();
}

HWND Control(HWND window, const char* type, const char* text, DWORD style, int id, int x, int y, int width, int height)
{
    auto control = CreateWindowExA(type == std::string("EDIT") ? WS_EX_CLIENTEDGE : 0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    SendMessage(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}

const char kIntroText[] =
    "Colours the walkable floor by how lit it is, to show where a spy can hide: blue hidden, yellow partly visible, red exposed "
    "(also told apart by the lines: a square, one diagonal, both). Brightness runs from 0 (black) to 255.\r\n\r\n"
    "It is an estimate, not the game's own visibility formula. Baked is the floor's lightmap from Build Lighting (on a static mesh, "
    "the static lights Build Lighting found reaching it). In-game lights are the InGame and Dynamic lights reaching a point at the "
    "height set below, with the engine's falloff, blocked by BSP and static meshes. Projectors, movers and flicker are ignored. "
    "Playtest to confirm.";

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            // The intro keeps all its lines in the larger modern font; everything below moves down.
            auto intro = Control(window, "STATIC", kIntroText, 0, kIntro, 12, 10, 456, 128);
            const int introHeight = Appearance::FitHeight(intro, 456, 128);
            const int dy = introHeight - 128;
            if (dy) MoveWindow(intro, 12, 10, 456, introHeight, FALSE);
            Control(window, "BUTTON", "&Show in the viewports", BS_AUTOCHECKBOX | WS_TABSTOP, kShowCheck, 12, 146 + dy, 220, 22);
            const std::pair<int, const char*> fields[] = {
                { kHiddenEdit, "Hidden below" }, { kExposedEdit, "Exposed from" },
                { kPatchEdit, "Patch size (units)" }, { kChestEdit, "Lights measured at height" } };
            for (int i = 0; i < 4; ++i)
            {
                const int x = 12 + (i % 2) * 236, y = 176 + dy + (i / 2) * 28;
                Control(window, "STATIC", fields[i].second, 0, 0, x, y + 3, 140, 20);
                Control(window, "EDIT", "", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, fields[i].first, x + 144, y, 70, 22);
            }
            Control(window, "STATIC", "Brightness from", 0, 0, 12, 235 + dy, 140, 20);
            auto combo = Control(window, "COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kSourceCombo, 156, 232 + dy, 312, 120);
            for (auto source : { Source::Combined, Source::Lights, Source::Baked })
                SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(LightShadow::SourceName(source)));
            Control(window, "BUTTON", "Static &meshes block light", BS_AUTOCHECKBOX | WS_TABSTOP, kMeshShadowsCheck, 12, 262 + dy, 166, 22);
            Control(window, "BUTTON", "Static mesh &floors", BS_AUTOCHECKBOX | WS_TABSTOP, kMeshFloorsCheck, 184, 262 + dy, 130, 22);
            Control(window, "BUTTON", "Spotlight &cones", BS_AUTOCHECKBOX | WS_TABSTOP, kSpotConesCheck, 320, 262 + dy, 148, 22);
            Control(window, "STATIC", "", 0, kSummary, 12, 292 + dy, 456, 84);
            Control(window, "STATIC", "", 0, kStatusText, 12, 378 + dy, 456, 34);
            Control(window, "BUTTON", "&Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, kApply, 12, 418 + dy, 90, 26);
            Control(window, "BUTTON", "&Refresh", WS_TABSTOP, kRefreshButton, 110, 418 + dy, 90, 26);
            Control(window, "BUTTON", "Close", WS_TABSTOP, IDCANCEL, 378, 418 + dy, 90, 26);
            if (dy)
            {
                RECT frame{};
                GetWindowRect(window, &frame);
                SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top + dy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w))
            {
            case kApply: Apply(); return 0;
            case kRefreshButton: if (TrySample(true)) SetStatus("Sampled again."); FillWindow(); Redraw(); return 0;
            case kShowCheck: Show(IsDlgButtonChecked(window, kShowCheck) == BST_CHECKED); FillWindow(); return 0;
            case IDCANCEL: DestroyWindow(window); return 0;
            }
            break;
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_DESTROY: WindowPlacement::Remember(window, "LightShadow"); state.window = nullptr; return 0;
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

void OpenSettings(HWND owner)
{
    Load();
    if (!IsWindow(state.window))
    {
        WNDCLASSA wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "ReloadedLightShadowMap";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassA(&wc);
        RECT rect{ 0, 0, 480, 456 };
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        AdjustWindowRectEx(&rect, style, FALSE, WS_EX_TOOLWINDOW);
        state.window = CreateWindowExA(WS_EX_TOOLWINDOW, wc.lpszClassName, kTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                       rect.right - rect.left, rect.bottom - rect.top, owner, nullptr, wc.hInstance, nullptr);
        if (!state.window) throw std::runtime_error("Could not open the Light and Shadow Map window.");
        WindowPlacement::Restore(state.window, "LightShadow");
    }
    FillWindow();
    SetDlgItemTextA(state.window, kStatusText, state.status.c_str());
    ShowWindow(state.window, SW_SHOW);
    SetForegroundWindow(state.window);
}
} // namespace

bool HandleCommand(UINT command)
{
    if (command != kToggle && command != kSettings && command != kRefresh) return false;
    try
    {
        Load();
        if (command == kToggle) Show(!state.shown);
        else if (command == kSettings) OpenSettings(GetActiveWindow());
        else { TrySample(true); FillWindow(); Redraw(); }
        if (command != kSettings && !state.status.empty()) MessageBeep(MB_ICONWARNING);
    }
    catch (const std::exception& e)
    {
        SetStatus(e.what());
        Logger::log(std::string("LightShadowMap: ") + e.what());
    }
    return true;
}

void DrawViewportOverlay(uintptr_t viewport, uintptr_t sceneNode)
{
    if (!state.shown) return;
    __try { DrawGuarded(viewport, sceneNode); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (!state.overlayFaulted) LogFault();
        state.overlayFaulted = true;
    }
}

const std::vector<LightShadow::Cell>& Patches()
{
    Load();
    bool fresh = state.sampled;
    if (fresh)
    {
        try { fresh = Workflow::Editor::MapGeneration() == state.generation; }
        catch (const std::exception&) { fresh = false; }
    }
    if (!fresh) TrySample();
    return state.cells;
}
int PatchSize() { Load(); return state.settings.cell; }
void SetPlanLayer(bool on)
{
    state.planLayer = on;
    UpdateTimer();
}
}
