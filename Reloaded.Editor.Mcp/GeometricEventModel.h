#pragma once
// Geometric events (GEs): the edges of a map's geometry where the spies grab a
// ledge, go hand over hand, climb a pipe or a ladder, slide down a zip line or
// jump a fence. The editor's own GE ADD command makes one from its points
// (ElementPanel.inl); this is what those points mean and the wiki's
// measurements for them (Geometric-Event-(GE), scct://wiki/geometric-events).
// Engine-free: the floor under a point comes from the caller, either traced in
// the live map or read off the Map Design pieces. Tests:
// tests/GeometricEventModelTests.cpp.
#include "../Reloaded.Editor/WorkflowModel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Mcp::Ge
{
    using Json = Workflow::Json;
    using Vector = Workflow::Vector;

    // The kinds, as the Map Design element tool lists them: its name, the GE
    // ADD command, how many points it takes, what the third point is, and the
    // level spec's name for it (scct.level-spec ges[].kind).
    struct Kind { const char* name; const char* command; int points; const char* third; const char* spec; };
    inline const std::vector<Kind>& Kinds()
    {
        static const std::vector<Kind> kinds = {
            {"Zip line", "ZL", 2, nullptr, "zipline"},
            {"Hand over hand", "HOH", 2, nullptr, "hoh"},
            {"Pipe", "PIPE", 3, "the side the player climbs from", "pipe"},
            {"Ladder", "LADDER", 3, "the side the player climbs from", "ladder"},
            {"Climbable fence", "FENCE", 2, nullptr, "fence"},
            {"Pole", "POLE", 2, nullptr, "pole"},
            {"Ledge grab", "LG", 2, nullptr, "ledge"}};
        return kinds;
    }
    // Letters and digits only, lower case, so "Zip line", "zip-line" and
    // "ZIPLINE" read the same.
    inline std::string Squash(const std::string& text)
    {
        std::string out;
        for (unsigned char c : text) if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
        return out;
    }
    // A kind by its panel name, its command or its spec name, ignoring case
    // and spacing; a few other spellings agents use are accepted too.
    inline const Kind* Find(const std::string& text)
    {
        const auto key = Squash(text);
        if (key.empty()) return nullptr;
        for (const auto& kind : Kinds())
            if (key == Squash(kind.name) || key == Squash(kind.command) || key == Squash(kind.spec)) return &kind;
        static const std::vector<std::pair<const char*, const char*>> aliases = {
            {"ledge", "LG"}, {"ledgegrab", "LG"}, {"grab", "LG"}, {"handoverhand", "HOH"}, {"hand", "HOH"}, {"monkeybars", "HOH"},
            {"zip", "ZL"}, {"ziplines", "ZL"}, {"cable", "ZL"}, {"climbablefence", "FENCE"}, {"fencejump", "FENCE"}};
        for (const auto& [alias, command] : aliases)
            if (key == alias) for (const auto& kind : Kinds()) if (Squash(kind.command) == Squash(command)) return &kind;
        return nullptr;
    }
    inline const Kind& Require(const std::string& text)
    {
        if (const auto* kind = Find(text)) return *kind;
        throw std::runtime_error("\"" + text + "\" is not a geometric event. Use ledge, hoh, pipe, ladder, zipline, fence or pole (or the GE ADD command: LG, HOH, PIPE, LADDER, ZL, FENCE, POLE).");
    }

    // A point as [x, y, z] or {X, Y, Z} (any case), in Unreal units.
    inline Vector Point(const Json& value, const std::string& what)
    {
        Vector out{};
        if (value.is_array() && value.size() == 3)
        {
            for (size_t i = 0; i < 3; ++i)
            {
                if (!value[i].is_number()) throw std::runtime_error(what + " must be [x, y, z] in Unreal units.");
                out[i] = value[i].get<double>();
            }
        }
        else if (value.is_object())
        {
            const char* names[3][2] = {{"X", "x"}, {"Y", "y"}, {"Z", "z"}};
            for (size_t i = 0; i < 3; ++i)
            {
                const Json* found = nullptr;
                for (const char* name : names[i]) if (value.contains(name)) found = &value.at(name);
                if (!found) throw std::runtime_error(what + " must be [x, y, z] in Unreal units.");
                if (found->is_number()) out[i] = found->get<double>();
                else if (found->is_string())
                {
                    try { out[i] = std::stod(found->get<std::string>()); }
                    catch (const std::exception&) { throw std::runtime_error(what + " must be [x, y, z] in Unreal units."); }
                }
                else throw std::runtime_error(what + " must be [x, y, z] in Unreal units.");
            }
        }
        else throw std::runtime_error(what + " must be [x, y, z] in Unreal units.");
        for (double n : out)
            if (!std::isfinite(n) || std::abs(n) > 1000000) throw std::runtime_error(what + " is outside the world.");
        return out;
    }

    // The order the game needs. A pipe or ladder starts at its lower end, and
    // its side point (where the player climbs from) sits at that end's height,
    // as in the stock maps: the game ignores one whose side point is at
    // mid-height (ElementPanel.inl, tried in game). Other kinds keep theirs.
    inline std::vector<Vector> Ordered(const Kind& kind, std::vector<Vector> points)
    {
        if (points.size() != static_cast<size_t>(kind.points))
            throw std::runtime_error(std::string(kind.name) + " takes " + std::to_string(kind.points) + " points, not " + std::to_string(points.size()) + ".");
        if (kind.points == 3)
        {
            if (points[0][2] > points[1][2]) std::swap(points[0], points[1]);
            points[2][2] = points[0][2];
        }
        return points;
    }
    inline bool InOrder(const Kind& kind, const std::vector<Vector>& points)
    {
        if (kind.points != 3 || points.size() != 3) return true;
        return points[0][2] <= points[1][2] && std::abs(points[2][2] - points[0][2]) <= 1;
    }
    inline std::string Fixed(double value, int decimals = 3)
    {
        std::ostringstream text;
        text.setf(std::ios::fixed);
        text.precision(decimals);
        text << value;
        return text.str();
    }
    // The console command for an element whose points are already in order:
    // GE ADD ZL X1=.. Y1=.. Z1=.. X2=.. ...
    inline std::string AddCommand(const Kind& kind, const std::vector<Vector>& points)
    {
        if (points.size() != static_cast<size_t>(kind.points))
            throw std::runtime_error(std::string(kind.name) + " takes " + std::to_string(kind.points) + " points.");
        std::string command = std::string("GE ADD ") + kind.command;
        for (size_t i = 0; i < points.size(); ++i)
        {
            const auto n = std::to_string(i + 1);
            command += " X" + n + "=" + Fixed(points[i][0]) + " Y" + n + "=" + Fixed(points[i][1]) + " Z" + n + "=" + Fixed(points[i][2]);
        }
        return command;
    }

    // The wiki's measurements (Geometric-Event-(GE)), in Unreal units.
    namespace Wiki
    {
        constexpr double LedgeMin = 208;      // lowest ledge a spy grabs from below
        constexpr double LedgeStanding = 320; // highest grabbed from a standstill
        constexpr double LedgeRunning = 448;  // highest grabbed with a running jump (less smooth)
        constexpr double LedgeCoop = 512;     // the co-op boost height
        constexpr double CoopDepth = 144;     // free platform the second spy needs at co-op height
        constexpr double Obstacle = 115;      // below this the spy just jumps the obstacle
        constexpr double PullUp = 64;         // ledge to wall, to pull up and stand
        constexpr double LedgeLength = 32;
        constexpr double FenceMin = 88, FenceMax = 128, FenceBest = 96;
        constexpr double HandOverHand = 216;  // clearance below a hand-over-hand bar
        constexpr double ZiplineRadius = 2;
        constexpr double ZiplineHang = 384;   // highest above the floor a spy can reach the cable
        constexpr double ZiplineClearance = 198, ZiplineTucked = 161;
        constexpr int LadderBarsMin = 8;      // spies climb two bars at a time
        // The wiki counts a ladder in bars but gives no bar spacing. RE+ takes
        // 32 units: about 30 cm (1 unit is 1 cm, the wiki's laser page), the
        // rung pitch of real ladders and of the stock ladder meshes, so the
        // 8-bar minimum is a 256-unit climb. A spec states bars (or
        // barSpacing) on the element when its mesh differs.
        constexpr double LadderBarSpacing = 32;
        constexpr double CoopTolerance = 16;
    }

    // One element to measure. bars and surface are what the points cannot say:
    // the rungs of the ladder mesh, and how deep the platform behind a ledge is.
    struct Element
    {
        const Kind* kind = nullptr;
        std::vector<Vector> points;
        int bars = 0;                         // 0: from the height and barSpacing
        double barSpacing = Wiki::LadderBarSpacing;
        double surface = -1;                  // ledge to wall behind it; <0 unknown
    };
    // {kind | command, points, bars?, barSpacing?, surface?}: a level spec ges[]
    // entry, a ge.add request or a ge.list element ({kind: "Zip line", points,
    // command: "GE ADD ZL ..."}).
    inline Element FromJson(const Json& ge)
    {
        if (!ge.is_object()) throw std::runtime_error("A geometric event is {kind, points}.");
        Element e;
        std::string kind;
        for (const char* key : {"kind", "type", "command"})
            if (ge.contains(key) && ge.at(key).is_string() && !ge.at(key).get<std::string>().empty()) { kind = ge.at(key).get<std::string>(); break; }
        if (kind.rfind("GE ADD ", 0) == 0) kind = kind.substr(7, kind.find(' ', 7) == std::string::npos ? std::string::npos : kind.find(' ', 7) - 7);
        e.kind = &Require(kind);
        if (!ge.contains("points") || !ge.at("points").is_array()) throw std::runtime_error(std::string(e.kind->name) + " needs points: [[x, y, z], ...].");
        for (size_t i = 0; i < ge.at("points").size(); ++i) e.points.push_back(Point(ge.at("points")[i], std::string(e.kind->name) + " point " + std::to_string(i + 1)));
        if (ge.contains("bars") && ge.at("bars").is_number_integer()) e.bars = ge.at("bars").get<int>();
        if (ge.contains("barSpacing") && ge.at("barSpacing").is_number() && ge.at("barSpacing").get<double>() > 0) e.barSpacing = ge.at("barSpacing").get<double>();
        if (ge.contains("surface") && ge.at("surface").is_number()) e.surface = ge.at("surface").get<double>();
        return e;
    }
    inline Json ToJson(const Element& e)
    {
        Json points = Json::array();
        for (const auto& p : e.points) points.push_back(p);
        Json out{{"kind", e.kind ? e.kind->spec : ""}, {"command", e.kind ? e.kind->command : ""}, {"points", points}};
        if (e.bars) out["bars"] = e.bars;
        if (e.surface >= 0) out["surface"] = e.surface;
        return out;
    }

    // The floor under a point: the height of the first surface straight below
    // it, or nothing when the caller does not know.
    using FloorAt = std::function<std::optional<double>(const Vector& at)>;

    inline double Planar(const Vector& a, const Vector& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }
    inline double Distance(const Vector& a, const Vector& b) { return std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2])); }
    inline Vector Lerp(const Vector& a, const Vector& b, double t) { return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t}; }

    // Where the checks read the floor, so a live caller knows what to trace
    // straight down from. A ledge or a fence sits on an edge, so its floor is
    // read a little to either side of the edge and just below it (the side
    // inside the geometry finds the geometry itself, the open side the floor
    // the spy stands on). A hand-over-hand bar or a zip line is hung from, so
    // its floor is read under the line.
    inline constexpr double kSideOffset = 24;
    inline std::vector<Vector> Probes(const Element& e)
    {
        std::vector<Vector> out;
        if (!e.kind || e.points.size() < static_cast<size_t>(e.kind->points)) return out;
        const auto& a = e.points[0];
        const auto& b = e.points[1];
        const std::string command = e.kind->command;
        if (command == "LG" || command == "FENCE")
        {
            const Vector mid = Lerp(a, b, .5);
            const double planar = Planar(a, b);
            const double nx = planar > 1e-6 ? -(b[1] - a[1]) / planar : 1, ny = planar > 1e-6 ? (b[0] - a[0]) / planar : 0;
            out.push_back({mid[0] + nx * kSideOffset, mid[1] + ny * kSideOffset, mid[2] - 2});
            out.push_back({mid[0] - nx * kSideOffset, mid[1] - ny * kSideOffset, mid[2] - 2});
        }
        else if (command == "HOH")
            for (double t : {0.0, .5, 1.0}) { auto p = Lerp(a, b, t); p[2] -= 2; out.push_back(p); }
        else if (command == "ZL")
            for (double t : {0.0, .25, .5, .75, 1.0}) { auto p = Lerp(a, b, t); p[2] -= Wiki::ZiplineRadius + 2; out.push_back(p); }
        return out;
    }

    // A measurement outside the wiki's numbers. rule is the Rules catalogue id
    // (LevelRulesModel.h); severity is this model's own reading, which the
    // catalogue may change per profile.
    struct Finding { std::string severity, rule, message; double measured = 0; Json suggestion; };

    inline std::string Units(double value) { return Fixed(value, 0) + " units"; }
    inline std::vector<Finding> Measure(const Element& e, const FloorAt& floor = {})
    {
        std::vector<Finding> out;
        if (!e.kind) { out.push_back({"error", "ge.points", "A geometric event needs a kind.", 0, Json()}); return out; }
        const std::string name = e.kind->name;
        if (e.points.size() != static_cast<size_t>(e.kind->points))
        {
            out.push_back({"error", "ge.points", name + " takes " + std::to_string(e.kind->points) + " points (" +
                           (e.kind->third ? "the two ends, then " + std::string(e.kind->third) : std::string("its two ends")) + "), not " + std::to_string(e.points.size()) + ".",
                           static_cast<double>(e.points.size()), Json()});
            return out;
        }
        const auto& a = e.points[0];
        const auto& b = e.points[1];
        const std::string command = e.kind->command;
        // The lowest floor any probe found: a ledge is measured from the
        // floor in front of it, never from its own top.
        auto lowest = [&]() -> std::optional<double>
        {
            std::optional<double> best;
            if (!floor) return best;
            for (const auto& p : Probes(e))
                if (const auto f = floor(p); f && *f <= p[2] + 2) best = best ? std::min(*best, *f) : *f;
            return best;
        };
        if (command == "PIPE" || command == "LADDER")
        {
            if (!InOrder(*e.kind, e.points))
            {
                const auto ordered = Ordered(*e.kind, e.points);
                Json points = Json::array();
                for (const auto& p : ordered) points.push_back(p);
                out.push_back({"warning", "ge.point-order",
                               name + " points must start at the lower end, with the side point at that end's height (" + Units(ordered[0][2]) +
                                   "); the game ignores one whose side point is higher. ge.add and level_build reorder them.",
                               a[2], Json{{"points", points}}});
            }
        }
        if (command == "LG")
        {
            const double length = Distance(a, b);
            if (length < Wiki::LedgeLength)
                out.push_back({"warning", "ge.ledge-length", name + " is " + Units(length) + " long; a ledge needs at least " + Units(Wiki::LedgeLength) + ".", length, Json()});
            if (const auto f = lowest())
            {
                const double top = std::max(a[2], b[2]), h = top - *f;
                if (h < Wiki::Obstacle)
                    out.push_back({"warning", "ge.obstacle", name + " is " + Units(h) + " above the floor, below the " + Units(Wiki::Obstacle) +
                                   " a climbable obstacle needs: the spy simply jumps it and never uses the ledge.", h, Json()});
                else if (h > Wiki::LedgeStanding && h <= Wiki::LedgeRunning)
                    out.push_back({"info", "ge.ledge-reach", name + " is " + Units(h) + " above the floor: above the " + Units(Wiki::LedgeStanding) +
                                   " a spy grabs standing, so only a running jump reaches it and the move is less smooth.", h, Json()});
                else if (std::abs(h - Wiki::LedgeCoop) <= Wiki::CoopTolerance)
                    out.push_back({"info", "ge.ledge-reach", name + " is " + Units(h) + " above the floor, the co-op height: one spy boosts the other, who needs " +
                                   Units(Wiki::CoopDepth) + " of free platform behind the ledge.", h, Json()});
                else if (h > Wiki::LedgeRunning)
                    out.push_back({"warning", "ge.ledge-height", name + " is " + Units(h) + " above the floor: too high to grab (" + Units(Wiki::LedgeRunning) +
                                   " at most with a running jump) and not at the " + Units(Wiki::LedgeCoop) + " co-op height.", h, Json()});
            }
            if (e.surface >= 0 && e.surface < Wiki::PullUp)
                out.push_back({"warning", "ge.pull-up", name + " has " + Units(e.surface) + " from the ledge to the wall behind it; a spy needs " + Units(Wiki::PullUp) +
                               " to pull up and stand.", e.surface, Json()});
        }
        else if (command == "FENCE")
        {
            if (const auto f = lowest())
            {
                const double h = std::max(a[2], b[2]) - *f;
                if (h < Wiki::FenceMin || h > Wiki::FenceMax)
                    out.push_back({"warning", "ge.fence-height", name + " is " + Units(h) + " high; the fence jump needs " + Units(Wiki::FenceMin) + " to " +
                                   Units(Wiki::FenceMax) + " (" + Units(Wiki::FenceBest) + " recommended).", h, Json()});
                else if (std::abs(h - Wiki::FenceBest) > 8)
                    out.push_back({"info", "ge.fence-recommended", name + " is " + Units(h) + " high, which works; " + Units(Wiki::FenceBest) + " is the recommended fence height.", h, Json()});
            }
        }
        else if (command == "HOH")
        {
            std::optional<double> clearance;
            if (floor)
                for (const auto& p : Probes(e))
                    if (const auto f = floor(p); f && *f <= p[2] + 2) clearance = clearance ? std::min(*clearance, p[2] + 2 - *f) : p[2] + 2 - *f;
            if (clearance && *clearance < Wiki::HandOverHand)
                out.push_back({"warning", "ge.hoh-clearance", name + " has " + Units(*clearance) + " of clearance below it; a spy needs " + Units(Wiki::HandOverHand) + " to go hand over hand.", *clearance, Json()});
        }
        else if (command == "ZL")
        {
            if (std::abs(a[2] - b[2]) < 1)
                out.push_back({"warning", "ge.zipline-slope", name + " is level; a spy slides down a zip line, so give its ends different heights.", 0, Json()});
            if (floor)
            {
                const auto probes = Probes(e);
                const auto& high = a[2] >= b[2] ? probes.front() : probes.back();
                if (const auto f = floor(high); f && *f <= high[2] + 2)
                {
                    const double hang = high[2] + Wiki::ZiplineRadius + 2 - *f;
                    if (hang > Wiki::ZiplineHang)
                        out.push_back({"warning", "ge.zipline-hang", name + " starts " + Units(hang) + " above the floor; a spy reaches a zip line at most " + Units(Wiki::ZiplineHang) + " up.", hang, Json()});
                }
                std::optional<double> clearance;
                for (const auto& p : probes)
                    if (const auto f = floor(p); f && *f <= p[2] + 2) clearance = clearance ? std::min(*clearance, p[2] + 2 - *f) : p[2] + 2 - *f;
                if (clearance && *clearance < Wiki::ZiplineClearance)
                    out.push_back({"warning", "ge.zipline-clearance",
                                   *clearance >= Wiki::ZiplineTucked
                                       ? name + " has " + Units(*clearance) + " of clearance below it: under the " + Units(Wiki::ZiplineClearance) + " a hanging spy needs, so the spy must tuck."
                                       : name + " has " + Units(*clearance) + " of clearance below it: under the " + Units(Wiki::ZiplineTucked) + " even a tucked spy needs.",
                                   *clearance, Json()});
            }
        }
        else if (command == "LADDER")
        {
            const double height = std::abs(b[2] - a[2]);
            const int bars = e.bars > 0 ? e.bars : static_cast<int>(std::lround(height / e.barSpacing));
            if (bars < Wiki::LadderBarsMin || bars % 2)
            {
                const int wanted = std::max(Wiki::LadderBarsMin, bars % 2 ? bars + 1 : bars);
                const auto ordered = Ordered(*e.kind, e.points);
                auto top = ordered[1];
                top[2] = ordered[0][2] + wanted * e.barSpacing;
                Json points = Json::array({ordered[0], top, ordered[2]});
                out.push_back({"warning", "ge.ladder-bars",
                               name + " has " + std::to_string(bars) + " bars (" + Units(height) + (e.bars > 0 ? "" : " at " + Units(e.barSpacing) + " a bar") +
                                   "); spies climb two at a time, so a ladder needs an even number of bars, at least " + std::to_string(Wiki::LadderBarsMin) + ": " +
                                   std::to_string(wanted) + " bars is " + Units(wanted * e.barSpacing) + ".",
                               static_cast<double>(bars), Json{{"points", points}, {"bars", wanted}}});
            }
        }
        return out;
    }
}
