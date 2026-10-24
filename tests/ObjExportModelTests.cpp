#include "../Reloaded.Editor/ObjExportModel.h"
#include <iostream>
#include <source_location>
#include <sstream>
#include <string>
using namespace Workflow::ObjExport;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}
template <class F> void Reject(F f, std::source_location where = std::source_location::current())
{
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    if (!caught) throw std::runtime_error("invalid input accepted at line " + std::to_string(where.line()));
}
bool Near(double a, double b, double e = 1e-6) { return std::abs(a - b) <= e; }
bool Near(const V3& a, const V3& b, double e = 1e-6) { return Near(a[0], b[0], e) && Near(a[1], b[1], e) && Near(a[2], b[2], e); }

std::vector<std::string> Lines(const std::string& text, const std::string& prefix)
{
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        if (line.rfind(prefix, 0) == 0) out.push_back(line);
    return out;
}
std::vector<V3> Vertices(const std::string& obj, const char* tag = "v ")
{
    std::vector<V3> out;
    for (const auto& line : Lines(obj, tag))
    {
        std::istringstream in(line.substr(2));
        V3 v{};
        in >> v[0] >> v[1] >> v[2];
        out.push_back(v);
    }
    return out;
}
// Face corners as (v, vt, vn) 1-based.
std::vector<std::vector<std::array<int, 3>>> Faces(const std::string& obj)
{
    std::vector<std::vector<std::array<int, 3>>> out;
    for (const auto& line : Lines(obj, "f "))
    {
        std::istringstream in(line.substr(2));
        std::vector<std::array<int, 3>> face;
        for (std::string corner; in >> corner;)
        {
            std::array<int, 3> c{0, 0, 0};
            sscanf_s(corner.c_str(), "%d/%d/%d", &c[0], &c[1], &c[2]);
            face.push_back(c);
        }
        out.push_back(face);
    }
    return out;
}

void Frames()
{
    Check(Near(ToFile({1, 2, 3}, 0.01), {0.01, -0.02, 0.03}), "Unreal Y is negated, Z stays up");
    auto a = RotationAxes(0, 16384, 0); // a quarter turn of yaw: X turns to Y
    Check(Near(a.x, {0, 1, 0}) && Near(a.y, {-1, 0, 0}) && Near(a.z, {0, 0, 1}), "yaw turns X towards Y");
    a = RotationAxes(16384, 0, 0); // pitch up: X turns to Z
    Check(Near(a.x, {0, 0, 1}), "pitch lifts X");
    Placement p;
    p.location = {100, 0, 0};
    p.prePivot = {10, 0, 0};
    p.scale = {2, 2, 2};
    p.rotation = {0, 16384, 0};
    Check(Near(Place({20, 0, 0}, p, RotationAxes(0, 16384, 0)), {100, 20, 0}), "PrePivot off, scaled, turned, moved");
    Check(!Mirrors(p), "a positive scale does not mirror");
    p.scale = {-1, 1, 1};
    Check(Mirrors(p), "one negative axis mirrors");
    p.scale = {-1, -1, 1};
    Check(!Mirrors(p), "two negative axes do not");
    p.scale = {2, 1, 1};
    p.rotation = {0, 0, 0};
    Check(Near(PlaceNormal(Normalized({1, 1, 0}), p, RotationAxes(0, 0, 0)), Normalized({0.5, 1, 0})), "normals divide by the scale");
    Check(Near(Normalized(Newell({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {0, 1, 0}})), {0, 0, 1}), "three corners in a line still give the normal");
}

void Names()
{
    Check(SafeName("My Pkg.Wall#1") == "My_Pkg.Wall_1", "spaces and signs become _");
    Check(SafeName("") == "_", "never empty");
    std::set<std::string> used;
    Check(UniqueName("Wall", used) == "Wall", "first");
    Check(UniqueName("wall", used) == "wall_2", "case folded");
    Check(UniqueName("Wall", used) == "Wall_3", "third");
}

Scene Square(bool flipCorners)
{
    // A floor square at Z = 0 facing up (+Z), 256 units wide.
    Scene s;
    s.points = {{0, 0, 0}, {256, 0, 0}, {256, 256, 0}, {0, 256, 0}};
    s.materials.push_back({"Floor", 0});
    s.textures.push_back({"textures/Floor.png", false});
    Polygon poly;
    poly.material = 0;
    poly.points = flipCorners ? std::vector<int>{3, 2, 1, 0} : std::vector<int>{0, 1, 2, 3};
    poly.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    if (flipCorners) std::reverse(poly.uvs.begin(), poly.uvs.end());
    poly.normal = {0, 0, 1};
    s.polygons.push_back(poly);
    return s;
}
// The normal by the right-hand rule of a written face, in the file's frame.
V3 FaceNormal(const std::string& obj, size_t face)
{
    auto v = Vertices(obj);
    std::vector<V3> corners;
    const auto faces = Faces(obj);
    for (auto& c : faces[face]) corners.push_back(v[c[0] - 1]);
    return Normalized(Newell(corners));
}

void Bsp()
{
    for (bool flip : {false, true})
    {
        std::ostringstream out;
        auto totals = WriteObj(out, Square(flip), 0.01, "Map.mtl", "test");
        const auto obj = out.str();
        Check(totals.faces == 1 && totals.objects == 1 && totals.vertices == 4, "one quad, four shared points");
        Check(Lines(obj, "o BSP").size() == 1 && Lines(obj, "mtllib Map.mtl").size() == 1 && Lines(obj, "usemtl Floor").size() == 1, "object, library, material");
        Check(Faces(obj)[0].size() == 4, "BSP polygons stay polygons");
        Check(Near(FaceNormal(obj, 0), {0, 0, 1}), flip ? "a polygon wound backwards still faces its plane" : "faces up in Z-up");
        const auto vs = Vertices(obj);
        Check(std::any_of(vs.begin(), vs.end(), [](const V3& x) { return Near(x, {2.56, 0, 0}); }), "metres at 1 unit = 1 cm");
        Check(Near(Vertices(obj, "vn")[0], {0, 0, 1}), "the plane normal is written");
        // vt is (u, 1 - v)
        auto vts = Lines(obj, "vt ");
        Check(vts.size() == 4 && (vts[0] == "vt 0 1" || vts[0] == "vt 0 0"), "texture coordinates flipped in v");
    }
    // A wall facing -Y in Unreal faces +Y in the file (Y negated).
    Scene s;
    s.points = {{0, 0, 0}, {0, 0, 100}, {100, 0, 100}, {100, 0, 0}};
    Polygon wall;
    wall.points = {0, 1, 2, 3};
    wall.normal = {0, -1, 0};
    s.polygons.push_back(wall);
    std::ostringstream out;
    WriteObj(out, s, 1, "m.mtl", "t");
    Check(Near(FaceNormal(out.str(), 0), {0, 1, 0}), "an Unreal -Y wall faces +Y in the file");
    Check(Lines(out.str(), "usemtl Default").size() == 1, "no material is Default");
    // Repeated corners and polygons too small are left out; the sky zone has its own object.
    Scene d;
    d.points = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}};
    Polygon degenerate;
    degenerate.points = {0, 1, 1, 0};
    d.polygons.push_back(degenerate);
    Polygon sky;
    sky.points = {0, 1, 1, 2, 0};
    sky.sky = true;
    d.polygons.push_back(sky);
    std::ostringstream o2;
    auto t2 = WriteObj(o2, d, 1, "m.mtl", "t");
    Check(t2.faces == 1 && Lines(o2.str(), "o BSP_SkyZone").size() == 1, "degenerate dropped, sky zone separate");
    Check(Faces(o2.str())[0].size() == 3, "repeats dropped from a corner list");
}

Scene OneTriangleMesh(Placement placement, std::uint32_t smoothing = 0)
{
    Scene s;
    Mesh m;
    m.positions = {{0, 0, 0}, {0, 100, 0}, {100, 0, 0}};
    m.uvs = {{0, 0}, {0, 1}, {1, 0}};
    Mesh::Triangle t;
    t.v = {0, 1, 2};
    t.t = {0, 1, 2};
    t.smoothing = smoothing;
    m.triangles.push_back(t);
    s.meshes.push_back(m);
    s.materials.push_back({"Crate", -1});
    Instance i;
    i.name = "StaticMeshActor0";
    i.mesh = 0;
    i.placement = placement;
    i.slots = {0};
    s.instances.push_back(i);
    return s;
}

void Meshes()
{
    // Unreal draws (0,0,0) (0,100,0) (100,0,0) facing +Z (clockwise from above in its left-handed frame).
    Placement p;
    std::ostringstream out;
    auto totals = WriteObj(out, OneTriangleMesh(p), 0.01, "m.mtl", "t");
    auto obj = out.str();
    Check(totals.faces == 1 && totals.objects == 1 && Lines(obj, "o StaticMeshActor0").size() == 1, "one object per actor");
    Check(Near(FaceNormal(obj, 0), {0, 0, 1}), "a mesh triangle faces up in the file");
    Check(Near(Vertices(obj, "vn")[0], {0, 0, 1}), "its normal agrees");
    Check(Lines(obj, "usemtl Crate").size() == 1 && Lines(obj, "s off").size() == 1, "material and flat shading");

    // Moved, turned and scaled.
    p.location = {1000, 0, 0};
    p.scale = {2, 2, 2};
    p.rotation = {0, 16384, 0};
    std::ostringstream o2;
    WriteObj(o2, OneTriangleMesh(p), 1, "m.mtl", "t");
    auto v = Vertices(o2.str());
    Check(Near(v[2], {1000, -200, 0}), "(100,0,0) scaled 2 and turned 90 lands at Unreal (1000,200,0)");

    // Mirrored: still faces outward.
    p = {};
    p.scale = {-1, 1, 1};
    std::ostringstream o3;
    WriteObj(o3, OneTriangleMesh(p), 1, "m.mtl", "t");
    Check(Near(FaceNormal(o3.str(), 0), {0, 0, 1}), "a mirrored mesh is turned back the right way out");
    Check(Near(Vertices(o3.str(), "vn")[0], {0, 0, 1}), "and its normal agrees");

    // Smoothing: two triangles of a ridge share a normal across the ridge when they share a group.
    Mesh ridge;
    ridge.positions = {{0, 0, 0}, {0, 100, 0}, {100, 50, 50}, {-100, 50, 50}};
    Mesh::Triangle a, b;
    a.v = {0, 1, 2};
    b.v = {0, 3, 1};
    a.smoothing = b.smoothing = 1;
    ridge.triangles = {a, b};
    auto n = SmoothNormals(ridge);
    Check(Near(n[0], n[3]) && Near(n[0], {0, 0, 1}, 1e-9), "a shared corner of one group averages its faces");
    ridge.triangles[1].smoothing = 2;
    n = SmoothNormals(ridge);
    Check(!Near(n[0], n[3]), "different groups keep a hard edge");

    // Smoothed faces say s 1; material slots fall back to the first.
    auto s = OneTriangleMesh({}, 4);
    s.meshes[0].triangles[0].slot = 3;
    std::ostringstream o4;
    WriteObj(o4, s, 1, "m.mtl", "t");
    Check(Lines(o4.str(), "s 1").size() == 1 && Lines(o4.str(), "usemtl Crate").size() == 1, "smooth group and slot fallback");
}

void Mtl()
{
    Scene s;
    s.textures = {{"textures/A.png", false}, {"textures/B.png", true}, {"", false}};
    s.materials = {{"A", 0}, {"B", 1}, {"C", 2}, {"D", -1}};
    std::ostringstream out;
    WriteMtl(out, s, "t");
    const auto mtl = out.str();
    Check(Lines(mtl, "newmtl ").size() == 5, "Default plus four");
    Check(Lines(mtl, "map_Kd ").size() == 2 && Lines(mtl, "map_d textures/B.png").size() == 1, "alpha only for the alpha texture");
    Check(Lines(mtl, "Kd 0.6 0.6 0.6").size() == 3, "grey without a written texture");
}

void Dxt()
{
    // DXT1 block: c0 = pure red (0xF800), c1 = pure blue (0x001F); indices 0,1,2,3 across the first row.
    std::uint8_t dxt1[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0, 0, 0};
    auto img = Decode(kDXT1, 4, 4, dxt1, 8);
    Check(img.rgba[0] == 255 && img.rgba[2] == 0 && img.rgba[3] == 255, "index 0 is c0");
    Check(img.rgba[4] == 0 && img.rgba[6] == 255, "index 1 is c1");
    Check(img.rgba[8] == 170 && img.rgba[10] == 85, "index 2 is two thirds c0");
    Check(img.rgba[12] == 85 && img.rgba[14] == 170, "index 3 is two thirds c1");
    Check(img.rgba[16] == 255 && img.rgba[18] == 0, "row two is all index 0");
    // c0 <= c1: three colours and transparent black.
    std::uint8_t punch[8] = {0x1F, 0x00, 0x00, 0xF8, 0xC0, 0, 0, 0};
    img = Decode(kDXT1, 4, 4, punch, 8);
    Check(img.rgba[12] == 0 && img.rgba[15] == 0, "index 3 is transparent in three-colour DXT1");
    // DXT5: alpha endpoints 255 and 0, first pixel index 1 (= 0), rest 0 (= 255).
    std::uint8_t dxt5[16] = {255, 0, 1, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    img = Decode(kDXT5, 4, 4, dxt5, 16);
    Check(img.rgba[3] == 0 && img.rgba[7] == 255 && img.rgba[0] == 255, "DXT5 alpha indices");
    // DXT3: explicit 4-bit alpha.
    std::uint8_t dxt3[16] = {0x0F, 0xF0, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    img = Decode(kDXT3, 4, 4, dxt3, 16);
    Check(img.rgba[3] == 255 && img.rgba[7] == 0 && img.rgba[11] == 0 && img.rgba[15] == 255, "DXT3 nibbles, low first");
    // A 2 x 2 DXT texture still takes a whole block.
    Check(MipBytes(kDXT1, 2, 2) == 8 && MipBytes(kDXT5, 8, 4) == 32 && MipBytes(kRGBA8, 2, 2) == 16, "block sizes");
    img = Decode(kDXT1, 2, 2, dxt1, 8);
    Check(img.rgba.size() == 16 && img.rgba[4] == 0 && img.rgba[6] == 255, "a texture smaller than a block");
    Reject([] { std::uint8_t b[4]{}; Decode(kDXT1, 4, 4, b, 4); });
    Reject([] { std::uint8_t b[64]{}; Decode(2, 4, 4, b, 64); });
    Reject([] { std::uint8_t b[64]{}; Decode(kRGBA8, 0, 4, b, 64); });
}

void Uncompressed()
{
    std::uint8_t bgra[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    auto img = Decode(kRGBA8, 2, 1, bgra, 8);
    Check(img.rgba[0] == 3 && img.rgba[1] == 2 && img.rgba[2] == 1 && img.rgba[3] == 4, "BGRA to RGBA");
    std::uint8_t p8[2] = {1, 7};
    std::vector<std::array<std::uint8_t, 4>> palette{{0, 0, 0, 255}, {10, 20, 30, 128}};
    img = Decode(kP8, 2, 1, p8, 2, palette);
    Check(img.rgba[0] == 30 && img.rgba[1] == 20 && img.rgba[2] == 10 && img.rgba[3] == 128, "P8 palette is B, G, R, A");
    Check(img.rgba[4] == 0 && img.rgba[7] == 255, "an index past the palette is black");
    std::uint8_t l8[1] = {99};
    img = Decode(kL8, 1, 1, l8, 1);
    Check(img.rgba[0] == 99 && img.rgba[2] == 99 && img.rgba[3] == 255, "L8 grey");
}

// Reads back a PNG made with stored blocks: header, chunk CRCs, and the
// pixels after undoing each row's filter.
Image ReadStoredPng(const std::string& png, bool& alpha)
{
    Check(png.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0, "PNG signature");
    size_t at = 8;
    Image img;
    std::string idat;
    int channels = 0;
    auto be = [&](size_t p) { return (std::uint32_t(std::uint8_t(png[p])) << 24) | (std::uint32_t(std::uint8_t(png[p + 1])) << 16) | (std::uint32_t(std::uint8_t(png[p + 2])) << 8) | std::uint8_t(png[p + 3]); };
    while (at < png.size())
    {
        const auto n = be(at);
        const std::string type = png.substr(at + 4, 4);
        const auto crc = Crc32(reinterpret_cast<const std::uint8_t*>(png.data() + at + 4), n + 4);
        Check(crc == be(at + 8 + n), "chunk CRC");
        if (type == "IHDR")
        {
            img.width = static_cast<int>(be(at + 8));
            img.height = static_cast<int>(be(at + 12));
            channels = png[at + 17] == 6 ? 4 : 3;
            alpha = channels == 4;
        }
        if (type == "IDAT") idat += png.substr(at + 8, n);
        at += 12 + n;
    }
    // zlib: 2-byte header, stored blocks, adler.
    std::string raw;
    size_t p = 2;
    for (;;)
    {
        const bool last = idat[p] & 1;
        const size_t len = std::uint8_t(idat[p + 1]) | (std::uint8_t(idat[p + 2]) << 8);
        raw += idat.substr(p + 5, len);
        p += 5 + len;
        if (last) break;
    }
    const auto adler = Adler32(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size());
    Check(adler == ((std::uint32_t(std::uint8_t(idat[p])) << 24) | (std::uint32_t(std::uint8_t(idat[p + 1])) << 16) | (std::uint32_t(std::uint8_t(idat[p + 2])) << 8) | std::uint8_t(idat[p + 3])), "adler");
    const size_t stride = static_cast<size_t>(img.width) * channels;
    std::vector<std::uint8_t> prev(stride, 0), row(stride);
    img.rgba.assign(static_cast<size_t>(img.width) * img.height * 4, 255);
    for (int y = 0; y < img.height; ++y)
    {
        const int filter = std::uint8_t(raw[y * (stride + 1)]);
        for (size_t i = 0; i < stride; ++i)
        {
            const int x = std::uint8_t(raw[y * (stride + 1) + 1 + i]);
            const int a = i >= static_cast<size_t>(channels) ? row[i - channels] : 0, b = prev[i], c = i >= static_cast<size_t>(channels) ? prev[i - channels] : 0;
            int pr = 0;
            if (filter == 1) pr = a;
            else if (filter == 2) pr = b;
            else if (filter == 3) pr = (a + b) / 2;
            else if (filter == 4) { const int q = a + b - c, pa = std::abs(q - a), pb = std::abs(q - b), pc = std::abs(q - c); pr = pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
            row[i] = static_cast<std::uint8_t>(x + pr);
        }
        for (int x = 0; x < img.width; ++x)
            for (int c = 0; c < channels; ++c) img.rgba[(static_cast<size_t>(y) * img.width + x) * 4 + c] = row[static_cast<size_t>(x) * channels + c];
        prev = row;
    }
    return img;
}

void PngRoundTrip()
{
    Image img;
    img.width = 37;
    img.height = 23;
    img.rgba.resize(static_cast<size_t>(img.width) * img.height * 4);
    for (size_t i = 0; i < img.rgba.size(); ++i) img.rgba[i] = static_cast<std::uint8_t>((i * 7 + (i / 148) * 13) & 0xFF);
    for (bool alpha : {true, false})
    {
        bool readAlpha = false;
        auto back = ReadStoredPng(Png(img, alpha), readAlpha);
        Check(readAlpha == alpha && back.width == 37 && back.height == 23, "PNG header");
        bool same = true;
        for (size_t i = 0; i < img.rgba.size(); ++i)
            if ((i % 4 != 3 || alpha) && back.rgba[i] != img.rgba[i]) same = false;
        Check(same, alpha ? "RGBA pixels come back" : "RGB pixels come back");
    }
    // Big enough for several stored blocks.
    Image big;
    big.width = 300;
    big.height = 300;
    big.rgba.assign(300 * 300 * 4, 0);
    for (size_t i = 0; i < big.rgba.size(); ++i) big.rgba[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 24);
    bool a = false;
    auto back = ReadStoredPng(Png(big, true), a);
    Check(back.rgba == big.rgba, "multi-block stream");
    Check(Crc32(reinterpret_cast<const std::uint8_t*>("123456789"), 9) == 0xCBF43926u, "CRC-32 check value");
    Reject([] { Image bad; bad.width = 2; bad.height = 2; Png(bad, false); });
}

int main()
{
    try
    {
        Frames();
        Names();
        Bsp();
        Meshes();
        Mtl();
        Dxt();
        Uncompressed();
        PngRoundTrip();
        std::cout << "ObjExportModelTests: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "ObjExportModelTests FAILED: " << e.what() << "\n";
        return 1;
    }
}
