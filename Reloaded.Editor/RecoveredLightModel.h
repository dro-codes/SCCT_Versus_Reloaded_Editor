#pragma once
// Bringing back the static lights a compiled map lost, after the OpenSCCT
// Editor's light recovery (LostLightRecovery, LightFitter, BakeLight).
//
// The stock cook leaves out every light whose only job is the bake, so a
// compiled map keeps their light in its lightmaps but not the lights. This is
// the pure part of recovering them:
//
//  - the lightmap term of a light at a surface point, as Build Lighting bakes
//    it (FUN_111b00c0: point S(x) cos, spots, sun, the NonIncidence family,
//    Shell, Cylinder), and its colour through FGetHSV;
//  - the compiled map's lightmap texels as world samples, thinned to one per
//    cell and surface orientation;
//  - a rebake of the lights the map still has at those samples, and what the
//    lightmaps hold beyond it (the residual);
//  - how much of a light's predicted light the residual holds (its support),
//    which checks an editor copy's lights against this bake;
//  - stand-in point lights fitted to the residual: greedy seeds at the
//    brightest smoothed residual, a start on the seed's normal at six heights
//    and eleven radii, Levenberg-Marquardt on position, reach and colour with
//    a soft L1 loss, shadows traced on the map's own geometry.
//
// Fitted lights restore the look, not the author's lights. On sixteen maps
// with a known editor copy about two in five of the OpenSCCT Editor's sat
// within 128 units of a lost light.
//
// No Windows or engine calls, so the tests compile it alone.
#include "RecoveredBspGeometry.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RecoveredLights
{
    using Vec3 = RecoveredBspGeometry::Vec3;

    inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
    inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
    inline Vec3 Normalized(const Vec3& a)
    {
        const double length = Length(a);
        return length > 0 ? a * (1 / length) : a;
    }
    inline double Channel(const Vec3& a, int index) { return index == 0 ? a.x : index == 1 ? a.y : a.z; }

    // Runs body(i) for i in [0, count) on the machine's threads. body must
    // only write what belongs to i.
    template <class Body> void ParallelFor(std::size_t count, Body&& body)
    {
        const std::size_t threads = (std::min<std::size_t>)((std::max)(1u, std::thread::hardware_concurrency()), 12);
        if (count < 256 || threads < 2)
        {
            for (std::size_t index = 0; index < count; ++index) body(index);
            return;
        }
        std::atomic<std::size_t> next{0};
        const std::size_t step = (std::max<std::size_t>)(16, count / (threads * 8));
        auto work = [&]
        {
            for (;;)
            {
                const std::size_t first = next.fetch_add(step);
                if (first >= count) return;
                const std::size_t last = (std::min)(count, first + step);
                for (std::size_t index = first; index < last; ++index) body(index);
            }
        };
        std::vector<std::thread> pool;
        for (std::size_t index = 1; index < threads; ++index) pool.emplace_back(work);
        work();
        for (auto& thread : pool) thread.join();
    }

    // ---- a light as the lightmapper sees it ---------------------------------------------------------------------

    namespace Effect
    {
        constexpr int StaticSpot = 8, Spotlight = 12, NonIncidence = 13, Shell = 14, Cylinder = 17, Sunlight = 19,
            QuadraticNonIncidence = 20, SpotShadow = 22;
    }

    // FGetHSV(hue, saturation, 255) x brightness / 255: the hue triangle (red
    // 0, green 85, blue 170) pulled toward white by saturation / 255, at the
    // value-255 scale 0.8213 (editor 0x11096ab0).
    inline Vec3 HsvColour(int hue, int saturation, double brightness)
    {
        hue = (std::max)(0, (std::min)(255, hue));
        double r, g, b;
        if (hue < 86) { r = (85 - hue) / 85.0; g = hue / 85.0; b = 0; }
        else if (hue < 171) { r = 0; g = (170 - hue) / 85.0; b = (hue - 85) / 85.0; }
        else { r = (hue - 170) / 85.0; g = 0; b = (255 - hue) / 84.0; }
        const double s = (std::max)(0, (std::min)(255, saturation)) / 255.0;
        const double k = 0.8213 * (brightness / 255.0);
        return {(r + s * (1 - r)) * k, (g + s * (1 - g)) * k, (b + s * (1 - b)) * k};
    }

    // A zone's ambient fill of lightmap texels, in bytes: half of FGetHSV with
    // its brightness curve clamp(0.7 v / (sqrt(v) + 0.01)), v = brightness x
    // 1.4 / 255, truncated (FLightMap::GetTextureData, 0x111b1d10).
    inline Vec3 LightmapAmbientBytes(int hue, int saturation, int brightness)
    {
        const double v = (std::max)(0, brightness) * 1.4 / 255.0;
        const double scale = (std::max)(0.0, (std::min)(1.0, v * (0.7 / (0.01 + std::sqrt(v)))));
        const Vec3 mix = HsvColour(hue, saturation, 255) * (1 / 0.8213);
        return {std::floor(mix.x * scale * 127.5), std::floor(mix.y * scale * 127.5), std::floor(mix.z * scale * 127.5)};
    }

    struct Light
    {
        std::string name;
        Vec3 location;
        Vec3 direction{1, 0, 0};    // spots and suns shine along it
        double brightness = 64;     // LightBrightness
        double lightRadius = 64;    // as stored; the reach is Radius()
        int hue = 0, saturation = 255, effect = 0, cone = 128;
        int type = 1, period = 32, phase = 0;   // LightType 0 is none
        int zone = 0;
        bool ownZoneOnly = false;   // bAffectOwnZoneOnly
        bool dynamic = false;       // bDynamicLight, or movable and not static
        bool applyStatic = true;    // bApplyToStaticLighting
        bool inGame = false;        // bApplyToInGameLighting
        bool gameplay = false;      // switched at run time
        double heatRadius = 0, heatIntensity = 0;
        double levelBrightness = 1; // LevelInfo.Brightness scales every light
        // False when the map does not store the light's LightRadius, so the
        // value is the class default of whichever editor loaded it.
        bool storesRadius = true;

        bool IsSun() const { return effect == Effect::Sunlight; }
        bool IsSpot() const { return effect == Effect::StaticSpot || effect == Effect::Spotlight; }
        bool IsCylinder() const { return effect == Effect::Cylinder; }
        // Reach in units: 25 (LightRadius + 1).
        double Radius() const { return 25.0 * (lightRadius + 1); }

        // Baked into lightmaps: a static light with a light type, not
        // dynamic, not switched in game, not an Echelon spot shadow light.
        bool Baked() const { return type != 0 && applyStatic && !dynamic && !gameplay && effect != Effect::SpotShadow; }

        // What the stock cook leaves out of a compiled map: a light that
        // matters only to the bake (editor 0x10ee13b0, filter 0x10edd1e0).
        bool CookDrops() const
        {
            return type != 0 && !(inGame || dynamic || effect == Effect::SpotShadow || (heatRadius != 0 && heatIntensity != 0));
        }

        // The light type's scale at bake time: pulses at their phase, flicker
        // at its mean, blink on or off by its phase.
        double Modulation() const
        {
            auto angle = [&] { return std::fmod(double(phase << 8), 6.283185307179586); };
            switch (type)
            {
            case 2: return 0.6 + 0.39 * std::sin(angle());
            case 3: return ((static_cast<long long>(phase) << 8) & 1) != 0 ? 0.0 : 1.0;
            case 4: return 0.375;
            case 7: return 0.9 + 0.09 * std::sin(angle());
            default: return 1.0;
            }
        }

        // Stored units per unit term, 0..1 and above per channel.
        Vec3 Colour() const { return HsvColour(hue, saturation, brightness) * (Modulation() * levelBrightness); }

        // The lightmap term at a surface point, before shadows. x = d / R,
        // S(x) = 1 - 3x^2 + 2x^3. Two-sided surfaces take the absolute cosine.
        double Surface(const Vec3& p, const Vec3& n, bool twoSided) const
        {
            if (IsSun())
            {
                const double c = -Dot(n, direction);
                return twoSided ? std::fabs(c) : (std::max)(0.0, c);
            }
            const double r = Radius();
            if (IsCylinder())
            {
                const double dx = p.x - location.x, dy = p.y - location.y;
                return (std::max)(0.0, 1 - (dx * dx + dy * dy) / (r * r));
            }
            const Vec3 v = location - p;
            const double d = Length(v);
            if (d >= r) return 0;
            const double x = d / r;
            switch (effect)
            {
            case Effect::NonIncidence: return 1 - x;
            case Effect::QuadraticNonIncidence: return 1 - x * x;
            case Effect::Shell: return x >= 0.8 ? 1 - 10 * std::fabs(x - 0.9) : 0;
            default: break;
            }
            if (d < 1e-6) return 0;
            double cosine = Dot(n, v) / d;
            if (twoSided) cosine = std::fabs(cosine);
            if (cosine <= 0) return 0;
            double f = (1 - 3 * x * x + 2 * x * x * x) * cosine;
            if (IsSpot())
            {
                // The UE1 cone: k = 1 - LightCone / 256.
                const double k = 1 - cone / 256.0;
                const double cosA = -Dot(v, direction) / d;
                if (cosA <= k || k >= 1) return 0;
                const double t = (cosA - k) / (1 - k);
                f *= t * t;
            }
            return f;
        }
    };

    // ---- the map's own geometry, for shadows --------------------------------------------------------------------

    struct Occluders
    {
        // True when nothing lies between the two points.
        std::function<bool(const Vec3& from, const Vec3& to)> clear;
        // True when a ray from the point towards the sun reaches sky.
        std::function<bool(const Vec3& from, const Vec3& towardsSun)> sunClear;
    };

    // How far shadow rays start off the surface, and stop short of the light.
    constexpr double kShadowBias = 0.5;

    // True when nothing blocks the way from a surface point to the light.
    inline bool Lit(const Light& light, const Vec3& p, const Vec3& n, bool twoSided, const Occluders& occluders)
    {
        const Vec3 toLight = light.IsSun() ? light.direction * -1 : light.location - p;
        const Vec3 side = twoSided && Dot(n, toLight) < 0 ? n * -1 : n;
        const Vec3 origin = p + side * kShadowBias;
        if (light.IsSun())
            return !occluders.sunClear || occluders.sunClear(origin, Normalized(toLight));
        const Vec3 d = light.location - origin;
        const double length = Length(d);
        if (length < 1e-3 || !occluders.clear) return true;
        return occluders.clear(origin, origin + d * ((std::max)(0.0, length - kShadowBias) / length));
    }

    // ---- lightmap texels as world samples -----------------------------------------------------------------------

    struct Samples
    {
        std::vector<Vec3> position, normal;
        std::vector<unsigned char> twoSided;
        std::vector<int> zone;
        std::vector<float> atlas;   // the compiled lightmap's RGB bytes, three a sample
        std::size_t Count() const { return position.size(); }
    };

    // A lightmap atlas as the editor stores it: TEXF_RGBA8, B,G,R,A in memory.
    struct AtlasImage
    {
        int width = 0, height = 0;
        const std::uint32_t* pixels = nullptr;
    };

    struct LitVertex
    {
        Vec3 position;
        double u = 0, v = 0;    // lightmap UV, 0..1 across the atlas
    };

    // Collects the samples of a map: one per lightmap texel whose centre lies
    // on a drawn polygon, thinned to one per cell of `thin` units per surface
    // orientation (0 keeps every texel).
    class SampleBuilder
    {
    public:
        explicit SampleBuilder(double thin) : thin_(thin) {}

        // One drawn polygon with its lightmap UVs. atlasId tells atlases apart.
        void AddPolygon(const std::vector<LitVertex>& polygon, const Vec3& normal, bool twoSided, int zone,
                        const AtlasImage& image, int atlasId)
        {
            if (polygon.size() < 3 || !image.pixels || image.width <= 0 || image.height <= 0) return;
            // The UV-to-world map of the polygon's plane, from the three
            // corners that span the most lightmap area.
            std::size_t best[3]{0, 1, 2};
            double bestDeterminant = 0;
            for (std::size_t i = 0; i < polygon.size(); ++i)
                for (std::size_t j = i + 1; j < polygon.size(); ++j)
                    for (std::size_t k = j + 1; k < polygon.size(); ++k)
                    {
                        const double determinant = (polygon[j].u - polygon[i].u) * (polygon[k].v - polygon[i].v)
                            - (polygon[k].u - polygon[i].u) * (polygon[j].v - polygon[i].v);
                        if (std::fabs(determinant) > std::fabs(bestDeterminant))
                        {
                            bestDeterminant = determinant;
                            best[0] = i; best[1] = j; best[2] = k;
                        }
                    }
            if (!std::isfinite(bestDeterminant) || std::fabs(bestDeterminant) < 1e-12) return;
            const LitVertex& a = polygon[best[0]];
            const double du1 = polygon[best[1]].u - a.u, dv1 = polygon[best[1]].v - a.v;
            const double du2 = polygon[best[2]].u - a.u, dv2 = polygon[best[2]].v - a.v;
            const Vec3 e1 = polygon[best[1]].position - a.position, e2 = polygon[best[2]].position - a.position;
            const Vec3 alongU = (e1 * dv2 - e2 * dv1) * (1 / bestDeterminant);
            const Vec3 alongV = (e2 * du1 - e1 * du2) * (1 / bestDeterminant);
            double minU = polygon[0].u, maxU = minU, minV = polygon[0].v, maxV = minV;
            for (const LitVertex& vertex : polygon)
            {
                minU = (std::min)(minU, vertex.u); maxU = (std::max)(maxU, vertex.u);
                minV = (std::min)(minV, vertex.v); maxV = (std::max)(maxV, vertex.v);
            }
            const int x0 = (std::max)(0, static_cast<int>(std::floor(minU * image.width - 0.5)));
            const int x1 = (std::min)(image.width - 1, static_cast<int>(std::ceil(maxU * image.width - 0.5)));
            const int y0 = (std::max)(0, static_cast<int>(std::floor(minV * image.height - 0.5)));
            const int y1 = (std::min)(image.height - 1, static_cast<int>(std::ceil(maxV * image.height - 0.5)));
            if (x1 < x0 || y1 < y0 || std::int64_t(x1 - x0 + 1) * (y1 - y0 + 1) > 4096 * 4096) return;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const double u = (x + 0.5) / image.width, v = (y + 0.5) / image.height;
                    if (!Inside(polygon, u, v)) continue;
                    const std::uint64_t texel = (std::uint64_t(static_cast<std::uint32_t>(atlasId)) << 32)
                        | static_cast<std::uint32_t>(y * image.width + x);
                    if (!seen_.insert(texel).second) continue;
                    const Vec3 p = a.position + alongU * (u - a.u) + alongV * (v - a.v);
                    if (thin_ > 0)
                    {
                        const Cell cell{static_cast<long long>(std::floor(p.x / thin_)), static_cast<long long>(std::floor(p.y / thin_)),
                            static_cast<long long>(std::floor(p.z / thin_)), static_cast<int>(std::lround(normal.x * 4)),
                            static_cast<int>(std::lround(normal.y * 4)), static_cast<int>(std::lround(normal.z * 4))};
                        if (!cells_.insert(cell).second) continue;
                    }
                    const std::uint32_t pixel = image.pixels[y * image.width + x];
                    samples.position.push_back(p);
                    samples.normal.push_back(normal);
                    samples.twoSided.push_back(twoSided ? 1 : 0);
                    samples.zone.push_back(zone);
                    samples.atlas.push_back(static_cast<float>((pixel >> 16) & 255));
                    samples.atlas.push_back(static_cast<float>((pixel >> 8) & 255));
                    samples.atlas.push_back(static_cast<float>(pixel & 255));
                }
        }

        Samples samples;

    private:
        using Cell = std::tuple<long long, long long, long long, int, int, int>;

        static bool Inside(const std::vector<LitVertex>& polygon, double u, double v)
        {
            bool inside = false;
            for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
                if ((polygon[i].v > v) != (polygon[j].v > v)
                    && u < (polygon[j].u - polygon[i].u) * (v - polygon[i].v) / (polygon[j].v - polygon[i].v) + polygon[i].u)
                    inside = !inside;
            return inside;
        }

        double thin_;
        std::unordered_set<std::uint64_t> seen_;
        std::set<Cell> cells_;
    };

    // A uniform grid over sample positions, for reach and neighbour queries.
    class SampleGrid
    {
    public:
        SampleGrid(const std::vector<Vec3>& points, double cell) : points_(points), cell_(cell)
        {
            for (std::size_t index = 0; index < points.size(); ++index) cells_[KeyOf(points[index])].push_back(static_cast<int>(index));
        }

        std::vector<int> Within(const Vec3& centre, double radius) const
        {
            std::vector<int> result;
            const Key low = KeyOf(centre - Vec3{radius, radius, radius}), high = KeyOf(centre + Vec3{radius, radius, radius});
            const double r2 = radius * radius;
            auto collect = [&](const std::vector<int>& list)
            {
                for (int index : list)
                {
                    const Vec3 d = points_[static_cast<std::size_t>(index)] - centre;
                    if (Dot(d, d) <= r2) result.push_back(index);
                }
            };
            const double span = (double(high.x) - low.x + 1) * (double(high.y) - low.y + 1) * (double(high.z) - low.z + 1);
            if (span > static_cast<double>(cells_.size()))
            {
                // A reach wider than the map: walk the cells there are.
                for (const auto& cell : cells_)
                    if (cell.first.x >= low.x && cell.first.x <= high.x && cell.first.y >= low.y && cell.first.y <= high.y
                        && cell.first.z >= low.z && cell.first.z <= high.z) collect(cell.second);
            }
            else
                for (int x = low.x; x <= high.x; ++x)
                    for (int y = low.y; y <= high.y; ++y)
                        for (int z = low.z; z <= high.z; ++z)
                        {
                            const auto found = cells_.find(Key{x, y, z});
                            if (found != cells_.end()) collect(found->second);
                        }
            std::sort(result.begin(), result.end());
            return result;
        }

        // Up to k nearest points within radius, the point itself included.
        std::vector<int> Nearest(const Vec3& centre, std::size_t k, double radius) const
        {
            std::vector<int> found = Within(centre, radius);
            std::stable_sort(found.begin(), found.end(), [&](int a, int b)
            {
                const Vec3 da = points_[static_cast<std::size_t>(a)] - centre, db = points_[static_cast<std::size_t>(b)] - centre;
                return Dot(da, da) < Dot(db, db);
            });
            if (found.size() > k) found.resize(k);
            return found;
        }

    private:
        struct Key
        {
            int x = 0, y = 0, z = 0;
            Key() = default;
            Key(int a, int b, int c) : x(a), y(b), z(c) {}
            Key(const Vec3& p, double cell) : x(static_cast<int>(std::floor(p.x / cell))), y(static_cast<int>(std::floor(p.y / cell))),
                z(static_cast<int>(std::floor(p.z / cell))) {}
            bool operator<(const Key& o) const { return std::tie(x, y, z) < std::tie(o.x, o.y, o.z); }
        };
        Key KeyOf(const Vec3& p) const { return Key(p, cell_); }

        const std::vector<Vec3>& points_;
        double cell_;
        std::map<Key, std::vector<int>> cells_;
    };

    // ---- the scene a recovery works on --------------------------------------------------------------------------

    struct Scene
    {
        Samples samples;
        Occluders occluders;
        // A zone's ambient fill of its lightmap texels, bytes per channel.
        std::function<Vec3(int zone)> ambient;
        // The compiled map's zone at a point (for lights taken from elsewhere).
        std::function<int(const Vec3&)> zoneAt;
        double levelBrightness = 1;
        std::size_t meshTriangles = 0;
    };

    // The faithful bake of lights at every sample, RGB bytes: each zone's
    // ambient, then one byte of term per light through its colour, added with
    // saturation. Every light went through the original's shadow unpack, so
    // an unshadowed texel holds 254 of 255. defaultRadius replaces the
    // LightRadius of lights that store none.
    inline std::vector<float> BakeAt(const Scene& scene, const std::vector<Light>& lights, double defaultRadius = 0)
    {
        const Samples& s = scene.samples;
        std::vector<float> rgb(s.Count() * 3);
        for (std::size_t i = 0; i < s.Count(); ++i)
        {
            const Vec3 ambient = scene.ambient ? scene.ambient(s.zone[i]) : Vec3{};
            rgb[3 * i] = static_cast<float>(ambient.x);
            rgb[3 * i + 1] = static_cast<float>(ambient.y);
            rgb[3 * i + 2] = static_cast<float>(ambient.z);
        }
        const SampleGrid grid(s.position, 256);
        for (Light light : lights)
        {
            if (!light.Baked()) continue;
            if (!light.storesRadius && defaultRadius > 0) light.lightRadius = defaultRadius;
            const Vec3 colour = light.Colour();
            std::vector<int> reached;
            if (light.IsSun() || light.IsCylinder())
            {
                reached.resize(s.Count());
                for (std::size_t i = 0; i < reached.size(); ++i) reached[i] = static_cast<int>(i);
            }
            else reached = grid.Within(light.location, light.Radius());
            ParallelFor(reached.size(), [&](std::size_t k)
            {
                const std::size_t i = static_cast<std::size_t>(reached[k]);
                if (light.ownZoneOnly && s.zone[i] != light.zone) return;
                const double f = light.Surface(s.position[i], s.normal[i], s.twoSided[i] != 0) * (254.0 / 255.0);
                if (f <= 0 || !Lit(light, s.position[i], s.normal[i], s.twoSided[i] != 0, scene.occluders)) return;
                const double term = std::floor((std::min)(255.0, 255 * f));
                for (int c = 0; c < 3; ++c)
                    rgb[3 * i + c] = static_cast<float>((std::min)(255.0,
                        rgb[3 * i + c] + std::floor((std::min)(255.0, term * Channel(colour, c)))));
            });
        }
        return rgb;
    }

    struct BakeError
    {
        double mae = 0, bias = 0;
        std::size_t samples = 0;
    };

    // Mean absolute and mean signed byte error (bake minus lightmap).
    inline BakeError Compare(const Samples& samples, const std::vector<float>& baked)
    {
        BakeError error;
        double absolute = 0, sign = 0;
        for (std::size_t i = 0; i < baked.size() && i < samples.atlas.size(); ++i)
        {
            const double d = baked[i] - samples.atlas[i];
            absolute += std::fabs(d);
            sign += d;
        }
        error.samples = samples.Count();
        if (!baked.empty()) { error.mae = absolute / baked.size(); error.bias = sign / baked.size(); }
        return error;
    }

    inline std::vector<float> Residual(const Samples& samples, const std::vector<float>& baked)
    {
        std::vector<float> residual(samples.atlas.size());
        for (std::size_t i = 0; i < residual.size(); ++i) residual[i] = samples.atlas[i] - baked[i];
        return residual;
    }

    // The share of a light's predicted light (hard shadows, on samples it
    // lights by 8 bytes or more) the residual holds: close to 1 when the bake has
    // it, close to 0 when it was added or moved after the bake. Negative when
    // fewer than five samples can tell. held and predicted take the sums.
    inline double Support(const Scene& scene, const SampleGrid& grid, const std::vector<float>& residual, const Light& light,
                          double& held, double& predicted)
    {
        if (!light.Baked()) return -1;
        const Samples& s = scene.samples;
        std::vector<int> reached;
        if (light.IsSun())
        {
            reached.resize(s.Count());
            for (std::size_t i = 0; i < reached.size(); ++i) reached[i] = static_cast<int>(i);
        }
        else reached = grid.Within(light.location, light.Radius());
        const Vec3 colour = light.Colour() * 255;
        std::vector<double> own(reached.size()), left(reached.size());
        ParallelFor(reached.size(), [&](std::size_t k)
        {
            const std::size_t i = static_cast<std::size_t>(reached[k]);
            const double f = light.Surface(s.position[i], s.normal[i], s.twoSided[i] != 0);
            if (f <= 0 || (light.ownZoneOnly && s.zone[i] != light.zone)
                || !Lit(light, s.position[i], s.normal[i], s.twoSided[i] != 0, scene.occluders)) return;
            own[k] = ((std::min)(255.0, f * colour.x) + (std::min)(255.0, f * colour.y) + (std::min)(255.0, f * colour.z)) / 3;
            left[k] = (std::max)(0.0, (double(residual[3 * i]) + residual[3 * i + 1] + residual[3 * i + 2]) / 3);
        });
        double h = 0, p = 0;
        int count = 0;
        for (std::size_t k = 0; k < reached.size(); ++k)
            if (own[k] >= 8) { h += (std::min)(left[k], own[k]); p += own[k]; ++count; }
        if (count < 5) return -1;
        held += h;
        predicted += p;
        return h / p;
    }

    // ---- fitting stand-in lights --------------------------------------------------------------------------------

    struct FitSettings
    {
        double thin = 24;               // one sample per cell of this size per orientation
        double stopResidual = 10;       // smallest seed residual (bytes) worth a light
        double minPeak = 10;            // smallest peak (bytes at full term) of an accepted light
        double failureMask = 96;        // a seed that gives no light masks this radius
        double overshootWeight = 0.7;   // weight of over-lit samples in a candidate's score
        int maxFailures = 60;
        int maxLights = 1500;
        int softLimit = 300;            // after this many lights a seed must reach twice the floor
    };

    struct FittedLight
    {
        Vec3 location;
        double radius = 0;      // reach, units
        Vec3 amplitude;         // bytes per channel at a full term
        double gain = 0;
        double seed = 0;        // the smoothed residual at the texel it was seeded from
        double support = -1;    // share of its predicted light the residual held; negative: too faint to say
        double Peak() const { return (std::max)(amplitude.x, (std::max)(amplitude.y, amplitude.z)); }
    };

    // .NET's seeded System.Random (Knuth's subtractive generator), so the
    // subsamples are drawn as the OpenSCCT Editor draws them.
    class SeededRandom
    {
    public:
        explicit SeededRandom(int seed)
        {
            const int subtraction = seed == (std::numeric_limits<int>::min)() ? (std::numeric_limits<int>::max)() : std::abs(seed);
            int mj = 161803398 - subtraction, mk = 1, ii = 0;
            table_[55] = mj;
            for (int i = 1; i < 55; ++i)
            {
                if ((ii += 21) >= 55) ii -= 55;
                table_[ii] = mk;
                mk = mj - mk;
                if (mk < 0) mk += (std::numeric_limits<int>::max)();
                mj = table_[ii];
            }
            for (int k = 1; k < 5; ++k)
                for (int i = 1; i < 56; ++i)
                {
                    int n = i + 30;
                    if (n >= 55) n -= 55;
                    table_[i] = static_cast<int>(static_cast<unsigned>(table_[i]) - static_cast<unsigned>(table_[1 + n]));
                    if (table_[i] < 0) table_[i] += (std::numeric_limits<int>::max)();
                }
        }

        // In [low, high).
        int Next(int low, int high)
        {
            if (++next_ >= 56) next_ = 1;
            if (++nextP_ >= 56) nextP_ = 1;
            int value = table_[next_] - table_[nextP_];
            if (value == (std::numeric_limits<int>::max)()) --value;
            if (value < 0) value += (std::numeric_limits<int>::max)();
            table_[next_] = value;
            const double sample = value * (1.0 / (std::numeric_limits<int>::max)());
            return static_cast<int>(sample * (double(high) - low)) + low;
        }

    private:
        int table_[56]{};
        int next_ = 0, nextP_ = 21;
    };

    class LightFitter
    {
    public:
        // residual: bytes per sample and channel the known lights leave
        // unexplained (lightmap minus bake). A saturated lightmap texel only
        // bounds the sum from below.
        LightFitter(const Scene& scene, std::vector<float> residual, const FitSettings& settings)
            : scene_(scene), s_(scene.samples), res_(std::move(residual)), settings_(settings), grid_(scene.samples.position, 256)
        {
            for (std::size_t i = 0; i < s_.Count(); ++i)
                if ((std::max)(s_.atlas[3 * i], (std::max)(s_.atlas[3 * i + 1], s_.atlas[3 * i + 2])) >= 254.5f)
                    for (int c = 0; c < 3; ++c) res_[3 * i + c] = (std::max)(0.0f, res_[3 * i + c]);
            neighbours_.resize(s_.Count());
            ParallelFor(s_.Count(), [&](std::size_t i) { neighbours_[i] = grid_.Nearest(s_.position[i], 6, settings_.thin * 3); });
            smooth_.resize(s_.Count());
            ParallelFor(s_.Count(), [&](std::size_t i) { Smooth(i); });
        }

        // The 95th percentile of the smoothed overshoot (the model brighter
        // than the bake) on samples the known lights light by 24 bytes or
        // more: model error the fit should not place lights on. At least
        // stopResidual and at most 30. measured is negative when fewer than a
        // hundred samples can tell.
        double NoiseFloor(const std::vector<float>& knownLight, double& measured) const
        {
            std::vector<float> values;
            for (std::size_t i = 0; i < s_.Count(); ++i)
                if ((knownLight[3 * i] + knownLight[3 * i + 1] + knownLight[3 * i + 2]) / 3 >= 24)
                    values.push_back((std::max)(0.0f, -smooth_[i]));
            measured = -1;
            if (values.size() < 100) return settings_.stopResidual;
            std::sort(values.begin(), values.end());
            measured = values[static_cast<std::size_t>(values.size() * 0.95)];
            return (std::max)(settings_.stopResidual, (std::min)(30.0, measured));
        }

        // The share of samples whose smoothed residual is above the floor,
        // and the mean excess over it (bytes per sample). A lost light leaves
        // a patch well above the floor; model error leaves scattered samples
        // just above it.
        void Structure(double floor, double& share, double& excess) const
        {
            std::size_t above = 0;
            double sum = 0;
            for (float value : smooth_)
                if (value > floor) { ++above; sum += value - floor; }
            share = smooth_.empty() ? 0 : double(above) / smooth_.size();
            excess = smooth_.empty() ? 0 : sum / smooth_.size();
        }

        // The greedy fit down to `floor` bytes of smoothed residual. step is
        // called with the lights placed so far; false stops the fit there.
        std::vector<FittedLight> Run(double floor, const std::function<bool(std::size_t, double)>& step = {})
        {
            floor = (std::max)(floor, settings_.stopResidual);
            const double minPeak = (std::max)(settings_.minPeak, floor);
            std::vector<FittedLight> found;
            std::vector<unsigned char> mask(s_.Count());
            int failures = 0;
            while (static_cast<int>(found.size()) < settings_.maxLights)
            {
                int seed = -1;
                float best = std::numeric_limits<float>::lowest();
                for (std::size_t i = 0; i < smooth_.size(); ++i)
                    if (!mask[i] && smooth_[i] > best) { best = smooth_[i]; seed = static_cast<int>(i); }
                const double stop = static_cast<int>(found.size()) >= settings_.softLimit ? 2 * floor : floor;
                if (seed < 0 || best < stop) break;
                if (step && !step(found.size(), best)) break;
                bool placed = false;
                Start start;
                if (Candidate(seed, start))
                {
                    Vec3 at = start.at;
                    double radius = start.radius;
                    Refine(seed, start, at, radius);
                    FittedLight light;
                    if (Apply(at, radius, best, minPeak, light))
                    {
                        found.push_back(light);
                        placed = true;
                    }
                }
                if (placed) { failures = 0; continue; }
                for (int i : grid_.Within(s_.position[static_cast<std::size_t>(seed)], settings_.failureMask))
                    mask[static_cast<std::size_t>(i)] = 1;
                if (++failures > settings_.maxFailures) break;
            }
            return found;
        }

    private:
        struct Start
        {
            Vec3 at;
            double radius = 0;
            Vec3 amplitude;
        };

        // LightRadius 4 to 110: reach 125 to 2775 units.
        static const std::array<double, 11>& Radii()
        {
            static const std::array<double, 11> radii = [] {
                std::array<double, 11> r{};
                const int stored[11] = {4, 7, 10, 14, 19, 25, 33, 45, 60, 80, 110};
                for (int i = 0; i < 11; ++i) r[static_cast<std::size_t>(i)] = 25.0 * (stored[i] + 1);
                return r;
            }();
            return radii;
        }
        static constexpr int kMaxCandidateSamples = 6000;
        static constexpr double kSoftL1Scale = 12;

        // A stand-in point light as the bake sees it (the term only).
        static Light Point(const Vec3& at, double radius)
        {
            Light light;
            light.location = at;
            light.lightRadius = radius / 25 - 1;
            light.brightness = 255;
            return light;
        }
        double Term(const Light& light, int i) const
        {
            const std::size_t at = static_cast<std::size_t>(i);
            return light.Surface(s_.position[at], s_.normal[at], s_.twoSided[at] != 0);
        }
        bool LitAt(const Light& light, int i) const
        {
            const std::size_t at = static_cast<std::size_t>(i);
            return Lit(light, s_.position[at], s_.normal[at], s_.twoSided[at] != 0, scene_.occluders);
        }

        static std::vector<int> Subsample(std::vector<int> indices, std::size_t most, SeededRandom& random)
        {
            if (indices.size() <= most) return indices;
            for (std::size_t i = 0; i < most; ++i)
                std::swap(indices[i], indices[static_cast<std::size_t>(random.Next(static_cast<int>(i), static_cast<int>(indices.size())))]);
            indices.resize(most);
            return indices;
        }

        // Least-squares colour for term g over the samples, and how much it
        // lowers the squared residual (over-lit samples weighted down: a
        // missed shadow over-lights and should not veto).
        double Score(const std::vector<int>& indices, const std::vector<double>& g, Vec3& amplitude) const
        {
            double gg = 0, gr = 0, gG = 0, gB = 0;
            for (std::size_t k = 0; k < indices.size(); ++k)
            {
                if (g[k] <= 0) continue;
                const std::size_t i = 3 * static_cast<std::size_t>(indices[k]);
                gg += g[k] * g[k];
                gr += g[k] * res_[i];
                gG += g[k] * res_[i + 1];
                gB += g[k] * res_[i + 2];
            }
            amplitude = {};
            if (gg <= 0) return 0;
            amplitude = {(std::max)(0.0, gr / gg), (std::max)(0.0, gG / gg), (std::max)(0.0, gB / gg)};
            double gain = 0;
            for (std::size_t k = 0; k < indices.size(); ++k)
            {
                if (g[k] <= 0) continue;
                const std::size_t i = 3 * static_cast<std::size_t>(indices[k]);
                for (int c = 0; c < 3; ++c)
                {
                    const double r = res_[i + static_cast<std::size_t>(c)];
                    double e = r - g[k] * Channel(amplitude, c);
                    if (e < 0) e *= settings_.overshootWeight;
                    gain += r * r - e * e;
                }
            }
            return gain;
        }

        // The best light on the seed's normal: six heights, eleven radii,
        // colour in closed form.
        bool Candidate(int seed, Start& best) const
        {
            static const double heights[] = {12, 24, 48, 96, 192, 384};
            const Vec3 p0 = s_.position[static_cast<std::size_t>(seed)], n0 = s_.normal[static_cast<std::size_t>(seed)];
            double bestGain = 0;
            bool found = false;
            SeededRandom random(seed);
            for (double height : heights)
            {
                const Vec3 at = p0 + n0 * height;
                const Light probe = Point(at, Radii().back());
                // A light in solid space cannot see its own seed.
                if (!LitAt(probe, seed)) continue;
                const std::vector<int> nearby = Subsample(grid_.Within(at, Radii().back()), kMaxCandidateSamples, random);
                if (nearby.size() < 8) continue;
                std::vector<unsigned char> lit(nearby.size());
                ParallelFor(nearby.size(), [&](std::size_t k) { lit[k] = LitAt(probe, nearby[k]) ? 1 : 0; });
                std::vector<int> visible;
                for (std::size_t k = 0; k < nearby.size(); ++k)
                    if (lit[k]) visible.push_back(nearby[k]);
                std::vector<double> g(visible.size());
                for (double radius : Radii())
                {
                    const Light light = Point(at, radius);
                    for (std::size_t k = 0; k < visible.size(); ++k) g[k] = Term(light, visible[k]);
                    Vec3 amplitude;
                    const double gain = Score(visible, g, amplitude);
                    if (gain > bestGain)
                    {
                        bestGain = gain;
                        best = {at, radius, amplitude};
                        found = true;
                    }
                }
            }
            return found;
        }

        std::vector<double> Terms(const std::vector<int>& indices, const double* x) const
        {
            const Light light = Point({x[0], x[1], x[2]}, std::exp(x[3]));
            std::vector<double> g(indices.size());
            for (std::size_t k = 0; k < indices.size(); ++k) g[k] = Term(light, indices[k]);
            return g;
        }

        double Cost(const std::vector<int>& indices, const double* x) const
        {
            const std::vector<double> g = Terms(indices, x);
            double cost = 0;
            for (std::size_t k = 0; k < indices.size(); ++k)
                for (int c = 0; c < 3; ++c)
                {
                    const double z = (res_[3 * static_cast<std::size_t>(indices[k]) + static_cast<std::size_t>(c)] - g[k] * x[4 + c]) / kSoftL1Scale;
                    cost += std::sqrt(1 + z * z) - 1;
                }
            return cost;
        }

        // Gaussian elimination with partial pivoting; false when singular.
        static bool Solve(double m[7][7], double v[7], double x[7])
        {
            for (int col = 0; col < 7; ++col)
            {
                int pivot = col;
                for (int r = col + 1; r < 7; ++r)
                    if (std::fabs(m[r][col]) > std::fabs(m[pivot][col])) pivot = r;
                if (std::fabs(m[pivot][col]) < 1e-12) return false;
                if (pivot != col)
                {
                    for (int k = 0; k < 7; ++k) std::swap(m[col][k], m[pivot][k]);
                    std::swap(v[col], v[pivot]);
                }
                for (int r = col + 1; r < 7; ++r)
                {
                    const double f = m[r][col] / m[col][col];
                    for (int k = col; k < 7; ++k) m[r][k] -= f * m[col][k];
                    v[r] -= f * v[col];
                }
            }
            for (int r = 6; r >= 0; --r)
            {
                double sum = v[r];
                for (int k = r + 1; k < 7; ++k) sum -= m[r][k] * x[k];
                x[r] = sum / m[r][r];
            }
            return true;
        }

        // Refines position and reach with the colour by Levenberg-Marquardt,
        // iteratively reweighted for a soft L1 loss (scale 12 bytes), on up to
        // 6,000 samples the starting light sees (visibility held fixed);
        // position within 200 units.
        void Refine(int seed, const Start& start, Vec3& at, double& radius) const
        {
            const Light first = Point(start.at, start.radius);
            SeededRandom random(seed ^ 0x5bd1e995);
            const std::vector<int> nearby = Subsample(grid_.Within(start.at, start.radius * 1.15), 8000, random);
            std::vector<unsigned char> lit(nearby.size());
            ParallelFor(nearby.size(), [&](std::size_t k) { lit[k] = LitAt(first, nearby[k]) ? 1 : 0; });
            std::vector<int> visible;
            for (std::size_t k = 0; k < nearby.size(); ++k)
                if (lit[k]) visible.push_back(nearby[k]);
            const std::vector<int> indices = Subsample(std::move(visible), kMaxCandidateSamples, random);
            if (indices.size() < 12) return;

            // x = (X, Y, Z, ln R, aR, aG, aB)
            double x[7] = {start.at.x, start.at.y, start.at.z, std::log(start.radius), (std::max)(start.amplitude.x, 0.5),
                           (std::max)(start.amplitude.y, 0.5), (std::max)(start.amplitude.z, 0.5)};
            const double low[7] = {start.at.x - 200, start.at.y - 200, start.at.z - 200, std::log(60.0), 0, 0, 0};
            const double high[7] = {start.at.x + 200, start.at.y + 200, start.at.z + 200, std::log(6000.0), 600, 600, 600};
            const double steps[4] = {2, 2, 2, 0.02};
            double lambda = 1e-2;
            double cost = Cost(indices, x);
            for (int iteration = 0; iteration < 30; ++iteration)
            {
                const std::vector<double> g = Terms(indices, x);
                std::vector<double> dg[4];
                for (int j = 0; j < 4; ++j)
                {
                    double moved[7];
                    std::copy(x, x + 7, moved);
                    moved[j] += steps[j];
                    dg[j] = Terms(indices, moved);
                    for (std::size_t k = 0; k < indices.size(); ++k) dg[j][k] = (dg[j][k] - g[k]) / steps[j];
                }
                // Normal equations of the reweighted problem: (J'WJ) d = -J'We.
                double a[7][7]{}, b[7]{}, row[7];
                for (std::size_t k = 0; k < indices.size(); ++k)
                    for (int c = 0; c < 3; ++c)
                    {
                        const double e = res_[3 * static_cast<std::size_t>(indices[k]) + static_cast<std::size_t>(c)] - g[k] * x[4 + c];
                        const double w = 1 / std::sqrt(1 + (e / kSoftL1Scale) * (e / kSoftL1Scale));
                        for (int j = 0; j < 4; ++j) row[j] = -x[4 + c] * dg[j][k];
                        row[4] = row[5] = row[6] = 0;
                        row[4 + c] = -g[k];
                        for (int p = 0; p < 7; ++p)
                        {
                            if (row[p] == 0) continue;
                            b[p] -= w * row[p] * e;
                            for (int q = 0; q < 7; ++q) a[p][q] += w * row[p] * row[q];
                        }
                    }
                bool improved = false;
                while (lambda < 1e8)
                {
                    double damped[7][7], rhs[7], d[7];
                    for (int p = 0; p < 7; ++p)
                    {
                        for (int q = 0; q < 7; ++q) damped[p][q] = a[p][q];
                        damped[p][p] += lambda * (std::max)(a[p][p], 1e-9);
                        rhs[p] = b[p];
                    }
                    if (!Solve(damped, rhs, d)) { lambda *= 4; continue; }
                    double trial[7];
                    for (int p = 0; p < 7; ++p) trial[p] = (std::max)(low[p], (std::min)(high[p], x[p] + d[p]));
                    const double trialCost = Cost(indices, trial);
                    if (trialCost < cost)
                    {
                        improved = cost - trialCost > 1e-4 * cost;
                        std::copy(trial, trial + 7, x);
                        cost = trialCost;
                        lambda = (std::max)(lambda / 3, 1e-7);
                        break;
                    }
                    lambda *= 4;
                }
                if (!improved) break;
            }
            const Vec3 refined{x[0], x[1], x[2]};
            const double reach = std::exp(x[3]);
            // A refined light that moved into solid space (it no longer sees
            // its seed) keeps its start.
            if (LitAt(Point(refined, reach), seed)) { at = refined; radius = reach; }
        }

        // Traces the refined light's shadows on every sample it reaches,
        // solves its colour again and subtracts it, when it lowers the
        // residual and peaks at minPeak bytes or more.
        bool Apply(const Vec3& at, double radius, double seedResidual, double minPeak, FittedLight& light)
        {
            const Light point = Point(at, radius);
            const std::vector<int> reached = grid_.Within(at, radius);
            std::vector<double> g(reached.size());
            ParallelFor(reached.size(), [&](std::size_t k) { g[k] = LitAt(point, reached[k]) ? Term(point, reached[k]) : 0; });
            Vec3 amplitude;
            const double gain = Score(reached, g, amplitude);
            if (gain <= 0 || (std::max)(amplitude.x, (std::max)(amplitude.y, amplitude.z)) < minPeak) return false;
            double held = 0, predicted = 0;
            std::vector<int> again;
            for (std::size_t k = 0; k < reached.size(); ++k)
            {
                if (g[k] <= 0) continue;
                const std::size_t i = 3 * static_cast<std::size_t>(reached[k]);
                double expect = 0, was = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const double add = (std::min)(255.0, g[k] * Channel(amplitude, c));
                    expect += add;
                    was += res_[i + static_cast<std::size_t>(c)];
                    res_[i + static_cast<std::size_t>(c)] -= static_cast<float>(add);
                }
                if (expect >= 24)
                {
                    held += (std::max)(0.0, (std::min)(was, expect));
                    predicted += expect;
                }
                again.push_back(reached[k]);
                for (int neighbour : neighbours_[static_cast<std::size_t>(reached[k])]) again.push_back(neighbour);
            }
            // The smoothed residual changes for the samples touched and their neighbours.
            std::sort(again.begin(), again.end());
            again.erase(std::unique(again.begin(), again.end()), again.end());
            ParallelFor(again.size(), [&](std::size_t k) { Smooth(static_cast<std::size_t>(again[k])); });
            light = {at, radius, amplitude, gain, seedResidual, predicted > 0 ? held / predicted : -1};
            return true;
        }

        // Median of the residual's mean over a sample and its nearest
        // neighbours (one texel's noise is no seed).
        void Smooth(std::size_t i)
        {
            const std::vector<int>& nearby = neighbours_[i];
            float values[8];
            std::size_t count = 0;
            for (int j : nearby)
            {
                if (count == 8) break;
                const std::size_t at = 3 * static_cast<std::size_t>(j);
                values[count++] = (res_[at] + res_[at + 1] + res_[at + 2]) / 3;
            }
            std::sort(values, values + count);
            smooth_[i] = count == 0 ? 0 : count % 2 == 1 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2;
        }

        const Scene& scene_;
        const Samples& s_;
        std::vector<float> res_;
        FitSettings settings_;
        SampleGrid grid_;
        std::vector<std::vector<int>> neighbours_;
        std::vector<float> smooth_;
    };

    // LightBrightness, LightHue and LightSaturation whose bake colour is
    // closest to an amplitude (bytes at a full term): the hue and saturation
    // of smallest angle, then the brightness by least squares.
    inline void ColourOf(const Vec3& amplitude, double levelBrightness, double& brightness, int& hue, int& saturation)
    {
        brightness = 0; hue = 0; saturation = 255;
        const Vec3 a = amplitude * (1 / 255.0);
        if (Length(a) <= 0) return;
        const Vec3 direction = Normalized(a);
        double bestCosine = -std::numeric_limits<double>::infinity();
        Vec3 bestUnit;
        for (int h = 0; h < 256; ++h)
            for (int s = 0; s < 256; ++s)
            {
                const Vec3 unit = HsvColour(h, s, 1);
                const double cosine = Dot(unit, direction) / Length(unit);
                if (cosine > bestCosine) { bestCosine = cosine; bestUnit = unit; hue = h; saturation = s; }
            }
        const Vec3 unit = bestUnit * (std::max)(levelBrightness, 1e-6);
        brightness = Dot(a, unit) / Dot(unit, unit);
    }

    // The chance a stand-in sits where an author's light was, by its peak
    // (bytes): the pooled precision the OpenSCCT Editor measured on fifteen
    // maps with a known editor copy.
    inline double FitConfidence(double peak)
    {
        return peak >= 40 ? 0.56 : peak >= 30 ? 0.35 : peak >= 20 ? 0.26 : 0.23;
    }

    // The fit gate: stand-ins are fitted only when the remaining lights'
    // rebake is at least this far from the lightmaps (mean bytes) and at
    // least this share of the samples hold unexplained light above the noise
    // floor. Below either, the residual is mostly model error and a fit
    // places noise lights.
    constexpr double kFitGateError = 2.0;
    constexpr double kFitGateShare = 0.01;
    // Lights of an editor copy whose support is under this are doubtful.
    constexpr double kReviewSupport = 0.3;
    // An editor copy of another bake is taken when the bake holds at least
    // this share of its lights' predicted light.
    constexpr double kStaleTwinSupport = 0.5;

    // A coordinate as the editor's text export writes a float.
    inline std::string FloatText(double value)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%.6f", static_cast<double>(static_cast<float>(value)));
        return text;
    }

    // A fitted light as a Light actor: a bake-only static light, as the cook
    // strips, with its LightRadius stored.
    inline std::string StandInActor(const FittedLight& fitted, double levelBrightness, const std::string& name)
    {
        double brightness = 0;
        int hue = 0, saturation = 255;
        ColourOf(fitted.amplitude, levelBrightness, brightness, hue, saturation);
        char numbers[160];
        std::snprintf(numbers, sizeof(numbers), "    LightBrightness=%.2f\n    LightHue=%d\n    LightSaturation=%d\n    LightRadius=%.2f\n",
            (std::max)(0.0, brightness), hue, saturation, (std::max)(1.0, fitted.radius / 25 - 1));
        return "Begin Actor Class=Light Name=" + name + "\n" + numbers
            + "    Location=(X=" + FloatText(fitted.location.x) + ",Y=" + FloatText(fitted.location.y)
            + ",Z=" + FloatText(fitted.location.z) + ")\n    Group=\"RecoveredLights\"\nEnd Actor\n";
    }
}
