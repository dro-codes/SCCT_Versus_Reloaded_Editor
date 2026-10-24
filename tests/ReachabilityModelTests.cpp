#include "../Reloaded.Editor/ReachabilityModel.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace Reach;

namespace
{
    // A level of solid axis-aligned boxes, swept the way the engine sweeps a
    // pawn: a box against boxes, stopping a little short of the first one met.
    struct Box { Vec3 lo, hi; };
    class BoxWorld : public World
    {
    public:
        std::vector<Box> boxes;
        size_t sweeps = 0;
        void Add(Vec3 lo, Vec3 hi) { boxes.push_back({lo, hi}); }
        Hit Sweep(const Vec3& from, const Vec3& to, const Vec3& extent) override
        {
            ++sweeps;
            Hit best;
            double bestT = 2;
            const Vec3 d = Sub(to, from);
            for (const auto& box : boxes)
            {
                Vec3 lo = Reach::Sub(box.lo, extent), hi = Reach::Add(box.hi, extent);
                double enter = 0, leave = 1;
                int axis = -1;
                bool miss = false;
                for (int i = 0; i < 3 && !miss; ++i)
                {
                    if (std::abs(d[i]) < 1e-12)
                    {
                        if (!(from[i] > lo[i] && from[i] < hi[i])) miss = true;
                        continue;
                    }
                    double t0 = (lo[i] - from[i]) / d[i], t1 = (hi[i] - from[i]) / d[i];
                    if (t0 > t1) std::swap(t0, t1);
                    if (t0 > enter) { enter = t0; axis = i; }
                    leave = (std::min)(leave, t1);
                    if (enter >= leave) miss = true;
                }
                if (miss || enter >= 1 || leave <= 0) continue;
                if (enter < bestT)
                {
                    bestT = enter;
                    best.hit = true;
                    best.normal = {0, 0, 0};
                    if (axis >= 0) best.normal[static_cast<size_t>(axis)] = d[static_cast<size_t>(axis)] > 0 ? -1 : 1;
                }
            }
            if (best.hit)
            {
                const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const double t = (std::max)(0.0, bestT - (length > 0 ? 0.1 / length : 0));
                best.location = {from[0] + d[0] * t, from[1] + d[1] * t, from[2] + d[2] * t};
            }
            return best;
        }
        bool Fits(const Vec3& centre, const Vec3& extent) override
        {
            for (const auto& box : boxes)
            {
                bool overlap = true;
                for (int i = 0; i < 3; ++i)
                    overlap = overlap && centre[i] - extent[i] < box.hi[i] - 0.01 && centre[i] + extent[i] > box.lo[i] + 0.01;
                if (overlap) return false;
            }
            return true;
        }
        // A closed room: floor at z 0 and four walls 400 high.
        void Room(double x0, double y0, double x1, double y1)
        {
            Add({x0 - 16, y0 - 16, -16}, {x1 + 16, y1 + 16, 0});
            Add({x0 - 16, y0 - 16, 0}, {x0, y1 + 16, 400});
            Add({x1, y0 - 16, 0}, {x1 + 16, y1 + 16, 400});
            Add({x0, y0 - 16, 0}, {x1, y0, 400});
            Add({x0, y1, 0}, {x1, y1 + 16, 400});
        }
    };

    // Hits whatever is below with a fixed floor normal.
    class SlopeWorld : public World
    {
    public:
        double normalZ = 0.5;
        Hit Sweep(const Vec3& from, const Vec3& to, const Vec3&) override
        {
            Hit hit;
            if (to[2] < from[2] - 1) { hit.hit = true; hit.location = {from[0], from[1], from[2] - 1}; hit.normal = {std::sqrt(1 - normalZ * normalZ), 0, normalZ}; }
            return hit;
        }
        bool Fits(const Vec3&, const Vec3&) override { return true; }
    };

    Flood Fill(BoxWorld& world, int team, const Vec3& start, Settings settings = {})
    {
        Flood flood(world, DefaultPawn(team), settings);
        flood.AddStart(0, start);
        while (!flood.Step(64)) {}
        return flood;
    }
    bool Any(const Flood& flood, bool (*test)(const Node&))
    {
        for (const auto& node : flood.Nodes()) if (test(node)) return true;
        return false;
    }
    size_t CountChecks(const std::vector<Finding>& findings, const std::string& check)
    {
        size_t n = 0;
        for (const auto& f : findings) n += f.check == check;
        return n;
    }
}

int main()
{
    // Settings and pawns clamp to usable values.
    {
        Settings s; s.cell = 2; s.maxCells = 5; s.reach = -3;
        s = Clamp(s);
        assert(s.cell == 16 && s.maxCells == 1000 && s.reach == 0);
        Pawn p; p.radius = 0; p.height = 999; p.crouchHeight = 500; p.slope = 95;
        p = ClampPawn(p, kMerc);
        assert(p.radius == 35 && p.height == 87.5 && p.crouchHeight == 68 && p.slope == 45);
        assert(DefaultPawn(kSpy).height == 89 && DefaultPawn(kSpy).crouchHeight == 50 && DefaultPawn(kMerc).maxDrop == 950);
        assert(std::abs(DefaultPawn(kSpy).MinFloorZ() - 0.7061) < 0.001);
    }
    // A closed room: the fill covers its floor and stays inside the walls.
    {
        BoxWorld world;
        world.Room(-256, -256, 256, 256);
        auto flood = Fill(world, kSpy, {5, 3, 100});
        assert(flood.Seeds().size() == 1 && flood.Seeds()[0].node == 0 && flood.Seeds()[0].problem.empty());
        assert(std::abs(flood.Nodes()[0].at[2]) < 0.5);
        // The box (radius 35) keeps 35 off each wall: centres within 221 of the middle.
        for (const auto& node : flood.Nodes())
            assert(std::abs(node.at[0]) <= 221.1 && std::abs(node.at[1]) <= 221.1 && std::abs(node.at[2]) < 0.5);
        assert(flood.Nodes().size() >= 150 && flood.Nodes().size() <= 200);
        assert(flood.Done() && !flood.Truncated());
        assert(flood.Nodes()[0].moved);
    }
    // A doorway 80 wide off the grid's line is found by the sideways tries; 60 is too narrow.
    for (double width : {80.0, 60.0})
    {
        BoxWorld world;
        world.Add({-600, -300, -16}, {600, 300, 0});
        world.Add({-600, -316, 0}, {600, -300, 400});
        world.Add({-600, 300, 0}, {600, 316, 400});
        world.Add({-616, -316, 0}, {-600, 316, 400});
        world.Add({600, -316, 0}, {616, 316, 400});
        // The dividing wall at x 0, with its door centred at y 13.
        world.Add({-8, -300, 0}, {8, 13 - width / 2, 400});
        world.Add({-8, 13 + width / 2, 0}, {8, 300, 400});
        auto flood = Fill(world, kSpy, {-300, -200, 95});
        const bool through = Any(flood, [](const Node& n) { return n.at[0] > 100; });
        assert(through == (width == 80));
    }
    // A gap 110 high: a crouched spy (100) gets through, a merc (136 crouched) does not.
    {
        BoxWorld world;
        world.Room(-400, -200, 400, 200);
        world.Add({-8, -200, 0}, {8, -60, 400});
        world.Add({-8, 60, 0}, {8, 200, 400});
        world.Add({-8, -60, 110}, {8, 60, 400});
        auto spy = Fill(world, kSpy, {-300, 0, 95});
        auto merc = Fill(world, kMerc, {-300, 0, 95});
        assert(Any(spy, [](const Node& n) { return n.at[0] > 100; }));
        assert(Any(spy, [](const Node& n) { return n.crouchOnly; }));
        assert(!Any(merc, [](const Node& n) { return n.at[0] > 8; }));
        // Past the gap the spy stands again.
        for (const auto& node : spy.Nodes()) if (node.at[0] > 100) assert(!node.crouchOnly);
        const Flood* floods[2] = {&spy, &merc};
        const auto patches = Patches(floods);
        const auto summary = Summarise(patches);
        assert(summary.spies > 0 && summary.both > 0 && summary.mercs == 0);
        for (const auto& p : patches)
        {
            if (p.at[0] > 100) assert(p.teams == 1);
            if (p.at[0] < -100) assert(p.teams == 3);
        }
        // Lifting the gap's top past 140 lets the merc crawl through as well.
        world.boxes.back().lo[2] = 145;
        auto merc2 = Fill(world, kMerc, {-300, 0, 95});
        assert(Any(merc2, [](const Node& n) { return n.at[0] > 100; }));
    }
    // Steps: 30 is walked up, 60 is not.
    {
        BoxWorld world;
        world.Room(-400, -400, 400, 400);
        world.Add({100, -100, 0}, {300, 100, 30});
        world.Add({-300, -100, 0}, {-120, 100, 60});
        auto flood = Fill(world, kSpy, {0, 250, 95});
        assert(Any(flood, [](const Node& n) { return std::abs(n.at[2] - 30) < 0.5; }));
        assert(!Any(flood, [](const Node& n) { return std::abs(n.at[2] - 60) < 0.5; }));
    }
    // Stairs of 16-unit risers lead up to a landing.
    {
        BoxWorld world;
        world.Room(-600, -200, 600, 200);
        for (int i = 0; i < 10; ++i) world.Add({-100.0 + i * 24, -200, 0}, {600, 200, 16.0 * (i + 1)});
        auto flood = Fill(world, kSpy, {-450, 0, 95});
        assert(Any(flood, [](const Node& n) { return std::abs(n.at[2] - 160) < 0.5; }));
    }
    // Drops: off a 300 ledge both teams; off a 1200 ledge spies only (their drop limit is 1400, mercs' 950).
    for (double height : {300.0, 1200.0})
    {
        BoxWorld world;
        world.Room(-800, -800, 800, 800);
        world.boxes[1].hi[2] = world.boxes[2].hi[2] = world.boxes[3].hi[2] = world.boxes[4].hi[2] = 2000;
        world.Add({-200, -200, 0}, {200, 200, height});
        auto spy = Fill(world, kSpy, {0, 0, height + 95});
        auto merc = Fill(world, kMerc, {0, 0, height + 95});
        auto low = [](const Node& n) { return std::abs(n.at[2]) < 0.5; };
        assert(Any(spy, low));
        assert(Any(merc, low) == (height == 300));
        // Nobody climbs back up: the ledge's top is reached from the start only.
    }
    // A start inside solid is stuck; one in a closet too small to step in reaches nothing.
    {
        BoxWorld world;
        world.Room(-500, -500, 500, 500);
        world.Add({200, 200, 0}, {400, 400, 400});
        // The closet: 90 square inside, walls 400 high.
        world.Add({-400, -400, 0}, {-300, -290, 400});
        world.Add({-400, -200, 0}, {-300, -100, 400});
        world.Add({-400, -290, 0}, {-390, -200, 400});
        world.Add({-310, -290, 0}, {-300, -200, 400});
        Scene scene;
        scene.starts = {{"MyLevel.Start0", "Start0", kSpy, {0, 0, 95}}, {"MyLevel.Start1", "Start1", kSpy, {300, 300, 95}},
                        {"MyLevel.Start2", "Start2", kSpy, {-350, -245, 95}}, {"MyLevel.Start3", "Start3", kMerc, {10, 0, 95}}};
        Flood spy(world, DefaultPawn(kSpy), {});
        Flood merc(world, DefaultPawn(kMerc), {});
        for (int i = 0; i < 4; ++i) (scene.starts[i].team == kSpy ? spy : merc).AddStart(i, scene.starts[i].location);
        while (!spy.Step(64)) {}
        while (!merc.Step(64)) {}
        assert(spy.Seeds()[1].node < 0 && !spy.Seeds()[1].problem.empty());
        assert(spy.Seeds()[2].node >= 0 && !spy.Nodes()[static_cast<size_t>(spy.Seeds()[2].node)].moved);
        const Flood* floods[2] = {&spy, &merc};
        const auto findings = Findings(scene, floods);
        assert(CountChecks(findings, "Start reaches nothing") == 2);
        for (const auto& f : findings)
            if (f.check == "Start reaches nothing") assert(f.severity == 0 && f.actors.size() == 1 && (f.actors[0] == "MyLevel.Start1" || f.actors[0] == "MyLevel.Start2"));
    }
    // A merc start next to a spy start stands on the spy fill's cells only in
    // its own fill; a second start inside the first one's area joins it.
    {
        BoxWorld world;
        world.Room(-300, -300, 300, 300);
        Flood flood(world, DefaultPawn(kSpy), {});
        flood.AddStart(0, {0, 0, 95});
        while (!flood.Step(64)) {}
        flood.AddStart(1, {3, 2, 95});
        assert(flood.Seeds()[1].joined && flood.Seeds()[1].node >= 0);
    }
    // Objectives and pickups: what each team cannot get to.
    {
        BoxWorld world;
        world.Room(-400, -400, 400, 400);
        // A sealed vault in the corner.
        world.Add({200, 200, 0}, {216, 400, 400});
        world.Add({200, 200, 0}, {400, 216, 400});
        Scene scene;
        scene.starts = {{"L.SpyStart", "SpyStart", kSpy, {-300, -300, 95}}, {"L.MercStart", "MercStart", kMerc, {-300, 300, 95}}};
        Target open; open.path = "L.Terminal"; open.name = "Terminal"; open.kind = "Computer terminal"; open.location = {0, 0, 40}; open.radius = 20; open.height = 40;
        Target vault = open; vault.path = "L.VaultTerminal"; vault.name = "VaultTerminal"; vault.location = {320, 320, 40};
        Target pickup; pickup.path = "L.Ammo"; pickup.name = "Ammo"; pickup.kind = "Pickup"; pickup.objective = false; pickup.team = kMerc; pickup.location = {330, 330, 20};
        Target high = open; high.path = "L.High"; high.name = "High"; high.location = {0, 0, 600};
        scene.targets = {open, vault, pickup, high};
        Flood spy(world, DefaultPawn(kSpy), {});
        Flood merc(world, DefaultPawn(kMerc), {});
        spy.AddStart(0, scene.starts[0].location);
        merc.AddStart(1, scene.starts[1].location);
        while (!spy.Step(64)) {}
        while (!merc.Step(64)) {}
        assert(Reaches(spy, open, 48) && !Reaches(spy, vault, 48) && !Reaches(spy, high, 48));
        const Flood* floods[2] = {&spy, &merc};
        const auto findings = Findings(scene, floods);
        assert(CountChecks(findings, "Spies cannot reach") == 2);
        assert(CountChecks(findings, "Mercs cannot reach") == 2);
        assert(CountChecks(findings, "Pickup out of reach") == 1);
        assert(CountChecks(findings, "Start reaches nothing") == 0);
        for (const auto& f : findings)
        {
            if (f.check == "Spies cannot reach") assert(f.severity == 1 && (f.actors[0] == "L.VaultTerminal" || f.actors[0] == "L.High"));
            if (f.check == "Mercs cannot reach") assert(f.severity == 1);
            if (f.check == "Pickup out of reach") assert(f.actors[0] == "L.Ammo" && f.message.find("mercs") != std::string::npos);
        }
        // No merc starts: a note instead of merc rows.
        const Flood* spiesOnly[2] = {&spy, nullptr};
        const auto alone = Findings(scene, spiesOnly);
        assert(CountChecks(alone, "No merc starts") == 1 && CountChecks(alone, "Mercs cannot reach") == 0);
    }
    // Pickup teams from class names.
    {
        assert(PickupTeam({"SSpyAmmoPickup", "Pickup"}) == kSpy);
        assert(PickupTeam({"SMercMedkit", "Pickup"}) == kMerc);
        assert(PickupTeam({"SHealthPickup", "Pickup", "Actor"}) == -1);
    }
    // A floor steeper than the walkable slope is not stood on.
    {
        SlopeWorld world;
        Flood flood(world, DefaultPawn(kSpy), {});
        flood.AddStart(0, {0, 0, 100});
        assert(flood.Seeds()[0].node < 0 && flood.Seeds()[0].problem.find("steep") != std::string::npos);
        world.normalZ = 0.8;
        Flood gentle(world, DefaultPawn(kSpy), {});
        gentle.AddStart(0, {0, 0, 100});
        assert(gentle.Seeds()[0].node == 0);
    }
    // The fill stops at MaxCells and says so.
    {
        BoxWorld world;
        world.Room(-3000, -3000, 3000, 3000);
        Settings s; s.maxCells = 1000;
        auto flood = Fill(world, kSpy, {0, 0, 95}, s);
        assert(flood.Truncated() && flood.Nodes().size() == 1000);
        Scene scene;
        scene.starts = {{"L.S", "S", kSpy, {0, 0, 95}}};
        const Flood* floods[2] = {&flood, nullptr};
        assert(CountChecks(Findings(scene, floods), "Reachability stopped") == 1);
    }
    // Bounds keep the fill inside the map.
    {
        BoxWorld world;
        world.Add({-3000, -3000, -16}, {3000, 3000, 0});
        Flood flood(world, DefaultPawn(kSpy), {}, Flood::Bounds{{-200, -200, -100}, {200, 200, 100}});
        flood.AddStart(0, {0, 0, 95});
        while (!flood.Step(64)) {}
        for (const auto& node : flood.Nodes()) assert(std::abs(node.at[0]) <= 200 && std::abs(node.at[1]) <= 200);
        assert(flood.Nodes().size() > 100);
    }
    // Drawing: a square for both teams, a diagonal for spies only, both for mercs only.
    {
        Patch both{{0, 0, 0}, 3}, spies{{0, 0, 0}, 1}, mercs{{0, 0, 0}, 2};
        assert(Outline(both, 32).size() == 4 && Outline(spies, 32).size() == 5 && Outline(mercs, 32).size() == 6);
        assert(Outline(both, 32)[0].first[2] == 3);
        Summary s; s.both = 1200; s.spies = 3; s.mercs = 0;
        const auto lines = Legend(s, 32, false);
        assert(lines.size() == 6 && lines[1].find("1,200") != std::string::npos && lines[4].find("ladders") != std::string::npos);
        assert(Legend(s, 32, true)[0].find("changed") != std::string::npos);
        assert(Number(1234567) == "1,234,567");
    }
    std::puts("ReachabilityModelTests passed");
    return 0;
}
