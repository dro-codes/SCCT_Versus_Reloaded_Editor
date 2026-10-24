#include "pch.h"
#undef min
#undef max
#include "Reachability.h"
#include "ReachabilityModel.h"
#include "MapCheck.h"
#include "MeasureModel.h"
#include "WorkflowEditor.h"
#include "WorkflowTools.h"
#include "GridSizeShortcutState.h"
#include "logger.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace Reachability
{
namespace
{
using Address = uintptr_t;
using Clock = std::chrono::steady_clock;
namespace Measure = Workflow::Measure;
namespace Editor = Workflow::Editor;

const char kSection[] = "Reachability";

constexpr Address kLevelViewportConfigs = 0x1165e8d4, kLevelViewportCount = 0x1165e8d8;
// FSceneNode: WorldToScreen; FLineBatcher and the canvas calls, as
// LightShadowMap.cpp and MeasureTool.cpp use them.
constexpr Address kWorldToScreen = 0x114;
constexpr Address kLineBatcher = 0x11176c80, kLineBatcherDraw = 0x11177480, kLineBatcherEnd = 0x11179ae0;
constexpr Address kCanvasSetClip = 0x110b3860, kCanvasPrint = 0x110b6a30, kCanvasTextSize = 0x110b6970;

// FColor is B,G,R,A in memory: both teams green, spies only blue, mercs only red.
constexpr uint32_t kBoth = 0xFF3CD25A, kSpies = 0xFF3C82FF, kMercs = 0xFFFF3C3C;
constexpr uint32_t kTextColour = 0xFFF0F0F0, kShadowColour = 0xFF000000;
constexpr double kViewDistance = 8192;
// Work per timer tick, and how often, so the editor stays responsive while a
// run goes on (a quarter of the time is left free).
constexpr auto kSlice = std::chrono::milliseconds(30);
constexpr UINT kTickEvery = 40;
constexpr auto kShowEvery = std::chrono::milliseconds(1500);

struct Options
{
    Reach::Settings settings;
    double maxDrop[2] = {Reach::DefaultPawn(Reach::kSpy).maxDrop, Reach::DefaultPawn(Reach::kMerc).maxDrop};
    bool runWithMapCheck = true;
};

// A fill in progress. The world outlives the floods that ask it.
struct Run
{
    Reach::Scene scene;
    std::unique_ptr<Reach::World> world;
    std::unique_ptr<Reach::Flood> floods[2];
    int phase = 0;   // the team being filled; 2 when both are done
    unsigned generation = 0, revision = 0;
    Clock::time_point started{}, shown{};
};

struct State
{
    bool loaded = false;
    Options options;
    bool shown = false, planLayer = false, notifyMapCheck = false;
    std::unique_ptr<Run> run;
    // The last finished run (or the cells of the one going on).
    bool done = false;
    unsigned generation = 0, revision = 0;
    std::vector<Reach::Patch> patches;
    Reach::Summary summary;
    std::vector<Reach::Finding> findings;
    size_t cells[2] = {};
    bool worked[2] = {};
    double cell = 32, seconds = 0;
    // A run cancelled or failed on this map as it was: Map Check and the plan
    // do not start another by themselves until it changes.
    bool stopped = false;
    unsigned stoppedGeneration = 0, stoppedRevision = 0;
    std::string status;
    UINT_PTR timer = 0;
    unsigned version = 0;
    bool overlayFaulted = false;
} state;

std::string IniPath()
{
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    return (std::filesystem::path(exe).parent_path() / "Reloaded_Editor.ini").string();
}
// Written once with the defaults so the section can be found and edited.
void Load()
{
    if (state.loaded) return;
    state.loaded = true;
    const auto ini = IniPath();
    const Options defaults;
    const struct { const char* key; std::string value; } keys[] = {
        {"CellSize", std::to_string(static_cast<int>(defaults.settings.cell))},
        {"MaxCells", std::to_string(defaults.settings.maxCells)},
        {"ReachDistance", std::to_string(static_cast<int>(defaults.settings.reach))},
        {"SpyMaxDrop", std::to_string(static_cast<int>(defaults.maxDrop[0]))},
        {"MercMaxDrop", std::to_string(static_cast<int>(defaults.maxDrop[1]))},
        {"RunWithMapCheck", defaults.runWithMapCheck ? "1" : "0"}};
    for (const auto& key : keys)
    {
        char value[32] = {};
        GetPrivateProfileStringA(kSection, key.key, "", value, sizeof(value), ini.c_str());
        if (!value[0]) WritePrivateProfileStringA(kSection, key.key, key.value.c_str(), ini.c_str());
    }
    auto number = [&](const char* key, double fallback)
    {
        char value[32] = {};
        GetPrivateProfileStringA(kSection, key, "", value, sizeof(value), ini.c_str());
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        return end != value && std::isfinite(parsed) ? parsed : fallback;
    };
    Options o;
    o.settings.cell = number("CellSize", defaults.settings.cell);
    o.settings.maxCells = static_cast<size_t>(std::clamp(number("MaxCells", static_cast<double>(defaults.settings.maxCells)), 0.0, 1e9));
    o.settings.reach = number("ReachDistance", defaults.settings.reach);
    o.settings = Reach::Clamp(o.settings);
    o.maxDrop[0] = number("SpyMaxDrop", defaults.maxDrop[0]);
    o.maxDrop[1] = number("MercMaxDrop", defaults.maxDrop[1]);
    o.runWithMapCheck = GetPrivateProfileIntA(kSection, "RunWithMapCheck", 1, ini.c_str()) != 0;
    state.options = o;
}

void Redraw()
{
    try { Editor::Redraw(); }
    catch (const std::exception&) { /* No map: nothing to draw into. */ }
}
void UpdateMenu()
{
    if (auto frame = Editor::MainWindow())
        if (auto menu = GetMenu(frame))
            CheckMenuItem(menu, kToggle, MF_BYCOMMAND | (state.shown ? MF_CHECKED : MF_UNCHECKED));
}
bool CurrentMap(unsigned& generation, unsigned& revision)
{
    try { generation = Editor::MapGeneration(); revision = Editor::ContentRevision(); return true; }
    catch (const std::exception&) { return false; }
}
bool Fresh()
{
    unsigned generation = 0, revision = 0;
    return state.done && CurrentMap(generation, revision) && generation == state.generation && revision == state.revision;
}
bool StoppedHere()
{
    unsigned generation = 0, revision = 0;
    return state.stopped && CurrentMap(generation, revision) && generation == state.stoppedGeneration && revision == state.stoppedRevision;
}

void Notify()
{
    ++state.version;
    if (state.notifyMapCheck)
    {
        state.notifyMapCheck = false;
        MapCheck::ReachabilityChanged();
    }
}
void StopTimer()
{
    if (state.timer) { KillTimer(nullptr, state.timer); state.timer = 0; }
}
// The run ends without a result: cancelled, another map, or a query failed.
void Abandon(const std::string& why)
{
    if (state.run)
    {
        state.stopped = true;
        state.stoppedGeneration = state.run->generation;
        state.stoppedRevision = state.run->revision;
    }
    state.run.reset();
    StopTimer();
    state.patches.clear();
    state.done = false;
    state.status = why;
    Logger::log("Reachability: " + why);
    Redraw();
    Notify();
}

std::vector<Reach::Patch> PartialPatches(const Run& run)
{
    const Reach::Flood* floods[2] = {run.floods[0].get(), run.floods[1].get()};
    return Reach::Patches(floods);
}

void Finish()
{
    auto& run = *state.run;
    const Reach::Flood* floods[2] = {run.floods[0].get(), run.floods[1].get()};
    state.patches = Reach::Patches(floods);
    state.summary = Reach::Summarise(state.patches);
    state.findings = Reach::Findings(run.scene, floods);
    state.cell = run.floods[0] ? run.floods[0]->GetSettings().cell : run.floods[1] ? run.floods[1]->GetSettings().cell : state.options.settings.cell;
    size_t calls = 0;
    for (int team : {Reach::kSpy, Reach::kMerc})
    {
        state.worked[team] = floods[team] != nullptr;
        state.cells[team] = floods[team] ? floods[team]->Nodes().size() : 0;
        calls += floods[team] ? floods[team]->Calls() : 0;
    }
    state.generation = run.generation;
    state.revision = run.revision;
    state.seconds = std::chrono::duration<double>(Clock::now() - run.started).count();
    state.done = true;
    state.stopped = false;
    state.status.clear();
    char took[32] = {};
    snprintf(took, sizeof(took), "%.1f", state.seconds);
    Logger::log("Reachability: spies " + std::to_string(state.cells[0]) + " cells, mercs " + std::to_string(state.cells[1]) + ", "
                + std::to_string(calls) + " collision queries in " + took + " s; " + std::to_string(state.summary.both) + " cells both teams, "
                + std::to_string(state.summary.spies) + " spies only, " + std::to_string(state.summary.mercs) + " mercs only; "
                + std::to_string(state.findings.size()) + " finding(s)");
    state.run.reset();
    StopTimer();
    Redraw();
    Notify();
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD)
{
    if (!state.run) { StopTimer(); return; }
    // Not under a modal dialog or a build's progress box, and not mid-drag.
    if (auto frame = Editor::MainWindow(); frame && !IsWindowEnabled(frame)) return;
    if ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) return;
    try
    {
        unsigned generation = 0, revision = 0;
        if (!CurrentMap(generation, revision) || generation != state.run->generation) { Abandon("Another map was opened, so the reachability run stopped."); return; }
        auto& run = *state.run;
        const auto deadline = Clock::now() + kSlice;
        while (Clock::now() < deadline)
        {
            auto* flood = run.floods[run.phase].get();
            if (!flood || flood->Step(4))
            {
                if (++run.phase >= 2) { Finish(); return; }
            }
        }
        if (Clock::now() - run.shown > kShowEvery)
        {
            run.shown = Clock::now();
            state.patches = PartialPatches(run);
            ++state.version;
            if (state.shown) Redraw();
        }
    }
    catch (const std::exception& e)
    {
        Abandon(std::string("The reachability run stopped: ") + e.what());
    }
}

// Starts a run on the open map. Throws with a plain message when it cannot.
void Start()
{
    Load();
    state.run.reset();
    StopTimer();
    auto run = std::make_unique<Run>();
    run->started = run->shown = Clock::now();
    run->scene = Editor::ReachabilityScene();
    // Step and walkable slope from Map Design's Movement limits when the map
    // has them, else the pawn classes' own step (ReachabilityScene).
    Workflow::Json movement;
    try { movement = WorkflowTools::MapMovementLimits(); }
    catch (const std::exception&) { movement = nullptr; }
    for (int team : {Reach::kSpy, Reach::kMerc})
    {
        auto pawn = run->scene.pawns[team];
        if (movement.is_object())
        {
            pawn.step = movement.value("stepHeight", pawn.step);
            pawn.slope = movement.value("slope", pawn.slope);
        }
        pawn.maxDrop = state.options.maxDrop[team];
        run->scene.pawns[team] = Reach::ClampPawn(pawn, team);
    }
    run->world = Editor::ReachabilityWorld();
    std::optional<Reach::Flood::Bounds> bounds;
    if (run->scene.boundsKnown) bounds = Reach::Flood::Bounds{run->scene.lo, run->scene.hi};
    for (int team : {Reach::kSpy, Reach::kMerc})
    {
        bool any = false;
        for (const auto& start : run->scene.starts) any = any || start.team == team;
        if (any) run->floods[team] = std::make_unique<Reach::Flood>(*run->world, run->scene.pawns[team], state.options.settings, bounds);
    }
    if (!run->floods[0] && !run->floods[1]) throw std::runtime_error("The map has no spy or merc player starts to walk from.");
    for (size_t i = 0; i < run->scene.starts.size(); ++i)
    {
        const auto& start = run->scene.starts[i];
        if (start.team == Reach::kSpy || start.team == Reach::kMerc) run->floods[start.team]->AddStart(static_cast<int>(i), start.location);
    }
    for (int team : {Reach::kSpy, Reach::kMerc})
    {
        const auto& p = run->scene.pawns[team];
        Logger::log(std::string("Reachability: ") + Reach::TeamName(team) + " radius " + Reach::Number(p.radius) + ", half-height " + Reach::Number(p.height)
                    + " (crouched " + Reach::Number(p.crouchHeight) + "), step " + Reach::Number(p.step) + ", slope " + Reach::Number(p.slope)
                    + ", drop " + Reach::Number(p.maxDrop) + "; " + std::to_string(run->scene.targets.size()) + " objectives and pickups");
    }
    if (!CurrentMap(run->generation, run->revision)) throw std::runtime_error("No map is open.");
    state.run = std::move(run);
    state.patches.clear();
    state.done = false;
    state.stopped = false;
    state.status.clear();
    state.timer = SetTimer(nullptr, 0, kTickEvery, Tick);
    ++state.version;
}
bool TryStart()
{
    try { Start(); return true; }
    catch (const std::exception& e)
    {
        state.run.reset();
        StopTimer();
        state.done = false;
        state.patches.clear();
        state.status = e.what();
        unsigned generation = 0, revision = 0;
        if (CurrentMap(generation, revision)) { state.stopped = true; state.stoppedGeneration = generation; state.stoppedRevision = revision; }
        Logger::log(std::string("Reachability: not worked out: ") + e.what());
        ++state.version;
        return false;
    }
}

// "spies 1,234 cells, mercs 56 cells" for the run going on.
std::string Counts()
{
    if (!state.run) return {};
    std::string text;
    for (int team : {Reach::kSpy, Reach::kMerc})
        if (state.run->floods[team]) text += (text.empty() ? "" : ", ") + std::string(Reach::TeamName(team)) + " " + Reach::Number(static_cast<double>(state.run->floods[team]->Nodes().size())) + " cells";
    return text;
}
std::string Progress() { return state.run ? "Reachability: working it out: " + Counts() : std::string(); }

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
FVec ToF(const Reach::Vec3& p) { return {float(p[0]), float(p[1]), float(p[2])}; }

// Text lines in the viewport's top-left corner, each in its own colour, with a shadow.
void DrawText(Address viewport, const std::vector<std::pair<std::string, uint32_t>>& text)
{
    if (text.empty()) return;
    const auto canvas = Read<Address>(viewport + 0x68);
    const auto font = Read<Address>(canvas + 0x64);
    if (!font) return;
    using SetClipFn = void(__thiscall*)(Address, int, int, int, int);
    using PrintFn = void(__cdecl*)(Address, Address, int, const char*, ...);
    using SizeFn = void(__cdecl*)(Address, Address, int*, int*, const char*, ...);
    // A line too wide for a small viewport wraps and measures taller: the
    // shortest height is one line's.
    int width = 0, lineHeight = 0;
    for (const auto& line : text)
    {
        int x = 0, y = 0;
        reinterpret_cast<SizeFn>(kCanvasTextSize)(canvas, font, &x, &y, "%s", line.first.c_str());
        width = std::max(width, x);
        if (y > 0) lineHeight = lineHeight ? std::min(lineHeight, y) : y;
    }
    if (lineHeight <= 0) lineHeight = 12;
    const int left = 6, top = 6;
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

uint32_t ColourOf(const Reach::Patch& patch) { return patch.teams == 3 ? kBoth : patch.teams == 1 ? kSpies : kMercs; }

void DrawChecked(Address viewport, Address node)
{
    if (!state.shown || !IsLevelViewport(viewport)) return;
    const int width = Read<int>(viewport + 0xa0), height = Read<int>(viewport + 0xa4);
    if (width <= 0 || height <= 0) return;
    const auto worldToScreen = Read<Measure::Matrix>(node + kWorldToScreen);
    const auto camera = Read<Address>(viewport + 0x30);
    const int hidden = Measure::DepthAxis(Read<int>(camera + 0x4fc));
    const auto location = Read<std::array<float, 3>>(camera + 0x80);
    unsigned generation = 0, revision = 0;
    const bool current = CurrentMap(generation, revision);
    const bool forThisMap = current && (state.run ? generation == state.run->generation : generation == state.generation);
    // Front and side views would show the cells edge-on: legend only.
    if (forThisMap && !state.patches.empty() && hidden != 0 && hidden != 1)
    {
        const auto ri = Read<Address>(viewport + 0x164);
        alignas(16) unsigned char batcher[0x40] = {};
        reinterpret_cast<void*(__thiscall*)(void*, Address, int)>(kLineBatcher)(batcher, ri, hidden < 0 ? 1 : 0);
        const double size = state.run && state.run->floods[0] ? state.run->floods[0]->GetSettings().cell
                          : state.run && state.run->floods[1] ? state.run->floods[1]->GetSettings().cell : state.cell;
        for (const auto& patch : state.patches)
        {
            if (hidden < 0)
            {
                const double dx = patch.at[0] - location[0], dy = patch.at[1] - location[1], dz = patch.at[2] - location[2];
                if (dx * dx + dy * dy + dz * dz > kViewDistance * kViewDistance) continue;
            }
            const auto p = Measure::Transform(worldToScreen, {patch.at[0], patch.at[1], patch.at[2], 1});
            if (!(p[3] > 1e-6)) continue;
            if (std::abs(p[0] / p[3]) > 1.25 || std::abs(p[1] / p[3]) > 1.25) continue;
            const uint32_t colour = ColourOf(patch);
            for (const auto& [a, b] : Reach::Outline(patch, size))
                reinterpret_cast<void(__thiscall*)(void*, FVec, FVec, uint32_t)>(kLineBatcherDraw)(batcher, ToF(a), ToF(b), colour);
        }
        reinterpret_cast<void(__thiscall*)(void*)>(kLineBatcherEnd)(batcher);
    }
    std::vector<std::pair<std::string, uint32_t>> text;
    if (state.run)
    {
        text.push_back({Progress() + ". Build > Reachability Map again cancels.", kTextColour});
        text.push_back({std::string(Reach::kNotModelled) + ".", kTextColour});
    }
    else if (!state.done || !forThisMap)
        text.push_back({"Reachability: " + (state.status.empty() ? std::string("not worked out") : state.status), kTextColour});
    else
    {
        const auto lines = Reach::Legend(state.summary, state.cell, revision != state.revision);
        const uint32_t colours[] = {kTextColour, kBoth, kSpies, kMercs};
        for (size_t i = 0; i < lines.size(); ++i) text.push_back({lines[i], i < 4 ? colours[i] : kTextColour});
    }
    DrawText(viewport, text);
}
void DrawGuarded(Address viewport, Address node)
{
    try { DrawChecked(viewport, node); }
    catch (const std::exception& e)
    {
        if (!state.overlayFaulted) Logger::log(std::string("Reachability: overlay skipped: ") + e.what());
        state.overlayFaulted = true;
    }
}
void LogFault() { Logger::log("Reachability: overlay faulted and was skipped"); }

void Show(bool on)
{
    Load();
    state.shown = on;
    if (on && !Fresh() && !state.run && !TryStart()) MessageBeep(MB_ICONWARNING);
    if (!on && state.run) Abandon("Cancelled.");
    UpdateMenu();
    Redraw();
}
} // namespace

bool HandleCommand(UINT command)
{
    if (command != kToggle) return false;
    try { Show(!state.shown); }
    catch (const std::exception& e)
    {
        state.status = e.what();
        Logger::log(std::string("Reachability: ") + e.what());
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

std::vector<Reach::Finding> MapCheckRows()
{
    Load();
    std::vector<Reach::Finding> rows;
    const char* check = "Reachability";
    if (Fresh())
    {
        rows = state.findings;
        std::string worked;
        for (int team : {Reach::kSpy, Reach::kMerc})
            if (state.worked[team]) worked += (worked.empty() ? "" : ", ") + std::string(Reach::TeamName(team)) + " " + Reach::Number(static_cast<double>(state.cells[team])) + " cells";
        rows.push_back({2, check, "Worked out on foot from the player starts on a " + Reach::Number(state.cell) + "-unit grid (" + worked + "). "
                        + Reach::kNotModelled + ". Build > Reachability Map shows it in the viewports."});
        return rows;
    }
    if (!state.run && state.options.runWithMapCheck && !StoppedHere()) TryStart();
    if (state.run)
    {
        state.notifyMapCheck = true;
        rows.push_back({2, check, "Where each team can walk is being worked out (" + Counts()
                        + " so far); Map Check refreshes with the objectives and pickups out of reach when it finishes. Build > Reachability Map shows it, and cancels it.", {}});
        return rows;
    }
    rows.push_back({2, check, state.status.empty() ? std::string("Not worked out: Build > Reachability Map works out where each team can walk (RunWithMapCheck in Reloaded_Editor.ini [Reachability] does it here).")
                                                   : "Not worked out: " + state.status + " Build > Reachability Map tries again."});
    return rows;
}

const std::vector<Reach::Patch>& Patches()
{
    static const std::vector<Reach::Patch> none;
    unsigned generation = 0, revision = 0;
    if (!CurrentMap(generation, revision)) return none;
    if (state.run) return generation == state.run->generation ? state.patches : none;
    return state.done && generation == state.generation ? state.patches : none;
}
double CellSize()
{
    if (state.run && state.run->floods[0]) return state.run->floods[0]->GetSettings().cell;
    if (state.run && state.run->floods[1]) return state.run->floods[1]->GetSettings().cell;
    return state.done ? state.cell : (Load(), state.options.settings.cell);
}
void SetPlanLayer(bool on)
{
    Load();
    state.planLayer = on;
    if (on && !Fresh() && !state.run && !StoppedHere()) TryStart();
}
unsigned Version() { return state.version; }
std::string Status()
{
    if (state.run) return Progress() + ". RE+ Build > Reachability Map cancels it.";
    unsigned generation = 0, revision = 0;
    if (!state.done || !CurrentMap(generation, revision) || generation != state.generation)
        return "Reachability map: " + (state.status.empty() ? std::string("not worked out.") : state.status);
    const auto& s = state.summary;
    return "Reachability map: green both teams " + Reach::Number(static_cast<double>(s.both)) + " cells, blue spies only " + Reach::Number(static_cast<double>(s.spies))
        + ", red mercs only " + Reach::Number(static_cast<double>(s.mercs)) + (revision != state.revision ? " (the map changed since)" : "") + ". " + Reach::kNotModelled + ".";
}
}
