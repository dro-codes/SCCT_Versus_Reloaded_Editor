#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

// Map Check: problems the stock editor's Check Map for Errors misses, worked
// out from the BSP as last built and the map's actors.
//
// Point queries copy the stock editor (ChaosTheory_Editor.exe):
// - UModel::PointRegion (0x11192BC0) walks Nodes (+0x54, 0x5C each) from node
//   0: front when Plane.PlaneDot(Location) >= 0, next node iChild[front]
//   (back +0x30, front +0x34), until INDEX_NONE. The zone is the last node's
//   iZone[front] (+0x58) when NumZones (+0x114) is non-zero, the leaf its
//   iLeaf[front] (+0x4C, 16-bit). It also tracks "outside" space, which is
//   empty space (outside the solid), from RootOutside (+0x104, 0 for a
//   subtractive level that starts solid): a CSG node (NumVertices +0x5A
//   non-zero, NodeFlags +0x5B without 0x21) makes its front side outside and
//   its back side solid, as MapRecovery's space probes rely on.
// - FEditorVisibility zone building (0x1101B6CF) numbers empty leaves
//   1 + id mod 63 and stores NumZones = found + 1, clamped to 1..64. Zone 0 is
//   solid space: BuildZoneInfo (0x1101D604) reports a ZoneInfo there as "out of
//   all zones" and two in one zone as "in the same zone as".
// - Zone portals are surfaces flagged PF_Portal (0x04000000, Surfs +0x94,
//   0x2C each, flags +0x14). A portal that does not seal its opening leaves the
//   same zone on both sides, which merges the rooms it was meant to divide.
// Everything here is plain data so it can be tested without the editor
// (tests/MapCheckModelTests.cpp). MapCheckNative.inl reads the scene, and
// PlacementNative.inl (MapCheckFloorGaps) traces below the static meshes.
namespace MapCheck
{
    using Vec3 = std::array<double, 3>;

    inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
    inline Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
    inline Vec3 Scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
    inline double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    inline Vec3 Cross(const Vec3& a, const Vec3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
    inline double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

    inline std::string Fold(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // ---------------------------------------------------------------- BSP --

    struct Node
    {
        Vec3 normal{};
        double distance = 0;          // Plane.W: PlaneDot = Dot(normal, p) - distance
        int back = -1, front = -1;    // iChild[0], iChild[1]
        bool csg = false;             // NumVertices > 0 and NodeFlags & 0x21 == 0
        uint8_t zone[2] = {0, 0};     // iZone[back], iZone[front]
        int leaf[2] = {-1, -1};
    };

    struct Bsp
    {
        std::vector<Node> nodes;
        bool rootOutside = false;   // false: the level starts solid (subtractive)
        int numZones = 0;
        bool Built() const { return !nodes.empty() && numZones > 0; }
    };

    struct Region
    {
        int zone = 0, leaf = -1;
        bool solid = false;   // in solid space, not "outside"
        bool valid = false;   // the walk ended at a leaf
    };

    inline Region PointRegion(const Bsp& bsp, const Vec3& p)
    {
        Region result;
        if (bsp.nodes.empty()) return result;
        const int count = static_cast<int>(bsp.nodes.size());
        bool outside = bsp.rootOutside;
        int index = 0, parent = -1, front = 0;
        for (int steps = 0; index != -1; ++steps)
        {
            if (index < 0 || index >= count || steps > count) return result;
            const auto& node = bsp.nodes[static_cast<size_t>(index)];
            front = Dot(node.normal, p) - node.distance >= 0 ? 1 : 0;
            outside = front ? (outside || node.csg) : (outside && !node.csg);
            parent = index;
            index = front ? node.front : node.back;
        }
        const auto& last = bsp.nodes[static_cast<size_t>(parent)];
        result.zone = bsp.numZones ? last.zone[front] : 0;
        result.leaf = last.leaf[front];
        result.solid = !outside;
        result.valid = true;
        return result;
    }

    // A node vertex's point index: FVert.pVertex is 16 bits, and RE+ patches
    // the engine to read it unsigned (BspDiagnostics), so 0x8000-0xFFFE are
    // points and 0xFFFF is none. A rebuild leaves the next two bytes undefined.
    // -1 when it names no point.
    inline int PointIndex(uint32_t raw, size_t count)
    {
        const uint32_t index = raw & 0xFFFF;
        return index == 0xFFFF || index >= count ? -1 : static_cast<int>(index);
    }

    // ------------------------------------------------------------- scene --

    struct Actor
    {
        std::string path, name;
        std::vector<std::string> classes;   // class names, most derived first
        Vec3 location{};
        double radius = 0, height = 0;
        bool collideActors = false, blockActors = false;
        // Filled by the editor's own encroachment test (ULevel::EncroachingWorldGeometry)
        // when the actor is one Map Check tests against geometry.
        bool encroachTested = false, encroachesBsp = false;
        std::string encroachingActor;       // a world-geometry actor (static mesh) it overlaps
    };

    // One zone portal surface and the polygons the BSP kept of it.
    struct Portal
    {
        int surface = -1;
        std::string brush;                       // the sheet brush it came from, if known
        std::vector<std::vector<Vec3>> fragments; // convex node polygons
        Vec3 normal{};
    };

    // An entry from the stock CheckForErrors pass (MapCheck_Add).
    struct StockEntry
    {
        int type = 0;   // 0 error, 1 warning, otherwise a note
        std::string actor, message;
    };

    // A static mesh's gap above the surface below it: how far Drop to Floor
    // would move it down (negative when sunk into it), and its box.
    struct FloorGap
    {
        std::string path, name;
        double gap = 0;
        Vec3 lo{}, hi{};
    };

    // The editor's drop traces for every visible static mesh, and the boxes of
    // the meshes those traces may not see (not colliding or not blocking).
    struct FloorScan
    {
        bool ran = false;
        bool stopped = false;   // a line check failed: only the first `checked` meshes were traced
        size_t checked = 0;
        std::vector<FloorGap> gaps;
        std::vector<std::pair<Vec3, Vec3>> soft;   // lo, hi
    };

    // A static mesh's collision as the editor keeps it (UStaticMesh, editor
    // layout; MapCheckNative.inl reads it):
    // - +0x104 per-polygon collision triangles (data, count +0x108), and their
    //   BSP at +0x114 (count +0x118);
    // - +0x128 the hull vertices (24 bytes: position, normal), +0x148 the hull
    //   triangles (MCDCX hulls; WORD v0, v1, v2, material), and their BSP at
    //   +0x150 (count +0x154).
    // A BSP node is six int16s (triangle, children...), so a tree stops at
    // 32767 nodes (StaticMeshCollisionFix caps the build there) and triangles
    // no node holds never collide. The importer stops a mesh's per-polygon
    // set at 15000 triangles.
    // The hull triangles are stored hull after hull, with corners shared
    // between hulls (one vertex per position), so HullsOf finds the hulls
    // again from the order and shape.
    struct HullTriangle { int a = 0, b = 0, c = 0; };

    struct MeshCollision
    {
        std::string path, name;
        std::vector<std::string> actors;   // the map's actors that use it
        bool colliding = false;            // one of them has bCollideActors
        int drawnTriangles = 0;
        int polyTriangles = 0, polyNodes = 0, polyInTree = -1;   // -1: not read
        std::vector<Vec3> hullVertices;
        std::vector<HullTriangle> hullTriangles;
        int hullNodes = 0, hullInTree = -1;
    };

    struct Scene
    {
        Bsp bsp;
        bool boundsKnown = false;
        Vec3 boundsMin{}, boundsMax{};
        std::vector<Actor> actors;
        std::vector<Portal> portals;
        std::vector<std::string> portalBrushesWithoutFaces; // portal sheets with no BSP surface at all
        std::vector<std::string> zoneActors;                // Zones[i].ZoneActor paths ("" = none)
        bool stockRan = false;
        std::vector<StockEntry> stock;
        FloorScan floors;
        std::vector<MeshCollision> meshes;                 // every static mesh the map's actors use
    };

    struct Settings
    {
        double geometryTolerance = 4;    // units a collision cylinder may sink into BSP
        double farOutside = 2048;        // units beyond the BSP bounds before "far outside"
        int leakGridCells = 2000000;     // budget for the leak path search grid
        bool zonesWithoutZoneInfo = true;
        bool stockChecks = true;
        double floatMinGap = 1;          // a static mesh this far above the surface below it floats...
        double floatMaxGap = 48;         // ...up to this far (0 turns the check off)
        // Static mesh collision limits (0 turns a check off). The game tags each
        // collision triangle a trace tests in one fixed table of 5512 slots
        // (SCCT Versus +0x147880, the table at 0x10C8BC10), for hulls and
        // per-polygon sets alike, with no bounds check.
        int collisionTriangleLimit = 5512;
        int collisionNodeLimit = 32767;  // int16 BSP child indices
        int polyTriangleCap = 15000;     // the importer's per-polygon cap
        bool malformedHulls = true;
    };

    inline Settings ClampSettings(Settings s)
    {
        s.collisionTriangleLimit = std::clamp(s.collisionTriangleLimit, 0, 1000000);
        s.collisionNodeLimit = std::clamp(s.collisionNodeLimit, 0, 1000000);
        s.polyTriangleCap = std::clamp(s.polyTriangleCap, 0, 1000000);
        s.geometryTolerance = std::clamp(s.geometryTolerance, 0.0, 64.0);
        s.farOutside = std::clamp(s.farOutside, 0.0, 262144.0);
        s.leakGridCells = std::clamp(s.leakGridCells, 10000, 20000000);
        s.floatMinGap = std::clamp(s.floatMinGap, 0.5, 64.0);
        s.floatMaxGap = std::clamp(s.floatMaxGap, 0.0, 512.0);
        if (s.floatMaxGap > 0 && s.floatMaxGap < s.floatMinGap) s.floatMaxGap = s.floatMinGap;
        return s;
    }

    // The engine's world limit (HALF_WORLD_MAX).
    constexpr double kWorldLimit = 262144;

    // ------------------------------------------------------------ problems --

    enum class Severity { Error, Warning, Note };
    inline const char* SeverityName(Severity s) { return s == Severity::Error ? "Error" : s == Severity::Warning ? "Warning" : "Note"; }

    struct Problem
    {
        Severity severity = Severity::Warning;
        std::string group, check, message;
        std::vector<std::string> actors;   // selected by the row
        std::vector<int> surfaces;         // selected by the row (BSP surface indices)
        std::vector<Vec3> path;            // a leak path, drawn in the viewports
        bool pathThroughPortal = false;    // the path had to pass through a portal sheet
    };

    struct Report
    {
        std::vector<Problem> problems;
        bool bspBuilt = false;
        size_t actorsChecked = 0, zones = 0;
        size_t Count(Severity s) const
        {
            return static_cast<size_t>(std::count_if(problems.begin(), problems.end(), [&](const Problem& p) { return p.severity == s; }));
        }
    };

    inline bool HasClass(const Actor& actor, const char* name)
    {
        const auto wanted = Fold(name);
        for (const auto& c : actor.classes) if (Fold(c) == wanted) return true;
        return false;
    }
    inline bool ClassContains(const Actor& actor, const char* part)
    {
        const auto wanted = Fold(part);
        for (const auto& c : actor.classes) if (Fold(c).find(wanted) != std::string::npos) return true;
        return false;
    }

    // How Map Check treats an actor's position.
    enum class Role
    {
        Skip,       // its location means nothing on its own (brushes, volumes, infos, static meshes)
        Zone,       // a ZoneInfo: checked by the zone checks
        Normal,     // in the void is worth a warning
        Important,  // players and gameplay rely on it: void and geometry are errors
    };

    inline Role RoleOf(const Actor& actor)
    {
        if (HasClass(actor, "LevelInfo")) return Role::Skip;
        if (HasClass(actor, "ZoneInfo")) return Role::Zone;
        // A brush's pivot (movers, volumes, sheets) and a static mesh's origin
        // may sit inside walls by design; Info actors have no place in the world.
        // Camera actors are the editor's own viewport cameras.
        if (HasClass(actor, "Brush") || HasClass(actor, "Mover") || HasClass(actor, "StaticMeshActor") || HasClass(actor, "Info") || HasClass(actor, "Camera")) return Role::Skip;
        for (const char* name : {"NavigationPoint", "Pickup", "Inventory", "Pawn", "Trigger", "SObjective", "SFlagDropZone"})
            if (HasClass(actor, name)) return Role::Important;
        if (ClassContains(actor, "Objective") || ClassContains(actor, "PlayerStart")) return Role::Important;
        return Role::Normal;
    }

    // The actors whose collision Map Check tests against BSP and static meshes:
    // the ones a player is spawned into or has to walk into. Triggers and
    // objectives are touch volumes, which a wall may cut into (ShipD's computer
    // terminals sit half inside theirs).
    inline bool TestsGeometry(const Actor& actor)
    {
        if (actor.radius <= 0 || actor.height <= 0 || RoleOf(actor) != Role::Important) return false;
        if (ClassContains(actor, "PlayerStart")) return true;
        for (const char* name : {"Pickup", "Inventory", "Pawn"})
            if (HasClass(actor, name)) return true;
        return false;
    }

    // Points of the collision cylinder, shrunk by the tolerance: the centre, and
    // eight around the rim at the bottom, middle and top.
    inline std::vector<Vec3> CylinderSamples(const Actor& actor, double tolerance)
    {
        std::vector<Vec3> points{actor.location};
        const double r = std::max(0.0, actor.radius - tolerance), h = std::max(0.0, actor.height - tolerance);
        for (double z : {-h, 0.0, h})
        {
            if (z != 0) points.push_back(Add(actor.location, {0, 0, z}));
            for (int i = 0; i < 8; ++i)
            {
                const double a = i * 3.14159265358979323846 / 4;
                points.push_back(Add(actor.location, {r * std::cos(a), r * std::sin(a), z}));
            }
        }
        return points;
    }

    inline double OutsideBounds(const Scene& scene, const Vec3& p)
    {
        if (!scene.boundsKnown) return 0;
        double d2 = 0;
        for (int i = 0; i < 3; ++i)
        {
            const double d = std::max({scene.boundsMin[static_cast<size_t>(i)] - p[static_cast<size_t>(i)], 0.0, p[static_cast<size_t>(i)] - scene.boundsMax[static_cast<size_t>(i)]});
            d2 += d * d;
        }
        return std::sqrt(d2);
    }

    inline std::string Units(double value)
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

    // ----------------------------------------------------------- portals --

    // Segment p->q against a convex polygon (any winding). True when it passes
    // through the polygon's interior.
    inline bool SegmentCrossesPolygon(const Vec3& p, const Vec3& q, const std::vector<Vec3>& poly)
    {
        if (poly.size() < 3) return false;
        Vec3 normal{};
        for (size_t i = 0; i < poly.size(); ++i)
            normal = Add(normal, Cross(poly[i], poly[(i + 1) % poly.size()]));
        const double length = Length(normal);
        if (length < 1e-9) return false;
        normal = Scale(normal, 1 / length);
        const double dp = Dot(normal, Sub(p, poly[0])), dq = Dot(normal, Sub(q, poly[0]));
        if ((dp > 0 && dq > 0) || (dp < 0 && dq < 0) || dp == dq) return false;
        const double t = dp / (dp - dq);
        const Vec3 hit = Add(p, Scale(Sub(q, p), t));
        int sign = 0;
        for (size_t i = 0; i < poly.size(); ++i)
        {
            const auto& a = poly[i];
            const auto& b = poly[(i + 1) % poly.size()];
            const double side = Dot(normal, Cross(Sub(b, a), Sub(hit, a)));
            const int s = side > 1e-9 ? 1 : side < -1e-9 ? -1 : 0;
            if (!s) continue;
            if (!sign) sign = s;
            else if (s != sign) return false;
        }
        return true;
    }

    inline Vec3 Centroid(const std::vector<Vec3>& poly)
    {
        Vec3 c{};
        for (const auto& p : poly) c = Add(c, p);
        return poly.empty() ? c : Scale(c, 1.0 / static_cast<double>(poly.size()));
    }

    // Zones either side of each fragment, probed a few units off its plane.
    struct PortalSides
    {
        std::set<std::pair<int, int>> pairs;   // (back zone, front zone), solid as -1
        bool Seals() const
        {
            for (const auto& [a, b] : pairs) if (a >= 0 && b >= 0 && a != b) return true;
            return false;
        }
        bool SameZoneBothSides(int& zone) const
        {
            for (const auto& [a, b] : pairs)
                if (a > 0 && a == b) { zone = a; return true; }
            return false;
        }
    };

    inline PortalSides SidesOf(const Bsp& bsp, const Portal& portal, double probe = 8)
    {
        PortalSides sides;
        for (const auto& fragment : portal.fragments)
        {
            if (fragment.size() < 3) continue;
            Vec3 normal{};
            for (size_t i = 0; i < fragment.size(); ++i) normal = Add(normal, Cross(fragment[i], fragment[(i + 1) % fragment.size()]));
            const double length = Length(normal);
            normal = length > 1e-9 ? Scale(normal, 1 / length) : portal.normal;
            const auto centre = Centroid(fragment);
            auto zoneAt = [&](const Vec3& p) { const auto r = PointRegion(bsp, p); return r.valid && !r.solid && r.zone > 0 ? r.zone : -1; };
            sides.pairs.insert({zoneAt(Sub(centre, Scale(normal, probe))), zoneAt(Add(centre, Scale(normal, probe)))});
        }
        return sides;
    }

    // --------------------------------------------------------- leak paths --

    struct LeakPath
    {
        std::vector<Vec3> points;
        bool throughPortal = false;
    };

    // A path between two points through empty space of one zone, found on a
    // grid over the given box (pointfile style). Edges never cross a portal
    // polygon on the first try, so the path shows the gap around the portal;
    // when no such path fits the grid it tries again allowing portal crossings
    // and says so. Empty when the points are not connected at this resolution.
    class LeakFinder
    {
    public:
        LeakFinder(const Bsp& bsp, const std::vector<Portal>& portals, int zone, Vec3 low, Vec3 high, int maxCells)
            : bsp_(bsp), zone_(zone)
        {
            for (const auto& portal : portals)
                for (const auto& fragment : portal.fragments)
                    if (fragment.size() >= 3)
                    {
                        Box box{fragment[0], fragment[0], &fragment};
                        for (const auto& p : fragment)
                            for (size_t i = 0; i < 3; ++i) { box.low[i] = std::min(box.low[i], p[i]); box.high[i] = std::max(box.high[i], p[i]); }
                        boxes_.push_back(box);
                    }
            double volume = 1;
            for (size_t i = 0; i < 3; ++i) volume *= std::max(1.0, high[i] - low[i]);
            cell_ = std::max(8.0, std::cbrt(volume / std::max(1000, maxCells)));
            for (size_t i = 0; i < 3; ++i)
            {
                low_[i] = low[i] - cell_;
                count_[i] = std::max(1, static_cast<int>(std::ceil((high[i] - low[i]) / cell_)) + 2);
            }
        }

        double Cell() const { return cell_; }

        LeakPath Find(const Vec3& from, const Vec3& to)
        {
            LeakPath result;
            for (int pass = 0; pass < 2 && result.points.empty(); ++pass)
            {
                blockPortals_ = pass == 0;
                result.points = Search(from, to);
                result.throughPortal = !blockPortals_ && !result.points.empty();
            }
            return result;
        }

    private:
        struct Box { Vec3 low, high; const std::vector<Vec3>* poly; };
        const Bsp& bsp_;
        int zone_;
        double cell_ = 64;
        Vec3 low_{};
        std::array<int, 3> count_{};
        std::vector<Box> boxes_;
        std::vector<uint8_t> state_;   // 0 unknown, 1 open, 2 closed
        bool blockPortals_ = true;

        size_t Total() const { return static_cast<size_t>(count_[0]) * count_[1] * count_[2]; }
        size_t Index(int x, int y, int z) const { return (static_cast<size_t>(z) * count_[1] + y) * count_[0] + x; }
        Vec3 Centre(size_t index) const
        {
            const size_t x = index % count_[0], y = index / count_[0] % count_[1], z = index / (static_cast<size_t>(count_[0]) * count_[1]);
            return {low_[0] + (x + 0.5) * cell_, low_[1] + (y + 0.5) * cell_, low_[2] + (z + 0.5) * cell_};
        }
        bool InZone(const Vec3& p) const
        {
            const auto r = PointRegion(bsp_, p);
            return r.valid && !r.solid && r.zone == zone_;
        }
        bool Open(size_t index)
        {
            if (!state_[index]) state_[index] = InZone(Centre(index)) ? 1 : 2;
            return state_[index] == 1;
        }
        bool CrossesPortal(const Vec3& p, const Vec3& q) const
        {
            if (!blockPortals_) return false;
            for (const auto& box : boxes_)
            {
                bool apart = false;
                for (size_t i = 0; i < 3 && !apart; ++i)
                    apart = std::max(p[i], q[i]) < box.low[i] - 0.01 || std::min(p[i], q[i]) > box.high[i] + 0.01;
                if (!apart && SegmentCrossesPolygon(p, q, *box.poly)) return true;
            }
            return false;
        }
        // Clear when sampled points along it stay in the zone and it crosses no portal.
        bool Clear(const Vec3& p, const Vec3& q) const
        {
            const double length = Length(Sub(q, p));
            const int steps = std::max(1, static_cast<int>(std::ceil(length / (cell_ / 4))));
            for (int i = 1; i < steps; ++i)
                if (!InZone(Add(p, Scale(Sub(q, p), static_cast<double>(i) / steps)))) return false;
            return !CrossesPortal(p, q);
        }
        bool CellOf(const Vec3& p, std::array<int, 3>& c) const
        {
            for (size_t i = 0; i < 3; ++i)
            {
                c[i] = static_cast<int>(std::floor((p[i] - low_[i]) / cell_));
                if (c[i] < 0 || c[i] >= count_[i]) return false;
            }
            return true;
        }
        // The open cells near a point that a clear segment reaches from it.
        std::vector<size_t> Anchors(const Vec3& p)
        {
            std::vector<size_t> found;
            std::array<int, 3> c{};
            if (!CellOf(p, c)) return found;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int x = c[0] + dx, y = c[1] + dy, z = c[2] + dz;
                        if (x < 0 || y < 0 || z < 0 || x >= count_[0] || y >= count_[1] || z >= count_[2]) continue;
                        const auto index = Index(x, y, z);
                        if (Open(index) && Clear(p, Centre(index))) found.push_back(index);
                    }
            return found;
        }
        std::vector<Vec3> Search(const Vec3& from, const Vec3& to)
        {
            state_.assign(Total(), 0);
            const auto starts = Anchors(from), goals = Anchors(to);
            if (starts.empty() || goals.empty()) return {};
            std::set<size_t> goalSet(goals.begin(), goals.end());
            std::vector<int64_t> parent(Total(), -2);
            std::deque<size_t> queue;
            for (auto s : starts) { parent[s] = -1; queue.push_back(s); }
            int64_t reached = -1;
            const int steps[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            while (!queue.empty() && reached < 0)
            {
                const auto index = queue.front();
                queue.pop_front();
                if (goalSet.count(index)) { reached = static_cast<int64_t>(index); break; }
                const int x = static_cast<int>(index % count_[0]), y = static_cast<int>(index / count_[0] % count_[1]),
                          z = static_cast<int>(index / (static_cast<size_t>(count_[0]) * count_[1]));
                for (const auto& s : steps)
                {
                    const int nx = x + s[0], ny = y + s[1], nz = z + s[2];
                    if (nx < 0 || ny < 0 || nz < 0 || nx >= count_[0] || ny >= count_[1] || nz >= count_[2]) continue;
                    const auto next = Index(nx, ny, nz);
                    if (parent[next] != -2 || !Open(next)) continue;
                    // The midpoint catches walls thinner than a cell.
                    const auto a = Centre(index), b = Centre(next);
                    if (!InZone(Scale(Add(a, b), 0.5)) || CrossesPortal(a, b)) continue;
                    parent[next] = static_cast<int64_t>(index);
                    queue.push_back(next);
                }
            }
            if (reached < 0) return {};
            std::vector<Vec3> cells;
            for (int64_t at = reached; at >= 0; at = parent[static_cast<size_t>(at)]) cells.push_back(Centre(static_cast<size_t>(at)));
            std::reverse(cells.begin(), cells.end());
            std::vector<Vec3> raw{from};
            raw.insert(raw.end(), cells.begin(), cells.end());
            raw.push_back(to);
            // Pull the string tight: skip ahead while the straight line stays clear.
            std::vector<Vec3> path{raw.front()};
            size_t i = 0;
            while (i + 1 < raw.size())
            {
                size_t j = raw.size() - 1;
                while (j > i + 1 && !Clear(raw[i], raw[j])) --j;
                path.push_back(raw[j]);
                i = j;
            }
            return path;
        }
    };

    inline double PathLength(const std::vector<Vec3>& path)
    {
        double total = 0;
        for (size_t i = 1; i < path.size(); ++i) total += Length(Sub(path[i], path[i - 1]));
        return total;
    }

    // ----------------------------------------------------- collision hulls --

    // One MCDCX hull found again in a mesh's hull triangles.
    struct Hull
    {
        size_t first = 0, count = 0;   // its first triangle and how many it has
        int corners = 0;               // distinct corner positions
        bool closed = false;           // every edge is shared by an even number of triangles
        Vec3 lo{}, hi{};               // mesh-space box
        // A closed convex hull with V corners has 2V - 4 triangles (Euler);
        // an open one fewer. More can only be faces that overlap.
        bool Malformed() const { return static_cast<long long>(count) > 2LL * corners - 4; }
    };

    // The hulls in a mesh's hull triangles. They are stored one hull after
    // another, and a hull is convex and (when whole) closed, so a hull ends at
    // the triangle where the run so far is closed, or where the next triangle
    // would make it concave (a corner of it in front of one of the run's
    // planes while others are behind, or the reverse). Both windings occur.
    // The tolerance (1 unit plus 1/1000 of the mesh's extent) keeps a hull
    // with slightly bent faces together, so its extra faces still show.
    inline std::vector<Hull> HullsOf(const std::vector<Vec3>& vertices, const std::vector<HullTriangle>& triangles)
    {
        std::vector<Hull> hulls;
        double scale = 0;
        for (const auto& v : vertices)
            for (double x : v) scale = std::max(scale, std::fabs(x));
        const double tolerance = 1 + 1e-3 * scale;
        struct Plane { Vec3 normal; double distance; int side; };   // side: +1 corners in front, -1 behind, 0 not known yet
        std::vector<Plane> planes;
        std::set<int> corners;
        std::map<uint64_t, int> edges;
        int odd = 0;
        size_t first = 0, count = 0;
        auto close = [&]()
        {
            if (count)
            {
                Hull hull;
                hull.first = first; hull.count = count; hull.closed = odd == 0;
                std::set<Vec3> positions;
                for (int index : corners) positions.insert(vertices[static_cast<size_t>(index)]);
                hull.corners = static_cast<int>(positions.size());
                hull.lo = hull.hi = *positions.begin();
                for (const auto& p : positions)
                    for (size_t k = 0; k < 3; ++k) { hull.lo[k] = std::min(hull.lo[k], p[k]); hull.hi[k] = std::max(hull.hi[k], p[k]); }
                hulls.push_back(hull);
            }
            planes.clear(); corners.clear(); edges.clear(); odd = 0; count = 0;
        };
        auto valid = [&](int index) { return index >= 0 && static_cast<size_t>(index) < vertices.size(); };
        for (size_t i = 0; i < triangles.size(); ++i)
        {
            const auto& t = triangles[i];
            if (!valid(t.a) || !valid(t.b) || !valid(t.c)) continue;
            const Vec3* points[3] = {&vertices[static_cast<size_t>(t.a)], &vertices[static_cast<size_t>(t.b)], &vertices[static_cast<size_t>(t.c)]};
            Vec3 normal = Cross(Sub(*points[1], *points[0]), Sub(*points[2], *points[0]));
            const double length = Length(normal);
            const bool flat = length > 1e-9;
            if (flat) normal = Scale(normal, 1 / length);
            const double distance = flat ? Dot(normal, *points[0]) : 0;
            if (count)
            {
                bool split = odd == 0;
                if (!split && flat)
                {
                    double lowest = 0, highest = 0;
                    for (int index : corners)
                    {
                        const double d = Dot(normal, vertices[static_cast<size_t>(index)]) - distance;
                        lowest = std::min(lowest, d); highest = std::max(highest, d);
                    }
                    split = highest > tolerance && lowest < -tolerance;
                }
                for (size_t k = 0; k < planes.size() && !split; ++k)
                {
                    int side = planes[k].side;
                    for (const auto* p : points)
                    {
                        const double d = Dot(planes[k].normal, *p) - planes[k].distance;
                        if (side == 0 && std::fabs(d) > tolerance) side = d > 0 ? 1 : -1;
                        else if (side != 0 && d * side < -tolerance) { split = true; break; }
                    }
                }
                if (split) close();
            }
            if (!count) first = i;
            ++count;
            for (int index : {t.a, t.b, t.c}) corners.insert(index);
            const int ends[3][2] = {{t.a, t.b}, {t.b, t.c}, {t.c, t.a}};
            for (const auto& e : ends)
            {
                const uint64_t key = (static_cast<uint64_t>(std::min(e[0], e[1])) << 32) | static_cast<uint32_t>(std::max(e[0], e[1]));
                odd += ++edges[key] % 2 ? 1 : -1;
            }
            // Which side of each plane the corners are on, now that this triangle's are in.
            for (auto& plane : planes)
                if (plane.side == 0)
                    for (const auto* p : points)
                    {
                        const double d = Dot(plane.normal, *p) - plane.distance;
                        if (std::fabs(d) > tolerance) { plane.side = d > 0 ? 1 : -1; break; }
                    }
            if (flat)
            {
                Plane plane{normal, distance, 0};
                for (int index : corners)
                {
                    const double d = Dot(normal, vertices[static_cast<size_t>(index)]) - distance;
                    if (std::fabs(d) > tolerance) { plane.side = d > 0 ? 1 : -1; break; }
                }
                planes.push_back(plane);
            }
        }
        close();
        return hulls;
    }

    // ------------------------------------------------------------ checks --

    // A check group: one entry per kind of problem, run in order. Add a new
    // group here (and to the README row) to extend Map Check.
    struct CheckGroup
    {
        const char* id;
        const char* title;
        std::function<void(const Scene&, const Settings&, Report&)> run;
    };

    namespace Detail
    {
        inline Problem Make(Severity severity, const char* group, const char* check, std::string message)
        {
            Problem p;
            p.severity = severity; p.group = group; p.check = check; p.message = std::move(message);
            return p;
        }

        constexpr const char* kActors = "Actors";
        constexpr const char* kZones = "Zones";
        constexpr const char* kProps = "Props";
        constexpr const char* kCollision = "Collision";
        constexpr const char* kStock = "Editor checks";

        inline void CheckActors(const Scene& scene, const Settings& settings, Report& report)
        {
            const auto& bsp = scene.bsp;
            for (const auto& actor : scene.actors)
            {
                const auto role = RoleOf(actor);
                if (role == Role::Skip || role == Role::Zone) continue;
                ++report.actorsChecked;
                const auto important = role == Role::Important;
                const double outside = OutsideBounds(scene, actor.location);
                const bool beyondWorld = std::fabs(actor.location[0]) > kWorldLimit || std::fabs(actor.location[1]) > kWorldLimit || std::fabs(actor.location[2]) > kWorldLimit;
                auto inSolid = [&](const Vec3& p) { const auto r = PointRegion(bsp, p); return r.valid && (r.zone == 0 || r.solid); };
                const double reach = settings.geometryTolerance + 1;
                const bool inVoid = bsp.Built() && inSolid(actor.location)
                    && inSolid(Add(actor.location, {0, 0, reach})) && inSolid(Add(actor.location, {0, 0, -reach}));
                if (beyondWorld)
                {
                    auto p = Make(Severity::Error, kActors, "Beyond the world limit",
                                  actor.name + " is beyond the engine's world limit of " + Units(kWorldLimit) + " units; the game cannot place it.");
                    p.actors = {actor.path};
                    report.problems.push_back(std::move(p));
                    continue;
                }
                if (outside > settings.farOutside && settings.farOutside > 0)
                {
                    auto p = Make(important ? Severity::Error : Severity::Warning, kActors, "Far outside the map",
                                  actor.name + " is " + Units(outside) + " units outside the map's geometry" + (inVoid ? ", in the void." : "."));
                    p.actors = {actor.path};
                    report.problems.push_back(std::move(p));
                    continue;
                }
                if (inVoid)
                {
                    // Wall-mounted props (buttons, health stations) often have their
                    // origin inside the wall, so only gameplay actors are more than a note.
                    auto p = Make(important ? Severity::Error : Severity::Note, kActors, "In the void",
                                  actor.name + " is inside solid space, outside every zone"
                                  + (important ? "; players can never reach it." : ". Fine for a prop mounted on a wall; otherwise it is lost."));
                    p.actors = {actor.path};
                    report.problems.push_back(std::move(p));
                    continue;
                }
                if (!TestsGeometry(actor)) continue;
                size_t solid = 0, total = 0;
                if (bsp.Built())
                    for (const auto& point : CylinderSamples(actor, settings.geometryTolerance))
                    {
                        ++total;
                        const auto r = PointRegion(bsp, point);
                        if (r.valid && (r.solid || r.zone == 0)) ++solid;
                    }
                if (solid || (actor.encroachTested && actor.encroachesBsp))
                {
                    auto p = Make(Severity::Error, kActors, "Stuck in BSP",
                                  actor.name + "'s collision cylinder (radius " + Units(actor.radius) + ", height " + Units(actor.height)
                                  + ") cuts into BSP geometry" + (solid ? " (" + std::to_string(solid) + " of " + std::to_string(total) + " sample points in solid)" : "") + ".");
                    p.actors = {actor.path};
                    report.problems.push_back(std::move(p));
                }
                else if (actor.encroachTested && !actor.encroachingActor.empty())
                {
                    auto p = Make(Severity::Error, kActors, "Stuck in a static mesh",
                                  actor.name + "'s collision cylinder overlaps blocking geometry of " + actor.encroachingActor + ".");
                    p.actors = {actor.path, actor.encroachingActor};
                    report.problems.push_back(std::move(p));
                }
            }
        }

        inline std::string ShortName(const std::string& path)
        {
            const auto dot = path.find_last_of('.');
            return dot == std::string::npos ? path : path.substr(dot + 1);
        }

        inline std::string ZoneLabel(const Scene& scene, int zone)
        {
            std::string label = "zone " + std::to_string(zone);
            if (zone >= 0 && static_cast<size_t>(zone) < scene.zoneActors.size() && !scene.zoneActors[static_cast<size_t>(zone)].empty())
                label += " (" + ShortName(scene.zoneActors[static_cast<size_t>(zone)]) + ")";
            return label;
        }

        // Grid box for a leak search: the BSP bounds, grown to hold both ends.
        inline void SearchBox(const Scene& scene, const Vec3& a, const Vec3& b, Vec3& low, Vec3& high)
        {
            low = scene.boundsKnown ? scene.boundsMin : a;
            high = scene.boundsKnown ? scene.boundsMax : a;
            for (const auto& p : {a, b})
                for (size_t i = 0; i < 3; ++i) { low[i] = std::min(low[i], p[i]); high[i] = std::max(high[i], p[i]); }
        }

        inline void AttachPath(const Scene& scene, const Settings& settings, int zone, const Vec3& from, const Vec3& to, Problem& p)
        {
            Vec3 low{}, high{};
            SearchBox(scene, from, to, low, high);
            LeakFinder finder(scene.bsp, scene.portals, zone, low, high, settings.leakGridCells);
            const auto path = finder.Find(from, to);
            if (path.points.empty())
            {
                p.message += " No leak path was found at a " + Units(finder.Cell()) + "-unit grid; the gap is narrower than that.";
                return;
            }
            p.path = path.points;
            p.pathThroughPortal = path.throughPortal;
            p.message += path.throughPortal
                ? " The path (" + Units(PathLength(path.points)) + " units) only fits through a portal sheet: the gap is beside that portal and narrower than " + Units(finder.Cell()) + " units."
                : " Leak path: " + Units(PathLength(path.points)) + " units, shown in the viewports.";
        }

        inline void CheckZones(const Scene& scene, const Settings& settings, Report& report)
        {
            const auto& bsp = scene.bsp;
            report.zones = bsp.numZones > 0 ? static_cast<size_t>(bsp.numZones - 1) : 0;
            if (bsp.numZones >= 64)
                report.problems.push_back(Make(Severity::Warning, kZones, "Zone limit",
                    "The BSP has 64 zones, the engine's limit. Zones past 63 share numbers with others (the editor numbers them 1 + n mod 63), so their settings and visibility mix."));

            // ZoneInfos: out of all zones, or sharing a zone.
            std::map<int, std::vector<const Actor*>> byZone;
            for (const auto& actor : scene.actors)
            {
                if (RoleOf(actor) != Role::Zone) continue;
                const auto region = PointRegion(bsp, actor.location);
                if (!region.valid || region.solid || region.zone == 0)
                {
                    auto p = Make(Severity::Error, kZones, "ZoneInfo outside all zones",
                                  actor.name + " is in solid space, so it sets up no zone. Move it into the room it belongs to.");
                    p.actors = {actor.path};
                    report.problems.push_back(std::move(p));
                    continue;
                }
                byZone[region.zone].push_back(&actor);
            }
            std::set<int> withInfo;
            for (const auto& [zone, infos] : byZone)
            {
                withInfo.insert(zone);
                if (infos.size() < 2) continue;
                std::string names;
                for (const auto* info : infos) names += (names.empty() ? "" : ", ") + info->name;
                auto p = Make(Severity::Error, kZones, "Zones leak into each other",
                              names + " are all in " + ZoneLabel(scene, zone) + ". The areas they were meant to set up are joined: a zone portal does not seal its opening, or an opening has no portal.");
                for (const auto* info : infos) p.actors.push_back(info->path);
                AttachPath(scene, settings, zone, infos[0]->location, infos[1]->location, p);
                report.problems.push_back(std::move(p));
            }

            // Zones open to the outside of the map (a hole to the outside of an
            // additive level, or a hollow map).
            if (scene.boundsKnown && bsp.Built())
            {
                std::set<int> open;
                Vec3 probe{};
                for (int corner = 0; corner < 8; ++corner)
                {
                    Vec3 p{};
                    for (size_t i = 0; i < 3; ++i) p[i] = (corner >> i) & 1 ? scene.boundsMax[i] + 256 : scene.boundsMin[i] - 256;
                    const auto r = PointRegion(bsp, p);
                    if (r.valid && !r.solid && r.zone > 0 && open.insert(r.zone).second && open.size() == 1) probe = p;
                }
                for (int zone : open)
                {
                    auto p = Make(Severity::Warning, kZones, "Zone open to the outside",
                                  "Space outside the map's geometry belongs to " + ZoneLabel(scene, zone) + ": the level is not sealed, so actors there are not inside the map.");
                    const Actor* inside = nullptr;
                    for (const auto& actor : scene.actors)
                    {
                        if (RoleOf(actor) == Role::Skip) continue;
                        const auto r = PointRegion(bsp, actor.location);
                        if (r.zone == zone && OutsideBounds(scene, actor.location) == 0) { inside = &actor; break; }
                    }
                    if (inside)
                    {
                        p.actors = {inside->path};
                        AttachPath(scene, settings, zone, inside->location, probe, p);
                    }
                    report.problems.push_back(std::move(p));
                }
            }

            // Portals, one row per sheet brush (its two faces are two surfaces).
            struct Sheet { std::string brush; std::vector<int> surfaces; std::vector<const Portal*> parts; };
            std::vector<Sheet> sheets;
            for (const auto& portal : scene.portals)
            {
                auto found = std::find_if(sheets.begin(), sheets.end(), [&](const Sheet& sheet) { return !portal.brush.empty() && sheet.brush == portal.brush; });
                if (found == sheets.end()) { sheets.push_back({portal.brush, {}, {}}); found = sheets.end() - 1; }
                found->surfaces.push_back(portal.surface);
                found->parts.push_back(&portal);
            }
            const std::string wrapped = bsp.numZones >= 64 ? " With 64 zones, different zones can share a number, so this may be a false alarm." : "";
            for (const auto& sheet : sheets)
            {
                const auto label = sheet.brush.empty() ? "Zone portal surface " + std::to_string(sheet.surfaces.front()) : "Zone portal " + ShortName(sheet.brush);
                std::vector<std::string> brush;
                if (!sheet.brush.empty()) brush.push_back(sheet.brush);
                size_t fragments = 0;
                PortalSides sides;
                for (const auto* part : sheet.parts)
                {
                    fragments += part->fragments.size();
                    const auto more = SidesOf(bsp, *part);
                    sides.pairs.insert(more.pairs.begin(), more.pairs.end());
                }
                auto add = [&](const char* check, const std::string& message)
                {
                    auto p = Make(Severity::Warning, kZones, check, message);
                    p.actors = brush; p.surfaces = sheet.surfaces;
                    report.problems.push_back(std::move(p));
                };
                int zone = 0;
                if (!fragments)
                    add("Unused zone portal", label + " has no polygons in the BSP: it is inside solid geometry, covered, or divided nothing when the BSP was built.");
                else if (sides.Seals())
                    continue;
                else if (sides.SameZoneBothSides(zone))
                    add("Portal does not seal", label + " has " + ZoneLabel(scene, zone) + " on both sides: it does not close its opening (or the space around it has another way through), so it divides nothing." + wrapped);
                else
                    add("Unconnected zone portal", label + " touches solid space on one side or both, so it connects no two zones.");
            }
            for (const auto& path : scene.portalBrushesWithoutFaces)
            {
                auto p = Make(Severity::Warning, kZones, "Unused zone portal",
                              "Zone portal " + ShortName(path) + " left no surfaces in the BSP: it is inside solid geometry, or the geometry has not been rebuilt since it was added.");
                p.actors = {path};
                report.problems.push_back(std::move(p));
            }

            if (settings.zonesWithoutZoneInfo && bsp.numZones > 1)
            {
                std::string missing;
                size_t count = 0;
                for (int zone = 1; zone < bsp.numZones; ++zone)
                    if (!withInfo.count(zone)) { missing += (missing.empty() ? "" : ", ") + std::to_string(zone); ++count; }
                if (count)
                    report.problems.push_back(Make(Severity::Note, kZones, "Zones without a ZoneInfo",
                        (count == 1 ? "Zone " : "Zones ") + missing + (count == 1 ? " has" : " have") + " no ZoneInfo and use the LevelInfo's zone settings."));
            }
        }

        // A gap in units, with one decimal below 10 when it is not whole (0.7).
        inline std::string GapUnits(double value)
        {
            const long long tenths = std::llround(value * 10);
            if (value <= 0 || value >= 10 || tenths % 10 == 0) return Units(value);
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
        }

        // A flat mesh (a decal, a puddle, a floor sign) under this high is
        // often lifted up to kFlatMeshLift on purpose, against z-fighting.
        constexpr double kFlatMeshHeight = 2, kFlatMeshLift = 2;

        // Static meshes a little above the surface below them: left behind when
        // the floor or the mesh under them moved. The trace may not see a mesh
        // that does not block, so one whose box tops out at the mesh's bottom
        // may be what it rests on: not floating. One inside such a box (a
        // shelf, a cart) may rest on a surface within it, and Drop to Floor
        // may drop it through, so it is listed apart without the End advice.
        inline void CheckProps(const Scene& scene, const Settings& settings, Report& report)
        {
            const auto& floors = scene.floors;
            if (!floors.ran) return;
            std::vector<const FloorGap*> floating, inside;
            for (const auto& gap : floors.gaps)
            {
                if (gap.gap < settings.floatMinGap || gap.gap > settings.floatMaxGap) continue;
                if (gap.hi[2] - gap.lo[2] < kFlatMeshHeight && gap.gap <= kFlatMeshLift) continue;
                const double bottom = gap.lo[2];
                bool resting = false, within = false;
                for (const auto& [lo, hi] : floors.soft)
                {
                    if (lo == gap.lo && hi == gap.hi) continue;   // its own box
                    if (!(lo[0] < gap.hi[0] && hi[0] > gap.lo[0] && lo[1] < gap.hi[1] && hi[1] > gap.lo[1])) continue;
                    resting = resting || (hi[2] >= bottom - 1 && hi[2] <= bottom + 1);
                    within = within || (lo[2] < bottom && hi[2] > bottom + 1);
                }
                if (!resting) (within ? inside : floating).push_back(&gap);
            }
            auto units = [](double value) { const auto n = GapUnits(value); return n + (n == "1" ? " unit" : " units"); };
            if (floating.size() >= 2)
            {
                auto p = Make(Severity::Warning, kProps, "Floating meshes",
                              std::to_string(floating.size()) + " static meshes float " + GapUnits(settings.floatMinGap) + "-" + GapUnits(settings.floatMaxGap)
                              + " units above the surface below them. This row selects them all: review the list (wall-mounted props near a ledge can show up), then press End over a viewport.");
                for (const auto* gap : floating) p.actors.push_back(gap->path);
                report.problems.push_back(std::move(p));
            }
            for (const auto* gap : floating)
            {
                auto p = Make(Severity::Warning, kProps, "Floating mesh",
                              gap->name + " floats " + units(gap->gap) + " above the surface below it; select it and press End over a viewport (Drop to Floor).");
                p.actors = {gap->path};
                report.problems.push_back(std::move(p));
            }
            for (const auto* gap : inside)
            {
                auto p = Make(Severity::Note, kProps, "Over a mesh that does not block",
                              gap->name + " is " + units(gap->gap) + " above the surface below it, inside the box of a static mesh that does not block (a shelf, or a mesh "
                              "with its collision off). If it rests on that mesh, leave it: End may drop it through.");
                p.actors = {gap->path};
                report.problems.push_back(std::move(p));
            }
            if (floors.stopped)
                report.problems.push_back(Make(Severity::Note, kProps, "Floating check stopped",
                    "The floating mesh check stopped after " + std::to_string(floors.checked) + (floors.checked == 1 ? " static mesh" : " static meshes")
                    + " because the editor's line check failed, so the meshes after those were not checked. Refresh to try again."));
        }

        inline std::string Point(const Vec3& p)
        {
            return "(" + Units(p[0]) + ", " + Units(p[1]) + ", " + Units(p[2]) + ")";
        }

        // Static mesh collision the game cannot use or that crashes it, one row
        // per mesh and limit; each row selects the actors that use the mesh.
        inline void CheckCollision(const Scene& scene, const Settings& settings, Report& report)
        {
            for (const auto& mesh : scene.meshes)
            {
                const int hullTriangles = static_cast<int>(mesh.hullTriangles.size());
                const auto copies = mesh.actors.size();
                const std::string used = "Used by " + std::to_string(copies) + (copies == 1 ? " actor." : " actors.");
                // A mesh none of whose actors collide is never traced against.
                auto add = [&](Severity severity, const char* check, const std::string& message)
                {
                    auto p = Make(mesh.colliding ? severity : Severity::Note, kCollision, check,
                                  message + " " + used + (mesh.colliding ? "" : " None of them collide now, so this matters only once one does."));
                    p.actors = mesh.actors;
                    report.problems.push_back(std::move(p));
                };

                const int limit = settings.collisionTriangleLimit;
                if (limit > 0 && (mesh.polyTriangles > limit || hullTriangles > limit))
                {
                    std::string sets;
                    if (mesh.polyTriangles > limit) sets = Units(mesh.polyTriangles) + " per-polygon collision triangles";
                    if (hullTriangles > limit) sets += (sets.empty() ? "" : " and ") + Units(hullTriangles) + " hull triangles";
                    add(Severity::Error, "Crashes the game",
                        mesh.name + " has " + sets + ", over the game's limit of " + Units(limit) + " per set: it crashes the game. The game marks each collision "
                        "triangle a trace tests in a fixed table of " + Units(limit) + " slots without checking the count, so a trace that reaches a later "
                        "triangle writes over the game's memory and the game crashes at random places. Split the mesh into parts or simplify its collision.");
                }

                const int cap = settings.polyTriangleCap;
                if (cap > 0 && mesh.polyTriangles >= cap)
                    add(Severity::Warning, "Per-polygon collision cut short",
                        mesh.name + "'s per-polygon collision stopped at the importer's cap of " + Units(cap) + " triangles"
                        + (mesh.drawnTriangles > mesh.polyTriangles ? " (it draws " + Units(mesh.drawnTriangles) + ")" : "")
                        + ", so faces past it may have no collision" + (hullTriangles ? ": shots may pass through them (players collide with its hulls)." : ": players and shots may pass through them.")
                        + " Split the mesh into parts.");

                const int nodes = settings.collisionNodeLimit;
                auto lost = [](int triangles, int inTree) { return inTree >= 0 && inTree < triangles ? triangles - inTree : 0; };
                if (nodes > 0 && mesh.polyNodes >= nodes)
                {
                    const int missing = lost(mesh.polyTriangles, mesh.polyInTree);
                    add(hullTriangles ? Severity::Warning : Severity::Error, "Collision BSP full",
                        mesh.name + "'s per-polygon collision BSP is full (" + Units(mesh.polyNodes) + " nodes, the most its 16-bit indices hold)"
                        + (missing ? ": " + Units(missing) + " of its " + Units(mesh.polyTriangles) + " collision triangles are in no node and never collide" : "; triangles it did not reach never collide")
                        + (hullTriangles ? ". Players collide with its hulls, so this lets shots through." : ", so players and shots pass through them.")
                        + " Split the mesh into parts.");
                }
                if (nodes > 0 && mesh.hullNodes >= nodes)
                {
                    const int missing = lost(hullTriangles, mesh.hullInTree);
                    add(Severity::Error, "Hull BSP full",
                        mesh.name + "'s hull collision BSP is full (" + Units(mesh.hullNodes) + " nodes, the most its 16-bit indices hold)"
                        + (missing ? ": " + Units(missing) + " of its " + Units(hullTriangles) + " hull triangles are in no node" : "")
                        + ", so the hulls past it do not collide and players fall through them. Use fewer, simpler hulls, or split the mesh.");
                }

                if (settings.malformedHulls && hullTriangles)
                {
                    std::vector<Hull> bad;
                    for (const auto& hull : HullsOf(mesh.hullVertices, mesh.hullTriangles))
                        if (hull.Malformed()) bad.push_back(hull);
                    if (!bad.empty())
                    {
                        const auto& h = bad.front();
                        add(Severity::Error, "Malformed collision hull",
                            mesh.name + (bad.size() == 1 ? " has a malformed collision hull: " : " has " + std::to_string(bad.size()) + " malformed collision hulls. The first: ")
                            + std::to_string(h.count) + " triangles over " + std::to_string(h.corners) + " corners, where a convex hull has at most "
                            + std::to_string(std::max(0, 2 * h.corners - 4)) + " (2 x corners - 4), so its faces overlap and the game ignores it: players fall or walk through it. "
                            "It spans " + Point(h.lo) + " to " + Point(h.hi) + " in the mesh's own space. Export it again with flat faces, one per set of corners.");
                    }
                }
            }
        }

        inline void CheckStock(const Scene& scene, const Settings&, Report& report)
        {
            for (const auto& entry : scene.stock)
            {
                auto p = Make(entry.type == 0 ? Severity::Error : entry.type == 1 ? Severity::Warning : Severity::Note, kStock, "Check Map for Errors", entry.message);
                if (!entry.actor.empty()) p.actors = {entry.actor};
                report.problems.push_back(std::move(p));
            }
        }
    }

    inline const std::vector<CheckGroup>& CheckGroups()
    {
        static const std::vector<CheckGroup> groups{
            {"actors", "Actors outside the world or stuck in geometry", Detail::CheckActors},
            {"zones", "BSP leaks and zones that are not sealed", Detail::CheckZones},
            {"props", "Static meshes floating above the surface below them", Detail::CheckProps},
            {"collision", "Static mesh collision past the game's limits", Detail::CheckCollision},
            {"stock", "The editor's own Check Map for Errors", Detail::CheckStock},
        };
        return groups;
    }

    inline Report Analyse(const Scene& scene, const Settings& settings)
    {
        Report report;
        report.bspBuilt = scene.bsp.Built();
        if (!report.bspBuilt)
            report.problems.push_back(Detail::Make(Severity::Warning, Detail::kZones, "BSP not built",
                "The map's BSP has not been built, so the void, geometry, zone and floating mesh checks were skipped. Build geometry and refresh."));
        for (const auto& group : CheckGroups())
        {
            const std::string id = group.id;
            if (!report.bspBuilt && (id == "zones" || id == "props")) continue;
            if (id == "props" && settings.floatMaxGap <= 0) continue;
            group.run(scene, settings, report);
        }
        std::stable_sort(report.problems.begin(), report.problems.end(), [](const Problem& a, const Problem& b) { return a.severity < b.severity; });
        return report;
    }
}
