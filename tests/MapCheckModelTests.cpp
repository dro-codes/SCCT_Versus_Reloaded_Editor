#include "../Reloaded.Editor/MapCheckModel.h"
#include <cassert>
#include <cstdio>
#include <string>

using namespace MapCheck;

namespace
{
    // A CSG wall plane: in front (the room side) is empty, behind is solid.
    Node Wall(Vec3 normal, double distance, int next, int zone)
    {
        Node n; n.normal = normal; n.distance = distance; n.csg = true;
        n.front = next; n.back = -1;
        if (next == -1) n.zone[1] = static_cast<uint8_t>(zone);
        return n;
    }

    // Appends the five remaining walls of an axis-aligned room [x0,x1] x [0,100] x [0,100]
    // as a chain of nodes ending in an empty leaf of `zone`. Returns the first node.
    int RoomChain(Bsp& bsp, double x0, double x1, int zone, bool lowX, bool highX)
    {
        const int first = static_cast<int>(bsp.nodes.size());
        std::vector<std::pair<Vec3, double>> planes;
        if (lowX) planes.push_back({{1, 0, 0}, x0});
        if (highX) planes.push_back({{-1, 0, 0}, -x1});
        planes.push_back({{0, 1, 0}, 0});
        planes.push_back({{0, -1, 0}, -100});
        planes.push_back({{0, 0, 1}, 0});
        planes.push_back({{0, 0, -1}, -100});
        for (size_t i = 0; i < planes.size(); ++i)
        {
            const bool last = i + 1 == planes.size();
            bsp.nodes.push_back(Wall(planes[i].first, planes[i].second, last ? -1 : first + static_cast<int>(i) + 1, zone));
        }
        return first;
    }

    // Two rooms side by side (x 0-100 and 100-200), open to each other at x = 100,
    // where a non-CSG portal plane splits the tree. When `sealed`, the rooms are
    // zones 1 and 2; otherwise the zone builder found them joined (zone 1).
    Bsp TwoRooms(bool sealed)
    {
        Bsp bsp;
        bsp.rootOutside = false;
        bsp.numZones = sealed ? 3 : 2;
        bsp.nodes.push_back({});                // root: the portal plane x >= 100
        bsp.nodes[0].normal = {1, 0, 0};
        bsp.nodes[0].distance = 100;
        const int b = RoomChain(bsp, 100, 200, sealed ? 2 : 1, false, true);
        const int a = RoomChain(bsp, 0, 100, 1, true, false);
        bsp.nodes[0].front = b;
        bsp.nodes[0].back = a;
        return bsp;
    }

    Scene Base(bool sealed, double portalTop)
    {
        Scene scene;
        scene.bsp = TwoRooms(sealed);
        scene.boundsKnown = true;
        scene.boundsMin = {0, 0, 0};
        scene.boundsMax = {200, 100, 100};
        scene.zoneActors = {"", "MyLevel.ZoneInfo0", sealed ? "MyLevel.ZoneInfo1" : ""};
        Portal portal;
        portal.surface = 7;
        portal.brush = "MyLevel.Sheet0";
        portal.normal = {1, 0, 0};
        portal.fragments.push_back({{100, 0, 0}, {100, portalTop, 0}, {100, portalTop, 100}, {100, 0, 100}});
        scene.portals.push_back(portal);
        return scene;
    }

    Actor Make(const char* name, std::vector<std::string> classes, Vec3 at, double radius = 0, double height = 0)
    {
        Actor a;
        a.name = name;
        a.path = std::string("MyLevel.") + name;
        a.classes = std::move(classes);
        a.location = at;
        a.radius = radius;
        a.height = height;
        return a;
    }

    const std::vector<std::string> kPlayerStart{"SPlayerStart", "PlayerStart", "NavigationPoint", "Actor", "Object"};
    const std::vector<std::string> kZoneInfo{"ZoneInfo", "Info", "Actor", "Object"};
    const std::vector<std::string> kLight{"Light", "Actor", "Object"};

    const Problem* FindCheck(const Report& report, const std::string& check, size_t nth = 0)
    {
        for (const auto& p : report.problems)
            if (p.check == check && nth-- == 0) return &p;
        return nullptr;
    }
    size_t CountCheck(const Report& report, const std::string& check)
    {
        size_t n = 0;
        for (const auto& p : report.problems) n += p.check == check;
        return n;
    }

    // A box hull appended to a mesh's hull set the way the editor stores one:
    // positions shared with earlier hulls reuse their vertex, and its twelve
    // triangles follow the earlier hulls'. `flip` reverses the winding; `order`
    // starts with the bottom then the top face, as stock meshes do.
    void AddBox(MeshCollision& mesh, Vec3 lo, Vec3 hi, bool flip = false, int dropFaces = 0)
    {
        auto vertex = [&](double x, double y, double z)
        {
            const Vec3 p{x, y, z};
            for (size_t i = 0; i < mesh.hullVertices.size(); ++i)
                if (mesh.hullVertices[i] == p) return static_cast<int>(i);
            mesh.hullVertices.push_back(p);
            return static_cast<int>(mesh.hullVertices.size() - 1);
        };
        int c[8];
        for (int i = 0; i < 8; ++i) c[i] = vertex(i & 1 ? hi[0] : lo[0], i & 2 ? hi[1] : lo[1], i & 4 ? hi[2] : lo[2]);
        // Quads (outward winding): bottom, top, -y, +y, -x, +x.
        const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
        for (int q = 0; q < 6 - dropFaces; ++q)
        {
            const auto& f = quads[q];
            HullTriangle a{c[f[0]], c[f[1]], c[f[2]]}, b{c[f[0]], c[f[2]], c[f[3]]};
            if (flip) { std::swap(a.b, a.c); std::swap(b.b, b.c); }
            mesh.hullTriangles.push_back(a);
            mesh.hullTriangles.push_back(b);
        }
    }

    MeshCollision Mesh(const char* name, size_t actors = 1)
    {
        MeshCollision mesh;
        mesh.name = name;
        mesh.path = std::string("MyLevel.Meshes.") + name;
        for (size_t i = 0; i < actors; ++i) mesh.actors.push_back("MyLevel.StaticMeshActor" + std::to_string(i));
        mesh.colliding = true;
        return mesh;
    }

    void CollisionTests()
    {
        // Hulls found again: separate boxes, boxes sharing a face (and so
        // corners), either winding, an open hull, and a box with two extra
        // triangles over one face.
        {
            auto mesh = Mesh("Kit");
            AddBox(mesh, {0, 0, 0}, {10, 10, 10});
            AddBox(mesh, {10, 0, 0}, {20, 10, 10}, true);         // shares the x = 10 face
            AddBox(mesh, {0, 0, 10}, {20, 10, 30});                // on top of both: shares four corners, convex with them
            AddBox(mesh, {100, 0, 0}, {110, 10, 5}, false, 1);     // open: no +x face
            const auto hulls = HullsOf(mesh.hullVertices, mesh.hullTriangles);
            assert(hulls.size() == 4);
            for (size_t i = 0; i < 3; ++i) assert(hulls[i].count == 12 && hulls[i].corners == 8 && hulls[i].closed && !hulls[i].Malformed());
            assert(hulls[1].first == 12 && hulls[2].first == 24);
            assert(hulls[2].lo == (Vec3{0, 0, 10}) && hulls[2].hi == (Vec3{20, 10, 30}));
            assert(hulls[3].count == 10 && !hulls[3].closed && !hulls[3].Malformed());

            // Vertex sharing alone would count the first two boxes as one shape of
            // 24 triangles over 12 corners, more than 2 x 12 - 4.
            Scene scene;
            scene.meshes.push_back(mesh);
            assert(!FindCheck(Analyse(scene, Settings{}), "Malformed collision hull"));

            // Two extra triangles over the first box's top: 14 triangles, 8 corners.
            auto bad = mesh;
            const auto top = bad.hullTriangles[2];
            bad.hullTriangles.insert(bad.hullTriangles.begin() + 4, {{top.a, top.b, top.c}, {top.a, top.c, top.b}});
            const auto again = HullsOf(bad.hullVertices, bad.hullTriangles);
            assert(again.size() == 4 && again[0].count == 14 && again[0].corners == 8 && again[0].Malformed());
            scene.meshes = {bad};
            const auto report = Analyse(scene, Settings{});
            const auto* row = FindCheck(report, "Malformed collision hull");
            assert(row && row->severity == Severity::Error && row->group == "Collision" && row->actors == bad.actors);
            assert(row->message.find("Kit has a malformed collision hull: 14 triangles over 8 corners, where a convex hull has at most 12") == 0);
            assert(row->message.find("players fall or walk through it") != std::string::npos && row->message.find("(0, 0, 0) to (10, 10, 10)") != std::string::npos);
            Settings off;
            off.malformedHulls = false;
            assert(!FindCheck(Analyse(scene, off), "Malformed collision hull"));

            // A face bent by less than the tolerance stays one clean hull.
            auto bent = Mesh("Bent");
            AddBox(bent, {0, 0, 0}, {1000, 1000, 1000});
            bent.hullVertices[7][2] += 1.5;   // one top corner up: the top is two planes now
            const auto one = HullsOf(bent.hullVertices, bent.hullTriangles);
            assert(one.size() == 1 && one[0].count == 12 && !one[0].Malformed());
            // Several malformed hulls in one mesh: one row that counts them.
            auto two = bad;
            const auto second = two.hullTriangles[16];   // the second box's top
            two.hullTriangles.insert(two.hullTriangles.begin() + 18, {second, {second.a, second.c, second.b}});
            scene.meshes = {two};
            const auto both = Analyse(scene, Settings{});
            assert(CountCheck(both, "Malformed collision hull") == 1);
            assert(FindCheck(both, "Malformed collision hull")->message.find("Kit has 2 malformed collision hulls. The first: 14 triangles") == 0);
        }

        // The 5512-triangle table: either set over it crashes the game.
        {
            Scene scene;
            auto own = Mesh("Cabin", 3);
            own.polyTriangles = 5513; own.drawnTriangles = 5513;
            auto exact = Mesh("Exact");
            exact.polyTriangles = 5512;
            auto hulls = Mesh("Hulled");
            for (int i = 0; i < 460; ++i) AddBox(hulls, {i * 20.0, 0, 0}, {i * 20.0 + 10, 10, 10});   // 5520 hull triangles
            hulls.polyTriangles = 200;
            auto quiet = Mesh("Quiet");
            quiet.polyTriangles = 9000;
            quiet.colliding = false;
            scene.meshes = {own, exact, hulls, quiet};
            const auto report = Analyse(scene, Settings{});
            assert(CountCheck(report, "Crashes the game") == 3);
            const auto* cabin = FindCheck(report, "Crashes the game");
            assert(cabin->severity == Severity::Error && cabin->actors.size() == 3);
            assert(cabin->message.find("Cabin has 5,513 per-polygon collision triangles, over the game's limit of 5,512 per set: it crashes the game.") == 0);
            assert(cabin->message.find("fixed table of 5,512 slots") != std::string::npos && cabin->message.find("Used by 3 actors.") != std::string::npos);
            assert(FindCheck(report, "Crashes the game", 1)->message.find("Hulled has 5,520 hull triangles, over") == 0);
            const auto* note = FindCheck(report, "Crashes the game", 2);
            assert(note->severity == Severity::Note && note->message.find("Quiet has 9,000") == 0 && note->message.find("None of them collide now") != std::string::npos);
            assert(!FindCheck(report, "Malformed collision hull") && !FindCheck(report, "Hull BSP full"));
            Settings off;
            off.collisionTriangleLimit = 0;
            assert(!FindCheck(Analyse(scene, off), "Crashes the game"));
            Settings lower;
            lower.collisionTriangleLimit = 5000;
            assert(CountCheck(Analyse(scene, lower), "Crashes the game") == 4);
        }

        // Full BSPs and the importer's 15000 cap.
        {
            Scene scene;
            auto jumbo = Mesh("Jumbo", 2);
            jumbo.polyTriangles = 15000; jumbo.drawnTriangles = 24000; jumbo.polyNodes = 32767; jumbo.polyInTree = 11697;
            AddBox(jumbo, {0, 0, 0}, {10, 10, 10});
            jumbo.hullNodes = 32767; jumbo.hullInTree = 9;
            auto fronts = Mesh("Fronts");
            fronts.polyTriangles = 5000; fronts.polyNodes = 32767; fronts.polyInTree = 3086;
            auto fits = Mesh("Fits");
            fits.polyTriangles = 14999; fits.polyNodes = 32766; fits.polyInTree = 14999;
            scene.meshes = {jumbo, fronts, fits};
            const auto report = Analyse(scene, Settings{});
            const auto* cut = FindCheck(report, "Per-polygon collision cut short");
            assert(CountCheck(report, "Per-polygon collision cut short") == 1 && cut->severity == Severity::Warning);
            assert(cut->message.find("Jumbo's per-polygon collision stopped at the importer's cap of 15,000 triangles (it draws 24,000)") == 0);
            assert(cut->message.find("players collide with its hulls") != std::string::npos);
            assert(CountCheck(report, "Collision BSP full") == 2);
            // Errors sort first: the mesh without hulls loses player collision.
            const auto* noHulls = FindCheck(report, "Collision BSP full");
            const auto* withHulls = FindCheck(report, "Collision BSP full", 1);
            assert(withHulls->severity == Severity::Warning && noHulls->severity == Severity::Error);
            assert(noHulls->message.find("Fronts's per-polygon collision BSP is full (32,767 nodes, the most its 16-bit indices hold): 1,914 of its 5,000 collision triangles are in no node") == 0);
            assert(noHulls->message.find("players and shots pass through") != std::string::npos);
            assert(withHulls->message.find("Jumbo's") == 0 && withHulls->message.find("lets shots through") != std::string::npos);
            const auto* hull = FindCheck(report, "Hull BSP full");
            assert(hull && hull->severity == Severity::Error && hull->message.find("Jumbo's hull collision BSP is full (32,767 nodes, the most its 16-bit indices hold): 3 of its 12 hull triangles are in no node") == 0);
            assert(CountCheck(report, "Crashes the game") == 2 && !FindCheck(report, "Malformed collision hull"));   // Jumbo and Fits
            Settings off;
            off.collisionNodeLimit = 0; off.polyTriangleCap = 0;
            const auto quiet = Analyse(scene, off);
            assert(!FindCheck(quiet, "Collision BSP full") && !FindCheck(quiet, "Hull BSP full") && !FindCheck(quiet, "Per-polygon collision cut short"));
        }

        // Settings bounds; and the check runs without a built BSP.
        {
            Settings s;
            s.collisionTriangleLimit = -4; s.collisionNodeLimit = 5000000; s.polyTriangleCap = 15000;
            s = ClampSettings(s);
            assert(s.collisionTriangleLimit == 0 && s.collisionNodeLimit == 1000000 && s.polyTriangleCap == 15000);
            Scene unbuilt;
            auto mesh = Mesh("Big");
            mesh.polyTriangles = 6000;
            unbuilt.meshes = {mesh};
            const auto report = Analyse(unbuilt, Settings{});
            assert(FindCheck(report, "BSP not built") && FindCheck(report, "Crashes the game"));
        }
    }
}

int main()
{
    // Point queries follow UModel::PointRegion: zone of the last node's side,
    // solid behind CSG walls.
    {
        const auto bsp = TwoRooms(true);
        auto r = PointRegion(bsp, {50, 50, 50});
        assert(r.valid && !r.solid && r.zone == 1);
        r = PointRegion(bsp, {150, 50, 50});
        assert(r.valid && !r.solid && r.zone == 2);
        r = PointRegion(bsp, {250, 50, 50});
        assert(r.valid && r.solid && r.zone == 0);
        r = PointRegion(bsp, {50, 50, -5});
        assert(r.solid && r.zone == 0);
        // On a plane counts as in front, like the engine's >= 0 test.
        r = PointRegion(bsp, {50, 50, 0});
        assert(!r.solid && r.zone == 1);
        Bsp unbuilt;
        assert(!PointRegion(unbuilt, {0, 0, 0}).valid && !unbuilt.Built());
        // A level that starts empty (RootOutside) with no nodes on a path stays empty.
        Bsp open;
        open.rootOutside = true;
        open.numZones = 2;
        open.nodes.push_back({});
        open.nodes[0].normal = {0, 0, 1};
        open.nodes[0].zone[0] = 1;
        open.nodes[0].zone[1] = 1;
        assert(!PointRegion(open, {0, 0, -50}).solid && PointRegion(open, {0, 0, -50}).zone == 1);
        // A broken child index never walks off the array.
        Bsp broken = bsp;
        broken.nodes[0].front = 999;
        assert(!PointRegion(broken, {150, 50, 50}).valid);
    }

    // Geometry helpers.
    {
        const std::vector<Vec3> square{{0, 0, 0}, {0, 10, 0}, {0, 10, 10}, {0, 0, 10}};
        assert(SegmentCrossesPolygon({-5, 5, 5}, {5, 5, 5}, square));
        assert(SegmentCrossesPolygon({5, 5, 5}, {-5, 5, 5}, square));     // either direction
        assert(!SegmentCrossesPolygon({-5, 15, 5}, {5, 15, 5}, square));  // beside it
        assert(!SegmentCrossesPolygon({1, 5, 5}, {5, 5, 5}, square));     // does not reach the plane
        assert(!SegmentCrossesPolygon({0, 5, 5}, {0, 6, 5}, square));     // along the plane
        const std::vector<Vec3> reversed{square.rbegin(), square.rend()};
        assert(SegmentCrossesPolygon({-5, 5, 5}, {5, 5, 5}, reversed));   // any winding
        Actor a = Make("A", kPlayerStart, {0, 0, 0}, 40, 40);
        const auto samples = CylinderSamples(a, 4);
        assert(samples.size() == 27);
        double lowest = 0, widest = 0;
        for (const auto& p : samples) { lowest = std::min(lowest, p[2]); widest = std::max(widest, p[0]); }
        assert(std::fabs(lowest + 36) < 1e-9 && std::fabs(widest - 36) < 1e-9);
        assert(Units(12345.6) == "12,346" && Units(-1000) == "-1,000" && Units(999) == "999");
        Settings s; s.geometryTolerance = -3; s.leakGridCells = 5;
        s = ClampSettings(s);
        assert(s.geometryTolerance == 0 && s.leakGridCells == 10000);
    }

    // Roles: what Map Check looks at.
    {
        assert(RoleOf(Make("P", kPlayerStart, {})) == Role::Important);
        assert(RoleOf(Make("Z", kZoneInfo, {})) == Role::Zone);
        assert(RoleOf(Make("L", {"LevelInfo", "ZoneInfo", "Info", "Actor"}, {})) == Role::Skip);
        assert(RoleOf(Make("B", {"Mover", "Brush", "Actor"}, {})) == Role::Skip);
        assert(RoleOf(Make("SM", {"SMover", "Mover", "Actor"}, {})) == Role::Skip);
        assert(RoleOf(Make("C", {"Camera", "PlayerController", "Controller", "Actor"}, {})) == Role::Skip);
        assert(RoleOf(Make("S", {"StaticMeshActor", "Actor"}, {})) == Role::Skip);
        assert(RoleOf(Make("M", {"SMission", "Info", "Actor"}, {})) == Role::Skip);
        assert(RoleOf(Make("O", {"SComputerObjectiveTrigger", "Actor"}, {})) == Role::Important);
        assert(RoleOf(Make("I", {"SPickup", "Pickup", "Actor"}, {})) == Role::Important);
        assert(RoleOf(Make("Li", kLight, {})) == Role::Normal);
        assert(!TestsGeometry(Make("P0", kPlayerStart, {}, 0, 0)));
        assert(TestsGeometry(Make("P1", kPlayerStart, {}, 40, 80)));
        assert(!TestsGeometry(Make("L1", kLight, {}, 40, 80)));
        // Touch volumes may cut into walls; only spawn points, pickups and pawns are tested.
        assert(!TestsGeometry(Make("T1", {"SComputerObjectiveTrigger", "Actor"}, {}, 22, 22)));
        assert(!TestsGeometry(Make("D1", {"SFlagDropZone", "Actor"}, {}, 5, 5)));
        assert(TestsGeometry(Make("I1", {"SPickup", "Pickup", "Actor"}, {}, 10, 10)));
    }

    // Actors in the void, far outside, beyond the world, stuck in BSP or a mesh.
    {
        auto scene = Base(true, 100);
        scene.actors.push_back(Make("GoodStart", kPlayerStart, {50, 50, 45}, 40, 40));
        scene.actors.push_back(Make("SunkStart", kPlayerStart, {50, 50, 30}, 40, 40));
        scene.actors.push_back(Make("VoidStart", kPlayerStart, {300, 50, 50}, 40, 40));
        scene.actors.push_back(Make("VoidLight", kLight, {50, 50, 150}));
        scene.actors.push_back(Make("FarLight", kLight, {50, 50, 5000}));
        scene.actors.push_back(Make("LostStart", kPlayerStart, {300000, 0, 0}, 40, 40));
        scene.actors.push_back(Make("Wall", {"StaticMeshActor", "Actor"}, {500, 50, 50}));
        // An origin a couple of units under the floor rests on it; it is not lost.
        scene.actors.push_back(Make("FloorFlag", {"SFlag", "SObjective", "Actor"}, {50, 50, -2}));
        scene.actors.push_back(Make("Terminal", {"SComputerObjectiveTrigger", "Actor"}, {50, 2, 50}, 22, 22));
        auto meshed = Make("MeshedStart", kPlayerStart, {150, 50, 50}, 20, 20);
        meshed.encroachTested = true;
        meshed.encroachingActor = "MyLevel.StaticMeshActor3";
        scene.actors.push_back(meshed);
        auto engineSays = Make("EngineStart", kPlayerStart, {160, 50, 50}, 20, 20);
        engineSays.encroachTested = true;
        engineSays.encroachesBsp = true;
        scene.actors.push_back(engineSays);
        scene.actors.push_back(Make("ZoneA", kZoneInfo, {50, 50, 50}));
        scene.actors.push_back(Make("ZoneB", kZoneInfo, {150, 50, 50}));

        Settings settings;
        const auto report = Analyse(scene, settings);
        assert(report.bspBuilt && report.zones == 2);
        assert(report.actorsChecked == 10);

        auto stuck = FindCheck(report, "Stuck in BSP");
        assert(stuck && stuck->actors == std::vector<std::string>{"MyLevel.SunkStart"} && stuck->severity == Severity::Error);
        auto engine = FindCheck(report, "Stuck in BSP", 1);
        assert(engine && engine->actors[0] == "MyLevel.EngineStart");
        assert(CountCheck(report, "Stuck in BSP") == 2);

        auto voidStart = FindCheck(report, "In the void");
        assert(voidStart && voidStart->actors[0] == "MyLevel.VoidStart" && voidStart->severity == Severity::Error);
        auto voidLight = FindCheck(report, "In the void", 1);
        assert(voidLight && voidLight->actors[0] == "MyLevel.VoidLight" && voidLight->severity == Severity::Note);
        assert(CountCheck(report, "In the void") == 2);

        auto far = FindCheck(report, "Far outside the map");
        assert(far && far->actors[0] == "MyLevel.FarLight" && far->message.find("4,900 units") != std::string::npos);
        assert(far->message.find("in the void") != std::string::npos);
        auto lost = FindCheck(report, "Beyond the world limit");
        assert(lost && lost->actors[0] == "MyLevel.LostStart" && lost->severity == Severity::Error);

        auto mesh = FindCheck(report, "Stuck in a static mesh");
        assert(mesh && mesh->actors == (std::vector<std::string>{"MyLevel.MeshedStart", "MyLevel.StaticMeshActor3"}));

        // The sealed rooms raise no zone problems, and errors sort first.
        assert(!FindCheck(report, "Zones leak into each other") && !FindCheck(report, "Portal does not seal"));
        assert(!FindCheck(report, "Zones without a ZoneInfo"));
        for (size_t i = 1; i < report.problems.size(); ++i) assert(report.problems[i - 1].severity <= report.problems[i].severity);
        assert(report.Count(Severity::Error) == 5 && report.Count(Severity::Warning) == 1 && report.Count(Severity::Note) == 1);

        // A larger tolerance forgives the sunk start; a "far" limit of 0 turns the far check off.
        settings.geometryTolerance = 20;
        settings.farOutside = 0;
        const auto relaxed = Analyse(scene, settings);
        assert(CountCheck(relaxed, "Stuck in BSP") == 1);
        assert(!FindCheck(relaxed, "Far outside the map") && CountCheck(relaxed, "In the void") == 3);
    }

    // A portal that leaves a gap: the rooms are one zone, both ZoneInfos are in
    // it, and the leak path goes round the portal through the gap.
    {
        auto scene = Base(false, 80);
        scene.actors.push_back(Make("ZoneA", kZoneInfo, {50, 50, 50}));
        scene.actors.push_back(Make("ZoneB", kZoneInfo, {150, 50, 50}));
        Settings settings;
        settings.leakGridCells = 200000;
        const auto report = Analyse(scene, settings);
        auto leak = FindCheck(report, "Zones leak into each other");
        assert(leak && leak->severity == Severity::Error);
        assert(leak->actors == (std::vector<std::string>{"MyLevel.ZoneA", "MyLevel.ZoneB"}));
        assert(leak->path.size() >= 3 && !leak->pathThroughPortal);
        assert(leak->path.front() == (Vec3{50, 50, 50}) && leak->path.back() == (Vec3{150, 50, 50}));
        bool throughGap = false;
        for (size_t i = 1; i < leak->path.size(); ++i)
        {
            const auto& a = leak->path[i - 1];
            const auto& b = leak->path[i];
            assert(!SegmentCrossesPolygon(a, b, scene.portals[0].fragments[0]));
            if ((a[0] - 100) * (b[0] - 100) <= 0)
            {
                const double t = a[0] == b[0] ? 0 : (100 - a[0]) / (b[0] - a[0]);
                throughGap = throughGap || a[1] + t * (b[1] - a[1]) > 80;
            }
        }
        assert(throughGap);
        assert(leak->message.find("Leak path") != std::string::npos);

        auto portal = FindCheck(report, "Portal does not seal");
        assert(portal && portal->surfaces == std::vector<int>{7} && portal->actors[0] == "MyLevel.Sheet0");
        assert(portal->severity == Severity::Warning);
        assert(portal->message.find("false alarm") == std::string::npos);
        assert(portal->message.find("zone 1 (ZoneInfo0)") != std::string::npos);

        // When no gap fits the grid (here the sheet fills the opening), the path goes through the portal sheet.
        auto tight = Base(false, 100);
        tight.actors = scene.actors;
        const auto narrow = Analyse(tight, settings);
        auto through = FindCheck(narrow, "Zones leak into each other");
        assert(through && through->pathThroughPortal && through->path.size() >= 2);
        assert(through->message.find("portal sheet") != std::string::npos);
    }

    // Unused and unconnected portals, ZoneInfos in solid, zones without ZoneInfos,
    // the zone limit and the editor's own entries.
    {
        auto scene = Base(true, 100);
        Portal empty;
        empty.surface = 9;
        scene.portals.push_back(empty);
        Portal buried;
        buried.surface = 10;
        buried.brush = "MyLevel.Sheet2";
        buried.fragments.push_back({{300, 0, 0}, {300, 100, 0}, {300, 100, 100}, {300, 0, 100}});
        scene.portals.push_back(buried);
        scene.portalBrushesWithoutFaces.push_back("MyLevel.Sheet3");
        scene.actors.push_back(Make("ZoneA", kZoneInfo, {50, 50, 50}));
        scene.actors.push_back(Make("LostZone", kZoneInfo, {50, 50, -50}));
        scene.stock.push_back({0, "MyLevel.Light4", "Light is in solid"});
        scene.stock.push_back({1, "", "Map has no PlayerStart"});
        scene.stock.push_back({2, "MyLevel.Light5", "A note"});
        const auto report = Analyse(scene, Settings{});
        auto unused = FindCheck(report, "Unused zone portal");
        assert(unused && unused->surfaces == std::vector<int>{9} && unused->actors.empty());
        auto unusedBrush = FindCheck(report, "Unused zone portal", 1);
        assert(unusedBrush && unusedBrush->actors == std::vector<std::string>{"MyLevel.Sheet3"});
        auto unconnected = FindCheck(report, "Unconnected zone portal");
        assert(unconnected && unconnected->surfaces == std::vector<int>{10});
        assert(!FindCheck(report, "Portal does not seal"));
        auto lost = FindCheck(report, "ZoneInfo outside all zones");
        assert(lost && lost->actors[0] == "MyLevel.LostZone");
        auto missing = FindCheck(report, "Zones without a ZoneInfo");
        assert(missing && missing->severity == Severity::Note && missing->message.find("Zone 2 has") == 0);
        assert(CountCheck(report, "Check Map for Errors") == 3);
        auto stock = FindCheck(report, "Check Map for Errors");
        assert(stock->severity == Severity::Error && stock->actors[0] == "MyLevel.Light4" && stock->group == "Editor checks");
        assert(FindCheck(report, "Check Map for Errors", 1)->actors.empty());

        Settings quiet;
        quiet.zonesWithoutZoneInfo = false;
        assert(!FindCheck(Analyse(scene, quiet), "Zones without a ZoneInfo"));

        scene.bsp.numZones = 64;
        assert(FindCheck(Analyse(scene, Settings{}), "Zone limit"));

        // Both faces of one sheet are one row; a face the BSP dropped does not make it unused.
        auto sheet = Base(false, 80);
        Portal back = sheet.portals[0];
        back.surface = 8;
        back.fragments.clear();
        sheet.portals.push_back(back);
        sheet.bsp.numZones = 64;
        const auto merged = Analyse(sheet, Settings{});
        auto row = FindCheck(merged, "Portal does not seal");
        assert(row && row->surfaces == (std::vector<int>{7, 8}) && CountCheck(merged, "Portal does not seal") == 1);
        assert(!FindCheck(merged, "Unused zone portal"));
        assert(row->message.find("false alarm") != std::string::npos);
    }

    // A level open to the outside: space past the bounds is a zone, and the path
    // runs from an actor in that zone out past the geometry.
    {
        Scene scene;
        scene.bsp.rootOutside = false;
        scene.bsp.numZones = 2;
        scene.bsp.nodes.push_back(Wall({0, 0, 1}, 0, -1, 1));   // a floor and nothing else
        scene.boundsKnown = true;
        scene.boundsMin = {0, 0, 0};
        scene.boundsMax = {100, 100, 100};
        scene.actors.push_back(Make("Pickup0", {"SPickup", "Pickup", "Actor"}, {50, 50, 50}));
        Settings settings;
        settings.leakGridCells = 100000;
        const auto report = Analyse(scene, settings);
        auto open = FindCheck(report, "Zone open to the outside");
        assert(open && open->actors[0] == "MyLevel.Pickup0");
        assert(open->path.size() == 2 && open->path.front() == (Vec3{50, 50, 50}));
        assert(open->path.back()[2] > 100);
    }

    // No BSP: one warning, and the zone checks are skipped.
    {
        Scene scene;
        scene.actors.push_back(Make("Start", kPlayerStart, {0, 0, 0}, 40, 40));
        scene.actors.push_back(Make("ZoneA", kZoneInfo, {0, 0, 0}));
        const auto report = Analyse(scene, Settings{});
        assert(!report.bspBuilt);
        assert(FindCheck(report, "BSP not built"));
        assert(report.problems.size() == 1);
    }

    // Static meshes floating above the surface below them, from the editor's drop traces.
    {
        auto scene = Base(true, 100);
        scene.floors.ran = true;
        auto gap = [&](const char* name, double distance, Vec3 lo, Vec3 hi)
        {
            scene.floors.gaps.push_back({std::string("MyLevel.") + name, name, distance, lo, hi});
        };
        gap("Crate20", 20, {10, 10, 20}, {30, 30, 40});
        gap("Barrel2", 2, {40, 10, 2}, {50, 20, 30});
        gap("Resting", 0.3, {60, 10, 0.3}, {70, 20, 10});
        gap("Nudged", 0.8, {60, 30, 0.8}, {70, 40, 10});
        gap("Lamp", 60, {60, 60, 60}, {70, 70, 80});
        gap("Sunk", -5, {80, 10, -5}, {90, 20, 10});
        const auto report = Analyse(scene, Settings{});
        assert(CountCheck(report, "Floating mesh") == 2);
        auto crate = FindCheck(report, "Floating mesh");
        assert(crate && crate->actors == std::vector<std::string>{"MyLevel.Crate20"});
        assert(crate->severity == Severity::Warning && crate->group == "Props");
        assert(crate->message.find("Crate20 floats 20 units above the surface below it") == 0);
        assert(FindCheck(report, "Floating mesh", 1)->actors[0] == "MyLevel.Barrel2");
        // Two or more get a row first that selects them all.
        auto all = FindCheck(report, "Floating meshes");
        assert(all && all->severity == Severity::Warning && CountCheck(report, "Floating meshes") == 1);
        assert(all->actors == (std::vector<std::string>{"MyLevel.Crate20", "MyLevel.Barrel2"}));
        assert(all->message.find("2 static meshes float 1-48 units") == 0);
        assert(all < crate);

        auto one = scene;
        one.floors.gaps.resize(1);
        const auto single = Analyse(one, Settings{});
        assert(CountCheck(single, "Floating mesh") == 1 && !FindCheck(single, "Floating meshes"));

        // A mesh that does not block (the trace went through it) whose box tops
        // out at Crate20's bottom may be what it rests on: no row. Soft boxes
        // beside it, above it or well below its bottom leave the row.
        for (const double top : {19.5, 20.0, 20.8})
        {
            auto rug = scene;
            rug.floors.soft.push_back({{0, 0, 0}, {40, 40, top}});
            const auto rugged = Analyse(rug, Settings{});
            assert(CountCheck(rugged, "Floating mesh") == 1 && FindCheck(rugged, "Floating mesh")->actors[0] == "MyLevel.Barrel2");
            assert(!FindCheck(rugged, "Floating meshes") && !FindCheck(rugged, "Over a mesh that does not block"));
        }
        auto beside = scene;
        beside.floors.soft.push_back({{100, 100, 0}, {120, 120, 19.5}});
        beside.floors.soft.push_back({{0, 0, 0}, {40, 40, 1}});       // a rug 19 units below it
        beside.floors.soft.push_back({{0, 0, 30}, {40, 40, 60}});     // a lamp above it
        beside.floors.soft.push_back({{10, 10, 20}, {30, 30, 20}});   // its own box, a flat mesh
        beside.floors.gaps[0].hi[2] = 20;
        const auto besides = Analyse(beside, Settings{});
        assert(CountCheck(besides, "Floating mesh") == 2 && !FindCheck(besides, "Over a mesh that does not block"));

        // Inside the box of one (a shelf it may stand on): listed apart, without End.
        auto shelf = scene;
        shelf.floors.soft.push_back({{0, 0, 0}, {40, 40, 50}});
        const auto shelved = Analyse(shelf, Settings{});
        assert(CountCheck(shelved, "Floating mesh") == 1 && FindCheck(shelved, "Floating mesh")->actors[0] == "MyLevel.Barrel2");
        assert(!FindCheck(shelved, "Floating meshes") && CountCheck(shelved, "Over a mesh that does not block") == 1);
        auto onShelf = FindCheck(shelved, "Over a mesh that does not block");
        assert(onShelf->severity == Severity::Note && onShelf->actors == std::vector<std::string>{"MyLevel.Crate20"});
        assert(onShelf->message.find("Crate20 is 20 units above") == 0 && onShelf->message.find("press End") == std::string::npos);
        assert(onShelf->message.find("End may drop it through") != std::string::npos);

        // A line check that failed stops the scan: the rows found so far, and a note.
        auto stopped = scene;
        stopped.floors.stopped = true;
        stopped.floors.checked = 6;
        const auto partial = Analyse(stopped, Settings{});
        auto note = FindCheck(partial, "Floating check stopped");
        assert(note && note->severity == Severity::Note && CountCheck(partial, "Floating mesh") == 2);
        assert(note->message.find("stopped after 6 static meshes because") != std::string::npos);
        stopped.floors.checked = 1;
        assert(FindCheck(Analyse(stopped, Settings{}), "Floating check stopped")->message.find("after 1 static mesh because") != std::string::npos);
        assert(!FindCheck(report, "Floating check stopped"));

        // Gaps under 10 keep a decimal; 1 is singular.
        auto close = scene;
        close.floors.gaps = {{"MyLevel.Mat", "Mat", 1.04, {0, 0, 1.04}, {10, 10, 12}}};
        assert(FindCheck(Analyse(close, Settings{}), "Floating mesh")->message.find("Mat floats 1 unit above") == 0);

        // A flat mesh (under 2 units high: a decal, a puddle) lifted 2 units or
        // less is left out, even with the widest limits; higher, or a thicker
        // mesh, is listed.
        auto flat = scene;
        flat.floors.gaps = {
            {"MyLevel.Puddle", "Puddle", 1.5, {0, 0, 1.5}, {40, 40, 1.5}},
            {"MyLevel.Sign", "Sign", 2, {0, 50, 2}, {40, 60, 3.9}},
            {"MyLevel.Stain", "Stain", 0.6, {0, 70, 0.6}, {40, 80, 1}},
            {"MyLevel.Sheet", "Sheet", 2.5, {50, 0, 2.5}, {90, 40, 3.5}},
            {"MyLevel.Plank", "Plank", 1.5, {50, 50, 1.5}, {90, 60, 3.5}},
        };
        Settings widest;
        widest.floatMinGap = 0.5;
        for (const auto& settings : {Settings{}, widest})
        {
            const auto flats = Analyse(flat, settings);
            assert(CountCheck(flats, "Floating mesh") == 2);
            assert(FindCheck(flats, "Floating mesh")->actors[0] == "MyLevel.Sheet" && FindCheck(flats, "Floating mesh", 1)->actors[0] == "MyLevel.Plank");
            assert(FindCheck(flats, "Floating meshes")->actors == (std::vector<std::string>{"MyLevel.Sheet", "MyLevel.Plank"}));
        }

        // Wider limits, and FloatingMaxGap 0 turns the check off.
        Settings wide;
        wide.floatMinGap = 0.5;
        wide.floatMaxGap = 64;
        const auto widened = Analyse(scene, wide);
        assert(CountCheck(widened, "Floating mesh") == 4);
        assert(FindCheck(widened, "Floating mesh", 2)->message.find("Nudged floats 0.8 units above") == 0);
        assert(FindCheck(widened, "Floating meshes")->message.find("4 static meshes float 0.5-64 units") == 0);
        Settings off;
        off.floatMaxGap = 0;
        off = ClampSettings(off);
        assert(off.floatMaxGap == 0 && !FindCheck(Analyse(scene, off), "Floating mesh") && !FindCheck(Analyse(scene, off), "Floating meshes"));

        // Not run, or no BSP: nothing.
        auto notRun = scene;
        notRun.floors.ran = false;
        assert(!FindCheck(Analyse(notRun, Settings{}), "Floating mesh"));
        Scene unbuilt;
        unbuilt.floors = scene.floors;
        const auto skipped = Analyse(unbuilt, Settings{});
        assert(FindCheck(skipped, "BSP not built") && skipped.problems.size() == 1);

        // Settings bounds.
        Settings s;
        s.floatMinGap = 0.1;
        s.floatMaxGap = 1000;
        s = ClampSettings(s);
        assert(s.floatMinGap == 0.5 && s.floatMaxGap == 512);
        s.floatMinGap = 100;
        s.floatMaxGap = 10;
        s = ClampSettings(s);
        assert(s.floatMinGap == 64 && s.floatMaxGap == 64);
        s.floatMinGap = 8;
        s.floatMaxGap = -5;
        s = ClampSettings(s);
        assert(s.floatMinGap == 8 && s.floatMaxGap == 0);
    }

    // BSP point indices, read unsigned as the patched engine reads them.
    {
        assert(PointIndex(0x8001, 0x9000) == 0x8001);
        assert(PointIndex(0xFFFF, 70000) == -1);
        assert(PointIndex(0xABCD1234, 0x2000) == 0x1234);
        assert(PointIndex(0x2000, 0x2000) == -1 && PointIndex(0x1FFF, 0x2000) == 0x1FFF && PointIndex(0, 0) == -1);
    }

    // Check groups are listed for the window and future checks.
    assert(CheckGroups().size() == 5 && std::string(CheckGroups()[0].id) == "actors");
    assert(std::string(CheckGroups()[2].id) == "props" && std::string(CheckGroups()[3].id) == "collision" && std::string(CheckGroups()[4].id) == "stock");

    CollisionTests();

    std::puts("MapCheckModelTests passed");
    return 0;
}
