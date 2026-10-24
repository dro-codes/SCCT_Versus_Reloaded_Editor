#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Reachability: where a spy and where a merc can walk from their team's player
// starts, worked out on a grid of steps with the engine's own collision
// queries (World). Pure logic, no editor calls: ReachabilityNative.inl answers
// the queries on the built level and Reachability.cpp runs the fill a few
// nodes at a time, draws it and hands Map Check its rows.
//
// How a step is taken, after UE2's walking (physWalking / stepUp):
// - A node is a place a player stands: x, y and the floor's height. The pawn
//   is a box (UE2 sweeps pawns against the world as boxes) of the team's
//   collision radius and half-height, kept kLift above the floor as a walking
//   pawn floats. Its cell is the grid square holding x, y; a cell keeps one
//   node per floor (floors closer than kMerge are the same floor).
// - From a node, each of eight directions one cell long: sweep straight
//   across; when that is blocked, sweep up by the step height, across at that
//   height, and down. The way down may drop any distance up to the team's
//   drop limit; the floor met must be walkable (its normal's Z at least the
//   cosine of the walkable slope) and no higher than a step above.
// - Head-on, a grid only lines up with a doorway by chance, so a blocked
//   straight move (the four axis directions) is tried again from a quarter
//   and half a cell to either side. Nodes keep where they landed, so a line
//   of steps through a door stays lined up with it.
// - When a standing player cannot get through, a crouched one is tried (each
//   team's crouch height): a node where a standing player does not fit is
//   crouch-only, and only crouched moves leave it.
// Not modelled: ladders, pipes, ledge grabs, zip lines and jumps (the only way
// up is a step), movers (doors count as open; lifts and their platforms are
// left out), and players pushing each other.
namespace Reach
{
using Vec3 = std::array<double, 3>;

inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline double Distance(const Vec3& a, const Vec3& b)
{
    const auto d = Sub(a, b);
    return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

// A sweep's first blocking hit: where the box stopped (its centre) and the
// surface's normal.
struct Hit
{
    bool hit = false;
    Vec3 location{}, normal{};
};

// The level's collision as the walk asks about it.
class World
{
public:
    virtual ~World() = default;
    // A box of half-size extent moved from one centre to another.
    virtual Hit Sweep(const Vec3& from, const Vec3& to, const Vec3& extent) = 0;
    // Whether the box fits at a centre without overlapping the world.
    virtual bool Fits(const Vec3& centre, const Vec3& extent) = 0;
};

enum Team : int { kSpy = 0, kMerc = 1 };
inline const char* TeamName(int team, bool plural = true) { return team == kSpy ? (plural ? "spies" : "spy") : (plural ? "mercs" : "merc"); }

// One team's player: its collision cylinder (radius, half-heights) and what it
// can climb and fall.
struct Pawn
{
    double radius = 35, height = 89, crouchHeight = 50;
    double step = 35;       // highest step walked up
    double slope = 45;      // steepest walkable floor, degrees
    double maxDrop = 1400;  // furthest fall followed (survivable from full health)
    bool crouch = true;
    double MinFloorZ() const { return std::cos(std::clamp(slope, 1.0, 89.0) * 3.14159265358979323846 / 180) - 1e-3; }
};

// Measured in game (MoveLabD, ClimbLabD): a spy is 178 tall standing (its middle
// 89 above the floor) and 100 crouched; a merc 175 (87.5) and 136 (68); both 35
// in radius and step 32 but not 36. A spy lives through a 1400 fall from full
// health, a merc through 900 (both die at 1500 and 1000).
inline Pawn DefaultPawn(int team)
{
    Pawn pawn;
    if (team == kMerc) { pawn.height = 87.5; pawn.crouchHeight = 68; pawn.maxDrop = 950; }
    return pawn;
}

// Sizes read from a pawn class are kept only when they make sense.
inline Pawn ClampPawn(Pawn pawn, int team)
{
    const auto fallback = DefaultPawn(team);
    auto keep = [](double value, double lo, double hi, double otherwise) { return std::isfinite(value) && value >= lo && value <= hi ? value : otherwise; };
    pawn.radius = keep(pawn.radius, 8, 128, fallback.radius);
    pawn.height = keep(pawn.height, 24, 160, fallback.height);
    pawn.crouchHeight = keep(pawn.crouchHeight, 12, pawn.height, (std::min)(fallback.crouchHeight, pawn.height));
    pawn.step = keep(pawn.step, 0, 256, fallback.step);
    pawn.slope = keep(pawn.slope, 1, 89, fallback.slope);
    pawn.maxDrop = keep(pawn.maxDrop, 0, 65536, fallback.maxDrop);
    return pawn;
}

// [Reachability] in Reloaded_Editor.ini.
struct Settings
{
    double cell = 32;          // grid step in units
    size_t maxCells = 150000;  // nodes per team before the fill stops
    double reach = 48;         // how near a player must get to an objective's or pickup's collision
};
inline Settings Clamp(Settings s)
{
    s.cell = std::isfinite(s.cell) ? std::clamp(s.cell, 16.0, 256.0) : 32;
    s.maxCells = std::clamp<size_t>(s.maxCells, 1000, 2000000);
    s.reach = std::isfinite(s.reach) ? std::clamp(s.reach, 0.0, 512.0) : 48;
    return s;
}

constexpr double kLift = 2;    // a walking pawn's gap above the floor
constexpr double kMerge = 24;  // nodes in one cell this close in height are one floor

// A place a player stands: at[2] is the floor's height.
struct Node
{
    Vec3 at{};
    bool crouchOnly = false;
    bool moved = false;   // a step from it reached another node
    int origin = -1;      // the seed it was first reached from
};

// A player start added to the fill: its node, or why it has none.
struct Seed
{
    int start = -1;
    int node = -1;
    bool joined = false;  // it stands on a node another start reached first
    std::string problem;
};

class Flood
{
public:
    struct Bounds { Vec3 lo{}, hi{}; };

    Flood(World& world, Pawn pawn, Settings settings, std::optional<Bounds> bounds = std::nullopt)
        : world_(world), pawn_(pawn), settings_(Clamp(settings)), bounds_(bounds) {}

    const Pawn& GetPawn() const { return pawn_; }
    const Settings& GetSettings() const { return settings_; }
    const std::vector<Node>& Nodes() const { return nodes_; }
    const std::vector<Seed>& Seeds() const { return seeds_; }
    bool Truncated() const { return truncated_; }
    bool Done() const { return truncated_ || head_ >= nodes_.size(); }
    size_t Calls() const { return calls_; }
    double Half(bool crouched) const { return crouched ? pawn_.crouchHeight : pawn_.height; }

    // A player start at its location (the actor's centre). The player is put
    // where it fits (at the start, or up to two steps higher, standing and
    // then crouched) and dropped to the floor below.
    int AddStart(int start, const Vec3& location)
    {
        Seed seed;
        seed.start = start;
        std::string problem = "a player spawned there would be stuck in the geometry";
        for (int mode = 0; mode < (pawn_.crouch ? 2 : 1) && seed.node < 0; ++mode)
        {
            const bool crouched = mode == 1;
            const double h = Half(crouched);
            // On the middle of its cell first, so every start's steps (both
            // teams') line up on one grid and their cells compare; then where it
            // is, and, as the engine's FindSpot does for a start a little too
            // close to a wall, up to half a cell sideways.
            const double n = settings_.cell / 2;
            const double sx = (std::floor(location[0] / settings_.cell) + 0.5) * settings_.cell - location[0];
            const double sy = (std::floor(location[1] / settings_.cell) + 0.5) * settings_.cell - location[1];
            const double nudges[10][2] = {{sx, sy}, {0, 0}, {n, 0}, {-n, 0}, {0, n}, {0, -n}, {n, n}, {n, -n}, {-n, n}, {-n, -n}};
            bool placed = false;
            for (const auto& nudge : nudges)
            for (double raise : {0.0, pawn_.step, 2 * pawn_.step})
            {
                if (placed) break;
                const Vec3 centre{location[0] + nudge[0], location[1] + nudge[1], location[2] + raise};
                if (!Fits(centre, Extent(crouched))) continue;
                const double lowest = location[2] - h - pawn_.maxDrop;
                const auto land = Sweep(centre, {centre[0], centre[1], lowest + h}, Extent(crouched));
                placed = true;
                if (!land.hit) { problem = "there is no floor within " + std::to_string(static_cast<int>(pawn_.maxDrop)) + " units below it"; break; }
                if (land.normal[2] < pawn_.MinFloorZ()) { problem = "the floor below it is too steep to stand on"; break; }
                Node node;
                node.at = {centre[0], centre[1], land.location[2] - h};
                node.crouchOnly = crouched;
                node.origin = static_cast<int>(seeds_.size());
                if (const int existing = Find(node.at); existing >= 0) { seed.node = existing; seed.joined = true; }
                else seed.node = Add(node);
            }
            if (placed) break;
        }
        if (seed.node < 0) seed.problem = problem;
        seeds_.push_back(seed);
        return static_cast<int>(seeds_.size()) - 1;
    }

    // Takes the steps from up to `count` more nodes. True when the fill is done.
    bool Step(size_t count)
    {
        static const int directions[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        for (size_t n = 0; n < count && !Done(); ++n)
        {
            const size_t index = head_++;
            for (const auto& d : directions)
            {
                if (truncated_) break;
                const Node from = nodes_[index];
                const double dx = d[0] * settings_.cell, dy = d[1] * settings_.cell;
                // A neighbour already standing at this floor: nothing new there.
                if (Find({from.at[0] + dx, from.at[1] + dy, from.at[2]}) >= 0) { nodes_[index].moved = true; continue; }
                const auto landing = Move(from, dx, dy, d[0] == 0 || d[1] == 0);
                if (!landing) continue;
                nodes_[index].moved = true;
                if (Find(landing->at) >= 0) continue;
                Node node = *landing;
                node.origin = from.origin;
                if (nodes_.size() >= settings_.maxCells) { truncated_ = true; break; }
                Add(node);
            }
        }
        return Done();
    }

    // The node in the cell under a point on (or within kMerge of) its floor, or -1.
    int Find(const Vec3& at) const
    {
        const auto column = columns_.find(Key(at));
        if (column == columns_.end()) return -1;
        for (int index : column->second)
            if (std::abs(nodes_[static_cast<size_t>(index)].at[2] - at[2]) <= kMerge) return index;
        return -1;
    }
    std::pair<long long, long long> Cell(const Vec3& at) const
    {
        return {static_cast<long long>(std::floor(at[0] / settings_.cell)), static_cast<long long>(std::floor(at[1] / settings_.cell))};
    }
    // Nodes in the cells a box around a point touches.
    template<class F> void Near(const Vec3& at, double radius, F&& visit) const
    {
        const auto lo = Cell({at[0] - radius, at[1] - radius, 0}), hi = Cell({at[0] + radius, at[1] + radius, 0});
        if ((hi.first - lo.first + 1) * (hi.second - lo.second + 1) > 40000) return;
        for (long long x = lo.first; x <= hi.first; ++x)
            for (long long y = lo.second; y <= hi.second; ++y)
            {
                const auto column = columns_.find(Pack(x, y));
                if (column == columns_.end()) continue;
                for (int index : column->second) visit(nodes_[static_cast<size_t>(index)]);
            }
    }

private:
    World& world_;
    Pawn pawn_;
    Settings settings_;
    std::optional<Bounds> bounds_;
    std::vector<Node> nodes_;
    std::vector<Seed> seeds_;
    std::unordered_map<long long, std::vector<int>> columns_;
    size_t head_ = 0, calls_ = 0;
    bool truncated_ = false;

    static long long Pack(long long x, long long y) { return (x << 32) ^ (y & 0xffffffffLL); }
    long long Key(const Vec3& at) const { const auto c = Cell(at); return Pack(c.first, c.second); }
    Vec3 Extent(bool crouched) const { return {pawn_.radius, pawn_.radius, Half(crouched)}; }
    Hit Sweep(const Vec3& from, const Vec3& to, const Vec3& extent) { ++calls_; return world_.Sweep(from, to, extent); }
    bool Fits(const Vec3& centre, const Vec3& extent) { ++calls_; return world_.Fits(centre, extent); }
    bool Clear(const Vec3& from, const Vec3& to, const Vec3& extent)
    {
        const auto hit = Sweep(from, to, extent);
        return !hit.hit || Distance(hit.location, to) < 0.05;
    }
    bool Inside(const Vec3& at) const
    {
        if (!bounds_) return true;
        for (size_t i = 0; i < 2; ++i)
            if (at[i] < bounds_->lo[i] || at[i] > bounds_->hi[i]) return false;
        return true;
    }
    int Add(const Node& node)
    {
        const int index = static_cast<int>(nodes_.size());
        nodes_.push_back(node);
        columns_[Key(node.at)].push_back(index);
        return index;
    }

    enum class Try { Blocked, Landed, NoFloor };
    // One step from a box centre: across, or up a step, across and down.
    Try StepFrom(const Node& node, const Vec3& from, double dx, double dy, bool crouched, Node& landing)
    {
        const Vec3 extent = Extent(crouched);
        const double h = Half(crouched);
        const Vec3 to{from[0] + dx, from[1] + dy, from[2]};
        if (!Inside(to)) return Try::Blocked;
        Vec3 end = to;
        if (!Clear(from, to, extent))
        {
            const Vec3 up{from[0], from[1], from[2] + pawn_.step};
            const auto ceiling = Sweep(from, up, extent);
            const double top = ceiling.hit ? ceiling.location[2] : up[2];
            if (top < from[2] + 1) return Try::Blocked;
            const Vec3 raised{from[0], from[1], top};
            end = {to[0], to[1], top};
            if (!Clear(raised, end, extent)) return Try::Blocked;
        }
        const double lowest = node.at[2] - pawn_.maxDrop;
        const auto land = Sweep(end, {end[0], end[1], lowest + h}, extent);
        if (!land.hit) return Try::NoFloor;
        if (land.normal[2] < pawn_.MinFloorZ()) return Try::Blocked;
        const double floor = land.location[2] - h;
        if (floor > node.at[2] + pawn_.step + kLift + 1) return Try::Blocked;
        landing = Node{};
        landing.at = {end[0], end[1], floor};
        return Try::Landed;
    }

    std::optional<Node> Move(const Node& node, double dx, double dy, bool orthogonal)
    {
        for (int mode = node.crouchOnly ? 1 : 0; mode < (pawn_.crouch ? 2 : 1); ++mode)
        {
            const bool crouched = mode == 1;
            const Vec3 centre{node.at[0], node.at[1], node.at[2] + Half(crouched) + kLift};
            Node landing;
            Try result = StepFrom(node, centre, dx, dy, crouched, landing);
            if (result == Try::NoFloor) return std::nullopt;
            if (result == Try::Blocked && orthogonal)
            {
                // Sideways first: a quarter and half a cell either way.
                const double length = std::sqrt(dx * dx + dy * dy);
                const double px = -dy / length, py = dx / length, q = settings_.cell / 4;
                for (double offset : {q, -q, 2 * q, -2 * q})
                {
                    const Vec3 side{centre[0] + px * offset, centre[1] + py * offset, centre[2]};
                    if (!Inside(side) || !Clear(centre, side, Extent(crouched))) continue;
                    result = StepFrom(node, side, dx, dy, crouched, landing);
                    if (result != Try::Blocked) break;
                }
                if (result == Try::NoFloor) return std::nullopt;
            }
            if (result != Try::Landed) continue;
            // A crouched arrival stands up where a standing player fits.
            landing.crouchOnly = crouched
                && !(Fits({landing.at[0], landing.at[1], landing.at[2] + pawn_.height + kLift}, Extent(false)));
            return landing;
        }
        return std::nullopt;
    }
};

// ----------------------------------------------------------- findings --

struct Start
{
    std::string path, name;
    int team = -1;   // TeamNumber: 0 spies, 1 mercs
    Vec3 location{};
};

// What a team has to get to.
struct Target
{
    std::string path, name, kind;   // kind as Map Design names it ("Computer terminal", "Pickup")
    Vec3 location{};
    double radius = 0, height = 0;   // its collision cylinder (0 when it has none)
    bool objective = true;          // false: a pickup
    int team = -1;                  // a pickup's team when its class names one, else -1
};

struct Scene
{
    std::vector<Start> starts;
    std::vector<Target> targets;
    Pawn pawns[2] = {DefaultPawn(kSpy), DefaultPawn(kMerc)};
    bool boundsKnown = false;
    Vec3 lo{}, hi{};
};

inline std::string Fold(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
// A pickup's team from its class names (most derived first), or -1.
inline int PickupTeam(const std::vector<std::string>& classes)
{
    for (const auto& c : classes)
    {
        const auto folded = Fold(c);
        const bool spy = folded.find("spy") != std::string::npos, merc = folded.find("merc") != std::string::npos;
        if (spy != merc) return spy ? kSpy : kMerc;
    }
    return -1;
}

// Whether a team's player gets near enough to touch or use the target.
inline bool Reaches(const Flood& flood, const Target& target, double reach)
{
    const auto& pawn = flood.GetPawn();
    const double across = (std::max)(0.0, target.radius) + pawn.radius + reach;
    bool found = false;
    flood.Near(target.location, across, [&](const Node& node)
    {
        if (found) return;
        const double dx = node.at[0] - target.location[0], dy = node.at[1] - target.location[1];
        if (dx * dx + dy * dy > across * across) return;
        const double half = node.crouchOnly ? pawn.crouchHeight : pawn.height;
        const double middle = node.at[2] + half + kLift;
        found = std::abs(middle - target.location[2]) <= (std::max)(0.0, target.height) + half + reach;
    });
    return found;
}

inline std::string Number(double value)
{
    long long n = std::llround(value);
    std::string digits = std::to_string(n < 0 ? -n : n), out;
    for (size_t i = 0; i < digits.size(); ++i)
    {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return (n < 0 ? "-" : "") + out;
}

constexpr const char* kNotModelled =
    "Ladders, pipes, ledge grabs, zip lines and jumps are not modelled (the only way up is a step), and doors and lifts are left out (doors count as open)";

struct Finding
{
    int severity = 1;   // 0 error, 1 warning, 2 note
    std::string check, message;
    std::vector<std::string> actors;
};

// Map Check's rows: objectives and pickups a team that needs them cannot get
// to (warnings, since a climb may be the way), and starts that reach nothing
// (errors). floods[t] is null for a team that was not
// worked out (no starts).
inline std::vector<Finding> Findings(const Scene& scene, const Flood* const floods[2])
{
    std::vector<Finding> out;
    const auto& settings = floods[0] ? floods[0]->GetSettings() : floods[1] ? floods[1]->GetSettings() : Settings{};
    for (int team : {kSpy, kMerc})
    {
        const auto* flood = floods[team];
        if (!flood)
        {
            out.push_back({2, "No " + std::string(TeamName(team, false)) + " starts",
                           "The map has no " + std::string(TeamName(team, false)) + " player start (TeamNumber " + std::to_string(team) + "), so where "
                           + TeamName(team) + " can walk was not worked out.", {}});
            continue;
        }
        for (const auto& seed : flood->Seeds())
        {
            if (seed.start < 0 || static_cast<size_t>(seed.start) >= scene.starts.size()) continue;
            const auto& start = scene.starts[static_cast<size_t>(seed.start)];
            std::string why;
            if (seed.node < 0) why = seed.problem;
            else if (!seed.joined && !flood->Nodes()[static_cast<size_t>(seed.node)].moved)
                why = "a player there cannot take a single step: walls or steps higher than " + Number(flood->GetPawn().step) + " units on every side";
            if (why.empty()) continue;
            out.push_back({0, "Start reaches nothing", start.name + " (" + TeamName(team, false) + " start) reaches nothing: " + why + ".", {start.path}});
        }
        if (flood->Truncated())
            out.push_back({2, "Reachability stopped", "The " + std::string(TeamName(team, false)) + " fill stopped at " + Number(static_cast<double>(flood->Nodes().size()))
                           + " cells (MaxCells), so more may be reachable than is shown. Raise MaxCells or CellSize in Reloaded_Editor.ini [Reachability].", {}});
    }
    for (const auto& target : scene.targets)
    {
        if (target.objective)
        {
            for (int team : {kSpy, kMerc})
            {
                const auto* flood = floods[team];
                if (!flood || flood->Nodes().empty() || Reaches(*flood, target, settings.reach)) continue;
                const bool spies = team == kSpy;
                // A warning, not an error: a climb the fill does not model may be the way there.
                out.push_back({1, spies ? "Spies cannot reach" : "Mercs cannot reach",
                               target.name + " (" + Fold(target.kind) + ") is out of the " + TeamName(team) + "' reach on foot: no " + TeamName(team, false)
                               + " start leads to within " + Number(settings.reach) + " units of it" + (spies ? "." : ", so they cannot defend it.")
                               + " Ladders, pipes, ledge grabs, zip lines and jumps are not modelled.", {target.path}});
            }
            continue;
        }
        // A pickup: its own team, or whoever can get to it.
        std::vector<int> teams;
        if (target.team >= 0) teams.push_back(target.team);
        else teams = {kSpy, kMerc};
        bool reached = false, tested = false;
        for (int team : teams)
        {
            const auto* flood = floods[team];
            if (!flood || flood->Nodes().empty()) continue;
            tested = true;
            reached = reached || Reaches(*flood, target, settings.reach);
        }
        if (!tested || reached) continue;
        out.push_back({1, "Pickup out of reach",
                       target.name + " is out of reach on foot for " + (target.team >= 0 ? std::string(TeamName(target.team)) : std::string("both teams"))
                       + ". Ladders, pipes, ledge grabs, zip lines and jumps are not modelled.", {target.path}});
    }
    return out;
}

// ------------------------------------------------------------ overlay --

// A cell as drawn: its centre on the floor and who stands there.
struct Patch
{
    Vec3 at{};
    unsigned teams = 0;   // bit 0 spies, bit 1 mercs
};

// Both teams' nodes, one patch per cell and floor.
inline std::vector<Patch> Patches(const Flood* const floods[2])
{
    std::vector<Patch> out;
    std::unordered_map<long long, std::vector<size_t>> columns;
    double cell = 32;
    for (int team : {kSpy, kMerc})
    {
        const auto* flood = floods[team];
        if (!flood) continue;
        cell = flood->GetSettings().cell;
        for (const auto& node : flood->Nodes())
        {
            const auto c = flood->Cell(node.at);
            const long long key = (c.first << 32) ^ (c.second & 0xffffffffLL);
            auto& column = columns[key];
            bool merged = false;
            for (size_t index : column)
                if (std::abs(out[index].at[2] - node.at[2]) <= kMerge) { out[index].teams |= 1u << team; merged = true; break; }
            if (merged) continue;
            column.push_back(out.size());
            out.push_back({{(static_cast<double>(c.first) + 0.5) * cell, (static_cast<double>(c.second) + 0.5) * cell, node.at[2]}, 1u << team});
        }
    }
    return out;
}

struct Summary
{
    size_t both = 0, spies = 0, mercs = 0;
    size_t Total() const { return both + spies + mercs; }
};
inline Summary Summarise(const std::vector<Patch>& patches)
{
    Summary s;
    for (const auto& p : patches)
    {
        if (p.teams == 3) ++s.both;
        else if (p.teams == 1) ++s.spies;
        else if (p.teams == 2) ++s.mercs;
    }
    return s;
}

// The legend drawn in the viewports, first line plain, then one per colour.
inline std::vector<std::string> Legend(const Summary& s, double cell, bool stale)
{
    std::vector<std::string> lines;
    lines.push_back("Reachability on foot from the player starts (" + Number(cell) + "-unit grid)" + (stale ? "; the map changed: Build > Reachability Map off and on to update" : ""));
    lines.push_back("Green  both teams: " + Number(static_cast<double>(s.both)) + " cells");
    lines.push_back("Blue  spies only (one diagonal): " + Number(static_cast<double>(s.spies)));
    lines.push_back("Red  mercs only (both diagonals): " + Number(static_cast<double>(s.mercs)));
    lines.push_back("Not modelled: ladders, pipes, ledge grabs, zip lines, jumps.");
    lines.push_back("Doors count as open; lifts are left out.");
    return lines;
}

// The lines a patch is drawn with, a little above the floor: a square inset
// from its neighbours, plus a diagonal for spies only and both for mercs only,
// so the colours read without colour too.
inline std::vector<std::pair<Vec3, Vec3>> Outline(const Patch& patch, double size, double lift = 3)
{
    const double h = size * 0.4, z = patch.at[2] + lift;
    const Vec3 p[4] = {{patch.at[0] - h, patch.at[1] - h, z}, {patch.at[0] + h, patch.at[1] - h, z},
                       {patch.at[0] + h, patch.at[1] + h, z}, {patch.at[0] - h, patch.at[1] + h, z}};
    std::vector<std::pair<Vec3, Vec3>> lines{{p[0], p[1]}, {p[1], p[2]}, {p[2], p[3]}, {p[3], p[0]}};
    if (patch.teams != 3) lines.push_back({p[0], p[2]});
    if (patch.teams == 2) lines.push_back({p[1], p[3]});
    return lines;
}
}
