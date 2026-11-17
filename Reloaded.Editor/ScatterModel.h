#pragma once
#include "PlacementModel.h"
#include <cstdint>
#include <functional>

// Scatter Props: fills an area with static meshes picked by weight, each
// turned and scaled at random, dropped onto the floor below and kept clear of
// walls, of the props it already placed and of the small meshes already in
// the map. The editor side (ScatterNative.inl) traces the level; everything
// here is plain geometry and the seeded choices, so the same seed and the
// same map give the same props.
namespace Workflow::Scatter
{
namespace Place = Workflow::Placement;

// Props per operation, tries per prop wanted, and the square a density counts in.
constexpr int kMaxProps = 500;
constexpr int kTriesPerProp = 40;
constexpr double kDensitySquare = 256.0 * 256.0;
// Meshes already in the map at most this big on every axis are props to keep clear of.
constexpr double kPropSize = 512;
// The largest circle radius.
constexpr double kMaxRadius = 100000;

// SplitMix64: the same numbers from a seed on every build, which the standard
// library's distributions do not promise.
class Random
{
public:
    explicit Random(uint64_t seed) : state(seed) {}
    uint64_t Next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    // In [0, 1).
    double Unit() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }
    double Range(double a, double b) { return a + (b - a) * Unit(); }
private:
    uint64_t state;
};

using Point = std::array<double, 2>;
using Polygon = std::vector<Point>;

// Where props go, seen from above: a circle, or the builder brush's faces
// (any point over one of them is inside).
struct Area
{
    bool circle = false;
    Point centre{};
    double radius = 0;
    std::vector<Polygon> polygons;
    Point lo{}, hi{};
};
inline double SignedArea(const Polygon& polygon)
{
    double twice = 0;
    for (size_t i = 0; i < polygon.size(); ++i)
    {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % polygon.size()];
        twice += a[0] * b[1] - b[0] * a[1];
    }
    return twice / 2;
}
// Even-odd crossings, so a face of any shape works.
inline bool InPolygon(const Polygon& polygon, const Point& p)
{
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
    {
        const auto& a = polygon[i];
        const auto& b = polygon[j];
        if ((a[1] > p[1]) != (b[1] > p[1]) && p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0]) inside = !inside;
    }
    return inside;
}
inline bool Inside(const Area& area, const Point& p)
{
    if (p[0] < area.lo[0] || p[0] > area.hi[0] || p[1] < area.lo[1] || p[1] > area.hi[1]) return false;
    if (area.circle) return std::hypot(p[0] - area.centre[0], p[1] - area.centre[1]) <= area.radius;
    for (const auto& polygon : area.polygons) if (InPolygon(polygon, p)) return true;
    return false;
}
inline Area CircleArea(const Point& centre, double radius)
{
    for (double v : centre) if (!std::isfinite(v) || std::abs(v) > 10000000) throw std::runtime_error("The last click is out of range: click in a viewport first.");
    if (!std::isfinite(radius) || radius < 8 || radius > kMaxRadius) throw std::runtime_error("The radius is between 8 and " + std::to_string(static_cast<int>(kMaxRadius)) + " units.");
    Area area;
    area.circle = true;
    area.centre = centre;
    area.radius = radius;
    area.lo = { centre[0] - radius, centre[1] - radius };
    area.hi = { centre[0] + radius, centre[1] + radius };
    return area;
}
// The faces of a brush flattened onto the floor plane; the ones seen edge-on
// (a box's sides) cover nothing and are left out.
inline Area PolygonArea(const std::vector<Polygon>& faces)
{
    Area area;
    bool first = true;
    for (const auto& face : faces)
    {
        if (face.size() < 3 || std::abs(SignedArea(face)) < 1) continue;
        for (const auto& p : face)
        {
            for (double v : p) if (!std::isfinite(v) || std::abs(v) > 10000000) throw std::runtime_error("The builder brush is out of range.");
            if (first) { area.lo = area.hi = p; first = false; }
            for (int i = 0; i < 2; ++i) { area.lo[i] = std::min(area.lo[i], p[i]); area.hi[i] = std::max(area.hi[i], p[i]); }
        }
        area.polygons.push_back(face);
    }
    if (area.polygons.empty()) throw std::runtime_error("Seen from above, the builder brush covers nothing.");
    return area;
}
// The area in square units: a circle's exactly, the faces' on a fine grid.
inline double Size(const Area& area)
{
    if (area.circle) return Place::kPi * area.radius * area.radius;
    constexpr int n = 128;
    const double w = (area.hi[0] - area.lo[0]) / n, h = (area.hi[1] - area.lo[1]) / n;
    int in = 0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (Inside(area, { area.lo[0] + (i + 0.5) * w, area.lo[1] + (j + 0.5) * h })) ++in;
    return w * h * in;
}

// One mesh to scatter: how often it is picked against the others, and its
// bounds as the mesh holds them (before any turn or scale).
struct Item { double weight = 1; Place::Box bounds; };
struct Settings
{
    bool byDensity = false;
    int count = 20;
    double density = 1;      // props per 256 x 256 units
    bool randomYaw = true;
    bool randomTilt = false; // pitch and roll too
    double scaleMin = 1, scaleMax = 1;
    double spacing = 0;      // the least gap between two props' boxes, seen from above
    uint64_t seed = 1;
};
inline void Validate(const Settings& s, const std::vector<Item>& items)
{
    if (items.empty()) throw std::runtime_error("Add a static mesh to scatter first.");
    double total = 0;
    for (const auto& item : items)
    {
        if (!std::isfinite(item.weight) || item.weight < 0 || item.weight > 1000000) throw std::runtime_error("A weight is 0 or more.");
        total += item.weight;
        Place::Check(item.bounds.lo, "A mesh's bounds are out of range."); Place::Check(item.bounds.hi, "A mesh's bounds are out of range.");
        for (int i = 0; i < 3; ++i) if (item.bounds.lo[i] > item.bounds.hi[i]) throw std::runtime_error("A mesh has no bounds.");
    }
    if (!(total > 0)) throw std::runtime_error("Give at least one mesh a weight above 0.");
    if (!s.byDensity && (s.count < 1 || s.count > kMaxProps)) throw std::runtime_error("Scatter between 1 and " + std::to_string(kMaxProps) + " props.");
    if (s.byDensity && (!std::isfinite(s.density) || s.density <= 0 || s.density > 1000)) throw std::runtime_error("The density is above 0.");
    if (!std::isfinite(s.scaleMin) || !std::isfinite(s.scaleMax) || s.scaleMin < 0.01 || s.scaleMax > 100 || s.scaleMin > s.scaleMax)
        throw std::runtime_error("The scale range is from 0.01 to 100, smallest first.");
    if (!std::isfinite(s.spacing) || s.spacing < 0 || s.spacing > 100000) throw std::runtime_error("The spacing is 0 or more.");
}
// How many props a run aims for: the count, or the density over the area.
inline int Target(const Settings& s, const Area& area)
{
    if (!s.byDensity) return s.count;
    const double wanted = std::round(s.density * Size(area) / kDensitySquare);
    if (wanted < 1) throw std::runtime_error("At that density the area holds no props: raise the density.");
    if (wanted > kMaxProps) throw std::runtime_error("That density makes " + std::to_string(static_cast<long long>(wanted)) + " props; one scatter places at most " + std::to_string(kMaxProps) + ".");
    return static_cast<int>(wanted);
}
// The item a number in [0, 1) picks, by weight; a weight of 0 is never picked.
inline size_t Pick(const std::vector<Item>& items, double unit)
{
    double total = 0;
    for (const auto& item : items) total += item.weight;
    double at = unit * total;
    size_t last = 0;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (!(items[i].weight > 0)) continue;
        last = i;
        if (at < items[i].weight) return i;
        at -= items[i].weight;
    }
    return last;
}

// A prop before its drop: where over the floor, which mesh, its turn and scale.
struct Candidate { size_t item = 0; Point at{}; Rotation rotation{}; double scale = 1; };
// A prop at rest: its pivot, turn, scale and world box.
struct Prop { size_t item = 0; Vector location{}; Rotation rotation{}; double scale = 1; Place::Box box; };

// The next candidate: a point inside the area, then the mesh, yaw, tilt and
// scale, always drawn in that order so a seed gives the same run. None when
// the area is too thin to hit.
inline std::optional<Candidate> Sample(Random& random, const Settings& s, const std::vector<Item>& items, const Area& area)
{
    Candidate c;
    bool found = false;
    for (int i = 0; i < 1000 && !found; ++i)
    {
        c.at = { random.Range(area.lo[0], area.hi[0]), random.Range(area.lo[1], area.hi[1]) };
        found = Inside(area, c.at);
    }
    if (!found) return std::nullopt;
    c.item = Pick(items, random.Unit());
    const int yaw = static_cast<int>(random.Unit() * 65536);
    const int pitch = static_cast<int>(random.Unit() * 65536), roll = static_cast<int>(random.Unit() * 65536);
    c.rotation = { s.randomTilt ? pitch : 0, s.randomYaw || s.randomTilt ? yaw : 0, s.randomTilt ? roll : 0 };
    const double t = random.Unit();
    c.scale = s.scaleMin + (s.scaleMax - s.scaleMin) * t;
    return c;
}
// The box a candidate makes with its pivot at height z.
inline Place::Box CandidateBox(const Candidate& c, const std::vector<Item>& items, double z = 0)
{
    return Place::WorldBox(items.at(c.item).bounds, { c.at[0], c.at[1], z }, c.rotation, { c.scale, c.scale, c.scale });
}

// How far apart two boxes are seen from above: the distance between their
// footprints, or less than 0 by how deep they overlap.
inline double Gap(const Place::Box& a, const Place::Box& b)
{
    const double gx = std::max(a.lo[0] - b.hi[0], b.lo[0] - a.hi[0]);
    const double gy = std::max(a.lo[1] - b.hi[1], b.lo[1] - a.hi[1]);
    if (gx < 0 && gy < 0) return std::max(gx, gy);
    return std::hypot(std::max(gx, 0.0), std::max(gy, 0.0));
}
// Boxes that share more than a unit of space on every axis; resting on top
// of one another is not overlapping.
inline bool Overlaps(const Place::Box& a, const Place::Box& b, double tolerance = 1)
{
    for (int i = 0; i < 3; ++i)
        if (std::min(a.hi[i], b.hi[i]) - std::max(a.lo[i], b.lo[i]) <= tolerance) return false;
    return true;
}
inline bool Crowded(const Place::Box& box, const std::vector<Prop>& props, double spacing)
{
    for (const auto& prop : props) if (Gap(box, prop.box) < spacing - 1e-6) return true;
    return false;
}
// Whether the box's whole footprint is inside the area.
inline bool Fits(const Area& area, const Place::Box& box)
{
    return Inside(area, { box.lo[0], box.lo[1] }) && Inside(area, { box.hi[0], box.lo[1] })
        && Inside(area, { box.lo[0], box.hi[1] }) && Inside(area, { box.hi[0], box.hi[1] })
        && Inside(area, { (box.lo[0] + box.hi[0]) / 2, (box.lo[1] + box.hi[1]) / 2 });
}

// Rays that find a wall or another object inside a prop at rest: level, from
// its centre out to 90% of its reach in eight directions, at half its height
// and again just above the floor (raised by how far a slope climbs along that
// ray), and one straight up from the floor to its top. floor is the floor
// point under the box's centre and normal the floor's there.
struct Probe { Vector start{}, direction{}; double reach = 0; };
inline std::vector<Probe> Probes(const Place::Box& box, const Vector& floor, const Vector& normal)
{
    const Vector centre = Place::Centre(box);
    Vector n{ 0, 0, 1 };
    if (Place::Length(normal) > 0.5 && normal[2] > 0.1) n = Place::Unit(normal);
    std::vector<Probe> probes;
    for (int k = 0; k < 8; ++k)
    {
        const double angle = k * Place::kPi / 4;
        const Vector d{ std::cos(angle), std::sin(angle), 0 };
        const double reach = Place::Support(box, d) * 0.9;
        if (reach < 1) continue;
        probes.push_back({ centre, d, reach });
        const double climb = std::max(0.0, -(n[0] * d[0] + n[1] * d[1]) / n[2] * reach);
        const double low = floor[2] + 4 + climb;
        if (low < centre[2] - 1) probes.push_back({ { centre[0], centre[1], low }, d, reach });
    }
    const double up = box.hi[2] - (floor[2] + 4);
    if (up > 1) probes.push_back({ { centre[0], centre[1], floor[2] + 4 }, { 0, 0, 1 }, up });
    return probes;
}
// Whether a prop at rest stands on something: of Drop to Floor's five rays
// (DropStarts, the centre first) and how far each went before it hit, the
// centre's and at least two others' must find the floor within a few units of
// the prop's bottom. A prop perched on a rail or hanging over a ledge has
// floor under one or two of them only. A prop turned to sit flat on a slope
// is flush with it, so its rays all find the floor at or above its bottom.
inline bool Supported(double bottom, const std::vector<Vector>& starts, const std::vector<std::optional<double>>& hits, double tolerance = 8)
{
    if (starts.empty() || starts.size() != hits.size()) return false;
    auto under = [&](size_t i) { return hits[i] && std::isfinite(*hits[i]) && *hits[i] > 0.5 && starts[i][2] - *hits[i] >= bottom - tolerance; };
    if (!under(0)) return false;
    int count = 0;
    for (size_t i = 0; i < starts.size(); ++i) count += under(i) ? 1 : 0;
    return count >= std::min<int>(3, static_cast<int>(starts.size()));
}
// Whether a probe hit something before the end of its reach.
inline bool Blocked(const Probe& probe, const std::optional<double>& hit)
{
    return hit && std::isfinite(*hit) && *hit < probe.reach;
}

// What the drop made of a candidate: the prop at rest, or why it has none.
enum class Miss { None, NoFloor, Wall };
struct Landing { Miss miss = Miss::None; Prop prop; };
using Drop = std::function<Landing(const Candidate&)>;

struct Result
{
    std::vector<Prop> props;
    int target = 0, tried = 0;
    // Left out: no floor, in a wall, over the area's edge, too close to a
    // prop placed before it, on a mesh already in the map.
    int noFloor = 0, walls = 0, outside = 0, crowded = 0, blocked = 0;
};
// Candidates until the target is reached or the tries run out. A candidate
// whose footprint already clashes is turned away before its drop; after it,
// a prop that leaves the area, sits too close to one placed before it or
// overlaps one of the obstacles (the small meshes already there) is too.
inline Result Run(const Settings& s, const std::vector<Item>& items, const Area& area, const std::vector<Place::Box>& obstacles, const Drop& drop)
{
    Validate(s, items);
    Result result;
    result.target = Target(s, area);
    Random random(s.seed);
    const int tries = result.target * kTriesPerProp;
    while (static_cast<int>(result.props.size()) < result.target && result.tried < tries)
    {
        ++result.tried;
        const auto candidate = Sample(random, s, items, area);
        if (!candidate) break;
        const auto before = CandidateBox(*candidate, items);
        if (Crowded(before, result.props, s.spacing)) { ++result.crowded; continue; }
        if (!Fits(area, before)) { ++result.outside; continue; }
        const auto landing = drop(*candidate);
        if (landing.miss == Miss::NoFloor) { ++result.noFloor; continue; }
        if (landing.miss == Miss::Wall) { ++result.walls; continue; }
        const auto& box = landing.prop.box;
        if (!Fits(area, box)) { ++result.outside; continue; }
        if (Crowded(box, result.props, s.spacing)) { ++result.crowded; continue; }
        bool clash = false;
        for (size_t i = 0; i < obstacles.size() && !clash; ++i) clash = Overlaps(box, obstacles[i]);
        if (clash) { ++result.blocked; continue; }
        result.props.push_back(landing.prop);
    }
    return result;
}

// A new seed for Re-roll, from any number (a clock, the old seed): never 0.
inline uint64_t NextSeed(uint64_t from)
{
    Random random(from ^ 0xA5A5A5A5u);
    const uint64_t seed = random.Next() % 1000000000ull;
    return seed ? seed : 1;
}
}
