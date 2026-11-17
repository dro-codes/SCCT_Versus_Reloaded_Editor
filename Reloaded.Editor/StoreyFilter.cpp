#include "pch.h"
#undef min
#undef max
#include "StoreyFilter.h"
#include "WindowPlacement.h"
#include "Appearance.h"
#include "WorkflowEditor.h"
#include "MapDesignModel.h"
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>
#pragma comment(lib,"gdiplus.lib")

// One storey of the map at a time in the editor's own viewports. The storeys
// are the same ones the Map Design plan offers, read the same way from the
// brushes; what differs is how a floor is shown: the plan draws only that
// floor, while here everything standing on the others is hidden, which is the
// only thing the native viewports understand. What this hides it puts back,
// and nothing else: an actor hidden by the Scene panel or Brush Visibility
// stays hidden when the filter is switched off.
namespace StoreyFilter
{
using namespace Workflow;
namespace
{
    HWND window = nullptr;
    ULONG_PTR gdiplus = 0;
    // The storeys, lowest first, and which detent is showing: 0 for all of
    // them, else counting down from the highest, as the plan's slider does.
    std::vector<Design::Storey> storeys;
    int current = 0;
    // The actors this filter hid, so switching off shows those and no others.
    std::set<std::string> ours;
    unsigned generation = 0;
    unsigned revision = ~0u;
    std::string status = "Every storey is showing.";
    constexpr UINT kTimer = 1;

    struct Layout { float x=0,top=0,step=0; int count=0; };

    Layout LayoutFor(const RECT& rect)
    {
        Layout layout;
        layout.count = static_cast<int>(storeys.size());
        layout.x = static_cast<float>(rect.right) - 34;
        layout.top = 46;
        const float room = static_cast<float>(std::max(60L, rect.bottom - 96));
        layout.step = std::clamp(room / static_cast<float>(layout.count + 1), 16.f, 30.f);
        return layout;
    }

    // The storeys of the open map, from the surfaces its brushes leave to
    // stand on. A map built with the toolkit and one built anywhere else are
    // read the same way here: the plan's pieces are a workspace record, and
    // the viewports know nothing about them.
    void Recompute(const Json& spans)
    {
        std::vector<Design::BrushBox> brushes;
        Vector low{1e18,1e18,1e18}, high{-1e18,-1e18,-1e18};
        for (const auto& actor : spans)
        {
            const int csg = actor.value("csg", 0);
            if ((csg != 1 && csg != 2) || !actor.value("box", false) || actor.value("portal", false)) continue;
            Design::BrushBox brush;
            brush.carve = csg == 2;
            brush.low = actor.at("low").get<Vector>();
            brush.high = actor.at("high").get<Vector>();
            for (int axis = 0; axis < 3; ++axis)
            {
                low[axis] = std::min(low[axis], brush.low[axis]);
                high[axis] = std::max(high[axis], brush.high[axis]);
            }
            brushes.push_back(brush);
        }
        std::vector<Design::FloorEvidence> evidence;
        for (const auto& brush : brushes)
        {
            if (Design::EnclosesMap(brush, low, high)) continue;
            if (Design::FloorEvidence surface; Design::FloorSurface(brush, surface)) evidence.push_back(surface);
        }
        Design::StoreyRules rules;
        rules.share = .06;
        rules.limit = 24;
        storeys = Design::Storeys(evidence, rules);
    }

    // The height band a detent covers: from just under its floor to just under
    // the next one, so what stands on the floor counts and the storey above
    // does not.
    void Band(int index, double& lower, double& upper)
    {
        const int count = static_cast<int>(storeys.size());
        const auto& storey = storeys[count - index];
        lower = storey.base - .5;
        upper = (count - index + 1 < count ? storeys[count - index + 1].base : storey.top + 16) - .5;
    }

    void Refresh(bool force);

    // Where everything stands, read once per change to the map: dragging the
    // slider asks for this at every detent, and walking a map's brushes is
    // not work to repeat while the mouse moves. A click that only selects is
    // not a change. Our own hiding opens no transaction, so neither counter
    // moves for it; the cache is corrected by hand after it instead.
    Json spans = Json::array();
    unsigned spansRevision = ~0u;

    Json& Spans(bool reread = false)
    {
        unsigned now = 0;
        try { now = Editor::ContentRevision(); }
        catch (const std::exception&) { return spans; }
        if (reread || now != spansRevision)
        {
            spans = Editor::DesignActorSpans();
            spansRevision = now;
        }
        return spans;
    }

    // Hides what the chosen storey leaves out and shows what it brings back.
    // This is a view, not an edit, so it takes no Undo step: re-applying it
    // after an Undo must not drop the step that was just undone.
    void Apply(int index)
    {
        const int count = static_cast<int>(storeys.size());
        index = std::clamp(index, 0, count);
        Json& actors = Spans();
        Json hide = Json::array(), show = Json::array();
        std::set<std::string> hiding, showing;
        double lower = 0, upper = 0;
        if (index > 0) Band(index, lower, upper);
        size_t shown = 0;
        for (const auto& actor : actors)
        {
            const auto path = actor.at("path").get<std::string>();
            const bool mine = ours.count(path) != 0;
            const auto low = actor.at("low").get<Vector>(), high = actor.at("high").get<Vector>();
            const bool wanted = index == 0 || Design::WithinFloor(lower, upper, low[2], high[2]);
            if (wanted)
            {
                ++shown;
                if (mine) { show.push_back(Json{{"path", actor.at("path")}, {"class", actor.at("class")}}); showing.insert(path); }
                continue;
            }
            // Something already hidden by hand stays hidden and stays theirs.
            if (!mine && actor.value("hidden", false)) continue;
            hide.push_back(Json{{"path", actor.at("path")}, {"class", actor.at("class")}});
            hiding.insert(path);
        }
        Editor::DesignSetHidden(hide, show);
        for (const auto& path : showing) ours.erase(path);
        for (const auto& path : hiding) ours.insert(path);
        // The boxes are still good; only what is hidden changed, and by us, so
        // the cache is corrected rather than read again.
        for (auto& actor : actors)
        {
            const auto path = actor.at("path").get<std::string>();
            if (hiding.count(path)) actor["hidden"] = true;
            else if (showing.count(path)) actor["hidden"] = false;
        }
        current = index;
        revision = Editor::ContentRevision();
        spansRevision = revision;
        if (index == 0)
            status = "Every storey is showing" + std::string(ours.empty() ? "." : "; " + std::to_string(ours.size()) + " still hidden.");
        else
            status = "Floor at Z " + Design::Round(storeys[count - index].base) + ", " + std::to_string(index)
                   + " of " + std::to_string(count) + " from the top: " + std::to_string(shown) + " actor(s) showing, "
                   + std::to_string(ours.size()) + " hidden.";
        if (window) InvalidateRect(window, nullptr, TRUE);
    }

    // The map or its geometry changed: read the storeys again and, if a floor
    // is showing, apply it to whatever is in the map now.
    void Refresh(bool force)
    {
        unsigned mapGeneration = 0, mapRevision = 0;
        try
        {
            mapGeneration = Editor::MapGeneration();
            mapRevision = Editor::ContentRevision();
        }
        catch (const std::exception&) { return; }
        if (mapGeneration != generation)
        {
            // Another map: nothing here is hidden by us any more.
            generation = mapGeneration;
            ours.clear();
            current = 0;
            force = true;
        }
        else if (!force && mapRevision == revision) return;
        try
        {
            Recompute(Spans(true));
            if (current > static_cast<int>(storeys.size())) current = static_cast<int>(storeys.size());
            Apply(current);
        }
        catch (const std::exception& error) { status = error.what(); }
        if (window) InvalidateRect(window, nullptr, TRUE);
    }

    void Step(int direction)
    {
        const int count = static_cast<int>(storeys.size());
        if (count == 0) { status = "No storeys yet: this map has no brushes to stand on."; return; }
        Apply(std::clamp(current + direction, 0, count));
    }

    void Paint(HDC dc)
    {
        using namespace Gdiplus;
        RECT rect{};
        GetClientRect(window, &rect);
        Bitmap buffer(std::max(1L, rect.right), std::max(1L, rect.bottom));
        Graphics g(&buffer);
        // Modern Dark: the window's dark background and text colours (GDI+ ignores the system's).
        const bool night = Appearance::Dark();
        const auto palette = Appearance::Colors();
        auto rgb = [](uint32_t c) { return Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); };
        const Color background = night ? rgb(palette.window) : Color(255, 255, 255, 255);
        g.Clear(background);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        Font heading(L"Segoe UI", 10.f, FontStyleBold);
        Font body(L"Segoe UI", 8.5f);
        SolidBrush dark(night ? rgb(palette.text) : Color(255, 32, 38, 48)), dim(night ? rgb(palette.grayText) : Color(255, 108, 116, 128)),
            strong(night ? Color(255, 255, 255, 255) : Color(255, 20, 30, 40));
        g.DrawString(L"Storeys", -1, &heading, PointF(12, 10), &dark);
        g.DrawString(L"Only this floor shows in the viewports.", -1, &body, PointF(12, 28), &dim);
        const auto layout = LayoutFor(rect);
        if (layout.count > 0)
        {
            const float bottom = layout.top + layout.step * layout.count;
            const Color line = night ? Color(255, 120, 126, 136) : Color(150, 90, 100, 115);
            Pen track(line, 3), tick(night ? line : Color(170, 90, 100, 115), 1);
            g.DrawLine(&track, layout.x, layout.top, layout.x, bottom);
            StringFormat rightAligned;
            rightAligned.SetAlignment(StringAlignmentFar);
            rightAligned.SetLineAlignment(StringAlignmentCenter);
            for (int i = 0; i <= layout.count; ++i)
            {
                const float y = layout.top + layout.step * i;
                g.DrawLine(&tick, layout.x - 5, y, layout.x + 5, y);
                if (layout.step < 18 && i != 0 && i != current) continue;
                const std::string text = i == 0 ? "All" : "Z " + Design::Round(storeys[layout.count - i].base);
                const std::wstring wide(text.begin(), text.end());
                g.DrawString(wide.c_str(), -1, &body, RectF(12, y - 9, layout.x - 24, 18), &rightAligned,
                             i == current ? &strong : &dim);
            }
            const float y = layout.top + layout.step * current;
            Pen outline(background, 2);
            SolidBrush handle(current ? Color(235, 0, 120, 200) : Color(235, 90, 100, 115));
            g.DrawEllipse(&outline, layout.x - 7, y - 7, 14.f, 14.f);
            g.FillEllipse(&handle, layout.x - 6, y - 6, 12.f, 12.f);
        }
        else g.DrawString(L"No storeys in this map.", -1, &body, PointF(12, 52), &dim);
        const std::wstring line(status.begin(), status.end());
        g.DrawString(line.c_str(), -1, &body, RectF(12, static_cast<float>(rect.bottom - 44),
                     static_cast<float>(std::max(20L, rect.right - 24)), 40.f), nullptr, &dim);
        Graphics target(dc);
        target.DrawImage(&buffer, 0, 0);
    }

    int DetentAt(int y)
    {
        RECT rect{};
        GetClientRect(window, &rect);
        const auto layout = LayoutFor(rect);
        if (layout.count == 0) return 0;
        return std::clamp(static_cast<int>(std::lround((y - layout.top) / layout.step)), 0, layout.count);
    }

    LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
    {
        try
        {
            if (message == WM_ERASEBKGND) return 1;
            if (message == WM_PAINT)
            {
                PAINTSTRUCT ps{};
                auto dc = BeginPaint(hwnd, &ps);
                Paint(dc);
                EndPaint(hwnd, &ps);
                return 0;
            }
            if (message == WM_TIMER && w == kTimer) { Refresh(false); return 0; }
            if (message == WM_SIZE) { InvalidateRect(hwnd, nullptr, TRUE); return 0; }
            if (message == WM_LBUTTONDOWN)
            {
                SetCapture(hwnd);
                Apply(DetentAt(GET_Y_LPARAM(l)));
                return 0;
            }
            if (message == WM_MOUSEMOVE && GetCapture() == hwnd && (w & MK_LBUTTON))
            {
                const int detent = DetentAt(GET_Y_LPARAM(l));
                if (detent != current) Apply(detent);
                return 0;
            }
            if (message == WM_LBUTTONUP && GetCapture() == hwnd) { ReleaseCapture(); return 0; }
            if (message == WM_MOUSEWHEEL) { Step(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1); return 0; }
            if (message == WM_KEYDOWN)
            {
                if (w == VK_PRIOR) { Step(-1); return 0; }
                if (w == VK_NEXT) { Step(1); return 0; }
                if (w == VK_HOME) { Apply(0); return 0; }
            }
            if (message == WM_CLOSE)
            {
                // Closing the palette puts the map back the way it was.
                Apply(0);
                DestroyWindow(hwnd);
                return 0;
            }
            if (message == WM_NCDESTROY)
            {
                WindowPlacement::Remember(hwnd, "Storeys");
                window = nullptr;
                return DefWindowProcA(hwnd, message, w, l);
            }
        }
        catch (const std::exception& error)
        {
            status = error.what();
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return DefWindowProcA(hwnd, message, w, l);
    }
}

void Open()
{
    if (window)
    {
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window);
        Refresh(true);
        return;
    }
    if (!gdiplus)
    {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&gdiplus, &input, nullptr) != Gdiplus::Ok)
            throw std::runtime_error("GDI+ is not available.");
    }
    WNDCLASSA wc{};
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpfnWndProc = Proc;
    wc.lpszClassName = "ReloadedStoreyFilter";
    RegisterClassA(&wc);
    // A slim palette, kept above the editor so it can sit against a viewport.
    // Owned by the frame: whatever was active (Map Design, say) would take the
    // palette with it when it closed.
    HWND owner = Editor::MainWindow();
    window = CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, "Storeys",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 230, 520,
                             owner ? owner : GetActiveWindow(), nullptr, wc.hInstance, nullptr);
    if (!window) throw std::runtime_error("Cannot open the storey palette.");
    WindowPlacement::Restore(window, "Storeys");
    SetTimer(window, kTimer, 1500, nullptr);
    generation = 0;
    Refresh(true);
}

bool HandleCommand(UINT command)
{
    if (command != kOpen) return false;
    Open();
    return true;
}

bool ViewportKey(WPARAM key)
{
    if (!window) return false;
    if (key == VK_PRIOR) { Step(-1); return true; }
    if (key == VK_NEXT) { Step(1); return true; }
    if (key == VK_HOME) { Apply(0); return true; }
    return false;
}
}
