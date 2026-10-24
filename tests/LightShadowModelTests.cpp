#include "../Reloaded.Editor/LightShadowModel.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace LightShadow;

namespace
{
    bool Near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }
    uint32_t Grey(int value) { return 0xFF000000u | uint32_t(value) << 16 | uint32_t(value) << 8 | uint32_t(value); }
    // A flat square floor at height z, as two triangles, lightmapped onto atlas 0.
    void Square(Scene& scene, double x0, double y0, double x1, double y1, double z, int atlas = 0)
    {
        Floor a{ { x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { 0, 1, 1 }, { 0, 0, 1 }, atlas };
        Floor b{ { x0, y0, z }, { x1, y1, z }, { x0, y1, z }, { 0, 1, 0 }, { 0, 1, 1 }, atlas };
        scene.floors.push_back(a);
        scene.floors.push_back(b);
    }
    // A cube from -1 to 1 with outward vertex normals, its triangles wound
    // either way (Finish puts them right).
    MeshShape Cube()
    {
        MeshShape shape;
        for (int face = 0; face < 6; ++face)
        {
            const int axis = face / 2;
            const float sign = face % 2 ? 1.f : -1.f;
            const uint32_t first = uint32_t(shape.points.size());
            for (int corner = 0; corner < 4; ++corner)
            {
                float p[3];
                p[axis] = sign;
                p[(axis + 1) % 3] = corner & 1 ? 1.f : -1.f;
                p[(axis + 2) % 3] = corner & 2 ? 1.f : -1.f;
                shape.points.push_back({ p[0], p[1], p[2] });
                float n[3] = { 0, 0, 0 };
                n[axis] = sign;
                shape.normals.push_back({ n[0], n[1], n[2] });
            }
            shape.triangles.push_back({ first, first + 1, first + 3 });
            shape.triangles.push_back({ first, first + 3, first + 2 });
        }
        shape.key = 99;
        Finish(shape);
        return shape;
    }
    const Cell& CellAt(const std::vector<Cell>& cells, double x, double y, double z)
    {
        for (const auto& c : cells)
            if (Near(c.at.x, x) && Near(c.at.y, y) && Near(c.at.z, z)) return c;
        std::printf("no patch at %g %g %g\n", x, y, z);
        assert(false);
        return cells.front();
    }
}

int main()
{
    // Settings: clamped into a usable, ordered range.
    {
        Settings s; s.hiddenBelow = 0; s.exposedFrom = -5; s.cell = 3; s.source = Source(9); s.chest = 999;
        s = Clamp(s);
        assert(s.hiddenBelow == 1 && s.exposedFrom == 2 && s.cell == 16 && s.source == Source::Combined && s.chest == 160);
        Settings t; t.hiddenBelow = 300; t.exposedFrom = 10; t.cell = 4096;
        t = Clamp(t);
        assert(t.hiddenBelow == 254 && t.exposedFrom == 255 && t.cell == 512);
        Settings d = Clamp(Settings{});
        assert(d.hiddenBelow == 40 && d.exposedFrom == 100 && d.cell == 64 && d.meshShadows && d.meshFloors && d.spotCones);
    }
    // Bands: below hidden is hidden, from exposed up is exposed; NaN is dark.
    {
        Settings s;
        assert(Classify(0, s) == Band::Hidden && Classify(39.9, s) == Band::Hidden);
        assert(Classify(40, s) == Band::Partial && Classify(99.9, s) == Band::Partial);
        assert(Classify(100, s) == Band::Exposed && Classify(255, s) == Band::Exposed);
        assert(Classify(std::nan(""), s) == Band::Hidden);
        assert(std::string(BandName(Band::Partial)) == "partly visible");
    }
    // Lightmap texels: luminance from B,G,R,A, bilinear between texel centres.
    {
        assert(Near(Luminance(Grey(200)), 200));
        assert(Near(Luminance(0xFFFF0000u), 0.299 * 255)); // red is the third byte
        assert(Near(Luminance(0xFF0000FFu), 0.114 * 255)); // blue the first
        Image image{ 2, 1, { Grey(0), Grey(100) } };
        assert(Near(SampleLuminance(image, 0.25, 0.5), 0));
        assert(Near(SampleLuminance(image, 0.75, 0.5), 100));
        assert(Near(SampleLuminance(image, 0.5, 0.5), 50));
        assert(Near(SampleLuminance(image, -3, 0.5), 0) && Near(SampleLuminance(image, 9, 0.5), 100));
        assert(SampleLuminance(Image{}, 0.5, 0.5) < 0);
        assert(SampleLuminance(image, std::nan(""), 0.5) < 0);
        assert(Near(BakedBrightness(60), 120) && Near(BakedBrightness(200), 255) && BakedBrightness(-1) < 0);
    }
    // Light colour: white keeps everything, a saturated blue keeps little.
    {
        assert(Near(ColourShare(0, 255), 1) && Near(ColourShare(170, 255), 1));
        assert(Near(ColourShare(0, 0), 0.299));          // pure red
        assert(Near(ColourShare(85, 0), 0.587));         // pure green
        assert(Near(ColourShare(170, 0), 0.114));        // pure blue
        assert(ColourShare(160, 90) > ColourShare(160, 0) && ColourShare(160, 90) < 1);
    }
    // Falloff, as the engine's SampleIntensity: 2 at the light, 2(1 - 3x^2 + 2x^3)
    // at a share x of the reach, none at or past it.
    {
        Light light{ { 0, 0, 100 }, 400, 200 };
        assert(Near(LightAt(light, { 0, 0, 100 }), 400));
        assert(Near(LightAt(light, { 200, 0, 100 }), 200));
        assert(Near(LightAt(light, { 0, 300, 100 }), 200 * 2 * (1 - 3 * 0.5625 + 2 * 0.421875)));
        assert(Near(LightAt(light, { 0, 0, 500 }), 0) && Near(LightAt(light, { 0, 900, 100 }), 0));
        assert(Near(LightAt(Light{ { 0, 0, 0 }, 0, 200 }, { 0, 0, 0 }), 0));
        // On a surface the light's angle counts; facing away is unlit.
        const Point up{ 0, 0, 1 }, down{ 0, 0, -1 };
        assert(Near(Intensity(light, { 0, 0, 0 }, true, &up), 2 * (1 - 3 * 0.0625 + 2 * 0.015625)));
        assert(Near(Intensity(light, { 0, 0, 0 }, true, &down), 0));
        assert(Near(Intensity(light, { 100, 0, 0 }, true, &up), Intensity(light, { 100, 0, 0 }) * (100 / std::sqrt(20000.0))));
        // The other LightEffect falloffs.
        Light flat = light; flat.effect = Effect::NonIncidence;
        assert(Near(Intensity(flat, { 200, 0, 100 }), 2 * std::sqrt(0.52)));
        assert(Near(Intensity(flat, { 0, 0, 0 }, true, &down), 0));
        Light quadratic = light; quadratic.effect = Effect::QuadraticNonIncidence;
        assert(Near(Intensity(quadratic, { 200, 0, 100 }), 2 * (1.02 - 0.25)));
        Light cylinder = light; cylinder.effect = Effect::Cylinder;
        assert(Near(Intensity(cylinder, { 0, 0, 300 }), 2) && Near(Intensity(cylinder, { 200, 0, 100 }), 2 * 0.75));
        Light sun = light; sun.effect = Effect::Sunlight; sun.forward = { 0, 0, -1 }; sun.radius = 0;
        assert(Near(Intensity(sun, { 9000, 0, 0 }), 2) && Near(Intensity(sun, { 0, 0, 0 }, true, &up), 2) && Near(Intensity(sun, { 0, 0, 0 }, true, &down), 0));
    }
    // Spotlight cones: Sine = 1 - LightCone/256; ((cos - Sine)/(1 - Sine))^2 inside,
    // nothing outside or behind.
    {
        Light spot{ { 0, 0, 0 }, 1000, 100 };
        spot.effect = Effect::Spotlight; spot.cone = 128; spot.forward = { 1, 0, 0 };
        const double base = Intensity(Light{ { 0, 0, 0 }, 1000, 100 }, { 300, 0, 0 });
        assert(Near(Intensity(spot, { 300, 0, 0 }), base) && Near(ConeFactor(spot, { 300, 0, 0 }), 1)); // on the axis
        const double angle = std::acos(0.75);
        const Point at75{ 300 * std::cos(angle), 300 * std::sin(angle), 0 };
        assert(Near(ConeFactor(spot, at75), 0.25) && Near(Intensity(spot, at75), base * 0.25));
        const double edge = std::acos(0.5) + 0.01; // just outside 60 degrees
        assert(Near(ConeFactor(spot, { 300 * std::cos(edge), 300 * std::sin(edge), 0 }), 0));
        assert(Near(Intensity(spot, { -300, 0, 0 }), 0)); // behind
        assert(Near(Intensity(spot, { 0, 300, 0 }, false), Intensity(Light{ { 0, 0, 0 }, 1000, 100 }, { 0, 300, 0 }))); // cones off
        Light narrow = spot; narrow.cone = 16; // Sine 0.9375
        assert(Intensity(narrow, at75) == 0 && Intensity(narrow, { 300, 1, 0 }) > 0);
        Light none = spot; none.cone = 0;
        assert(Intensity(none, { 300, 0, 0 }) == 0);
        Light statics = spot; statics.effect = Effect::StaticSpot;
        assert(Near(Intensity(statics, at75), base * 0.25));
    }
    // LE_ESpotShadow's frustum: EchelonRange ahead, SpotWidth by SpotHeight at its
    // end, NearPlaneRatio of that at the lamp.
    {
        Light shadow{ { 0, 0, 0 }, 2000, 100 };
        shadow.effect = Effect::ESpotShadow; shadow.range = 1000; shadow.spotWidth = 250; shadow.spotHeight = 100; shadow.nearRatio = 0.1;
        // Half way: 0.1 + 0.9 * 0.5 = 0.55 of the full size.
        assert(InsideFrustum(shadow, { 500, 137, 0 }) && !InsideFrustum(shadow, { 500, 138, 0 }));
        assert(InsideFrustum(shadow, { 500, 0, 54 }) && !InsideFrustum(shadow, { 500, 0, 56 }));
        assert(InsideFrustum(shadow, { 1000, -250, 100 }) && !InsideFrustum(shadow, { 1001, 0, 0 }) && !InsideFrustum(shadow, { -1, 0, 0 }));
        assert(InsideFrustum(shadow, { 1, 25, 10 }) && !InsideFrustum(shadow, { 1, 30, 0 }));
        assert(Intensity(shadow, { 500, 200, 0 }) == 0 && Intensity(shadow, { 500, 100, 0 }) > 0);
        assert(Intensity(shadow, { 500, 200, 0 }, false) > 0); // cones off: a point light
        Light box = shadow; box.nearRatio = 0.99; // the engine's box
        assert(InsideFrustum(box, { 1, 249, 99 }) && !InsideFrustum(box, { 1, 251, 0 }));
        Light turned = shadow; turned.forward = { 0, 1, 0 }; turned.right = { -1, 0, 0 }; turned.up = { 0, 0, 1 };
        assert(InsideFrustum(turned, { -137, 500, 0 }) && !InsideFrustum(turned, { 0, -500, 0 }));
        Light plain = shadow; plain.range = 0; // no frustum read
        assert(InsideFrustum(plain, { -500, 0, 0 }));
    }
    // Walkable: up-facing in either winding, walls and steep ramps are not.
    {
        Floor up{ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } }, down{ { 0, 0, 0 }, { 0, 1, 0 }, { 1, 0, 0 } };
        Floor wall{ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 } }, ramp{ { 0, 0, 0 }, { 1, 0, 1.2 }, { 0, 1, 0 } };
        assert(Walkable(up) && Walkable(down) && !Walkable(wall) && !Walkable(ramp));
        assert(!Walkable(Floor{})); // degenerate
    }
    // Sampling a 256-unit floor on a 64-unit grid: 16 patches at grid centres.
    {
        Scene scene;
        scene.atlases.push_back(std::make_shared<Image>(Image{ 1, 1, { Grey(30) } })); // stored 30 reads as 60
        Square(scene, 0, 0, 256, 256, 10);
        Settings s;
        auto result = Sample(scene, s);
        assert(result.cells.size() == 16 && !result.truncated);
        for (const auto& cell : result.cells)
        {
            assert(std::fmod(cell.at.x - 32, 64) == 0 && std::fmod(cell.at.y - 32, 64) == 0 && cell.at.z == 10);
            assert(Near(cell.baked, 60) && Near(cell.lights, 0) && Near(cell.value, 60) && cell.band == Band::Partial);
        }
        // Bigger patches: fewer of them; the edge shared by the two triangles
        // gives no duplicates.
        s.cell = 128;
        assert(Sample(scene, s).cells.size() == 4);
        // A wall is not floor.
        Scene walls;
        walls.floors.push_back(Floor{ { 0, 0, 0 }, { 256, 0, 0 }, { 0, 0, 256 } });
        assert(Sample(walls, Settings{}).cells.empty());
    }
    // Stacked storeys each get patches; touching coplanar floors do not double up.
    {
        Scene scene;
        scene.atlases.push_back(std::make_shared<Image>(Image{ 1, 1, { Grey(10) } }));
        Square(scene, 0, 0, 128, 128, 0);
        Square(scene, 0, 0, 128, 128, 256);
        Square(scene, 0, 0, 128, 128, 4); // the same floor, a sliver higher
        auto result = Sample(scene, Settings{});
        assert(result.cells.size() == 8);
    }
    // In-game lights: add up, capped at 255, and blocked by solid BSP.
    {
        Scene scene;
        Square(scene, 0, 0, 64, 64, 0, -1); // no baked lighting
        // A quarter of their reach above chest height: 2 * (1 - 3/16 + 2/64) each.
        scene.lights.push_back(Light{ { 32, 32, 148 }, 400, 60 });
        scene.lights.push_back(Light{ { 32, 32, 148 }, 400, 30 });
        Settings s;
        auto result = Sample(scene, s);
        assert(result.cells.size() == 1);
        assert(result.cells[0].baked < 0 && Near(result.cells[0].lights, 90 * 2 * 0.84375, 1e-4) && result.cells[0].band == Band::Exposed);
        scene.lights.push_back(Light{ { 32, 32, 48 }, 400, 200 });
        assert(Near(Sample(scene, s).cells[0].lights, 255));
        // A solid slab between z=100 and z=200 (outside above 200 and below 100):
        // a light above it does not reach the floor, one below does.
        Scene blocked;
        Square(blocked, 0, 0, 64, 64, 0, -1);
        blocked.bsp = { { { 0, 0, 1 }, 200, -1, 1, false }, { { 0, 0, -1 }, -100, -1, -1, true } };
        blocked.rootOutside = true;
        assert(LocalLightingMatch::ClearSegment(blocked.bsp, blocked.rootOutside, { 0, 0, 10 }, { 0, 0, 90 }));
        assert(!LocalLightingMatch::ClearSegment(blocked.bsp, blocked.rootOutside, { 0, 0, 10 }, { 0, 0, 300 }));
        assert(BspClear(blocked.bsp, blocked.rootOutside, { 0, 0, 10 }, { 0, 0, 90 }));
        assert(!BspClear(blocked.bsp, blocked.rootOutside, { 0, 0, 10 }, { 0, 0, 300 }));
        blocked.lights.push_back(Light{ { 32, 32, 300 }, 1000, 200 });
        assert(Near(Sample(blocked, s).cells[0].lights, 0));
        blocked.lights.push_back(Light{ { 32, 32, 90 }, 1000, 50 });
        auto lit = Sample(blocked, s).cells[0];
        assert(lit.lights > 95 && lit.lights < 100);
        // A lamp embedded a little in its fitting still reaches.
        Scene fitting = blocked;
        fitting.lights = { Light{ { 32, 32, 108 }, 1000, 50 } };
        assert(Sample(fitting, s).cells[0].lights > 80);
    }
    // Transforms: inverse round trip; a mirror shows in the determinant.
    {
        const Affine a = FromAxes({ 100, -20, 5 }, { 0, 2, 0 }, { -3, 0, 0 }, { 0, 0, 0.5 });
        Affine inverse;
        assert(Invert(a, inverse));
        const Point p{ 7, -11, 13 };
        const Point back = inverse.Apply(a.Apply(p));
        assert(Near(back.x, p.x, 1e-9) && Near(back.y, p.y, 1e-9) && Near(back.z, p.z, 1e-9));
        assert(Near(a.Determinant(), 3) && FromAxes({}, { -1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }).Determinant() < 0);
        Affine flat;
        assert(!Invert(FromAxes({}, { 1, 0, 0 }, { 0, 0, 0 }, { 0, 0, 1 }), flat));
    }
    // Segments against boxes, triangles and the hierarchy (checked against every box).
    {
        const float lo[3] = { 0, 0, 0 }, hi[3] = { 10, 10, 10 };
        auto hits = [&](Point o, Point d) { return SegmentHitsBox(lo, hi, o, Inverse(d)); };
        assert(hits({ -5, 5, 5 }, { 20, 0, 0 }) && !hits({ -5, 5, 5 }, { 4, 0, 0 }) && !hits({ -5, 15, 5 }, { 20, 0, 0 }));
        assert(hits({ 5, 5, 5 }, { 1, 0, 0 }) && hits({ -5, -5, -5 }, { 20, 20, 20 }) && !hits({ 5, 5, 20 }, { 0, 0, 5 }));
        assert(hits({ 0, 5, 5 }, { 0, 0, 1 })); // on a face, along it
        const Point a{ 0, 0, 0 }, b{ 10, 0, 0 }, c{ 0, 10, 0 };
        assert(SegmentHitsTriangle({ 2, 2, -5 }, { 0, 0, 10 }, a, b, c) && SegmentHitsTriangle({ 2, 2, 5 }, { 0, 0, -10 }, a, b, c));
        assert(!SegmentHitsTriangle({ 2, 2, -5 }, { 0, 0, 4 }, a, b, c));  // stops short
        assert(!SegmentHitsTriangle({ 8, 8, -5 }, { 0, 0, 10 }, a, b, c)); // beside the long edge
        assert(!SegmentHitsTriangle({ 2, 2, 1 }, { 5, 0, 0 }, a, b, c));   // parallel
        std::vector<Box> boxes;
        unsigned seed = 12345;
        auto random = [&] { seed = seed * 1103515245u + 12345u; return double((seed >> 8) % 10000) / 10; };
        for (int i = 0; i < 400; ++i)
        {
            Box box;
            const Point at{ random(), random(), random() };
            box.Add(at);
            box.Add(at + Point{ random() / 5, random() / 5, random() / 5 });
            boxes.push_back(box);
        }
        const auto bvh = BuildBvh(boxes);
        size_t total = 0;
        for (int trial = 0; trial < 300; ++trial)
        {
            const Point o{ random(), random(), random() }, d{ random() - 500, random() - 500, random() - 500 };
            const Point inverse = Inverse(d);
            std::vector<uint32_t> found;
            AnyHit(bvh, o, d, [&](uint32_t i) { found.push_back(i); return false; });
            std::sort(found.begin(), found.end());
            for (uint32_t i = 0; i < boxes.size(); ++i)
            {
                const float l[3] = { float(boxes[i].lo.x), float(boxes[i].lo.y), float(boxes[i].lo.z) };
                const float h[3] = { float(boxes[i].hi.x), float(boxes[i].hi.y), float(boxes[i].hi.z) };
                if (SegmentHitsBox(l, h, o, inverse)) { assert(std::binary_search(found.begin(), found.end(), i)); ++total; }
            }
        }
        assert(total > 50); // the trial lines did cross boxes
    }
    // Static meshes: a box blocks the line to a light and beside or over it does
    // not; the lamp's own small fitting does not; a mesh players stand on gets
    // patches on its top only.
    {
        auto shape = std::make_shared<MeshShape>(Cube());
        assert(shape->triangles.size() == 12 && Near(shape->bounds.lo.x, -1) && Near(shape->bounds.hi.z, 1));
        Scene scene;
        scene.shapes.push_back(shape);
        // A 128-unit box (the cube scaled 64) standing on the floor at x and y 0..128.
        MeshInstance box;
        box.id = 1;
        box.toWorld = FromAxes({ 64, 64, 64 }, { 64, 0, 0 }, { 0, 64, 0 }, { 0, 0, 64 });
        assert(Place(box, *shape) && Near(box.box.lo.x, 0) && Near(box.box.hi.z, 128));
        scene.instances.push_back(box);
        Prepare(scene);
        assert(scene.blockers.size() == 1);
        const Point lamp{ 64, 400, 64 };
        assert(!MeshClear(scene, { 64, -200, 64 }, lamp, lamp));                  // through the box
        assert(MeshClear(scene, { 300, -200, 64 }, { 300, 400, 64 }, lamp));      // beside it
        assert(MeshClear(scene, { 64, -200, 300 }, { 64, 400, 300 }, lamp));      // over it
        // A lamp inside a small housing is not put out by it; a big hall around it still blocks.
        assert(IsFitting(box.box, { 64, 64, 64 }) && MeshClear(scene, { 64, -200, 64 }, { 64, 64, 64 }, { 64, 64, 64 }));
        MeshInstance hall = box;
        hall.id = 2;
        hall.toWorld = FromAxes({ 0, 0, 0 }, { 1000, 0, 0 }, { 0, 1000, 0 }, { 0, 0, 1000 });
        assert(Place(hall, *shape) && !IsFitting(hall.box, { 0, 0, 0 }));
        Scene inside;
        inside.shapes = scene.shapes;
        inside.instances = { hall };
        Prepare(inside);
        assert(!MeshClear(inside, { 0, 0, 0 }, { 0, 3000, 0 }, { 0, 0, 0 }));

        // Sampling: a floor 0..256 with the box on it, a light past the box.
        Square(scene, 0, 0, 256, 256, 0, -1);
        scene.lights.push_back(Light{ { 64, 400, 48 }, 1000, 100 });
        Settings s;
        auto result = Sample(scene, s);
        assert(Near(CellAt(result.cells, 96, 32, 0).lights, 0)); // behind the box from the light
        assert(CellAt(result.cells, 224, 32, 0).lights > 0);     // clear of it
        s.meshShadows = false;
        assert(CellAt(Sample(scene, s).cells, 96, 32, 0).lights > 0);
        s.meshShadows = true;
        // Standing on it: patches on its top (z 128) once its floors are added.
        scene.instances[0].floor = true;
        scene.instances[0].shade.assign(shape->points.size(), 30.f);
        AddMeshFloors(scene);
        size_t top = 0;
        for (const auto& f : scene.floors)
            if (f.mesh) { assert(Near(f.a.z, 128) && Near(f.b.z, 128) && Near(f.c.z, 128)); ++top; }
        assert(top == 2); // its top face only: not its sides or underside
        result = Sample(scene, s);
        const auto onTop = CellAt(result.cells, 32, 32, 128);
        assert(onTop.mesh && Near(onTop.baked, 30) && onTop.lights > 0);
        assert(Summarise(result.cells).mesh == 4);
        s.meshFloors = false;
        assert(Summarise(Sample(scene, s).cells).mesh == 0);
        // Mirrored (a negative scale): still its top only.
        Scene mirrored;
        mirrored.shapes = scene.shapes;
        MeshInstance flipped = box;
        flipped.floor = true;
        flipped.toWorld = FromAxes({ 64, 64, 64 }, { -64, 0, 0 }, { 0, 64, 0 }, { 0, 0, 64 });
        assert(Place(flipped, *shape));
        mirrored.instances = { flipped };
        AddMeshFloors(mirrored);
        assert(mirrored.floors.size() == 2 && Near(mirrored.floors[0].a.z, 128));
    }
    // A static mesh vertex's baked brightness from the static lights that reach it.
    {
        const Light a{ { 0, 0, 100 }, 400, 50 }, b{ { 0, 0, -100 }, 400, 50 };
        const Point up{ 0, 0, 1 };
        assert(Near(VertexShade({ &a }, { 0, 0, 0 }, up), 50 * Intensity(a, { 0, 0, 0 }, true, &up)));
        assert(Near(VertexShade({ &a, &b }, { 0, 0, 0 }, up), VertexShade({ &a }, { 0, 0, 0 }, up))); // b is below the surface
        assert(Near(VertexShade({}, { 0, 0, 0 }, up), 0));
    }
    // Kept between samples: an unchanged map reuses every light; a moved light, or
    // a mesh moved within a light's reach, is worked out again and the rest kept.
    // Every result matches a sample from nothing.
    {
        auto shape = std::make_shared<MeshShape>(Cube());
        Scene scene;
        scene.shapes.push_back(shape);
        Square(scene, 0, 0, 1024, 256, 0, -1);
        MeshInstance box;
        box.id = 7;
        box.toWorld = FromAxes({ 128, 128, 64 }, { 32, 0, 0 }, { 0, 32, 0 }, { 0, 0, 64 });
        assert(Place(box, *shape));
        scene.instances.push_back(box);
        Light nearLight{ { 200, 128, 120 }, 300, 80 }; nearLight.id = 1;
        Light farLight{ { 900, 128, 120 }, 300, 80 }; farLight.id = 2;
        scene.lights = { nearLight, farLight };
        Prepare(scene);
        Settings s;
        LightCache cache;
        RelightStats stats;
        auto matches = [&](const std::vector<Cell>& cells) {
            const auto fresh = Sample(scene, s).cells;
            assert(fresh.size() == cells.size());
            for (size_t i = 0; i < cells.size(); ++i) assert(Near(fresh[i].lights, cells[i].lights, 1e-3) && fresh[i].band == cells[i].band);
        };
        auto relight = [&](unsigned threads) {
            auto cells = SampleCells(scene, s).cells;
            Relight(scene, cells, s, cache, &stats, threads);
            matches(cells);
        };
        relight(2);
        assert(stats.full && stats.lights == 2 && stats.reused == 0 && stats.tested > 0);
        relight(2);
        assert(!stats.full && stats.reused == 2 && stats.tested == 0 && stats.kept > 0);
        // The far light moves: only it is worked out again.
        scene.lights[1].at.x = 850;
        relight(1);
        assert(!stats.full && stats.reused == 1 && stats.tested > 0);
        // The box moves, within the near light's reach only.
        scene.instances[0].toWorld.m[0][3] = 160;
        assert(Place(scene.instances[0], *shape));
        Prepare(scene);
        relight(1);
        assert(!stats.full && stats.reused == 1);
        // A new floor: the old patches are kept, the new ones worked out.
        Square(scene, 0, 300, 1024, 364, 0, -1);
        relight(1);
        assert(!stats.full && stats.reused == 2 && stats.kept > 0 && stats.tested > 0);
        // A setting the lights depend on starts again; so does the BSP.
        s.chest = 30;
        relight(1);
        assert(stats.full && stats.reused == 0);
        scene.bsp = { { { 0, 0, 1 }, 2000, -1, -1, false } };
        relight(1);
        assert(stats.full);
        relight(1);
        assert(!stats.full && stats.reused == 2);
    }
    // Sources: combined takes the brighter; baked-only counts unbaked floors dark.
    {
        std::vector<Cell> cells(2);
        cells[0].baked = 150; cells[0].lights = 20;
        cells[1].baked = -1; cells[1].lights = 120;
        Settings s;
        ApplyBands(cells, s);
        assert(Near(cells[0].value, 150) && cells[0].band == Band::Exposed);
        assert(Near(cells[1].value, 120) && cells[1].band == Band::Exposed);
        s.source = Source::Lights;
        ApplyBands(cells, s);
        assert(cells[0].band == Band::Hidden && cells[1].band == Band::Exposed);
        s.source = Source::Baked;
        ApplyBands(cells, s);
        assert(cells[0].band == Band::Exposed && cells[1].band == Band::Hidden);
        auto summary = Summarise(cells);
        assert(summary.total == 2 && summary.bands[0] == 1 && summary.bands[2] == 1 && summary.unbaked == 1);
        assert(Percent(1, 3) == 33 && Percent(2, 3) == 67 && Percent(0, 0) == 0);
    }
    // Legend and readout text.
    {
        std::vector<Cell> cells(4);
        for (int i = 0; i < 4; ++i) cells[i].baked = i * 60.0;
        ApplyBands(cells, Settings{});
        auto lines = Legend(Summarise(cells), Settings{}, false, false);
        assert(lines.size() == 4);
        assert(lines[0] == "Light and Shadow (baked + in-game lights): 4 floor patches of 64 uu");
        assert(Near(Percentile(cells, 0), 0) && Near(Percentile(cells, 0.5), 120) && Near(Percentile(cells, 1), 180));
        assert(Near(Percentile({}, 0.5), 0));
        assert(lines[1].find("darker than 40: 25%") != std::string::npos);
        assert(lines[2].find("partly visible: 25%") != std::string::npos);
        assert(lines[3].find("100 or brighter: 50%") != std::string::npos);
        auto empty = Legend(Summary{}, Settings{}, false, false);
        assert(empty.size() == 5 && empty[4].find("No walkable floor") == 0);
        Settings noMesh; noMesh.meshFloors = false;
        assert(Legend(Summary{}, noMesh, false, false)[4].find("include static mesh floors") != std::string::npos);
        Summary meshes = Summarise(cells); meshes.mesh = 2;
        assert(Legend(meshes, Settings{}, false, false)[0].find(", 2 on static meshes") != std::string::npos);
        auto warned = Legend(Summarise(std::vector<Cell>(3)), Settings{}, true, true);
        assert(warned[0].find("refreshing") != std::string::npos && warned.size() == 6);
        assert(Describe(cells[1]) == "partly visible, brightness 60 (baked 60, in-game lights 0)");
        Cell none; none.lights = 12; none.value = 12;
        assert(Describe(none) == "hidden, brightness 12 (baked none, in-game lights 12)");
        none.mesh = true;
        assert(Describe(none) == "hidden, brightness 12 (baked none, in-game lights 12) on a static mesh");
    }
    // Drawing: four sides, plus a diagonal per band step.
    {
        Cell cell; cell.at = { 100, 200, 30 };
        cell.band = Band::Hidden;
        auto lines = Outline(cell, 64);
        assert(lines.size() == 4 && Near(lines[0].first.z, 32));
        assert(Near(lines[0].first.x, 100 - 64 * 0.42) && Near(lines[1].second.y, 200 + 64 * 0.42));
        cell.band = Band::Partial;
        assert(Outline(cell, 64).size() == 5);
        cell.band = Band::Exposed;
        assert(Outline(cell, 64).size() == 6);
    }
    // Readout lookup: within half a patch, the highest floor not above the point.
    {
        std::vector<Cell> cells(3);
        cells[0].at = { 32, 32, 0 }; cells[1].at = { 32, 32, 256 }; cells[2].at = { 96, 32, 0 };
        assert(At(cells, 40, 20, 64) == &cells[1]);
        assert(At(cells, 40, 20, 64, 100) == &cells[0]);
        assert(At(cells, 90, 20, 64) == &cells[2]);
        assert(At(cells, 500, 20, 64) == nullptr);
    }
    // The cap on patches.
    {
        Scene huge;
        Square(huge, 0, 0, 16 * 600, 16 * 600, 0, -1);
        Settings s; s.cell = 16;
        Square(huge, 20000, 0, 20064, 64, 0, -1); // a small room, listed last
        auto result = Sample(huge, s);
        assert(result.truncated && result.cells.size() == kMaxCells);
        // The small floor is sampled before the vast one is cut short.
        assert(std::any_of(result.cells.begin(), result.cells.end(), [](const Cell& c) { return c.at.x > 20000; }));
    }
    std::puts("LightShadowModelTests passed");
    return 0;
}
