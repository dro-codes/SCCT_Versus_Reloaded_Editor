#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// File > Export > Map to OBJ: the built level's BSP surfaces and its static
// meshes as Wavefront .obj + .mtl, with each texture as a PNG, for Blender.
//
// Axes. The file is Z up and right-handed, Blender's own frame: an Unreal
// point (X, Y, Z) is written (X, -Y, Z) times the scale. Unreal is
// left-handed (X forward, Y right, Z up), so Y is negated to keep the map
// the right way round rather than mirrored. Import it in Blender with
// Forward Y and Up Z. Faces wind counter-clockwise seen from the front, as
// OBJ expects; vt is (u, 1 - v), since Unreal's v runs down the texture.
//
// Units. The scale is metres per Unreal unit. The default, 1 unit = 1 cm,
// is 0.01, which matches the stock Blender exports and Blender's metres.
namespace Workflow::ObjExport
{
using V3 = std::array<double, 3>;
using V2 = std::array<double, 2>;

// ---------------------------------------------------------------------------
// Geometry

inline V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline double Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline V3 Cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
inline double Length(const V3& a) { return std::sqrt(Dot(a, a)); }
inline V3 Normalized(const V3& a)
{
    const double l = Length(a);
    return l > 0 ? V3{a[0] / l, a[1] / l, a[2] / l} : V3{0, 0, 0};
}

// An Unreal point or direction in the file's frame (see the top of the file).
inline V3 ToFile(const V3& u, double scale) { return {u[0] * scale, -u[1] * scale, u[2] * scale}; }

// A polygon's normal from all its corners (Newell's method), so three
// corners in a line at the start do not matter. Not normalised.
inline V3 Newell(const std::vector<V3>& p)
{
    V3 n{0, 0, 0};
    for (size_t i = 0; i < p.size(); ++i)
    {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % p.size()];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    return n;
}

// Unreal's FRotationMatrix for pitch, yaw and roll in 65536ths of a turn:
// the rotated point is x * X + y * Y + z * Z.
struct Axes { V3 x, y, z; };
inline Axes RotationAxes(int pitch, int yaw, int roll)
{
    constexpr double turn = 3.14159265358979323846 / 32768.0;
    const double p = pitch * turn, y = yaw * turn, r = roll * turn;
    const double sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(r), cr = std::cos(r);
    return {{cp * cy, cp * sy, sp},
            {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
            {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
}

// Where a static mesh actor puts its mesh: the mesh's own point, less the
// PrePivot, scaled by DrawScale * DrawScale3D, rotated, then moved to the
// actor's Location (Unreal's LocalToWorld).
struct Placement
{
    V3 location{0, 0, 0}, prePivot{0, 0, 0}, scale{1, 1, 1};
    std::array<int, 3> rotation{0, 0, 0};
};
inline V3 Place(const V3& local, const Placement& p, const Axes& a)
{
    const V3 s{(local[0] - p.prePivot[0]) * p.scale[0], (local[1] - p.prePivot[1]) * p.scale[1], (local[2] - p.prePivot[2]) * p.scale[2]};
    return {p.location[0] + s[0] * a.x[0] + s[1] * a.y[0] + s[2] * a.z[0],
            p.location[1] + s[0] * a.x[1] + s[1] * a.y[1] + s[2] * a.z[1],
            p.location[2] + s[0] * a.x[2] + s[1] * a.y[2] + s[2] * a.z[2]};
}
// A local normal through the same placement (the inverse transpose: divide
// by the scale, then rotate). Normalised.
inline V3 PlaceNormal(const V3& n, const Placement& p, const Axes& a)
{
    V3 s{};
    for (int i = 0; i < 3; ++i) s[i] = p.scale[i] != 0 ? n[i] / p.scale[i] : 0;
    return Normalized({s[0] * a.x[0] + s[1] * a.y[0] + s[2] * a.z[0], s[0] * a.x[1] + s[1] * a.y[1] + s[2] * a.z[1],
                       s[0] * a.x[2] + s[1] * a.y[2] + s[2] * a.z[2]});
}
// A negative scale on one or three axes turns the mesh inside out: its
// triangles are written the other way round.
inline bool Mirrors(const Placement& p) { return p.scale[0] * p.scale[1] * p.scale[2] < 0; }

// ---------------------------------------------------------------------------
// Scene

// A material as the file names it, and the texture it draws with (an index
// into Scene::textures, or -1 for none).
struct Material
{
    std::string name;
    int texture = -1;
};
struct Texture
{
    std::string file;   // relative to the .obj, e.g. "textures/Pkg.Group.Name.png"; empty when not written
    bool alpha = false; // cut-out or alpha texture: also its opacity map
};
// One built BSP polygon: its corners (indices into Scene::points, Unreal
// units), their texture coordinates (Unreal's u, v in texture widths) and
// the node's plane normal, which says which way it faces.
struct Polygon
{
    int material = -1;
    std::vector<int> points;
    std::vector<V2> uvs;
    V3 normal{0, 0, 1};
    bool sky = false; // in a SkyZoneInfo's zone: written as an object of its own
};
// A static mesh in its own units: welded positions and texture coordinates,
// and triangles in Unreal's order with the mesh's material slot and
// smoothing-group mask.
struct Mesh
{
    struct Triangle
    {
        std::array<int, 3> v{}, t{};
        int slot = 0;
        std::uint32_t smoothing = 0;
    };
    std::vector<V3> positions;
    std::vector<V2> uvs;
    std::vector<Triangle> triangles;
};
// A static mesh actor: its name, its mesh, where it puts it and the
// material each slot draws with (the mesh's own or the actor's Skins).
struct Instance
{
    std::string name;
    int mesh = -1;
    Placement placement;
    std::vector<int> slots;
};
struct Scene
{
    std::vector<V3> points;
    std::vector<Polygon> polygons;
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
    std::vector<Material> materials;
    std::vector<Texture> textures;
};

// Characters a file name, material or object name can keep; others become _.
inline std::string SafeName(const std::string& text)
{
    std::string out;
    for (unsigned char c : text)
        out += (std::isalnum(c) || c == '_' || c == '-' || c == '.') ? static_cast<char>(c) : '_';
    if (out.empty()) out = "_";
    return out;
}
// A name not yet in `used` (case folded, as Windows file names are),
// which it is added to: "a", then "a_2", "a_3"...
inline std::string UniqueName(const std::string& wanted, std::set<std::string>& used)
{
    auto fold = [](std::string s) { for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
    std::string name = wanted;
    for (int n = 2; used.count(fold(name)); ++n) name = wanted + "_" + std::to_string(n);
    used.insert(fold(name));
    return name;
}

// Per-corner normals for a mesh in its own units: each corner takes the
// area-weighted face normals of the triangles around its position that share
// a smoothing group with its own triangle (a triangle with no group is flat).
// Indexed [triangle * 3 + corner].
inline std::vector<V3> SmoothNormals(const Mesh& mesh)
{
    const size_t n = mesh.triangles.size();
    std::vector<V3> face(n);
    std::vector<std::vector<int>> around(mesh.positions.size());
    for (size_t i = 0; i < n; ++i)
    {
        const auto& t = mesh.triangles[i];
        bool valid = true;
        for (int v : t.v) valid = valid && v >= 0 && v < static_cast<int>(mesh.positions.size());
        if (!valid) continue;
        const auto& a = mesh.positions[t.v[0]];
        // Unreal's front face is clockwise in its left-handed frame, the same
        // turn as counter-clockwise once mirrored: the cross product of
        // (c - a) and (b - a) points out of it.
        face[i] = Cross(Sub(mesh.positions[t.v[2]], a), Sub(mesh.positions[t.v[1]], a));
        for (int v : t.v) around[v].push_back(static_cast<int>(i));
    }
    std::vector<V3> out(n * 3);
    for (size_t i = 0; i < n; ++i)
    {
        const auto& t = mesh.triangles[i];
        for (int c = 0; c < 3; ++c)
        {
            V3 sum = face[i];
            const int v = t.v[c];
            if (t.smoothing && v >= 0 && v < static_cast<int>(around.size()))
                for (int other : around[v])
                    if (other != static_cast<int>(i) && (mesh.triangles[other].smoothing & t.smoothing))
                        for (int k = 0; k < 3; ++k) sum[k] += face[other][k];
            out[i * 3 + c] = Normalized(sum);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// OBJ and MTL

struct Totals
{
    size_t vertices = 0, faces = 0, objects = 0;
};

namespace Detail
{
inline void Number(std::string& out, double v)
{
    if (!std::isfinite(v) || std::abs(v) < 5e-7) v = 0;
    char text[32];
    const int n = std::snprintf(text, sizeof(text), "%.6f", v);
    // Trailing zeros are noise in a file this size.
    int end = n;
    while (end > 1 && text[end - 1] == '0') --end;
    if (end > 0 && text[end - 1] == '.') --end;
    out.append(text, end);
}
inline void Triple(std::string& out, const char* tag, const V3& v)
{
    out += tag;
    for (double x : v) { out += ' '; Number(out, x); }
    out += '\n';
}
inline void Pair(std::string& out, const V2& v)
{
    out += "vt ";
    Number(out, v[0]);
    out += ' ';
    Number(out, v[1]);
    out += '\n';
}
inline void Flush(std::ostream& file, std::string& out, bool force = false)
{
    if (force || out.size() > (1u << 20)) { file.write(out.data(), static_cast<std::streamsize>(out.size())); out.clear(); }
}
inline const char* MaterialName(const Scene& scene, int material)
{
    return material >= 0 && material < static_cast<int>(scene.materials.size()) ? scene.materials[material].name.c_str() : "Default";
}
} // namespace Detail

// The .obj: the BSP as one object (BSP, and BSP_SkyZone for the sky zone's
// room when there is one), then one object per static mesh actor, named as
// the actor. `scale` is metres per Unreal unit. The BSP's points are written
// once and shared by its polygons; each polygon faces the way its plane does.
inline Totals WriteObj(std::ostream& file, const Scene& scene, double scale, const std::string& mtlFile, const std::string& header)
{
    using namespace Detail;
    Totals totals;
    std::string out;
    out += "# " + header + "\n";
    out += "# Z up, right-handed: Unreal (X, Y, Z) is written (X, -Y, Z) x ";
    Number(out, scale);
    out += " (metres). Blender: File > Import > Wavefront (.obj), Forward Axis Y, Up Axis Z.\n";
    out += "mtllib " + mtlFile + "\n";
    size_t v = 0, vt = 0, vn = 0; // written so far (OBJ indices are 1-based)

    for (int pass = 0; pass < 2; ++pass)
    {
        // Polygons of this object, by material.
        std::map<int, std::vector<size_t>> byMaterial;
        for (size_t i = 0; i < scene.polygons.size(); ++i)
            if (scene.polygons[i].sky == (pass == 1)) byMaterial[scene.polygons[i].material].push_back(i);
        if (byMaterial.empty()) continue;
        out += pass ? "o BSP_SkyZone\n" : "o BSP\n";
        ++totals.objects;
        // The points this object uses, written once each.
        std::vector<long long> written(scene.points.size(), -1);
        for (const auto& [material, list] : byMaterial)
            for (size_t i : list)
                for (int p : scene.polygons[i].points)
                    if (p >= 0 && p < static_cast<int>(scene.points.size()) && written[p] < 0)
                    {
                        written[p] = static_cast<long long>(++v);
                        Triple(out, "v", ToFile(scene.points[p], scale));
                        Flush(file, out);
                    }
        for (const auto& [material, list] : byMaterial)
        {
            out += "usemtl ";
            out += MaterialName(scene, material);
            out += "\ns off\n";
            for (size_t i : list)
            {
                const auto& poly = scene.polygons[i];
                // Corners in order, a corner repeated straight after itself dropped.
                std::vector<size_t> corners;
                for (size_t c = 0; c < poly.points.size(); ++c)
                {
                    const int p = poly.points[c];
                    if (p < 0 || p >= static_cast<int>(scene.points.size())) { corners.clear(); break; }
                    if (!corners.empty() && poly.points[corners.back()] == p) continue;
                    corners.push_back(c);
                }
                while (corners.size() > 1 && poly.points[corners.front()] == poly.points[corners.back()]) corners.pop_back();
                if (corners.size() < 3) continue;
                std::vector<V3> at;
                for (size_t c : corners) at.push_back(ToFile(scene.points[poly.points[c]], 1));
                const V3 normal = Normalized(ToFile(poly.normal, 1));
                if (Dot(Newell(at), normal) < 0) std::reverse(corners.begin(), corners.end());
                Triple(out, "vn", normal);
                ++vn;
                for (size_t c : corners)
                    Pair(out, {c < poly.uvs.size() ? poly.uvs[c][0] : 0, c < poly.uvs.size() ? 1 - poly.uvs[c][1] : 0});
                out += 'f';
                for (size_t k = 0; k < corners.size(); ++k)
                {
                    out += ' ' + std::to_string(written[poly.points[corners[k]]]) + '/' + std::to_string(vt + k + 1) + '/' + std::to_string(vn);
                }
                out += '\n';
                vt += corners.size();
                ++totals.faces;
                Flush(file, out);
            }
        }
    }

    // Static mesh actors.
    std::vector<std::vector<V3>> normals(scene.meshes.size());
    std::vector<bool> normalsReady(scene.meshes.size(), false);
    for (const auto& instance : scene.instances)
    {
        if (instance.mesh < 0 || instance.mesh >= static_cast<int>(scene.meshes.size())) continue;
        const auto& mesh = scene.meshes[instance.mesh];
        if (mesh.triangles.empty() || mesh.positions.empty()) continue;
        if (!normalsReady[instance.mesh]) { normals[instance.mesh] = SmoothNormals(mesh); normalsReady[instance.mesh] = true; }
        const auto& corner = normals[instance.mesh];
        const auto axes = RotationAxes(instance.placement.rotation[0], instance.placement.rotation[1], instance.placement.rotation[2]);
        const bool mirrored = Mirrors(instance.placement);
        out += "o " + instance.name + "\n";
        ++totals.objects;
        const size_t vBase = v, tBase = vt;
        for (const auto& p : mesh.positions) Triple(out, "v", ToFile(Place(p, instance.placement, axes), scale));
        v += mesh.positions.size();
        Flush(file, out);
        for (const auto& t : mesh.uvs) Pair(out, {t[0], 1 - t[1]});
        if (mesh.uvs.empty()) Pair(out, {0, 0});
        vt += std::max<size_t>(1, mesh.uvs.size());
        // Normals, shared where the same one recurs.
        std::map<std::array<long long, 3>, size_t> normalIndex;
        std::vector<size_t> cornerNormal(corner.size());
        for (size_t i = 0; i < corner.size(); ++i)
        {
            const V3 n = ToFile(PlaceNormal(corner[i], instance.placement, axes), 1);
            const std::array<long long, 3> key{std::llround(n[0] * 1e5), std::llround(n[1] * 1e5), std::llround(n[2] * 1e5)};
            auto [it, added] = normalIndex.emplace(key, vn + normalIndex.size() + 1);
            if (added) Triple(out, "vn", n);
            cornerNormal[i] = it->second;
        }
        vn += normalIndex.size();
        Flush(file, out);
        // Faces by material slot, then smoothing.
        std::vector<size_t> order(mesh.triangles.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const auto& x = mesh.triangles[a];
            const auto& y = mesh.triangles[b];
            return x.slot != y.slot ? x.slot < y.slot : (x.smoothing != 0) < (y.smoothing != 0);
        });
        int currentSlot = -2, currentSmooth = -1;
        for (size_t i : order)
        {
            const auto& t = mesh.triangles[i];
            bool valid = true;
            for (int c = 0; c < 3; ++c) valid = valid && t.v[c] >= 0 && t.v[c] < static_cast<int>(mesh.positions.size());
            if (!valid || t.v[0] == t.v[1] || t.v[1] == t.v[2] || t.v[0] == t.v[2]) continue;
            if (t.slot != currentSlot)
            {
                currentSlot = t.slot;
                const int material = t.slot >= 0 && t.slot < static_cast<int>(instance.slots.size()) ? instance.slots[t.slot]
                                     : instance.slots.empty()                                       ? -1
                                                                                                     : instance.slots[0];
                out += "usemtl ";
                out += MaterialName(scene, material);
                out += '\n';
                currentSmooth = -1;
            }
            const int smooth = t.smoothing ? 1 : 0;
            if (smooth != currentSmooth) { out += smooth ? "s 1\n" : "s off\n"; currentSmooth = smooth; }
            std::array<int, 3> c{0, 1, 2};
            if (mirrored) std::swap(c[1], c[2]);
            out += 'f';
            for (int k : c)
            {
                const int uv = t.t[k] >= 0 && t.t[k] < static_cast<int>(mesh.uvs.size()) ? t.t[k] : 0;
                out += ' ' + std::to_string(vBase + t.v[k] + 1) + '/' + std::to_string(tBase + uv + 1) + '/' +
                       std::to_string(cornerNormal[i * 3 + k]);
            }
            out += '\n';
            ++totals.faces;
            Flush(file, out);
        }
    }
    totals.vertices = v;
    Flush(file, out, true);
    return totals;
}

// The .mtl: each material white-lit with its texture as the diffuse map, and
// as the opacity map too for cut-out and alpha textures. A material without
// a texture is plain grey.
inline void WriteMtl(std::ostream& file, const Scene& scene, const std::string& header)
{
    std::string out = "# " + header + "\n";
    auto one = [&](const std::string& name, int texture) {
        const Texture* t = texture >= 0 && texture < static_cast<int>(scene.textures.size()) && !scene.textures[texture].file.empty()
                               ? &scene.textures[texture]
                               : nullptr;
        out += "\nnewmtl " + name + "\n";
        out += t ? "Kd 1 1 1\n" : "Kd 0.6 0.6 0.6\n";
        out += "Ka 0 0 0\nKs 0 0 0\nNs 10\nillum 1\n";
        if (t)
        {
            out += "map_Kd " + t->file + "\n";
            if (t->alpha) out += "map_d " + t->file + "\n";
        }
    };
    one("Default", -1);
    for (const auto& m : scene.materials) one(m.name, m.texture);
    file.write(out.data(), static_cast<std::streamsize>(out.size()));
}

// ---------------------------------------------------------------------------
// Textures

// Unreal's ETextureFormat values the exporter reads.
enum Format : int { kP8 = 0, kDXT1 = 3, kRGB8 = 4, kRGBA8 = 5, kDXT3 = 7, kDXT5 = 8, kL8 = 9 };

struct Image
{
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, top row first
};

inline bool Readable(int format)
{
    return format == kP8 || format == kDXT1 || format == kRGB8 || format == kRGBA8 || format == kDXT3 || format == kDXT5 || format == kL8;
}
// Bytes the top mip of a texture this size holds in this format (DXT works
// in 4 x 4 blocks).
inline size_t MipBytes(int format, int width, int height)
{
    const size_t w = static_cast<size_t>(std::max(width, 0)), h = static_cast<size_t>(std::max(height, 0));
    const size_t blocks = ((w + 3) / 4) * ((h + 3) / 4);
    switch (format)
    {
    case kP8: case kL8: return w * h;
    case kRGB8: return w * h * 3;
    case kRGBA8: return w * h * 4;
    case kDXT1: return blocks * 8;
    case kDXT3: case kDXT5: return blocks * 16;
    default: return 0;
    }
}

namespace Detail
{
inline void Colour565(std::uint16_t c, std::uint8_t out[3])
{
    out[0] = static_cast<std::uint8_t>(((c >> 11) & 31) * 255 / 31);
    out[1] = static_cast<std::uint8_t>(((c >> 5) & 63) * 255 / 63);
    out[2] = static_cast<std::uint8_t>((c & 31) * 255 / 31);
}
// One DXT colour block (8 bytes) into a 4 x 4 RGBA tile. DXT1 with
// c0 <= c1 has three colours and transparent black.
inline void ColourBlock(const std::uint8_t* b, bool dxt1, std::uint8_t tile[16][4])
{
    const std::uint16_t c0 = static_cast<std::uint16_t>(b[0] | (b[1] << 8)), c1 = static_cast<std::uint16_t>(b[2] | (b[3] << 8));
    std::uint8_t p[4][4] = {};
    Colour565(c0, p[0]);
    Colour565(c1, p[1]);
    p[0][3] = p[1][3] = p[2][3] = p[3][3] = 255;
    if (!dxt1 || c0 > c1)
        for (int k = 0; k < 3; ++k)
        {
            p[2][k] = static_cast<std::uint8_t>((2 * p[0][k] + p[1][k] + 1) / 3);
            p[3][k] = static_cast<std::uint8_t>((p[0][k] + 2 * p[1][k] + 1) / 3);
        }
    else
    {
        for (int k = 0; k < 3; ++k) p[2][k] = static_cast<std::uint8_t>((p[0][k] + p[1][k]) / 2);
        p[3][0] = p[3][1] = p[3][2] = p[3][3] = 0;
    }
    const std::uint32_t bits = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<std::uint32_t>(b[7]) << 24);
    for (int i = 0; i < 16; ++i) std::memcpy(tile[i], p[(bits >> (2 * i)) & 3], 4);
}
inline void Dxt5Alpha(const std::uint8_t* b, std::uint8_t tile[16][4])
{
    std::uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1])
        for (int i = 1; i < 7; ++i) a[i + 1] = static_cast<std::uint8_t>(((7 - i) * a[0] + i * a[1] + 3) / 7);
    else
    {
        for (int i = 1; i < 5; ++i) a[i + 1] = static_cast<std::uint8_t>(((5 - i) * a[0] + i * a[1] + 2) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) tile[i][3] = a[(bits >> (3 * i)) & 7];
}
} // namespace Detail

// The top mip as RGBA. A P8 palette is in Unreal's FColor byte order
// (B, G, R, A). Throws for a format it does not read or data too short.
inline Image Decode(int format, int width, int height, const std::uint8_t* data, size_t size,
                    const std::vector<std::array<std::uint8_t, 4>>& palette = {})
{
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) throw std::runtime_error("bad texture size");
    if (!Readable(format)) throw std::runtime_error("texture format " + std::to_string(format) + " is not read");
    if (!data || size < MipBytes(format, width, height)) throw std::runtime_error("texture data is short");
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.assign(static_cast<size_t>(width) * height * 4, 255);
    auto px = [&](int x, int y) { return &image.rgba[(static_cast<size_t>(y) * width + x) * 4]; };
    const size_t count = static_cast<size_t>(width) * height;
    switch (format)
    {
    case kP8:
        for (size_t i = 0; i < count; ++i)
        {
            const auto& c = data[i] < palette.size() ? palette[data[i]] : std::array<std::uint8_t, 4>{0, 0, 0, 255};
            std::uint8_t* o = &image.rgba[i * 4];
            o[0] = c[2]; o[1] = c[1]; o[2] = c[0]; o[3] = c[3];
        }
        break;
    case kL8:
        for (size_t i = 0; i < count; ++i) { std::uint8_t* o = &image.rgba[i * 4]; o[0] = o[1] = o[2] = data[i]; }
        break;
    case kRGB8:
        for (size_t i = 0; i < count; ++i) { std::uint8_t* o = &image.rgba[i * 4]; o[0] = data[i * 3 + 2]; o[1] = data[i * 3 + 1]; o[2] = data[i * 3]; }
        break;
    case kRGBA8:
        for (size_t i = 0; i < count; ++i)
        {
            std::uint8_t* o = &image.rgba[i * 4];
            o[0] = data[i * 4 + 2]; o[1] = data[i * 4 + 1]; o[2] = data[i * 4]; o[3] = data[i * 4 + 3];
        }
        break;
    default:
    {
        const size_t blockBytes = format == kDXT1 ? 8 : 16;
        const int across = (width + 3) / 4, down = (height + 3) / 4;
        for (int by = 0; by < down; ++by)
            for (int bx = 0; bx < across; ++bx)
            {
                const std::uint8_t* b = data + (static_cast<size_t>(by) * across + bx) * blockBytes;
                std::uint8_t tile[16][4];
                Detail::ColourBlock(format == kDXT1 ? b : b + 8, format == kDXT1, tile);
                if (format == kDXT3)
                    for (int i = 0; i < 16; ++i)
                    {
                        const int nibble = (b[i / 2] >> (4 * (i & 1))) & 15;
                        tile[i][3] = static_cast<std::uint8_t>(nibble * 17);
                    }
                else if (format == kDXT5)
                    Detail::Dxt5Alpha(b, tile);
                for (int i = 0; i < 16; ++i)
                {
                    const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x < width && y < height) std::memcpy(px(x, y), tile[i], 4);
                }
            }
    }
    }
    return image;
}

// ---------------------------------------------------------------------------
// PNG

inline std::uint32_t Crc32(const std::uint8_t* data, size_t size, std::uint32_t crc = 0)
{
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n)
        {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
inline std::uint32_t Adler32(const std::uint8_t* data, size_t size)
{
    std::uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; ++i) { a = (a + data[i]) % 65521; b = (b + a) % 65521; }
    return (b << 16) | a;
}
// A zlib stream of stored (uncompressed) deflate blocks: valid everywhere,
// just large. The editor passes zlib's own compressor instead.
inline std::string StoredZlib(const std::string& raw)
{
    std::string out = "\x78\x01";
    size_t at = 0;
    do
    {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + n == raw.size();
        out += static_cast<char>(last ? 1 : 0);
        out += static_cast<char>(n & 0xFF);
        out += static_cast<char>(n >> 8);
        out += static_cast<char>(~n & 0xFF);
        out += static_cast<char>((~n >> 8) & 0xFF);
        out.append(raw, at, n);
        at += n;
    } while (at < raw.size());
    const auto adler = Adler32(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size());
    for (int s = 24; s >= 0; s -= 8) out += static_cast<char>((adler >> s) & 0xFF);
    return out;
}
using Compressor = std::function<std::string(const std::string&)>;

// The image as an 8-bit PNG, RGBA when `alpha`, else RGB. Each row gets the
// filter that leaves it smallest (sum of magnitudes), as encoders usually do.
inline std::string Png(const Image& image, bool alpha, const Compressor& compress = StoredZlib)
{
    if (image.width <= 0 || image.height <= 0 || image.rgba.size() != static_cast<size_t>(image.width) * image.height * 4)
        throw std::runtime_error("bad image");
    const int channels = alpha ? 4 : 3;
    const size_t stride = static_cast<size_t>(image.width) * channels;
    std::string raw;
    raw.reserve((stride + 1) * image.height);
    std::vector<std::uint8_t> previous(stride, 0), row(stride), best, trial(stride);
    for (int y = 0; y < image.height; ++y)
    {
        for (int x = 0; x < image.width; ++x)
            for (int c = 0; c < channels; ++c) row[static_cast<size_t>(x) * channels + c] = image.rgba[(static_cast<size_t>(y) * image.width + x) * 4 + c];
        long long bestCost = -1;
        int bestFilter = 0;
        for (int filter = 0; filter < 5; ++filter)
        {
            long long cost = 0;
            for (size_t i = 0; i < stride; ++i)
            {
                const int a = i >= static_cast<size_t>(channels) ? row[i - channels] : 0, b = previous[i],
                          c = i >= static_cast<size_t>(channels) ? previous[i - channels] : 0;
                int predicted = 0;
                if (filter == 1) predicted = a;
                else if (filter == 2) predicted = b;
                else if (filter == 3) predicted = (a + b) / 2;
                else if (filter == 4)
                {
                    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    predicted = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                }
                trial[i] = static_cast<std::uint8_t>(row[i] - predicted);
                cost += trial[i] < 128 ? trial[i] : 256 - trial[i];
            }
            if (bestCost < 0 || cost < bestCost) { bestCost = cost; bestFilter = filter; best = trial; }
        }
        raw += static_cast<char>(bestFilter);
        raw.append(reinterpret_cast<const char*>(best.data()), best.size());
        previous = row;
    }
    std::string png("\x89PNG\r\n\x1a\n", 8);
    auto chunk = [&](const char* type, const std::string& body) {
        const std::uint32_t n = static_cast<std::uint32_t>(body.size());
        for (int s = 24; s >= 0; s -= 8) png += static_cast<char>((n >> s) & 0xFF);
        std::string typed = std::string(type, 4) + body;
        png += typed;
        const auto crc = Crc32(reinterpret_cast<const std::uint8_t*>(typed.data()), typed.size());
        for (int s = 24; s >= 0; s -= 8) png += static_cast<char>((crc >> s) & 0xFF);
    };
    std::string ihdr;
    for (std::uint32_t v : {static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height)})
        for (int s = 24; s >= 0; s -= 8) ihdr += static_cast<char>((v >> s) & 0xFF);
    ihdr += static_cast<char>(8);
    ihdr += static_cast<char>(alpha ? 6 : 2);
    ihdr += std::string(3, '\0');
    chunk("IHDR", ihdr);
    chunk("IDAT", compress(raw));
    chunk("IEND", "");
    return png;
}
} // namespace Workflow::ObjExport
