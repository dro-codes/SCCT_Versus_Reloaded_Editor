// Import OBJ: the OBJ and MTL readers, the axes, the convex hull builder, the splitter and the ASE
// text. The import itself (textures, the editor's importer, placing) is checked in the editor.
#include "../Reloaded.Editor/ObjImportModel.h"
#include <iostream>
#include <random>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
using namespace ObjImport;
int checks = 0;
void Check(bool value, const std::string& message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(message + " (line " + std::to_string(where.line()) + ")");
}
bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
size_t Count(const std::string& text, const std::string& part)
{
    size_t n = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}
template <class F> bool Throws(F f, const std::string& part = "")
{
    try { f(); }
    catch (const std::exception& e) { return part.empty() || Contains(e.what(), part); }
    return false;
}

// A box's OBJ text, its faces wound counter-clockwise seen from outside (as exporters write them).
std::string BoxObj(double x0, double y0, double z0, double x1, double y1, double z1, int base = 0, const std::string& object = "")
{
    std::ostringstream o;
    if (!object.empty()) o << "o " << object << "\n";
    for (int i = 0; i < 8; ++i) o << "v " << ((i & 1) ? x1 : x0) << " " << ((i & 2) ? y1 : y0) << " " << ((i & 4) ? z1 : z0) << "\n";
    const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (const auto& q : quads) o << "f " << q[0] + 1 + base << " " << q[1] + 1 + base << " " << q[2] + 1 + base << " " << q[3] + 1 + base << "\n";
    return o.str();
}
// Every triangle faces away from the middle (for convex shapes around it).
bool Outward(const Mesh& m, const Vec3& middle)
{
    for (const auto& t : m.triangles)
    {
        const auto n = Cross(Sub(t.p[1], t.p[0]), Sub(t.p[2], t.p[0]));
        const auto c = Mul(Add(Add(t.p[0], t.p[1]), t.p[2]), 1.0 / 3);
        if (Dot(n, Sub(c, middle)) <= 0) return false;
    }
    return true;
}
bool HullOutward(const Hull& h)
{
    Vec3 middle{0, 0, 0};
    for (const auto& p : h.corners) middle = Add(middle, p);
    middle = Mul(middle, 1.0 / static_cast<double>(h.corners.size()));
    for (const auto& t : h.triangles)
    {
        const auto& a = h.corners[static_cast<size_t>(t[0])];
        const auto n = Cross(Sub(h.corners[static_cast<size_t>(t[1])], a), Sub(h.corners[static_cast<size_t>(t[2])], a));
        if (Dot(n, Sub(a, middle)) <= 0) return false;
    }
    return true;
}
// The vertices and faces of the ASE's first GEOMOBJECT named `name`, as written (Max axes, A B C).
struct AseObject { std::vector<Vec3> vertices; std::vector<std::array<int, 3>> faces; std::vector<int> smoothing; };
AseObject ReadAse(const std::string& text, const std::string& name)
{
    AseObject o;
    auto at = text.find("*NODE_NAME \"" + name + "\"");
    if (at == std::string::npos) throw std::runtime_error("no object " + name);
    auto end = text.find("*GEOMOBJECT", at);
    std::istringstream lines(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
    for (std::string line; std::getline(lines, line);)
    {
        std::istringstream w(line);
        std::string key;
        w >> key;
        if (key == "*MESH_VERTEX") { int i; Vec3 p; w >> i >> p[0] >> p[1] >> p[2]; o.vertices.push_back(p); }
        else if (key == "*MESH_FACE")
        {
            std::string label, a, b, c, skip;
            int va, vb, vc;
            w >> label >> a >> va >> b >> vb >> c >> vc;
            o.faces.push_back({va, vb, vc});
            const auto s = line.find("*MESH_SMOOTHING ");
            o.smoothing.push_back(s == std::string::npos ? -1 : std::stoi(line.substr(s + 16)));
        }
    }
    return o;
}
bool Near(double a, double b, double e = 1e-6) { return std::abs(a - b) <= e; }

int main()
{
    try
    {
        // ---- names
        Check(ValidName("Crate_01") && !ValidName("1Crate") && !ValidName("Crate 01") && !ValidName(""), "valid names");
        Check(SafeName("Material.001") == "Material_001", "dots become _");
        Check(SafeName("  big  crate (v2) ") == "_big_crate_v2" || SafeName("  big  crate (v2) ") == "big_crate_v2", "spaces tidied: " + SafeName("  big  crate (v2) "));
        Check(SafeName("01wall") == "_01wall", "a leading digit gets _");
        Check(SafeName("") == "Mesh" && SafeName("...", "Tex") == "Tex", "empty names fall back");
        Check(SafeName(std::string(80, 'a')).size() == 63, "names stop at 63");
        Check(CollisionName("UCX_Crate") && CollisionName("ucx_Crate_01") && CollisionName("MCDCX_Crate") && !CollisionName("Crate_UCX"), "collision names");
        Check(PartName("Crate", 0) == "Crate" && PartName("Crate", 2) == "Crate_3", "part names");

        // ---- MTL
        {
            const auto m = ParseMtl("# made by hand\r\nnewmtl Wood Planks\r\nKd 0.5 0.25 1\r\nmap_Kd -s 2 2 1 -o 0 0 0 -clamp on textures/wood planks.png\r\n"
                                    "newmtl Metal\nmap_Kd -imfchan r metal.tga\nnewmtl Plain\n");
            Check(m.size() == 3 && m[0].name == "Wood Planks" && m[1].name == "Metal" && m[2].name == "Plain", "three materials");
            Check(m[0].diffuseMap == "textures/wood planks.png", "map_Kd after its options, spaces kept: " + m[0].diffuseMap);
            Check(Near(m[0].diffuse[1], 0.25), "Kd read");
            Check(m[1].diffuseMap == "metal.tga" && m[2].diffuseMap.empty(), "map_Kd -imfchan and none");
        }

        // ---- OBJ
        {
            const auto f = ParseObj("mtllib my things.mtl\n# a comment\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 \\\n 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                                    "o Box\ng Lid\nusemtl Wood\ns 1\nf 1/1 2/2 3/3 4/4\nusemtl Metal\ns off\nf -4//1 -3//1 -2//1\nf 1 2\n"
                                    "o UCX_Box\nf 1 2 3\n");
            Check(f.positions.size() == 4 && f.uvs.size() == 4, "positions and uvs, a continued line joined");
            Check(f.libraries.size() == 1 && f.libraries[0] == "my things.mtl", "mtllib with a space");
            Check(f.faces.size() == 3 && f.skippedFaces == 1 && !f.warnings.empty(), "faces, a two-corner face skipped");
            Check(f.faces[0].corners.size() == 4 && f.faces[0].corners[2].v == 2 && f.faces[0].corners[2].vt == 2, "v/vt");
            Check(f.faces[1].corners[0].v == 0 && f.faces[1].corners[0].vt == -1, "negative indices, v//vn");
            Check(f.materials.size() == 2 && f.faces[0].material == 0 && f.faces[1].material == 1, "usemtl");
            Check(f.faces[0].smoothing == 1 && f.faces[1].smoothing == 0, "s 1, s off");
            Check(f.groups[static_cast<size_t>(f.faces[0].group)] == "Box/Lid" && f.groups[static_cast<size_t>(f.faces[2].group)] == "UCX_Box", "o and g");
            Check(Throws([] { ParseObj("v 0 0 0\nf 1 2 3\n"); }, "Line 2"), "an index past the vertices says the line");
            Check(Throws([] { ParseObj("v 0 0\n"); }, "Line 1"), "a short v says the line");
            Check(Throws([] { ParseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 0 2\n"); }), "index 0 is not an OBJ index");
        }

        // ---- axes and winding: a box from Blender (Y up), 1 OBJ unit = 100 Unreal units
        {
            const auto file = ParseObj(BoxObj(0, 0, 0, 1, 2, 3));
            Options o;
            o.scale = 100;
            o.up = Up::Y;
            o.collision = Collision::Polygons;
            const auto b = Build(file, o, "Box");
            Check(b.mesh.triangles.size() == 12 && b.mesh.hulls.empty(), "12 triangles, no hull for per-polygon");
            const auto box = Bounds(b.mesh);
            // Y up: OBJ (x, y, z) is Unreal (x, z, y).
            Check(Near(box[3], 100) && Near(box[4], 300) && Near(box[5], 200), "Y-up axes and scale");
            Check(Outward(b.mesh, {50, 150, 100}), "faces outward after the mirror");
            o.up = Up::Z;
            const auto z = Build(file, o, "Box");
            const auto zb = Bounds(z.mesh);
            Check(Near(zb[1], -200) && Near(zb[4], 0) && Near(zb[5], 300), "Z-up: Y mirrored");
            Check(Outward(z.mesh, {50, -100, 150}), "Z-up faces outward");
            // Z up, ASE positions are the OBJ's own, faces wound as the OBJ's.
            o.scale = 1;
            const auto plain = Build(file, o, "Box");
            const auto ase = ReadAse(Ase(plain.mesh), "Box");
            Check(ase.vertices.size() == 8 && ase.faces.size() == 12, "ASE vertices welded");
            for (const auto& v : ase.vertices)
            {
                bool found = false;
                for (const auto& p : file.positions) found = found || (Near(v[0], p[0]) && Near(v[1], p[1]) && Near(v[2], p[2]));
                Check(found, "ASE vertex is an OBJ vertex");
            }
            for (const auto& face : ase.faces)
            {
                // Right-handed Max axes: each written face faces out of the box's middle.
                const auto& a = ase.vertices[static_cast<size_t>(face[0])];
                const auto n = Cross(Sub(ase.vertices[static_cast<size_t>(face[1])], a), Sub(ase.vertices[static_cast<size_t>(face[2])], a));
                Check(Dot(n, Sub(a, {0.5, 1, 1.5})) > 0, "ASE faces wound as the OBJ's");
            }
        }

        // ---- UVs: V flipped once on the way in and once on the way out
        {
            const auto file = ParseObj("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0.25 0.75\nvt 1 0.75\nvt 0.25 1\nusemtl A\ns 3\nf 1/1 2/2 3/3\n");
            Options o;
            o.collision = Collision::None;
            const auto b = Build(file, o, "Tri");
            bool found = false;
            for (const auto& uv : b.mesh.triangles[0].uv) found = found || (Near(uv[0], 0.25) && Near(uv[1], 0.25));
            Check(found, "OBJ v 0.75 is 0.25 down the picture");
            const auto text = Ase(b.mesh);
            Check(Contains(text, "\t0.250000\t0.750000\t0.000000"), "ASE writes the OBJ's own UV back");
            Check(ReadAse(text, "Tri").smoothing[0] == 3, "smoothing group written");
            Check(Contains(text, "*NUMSUBMTLS 1") && Contains(text, "*MATERIAL_NAME \"A\"") && Contains(text, "*BITMAP \"A.tga\""), "one slot named A");
            // Without UVs: projected, and still written.
            const auto bare = Build(ParseObj("v 0 0 0\nv 300 0 0\nv 0 300 0\nf 1 2 3\n"), o, "Bare");
            Check(bare.mesh.slots.size() == 1 && bare.mesh.slots[0].empty() && bare.slotMaterials[0] == -1, "no usemtl: an unnamed slot");
        }

        // ---- triangulating a concave polygon
        {
            const std::vector<Vec3> l{{0, 0, 0}, {2, 0, 0}, {2, 1, 0}, {1, 1, 0}, {1, 2, 0}, {0, 2, 0}};
            const auto tris = Triangulate(l);
            Check(tris.size() == 4, "an L in four triangles");
            double area = 0;
            for (const auto& t : tris)
            {
                const auto n = Cross(Sub(l[static_cast<size_t>(t[1])], l[static_cast<size_t>(t[0])]), Sub(l[static_cast<size_t>(t[2])], l[static_cast<size_t>(t[0])]));
                Check(n[2] > 0, "each wound as the polygon");
                area += n[2] / 2;
            }
            Check(Near(area, 3), "the L's area, no triangle across its notch");
        }

        // ---- convex hulls
        {
            // A cube with points inside, on its faces and on its edges: 8 corners, 12 triangles.
            std::vector<Vec3> pts;
            for (int i = 0; i < 8; ++i) pts.push_back({(i & 1) ? 100.0 : 0.0, (i & 2) ? 100.0 : 0.0, (i & 4) ? 100.0 : 0.0});
            pts.push_back({50, 50, 50});
            pts.push_back({50, 50, 100});
            pts.push_back({0, 50, 0});
            pts.push_back({30, 0, 70});
            pts.push_back({100, 100, 100});
            auto h = ConvexHull(pts);
            Check(h.corners.size() == 8 && h.triangles.size() == 12, "cube hull: " + std::to_string(h.corners.size()) + " corners " + std::to_string(h.triangles.size()) + " triangles");
            Check(HullClean(h) && HullOutward(h), "cube hull clean and outward");
            // Face points a hair off the face (within tolerance) stay on it.
            pts.push_back({50, 50, 100 + 1e-4});
            pts.push_back({100 + 2e-4, 40, 60});
            h = ConvexHull(pts);
            Check(h.corners.size() == 8 && h.triangles.size() == 12, "points within tolerance of a face are on it");
            // A 16-sided prism: 32 corners, 60 triangles.
            std::vector<Vec3> prism;
            for (int i = 0; i < 16; ++i)
                for (double z : {0.0, 250.0})
                    prism.push_back({300 * std::cos(i * 3.14159265358979 / 8), 300 * std::sin(i * 3.14159265358979 / 8), z});
            h = ConvexHull(prism);
            Check(h.corners.size() == 32 && h.triangles.size() == 60 && HullClean(h), "prism hull 2V - 4");
            // A twisted slab far out (a wing): one corner lifted a little; clean either way.
            std::vector<Vec3> slab{{3000, -3600, 0}, {4000, -3600, 0}, {3000, 3600, 0}, {4000, 3600, 0},
                                   {3000, -3600, 20}, {4000, -3600, 20}, {3000, 3600, 20}, {4000, 3600, 20.03}};
            h = ConvexHull(slab);
            Check(HullClean(h) && h.triangles.size() == 2 * h.corners.size() - 4, "twisted slab clean: " + std::to_string(h.triangles.size()));
            // Random clouds: always clean, always holding every point.
            std::mt19937 random(7);
            std::uniform_real_distribution<double> spread(-500, 500);
            for (int round = 0; round < 30; ++round)
            {
                std::vector<Vec3> cloud;
                const int n = 8 + round * 6;
                for (int i = 0; i < n; ++i) cloud.push_back({spread(random), spread(random) * (round % 3 + 1), spread(random) + 2000 * (round % 2)});
                bool fallback = false;
                const auto c = ConvexHull(cloud);
                Check(HullClean(MakeHull(cloud, {0, 0, 1}, 2, &fallback)) && !fallback, "MakeHull clean too");
                std::string why;
                Check(HullClean(c, kHullTolerance, &why), "cloud hull clean: " + why);
                Check(!fallback, "cloud hull made without the box fallback");
                Check(HullOutward(c), "cloud hull outward");
                for (const auto& p : cloud)
                    for (const auto& t : c.triangles)
                    {
                        const auto& a = c.corners[static_cast<size_t>(t[0])];
                        const auto nn = Unit(Cross(Sub(c.corners[static_cast<size_t>(t[1])], a), Sub(c.corners[static_cast<size_t>(t[2])], a)));
                        Check(fallback || Dot(nn, Sub(p, a)) <= 1e-5 * 3000, "every point inside the hull");
                    }
            }
            // A big scan-like cloud: thousands of points near a rounded block, clean and quick.
            {
                std::vector<Vec3> scan;
                std::normal_distribution<double> noise(0, 0.02);
                for (int i = 0; i < 6000; ++i)
                {
                    Vec3 d{spread(random), spread(random), spread(random)};
                    d = Mul(Unit(d), 400);
                    for (auto& c : d) c = std::clamp(c, -300.0, 300.0) + noise(random);
                    scan.push_back(d);
                }
                bool fallback = false, simplified = false;
                const auto full = ConvexHull(scan);
                Check(HullClean(full) && full.corners.size() > kHullCorners, "scan cloud's full hull clean: " + std::to_string(full.corners.size()) + " corners");
                const auto c = MakeHull(scan, {0, 0, 1}, 2, &fallback, nullptr, &simplified);
                Check(HullClean(c) && !fallback && simplified && c.corners.size() <= kHullCorners, "scan cloud hull simplified: " + std::to_string(c.corners.size()) + " corners");
                // Simplified inward, by little: every corner kept is a corner of the full hull's points.
                double reach = 0;
                for (const auto& p : c.corners) reach = std::max(reach, Length(p));
                Check(reach > 400, "simplified hull still reaches the block's corners");
            }
            // Points on a sphere (all corners, no flat faces): clean.
            std::vector<Vec3> sphere;
            for (int i = 0; i < 12; ++i)
                for (int j = 1; j < 8; ++j)
                {
                    const double a = i * 3.14159265358979 / 6, b = j * 3.14159265358979 / 8;
                    sphere.push_back({200 * std::sin(b) * std::cos(a), 200 * std::sin(b) * std::sin(a), 200 * std::cos(b)});
                }
            sphere.push_back({0, 0, 200});
            sphere.push_back({0, 0, -200});
            h = MakeHull(sphere, {0, 0, 1}, 2);
            Check(HullClean(h) && h.corners.size() == sphere.size(), "sphere hull keeps every point: " + std::to_string(h.corners.size()));
            // Flat points: a floor is given thickness below it (its faces look up).
            const std::vector<Vec3> floor{{0, 0, 0}, {512, 0, 0}, {512, 512, 0}, {0, 512, 0}};
            bool flat = false;
            h = MakeHull(floor, {0, 0, 1}, 2, nullptr, &flat);
            Check(flat && h.corners.size() == 8 && HullClean(h), "a flat floor becomes a slab");
            double low = 1, high = -1;
            for (const auto& p : h.corners) { low = std::min(low, p[2]); high = std::max(high, p[2]); }
            Check(Near(low, -2) && Near(high, 0), "the slab is below the floor");
            Check(Throws([] { ConvexHull({{0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {3, 3, 3}}); }), "points on a line have no hull");
            // A hull that does not come out clean is not handed back as it is: HullClean refuses overlaps.
            Hull bad = BoxHull({0, 0, 0}, {1, 1, 1});
            bad.triangles.push_back(bad.triangles[0]);
            Check(!HullClean(bad), "an extra triangle is not clean");
            Check(HullClean(BoxHull({0, 0, 0}, {10, 20, 30})) && HullOutward(BoxHull({0, 0, 0}, {10, 20, 30})), "box hull clean");
        }

        // ---- collision modes
        {
            const auto two = ParseObj(BoxObj(0, 0, 0, 1, 1, 1, 0, "Pair") + BoxObj(3, 0, 0, 4, 1, 1, 8));
            Options o;
            o.scale = 100;
            o.collision = Collision::Parts;
            auto b = Build(two, o, "Pair");
            Check(b.mesh.hulls.size() == 2 && HullTriangleCount(b.mesh) == 24, "one hull per connected part");
            o.collision = Collision::Groups;
            b = Build(two, o, "Pair");
            Check(b.mesh.hulls.size() == 1 && b.mesh.hulls[0].corners.size() == 8, "one hull per object, over both boxes");
            o.collision = Collision::Box;
            b = Build(two, o, "Pair");
            Check(b.mesh.hulls.size() == 1 && b.mesh.hulls[0].triangles.size() == 12, "a box");
            o.collision = Collision::None;
            Check(Build(two, o, "Pair").mesh.hulls.empty(), "none: no hull");
            // UCX_ objects: hulls, not drawn.
            const auto withUcx = ParseObj(BoxObj(0, 0, 0, 1, 1, 1, 0, "Crate") + BoxObj(-0.1, -0.1, 0, 1.1, 1.1, 1.2, 8, "UCX_Crate_00"));
            o.collision = Collision::File;
            b = Build(withUcx, o, "Crate");
            Check(b.mesh.triangles.size() == 12 && b.mesh.hulls.size() == 1 && b.collisionObjects == 1, "UCX_ object becomes the hull");
            Check(Near(Bounds(b.mesh)[4], 120), "its hull is the UCX_ box (OBJ z is Unreal y)");
            o.collision = Collision::Parts;
            b = Build(withUcx, o, "Crate");
            Check(b.mesh.triangles.size() == 12 && b.mesh.hulls.size() == 1 && Near(Bounds(b.mesh)[4], 100), "UCX_ never drawn, even when not used");
            o.collision = Collision::File;
            b = Build(two, o, "Pair");
            Check(b.mesh.hulls.size() == 2 && !b.warnings.empty(), "no UCX_ objects: parts, with a warning");
            // A sign: flat, given thickness behind it.
            const auto sign = ParseObj("v 0 0 0\nv 1 0 0\nv 1 0 1\nv 0 0 1\nf 1 2 3 4\n");
            b = Build(sign, o, "Sign");
            Check(b.mesh.hulls.size() == 1 && b.flatParts == 1 && HullClean(b.mesh.hulls[0]), "a flat part gets a slab hull");
        }

        // ---- splitting
        {
            // A 100 x 60 grid of quads: 12000 triangles, three parts.
            std::ostringstream o;
            for (int y = 0; y <= 60; ++y)
                for (int x = 0; x <= 100; ++x) o << "v " << x << " 0 " << y << "\n";
            o << "usemtl Left\n";
            for (int y = 0; y < 60; ++y)
                for (int x = 0; x < 100; ++x)
                {
                    if (x == 50) o << "usemtl Right\n";
                    const int a = y * 101 + x + 1;
                    o << "f " << a << " " << a + 1 << " " << a + 102 << " " << a + 101 << "\n";
                    if (x == 99) o << "usemtl Left\n";
                }
            Options opt;
            opt.scale = 10;
            opt.collision = Collision::Polygons;
            const auto b = Build(ParseObj(o.str()), opt, "Wall");
            Check(b.mesh.triangles.size() == 12000 && !WithinLimits(b.mesh), "12000 triangles are over the limit");
            Check(PartsNeeded(b.mesh) == 3, "three parts needed");
            const auto parts = Split(b.mesh);
            Check(parts.size() == 3 && parts[0].name == "Wall" && parts[1].name == "Wall_2" && parts[2].name == "Wall_3", "Wall, Wall_2, Wall_3");
            size_t sum = 0;
            for (const auto& p : parts)
            {
                Check(p.triangles.size() <= kCollisionTriangles && !p.triangles.empty(), "each part within the limit");
                for (const auto& t : p.triangles) Check(t.slot >= 0 && t.slot < static_cast<int>(p.slots.size()), "slots remapped");
                sum += p.triangles.size();
            }
            Check(sum == 12000, "no triangle lost or doubled");
            Check(parts[0].slots.size() == 1 || parts[2].slots.size() == 1, "a part keeps only the materials it uses");
            // Parts are compact: cut along the long side.
            const auto first = Bounds(parts[0]);
            Check(first[3] - first[0] < 600, "parts cut along the wall's length");
            // Many hulls over few triangles: spread so each part keeps within the hull limit.
            Mesh hulled;
            hulled.name = "Rack";
            hulled.slots = {"A"};
            for (int i = 0; i < 600; ++i)
            {
                hulled.hulls.push_back(BoxHull({i * 10.0, 0, 0}, {i * 10.0 + 8, 8, 8}));
                Triangle t;
                t.p = {Vec3{i * 10.0, 0, 8}, Vec3{i * 10.0 + 8, 0, 8}, Vec3{i * 10.0, 8, 8}};
                hulled.triangles.push_back(t);
            }
            Check(HullTriangleCount(hulled) == 7200 && PartsNeeded(hulled) == 2, "7200 hull triangles need two parts");
            const auto racks = Split(hulled);
            Check(racks.size() >= 2, "split by hulls");
            size_t hullsSeen = 0;
            for (const auto& p : racks)
            {
                Check(HullTriangleCount(p) <= kCollisionTriangles && !p.triangles.empty(), "each part within the hull limit, with triangles");
                hullsSeen += p.hulls.size();
            }
            Check(hullsSeen == 600, "every hull kept once");
            // A mesh within the limits comes back as it is.
            Check(Split(racks[0]).size() == 1, "within the limits: one part");
            // A hull alone over the limit cannot be split.
            Mesh huge = hulled;
            huge.hulls = {Hull{}};
            huge.hulls[0].triangles.resize(6000);
            Check(Throws([&] { Split(huge); }, "One hull alone"), "a hull over the limit is refused");
            Check(Split(b.mesh, 4000).size() == 3, "a lower limit per part");
        }

        // ---- ASE hulls: turned half a turn about Z, named MCDCX_<mesh>_NN
        {
            Mesh m;
            m.name = "Crate";
            m.slots = {"Wood"};
            Triangle t;
            t.p = {Vec3{10, 5, 0}, Vec3{20, 5, 0}, Vec3{10, 6, 0}};
            m.triangles.push_back(t);
            m.hulls.push_back(BoxHull({10, 5, 0}, {20, 6, 3}));
            const auto text = Ase(m);
            Check(Contains(text, "*NODE_NAME \"MCDCX_Crate_00\"") && Count(text, "*GEOMOBJECT") == 2, "one MCDCX object per hull");
            const auto hull = ReadAse(text, "MCDCX_Crate_00");
            double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
            for (const auto& v : hull.vertices) { minX = std::min(minX, v[0]); maxX = std::max(maxX, v[0]); minY = std::min(minY, v[1]); maxY = std::max(maxY, v[1]); }
            // Unreal x 10..20 turned is -20..-10; Unreal y 5..6 turned is -6..-5, written (Max) as 5..6.
            Check(Near(minX, -20) && Near(maxX, -10) && Near(minY, 5) && Near(maxY, 6), "hull turned against the importer's turn");
            Check(hull.faces.size() == 12 && hull.vertices.size() == 8, "hull triangles 2V - 4 in the file");
            Check(Contains(text, "\r\n") && Count(text, "*MESH_FACE ") == 13, "CRLF lines, 13 faces");
            Check(Throws([] { Ase(Mesh{}); }), "a mesh needs triangles");
        }
        std::cout << "ObjImportModelTests: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ObjImportModelTests FAILED: " << e.what() << "\n";
        return 1;
    }
}
