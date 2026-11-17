#pragma once
#include "LocalLightingMatch.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Light and Shadow map: how lit each patch of walkable floor is, as an
// estimate of where a spy can hide. Pure maths, no editor calls; the native
// reads are in LightShadowNative.inl and the drawing in LightShadowMap.cpp.
//
// What it estimates, and what it does not:
// - Floors: walkable BSP floor, and the walkable tops of static meshes a
//   player collides with (their render triangles).
// - Baked: the BSP floor's own lightmap texel at the patch (Build Lighting's
//   static lights and zone ambient), as luminance. UE2 lightmaps are drawn at
//   double brightness (modulate 2x), so a stored 128 reads as 255 on screen.
//   A static mesh floor has no lightmap: its baked value is worked out from
//   the static lights Build Lighting found reaching each of its vertices (the
//   mesh instance's visibility bits), with the engine's falloff below. Zone
//   ambient is not added there.
// - Lights: the in-game and dynamic lights (the ones the game draws on actors,
//   the Lighting Budget's "in game" set) reaching a point at spy chest height
//   above the patch, by the editor's own FDynamicLight::SampleIntensity
//   (0x111AED50): 2 x (1 - 3x^2 + 2x^3) of the light's brightness and colour,
//   x the share of its reach, with the LightEffect variants (NonIncidence,
//   QuadraticNonIncidence, Cylinder, Sunlight). Spotlights (LE_Spotlight,
//   LE_StaticSpot) light only inside LightCone: Sine = 1 - LightCone/256 and
//   ((cos - Sine) / (1 - Sine))^2 inside it. LE_ESpotShadow, the game's
//   shadow-casting projector light, lights only inside its frustum: EchelonRange
//   ahead, SpotWidth by SpotHeight at the far end, narrowing to NearPlaneRatio
//   of that at the lamp (the frustum its render data builds, 0x111A5480).
//   A light counts only when neither the BSP nor a drawn static mesh blocks
//   the line to it (a small mesh around the lamp is its fitting and does not).
//   Light strength is on the same scale as the doubled lightmaps: a light of
//   brightness 64 gives 128 right beside it.
// - Not taken into account: movers, projectors, light animation (flicker and
//   pulses count at full brightness), each actor's MaxLights cap, and the
//   game's own spy visibility formula, which is not known. The bands are a
//   guide for where to look, not what a merc will see. Playtest to confirm.
namespace LightShadow
{
using Point = LocalLightingMatch::Point;

enum class Source { Combined = 0, Lights = 1, Baked = 2 };
enum class Band { Hidden = 0, Partial = 1, Exposed = 2 };

inline const char* SourceName(Source source)
{
    switch (source)
    {
    case Source::Lights: return "in-game lights only";
    case Source::Baked: return "baked lighting only";
    default: return "brighter of baked lighting and in-game lights";
    }
}
inline const char* BandName(Band band)
{
    switch (band)
    {
    case Band::Hidden: return "hidden";
    case Band::Partial: return "partly visible";
    default: return "exposed";
    }
}

// [LightShadowMap] in Reloaded_Editor.ini. Brightness runs 0 (black) to 255.
struct Settings
{
    int hiddenBelow = 40;    // darker than this: good spy cover
    int exposedFrom = 100;   // this bright or more: exposed
    int cell = 64;           // floor patch size in units
    Source source = Source::Combined;
    int chest = 48;          // height above the floor lights are measured at
    bool meshShadows = true; // static meshes block lights
    bool meshFloors = true;  // static meshes players stand on get patches
    bool spotCones = true;   // spotlights light only inside their cone or frustum
};
inline Settings Clamp(Settings s)
{
    s.hiddenBelow = std::clamp(s.hiddenBelow, 1, 254);
    s.exposedFrom = std::clamp(s.exposedFrom, s.hiddenBelow + 1, 255);
    s.cell = std::clamp(s.cell, 16, 512);
    if (s.source != Source::Lights && s.source != Source::Baked) s.source = Source::Combined;
    s.chest = std::clamp(s.chest, 0, 160);
    return s;
}
inline Band Classify(double value, const Settings& s)
{
    if (!(value >= s.hiddenBelow)) return Band::Hidden; // NaN counts as dark
    return value >= s.exposedFrom ? Band::Exposed : Band::Partial;
}

// A lightmap atlas as the editor stores it: TEXF_RGBA8, B,G,R,A in memory.
struct Image
{
    int width = 0, height = 0;
    std::vector<uint32_t> pixels;
};
inline double Luminance(uint32_t bgra)
{
    const double b = bgra & 255, g = (bgra >> 8) & 255, r = (bgra >> 16) & 255;
    return 0.299 * r + 0.587 * g + 0.114 * b;
}
// Bilinear luminance at a lightmap UV (0..1 across the atlas), texel centres
// at half-texel offsets, clamped at the edges.
inline double SampleLuminance(const Image& image, double u, double v)
{
    if (image.width <= 0 || image.height <= 0 || image.pixels.size() < size_t(image.width) * image.height) return -1;
    if (!std::isfinite(u) || !std::isfinite(v)) return -1;
    const double x = std::clamp(u * image.width - 0.5, 0.0, double(image.width - 1));
    const double y = std::clamp(v * image.height - 0.5, 0.0, double(image.height - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = (std::min)(x0 + 1, image.width - 1), y1 = (std::min)(y0 + 1, image.height - 1);
    const double fx = x - x0, fy = y - y0;
    auto at = [&](int px, int py) { return Luminance(image.pixels[size_t(py) * image.width + px]); };
    const double top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx;
    const double bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
    return top * (1 - fy) + bottom * fy;
}
// Stored lightmap luminance to on-screen brightness (modulate 2x).
inline double BakedBrightness(double stored) { return stored < 0 ? -1 : (std::min)(255.0, stored * 2); }

// UE2's light colour (FGetHSV's hue and saturation; saturation 255 is white),
// as the share of white's luminance it keeps: a pure blue light is dimmer.
inline double ColourShare(int hue, int saturation)
{
    hue = std::clamp(hue, 0, 255);
    const double s = std::clamp(saturation, 0, 255) / 255.0;
    double r, g, b;
    if (hue < 86) { r = (85 - hue) / 85.0; g = hue / 85.0; b = 0; }
    else if (hue < 171) { r = 0; g = (170 - hue) / 85.0; b = (hue - 85) / 85.0; }
    else { r = (hue - 170) / 85.0; g = 0; b = (255 - hue) / 84.0; }
    r += s * (1 - r); g += s * (1 - g); b += s * (1 - b);
    return std::clamp(0.299 * r + 0.587 * g + 0.114 * b, 0.0, 1.0);
}

inline Point Cross(const Point& a, const Point& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline bool Same(const Point& a, const Point& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// ELightEffect values in this game's Engine.Actor (LE_ESpotShadow is its own).
namespace Effect
{
constexpr int StaticSpot = 8, Spotlight = 12, NonIncidence = 13, Cylinder = 17, Sunlight = 19, QuadraticNonIncidence = 20,
              ESpotShadow = 22;
}
// How far a sunlight's line of sight is followed back towards the sky.
constexpr double kSunReach = 32768;

struct Light
{
    Point at;               // the light actor's Location
    double radius = 0;      // reach in units (the in-game render radius)
    double brightness = 0;  // LightBrightness 0..255 times ColourShare
    int effect = 0;         // LightEffect
    int cone = 128;         // LightCone, for the spotlight effects
    Point forward{ 1, 0, 0 }, right{ 0, 1, 0 }, up{ 0, 0, 1 }; // from Rotation
    // LE_ESpotShadow's frustum: EchelonRange, SpotWidth, SpotHeight, NearPlaneRatio.
    double range = 0, spotWidth = 0, spotHeight = 0, nearRatio = 0.1;
    uint64_t id = 0;        // the actor, for the incremental cache
};
inline bool SameLight(const Light& a, const Light& b)
{
    return a.id == b.id && Same(a.at, b.at) && a.radius == b.radius && a.brightness == b.brightness && a.effect == b.effect
        && a.cone == b.cone && Same(a.forward, b.forward) && Same(a.right, b.right) && Same(a.up, b.up) && a.range == b.range
        && a.spotWidth == b.spotWidth && a.spotHeight == b.spotHeight && a.nearRatio == b.nearRatio;
}
inline bool IsSpot(const Light& light) { return light.effect == Effect::Spotlight || light.effect == Effect::StaticSpot; }

// Inside an LE_ESpotShadow light's frustum: ahead of the lamp, no further than
// EchelonRange, and within SpotWidth (to the side) by SpotHeight (up), scaled
// from NearPlaneRatio at the lamp to the full size at EchelonRange. A ratio of
// 0.99 or more is the box the engine uses then. No range: no frustum.
inline bool InsideFrustum(const Light& light, const Point& p)
{
    if (!(light.range > 0)) return true;
    const Point d = p - light.at;
    const double depth = Dot(d, light.forward);
    if (!(depth >= 0) || depth > light.range) return false;
    const double ratio = std::clamp(light.nearRatio, 0.0, 1.0);
    const double scale = ratio < 0.99 ? ratio + (1 - ratio) * depth / light.range : 1;
    return std::abs(Dot(d, light.right)) <= light.spotWidth * scale && std::abs(Dot(d, light.up)) <= light.spotHeight * scale;
}
// The spotlight cone factor: zero outside LightCone, rising as the square of
// the share of the way in from its edge to the axis.
inline double ConeFactor(const Light& light, const Point& p)
{
    const double sine = 1 - std::clamp(light.cone, 0, 255) / 256.0;
    const Point d = p - light.at;
    const double distance = std::sqrt(Dot(d, d));
    const double cosine = distance < 1e-6 ? 1 : Dot(d, light.forward) / distance;
    if (!(cosine > sine)) return 0;
    const double k = (cosine - sine) / (1 - sine);
    return k * k;
}
// The engine's intensity of one light at a point, before brightness and line
// of sight (SampleIntensity, 2 at full). With a surface normal the light's
// angle to the surface counts as the engine counts it; without one the point
// faces the light (spy chest height).
inline double Intensity(const Light& light, const Point& p, bool cones = true, const Point* normal = nullptr)
{
    if (light.effect == Effect::Sunlight)
    {
        if (!normal) return 2;
        const double d = Dot(*normal, light.forward);
        return d < 0 ? -2 * d : 0;
    }
    if (!(light.radius > 0)) return 0;
    const Point toLight = light.at - p;
    const double distance = std::sqrt(Dot(toLight, toLight));
    if (!std::isfinite(distance) || distance >= light.radius) return 0;
    const double x = distance / light.radius;
    double facing = 1;
    if (normal && distance > 1e-6) facing = Dot(toLight, *normal) / distance;
    double value;
    switch (light.effect)
    {
    case Effect::NonIncidence:
        if (!(facing > 0)) return 0;
        value = 2 * std::sqrt((std::max)(0.0, 1.02 - x));
        break;
    case Effect::QuadraticNonIncidence:
        if (!(facing > 0)) return 0;
        value = 2 * (1.02 - x * x);
        break;
    case Effect::Cylinder:
        value = 2 * (std::max)(0.0, 1 - (toLight.x * toLight.x + toLight.y * toLight.y) / (light.radius * light.radius));
        break;
    default:
        if (!(facing > 0)) return 0;
        value = 2 * facing * (1 - 3 * x * x + 2 * x * x * x);
    }
    if (cones)
    {
        if (IsSpot(light)) value *= ConeFactor(light, p);
        else if (light.effect == Effect::ESpotShadow && !InsideFrustum(light, p)) return 0;
    }
    return (std::max)(0.0, value);
}
// One light's brightness at a point (0..255 scale, before the cap), before
// line of sight.
inline double LightAt(const Light& light, const Point& p, bool cones = true)
{
    if (!(light.brightness > 0)) return 0;
    return light.brightness * Intensity(light, p, cones);
}

// ---- Static mesh geometry ---------------------------------------------------

struct Box
{
    Point lo{ 1e300, 1e300, 1e300 }, hi{ -1e300, -1e300, -1e300 };
    void Add(const Point& p)
    {
        lo = { (std::min)(lo.x, p.x), (std::min)(lo.y, p.y), (std::min)(lo.z, p.z) };
        hi = { (std::max)(hi.x, p.x), (std::max)(hi.y, p.y), (std::max)(hi.z, p.z) };
    }
    void Add(const Box& b) { if (b.Valid()) { Add(b.lo); Add(b.hi); } }
    bool Valid() const { return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z; }
    bool Contains(const Point& p, double margin = 0) const
    {
        return p.x >= lo.x - margin && p.x <= hi.x + margin && p.y >= lo.y - margin && p.y <= hi.y + margin && p.z >= lo.z - margin && p.z <= hi.z + margin;
    }
    // Within distance of a point (a light's sphere touching the box).
    bool Near(const Point& p, double distance) const
    {
        if (!Valid()) return false;
        const double dx = (std::max)({ lo.x - p.x, 0.0, p.x - hi.x }), dy = (std::max)({ lo.y - p.y, 0.0, p.y - hi.y }), dz = (std::max)({ lo.z - p.z, 0.0, p.z - hi.z });
        return dx * dx + dy * dy + dz * dz <= distance * distance;
    }
    Point Centre() const { return (lo + hi) * 0.5; }
};
inline bool SameBox(const Box& a, const Box& b) { return Same(a.lo, b.lo) && Same(a.hi, b.hi); }

// The segment from o to o + d (t from 0 to 1) against a box: the slab test.
inline bool SegmentHitsBox(const float lo[3], const float hi[3], const Point& o, const Point& inverse, double tFar = 1)
{
    double tNear = 0;
    const double origin[3] = { o.x, o.y, o.z }, inv[3] = { inverse.x, inverse.y, inverse.z };
    for (int axis = 0; axis < 3; ++axis)
    {
        double t0 = (lo[axis] - origin[axis]) * inv[axis], t1 = (hi[axis] - origin[axis]) * inv[axis];
        if (std::isnan(t0) || std::isnan(t1))
        {
            // Parallel to this slab and on its plane: inside only if within it.
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) return false;
            continue;
        }
        if (t0 > t1) std::swap(t0, t1);
        tNear = (std::max)(tNear, t0);
        tFar = (std::min)(tFar, t1);
        if (tNear > tFar) return false;
    }
    return true;
}
inline Point Inverse(const Point& d)
{
    auto one = [](double v) { return v == 0 ? (std::signbit(v) ? -1e300 : 1e300) : 1 / v; };
    return { one(d.x), one(d.y), one(d.z) };
}

// A bounding volume hierarchy over boxes, built by splitting at the median
// along the widest spread of their centres.
struct Bvh
{
    struct Node
    {
        float lo[3], hi[3];
        int32_t left = -1, right = -1; // children; -1 for a leaf
        uint32_t first = 0, count = 0; // a leaf's items
    };
    std::vector<Node> nodes;
    std::vector<uint32_t> items;
};
inline Bvh BuildBvh(const std::vector<Box>& boxes, uint32_t leaf = 4)
{
    Bvh bvh;
    bvh.items.reserve(boxes.size());
    for (uint32_t i = 0; i < boxes.size(); ++i) if (boxes[i].Valid()) bvh.items.push_back(i);
    if (bvh.items.empty()) return bvh;
    std::vector<Point> centres(boxes.size());
    for (auto i : bvh.items) centres[i] = boxes[i].Centre();
    struct Job { int32_t node; uint32_t first, count; };
    std::vector<Job> jobs{ { 0, 0, uint32_t(bvh.items.size()) } };
    bvh.nodes.emplace_back();
    while (!jobs.empty())
    {
        const Job job = jobs.back();
        jobs.pop_back();
        Box box, spread;
        for (uint32_t k = job.first; k < job.first + job.count; ++k)
        {
            box.Add(boxes[bvh.items[k]]);
            spread.Add(centres[bvh.items[k]]);
        }
        auto& node = bvh.nodes[job.node];
        node.lo[0] = float(box.lo.x); node.lo[1] = float(box.lo.y); node.lo[2] = float(box.lo.z);
        node.hi[0] = float(box.hi.x); node.hi[1] = float(box.hi.y); node.hi[2] = float(box.hi.z);
        // Floats round inwards; widen a hair so nothing on the edge is missed.
        for (int a = 0; a < 3; ++a)
        {
            node.lo[a] = std::nextafter(node.lo[a], -1e30f) - 1e-3f * (std::abs(node.lo[a]) * 1e-4f + 1);
            node.hi[a] = std::nextafter(node.hi[a], 1e30f) + 1e-3f * (std::abs(node.hi[a]) * 1e-4f + 1);
        }
        if (job.count <= leaf)
        {
            node.first = job.first;
            node.count = job.count;
            continue;
        }
        const Point extent = spread.hi - spread.lo;
        const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : extent.y >= extent.z ? 1 : 2;
        auto key = [&](uint32_t i) { return axis == 0 ? centres[i].x : axis == 1 ? centres[i].y : centres[i].z; };
        const uint32_t half = job.count / 2;
        auto begin = bvh.items.begin() + job.first;
        std::nth_element(begin, begin + half, begin + job.count, [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
        const int32_t left = int32_t(bvh.nodes.size());
        bvh.nodes.emplace_back();
        bvh.nodes.emplace_back();
        bvh.nodes[job.node].left = left;
        bvh.nodes[job.node].right = left + 1;
        jobs.push_back({ left, job.first, half });
        jobs.push_back({ left + 1, job.first + half, job.count - half });
    }
    return bvh;
}
// Calls hit(item) for every item whose box the segment o..o+d crosses, until
// one returns true.
template<class Hit> bool AnyHit(const Bvh& bvh, const Point& o, const Point& d, Hit&& hit)
{
    if (bvh.nodes.empty()) return false;
    const Point inverse = Inverse(d);
    int32_t stack[96];
    int top = 0;
    stack[top++] = 0;
    while (top)
    {
        const auto& node = bvh.nodes[stack[--top]];
        if (!SegmentHitsBox(node.lo, node.hi, o, inverse)) continue;
        if (node.left < 0)
        {
            for (uint32_t k = node.first; k < node.first + node.count; ++k)
                if (hit(bvh.items[k])) return true;
            continue;
        }
        if (top + 2 > 96) return false; // deeper than any balanced tree here
        stack[top++] = node.left;
        stack[top++] = node.right;
    }
    return false;
}
// Whether the segment o..o+d crosses a triangle (either side), strictly
// between its ends.
inline bool SegmentHitsTriangle(const Point& o, const Point& d, const Point& a, const Point& b, const Point& c)
{
    const Point e1 = b - a, e2 = c - a;
    const Point p = Cross(d, e2);
    const double det = Dot(e1, p);
    if (std::abs(det) < 1e-12) return false;
    const double inv = 1 / det;
    const Point s = o - a;
    const double u = Dot(s, p) * inv;
    if (u < 0 || u > 1) return false;
    const Point q = Cross(s, e1);
    const double v = Dot(d, q) * inv;
    if (v < 0 || u + v > 1) return false;
    const double t = Dot(e2, q) * inv;
    return t > 1e-6 && t < 1 - 1e-6;
}

struct Vec3f { float x = 0, y = 0, z = 0; };
inline Point ToPoint(const Vec3f& v) { return { v.x, v.y, v.z }; }

// One static mesh's render triangles in its own space, shared by every actor
// that draws it. Triangles are wound so their face normal agrees with the
// mesh's vertex normals (outwards).
struct MeshShape
{
    uint64_t key = 0;
    std::vector<Vec3f> points, normals;
    std::vector<std::array<uint32_t, 3>> triangles;
    Box bounds;
    Bvh bvh;
};
// Bounds and hierarchy, once the points and triangles are in; triangles with
// a corner out of range are dropped.
inline void Finish(MeshShape& shape)
{
    shape.triangles.erase(std::remove_if(shape.triangles.begin(), shape.triangles.end(), [&](const std::array<uint32_t, 3>& t) {
        return t[0] >= shape.points.size() || t[1] >= shape.points.size() || t[2] >= shape.points.size(); }), shape.triangles.end());
    if (shape.normals.size() == shape.points.size())
        for (auto& t : shape.triangles)
        {
            const Point a = ToPoint(shape.points[t[0]]), b = ToPoint(shape.points[t[1]]), c = ToPoint(shape.points[t[2]]);
            const Point n = ToPoint(shape.normals[t[0]]) + ToPoint(shape.normals[t[1]]) + ToPoint(shape.normals[t[2]]);
            if (Dot(Cross(b - a, c - a), n) < 0) std::swap(t[1], t[2]);
        }
    shape.bounds = Box{};
    std::vector<Box> boxes(shape.triangles.size());
    for (size_t i = 0; i < shape.triangles.size(); ++i)
        for (auto corner : shape.triangles[i])
        {
            boxes[i].Add(ToPoint(shape.points[corner]));
            shape.bounds.Add(ToPoint(shape.points[corner]));
        }
    shape.bvh = BuildBvh(boxes);
}
inline bool SegmentHitsShape(const MeshShape& shape, const Point& o, const Point& d)
{
    return AnyHit(shape.bvh, o, d, [&](uint32_t i) {
        const auto& t = shape.triangles[i];
        return SegmentHitsTriangle(o, d, ToPoint(shape.points[t[0]]), ToPoint(shape.points[t[1]]), ToPoint(shape.points[t[2]]));
    });
}

// An affine transform: world = m * (x, y, z, 1).
struct Affine
{
    double m[3][4]{ { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    Point Apply(const Point& p) const
    {
        return { m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3], m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
                 m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3] };
    }
    Point Turn(const Point& d) const
    {
        return { m[0][0] * d.x + m[0][1] * d.y + m[0][2] * d.z, m[1][0] * d.x + m[1][1] * d.y + m[1][2] * d.z, m[2][0] * d.x + m[2][1] * d.y + m[2][2] * d.z };
    }
    double Determinant() const
    {
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    }
};
inline bool SameAffine(const Affine& a, const Affine& b) { return std::memcmp(a.m, b.m, sizeof(a.m)) == 0; }
// The transform that takes a point's origin and three axes to where they go.
inline Affine FromAxes(const Point& origin, const Point& x, const Point& y, const Point& z)
{
    Affine a;
    const Point column[3] = { x, y, z };
    for (int c = 0; c < 3; ++c) { a.m[0][c] = column[c].x; a.m[1][c] = column[c].y; a.m[2][c] = column[c].z; }
    a.m[0][3] = origin.x; a.m[1][3] = origin.y; a.m[2][3] = origin.z;
    return a;
}
// false when the transform flattens space (a zero scale).
inline bool Invert(const Affine& a, Affine& out)
{
    const double det = a.Determinant();
    if (!(std::abs(det) > 1e-12) || !std::isfinite(det)) return false;
    const auto& m = a.m;
    double r[3][3];
    r[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det;
    r[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    r[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    r[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det;
    r[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    r[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    r[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det;
    r[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    r[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j) out.m[i][j] = r[i][j];
        out.m[i][3] = -(r[i][0] * m[0][3] + r[i][1] * m[1][3] + r[i][2] * m[2][3]);
    }
    return true;
}

// One actor drawing a static mesh.
struct MeshInstance
{
    uint64_t id = 0;          // the actor
    size_t shape = 0;         // into Scene::shapes
    Affine toWorld, toLocal;
    Box box;                  // world bounds
    bool blocks = true;       // drawn: casts shadow
    bool floor = false;       // players collide with it: can be stood on
    std::vector<float> shade; // baked brightness per shape vertex; empty when not built
};
// Fills the inverse and world bounds; false when the transform is unusable.
inline bool Place(MeshInstance& instance, const MeshShape& shape)
{
    if (!Invert(instance.toWorld, instance.toLocal) || !shape.bounds.Valid()) return false;
    instance.box = Box{};
    for (int corner = 0; corner < 8; ++corner)
        instance.box.Add(instance.toWorld.Apply({ corner & 1 ? shape.bounds.hi.x : shape.bounds.lo.x, corner & 2 ? shape.bounds.hi.y : shape.bounds.lo.y,
                                                  corner & 4 ? shape.bounds.hi.z : shape.bounds.lo.z }));
    return instance.box.Valid() && std::isfinite(instance.box.lo.x + instance.box.hi.x + instance.box.lo.y + instance.box.hi.y + instance.box.lo.z + instance.box.hi.z);
}

// A walkable floor triangle with its lightmap UVs; atlas -1 when the surface
// has no lightmap (not built, unlit, or a static mesh). A static mesh floor
// carries its corners' baked brightness instead (-1 when not built).
struct Floor
{
    Point a, b, c;
    double u[3]{}, v[3]{};
    int atlas = -1;
    double shade[3]{ -1, -1, -1 };
    bool mesh = false;
};
// Steeper than this is a wall or a ramp a spy cannot stand on (UE2's walkable
// floor normal).
constexpr double kWalkable = 0.7;

inline Point Normal(const Floor& f)
{
    const Point n = Cross(f.b - f.a, f.c - f.a);
    const double length = std::sqrt(Dot(n, n));
    if (!(length > 1e-9)) return {};
    return n * (1 / length);
}
// Faces up, either winding: BSP render vertices wind either way.
inline bool Walkable(const Floor& f) { return std::abs(Normal(f).z) >= kWalkable; }

struct Scene
{
    std::vector<Floor> floors;
    std::vector<std::shared_ptr<const Image>> atlases;
    std::vector<Light> lights;
    std::vector<LocalLightingMatch::BspNode> bsp; // empty: no line-of-sight test
    bool rootOutside = true;
    std::vector<std::shared_ptr<const MeshShape>> shapes;
    std::vector<MeshInstance> instances;
    // Built by Prepare: the instances that block light, and a hierarchy over them.
    std::vector<uint32_t> blockers;
    Bvh blockerBvh;
};

// The walkable, upward-facing triangles of the static meshes players stand on.
inline void AddMeshFloors(Scene& scene)
{
    for (const auto& instance : scene.instances)
    {
        if (!instance.floor || instance.shape >= scene.shapes.size() || !scene.shapes[instance.shape]) continue;
        const auto& shape = *scene.shapes[instance.shape];
        const bool mirrored = instance.toWorld.Determinant() < 0;
        const bool shaded = instance.shade.size() == shape.points.size();
        for (const auto& t : shape.triangles)
        {
            Floor f;
            f.a = instance.toWorld.Apply(ToPoint(shape.points[t[0]]));
            f.b = instance.toWorld.Apply(ToPoint(shape.points[t[1]]));
            f.c = instance.toWorld.Apply(ToPoint(shape.points[t[2]]));
            f.mesh = true;
            Point n = Normal(f);
            if (mirrored) n = n * -1;
            if (n.z < kWalkable) continue;
            if (shaded) for (int k = 0; k < 3; ++k) f.shade[k] = instance.shade[t[k]];
            scene.floors.push_back(f);
        }
    }
}
// Before sampling: the light-blocking instances and their hierarchy.
inline void Prepare(Scene& scene)
{
    scene.blockers.clear();
    std::vector<Box> boxes;
    for (uint32_t i = 0; i < scene.instances.size(); ++i)
    {
        const auto& instance = scene.instances[i];
        if (!instance.blocks || instance.shape >= scene.shapes.size() || !scene.shapes[instance.shape] || !instance.box.Valid()) continue;
        scene.blockers.push_back(i);
        boxes.push_back(instance.box);
    }
    scene.blockerBvh = BuildBvh(boxes, 2);
}

// The BSP line of sight without allocating (LocalLightingMatch::ClearSegment's
// walk, with a reused stack).
inline bool BspClear(const std::vector<LocalLightingMatch::BspNode>& nodes, bool rootOutside, const Point& a, const Point& b)
{
    if (nodes.empty()) return rootOutside;
    struct Part { int index; Point a, b; bool outside; };
    thread_local std::vector<Part> pending;
    pending.clear();
    pending.push_back({ 0, a, b, rootOutside });
    size_t visited = 0;
    while (!pending.empty())
    {
        const auto part = pending.back();
        pending.pop_back();
        if (++visited > nodes.size() * 4 + 64) return false;
        if (part.index == -1) { if (!part.outside) return false; continue; }
        if (part.index < 0 || size_t(part.index) >= nodes.size()) return false;
        const auto& node = nodes[part.index];
        const double da = Dot(node.normal, part.a) - node.distance, db = Dot(node.normal, part.b) - node.distance;
        const bool fa = da >= 0, fb = db >= 0;
        auto next = [&](bool front, Point start, Point end) {
            pending.push_back({ front ? node.front : node.back, start, end, front ? (part.outside || node.csg) : (part.outside && !node.csg) });
        };
        if (fa == fb) next(fa, part.a, part.b);
        else
        {
            const auto middle = part.a + (part.b - part.a) * (da / (da - db));
            next(fa, part.a, middle);
            next(fb, middle, part.b);
        }
    }
    return true;
}
// A mesh this small around its lamp is the lamp's fitting (a shade, a
// housing), which would otherwise put every light out.
constexpr double kFittingSize = 384;
inline bool IsFitting(const Box& box, const Point& lamp)
{
    const Point size = box.hi - box.lo;
    return box.Contains(lamp, 1) && Dot(size, size) <= kFittingSize * kFittingSize;
}
// Whether a drawn static mesh crosses the segment a..b. The lamp's own
// fitting does not count.
inline bool MeshClear(const Scene& scene, const Point& a, const Point& b, const Point& lamp)
{
    if (scene.blockers.empty()) return true;
    return !AnyHit(scene.blockerBvh, a, b - a, [&](uint32_t item) {
        const auto& instance = scene.instances[scene.blockers[item]];
        if (IsFitting(instance.box, lamp)) return false;
        const Point la = instance.toLocal.Apply(a), lb = instance.toLocal.Apply(b);
        return SegmentHitsShape(*scene.shapes[instance.shape], la, lb - la);
    });
}

struct Cell
{
    Point at;            // on the floor, at the patch centre
    double baked = -1;   // on-screen brightness of the floor's baked lighting, -1 unknown
    double lights = 0;   // in-game lights at chest height
    double value = 0;    // what the band is decided by
    Band band = Band::Hidden;
    bool mesh = false;   // on a static mesh
};

// Whether a light reaches a point: neither the BSP nor (when asked) a static
// mesh blocks the line. The light end stops short, since lamps often sit
// inside their fitting's geometry. A sunlight is followed back to the sky.
inline bool Reaches(const Scene& scene, const Light& light, const Point& p, bool meshes = true)
{
    const Point target = light.effect == Effect::Sunlight ? p - light.forward * kSunReach : light.at;
    Point d = target - p;
    const double length = std::sqrt(Dot(d, d));
    if (length < 1e-6) return true;
    const double keep = (std::max)(0.0, length - (std::min)(16.0, length * 0.5));
    const Point end = p + d * (keep / length);
    if (!scene.bsp.empty() && !BspClear(scene.bsp, scene.rootOutside, p, end)) return false;
    if (meshes && !MeshClear(scene, p, end, light.at)) return false;
    return true;
}
inline double LightsAt(const Scene& scene, const Point& p, const Settings& settings = Settings{})
{
    double total = 0;
    for (const auto& light : scene.lights)
    {
        const double amount = LightAt(light, p, settings.spotCones);
        if (amount > 0 && Reaches(scene, light, p, settings.meshShadows)) total += amount;
    }
    return (std::min)(total, 255.0);
}
// A static mesh vertex's baked brightness from the static lights that reach
// it, with the light's angle to the surface.
inline double VertexShade(const std::vector<const Light*>& visible, const Point& p, const Point& normal, bool cones = true)
{
    double total = 0;
    for (const auto* light : visible)
        if (light && light->brightness > 0) total += light->brightness * Intensity(*light, p, cones, &normal);
    return (std::min)(total, 255.0);
}
inline double Combine(double baked, double lights, Source source)
{
    if (source == Source::Lights) return lights;
    if (source == Source::Baked) return baked;
    return (std::max)(baked, lights);
}

// Bands sampled patches (Sample's caller does this, and again for new
// thresholds or source on the same geometry).
// Patches without baked lighting count as dark in the baked-only view.
inline void ApplyBands(std::vector<Cell>& cells, Settings settings)
{
    settings = Clamp(settings);
    for (auto& cell : cells)
    {
        cell.value = Combine((std::max)(cell.baked, 0.0), cell.lights, settings.source);
        cell.band = Classify(cell.value, settings);
    }
}

struct Result
{
    std::vector<Cell> cells;
    bool truncated = false;
};
constexpr size_t kMaxCells = 200000;

// The patches of every walkable floor triangle on a world grid of
// settings.cell, with their baked brightness: one patch per grid square per
// floor height (stacked storeys each get theirs). In-game lights are added by
// Relight.
inline Result SampleCells(const Scene& scene, Settings settings)
{
    settings = Clamp(settings);
    Result result;
    const double size = settings.cell;
    // Grid square and height band to the patch already sampled there.
    struct Key { long long x, y, z; bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; } };
    struct Hash { size_t operator()(const Key& k) const { return size_t(k.x * 73856093LL) ^ size_t(k.y * 19349663LL) ^ size_t(k.z * 83492791LL); } };
    std::unordered_map<Key, size_t, Hash> seen;
    // Smallest floors first: when a map has more patches than the cap, the
    // ones left out are on its vast outer slabs rather than in its rooms.
    std::vector<std::pair<double, size_t>> order;
    order.reserve(scene.floors.size());
    for (size_t i = 0; i < scene.floors.size(); ++i)
    {
        const auto& f = scene.floors[i];
        if (f.mesh && !settings.meshFloors) continue;
        order.push_back({ std::abs((f.b.x - f.a.x) * (f.c.y - f.a.y) - (f.b.y - f.a.y) * (f.c.x - f.a.x)), i });
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [area, index] : order)
    {
        const auto& f = scene.floors[index];
        if (!Walkable(f)) continue;
        const double minX = (std::min)({ f.a.x, f.b.x, f.c.x }), maxX = (std::max)({ f.a.x, f.b.x, f.c.x });
        const double minY = (std::min)({ f.a.y, f.b.y, f.c.y }), maxY = (std::max)({ f.a.y, f.b.y, f.c.y });
        if (!std::isfinite(minX + maxX + minY + maxY) || maxX - minX > 1e6 || maxY - minY > 1e6) continue;
        // 2D barycentrics in XY.
        const double det = (f.b.y - f.c.y) * (f.a.x - f.c.x) + (f.c.x - f.b.x) * (f.a.y - f.c.y);
        if (std::abs(det) < 1e-9) continue;
        const long long x0 = (long long)std::ceil(minX / size - 0.5), x1 = (long long)std::floor(maxX / size - 0.5);
        const long long y0 = (long long)std::ceil(minY / size - 0.5), y1 = (long long)std::floor(maxY / size - 0.5);
        for (long long iy = y0; iy <= y1; ++iy)
            for (long long ix = x0; ix <= x1; ++ix)
            {
                const double x = (ix + 0.5) * size, y = (iy + 0.5) * size;
                const double wa = ((f.b.y - f.c.y) * (x - f.c.x) + (f.c.x - f.b.x) * (y - f.c.y)) / det;
                const double wb = ((f.c.y - f.a.y) * (x - f.c.x) + (f.a.x - f.c.x) * (y - f.c.y)) / det;
                const double wc = 1 - wa - wb;
                const double e = -1e-9;
                if (wa < e || wb < e || wc < e) continue;
                const double z = f.a.z * wa + f.b.z * wb + f.c.z * wc;
                const Key key{ ix, iy, (long long)std::floor(z / 32) };
                if (seen.count(key)) continue;
                if (result.cells.size() >= kMaxCells) { result.truncated = true; return result; }
                seen.emplace(key, result.cells.size());
                Cell cell;
                cell.at = { x, y, z };
                cell.mesh = f.mesh;
                if (f.atlas >= 0 && size_t(f.atlas) < scene.atlases.size() && scene.atlases[f.atlas])
                    cell.baked = BakedBrightness(SampleLuminance(*scene.atlases[f.atlas],
                        f.u[0] * wa + f.u[1] * wb + f.u[2] * wc, f.v[0] * wa + f.v[1] * wb + f.v[2] * wc));
                else if (f.shade[0] >= 0 && f.shade[1] >= 0 && f.shade[2] >= 0)
                    cell.baked = (std::min)(255.0, f.shade[0] * wa + f.shade[1] * wb + f.shade[2] * wc);
                result.cells.push_back(cell);
            }
    }
    return result;
}

// ---- In-game lights, kept between samples ------------------------------------
//
// Each light's share at each patch is kept. On the next sample a light that
// has not changed, whose reach no added, moved or removed static mesh touches,
// keeps its shares at the patches that are still where they were; only new
// patches and changed lights are worked out again. A BSP or setting change
// starts again from nothing.

inline uint64_t Mix(uint64_t hash, const void* data, size_t size)
{
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 1099511628211ULL; }
    return hash;
}
inline uint64_t BspHash(const Scene& scene)
{
    uint64_t hash = 1469598103934665603ULL;
    hash = Mix(hash, &scene.rootOutside, sizeof(scene.rootOutside));
    for (const auto& node : scene.bsp)
    {
        const double values[4] = { node.normal.x, node.normal.y, node.normal.z, node.distance };
        const int links[3] = { node.front, node.back, node.csg ? 1 : 0 };
        hash = Mix(hash, values, sizeof(values));
        hash = Mix(hash, links, sizeof(links));
    }
    return hash;
}

struct LightCache
{
    bool valid = false;
    int chest = -1;
    bool meshShadows = true, spotCones = true;
    uint64_t bsp = 0;
    struct Entry { Light light; std::vector<std::pair<uint32_t, float>> amounts; }; // by cell index, cells it lights
    std::vector<Entry> entries;
    std::vector<Point> cells;
    struct Placed { uint64_t id, shape; Affine toWorld; Box box; bool blocks; };
    std::vector<Placed> placed; // sorted by id
};
struct RelightStats
{
    size_t lights = 0, reused = 0, tested = 0, kept = 0; // tested: patch and light pairs worked out; kept: taken over
    bool full = true;
};

namespace Detail
{
struct PointKey { long long x, y, z; bool operator==(const PointKey& o) const { return x == o.x && y == o.y && z == o.z; } };
// Patch centres are multiples of the patch size: mixed well, or the map's
// power-of-two buckets fill a few and every lookup walks a long chain.
struct PointKeyHash
{
    size_t operator()(const PointKey& k) const
    {
        uint64_t h = uint64_t(k.x) * 0x9E3779B97F4A7C15ULL;
        h ^= uint64_t(k.y) + 0x632BE59BD9B4E019ULL + (h << 6) + (h >> 2);
        h ^= uint64_t(k.z) + 0x85EBCA77C2B2AE63ULL + (h << 6) + (h >> 2);
        h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL; h ^= h >> 33;
        return size_t(h ^ (h >> 32));
    }
};
inline PointKey KeyOf(const Point& p) { return { std::llround(p.x * 64), std::llround(p.y * 64), std::llround(p.z * 64) }; }
}

// Adds the in-game lights to sampled patches, reusing what the cache still
// holds, then bands them. threads 0: as many as the machine has, up to 8.
inline void Relight(const Scene& scene, std::vector<Cell>& cells, Settings settings, LightCache& cache, RelightStats* stats = nullptr, unsigned threads = 0)
{
    settings = Clamp(settings);
    RelightStats local;
    const uint64_t bsp = BspHash(scene);
    std::vector<LightCache::Placed> placed;
    placed.reserve(scene.instances.size());
    for (const auto& instance : scene.instances)
        placed.push_back({ instance.id, instance.shape < scene.shapes.size() && scene.shapes[instance.shape] ? scene.shapes[instance.shape]->key : 0,
                           instance.toWorld, instance.box, instance.blocks && settings.meshShadows });
    std::sort(placed.begin(), placed.end(), [](const auto& a, const auto& b) { return a.id < b.id; });

    bool full = !cache.valid || cache.chest != settings.chest || cache.meshShadows != settings.meshShadows || cache.spotCones != settings.spotCones || cache.bsp != bsp;
    // Where blocking meshes appeared, moved or went.
    std::vector<Box> changed;
    if (!full)
    {
        size_t i = 0, j = 0;
        auto differs = [](const LightCache::Placed& a, const LightCache::Placed& b) {
            return a.shape != b.shape || a.blocks != b.blocks || !SameAffine(a.toWorld, b.toWorld) || !SameBox(a.box, b.box); };
        while (i < cache.placed.size() || j < placed.size())
        {
            if (j >= placed.size() || (i < cache.placed.size() && cache.placed[i].id < placed[j].id)) { if (cache.placed[i].blocks) changed.push_back(cache.placed[i].box); ++i; }
            else if (i >= cache.placed.size() || placed[j].id < cache.placed[i].id) { if (placed[j].blocks) changed.push_back(placed[j].box); ++j; }
            else
            {
                if (differs(cache.placed[i], placed[j]) && (cache.placed[i].blocks || placed[j].blocks))
                {
                    if (cache.placed[i].blocks) changed.push_back(cache.placed[i].box);
                    if (placed[j].blocks) changed.push_back(placed[j].box);
                }
                ++i; ++j;
            }
        }
        if (changed.size() > 4096) full = true;
    }
    local.full = full;

    // The cells' old positions, and a grid to find the cells near a light.
    std::unordered_map<Detail::PointKey, uint32_t, Detail::PointKeyHash> before;
    if (!full)
    {
        before.reserve(cache.cells.size());
        for (uint32_t i = 0; i < cache.cells.size(); ++i) before.emplace(Detail::KeyOf(cache.cells[i]), i);
    }
    constexpr double kBucket = 256;
    std::unordered_map<Detail::PointKey, std::vector<uint32_t>, Detail::PointKeyHash> grid;
    auto bucket = [&](double x, double y, double z) { return Detail::PointKey{ (long long)std::floor(x / kBucket), (long long)std::floor(y / kBucket), (long long)std::floor(z / kBucket) }; };
    for (uint32_t i = 0; i < cells.size(); ++i) grid[bucket(cells[i].at.x, cells[i].at.y, cells[i].at.z + settings.chest)].push_back(i);
    std::vector<int> oldIndex(cells.size(), -1);
    if (!full)
        for (uint32_t i = 0; i < cells.size(); ++i)
            if (auto found = before.find(Detail::KeyOf(cells[i].at)); found != before.end()) oldIndex[i] = int(found->second);

    std::unordered_map<uint64_t, size_t> oldLight;
    if (!full) for (size_t i = 0; i < cache.entries.size(); ++i) oldLight.emplace(cache.entries[i].light.id, i);

    std::vector<LightCache::Entry> entries(scene.lights.size());
    std::vector<size_t> tested(scene.lights.size()), kept(scene.lights.size());
    std::vector<char> reused(scene.lights.size());
    auto work = [&](size_t index) {
        const auto& light = scene.lights[index];
        auto& entry = entries[index];
        entry.light = light;
        const LightCache::Entry* old = nullptr;
        if (!full)
            if (auto found = oldLight.find(light.id); found != oldLight.end() && SameLight(cache.entries[found->second].light, light))
            {
                old = &cache.entries[found->second];
                const bool sun = light.effect == Effect::Sunlight;
                for (const auto& box : changed)
                    if (sun || box.Near(light.at, light.radius)) { old = nullptr; break; }
            }
        reused[index] = old != nullptr;
        auto evaluate = [&](uint32_t i) {
            const Point p{ cells[i].at.x, cells[i].at.y, cells[i].at.z + settings.chest };
            if (old && oldIndex[i] >= 0)
            {
                ++kept[index];
                const auto found = std::lower_bound(old->amounts.begin(), old->amounts.end(), std::make_pair(uint32_t(oldIndex[i]), -1.0f));
                if (found != old->amounts.end() && found->first == uint32_t(oldIndex[i])) entry.amounts.push_back({ i, found->second });
                return;
            }
            const double amount = LightAt(light, p, settings.spotCones);
            if (!(amount > 0)) return;
            ++tested[index];
            if (Reaches(scene, light, p, settings.meshShadows)) entry.amounts.push_back({ i, float(amount) });
        };
        if (light.effect == Effect::Sunlight)
            for (uint32_t i = 0; i < cells.size(); ++i) evaluate(i);
        else if (light.radius > 0 && std::isfinite(light.radius))
        {
            const auto lo = bucket(light.at.x - light.radius, light.at.y - light.radius, light.at.z - light.radius);
            const auto hi = bucket(light.at.x + light.radius, light.at.y + light.radius, light.at.z + light.radius);
            const double spans = double(hi.x - lo.x + 1) * double(hi.y - lo.y + 1) * double(hi.z - lo.z + 1);
            if (spans > double(grid.size()))
            {
                for (const auto& [key, members] : grid)
                    if (key.x >= lo.x && key.x <= hi.x && key.y >= lo.y && key.y <= hi.y && key.z >= lo.z && key.z <= hi.z)
                        for (auto i : members) evaluate(i);
            }
            else
                for (long long z = lo.z; z <= hi.z; ++z)
                    for (long long y = lo.y; y <= hi.y; ++y)
                        for (long long x = lo.x; x <= hi.x; ++x)
                            if (auto found = grid.find({ x, y, z }); found != grid.end())
                                for (auto i : found->second) evaluate(i);
        }
        std::sort(entry.amounts.begin(), entry.amounts.end());
    };
    if (!threads) threads = (std::min)(8u, (std::max)(1u, std::thread::hardware_concurrency()));
    threads = (std::max)(1u, (std::min<unsigned>)(threads, unsigned(scene.lights.size())));
    if (threads <= 1) for (size_t i = 0; i < scene.lights.size(); ++i) work(i);
    else
    {
        std::vector<std::thread> pool;
        std::atomic_size_t next{ 0 };
        for (unsigned t = 0; t < threads; ++t)
            pool.emplace_back([&] { for (size_t i; (i = next++) < scene.lights.size();) work(i); });
        for (auto& thread : pool) thread.join();
    }

    for (auto& cell : cells) cell.lights = 0;
    for (size_t l = 0; l < entries.size(); ++l)
    {
        for (const auto& [i, amount] : entries[l].amounts) cells[i].lights += amount;
        local.tested += tested[l];
        local.kept += kept[l];
        local.reused += reused[l] ? 1 : 0;
    }
    for (auto& cell : cells) cell.lights = (std::min)(cell.lights, 255.0);
    local.lights = scene.lights.size();
    ApplyBands(cells, settings);

    cache.valid = true;
    cache.chest = settings.chest;
    cache.meshShadows = settings.meshShadows;
    cache.spotCones = settings.spotCones;
    cache.bsp = bsp;
    cache.entries = std::move(entries);
    cache.cells.resize(cells.size());
    for (size_t i = 0; i < cells.size(); ++i) cache.cells[i] = cells[i].at;
    cache.placed = std::move(placed);
    if (stats) *stats = local;
}

// Samples every walkable floor and lights it, from nothing.
inline Result Sample(const Scene& scene, Settings settings)
{
    auto result = SampleCells(scene, settings);
    LightCache cache;
    Relight(scene, result.cells, settings, cache, nullptr, 1);
    return result;
}
struct Summary
{
    std::array<size_t, 3> bands{};
    size_t total = 0, unbaked = 0, mesh = 0;
};
inline Summary Summarise(const std::vector<Cell>& cells)
{
    Summary s;
    for (const auto& cell : cells)
    {
        ++s.bands[size_t(cell.band)];
        ++s.total;
        s.unbaked += cell.baked < 0;
        s.mesh += cell.mesh;
    }
    return s;
}
inline int Percent(size_t part, size_t total)
{
    return total ? int((part * 100 + total / 2) / total) : 0;
}

// The brightness below which a share (0..1) of the patches fall, for the log
// and for choosing thresholds.
inline double Percentile(const std::vector<Cell>& cells, double share)
{
    if (cells.empty()) return 0;
    std::vector<double> values;
    values.reserve(cells.size());
    for (const auto& cell : cells) values.push_back(cell.value);
    const size_t at = (std::min)(values.size() - 1, size_t(std::clamp(share, 0.0, 1.0) * double(values.size() - 1) + 0.5));
    std::nth_element(values.begin(), values.begin() + at, values.end());
    return values[at];
}

// The legend drawn in the viewports and shown in the settings window.
inline std::vector<std::string> Legend(const Summary& s, const Settings& settings, bool truncated, bool stale)
{
    std::vector<std::string> lines;
    const char* source = settings.source == Source::Lights ? "in-game lights" : settings.source == Source::Baked ? "baked lighting" : "baked + in-game lights";
    lines.push_back("Light and Shadow (" + std::string(source) + "): " + std::to_string(s.total) + " floor patches of "
        + std::to_string(settings.cell) + " uu" + (s.mesh ? ", " + std::to_string(s.mesh) + " on static meshes" : std::string())
        + (stale ? ", map changed, refreshing" : ""));
    lines.push_back("Blue  hidden, darker than " + std::to_string(settings.hiddenBelow) + ": " + std::to_string(Percent(s.bands[0], s.total)) + "%");
    lines.push_back("Yellow  partly visible: " + std::to_string(Percent(s.bands[1], s.total)) + "%");
    lines.push_back("Red  exposed, " + std::to_string(settings.exposedFrom) + " or brighter: " + std::to_string(Percent(s.bands[2], s.total)) + "%");
    if (!s.total) lines.push_back(settings.meshFloors ? "No walkable floor found: build the geometry." : "No walkable BSP floor found: build the geometry, or include static mesh floors.");
    if (truncated) lines.push_back("Too many patches: only the first " + std::to_string(kMaxCells) + " are shown; use a larger patch size.");
    if (s.total && s.unbaked * 2 > s.total && settings.source != Source::Lights)
        lines.push_back("Most floors have no baked lighting yet: Build > Lighting first.");
    return lines;
}
// The point under a patch readout: brightness and its parts.
inline std::string Describe(const Cell& cell)
{
    auto number = [](double v) { return std::to_string(int(std::lround(v))); };
    return std::string(BandName(cell.band)) + ", brightness " + number(cell.value) + " (baked "
        + (cell.baked < 0 ? std::string("none") : number(cell.baked)) + ", in-game lights " + number(cell.lights) + ")"
        + (cell.mesh ? " on a static mesh" : "");
}

// The lines a patch is drawn with, lifted slightly off the floor: a square
// inset from its neighbours, plus one diagonal when partly visible and both
// when exposed, so the bands read without colour too.
inline std::vector<std::pair<Point, Point>> Outline(const Cell& cell, double size, double lift = 2)
{
    const double h = size * 0.42;
    const double z = cell.at.z + lift;
    const Point p[4] = { { cell.at.x - h, cell.at.y - h, z }, { cell.at.x + h, cell.at.y - h, z },
                         { cell.at.x + h, cell.at.y + h, z }, { cell.at.x - h, cell.at.y + h, z } };
    std::vector<std::pair<Point, Point>> lines{ { p[0], p[1] }, { p[1], p[2] }, { p[2], p[3] }, { p[3], p[0] } };
    if (cell.band != Band::Hidden) lines.push_back({ p[0], p[2] });
    if (cell.band == Band::Exposed) lines.push_back({ p[1], p[3] });
    return lines;
}

// The patch nearest a point within half a patch horizontally, preferring the
// floor at or just below it (so a 2D top view picks the highest floor).
inline const Cell* At(const std::vector<Cell>& cells, double x, double y, double size, double z = 1e18)
{
    const Cell* best = nullptr;
    for (const auto& cell : cells)
    {
        if (std::abs(cell.at.x - x) > size / 2 || std::abs(cell.at.y - y) > size / 2) continue;
        if (cell.at.z > z + 1) continue;
        if (!best || cell.at.z > best->at.z) best = &cell;
    }
    return best;
}
}
