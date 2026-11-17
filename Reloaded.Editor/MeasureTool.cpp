#include "pch.h"
#undef min
#undef max
#include "MeasureTool.h"
#include "MeasureModel.h"
#include "LightShadowMap.h"
#include "Reachability.h"
#include "WorkflowEditor.h"
#include "GridSizeShortcutState.h"
#include "MemoryWriter.h"
#include "logger.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

namespace MeasureTool
{
namespace
{
    using Address = uintptr_t;
    using Workflow::Vector;
    namespace Measure = Workflow::Measure;

    constexpr Address kGEditor = 0x1165DFA0, kUnrealEd = 0x117a59b0;
    constexpr Address kLevelViewportConfigs = 0x1165e8d4, kLevelViewportCount = 0x1165e8d8;
    // UUnrealEdEngine::Draw, after the level, the pivot and the axis indicator
    // and before the UseSizingBox / selection overlays: mov edx,[GUnrealEd].
    // Its camera scene node is at ebp-0x71c and the viewport at [ebp+8] here.
    constexpr Address kOverlayAt = 0x10eccef1, kOverlayResume = 0x10eccef7;
    constexpr unsigned char kOverlayBytes[] = { 0x8b, 0x15, 0xb0, 0x59, 0x7a, 0x11 };
    // FSceneNode: WorldToScreen (Project, 0x1109f320) and ScreenToWorld (Deproject, 0x1109f390).
    constexpr Address kWorldToScreen = 0x114, kScreenToWorld = 0x154;
    // FLineBatcher(RI, ZTest), DrawLine(FVector, FVector, FColor), ~FLineBatcher (flushes),
    // as the editor's own path and link lines use them (0x10ece02b).
    constexpr Address kLineBatcher = 0x11176c80, kLineBatcherDraw = 0x11177480, kLineBatcherEnd = 0x11179ae0;
    // UCanvas: SetClip(X, Y, XL, YL), WrappedPrintf(Font, Center, Fmt, ...),
    // WrappedStrLenf(Font, &XL, &YL, Fmt, ...), as the UseSizingBox overlay uses them.
    constexpr Address kCanvasSetClip = 0x110b3860, kCanvasPrint = 0x110b6a30, kCanvasTextSize = 0x110b6970;

    // FColor is B,G,R,A in memory.
    constexpr uint32_t kLineColour = 0xFFFFE000, kLegColour = 0xFF8C7A30, kStartColour = 0xFF40FF40, kEndColour = 0xFFFF5050;
    constexpr uint32_t kTextColour = 0xFFFFE000, kShadowColour = 0xFF000000;

    struct ViewState
    {
        Measure::Matrix worldToScreen{}, screenToWorld{};
        int width = 0, height = 0, hidden = -1;
        Vector camera{};
    };
    std::map<Address, ViewState> views;
    Measure::Session session;
    Address sessionLevel = 0;
    Address clickViewport = 0;
    bool popupSurface = false;
    int popupHidden = -1;
    bool overlayFaulted = false;
    int overlayPauses = 0; // OverlayPause objects alive
    struct Overlay { Address level = 0; std::vector<OverlayLine> lines; };
    std::map<std::string, Overlay> overlays;

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
    Address CurrentLevel()
    {
        Address editor = 0, level = 0;
        if (!Copy(&editor, reinterpret_cast<const void*>(kGEditor), 4) || !editor) return 0;
        if (!Copy(&level, reinterpret_cast<const void*>(editor + 0x130), 4)) return 0;
        return level;
    }
    bool IsLevelViewport(Address viewport)
    {
        void* configs = nullptr; int count = 0;
        if (!Copy(&configs, reinterpret_cast<const void*>(kLevelViewportConfigs), 4)
            || !Copy(&count, reinterpret_cast<const void*>(kLevelViewportCount), 4))
            return false;
        return GridSizeShortcut::Detail::IsLevelViewport(reinterpret_cast<void*>(viewport), configs, count);
    }
    // The axis a level viewport looks along (2D views), from its camera's RendMap.
    int HiddenAxis(Address viewport)
    {
        if (!viewport || !IsLevelViewport(viewport)) return -1;
        const auto camera = Read<Address>(viewport + 0x30);
        return Measure::DepthAxis(Read<int>(camera + 0x4fc));
    }
    void Redraw()
    {
        try { Workflow::Editor::Redraw(); }
        catch (const std::exception&) { /* No map: nothing to draw into. */ }
    }

    // The point the popup was opened on: ClickLocation, snapped like Builder
    // Brush > Place Here (not along a clicked surface's normal, nor along the
    // axis a 2D view cannot see).
    Measure::Point ClickPoint(bool surface, int hidden)
    {
        const auto unrealEd = Read<Address>(kUnrealEd), editor = Read<Address>(kGEditor);
        const auto click = Read<std::array<float, 3>>(unrealEd + 0x1bc);
        const auto plane = Read<std::array<float, 3>>(unrealEd + 0x1d4);
        Vector normal{}; double length = 0;
        for (int axis = 0; axis < 3; ++axis) { normal[axis] = plane[axis]; length += normal[axis] * normal[axis]; }
        length = std::sqrt(length);
        // Backdrop and actor clicks leave a stale plane; only a surface click sets it.
        if (!surface || !std::isfinite(length) || length < 0.5) normal = {};
        else for (double& v : normal) v /= length;
        Measure::Point point{ { click[0], click[1], click[2] }, hidden };
        if (Read<unsigned>(editor + 0x1f8) & 1)
        {
            const auto grid = Read<std::array<float, 3>>(editor + 0x200);
            point.at = Measure::Snap(point.at, { grid[0], grid[1], grid[2] }, normal, hidden);
        }
        Workflow::Design::CheckVector(point.at);
        return point;
    }
    void Report()
    {
        if (!session.Complete()) return;
        const auto lines = Measure::Lines(Measure::Compare(*session.start, *session.end));
        Logger::log("Measure: " + Measure::Text(lines, " | "));
    }
    // A new end for the session, which forgets ends taken in another map.
    template<class F> void Change(F change)
    {
        const auto level = CurrentLevel();
        if (!level) throw std::runtime_error("Open a map first.");
        if (level != sessionLevel) { session.Clear(); sessionLevel = level; }
        change();
        Report();
        Redraw();
    }

    // M: from the last end to the mouse. A 2D view reads the point straight
    // off its plane; a 3D view takes where the mouse ray meets the floor at
    // the last end's height.
    void MeasureToMouse(Address viewport)
    {
        const auto found = views.find(viewport);
        if (found == views.end() || found->second.width <= 0) throw std::runtime_error("The viewport has not been drawn yet.");
        const auto& view = found->second;
        const auto window = Read<Address>(viewport + 0x1b4);
        const HWND hwnd = window ? Read<HWND>(window + 4) : nullptr;
        POINT mouse{};
        if (!hwnd || !GetCursorPos(&mouse) || !ScreenToClient(hwnd, &mouse)) throw std::runtime_error("The mouse is not over the viewport.");
        if (mouse.x < 0 || mouse.y < 0 || mouse.x >= view.width || mouse.y >= view.height) throw std::runtime_error("The mouse is not over the viewport.");
        const auto editor = Read<Address>(kGEditor);
        const bool snap = (Read<unsigned>(editor + 0x1f8) & 1) != 0;
        const auto grid = Read<std::array<float, 3>>(editor + 0x200);
        Measure::Point point;
        if (view.hidden >= 0)
            point = Measure::MouseInPlane(view.screenToWorld, mouse.x + 0.5, mouse.y + 0.5, view.width, view.height, view.hidden);
        else
        {
            const auto level = CurrentLevel();
            if (level != sessionLevel || !session.Last()) throw std::runtime_error("Start with right-click > Measure > Start Here; M in a 3D view measures across the floor of the last point.");
            double floor = session.Last()->at[2];
            if (session.Complete()) floor = Measure::Compare(*session.start, *session.end).to[2];
            point = Measure::MouseOnFloor(view.screenToWorld, view.camera, mouse.x + 0.5, mouse.y + 0.5, view.width, view.height, floor);
        }
        if (snap) point.at = Measure::Snap(point.at, { grid[0], grid[1], grid[2] }, view.hidden >= 0 ? Vector{} : Vector{ 0, 0, 1 }, point.hidden);
        Change([&] { session.Chain(point); });
    }

    struct FVec { float x, y, z; };
    FVec ToF(const Vector& v) { return { static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]) }; }

    // Arm length of an end's cross: about 7 pixels at that point's depth.
    double MarkerSize(const ViewState& view, const Vector& at)
    {
        const auto p = Measure::Transform(view.worldToScreen, { at[0], at[1], at[2], 1 });
        if (!(p[3] > 1e-6)) return 0;
        const auto pixel = Measure::ToPixels(p[0] / p[3], p[1] / p[3], view.width, view.height);
        const double z = p[2] / p[3];
        const auto a = Measure::Deproject(view.screenToWorld, pixel[0], pixel[1], z, view.width, view.height);
        const auto b = Measure::Deproject(view.screenToWorld, pixel[0] + 7, pixel[1], z, view.width, view.height);
        const double size = Workflow::Design::Distance(a, b);
        return std::isfinite(size) && size < 100000 ? size : 0;
    }

    void DrawLines(Address viewport, const ViewState& view, const std::vector<std::pair<std::pair<Vector, Vector>, uint32_t>>& lines)
    {
        const auto ri = Read<Address>(viewport + 0x164);
        alignas(16) unsigned char batcher[0x40] = {};
        // ZTest off: a measurement through a wall stays visible.
        reinterpret_cast<void*(__thiscall*)(void*, Address, int)>(kLineBatcher)(batcher, ri, 0);
        for (const auto& [ends, colour] : lines)
            reinterpret_cast<void(__thiscall*)(void*, FVec, FVec, uint32_t)>(kLineBatcherDraw)(batcher, ToF(ends.first), ToF(ends.second), colour);
        reinterpret_cast<void(__thiscall*)(void*)>(kLineBatcherEnd)(batcher);
    }

    // The readout, in the viewport's top-left corner where it covers neither end.
    void DrawLabel(Address viewport, const ViewState& view, const std::vector<std::string>& text)
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
            reinterpret_cast<SizeFn>(kCanvasTextSize)(canvas, font, &x, &y, "%s", line.c_str());
            width = std::max(width, x); lineHeight = std::max(lineHeight, y);
        }
        if (lineHeight <= 0) lineHeight = 12;
        const int height = lineHeight * static_cast<int>(text.size());
        const int left = 6, top = std::max(2, std::min(6, view.height - height - 4));
        // Origin, clip and pen (0x2c..0x43) and DrawColor (0x50) go back as they were.
        unsigned char saved[0x18] = {}; uint32_t savedColour = 0;
        if (!Copy(saved, reinterpret_cast<const void*>(canvas + 0x2c), sizeof(saved)) || !Copy(&savedColour, reinterpret_cast<const void*>(canvas + 0x50), 4)) return;
        for (int pass = 0; pass < 2; ++pass)
        {
            const uint32_t colour = pass ? kTextColour : kShadowColour;
            Copy(reinterpret_cast<void*>(canvas + 0x50), &colour, 4);
            for (size_t i = 0; i < text.size(); ++i)
            {
                const int shadow = pass ? 0 : 1;
                reinterpret_cast<SetClipFn>(kCanvasSetClip)(canvas, left + shadow, top + shadow + static_cast<int>(i) * lineHeight, width + 8, lineHeight + 2);
                reinterpret_cast<PrintFn>(kCanvasPrint)(canvas, font, 0, "%s", text[i].c_str());
            }
        }
        Copy(reinterpret_cast<void*>(canvas + 0x2c), saved, sizeof(saved));
        Copy(reinterpret_cast<void*>(canvas + 0x50), &savedColour, 4);
    }

    void DrawOverlayChecked(Address viewport, Address node)
    {
        if (!IsLevelViewport(viewport)) return;
        if (views.size() > 32) views.clear();
        auto& view = views[viewport];
        view.worldToScreen = Read<Measure::Matrix>(node + kWorldToScreen);
        view.screenToWorld = Read<Measure::Matrix>(node + kScreenToWorld);
        view.width = Read<int>(viewport + 0xa0);
        view.height = Read<int>(viewport + 0xa4);
        view.hidden = HiddenAxis(viewport);
        const auto camera = Read<Address>(viewport + 0x30);
        const auto location = Read<std::array<float, 3>>(camera + 0x80);
        view.camera = { location[0], location[1], location[2] };
        if (!overlays.empty() && view.width > 0 && view.height > 0)
        {
            const auto level = CurrentLevel();
            std::vector<std::pair<std::pair<Vector, Vector>, uint32_t>> extra;
            for (const auto& [owner, overlay] : overlays)
                if (overlay.level == level)
                    for (const auto& line : overlay.lines)
                        extra.push_back({ { { line.from[0], line.from[1], line.from[2] }, { line.to[0], line.to[1], line.to[2] } }, line.colour });
            if (!extra.empty()) DrawLines(viewport, view, extra);
        }
        if (!session.start || !sessionLevel || CurrentLevel() != sessionLevel || view.width <= 0 || view.height <= 0) return;

        std::vector<std::pair<std::pair<Vector, Vector>, uint32_t>> lines;
        auto cross = [&](const Vector& at, uint32_t colour)
        {
            const double arm = MarkerSize(view, at);
            if (arm <= 0) return;
            for (int axis = 0; axis < 3; ++axis)
            {
                Vector a = at, b = at; a[axis] -= arm; b[axis] += arm;
                lines.push_back({ { a, b }, colour });
            }
        };
        if (!session.Complete())
        {
            cross(session.start->at, kStartColour);
            DrawLines(viewport, view, lines);
            DrawLabel(viewport, view, { "Measuring from " + Measure::Position(session.start->at, session.start->hidden),
                                        "Right-click > Measure > To Here, or M over a viewport" });
            return;
        }
        const auto result = Measure::Compare(*session.start, *session.end);
        const auto& from = result.from; const auto& to = result.to;
        // The axis legs, X then Y then Z, when the line is not along one axis.
        int moving = 0; for (double d : result.delta) moving += std::abs(d) > 0.01;
        if (moving > 1)
        {
            const Vector cornerX{ to[0], from[1], from[2] }, cornerY{ to[0], to[1], from[2] };
            lines.push_back({ { from, cornerX }, kLegColour });
            lines.push_back({ { cornerX, cornerY }, kLegColour });
            lines.push_back({ { cornerY, to }, kLegColour });
        }
        lines.push_back({ { from, to }, kLineColour });
        cross(from, kStartColour);
        cross(to, kEndColour);
        DrawLines(viewport, view, lines);
        auto text = Measure::Lines(result);
        text.push_back(Measure::Ends(result));
        DrawLabel(viewport, view, text);
    }
    void DrawOverlayGuarded(Address viewport, Address node)
    {
        try { DrawOverlayChecked(viewport, node); }
        catch (const std::exception& e)
        {
            if (!overlayFaulted) Logger::log(std::string("Measure: overlay skipped: ") + e.what());
            overlayFaulted = true;
        }
    }
    void LogFault() { Logger::log("Measure: overlay faulted and was skipped"); }
    void __cdecl DrawOverlay(Address viewport, Address node)
    {
        // A picture is being captured (OverlayPause): the map alone.
        if (overlayPauses > 0) return;
        // The Light and Shadow map shares this hook, under the measurement.
        LightShadowMap::DrawViewportOverlay(viewport, node);
        Reachability::DrawViewportOverlay(viewport, node);
        __try { DrawOverlayGuarded(viewport, node); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!overlayFaulted) LogFault();
            overlayFaulted = true;
        }
    }
    __declspec(naked) void OverlayHook()
    {
        static Address resume = kOverlayResume;
        __asm
        {
            pushfd
            pushad
            lea eax, [ebp - 0x71c]
            push eax
            push dword ptr [ebp + 8]
            call DrawOverlay
            add esp, 8
            popad
            popfd
            // Replay the displaced instruction: mov edx,[GUnrealEd].
            mov edx, dword ptr ds:[0x117a59b0]
            jmp dword ptr [resume]
        }
    }
}

void Initialize()
{
    if (std::memcmp(reinterpret_cast<const void*>(kOverlayAt), kOverlayBytes, sizeof(kOverlayBytes)) != 0)
    {
        Logger::log("Measure: unexpected viewport draw code; the overlay is off");
        return;
    }
    if (MemoryWriter::WriteJump(kOverlayAt, OverlayHook))
        Logger::log("Measure: viewport overlay installed");
}

void SetOverlay(const std::string& owner, const std::vector<OverlayLine>& lines)
{
    if (lines.empty()) overlays.erase(owner);
    else overlays[owner] = { CurrentLevel(), lines };
    Redraw();
}

void AddMenu(HMENU menu, bool surface)
{
    if (!menu) return;
    popupSurface = surface;
    try { popupHidden = HiddenAxis(clickViewport); }
    catch (const std::exception&) { popupHidden = -1; }
    auto measure = CreatePopupMenu();
    if (!measure) return;
    AppendMenuA(measure, MF_STRING, kStart, "&Start Here");
    AppendMenuA(measure, MF_STRING, kTo, "&To Here");
    AppendMenuA(measure, MF_SEPARATOR, 0, nullptr);
    const bool any = session.start && sessionLevel == CurrentLevel();
    AppendMenuA(measure, MF_STRING | (any ? 0 : MF_GRAYED), kClear, "&Clear Measurement");
    AppendMenuA(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(measure), "&Measure");
}

bool HandleCommand(UINT command)
{
    if (command != kStart && command != kTo && command != kClear) return false;
    try
    {
        if (command == kClear) { session.Clear(); Redraw(); return true; }
        const auto point = ClickPoint(popupSurface, popupHidden);
        Change([&] { if (command == kStart) session.Start(point); else session.To(point); });
    }
    catch (const std::exception& e) { MessageBoxA(GetActiveWindow(), e.what(), "Measure", MB_OK | MB_ICONINFORMATION); }
    return true;
}

void SetToolLines(std::vector<ToolLine> lines)
{
    std::vector<OverlayLine> converted;
    converted.reserve(lines.size());
    for (const auto& line : lines)
        converted.push_back({ { line.from[0], line.from[1], line.from[2] }, { line.to[0], line.to[1], line.to[2] }, line.colour });
    if (converted.empty()) overlays.erase("placement");
    else overlays["placement"] = { CurrentLevel(), std::move(converted) };
}

OverlayPause::OverlayPause() { ++overlayPauses; }
OverlayPause::~OverlayPause()
{
    if (--overlayPauses == 0) Redraw();
}

bool ViewportMessage(void* viewport, UINT message, WPARAM wParam, LPARAM lParam)
{
    const auto target = reinterpret_cast<Address>(viewport);
    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONUP)
    {
        if (IsLevelViewport(target)) clickViewport = target;
        return false;
    }
    if (message != WM_KEYDOWN || wParam != 'M' || (lParam & (1 << 30))) return false;
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_MENU) < 0 || !IsLevelViewport(target)) return false;
    try { MeasureToMouse(target); }
    catch (const std::exception& e)
    {
        Logger::log(std::string("Measure: M ignored: ") + e.what());
        MessageBeep(MB_ICONWARNING);
    }
    return true;
}
}
