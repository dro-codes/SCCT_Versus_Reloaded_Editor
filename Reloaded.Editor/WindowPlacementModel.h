#pragma once
#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <vector>

// Where an RE+ window was when it was last closed, so it opens there again:
// [WindowPlacement] in Reloaded_Editor.ini, one key per window, as
// "x,y,width,height,maximised" in screen pixels (the size and place it has
// when not maximised). A place no longer on any monitor (one unplugged, or
// a resolution change) is not used: the window opens where Windows puts it.
// A window too large for the monitor it opens on is made to fit it.
namespace WindowPlacement
{
    struct Rect
    {
        int left = 0, top = 0, right = 0, bottom = 0;
    };

    struct Placement
    {
        int x = 0, y = 0, width = 0, height = 0;
        bool maximised = false;
    };

    // Sizes a window may come back at. Larger than any desktop is a damaged
    // entry, smaller than this leaves nothing to work with.
    constexpr int kMinWidth = 160, kMinHeight = 100, kMaxSize = 16384;
    // What must be on a monitor to count as there: a strip of the title bar,
    // enough to see and drag. The strip starts below the top edge, which a
    // maximised-then-restored window may leave a few pixels above a monitor.
    constexpr int kTitleInset = 4, kTitleHeight = 24, kGripWidth = 64, kGripHeight = 16;

    inline std::string Format(const Placement& p)
    {
        return std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.width) + "," + std::to_string(p.height) + ","
             + (p.maximised ? "1" : "0");
    }

    // Exactly five comma-separated integers, the last 0 or 1, the size
    // positive; blanks around a number are allowed. Anything else is no
    // placement at all.
    inline std::optional<Placement> Parse(const std::string& text)
    {
        int values[5] = {};
        size_t at = 0;
        for (int i = 0; i < 5; ++i)
        {
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
            const char* first = text.data() + at;
            const char* last = text.data() + text.size();
            auto [end, error] = std::from_chars(first, last, values[i]);
            if (error != std::errc() || end == first) return std::nullopt;
            at = static_cast<size_t>(end - text.data());
            while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
            if (i < 4)
            {
                if (at >= text.size() || text[at] != ',') return std::nullopt;
                ++at;
            }
        }
        if (at != text.size()) return std::nullopt;
        if (values[2] <= 0 || values[3] <= 0 || (values[4] != 0 && values[4] != 1)) return std::nullopt;
        return Placement{values[0], values[1], values[2], values[3], values[4] == 1};
    }

    // The size kept within reach: no smaller than a usable window (or the
    // window's own minimum, when larger), no larger than kMaxSize.
    inline Placement Clamp(Placement p, int minWidth = kMinWidth, int minHeight = kMinHeight)
    {
        minWidth = std::clamp(minWidth, 1, kMaxSize);
        minHeight = std::clamp(minHeight, 1, kMaxSize);
        p.width = std::clamp(p.width, minWidth, kMaxSize);
        p.height = std::clamp(p.height, minHeight, kMaxSize);
        return p;
    }

    inline int Overlap(int low, int high, int otherLow, int otherHigh)
    {
        return std::max(0, std::min(high, otherHigh) - std::max(low, otherLow));
    }

    // The monitor (work area) with enough of the title bar on it to see and
    // drag the window: kGripWidth across, kGripHeight down, on a single
    // monitor. None when no monitor has.
    inline const Rect* TitleMonitor(const Placement& p, const std::vector<Rect>& monitors)
    {
        if (p.width <= 0 || p.height <= 0) return nullptr;
        const long long right = static_cast<long long>(p.x) + p.width;
        if (right > 1000000000LL || p.x < -1000000000 || p.y < -1000000000 || p.y > 1000000000) return nullptr;
        const int left = p.x + kTitleInset, top = p.y + kTitleInset;
        const int stripRight = static_cast<int>(right) - kTitleInset, stripBottom = top + kTitleHeight;
        for (const auto& m : monitors)
            if (Overlap(left, stripRight, m.left, m.right) >= std::min(kGripWidth, stripRight - left)
                && Overlap(top, stripBottom, m.top, m.bottom) >= kGripHeight)
                return &m;
        return nullptr;
    }

    // Whether enough of the title bar is on one of these monitors to see and
    // drag the window.
    inline bool Usable(const Placement& p, const std::vector<Rect>& monitors)
    {
        return TitleMonitor(p, monitors) != nullptr;
    }

    // How far inside a monitor the window's far corner must be to count as
    // on it: its resize border, and a few pixels over the taskbar, do not
    // count as off the screen.
    constexpr int kCornerInset = 24;

    // A window whose far corner is off every monitor (the resolution dropped,
    // or it opens on a smaller screen) is made no larger than the monitor its
    // title bar is on, though never below minWidth x minHeight, and moved
    // inside it, so its bottom and right edges can be reached. One reaching
    // onto a second monitor is left as it is.
    inline Placement Fit(Placement p, const std::vector<Rect>& monitors, int minWidth = kMinWidth, int minHeight = kMinHeight)
    {
        const Rect* m = TitleMonitor(p, monitors);
        if (!m) return p;
        const long long cornerX = static_cast<long long>(p.x) + p.width - 1 - kCornerInset;
        const long long cornerY = static_cast<long long>(p.y) + p.height - 1 - kCornerInset;
        for (const auto& other : monitors)
            if (cornerX >= other.left && cornerX < other.right && cornerY >= other.top && cornerY < other.bottom) return p;
        const int areaWidth = m->right - m->left, areaHeight = m->bottom - m->top;
        p.width = std::min(p.width, std::max(areaWidth, minWidth));
        p.height = std::min(p.height, std::max(areaHeight, minHeight));
        p.x = std::clamp(p.x, m->left, std::max(m->left, m->right - p.width));
        p.y = std::clamp(p.y, m->top, std::max(m->top, m->bottom - p.height));
        return p;
    }
}
