#pragma once
// Import OBJ (Static Mesh Browser > File > Import OBJ..., and File > Import OBJ Mesh...): a
// Wavefront OBJ and its MTL files read, turned into Unreal's axes and units, given collision (none,
// a box, the mesh's own triangles, or convex hulls built from it), cut into parts the game can
// collide with, and written as the ASE text the editor's static mesh importer reads. Pure: no
// engine, so the tests compile it alone. ObjImportNative.inl runs the import.
//
// Facts the code relies on:
// - The game marks each collision triangle a trace tests in one table of 5512 slots and never
//   checks the index: a mesh with more triangles in its own set or in its hulls overwrites the
//   game's globals and crashes it. The editor also keeps at most 15000 per-polygon collision
//   triangles, and a collision BSP holds at most 32767 nodes (signed 16-bit children); past that
//   the triangles it never reached do not collide.
// - Hulls are MCDCX objects in the ASE. The importer turns objects whose names start MCD half a
//   turn about Z against the rest of the mesh, so they are written turned the other way.
// - The game ignores a hull whose faces overlap. A clean hull has flat faces (to 1e-5 of its
//   largest coordinate) and one face per set of corners: written as triangles, a hull with V
//   corners then has exactly 2V - 4 of them.
// - ASE positions are 3ds Max's: Unreal (x, y, z) is written (x, -y, z), so each face's corners
//   are written in reverse; V is written as 1 - v.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ObjImport
{
using Vec3 = std::array<double, 3>;
using Vec2 = std::array<double, 2>;

constexpr size_t kCollisionTriangles = 5512;   // per mesh, own triangles and hulls each: more crashes the game
constexpr size_t kPerPolygonTriangles = 15000; // the editor's per-polygon collision set stops here
constexpr int kBspNodes = 32767;               // a collision BSP's node cap
constexpr double kHullTolerance = 1e-5;        // a hull's flatness, times its largest coordinate (at least 1)
constexpr size_t kHullCorners = 128;           // a hull with more corners is simplified to this many
constexpr size_t kNameLength = 63;

// ---- small vector maths ----------------------------------------------------------------------
inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline Vec3 Mul(const Vec3& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
inline double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Unit(const Vec3& a)
{
    const double l = Length(a);
    return l > 1e-300 ? Mul(a, 1.0 / l) : Vec3{0, 0, 0};
}
// A polygon's normal by Newell's method (not unit): along (v1 - v0) x (v2 - v0) for a triangle.
inline Vec3 Newell(const std::vector<Vec3>& points)
{
    Vec3 n{0, 0, 0};
    for (size_t i = 0; i < points.size(); ++i)
    {
        const auto& p = points[i];
        const auto& q = points[(i + 1) % points.size()];
        n[0] += (p[1] - q[1]) * (p[2] + q[2]);
        n[1] += (p[2] - q[2]) * (p[0] + q[0]);
        n[2] += (p[0] - q[0]) * (p[1] + q[1]);
    }
    return n;
}

// ---- names -----------------------------------------------------------------------------------
inline std::string Lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// Package, group and object names the editor's commands take: a letter or _, then letters,
// digits and _, at most 63.
inline bool ValidName(const std::string& s)
{
    if (s.empty() || s.size() > kNameLength) return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}
// Any text made a valid name: other characters become _, runs of _ are kept to one, a leading
// digit gets a _ before it. Empty text gives the fallback.
inline std::string SafeName(const std::string& text, const std::string& fallback = "Mesh")
{
    std::string out;
    for (char c : text)
    {
        const bool ok = static_cast<unsigned char>(c) < 128 && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
        const char next = ok ? c : '_';
        if (next == '_' && !out.empty() && out.back() == '_') continue;
        out += next;
    }
    while (!out.empty() && out.back() == '_' && out.size() > 1) out.pop_back();
    if (out.empty() || out == "_") out = fallback;
    if (std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
    if (out.size() > kNameLength) out.resize(kNameLength);
    return out;
}
// OBJ objects or groups named UCX_... (the usual export convention) or MCD... are collision
// hulls, never drawn.
inline bool CollisionName(const std::string& name)
{
    const auto l = Lower(name);
    return l.rfind("ucx_", 0) == 0 || l.rfind("mcd", 0) == 0;
}

// ---- text ------------------------------------------------------------------------------------
// The file's statements, one per entry with its line number: comments gone, a line ending in \
// joined to the next.
inline std::vector<std::pair<int, std::string>> Statements(const std::string& text)
{
    std::vector<std::pair<int, std::string>> out;
    std::string current;
    int line = 0, start = 1;
    size_t at = 0;
    while (at <= text.size())
    {
        auto end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string piece = text.substr(at, end - at);
        ++line;
        if (!piece.empty() && piece.back() == '\r') piece.pop_back();
        if (current.empty()) start = line;
        const bool more = !piece.empty() && piece.back() == '\\';
        if (more) piece.pop_back();
        current += piece;
        if (!more)
        {
            const auto hash = current.find('#');
            if (hash != std::string::npos) current.resize(hash);
            out.push_back({start, current});
            current.clear();
        }
        if (end == text.size()) break;
        at = end + 1;
    }
    if (!current.empty()) out.push_back({start, current});
    return out;
}
inline std::vector<std::string> Words(const std::string& s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}
// The text after the statement's keyword, spaces and all (file and material names hold spaces).
inline std::string Rest(const std::string& statement)
{
    size_t i = 0;
    while (i < statement.size() && std::isspace(static_cast<unsigned char>(statement[i]))) ++i;
    while (i < statement.size() && !std::isspace(static_cast<unsigned char>(statement[i]))) ++i;
    while (i < statement.size() && std::isspace(static_cast<unsigned char>(statement[i]))) ++i;
    size_t e = statement.size();
    while (e > i && std::isspace(static_cast<unsigned char>(statement[e - 1]))) --e;
    return statement.substr(i, e - i);
}
inline bool Number(const std::string& word, double& value)
{
    if (word.empty()) return false;
    char* end = nullptr;
    value = std::strtod(word.c_str(), &end);
    return end && *end == 0 && std::isfinite(value);
}
inline double NumberAt(const std::vector<std::string>& words, size_t i, int line, const char* what)
{
    double v = 0;
    if (i >= words.size() || !Number(words[i], v))
        throw std::runtime_error("Line " + std::to_string(line) + ": " + what + " needs a number here.");
    return v;
}

// ---- MTL -------------------------------------------------------------------------------------
struct Material
{
    std::string name;
    std::string diffuseMap; // map_Kd's file, as written (relative to the MTL file's folder)
    Vec3 diffuse{0.8, 0.8, 0.8};
};
// map_Kd's file name, after the options (-s 1 1 1, -o, -bm 0.5, -clamp on, -imfchan r...).
inline std::string MapFile(const std::string& rest)
{
    std::vector<size_t> starts;
    std::vector<std::string> words;
    for (size_t i = 0; i < rest.size();)
    {
        while (i < rest.size() && std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
        const size_t j0 = i;
        while (i < rest.size() && !std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
        if (i > j0) { starts.push_back(j0); words.push_back(rest.substr(j0, i - j0)); }
    }
    size_t i = 0;
    while (i < words.size() && words[i].size() > 1 && words[i][0] == '-')
    {
        const auto option = Lower(words[i++]);
        if (option == "-imfchan" || option == "-type") { if (i < words.size()) ++i; continue; }
        double ignored = 0;
        while (i < words.size() && (Number(words[i], ignored) || Lower(words[i]) == "on" || Lower(words[i]) == "off")) ++i;
    }
    // The file name keeps its own spaces: the rest of the text from its first word.
    if (i >= words.size()) return {};
    auto file = rest.substr(starts[i]);
    while (!file.empty() && std::isspace(static_cast<unsigned char>(file.back()))) file.pop_back();
    return file;
}
inline std::vector<Material> ParseMtl(const std::string& text)
{
    std::vector<Material> out;
    for (const auto& [line, statement] : Statements(text))
    {
        const auto words = Words(statement);
        if (words.empty()) continue;
        const auto key = Lower(words[0]);
        if (key == "newmtl") { out.push_back({}); out.back().name = Rest(statement); continue; }
        if (out.empty()) continue;
        if (key == "map_kd") out.back().diffuseMap = MapFile(Rest(statement));
        else if (key == "kd" && words.size() >= 4)
            for (int k = 0; k < 3; ++k) out.back().diffuse[k] = NumberAt(words, static_cast<size_t>(k) + 1, line, "Kd");
    }
    return out;
}

// ---- OBJ -------------------------------------------------------------------------------------
struct Corner { int v = -1, vt = -1; };
struct Face
{
    std::vector<Corner> corners;
    int material = -1;   // into File::materials; -1 before any usemtl
    int group = 0;       // into File::groups
    unsigned smoothing = 0;
    int line = 0;
};
struct File
{
    std::vector<Vec3> positions;
    std::vector<Vec2> uvs;
    std::vector<std::string> materials;       // usemtl names, in the order first used
    std::vector<std::string> groups{""};      // object/group names; 0 is faces before any o or g
    std::vector<std::string> libraries;       // mtllib file names
    std::vector<Face> faces;
    std::vector<std::string> warnings;
    size_t skippedFaces = 0;                  // faces with fewer than three corners
};
inline int ObjIndex(const std::string& word, size_t count, int line, const char* what)
{
    char* end = nullptr;
    const long value = std::strtol(word.c_str(), &end, 10);
    if (word.empty() || !end || *end) throw std::runtime_error("Line " + std::to_string(line) + ": " + what + " index '" + word + "' is not a number.");
    const long long resolved = value < 0 ? static_cast<long long>(count) + value : static_cast<long long>(value) - 1;
    if (value == 0 || resolved < 0 || resolved >= static_cast<long long>(count))
        throw std::runtime_error("Line " + std::to_string(line) + ": " + what + " " + word + " does not exist (the file has " +
                                 std::to_string(count) + " so far).");
    return static_cast<int>(resolved);
}
inline File ParseObj(const std::string& text)
{
    File file;
    std::unordered_map<std::string, int> groupIndex{{"", 0}}, materialIndex;
    std::string object, group;
    int currentGroup = 0, currentMaterial = -1;
    unsigned smoothing = 0;
    auto setGroup = [&]() {
        std::string key = object.empty() ? group : group.empty() ? object : object + "/" + group;
        auto found = groupIndex.find(key);
        if (found == groupIndex.end())
        {
            found = groupIndex.emplace(key, static_cast<int>(file.groups.size())).first;
            file.groups.push_back(key);
        }
        currentGroup = found->second;
    };
    for (const auto& [line, statement] : Statements(text))
    {
        const auto words = Words(statement);
        if (words.empty()) continue;
        const auto& key = words[0];
        if (key == "v")
            file.positions.push_back({NumberAt(words, 1, line, "v"), NumberAt(words, 2, line, "v"), NumberAt(words, 3, line, "v")});
        else if (key == "vt")
            file.uvs.push_back({NumberAt(words, 1, line, "vt"), words.size() > 2 ? NumberAt(words, 2, line, "vt") : 0.0});
        else if (key == "f" || key == "fo")
        {
            Face face;
            face.material = currentMaterial;
            face.group = currentGroup;
            face.smoothing = smoothing;
            face.line = line;
            for (size_t i = 1; i < words.size(); ++i)
            {
                const auto& w = words[i];
                const auto slash = w.find('/');
                Corner c;
                c.v = ObjIndex(w.substr(0, slash), file.positions.size(), line, "vertex");
                if (slash != std::string::npos)
                {
                    const auto second = w.find('/', slash + 1);
                    const auto uv = w.substr(slash + 1, second == std::string::npos ? std::string::npos : second - slash - 1);
                    if (!uv.empty()) c.vt = ObjIndex(uv, file.uvs.size(), line, "texture coordinate");
                }
                face.corners.push_back(c);
            }
            if (face.corners.size() < 3) { ++file.skippedFaces; continue; }
            file.faces.push_back(std::move(face));
        }
        else if (key == "o") { object = Rest(statement); group.clear(); setGroup(); }
        else if (key == "g") { group = Rest(statement); setGroup(); }
        else if (key == "usemtl")
        {
            const auto name = Rest(statement);
            auto found = materialIndex.find(name);
            if (found == materialIndex.end())
            {
                found = materialIndex.emplace(name, static_cast<int>(file.materials.size())).first;
                file.materials.push_back(name);
            }
            currentMaterial = found->second;
        }
        else if (key == "mtllib")
        {
            // Usually one file; several are separated by spaces, but a lone name may hold spaces.
            const auto rest = Rest(statement);
            if (!rest.empty()) file.libraries.push_back(rest);
        }
        else if (key == "s")
        {
            const auto value = words.size() > 1 ? Lower(words[1]) : std::string("off");
            double n = 0;
            smoothing = (value == "off" || !Number(value, n) || n <= 0) ? 0u : static_cast<unsigned>((static_cast<long long>(n) - 1) % 32 + 1);
        }
    }
    if (file.skippedFaces) file.warnings.push_back(std::to_string(file.skippedFaces) + " faces with fewer than three corners were left out.");
    return file;
}

// ---- triangles -------------------------------------------------------------------------------
// A polygon cut into triangles (corner indices), wound as the polygon: ear clipping, so
// concave n-gons come out right; a fan when the polygon has no area.
inline std::vector<std::array<int, 3>> Triangulate(const std::vector<Vec3>& polygon)
{
    const int n = static_cast<int>(polygon.size());
    std::vector<std::array<int, 3>> out;
    if (n < 3) return out;
    if (n == 3) { out.push_back({0, 1, 2}); return out; }
    const auto normal = Newell(polygon);
    auto fan = [&](const std::vector<int>& left) {
        for (size_t i = 1; i + 1 < left.size(); ++i) out.push_back({left[0], left[i], left[i + 1]});
    };
    std::vector<int> left(static_cast<size_t>(n));
    std::iota(left.begin(), left.end(), 0);
    if (Length(normal) < 1e-12) { fan(left); return out; }
    // Project onto the plane: u, w with u x w along the normal, so the polygon runs counter-clockwise.
    const auto nu = Unit(normal);
    const Vec3 seed = std::abs(nu[0]) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const auto u = Unit(Cross(seed, nu)), w = Cross(nu, u);
    std::vector<Vec2> flat;
    for (const auto& p : polygon) flat.push_back({Dot(p, u), Dot(p, w)});
    auto cross2 = [&](int a, int b, int c) {
        return (flat[b][0] - flat[a][0]) * (flat[c][1] - flat[a][1]) - (flat[b][1] - flat[a][1]) * (flat[c][0] - flat[a][0]);
    };
    while (left.size() > 3)
    {
        bool clipped = false;
        const size_t m = left.size();
        for (size_t i = 0; i < m && !clipped; ++i)
        {
            const int a = left[(i + m - 1) % m], b = left[i], c = left[(i + 1) % m];
            if (cross2(a, b, c) <= 1e-12) continue;
            bool inside = false;
            for (size_t k = 0; k < m && !inside; ++k)
            {
                const int p = left[k];
                if (p == a || p == b || p == c) continue;
                inside = cross2(a, b, p) >= 0 && cross2(b, c, p) >= 0 && cross2(c, a, p) >= 0;
            }
            if (inside) continue;
            out.push_back({a, b, c});
            left.erase(left.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
        }
        if (!clipped) { fan(left); return out; }
    }
    out.push_back({left[0], left[1], left[2]});
    return out;
}

enum class Up { Y, Z };
enum class Collision { None, Box, Polygons, Groups, Parts, File };

struct Options
{
    double scale = 1.0;   // Unreal units per OBJ unit (1: one OBJ unit is one Unreal unit, about 1 cm)
    Up up = Up::Y;
    Collision collision = Collision::Parts;
};
// A point from the file in Unreal's axes and units. Both conversions mirror (OBJ is right-handed,
// Unreal left-handed), so faces are wound the other way round as well.
inline Vec3 ToUnreal(const Vec3& p, const Options& o)
{
    return o.up == Up::Z ? Vec3{p[0] * o.scale, -p[1] * o.scale, p[2] * o.scale}
                         : Vec3{p[0] * o.scale, p[2] * o.scale, p[1] * o.scale};
}

struct Triangle
{
    std::array<Vec3, 3> p;   // Unreal, (p1 - p0) x (p2 - p0) facing out
    std::array<Vec2, 3> uv;  // V down the picture (1 - the OBJ's v)
    int slot = 0;
    unsigned smoothing = 0;
    int group = 0;
};
// A convex hull: its corners and triangles, (b - a) x (c - a) facing out.
struct Hull
{
    std::vector<Vec3> corners;
    std::vector<std::array<int, 3>> triangles;
};
struct Mesh
{
    std::string name;
    std::vector<std::string> slots; // a material name per slot
    std::vector<Triangle> triangles;
    std::vector<Hull> hulls;
};

inline size_t HullTriangleCount(const Mesh& m)
{
    size_t n = 0;
    for (const auto& h : m.hulls) n += h.triangles.size();
    return n;
}
inline std::array<double, 6> Bounds(const Mesh& m)
{
    std::array<double, 6> b{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
    auto add = [&](const Vec3& p) { for (int a = 0; a < 3; ++a) { b[a] = std::min(b[a], p[a]); b[a + 3] = std::max(b[a + 3], p[a]); } };
    for (const auto& t : m.triangles) for (const auto& p : t.p) add(p);
    for (const auto& h : m.hulls) for (const auto& p : h.corners) add(p);
    return b;
}

// ---- convex hulls ----------------------------------------------------------------------------
// Points that span no volume: they lie on one plane (normal) or a line.
struct FlatPoints : std::runtime_error
{
    Vec3 normal;
    bool line;
    FlatPoints(const Vec3& n, bool onLine) : std::runtime_error(onLine ? "the points lie on a line" : "the points are flat"), normal(n), line(onLine) {}
};
// A hull that did not come out clean at this tolerance.
struct UncleanHull : std::runtime_error { using std::runtime_error::runtime_error; };

inline double HullEpsilon(const std::vector<Vec3>& points, double tolerance)
{
    double largest = 1.0;
    for (const auto& p : points) for (double c : p) largest = std::max(largest, std::abs(c));
    return tolerance * largest;
}
// The points once each (within 1e-4).
inline std::vector<Vec3> Distinct(const std::vector<Vec3>& input)
{
    std::set<std::array<long long, 3>> seen;
    std::vector<Vec3> out;
    for (const auto& p : input)
    {
        const std::array<long long, 3> key{std::llround(p[0] * 1e4), std::llround(p[1] * 1e4), std::llround(p[2] * 1e4)};
        if (seen.insert(key).second) out.push_back(p);
    }
    return out;
}

// Whether a hull is clean: closed (every edge once each way), convex (every corner behind every
// triangle's plane), and 2V - 4 triangles for its V corners (one face per set of corners).
inline bool HullClean(const Hull& h, double tolerance = kHullTolerance, std::string* why = nullptr)
{
    auto fail = [&](const std::string& reason) { if (why) *why = reason; return false; };
    const size_t v = h.corners.size();
    if (v < 4) return fail("fewer than four corners");
    if (h.triangles.size() != 2 * v - 4)
        return fail(std::to_string(h.triangles.size()) + " triangles for " + std::to_string(v) + " corners (not 2V - 4)");
    std::set<std::pair<int, int>> edges;
    std::vector<int> used(v, 0);
    for (const auto& t : h.triangles)
        for (int k = 0; k < 3; ++k)
        {
            const int a = t[k], b = t[(k + 1) % 3];
            if (a < 0 || b < 0 || a >= static_cast<int>(v) || b >= static_cast<int>(v) || a == b) return fail("bad corner index");
            if (!edges.insert({a, b}).second) return fail("an edge is used twice the same way");
            used[static_cast<size_t>(a)] = 1;
        }
    for (const auto& e : edges)
        if (!edges.count({e.second, e.first})) return fail("not closed");
    if (std::find(used.begin(), used.end(), 0) != used.end()) return fail("a corner is on no triangle");
    const double eps = HullEpsilon(h.corners, tolerance);
    for (const auto& t : h.triangles)
    {
        const auto n = Cross(Sub(h.corners[static_cast<size_t>(t[1])], h.corners[static_cast<size_t>(t[0])]),
                             Sub(h.corners[static_cast<size_t>(t[2])], h.corners[static_cast<size_t>(t[0])]));
        const double length = Length(n);
        if (length < 1e-12) return fail("a triangle has no area");
        const auto nu = Mul(n, 1.0 / length);
        const double d = Dot(nu, h.corners[static_cast<size_t>(t[0])]);
        for (const auto& p : h.corners)
            if (Dot(nu, p) - d > eps) return fail("not convex");
    }
    return true;
}

// The convex hull of the points, clean at `tolerance` (FlatPoints when they span no volume,
// UncleanHull when it did not come out clean). It is made to a far finer tolerance, so it stays
// convex; then points on one face or on the edge between two (within `tolerance`: a face is the
// hull's triangles within it of one plane) are not corners, and the hull is made again without
// them. Every corner then meets three faces or more and its triangles number 2V - 4.
inline Hull ConvexHull(const std::vector<Vec3>& input, double tolerance = kHullTolerance)
{
    auto pts = Distinct(input);
    if (pts.size() < 4)
    {
        if (pts.size() == 3) throw FlatPoints(Unit(Cross(Sub(pts[1], pts[0]), Sub(pts[2], pts[0]))), false);
        throw FlatPoints({0, 0, 1}, true);
    }
    // Faces are flat to eps; the hull itself is made to a far finer one, so it stays convex.
    const double eps = HullEpsilon(pts, tolerance), fine = HullEpsilon(pts, 1e-10);
    auto key = [](int a, int b) { return static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32 | static_cast<uint32_t>(b); };
    struct F { int a, b, c; Vec3 n; double d; bool alive; std::vector<int> outside; };
    for (int pass = 0; pass < 8; ++pass)
    {
        const size_t count = pts.size();
        auto point = [&](int i) -> const Vec3& { return pts[static_cast<size_t>(i)]; };
        // The first tetrahedron: far apart, off the line, off the plane.
        size_t i0 = 0;
        for (size_t i = 1; i < count; ++i) if (pts[i][0] < pts[i0][0]) i0 = i;
        size_t i1 = i0;
        double best = -1;
        for (size_t i = 0; i < count; ++i) { const double d = Length(Sub(pts[i], pts[i0])); if (d > best) { best = d; i1 = i; } }
        if (best <= eps) throw FlatPoints({0, 0, 1}, true);
        const auto axis = Sub(pts[i1], pts[i0]);
        size_t i2 = i0;
        best = -1;
        for (size_t i = 0; i < count; ++i)
        {
            const double d = Length(Cross(Sub(pts[i], pts[i0]), axis)) / Length(axis);
            if (d > best) { best = d; i2 = i; }
        }
        if (best <= eps) throw FlatPoints({0, 0, 1}, true);
        const auto planeNormal = Unit(Cross(axis, Sub(pts[i2], pts[i0])));
        size_t i3 = i0;
        best = -1;
        for (size_t i = 0; i < count; ++i)
        {
            const double d = std::abs(Dot(planeNormal, Sub(pts[i], pts[i0])));
            if (d > best) { best = d; i3 = i; }
        }
        if (best <= eps) throw FlatPoints(planeNormal, false);
        const Vec3 inside = Mul(Add(Add(pts[i0], pts[i1]), Add(pts[i2], pts[i3])), 0.25);

        // Quickhull: each face keeps the points outside it; the farthest of them is added next,
        // and the faces it sees (a connected patch, found across shared edges) give way to a fan.
        std::vector<F> faces;
        std::unordered_map<uint64_t, int> byEdge; // directed edge -> the live face that has it
        auto distance = [&](const F& f, int i) { return Dot(f.n, point(i)) - f.d; };
        auto add = [&](int a, int b, int c, const Vec3& fallback) {
            auto n = Cross(Sub(point(b), point(a)), Sub(point(c), point(a)));
            n = Length(n) < 1e-300 ? fallback : Unit(n);
            const int id = static_cast<int>(faces.size());
            faces.push_back(F{a, b, c, n, Dot(n, point(a)), true, {}});
            for (auto e : {key(a, b), key(b, c), key(c, a)})
                if (!byEdge.emplace(e, id).second) throw UncleanHull("an edge is used twice the same way");
            return id;
        };
        const int s[4] = {static_cast<int>(i0), static_cast<int>(i1), static_cast<int>(i2), static_cast<int>(i3)};
        const int tetra[4][3] = {{s[0], s[1], s[2]}, {s[0], s[1], s[3]}, {s[0], s[2], s[3]}, {s[1], s[2], s[3]}};
        for (const auto& t : tetra)
        {
            int a = t[0], b = t[1], c = t[2];
            if (Dot(Cross(Sub(point(b), point(a)), Sub(point(c), point(a))), Sub(inside, point(a))) > 0) std::swap(b, c);
            add(a, b, c, {0, 0, 1});
        }
        for (size_t i = 0; i < count; ++i)
        {
            if (i == i0 || i == i1 || i == i2 || i == i3) continue;
            for (auto& f : faces)
                if (distance(f, static_cast<int>(i)) > fine) { f.outside.push_back(static_cast<int>(i)); break; }
        }
        std::vector<int> pending{0, 1, 2, 3};
        while (!pending.empty())
        {
            const int start = pending.back();
            if (!faces[static_cast<size_t>(start)].alive || faces[static_cast<size_t>(start)].outside.empty()) { pending.pop_back(); continue; }
            int eye = -1;
            double farthest = -1;
            for (int i : faces[static_cast<size_t>(start)].outside)
            {
                const double d = distance(faces[static_cast<size_t>(start)], i);
                if (d > farthest) { farthest = d; eye = i; }
            }
            std::vector<int> visible{start}, stack{start};
            std::unordered_set<int> seen{start};
            while (!stack.empty())
            {
                const int f = stack.back();
                stack.pop_back();
                const auto& t = faces[static_cast<size_t>(f)];
                for (auto [a, b] : {std::pair{t.a, t.b}, std::pair{t.b, t.c}, std::pair{t.c, t.a}})
                {
                    const auto other = byEdge.find(key(b, a));
                    if (other == byEdge.end()) throw UncleanHull("not closed");
                    const int g = other->second;
                    if (!seen.insert(g).second) continue;
                    if (distance(faces[static_cast<size_t>(g)], eye) > fine) { visible.push_back(g); stack.push_back(g); }
                }
            }
            std::unordered_set<int> visibleSet(visible.begin(), visible.end());
            std::vector<std::pair<int, int>> horizon;
            std::vector<Vec3> horizonNormal;
            // The horizon must pass each corner once. Where it touches itself (the eye nearly in
            // the plane of a face beside the patch), the faces there join the patch.
            for (int round = 0;; ++round)
            {
                horizon.clear();
                horizonNormal.clear();
                for (int f : visible)
                {
                    const auto& t = faces[static_cast<size_t>(f)];
                    for (auto [a, b] : {std::pair{t.a, t.b}, std::pair{t.b, t.c}, std::pair{t.c, t.a}})
                        if (!visibleSet.count(byEdge.at(key(b, a)))) { horizon.push_back({a, b}); horizonNormal.push_back(t.n); }
                }
                std::unordered_map<int, int> starts;
                for (const auto& h : horizon) ++starts[h.first];
                std::vector<int> join;
                for (const auto& h : horizon)
                    if (starts[h.first] > 1 || starts[h.second] > 1)
                    {
                        const int g = byEdge.at(key(h.second, h.first));
                        if (!visibleSet.count(g) && distance(faces[static_cast<size_t>(g)], eye) > -fine) join.push_back(g);
                    }
                if (join.empty())
                {
                    for (const auto& [corner, n] : starts) if (n > 1) throw UncleanHull("the horizon touches itself");
                    break;
                }
                if (round > 64) throw UncleanHull("the horizon touches itself");
                for (int g : join) if (visibleSet.insert(g).second) visible.push_back(g);
            }
            std::vector<int> orphans;
            for (int f : visible)
            {
                auto& t = faces[static_cast<size_t>(f)];
                t.alive = false;
                for (int i : t.outside) if (i != eye) orphans.push_back(i);
                t.outside.clear();
                byEdge.erase(key(t.a, t.b));
                byEdge.erase(key(t.b, t.c));
                byEdge.erase(key(t.c, t.a));
            }
            std::vector<int> made;
            for (size_t h = 0; h < horizon.size(); ++h) made.push_back(add(horizon[h].first, horizon[h].second, eye, horizonNormal[h]));
            for (int i : orphans)
                for (int f : made)
                    if (distance(faces[static_cast<size_t>(f)], i) > fine) { faces[static_cast<size_t>(f)].outside.push_back(i); break; }
            for (int f : made) if (!faces[static_cast<size_t>(f)].outside.empty()) pending.push_back(f);
        }
        {
            std::vector<F> live;
            for (auto& f : faces) if (f.alive) live.push_back(std::move(f));
            faces.swap(live);
        }
        byEdge.clear();
        for (size_t f = 0; f < faces.size(); ++f)
        {
            const auto& t = faces[f];
            for (auto e : {key(t.a, t.b), key(t.b, t.c), key(t.c, t.a)})
                if (!byEdge.emplace(e, static_cast<int>(f)).second) throw UncleanHull("an edge is used twice the same way");
        }
        for (const auto& [edge, f] : byEdge)
            if (!byEdge.count(key(static_cast<int>(edge & 0xffffffffu), static_cast<int>(edge >> 32)))) throw UncleanHull("not closed");

        // Faces: grown from the largest triangle over its neighbours while their corners lie
        // within tolerance of its plane, so every face is flat by construction.
        std::vector<int> region(faces.size(), -1);
        std::vector<std::pair<Vec3, double>> planes;
        std::vector<double> areas(faces.size());
        for (size_t f = 0; f < faces.size(); ++f)
            areas[f] = Length(Cross(Sub(point(faces[f].b), point(faces[f].a)), Sub(point(faces[f].c), point(faces[f].a))));
        std::vector<size_t> seeds(faces.size());
        std::iota(seeds.begin(), seeds.end(), size_t{0});
        std::sort(seeds.begin(), seeds.end(), [&](size_t a, size_t b) { return areas[a] > areas[b]; });
        for (size_t seed : seeds)
        {
            if (region[seed] >= 0) continue;
            const int id = static_cast<int>(planes.size());
            const Vec3 n = faces[seed].n;
            const double d = faces[seed].d;
            planes.push_back({n, d});
            std::vector<size_t> stack{seed};
            region[seed] = id;
            while (!stack.empty())
            {
                const size_t f = stack.back();
                stack.pop_back();
                const auto& t = faces[f];
                for (auto [a, b] : {std::pair{t.a, t.b}, std::pair{t.b, t.c}, std::pair{t.c, t.a}})
                {
                    const size_t g = static_cast<size_t>(byEdge.at(key(b, a)));
                    if (region[g] >= 0) continue;
                    const auto& u = faces[g];
                    if (Dot(u.n, n) <= 0) continue;
                    bool flat = true;
                    for (int c : {u.a, u.b, u.c}) flat = flat && std::abs(Dot(n, point(c)) - d) <= eps;
                    if (!flat) continue;
                    region[g] = id;
                    stack.push_back(g);
                }
            }
        }
        if (planes.size() < 4) throw UncleanHull("fewer than four faces");
        // A corner meets three faces or more; a point on one face or on the edge between two is
        // not a corner: leave it out and make the hull again.
        std::map<int, std::set<int>> meets;
        for (size_t f = 0; f < faces.size(); ++f)
            for (int c : {faces[f].a, faces[f].b, faces[f].c}) meets[c].insert(region[f]);
        std::vector<Vec3> corners;
        bool pruned = false;
        for (const auto& [c, regions] : meets)
        {
            if (regions.size() >= 3) corners.push_back(point(c));
            else pruned = true;
        }
        if (pruned)
        {
            if (corners.size() < 4) throw UncleanHull("too few corners");
            pts = corners;
            continue;
        }
        // The hull's own triangles: flat each, convex together, and with every corner on three
        // faces or more, 2V - 4 of them.
        Hull out;
        std::map<int, int> remap;
        for (const auto& t : faces)
        {
            std::array<int, 3> tri{};
            int k = 0;
            for (int c : {t.a, t.b, t.c})
            {
                auto found = remap.find(c);
                if (found == remap.end()) { found = remap.emplace(c, static_cast<int>(out.corners.size())).first; out.corners.push_back(point(c)); }
                tri[static_cast<size_t>(k++)] = found->second;
            }
            out.triangles.push_back(tri);
        }
        std::string why;
        if (!HullClean(out, tolerance, &why)) throw UncleanHull(why);
        return out;
    }
    throw UncleanHull("the corners did not settle");
}

// A box hull from low to high: 8 corners, 12 triangles.
inline Hull BoxHull(const Vec3& low, const Vec3& high)
{
    Hull h;
    for (int i = 0; i < 8; ++i) h.corners.push_back({(i & 1) ? high[0] : low[0], (i & 2) ? high[1] : low[1], (i & 4) ? high[2] : low[2]});
    // Each face's corners counter-clockwise seen from outside.
    const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& q : quads)
    {
        h.triangles.push_back({q[0], q[1], q[2]});
        h.triangles.push_back({q[0], q[2], q[3]});
    }
    return h;
}
inline Hull BoundsHull(const std::vector<Vec3>& points)
{
    Vec3 low{1e300, 1e300, 1e300}, high{-1e300, -1e300, -1e300};
    for (const auto& p : points) for (int a = 0; a < 3; ++a) { low[a] = std::min(low[a], p[a]); high[a] = std::max(high[a], p[a]); }
    for (int a = 0; a < 3; ++a) if (high[a] - low[a] < 1.0) { low[a] -= 0.5; high[a] += 0.5; }
    return BoxHull(low, high);
}

// A clean hull for the points, whatever it takes: at the hull tolerance; then from points snapped
// to coarser grids (0.01 to 4 units), which takes away the near-flat corners that make faces
// overlap; at last their bounding box (fallback set). A flat set (a floor, a sign) is given
// `thickness` behind it, away from `facing` (the way its faces look), so it still blocks.
inline Hull MakeHull(const std::vector<Vec3>& points, const Vec3& facing, double thickness, bool* fallback = nullptr, bool* flatSet = nullptr,
                     bool* simplifiedSet = nullptr)
{
    if (fallback) *fallback = false;
    if (flatSet) *flatSet = false;
    if (simplifiedSet) *simplifiedSet = false;
    auto attempt = [&](const std::vector<Vec3>& set) -> Hull {
        try { return ConvexHull(set); }
        catch (const UncleanHull&) {}
        for (double grid : {0.01, 0.1, 1.0, 4.0})
        {
            std::vector<Vec3> snapped;
            for (const auto& p : set) snapped.push_back({std::round(p[0] / grid) * grid, std::round(p[1] / grid) * grid, std::round(p[2] / grid) * grid});
            try { return ConvexHull(snapped); }
            catch (const UncleanHull&) {}
            catch (const FlatPoints&) {}
        }
        if (fallback) *fallback = true;
        return BoundsHull(set);
    };
    auto simplified = [&](Hull h) {
        if (h.corners.size() <= kHullCorners) return h;
        if (simplifiedSet) *simplifiedSet = true;
        // The corners farthest along evenly spread directions: a hull just inside the full one.
        std::vector<Vec3> kept;
        const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
        for (size_t i = 0; i < kHullCorners; ++i)
        {
            const double z = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / static_cast<double>(kHullCorners);
            const double r = std::sqrt(std::max(0.0, 1.0 - z * z)), a = golden * static_cast<double>(i);
            const Vec3 direction{r * std::cos(a), r * std::sin(a), z};
            size_t best = 0;
            for (size_t c = 1; c < h.corners.size(); ++c)
                if (Dot(h.corners[c], direction) > Dot(h.corners[best], direction)) best = c;
            kept.push_back(h.corners[best]);
        }
        return attempt(kept);
    };
    try { return simplified(attempt(points)); }
    catch (const FlatPoints& flat)
    {
        if (flat.line) throw;
        if (flatSet) *flatSet = true;
        Vec3 n = flat.normal;
        if (Dot(n, facing) > 0) n = Mul(n, -1.0);
        std::vector<Vec3> solid = points;
        for (const auto& p : points) solid.push_back(Add(p, Mul(n, thickness)));
        return simplified(attempt(solid));
    }
}

inline std::string Counted(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// ---- building the mesh -----------------------------------------------------------------------
struct Built
{
    Mesh mesh;
    std::vector<std::string> warnings;
    std::vector<int> slotMaterials;  // per slot: index into File::materials, or -1 (no usemtl)
    size_t degenerate = 0;           // triangles with no area, left out
    size_t collisionObjects = 0;     // UCX_/MCD objects in the file
    size_t hullFallbacks = 0;        // hulls that became boxes
    size_t flatParts = 0;            // flat parts given thickness
    size_t skippedHulls = 0;         // parts on a line: no hull
    size_t simplifiedHulls = 0;      // hulls cut down to kHullCorners corners
};

// Each set of points made a hull (with the way its faces look, for flat sets).
inline void AddHulls(Built& built, const std::vector<std::pair<std::vector<Vec3>, Vec3>>& sets)
{
    for (const auto& [points, facing] : sets)
    {
        if (points.empty()) continue;
        try
        {
            bool fallback = false, flat = false, simplified = false;
            built.mesh.hulls.push_back(MakeHull(Distinct(points), facing, 2.0, &fallback, &flat, &simplified));
            if (simplified) ++built.simplifiedHulls;
            if (fallback) ++built.hullFallbacks;
            if (flat) ++built.flatParts;
        }
        catch (const FlatPoints&) { ++built.skippedHulls; }
    }
}

inline Built Build(const File& file, const Options& options, const std::string& name)
{
    Built built;
    built.mesh.name = name;
    if (!(options.scale > 0) || !std::isfinite(options.scale)) throw std::runtime_error("The scale must be a number above 0.");
    std::map<int, int> slotOf; // file material (or -1) -> slot
    std::map<int, std::vector<Vec3>> fileHulls;
    std::map<int, Vec3> fileFacing;
    for (const auto& face : file.faces)
    {
        const bool collision = CollisionName(file.groups[static_cast<size_t>(face.group)]);
        std::vector<Vec3> polygon;
        for (const auto& c : face.corners) polygon.push_back(ToUnreal(file.positions[static_cast<size_t>(c.v)], options));
        // Mirrored into Unreal's axes: the corners run the other way.
        std::vector<Corner> corners(face.corners.rbegin(), face.corners.rend());
        std::reverse(polygon.begin(), polygon.end());
        if (collision)
        {
            auto& set = fileHulls[face.group];
            set.insert(set.end(), polygon.begin(), polygon.end());
            fileFacing[face.group] = Add(fileFacing[face.group], Newell(polygon));
            continue;
        }
        auto found = slotOf.find(face.material);
        if (found == slotOf.end())
        {
            found = slotOf.emplace(face.material, static_cast<int>(built.slotMaterials.size())).first;
            built.slotMaterials.push_back(face.material);
            built.mesh.slots.push_back(face.material >= 0 ? file.materials[static_cast<size_t>(face.material)] : std::string());
        }
        const bool hasUv = std::all_of(corners.begin(), corners.end(), [](const Corner& c) { return c.vt >= 0; });
        for (const auto& t : Triangulate(polygon))
        {
            Triangle tri;
            for (int k = 0; k < 3; ++k) tri.p[static_cast<size_t>(k)] = polygon[static_cast<size_t>(t[static_cast<size_t>(k)])];
            const auto n = Cross(Sub(tri.p[1], tri.p[0]), Sub(tri.p[2], tri.p[0]));
            if (Length(n) < 1e-9) { ++built.degenerate; continue; }
            for (int k = 0; k < 3; ++k)
            {
                auto& uv = tri.uv[static_cast<size_t>(k)];
                if (hasUv)
                {
                    const auto& from = file.uvs[static_cast<size_t>(corners[static_cast<size_t>(t[static_cast<size_t>(k)])].vt)];
                    uv = {from[0], 1.0 - from[1]};
                }
                else
                {
                    // No UVs in the file: projected along the face's main axis, a texture every 256 units.
                    const auto& p = tri.p[static_cast<size_t>(k)];
                    const int axis = std::abs(n[2]) >= std::abs(n[0]) && std::abs(n[2]) >= std::abs(n[1]) ? 2 : std::abs(n[0]) >= std::abs(n[1]) ? 0 : 1;
                    const int a = axis == 0 ? 1 : 0, b = axis == 2 ? 1 : 2;
                    uv = {p[static_cast<size_t>(a)] / 256.0, (axis == 2 ? p[static_cast<size_t>(b)] : -p[static_cast<size_t>(b)]) / 256.0};
                }
            }
            tri.slot = found->second;
            tri.smoothing = face.smoothing;
            tri.group = face.group;
            built.mesh.triangles.push_back(tri);
        }
    }
    built.collisionObjects = fileHulls.size();
    if (built.mesh.triangles.empty()) throw std::runtime_error("The file has no faces to draw.");
    if (built.degenerate) built.warnings.push_back(Counted(built.degenerate, "triangle", "triangles") + " with no area left out.");

    std::vector<std::pair<std::vector<Vec3>, Vec3>> sets;
    auto collision = options.collision;
    if (collision == Collision::File && fileHulls.empty())
    {
        built.warnings.push_back("The file has no UCX_ or MCD objects, so each connected part got a hull.");
        collision = Collision::Parts;
    }
    if (collision == Collision::Box)
    {
        std::vector<Vec3> all;
        for (const auto& t : built.mesh.triangles) all.insert(all.end(), t.p.begin(), t.p.end());
        built.mesh.hulls.push_back(BoundsHull(all));
    }
    else if (collision == Collision::Groups)
    {
        std::map<int, std::pair<std::vector<Vec3>, Vec3>> byGroup;
        for (const auto& t : built.mesh.triangles)
        {
            auto& entry = byGroup[t.group];
            entry.first.insert(entry.first.end(), t.p.begin(), t.p.end());
            entry.second = Add(entry.second, Cross(Sub(t.p[1], t.p[0]), Sub(t.p[2], t.p[0])));
        }
        for (auto& [group, entry] : byGroup) sets.push_back(entry);
    }
    else if (collision == Collision::Parts)
    {
        // Triangles that share a corner (welded to 0.001) are one part.
        std::vector<int> parent(built.mesh.triangles.size());
        std::iota(parent.begin(), parent.end(), 0);
        std::function<int(int)> root = [&](int x) { while (parent[static_cast<size_t>(x)] != x) x = parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])]; return x; };
        std::map<std::array<long long, 3>, int> owner;
        for (size_t i = 0; i < built.mesh.triangles.size(); ++i)
            for (const auto& p : built.mesh.triangles[i].p)
            {
                const std::array<long long, 3> k{std::llround(p[0] * 1e3), std::llround(p[1] * 1e3), std::llround(p[2] * 1e3)};
                auto found = owner.find(k);
                if (found == owner.end()) owner.emplace(k, static_cast<int>(i));
                else parent[static_cast<size_t>(root(static_cast<int>(i)))] = root(found->second);
            }
        std::map<int, std::pair<std::vector<Vec3>, Vec3>> byPart;
        for (size_t i = 0; i < built.mesh.triangles.size(); ++i)
        {
            const auto& t = built.mesh.triangles[i];
            auto& entry = byPart[root(static_cast<int>(i))];
            entry.first.insert(entry.first.end(), t.p.begin(), t.p.end());
            entry.second = Add(entry.second, Cross(Sub(t.p[1], t.p[0]), Sub(t.p[2], t.p[0])));
        }
        for (auto& [part, entry] : byPart) sets.push_back(entry);
    }
    else if (collision == Collision::File)
        for (auto& [group, points] : fileHulls) sets.push_back({points, fileFacing[group]});
    AddHulls(built, sets);
    if (built.flatParts) built.warnings.push_back(Counted(built.flatParts, "flat part", "flat parts") + " (a sign, a floor) given a hull 2 units thick behind it.");
    if (built.hullFallbacks) built.warnings.push_back(Counted(built.hullFallbacks, "hull", "hulls") + " that could not be made clean became a box.");
    if (built.simplifiedHulls)
        built.warnings.push_back(Counted(built.simplifiedHulls, "hull", "hulls") + " of more than " + std::to_string(kHullCorners) + " corners simplified to " + std::to_string(kHullCorners) + ".");
    if (built.skippedHulls) built.warnings.push_back(Counted(built.skippedHulls, "part", "parts") + " on a line, with no hull.");
    if ((collision == Collision::Groups || collision == Collision::Parts || collision == Collision::File) && built.mesh.hulls.empty())
        built.warnings.push_back("No hull could be made, so players will collide with the mesh's own triangles.");
    return built;
}

// ---- splitting -------------------------------------------------------------------------------
// The name of part `index` (0 based): the mesh's own name, then Name_2, Name_3...
inline std::string PartName(const std::string& name, size_t index)
{
    if (index == 0) return name;
    const auto suffix = "_" + std::to_string(index + 1);
    return name.substr(0, std::min(name.size(), kNameLength - suffix.size())) + suffix;
}
// How many parts the mesh needs at least to stay within `limit` triangles and hull triangles each.
inline size_t PartsNeeded(const Mesh& m, size_t limit = kCollisionTriangles)
{
    return std::max<size_t>({1, (m.triangles.size() + limit - 1) / limit, (HullTriangleCount(m) + limit - 1) / limit});
}
inline bool WithinLimits(const Mesh& m, size_t limit = kCollisionTriangles)
{
    return m.triangles.size() <= limit && HullTriangleCount(m) <= limit;
}

// The mesh cut into parts the game can collide with: each at most `limit` triangles and `limit`
// hull triangles, its triangles close together (cut along the longest side, again and again),
// each hull with the part nearest it. The parts share the pivot, so placed at the same place and
// rotation they make the whole mesh again. A mesh within the limits comes back as it is.
inline std::vector<Mesh> Split(const Mesh& mesh, size_t limit = kCollisionTriangles)
{
    if (limit < 1) throw std::runtime_error("A part needs at least one triangle.");
    for (const auto& h : mesh.hulls)
        if (h.triangles.size() > limit)
            throw std::runtime_error("One hull alone has " + std::to_string(h.triangles.size()) + " triangles, more than the " +
                                     std::to_string(limit) + " a mesh may have: use a box or per-polygon collision.");
    if (WithinLimits(mesh, limit)) return {mesh};
    const size_t total = mesh.triangles.size();
    auto centroid = [&](size_t i) {
        const auto& t = mesh.triangles[i];
        return Mul(Add(Add(t.p[0], t.p[1]), t.p[2]), 1.0 / 3.0);
    };
    std::vector<Vec3> centres(total);
    for (size_t i = 0; i < total; ++i) centres[i] = centroid(i);
    std::function<void(std::vector<size_t>, size_t, std::vector<std::vector<size_t>>&)> cut =
        [&](std::vector<size_t> items, size_t k, std::vector<std::vector<size_t>>& out) {
            if (k <= 1 || items.size() <= 1) { out.push_back(std::move(items)); return; }
            Vec3 low{1e300, 1e300, 1e300}, high{-1e300, -1e300, -1e300};
            for (size_t i : items) for (int a = 0; a < 3; ++a) { low[a] = std::min(low[a], centres[i][a]); high[a] = std::max(high[a], centres[i][a]); }
            int axis = 0;
            for (int a = 1; a < 3; ++a) if (high[a] - low[a] > high[axis] - low[axis]) axis = a;
            const size_t k1 = k / 2;
            const size_t at = std::clamp<size_t>(items.size() * k1 / k, 1, items.size() - 1);
            std::nth_element(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(at), items.end(),
                             [&](size_t a, size_t b) { return centres[a][axis] < centres[b][axis]; });
            cut(std::vector<size_t>(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(at)), k1, out);
            cut(std::vector<size_t>(items.begin() + static_cast<std::ptrdiff_t>(at), items.end()), k - k1, out);
        };
    std::vector<size_t> hullOrder(mesh.hulls.size());
    std::iota(hullOrder.begin(), hullOrder.end(), size_t{0});
    std::sort(hullOrder.begin(), hullOrder.end(), [&](size_t a, size_t b) { return mesh.hulls[a].triangles.size() > mesh.hulls[b].triangles.size(); });
    for (size_t k = PartsNeeded(mesh, limit); k <= total; ++k)
    {
        std::vector<size_t> all(total);
        std::iota(all.begin(), all.end(), size_t{0});
        std::vector<std::vector<size_t>> groups;
        cut(all, k, groups);
        std::vector<std::array<double, 6>> boxes;
        for (const auto& g : groups)
        {
            std::array<double, 6> b{1e300, 1e300, 1e300, -1e300, -1e300, -1e300};
            for (size_t i : g) for (const auto& p : mesh.triangles[i].p) for (int a = 0; a < 3; ++a) { b[a] = std::min(b[a], p[a]); b[a + 3] = std::max(b[a + 3], p[a]); }
            boxes.push_back(b);
        }
        std::vector<size_t> load(groups.size(), 0);
        std::vector<std::vector<size_t>> hullsOf(groups.size());
        bool placed = true;
        for (size_t h : hullOrder)
        {
            Vec3 c{0, 0, 0};
            for (const auto& p : mesh.hulls[h].corners) c = Add(c, p);
            c = Mul(c, 1.0 / static_cast<double>(std::max<size_t>(1, mesh.hulls[h].corners.size())));
            std::vector<std::pair<double, size_t>> nearest;
            for (size_t g = 0; g < groups.size(); ++g)
            {
                double d = 0;
                for (int a = 0; a < 3; ++a)
                {
                    const double gap = std::max({boxes[g][a] - c[a], 0.0, c[a] - boxes[g][a + 3]});
                    d += gap * gap;
                }
                nearest.push_back({d, g});
            }
            std::sort(nearest.begin(), nearest.end());
            bool done = false;
            for (const auto& [d, g] : nearest)
                if (load[g] + mesh.hulls[h].triangles.size() <= limit)
                {
                    load[g] += mesh.hulls[h].triangles.size();
                    hullsOf[g].push_back(h);
                    done = true;
                    break;
                }
            if (!done) { placed = false; break; }
        }
        if (!placed) continue;
        std::vector<Mesh> parts;
        for (size_t g = 0; g < groups.size(); ++g)
        {
            Mesh part;
            part.name = PartName(mesh.name, g);
            std::sort(groups[g].begin(), groups[g].end());
            std::map<int, int> slots;
            for (size_t i : groups[g]) slots.emplace(mesh.triangles[i].slot, 0);
            int next = 0;
            for (auto& [slot, index] : slots) { index = next++; part.slots.push_back(mesh.slots[static_cast<size_t>(slot)]); }
            for (size_t i : groups[g])
            {
                auto t = mesh.triangles[i];
                t.slot = slots[t.slot];
                part.triangles.push_back(t);
            }
            std::sort(hullsOf[g].begin(), hullsOf[g].end());
            for (size_t h : hullsOf[g]) part.hulls.push_back(mesh.hulls[h]);
            parts.push_back(std::move(part));
        }
        return parts;
    }
    throw std::runtime_error("The hulls cannot be spread over parts of " + std::to_string(limit) + " triangles.");
}

// ---- ASE -------------------------------------------------------------------------------------
inline std::string AseNumber(double v)
{
    if (std::abs(v) < 5e-7) v = 0;
    char text[48];
    std::snprintf(text, sizeof(text), "%.6f", v);
    return text;
}
struct AseFace { std::array<Vec3, 3> p; std::array<Vec2, 3> uv; int slot; unsigned smoothing; };
// One GEOMOBJECT: Unreal (x, y, z) written (x, -y, z), each face's corners a, c, b, V as 1 - v.
inline std::string GeomObject(const std::string& name, const std::vector<AseFace>& faces)
{
    std::vector<Vec3> vertices;
    std::map<std::array<long long, 3>, int> index;
    std::vector<std::array<int, 3>> ids;
    for (const auto& f : faces)
    {
        std::array<int, 3> id{};
        for (int k = 0; k < 3; ++k)
        {
            const auto& p = f.p[static_cast<size_t>(k)];
            const std::array<long long, 3> key{std::llround(p[0] * 1e6), std::llround(p[1] * 1e6), std::llround(p[2] * 1e6)};
            auto found = index.find(key);
            if (found == index.end()) { found = index.emplace(key, static_cast<int>(vertices.size())).first; vertices.push_back(p); }
            id[static_cast<size_t>(k)] = found->second;
        }
        ids.push_back(id);
    }
    std::string out = "*GEOMOBJECT {\r\n\t*NODE_NAME \"" + name + "\"\r\n\t*MESH {\r\n\t\t*TIMEVALUE 0\r\n";
    out += "\t\t*MESH_NUMVERTEX " + std::to_string(vertices.size()) + "\r\n\t\t*MESH_NUMFACES " + std::to_string(faces.size()) + "\r\n";
    out += "\t\t*MESH_VERTEX_LIST {\r\n";
    for (size_t i = 0; i < vertices.size(); ++i)
        out += "\t\t\t*MESH_VERTEX " + std::to_string(i) + "\t" + AseNumber(vertices[i][0]) + "\t" + AseNumber(-vertices[i][1]) + "\t" +
               AseNumber(vertices[i][2]) + "\r\n";
    out += "\t\t}\r\n\t\t*MESH_FACE_LIST {\r\n";
    for (size_t i = 0; i < faces.size(); ++i)
        out += "\t\t\t*MESH_FACE " + std::to_string(i) + ":\tA: " + std::to_string(ids[i][0]) + " B: " + std::to_string(ids[i][2]) + " C: " +
               std::to_string(ids[i][1]) + " AB: 1 BC: 1 CA: 1\t*MESH_SMOOTHING " + std::to_string(faces[i].smoothing) + "\t*MESH_MTLID " +
               std::to_string(faces[i].slot) + "\r\n";
    out += "\t\t}\r\n\t\t*MESH_NUMTVERTEX " + std::to_string(3 * faces.size()) + "\r\n\t\t*MESH_TVERTLIST {\r\n";
    for (size_t i = 0; i < faces.size(); ++i)
        for (int k = 0; k < 3; ++k)
            out += "\t\t\t*MESH_TVERT " + std::to_string(3 * i + static_cast<size_t>(k)) + "\t" + AseNumber(faces[i].uv[static_cast<size_t>(k)][0]) + "\t" +
                   AseNumber(1.0 - faces[i].uv[static_cast<size_t>(k)][1]) + "\t0.000000\r\n";
    out += "\t\t}\r\n\t\t*MESH_NUMTVFACES " + std::to_string(faces.size()) + "\r\n\t\t*MESH_TFACELIST {\r\n";
    for (size_t i = 0; i < faces.size(); ++i)
        out += "\t\t\t*MESH_TFACE " + std::to_string(i) + "\t" + std::to_string(3 * i) + "\t" + std::to_string(3 * i + 2) + "\t" + std::to_string(3 * i + 1) + "\r\n";
    out += "\t\t}\r\n\t}\r\n\t*MATERIAL_REF 0\r\n}\r\n";
    return out;
}
// The mesh as the ASE text the editor's importer reads: one Multi/Sub-Object material whose
// sub-materials name the slots' materials (the importer finds each by name among the loaded
// materials, so each must exist), the geometry, then an MCDCX object per hull.
inline std::string Ase(const Mesh& m)
{
    if (m.triangles.empty()) throw std::runtime_error(m.name + " has no triangles.");
    std::string out = "*3DSMAX_ASCIIEXPORT\t200\r\n*COMMENT \"RE+ Import OBJ: " + m.name + "\"\r\n*MATERIAL_LIST {\r\n"
                      "\t*MATERIAL_COUNT 1\r\n\t*MATERIAL 0 {\r\n\t\t*MATERIAL_NAME \"" + m.name + "\"\r\n"
                      "\t\t*MATERIAL_CLASS \"Multi/Sub-Object\"\r\n\t\t*NUMSUBMTLS " + std::to_string(m.slots.size()) + "\r\n";
    for (size_t i = 0; i < m.slots.size(); ++i)
    {
        const auto& s = m.slots[i];
        out += "\t\t*SUBMATERIAL " + std::to_string(i) + " {\r\n\t\t\t*MATERIAL_NAME \"" + s + "\"\r\n"
               "\t\t\t*MATERIAL_CLASS \"Standard\"\r\n\t\t\t*MATERIAL_DIFFUSE 1.0000\t1.0000\t1.0000\r\n"
               "\t\t\t*MAP_DIFFUSE {\r\n\t\t\t\t*MAP_NAME \"" + s + "\"\r\n\t\t\t\t*MAP_CLASS \"Bitmap\"\r\n"
               "\t\t\t\t*BITMAP \"" + s + ".tga\"\r\n\t\t\t\t*UVW_U_TILING 1.0000\r\n\t\t\t\t*UVW_V_TILING 1.0000\r\n"
               "\t\t\t}\r\n\t\t}\r\n";
    }
    out += "\t}\r\n}\r\n";
    std::vector<AseFace> faces;
    for (const auto& t : m.triangles) faces.push_back({t.p, t.uv, t.slot, t.smoothing});
    out += GeomObject(m.name, faces);
    for (size_t i = 0; i < m.hulls.size(); ++i)
    {
        std::vector<AseFace> hull;
        const auto& h = m.hulls[i];
        // Turned half a turn about Z (a rotation, so the winding stays outward).
        auto turned = [&](int c) { const auto& p = h.corners[static_cast<size_t>(c)]; return Vec3{-p[0], -p[1], p[2]}; };
        for (const auto& t : h.triangles) hull.push_back({{turned(t[0]), turned(t[1]), turned(t[2])}, {}, 0, 0});
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), "%02d", static_cast<int>(i));
        out += GeomObject("MCDCX_" + m.name + "_" + suffix, hull);
    }
    return out;
}
}
