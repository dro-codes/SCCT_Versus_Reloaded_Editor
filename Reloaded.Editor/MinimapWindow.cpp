#include "pch.h"
#undef min
#undef max
#include "MinimapWindow.h"
#include "MinimapModel.h"
#include "MapDesignModel.h"
#include "WorkflowEditor.h"
#include "logger.h"
#include <windows.h>
#include <commdlg.h>
#include <objidl.h> // IStream and PROPID for gdiplus.h, which the lean Windows header leaves out.
#include <gdiplus.h>
#include <zlib.h>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

namespace MinimapWindow
{
namespace
{
using Workflow::Json;
namespace Editor = Workflow::Editor;
namespace fs = std::filesystem;
using Minimap::Bytes;

HWND window = nullptr;
ULONG_PTR gdiplusToken = 0;

// The map as last read, so changing an option only redraws.
struct Read
{
    unsigned generation = ~0u, revision = ~0u;
    Minimap::Scene scene;
    std::vector<Minimap::Marker> markers;
    std::vector<double> storeys;  // floor heights, lowest first
} cache;

// The floors in use: found in the map, or as typed into the window.
std::vector<double> floors;
unsigned filledGeneration = ~0u; // the map the floor choices were filled for

// The picture on show and how it was framed.
Bytes pixels;
int pixelSize = 0;
Minimap::Camera shownCamera;
std::string lastFolder;

void EnsureGdiplus()
{
    if (gdiplusToken) return;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr) != Gdiplus::Ok) throw std::runtime_error("GDI+ is not available.");
}

HWND Control(const char* type, const char* text, DWORD style, int id, int x, int y, int width, int height)
{
    auto control = CreateWindowExA(std::string(type) == "EDIT" ? WS_EX_CLIENTEDGE : 0, type, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandle(nullptr), nullptr);
    SendMessage(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}
void Status(const std::string& text) { if (window) SetDlgItemTextA(window, kStatus, text.c_str()); }
bool Checked(int id) { return IsDlgButtonChecked(window, id) == BST_CHECKED; }
int Selection(int id) { return static_cast<int>(SendDlgItemMessageA(window, id, CB_GETCURSEL, 0, 0)); }
std::string Text(int id)
{
    char buffer[512]{};
    GetDlgItemTextA(window, id, buffer, sizeof buffer);
    std::string text(buffer);
    const auto first = text.find_first_not_of(" \t"), last = text.find_last_not_of(" \t");
    return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
}

// --- Reading the map -------------------------------------------------------

// The storeys, as the Storeys palette and the Map Design plan find them: the
// surfaces brushes leave to stand on. A map made of meshes has few of those,
// so the built floors are asked when the brushes give none.
std::vector<double> Storeys(const Minimap::Scene& scene)
{
    namespace Design = Workflow::Design;
    std::vector<Design::BrushBox> brushes;
    Workflow::Vector low{ 1e18, 1e18, 1e18 }, high{ -1e18, -1e18, -1e18 };
    for (const auto& actor : Editor::DesignActorSpans())
    {
        const int csg = actor.value("csg", 0);
        if ((csg != 1 && csg != 2) || !actor.value("box", false) || actor.value("portal", false)) continue;
        Design::BrushBox brush;
        brush.carve = csg == 2;
        brush.low = actor.at("low").get<Workflow::Vector>();
        brush.high = actor.at("high").get<Workflow::Vector>();
        for (int axis = 0; axis < 3; ++axis) { low[axis] = std::min(low[axis], brush.low[axis]); high[axis] = std::max(high[axis], brush.high[axis]); }
        brushes.push_back(brush);
    }
    std::vector<Design::FloorEvidence> evidence;
    for (const auto& brush : brushes)
    {
        if (Design::EnclosesMap(brush, low, high)) continue;
        if (Design::FloorEvidence surface; Design::FloorSurface(brush, surface)) evidence.push_back(surface);
    }
    if (evidence.empty())
        for (const auto& face : scene.faces)
        {
            if (Minimap::Classify(face, Minimap::Band{}) != Minimap::Kind::Floor) continue;
            double area = 0;
            for (size_t i = 0; i < face.points.size(); ++i)
            {
                const auto& a = face.points[i];
                const auto& b = face.points[(i + 1) % face.points.size()];
                area += a[0] * b[1] - b[0] * a[1];
            }
            const double z = Minimap::MeanZ(face.points);
            evidence.push_back({ z, z + 128, std::sqrt(std::abs(area) / 2) });
        }
    // Coarser than the Storeys palette: a map page per landing or mezzanine
    // is no use in game, where the floor follows the player's height.
    Design::StoreyRules rules;
    rules.tolerance = Minimap::kFloorTolerance;
    rules.share = Minimap::kFloorShare;
    rules.limit = Minimap::kFloorLimit;
    std::vector<double> bases;
    for (const auto& storey : Design::Storeys(evidence, rules)) bases.push_back(storey.base);
    std::sort(bases.begin(), bases.end());
    return bases;
}

// Whether a reference list (exported object references such as
// SComputerObjectiveTrigger'MyLevel.Terminal1') names the actor at `path`.
bool Lists(const Json& references, const std::string& path)
{
    const auto folded = Workflow::Fold("'" + path + "'");
    for (const auto& entry : references)
        if (entry.is_string() && Workflow::Fold(entry.get<std::string>()).find(folded) != std::string::npos) return true;
    return false;
}

std::vector<Minimap::Marker> Markers()
{
    const auto actors = Editor::ObjectiveActors();
    // The objectives in mission order, then any no mission lists.
    std::vector<const Json*> objectives;
    for (const auto& mission : actors)
        if (mission.value("kind", std::string()) == "Mission")
            for (const auto& objective : actors)
                if (objective.value("kind", std::string()) == "Objective" && mission.contains("objectives") && Lists(mission.at("objectives"), objective.at("path").get<std::string>())
                    && std::find(objectives.begin(), objectives.end(), &objective) == objectives.end())
                    objectives.push_back(&objective);
    for (const auto& objective : actors)
        if (objective.value("kind", std::string()) == "Objective" && std::find(objectives.begin(), objectives.end(), &objective) == objectives.end()) objectives.push_back(&objective);
    std::vector<Minimap::Marker> markers;
    for (const auto& actor : actors)
    {
        const auto kind = actor.value("kind", std::string());
        Minimap::Marker m;
        if (kind == "Computer terminal" || kind == "Bomb target" || kind == "Objective trigger") m.kind = Minimap::MarkerKind::Objective;
        else if (kind == "Flag") m.kind = Minimap::MarkerKind::Flag;
        else if (kind == "Drop zone") m.kind = Minimap::MarkerKind::DropZone;
        else if (kind == "Player start") m.kind = actor.value("team", std::string()) == "1" ? Minimap::MarkerKind::MercSpawn : Minimap::MarkerKind::SpySpawn;
        else continue;
        const auto p = actor.at("position").get<Workflow::Vector>();
        m.position = { p[0], p[1], p[2] };
        m.yaw = actor.at("rotation").at(1).get<int>();
        if (m.kind == Minimap::MarkerKind::Objective)
            for (size_t i = 0; i < objectives.size(); ++i)
                if (objectives[i]->contains("triggers") && Lists(objectives[i]->at("triggers"), actor.at("path").get<std::string>())) { m.objective = static_cast<int>(i); break; }
        markers.push_back(m);
    }
    Minimap::Number(markers);
    return markers;
}

// The floors a map starts with: those its in-game map already has, else
// the ones found in its geometry.
std::vector<double> InitialFloors(const Read& map)
{
    std::vector<double> z;
    for (const auto& floor : map.scene.floors) z.push_back(floor.first);
    return z.empty() ? map.storeys : Minimap::ParseFloorList(Minimap::FloorList(z));
}

// The map as it stands now, leaving the window's copy alone.
Read ReadMap()
{
    Read fresh;
    fresh.scene = Editor::MinimapScene();
    fresh.markers = Markers();
    fresh.storeys = Storeys(fresh.scene);
    fresh.generation = Editor::MapGeneration();
    fresh.revision = Editor::ContentRevision();
    return fresh;
}

const Read& Map(bool reread)
{
    const unsigned generation = Editor::MapGeneration(), revision = Editor::ContentRevision();
    if (reread || generation != cache.generation || revision != cache.revision)
    {
        Read fresh = ReadMap();
        // Another map starts from its own floors (those its in-game map
        // already has, else the ones found); the same map keeps the heights
        // typed in for it.
        if (generation != cache.generation) floors = InitialFloors(fresh);
        cache = std::move(fresh);
    }
    return cache;
}

// --- Options ---------------------------------------------------------------

struct Options
{
    Minimap::Band band;
    int size = Minimap::kGameSize;
    size_t palette = 0;
    bool objectives = true, spawns = false, meshes = true;
    int framing = 0; // around the gameplay, around everything built, the map's own camera
};

double ParseZ(const std::string& text, double fallback)
{
    if (text.empty()) return fallback;
    size_t used = 0;
    double value = 0;
    try { value = std::stod(text, &used); } catch (const std::exception&) { used = std::string::npos; }
    if (used != text.size() || !std::isfinite(value)) throw std::runtime_error("The height range takes numbers (or nothing for no limit).");
    return value;
}

Options Current()
{
    Options o;
    o.band.lower = ParseZ(Text(kFrom), -std::numeric_limits<double>::infinity());
    o.band.upper = ParseZ(Text(kTo), std::numeric_limits<double>::infinity());
    if (!(o.band.lower < o.band.upper)) throw std::runtime_error("The top of the height range must be above its bottom.");
    static const int sizes[] = { 256, 512, 1024 };
    o.size = sizes[std::clamp(Selection(kSize), 0, 2)];
    o.palette = static_cast<size_t>(std::clamp(Selection(kStyle), 0, static_cast<int>(Minimap::Palettes().size()) - 1));
    o.objectives = Checked(kObjectives);
    o.spawns = Checked(kSpawns);
    o.meshes = Checked(kMeshes);
    o.framing = std::max(0, Selection(kFraming));
    return o;
}

// The camera every picture of this map shares: the one the map already has
// when asked to keep it, else the square around the whole map.
Minimap::Camera CameraFor(const Read& map, const Options& o)
{
    if (o.framing == 2 && Minimap::Usable(map.scene.camera) && Minimap::TopDown(map.scene.camera)) return map.scene.camera;
    std::vector<Minimap::Vec3> points;
    for (const auto& m : map.markers) points.push_back(m.position);
    const auto& meshes = o.meshes ? map.scene.meshes : std::vector<Minimap::MeshBox>{};
    return Minimap::CameraFor(Minimap::Square(o.framing == 1 ? Minimap::Extent(map.scene.faces, meshes, points) : Minimap::GameplayExtent(map.scene.faces, meshes, points), 0.06));
}

// --- Drawing ---------------------------------------------------------------

Gdiplus::Color Colour(const Minimap::Rgba& c) { return Gdiplus::Color(c.a, c.r, c.g, c.b); }

Bytes Render(const Read& map, const Options& o, const Minimap::Camera& camera)
{
    using namespace Gdiplus;
    EnsureGdiplus();
    const int size = o.size;
    const auto& palette = Minimap::Palettes()[o.palette];
    Bitmap target(size, size, PixelFormat32bppARGB);
    if (target.GetLastStatus() != Ok) throw std::runtime_error("Cannot make a picture that size.");
    {
        Graphics g(&target);
        g.Clear(Colour(palette.background));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        const float scale = size / 256.0f;
        auto point = [&](const Minimap::Vec3& p) { const auto px = Minimap::ToPixel(camera, p, size); return PointF(static_cast<REAL>(px.first), static_cast<REAL>(px.second)); };
        auto outline = [&](const std::vector<Minimap::Vec3>& points) { std::vector<PointF> out; for (const auto& p : points) out.push_back(point(p)); return out; };
        auto footprint = [&](const Minimap::MeshBox& m)
        {
            std::vector<PointF> out;
            for (const auto& [x, y] : Minimap::Footprint(m.corners)) out.push_back(point({ x, y, 0 }));
            return out;
        };
        SolidBrush silhouette(Colour(palette.silhouette)), floor(Colour(palette.floor)), obstacle(Colour(palette.obstacle));
        // The fills overlap their neighbours by a hair so no seam shows between faces.
        Pen silhouetteEdge(Colour(palette.silhouette), 1.0f), floorEdge(Colour(palette.floor), 1.0f);
        Pen wall(Colour(palette.wall), std::max(1.0f, 1.1f * scale)), obstacleEdge(Colour(palette.wall), std::max(0.75f, 0.6f * scale));
        silhouetteEdge.SetLineJoin(LineJoinRound); floorEdge.SetLineJoin(LineJoinRound);
        wall.SetStartCap(LineCapRound); wall.SetEndCap(LineCapRound);

        std::vector<const Minimap::Polygon*> floors, walls;
        const auto frame = Minimap::FrameOf(camera);
        for (const auto& face : map.scene.faces)
        {
            const auto kind = Minimap::Classify(face, o.band);
            if (kind == Minimap::Kind::Skip || Minimap::Backdrop(face, frame)) continue;
            if (kind == Minimap::Kind::Silhouette)
            {
                const auto shape = outline(face.points);
                g.FillPolygon(&silhouette, shape.data(), static_cast<INT>(shape.size()));
                g.DrawPolygon(&silhouetteEdge, shape.data(), static_cast<INT>(shape.size()));
            }
            else if (kind == Minimap::Kind::Floor) floors.push_back(&face);
            else if (kind == Minimap::Kind::Wall) walls.push_back(&face);
        }
        std::vector<const Minimap::MeshBox*> meshFloors, meshObstacles;
        if (o.meshes)
            for (const auto& mesh : map.scene.meshes)
            {
                const auto kind = Minimap::Classify(mesh, o.band);
                if (kind == Minimap::Kind::Silhouette)
                {
                    const auto shape = footprint(mesh);
                    if (shape.size() >= 3) g.FillPolygon(&silhouette, shape.data(), static_cast<INT>(shape.size()));
                }
                else if (kind == Minimap::Kind::Floor) meshFloors.push_back(&mesh);
                else if (kind == Minimap::Kind::Obstacle) meshObstacles.push_back(&mesh);
            }
        for (const auto* face : floors)
        {
            const auto shape = outline(face->points);
            g.FillPolygon(&floor, shape.data(), static_cast<INT>(shape.size()));
            g.DrawPolygon(&floorEdge, shape.data(), static_cast<INT>(shape.size()));
        }
        for (const auto* mesh : meshFloors)
        {
            const auto shape = footprint(*mesh);
            if (shape.size() >= 3) g.FillPolygon(&floor, shape.data(), static_cast<INT>(shape.size()));
        }
        for (const auto* mesh : meshObstacles)
        {
            const auto shape = footprint(*mesh);
            if (shape.size() < 3) continue;
            g.FillPolygon(&obstacle, shape.data(), static_cast<INT>(shape.size()));
            g.DrawPolygon(&obstacleEdge, shape.data(), static_cast<INT>(shape.size()));
        }
        // An upright face seen from above is a line along its foot.
        for (const auto* face : walls)
        {
            const auto shape = outline(face->points);
            g.DrawPolygon(&wall, shape.data(), static_cast<INT>(shape.size()));
        }

        // Markers: numbered discs for objectives, flags and drop zones, and an
        // arrow per spawn pointing the way it faces.
        FontFamily family(L"Arial");
        Font font(&family, std::max(7.0f, 9.5f * scale), FontStyleBold, UnitPixel);
        StringFormat centred;
        centred.SetAlignment(StringAlignmentCenter);
        centred.SetLineAlignment(StringAlignmentCenter);
        SolidBrush text(Colour(palette.text)), mark(Colour(palette.objective)), spy(Colour(palette.spy)), merc(Colour(palette.merc));
        Pen ring(Colour(palette.silhouette), std::max(1.0f, 1.2f * scale));
        for (const auto& m : map.markers)
        {
            if (!Minimap::Shows(o.band, m)) continue;
            const bool spawn = m.kind == Minimap::MarkerKind::SpySpawn || m.kind == Minimap::MarkerKind::MercSpawn;
            if (spawn ? !o.spawns : !o.objectives) continue;
            const auto at = point(m.position);
            if (spawn)
            {
                const auto [dx, dy] = Minimap::Heading(m.yaw);
                const REAL r = 4.5f * scale;
                const PointF arrow[3] = { PointF(at.X + static_cast<REAL>(dx) * r * 1.4f, at.Y + static_cast<REAL>(dy) * r * 1.4f),
                                          PointF(at.X + static_cast<REAL>(-dx * 0.8 - dy * 0.9) * r, at.Y + static_cast<REAL>(-dy * 0.8 + dx * 0.9) * r),
                                          PointF(at.X + static_cast<REAL>(-dx * 0.8 + dy * 0.9) * r, at.Y + static_cast<REAL>(-dy * 0.8 - dx * 0.9) * r) };
                g.FillPolygon(m.kind == Minimap::MarkerKind::SpySpawn ? &spy : &merc, arrow, 3);
                g.DrawPolygon(&ring, arrow, 3);
                continue;
            }
            const REAL r = (m.kind == Minimap::MarkerKind::Objective ? 7.0f : 5.0f) * scale;
            if (m.kind == Minimap::MarkerKind::DropZone)
            {
                g.FillRectangle(&mark, at.X - r, at.Y - r, 2 * r, 2 * r);
                g.DrawRectangle(&ring, at.X - r, at.Y - r, 2 * r, 2 * r);
            }
            else if (m.kind == Minimap::MarkerKind::Flag)
            {
                const PointF diamond[4] = { PointF(at.X, at.Y - r * 1.3f), PointF(at.X + r, at.Y), PointF(at.X, at.Y + r * 1.3f), PointF(at.X - r, at.Y) };
                g.FillPolygon(&mark, diamond, 4);
                g.DrawPolygon(&ring, diamond, 4);
            }
            else
            {
                g.FillEllipse(&mark, at.X - r, at.Y - r, 2 * r, 2 * r);
                g.DrawEllipse(&ring, at.X - r, at.Y - r, 2 * r, 2 * r);
                const std::wstring label(m.label.begin(), m.label.end());
                SolidBrush ink(Colour(palette.silhouette));
                g.DrawString(label.c_str(), -1, &font, RectF(at.X - 2 * r, at.Y - 2 * r + 0.5f * scale, 4 * r, 4 * r), &centred, &ink);
            }
        }
    }
    BitmapData data{};
    Rect rect(0, 0, size, size);
    if (target.LockBits(&rect, ImageLockModeRead, PixelFormat32bppARGB, &data) != Ok) throw std::runtime_error("The picture could not be read back.");
    Bytes rgba(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y)
    {
        const auto* row = static_cast<const std::uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
        for (int x = 0; x < size; ++x)
        {
            auto* out = &rgba[(static_cast<size_t>(y) * size + x) * 4];
            out[0] = row[x * 4 + 2]; out[1] = row[x * 4 + 1]; out[2] = row[x * 4]; out[3] = row[x * 4 + 3];
        }
    }
    target.UnlockBits(&data);
    return rgba;
}

// --- Files -----------------------------------------------------------------

Bytes ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot read " + path.string());
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
void WriteFile(const fs::path& path, const Bytes& bytes)
{
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("Cannot write " + path.string());
}
Bytes Unpack(const Bytes& file)
{
    const auto container = Snapshot::Describe(file);
    if (!container.compressed) return file;
    Bytes out(container.uncompressedSize);
    uLongf length = container.uncompressedSize;
    if (uncompress(out.data(), &length, file.data() + container.dataOffset, container.compressedSize) != Z_OK || length != container.uncompressedSize)
        throw std::runtime_error("The .utc package did not inflate.");
    return out;
}
std::string Stamp()
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    char text[32];
    std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &local);
    return text;
}
std::string Quote(const fs::path& path) { return "\"" + path.string() + "\""; }
std::string MapStem() { return fs::path(Editor::MapFile()).stem().string(); }

bool SavePng(const Bytes& rgba, int size, const fs::path& file)
{
    using namespace Gdiplus;
    EnsureGdiplus();
    UINT count = 0, bytes = 0;
    if (GetImageEncodersSize(&count, &bytes) != Ok || !bytes) return false;
    std::vector<std::uint8_t> buffer(bytes);
    auto* encoders = reinterpret_cast<ImageCodecInfo*>(buffer.data());
    if (GetImageEncoders(count, bytes, encoders) != Ok) return false;
    const CLSID* png = nullptr;
    for (UINT i = 0; i < count; ++i) if (encoders[i].MimeType && wcscmp(encoders[i].MimeType, L"image/png") == 0) png = &encoders[i].Clsid;
    if (!png) return false;
    Bitmap bitmap(size, size, PixelFormat32bppARGB);
    BitmapData data{};
    Rect rect(0, 0, size, size);
    if (bitmap.LockBits(&rect, ImageLockModeWrite, PixelFormat32bppARGB, &data) != Ok) return false;
    for (int y = 0; y < size; ++y)
    {
        auto* row = static_cast<std::uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
        for (int x = 0; x < size; ++x)
        {
            const auto* in = &rgba[(static_cast<size_t>(y) * size + x) * 4];
            row[x * 4] = in[2]; row[x * 4 + 1] = in[1]; row[x * 4 + 2] = in[0]; row[x * 4 + 3] = in[3];
        }
    }
    bitmap.UnlockBits(&data);
    return bitmap.Save(file.c_str(), png, nullptr) == Ok;
}

// --- Actions ---------------------------------------------------------------

void FillFloors();
void ShowBand();

void Redraw(bool reread = false)
{
    const auto& map = Map(reread);
    // Another map was opened while the window stayed up.
    if (window && map.generation != filledGeneration) { FillFloors(); ShowBand(); }
    const auto options = Current();
    shownCamera = CameraFor(map, options);
    pixels = Render(map, options, shownCamera);
    pixelSize = options.size;
    if (window) InvalidateRect(GetDlgItem(window, kPreview), nullptr, FALSE);
    size_t floors = 0, walls = 0;
    for (const auto& face : map.scene.faces)
    {
        const auto kind = Minimap::Classify(face, options.band);
        floors += kind == Minimap::Kind::Floor;
        walls += kind == Minimap::Kind::Wall;
    }
    const auto frame = Minimap::FrameOf(shownCamera);
    Status(std::to_string(map.scene.faces.size()) + " built faces (" + std::to_string(floors) + " floor, " + std::to_string(walls) + " wall on this range), "
         + std::to_string(map.scene.meshes.size()) + " meshes, " + std::to_string(map.markers.size()) + " markers. Picture covers "
         + Minimap::Number(frame.half * 2) + " units around (" + Minimap::Number(frame.cx) + ", " + Minimap::Number(frame.cy) + ").");
}

void FillFloors()
{
    const auto& map = Map(false);
    filledGeneration = map.generation;
    const int previous = Selection(kFloor);
    SendDlgItemMessageA(window, kFloor, CB_RESETCONTENT, 0, 0);
    SendDlgItemMessageA(window, kFloor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Whole map"));
    SetDlgItemTextA(window, kFloorList, Minimap::FloorList(floors).c_str());
    for (size_t i = 0; i < floors.size(); ++i)
    {
        const auto label = "Floor " + std::to_string(i + 1) + " (Z " + Workflow::Design::Round(floors[i]) + ")";
        SendDlgItemMessageA(window, kFloor, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    const int count = static_cast<int>(floors.size()) + 1;
    SendDlgItemMessageA(window, kFloor, CB_SETCURSEL, previous > 0 && previous < count ? previous : (count > 1 ? 1 : 0), 0);
    // The map's own camera is offered only when it has a usable one.
    const bool canKeep = Minimap::Usable(map.scene.camera) && Minimap::TopDown(map.scene.camera);
    const int framing = Selection(kFraming);
    SendDlgItemMessageA(window, kFraming, CB_RESETCONTENT, 0, 0);
    const char* choices[] = { "Around spawns and objectives", "Around everything built", "The map's current minimap" };
    for (int i = 0; i < (canKeep ? 3 : 2); ++i) SendDlgItemMessageA(window, kFraming, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choices[i]));
    SendDlgItemMessageA(window, kFraming, CB_SETCURSEL, framing >= 0 && (framing < 2 || canKeep) ? framing : (canKeep ? 2 : 0), 0);
}

// The chosen floor's band into the height boxes.
void ShowBand()
{
    const int floor = Selection(kFloor);
    if (floor <= 0 || floor > static_cast<int>(floors.size()))
    {
        SetDlgItemTextA(window, kFrom, "");
        SetDlgItemTextA(window, kTo, "");
        return;
    }
    const auto band = Minimap::Bands(floors)[static_cast<size_t>(floor - 1)];
    SetDlgItemTextA(window, kFrom, Minimap::Number(band.lower).c_str());
    SetDlgItemTextA(window, kTo, std::isinf(band.upper) ? "" : Minimap::Number(band.upper).c_str());
}

// The typed floor list and height range: a changed list redraws the floor
// choices (and that floor's band), else the heights as typed are drawn.
void ApplyTyped()
{
    auto typed = Minimap::ParseFloorList(Text(kFloorList));
    if (typed != floors)
    {
        floors = std::move(typed);
        FillFloors();
        ShowBand();
    }
    Redraw();
}

void SaveImage()
{
    if (pixels.empty()) Redraw();
    char file[MAX_PATH] = "";
    const auto name = MapStem().empty() ? std::string("Minimap") : MapStem() + "-minimap";
    strncpy_s(file, name.c_str(), _TRUNCATE);
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = window;
    dialog.lpstrFilter = "PNG image (*.png)\0*.png\0Targa image (*.tga)\0*.tga\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = sizeof file;
    dialog.lpstrDefExt = "png";
    dialog.lpstrInitialDir = lastFolder.empty() ? nullptr : lastFolder.c_str();
    dialog.lpstrTitle = "Save the minimap";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameA(&dialog)) return;
    const fs::path path(file);
    lastFolder = path.parent_path().string();
    std::string extension = path.extension().string();
    for (auto& c : extension) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (extension == ".tga") WriteFile(path, Minimap::Tga32(pixelSize, pixelSize, pixels));
    else if (!SavePng(pixels, pixelSize, path)) throw std::runtime_error("Cannot write " + path.string());
    Status("Saved " + path.string() + " (" + std::to_string(pixelSize) + " x " + std::to_string(pixelSize) + ").");
}

// Every floor's picture with the shared camera, imported into the map package
// and set as the map's in-game map. Returns ApplyMinimap's result.
Json ApplyInGame(const Read& map, Options options, const std::vector<double>& floorBases)
{
    options.size = Minimap::kGameSize; // What the game's map page is laid out for.
    const auto camera = CameraFor(map, options);
    auto bands = Minimap::Bands(floorBases);
    if (bands.empty()) bands.push_back(Minimap::Band{});
    const auto heights = Minimap::FloorHeights(bands);
    const auto stem = MapStem().empty() ? std::string("Untitled") : MapStem();
    std::vector<std::pair<double, fs::path>> images;
    for (size_t i = 0; i < bands.size(); ++i)
    {
        options.band = bands[i];
        const auto file = Editor::Directory() / "minimaps" / (stem + "-floor" + std::to_string(i) + ".tga");
        WriteFile(file, Minimap::Tga32(options.size, options.size, Render(map, options, camera)));
        images.emplace_back(heights[i], file);
    }
    auto applied = Editor::ApplyMinimap(camera, images);
    Logger::log("Minimap: set " + std::to_string(applied.at("floors").size()) + " floor(s) for " + stem);
    cache.revision = ~0u; // The LevelInfo changed; read it again for the next picture.
    return applied;
}

void SetInGameMap()
{
    const auto applied = ApplyInGame(Map(false), Current(), floors);
    const auto& set = applied.at("floors");
    const auto& unused = applied.at("unused");
    std::string text = "The in-game map now has " + std::to_string(set.size()) + " floor(s) (" + set.front().at("texture").get<std::string>()
                     + (set.size() > 1 ? " and up" : "") + "). Save the map to keep it; Undo puts the old settings back.";
    if (!unused.empty())
        text += " Unused from an earlier run: " + unused.front().get<std::string>() + (unused.size() > 1 ? " and up" : "") + " (delete in the Texture Browser).";
    Status(text);
}

// The picture as the Briefing texture of <Map>-i: backed up, loaded, imported,
// saved by the stock route and checked, as Level Snapshot does for Menu.
struct Briefing { fs::path utc; Snapshot::Texture texture; bool backedUp = false; };
Briefing WriteBriefingPicture(const Read& map, Options options)
{
    const auto stem = MapStem();
    const auto package = Snapshot::PackageName(stem);
    options.size = Minimap::kGameSize;
    const auto image = Render(map, options, CameraFor(map, options));
    const fs::path textures = Editor::Directory().parent_path().parent_path() / "Packages" / "Textures";
    const fs::path utc = textures / (package + ".utc"), utx = textures / (package + ".utx");
    const fs::path tga = Editor::Directory() / "minimaps" / (stem + "-briefing.tga");
    WriteFile(tga, Minimap::Tga32(options.size, options.size, image));
    std::error_code error;
    fs::create_directories(textures, error);

    size_t previousExports = 0;
    std::vector<std::pair<fs::path, fs::path>> backups;
    const fs::path backupDirectory = Editor::Directory() / "snapshot-backups";
    const std::string stamp = Stamp();
    for (const auto& file : { utc, utx })
    {
        if (!fs::exists(file, error)) continue;
        fs::create_directories(backupDirectory, error);
        const fs::path backup = backupDirectory / (file.stem().string() + "." + stamp + file.extension().string());
        if (!fs::copy_file(file, backup, fs::copy_options::overwrite_existing, error)) throw std::runtime_error("Cannot back up " + file.string() + " to " + backup.string());
        backups.emplace_back(file, backup);
        try { previousExports = std::max(previousExports, Snapshot::Parse(Unpack(ReadFile(file))).exports.size()); }
        catch (const std::exception& e) { Logger::log("Minimap: could not read " + file.string() + ": " + e.what()); }
    }
    // Loaded as the stock map open loads it (LOADMAPPROP): from the file
    // only when it is not in memory yet. OBJ LOAD would read a loaded
    // package back from the file, losing settings changed since.
    Editor::Exec("LOADMAPPROP MAP=\"" + stem + "\"");
    if (!Editor::Exec("TEXTURE IMPORT FILE=" + Quote(tga) + " NAME=\"" + Minimap::kBriefingTexture + "\" PACKAGE=\"" + package + "\" MIPS=0 MASKED=0 ALPHATEXTURE=1"))
        throw std::runtime_error("The editor's texture importer refused the picture.");
    // DXT5 with its alpha, as the stock Briefings are (and the other pictures Level Snapshot writes).
    Editor::Exec("TEXTURE COMPRESS NAME=" + package + "." + Minimap::kBriefingTexture + " FORMAT=DXT5");
    const auto started = fs::file_time_type::clock::now();
    Snapshot::Texture written;
    try
    {
        // With a map settings object the stock map save carries it into the
        // package, then saves the .utc; without one, save the package as it is.
        auto landed = [&] { return fs::exists(utc, error) && fs::last_write_time(utc, error) >= started; };
        // The stock save asks before writing a package smaller than the file
        // it replaces, which a recompressed picture often is. The file is
        // backed up and the result is checked below, so it goes first and the
        // save does not stop for the question.
        if (!backups.empty()) fs::remove(utc, error);
        bool saved = Editor::Exec("SAVEMAPPROP MAP=\"" + stem + "\"") && landed();
        if (!saved) saved = Editor::Exec("OBJ SAVEPACKAGE PACKAGE=\"" + package + "\" FILE=" + Quote(utc));
        if (!saved || !landed()) throw std::runtime_error("The editor did not write " + utc.string());
        const Bytes packageBytes = Unpack(ReadFile(utc));
        written = Minimap::VerifyTexture(packageBytes, Minimap::kBriefingTexture, options.size, previousExports);
        if (written.format != Snapshot::kFormatDxt5)
            throw std::runtime_error("The Briefing texture was saved in format " + std::to_string(written.format) + ", not DXT5.");
        if (fs::exists(utx, error)) WriteFile(utx, packageBytes);
    }
    catch (const std::exception&)
    {
        for (const auto& [file, backup] : backups) fs::copy_file(backup, file, fs::copy_options::overwrite_existing, error);
        throw;
    }
    Logger::log("Minimap: wrote the Briefing texture of " + utc.string());
    return { utc, written, !backups.empty() };
}

void WriteBriefing()
{
    const auto written = WriteBriefingPicture(Map(false), Current());
    Status("Saved as the Briefing texture of " + written.utc.filename().string() + " (" + std::to_string(written.texture.width) + " x " + std::to_string(written.texture.height)
         + "), the plan the lobby shows. " + (written.backedUp ? "The previous package is in ReloadedEditor\\snapshot-backups." : std::string("")));
}

// --- Window ----------------------------------------------------------------

void PaintPreview(HDC dc, const RECT& rect)
{
    using namespace Gdiplus;
    EnsureGdiplus();
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    Bitmap buffer(std::max(1, width), std::max(1, height), PixelFormat32bppARGB);
    {
        Graphics g(&buffer);
        // A chequer behind, so what is transparent in the game shows as such.
        SolidBrush light(Color(255, 205, 205, 205)), dark(Color(255, 170, 170, 170));
        for (int y = 0; y < height; y += 8)
            for (int x = 0; x < width; x += 8) g.FillRectangle(((x + y) / 8) % 2 ? &dark : &light, x, y, 8, 8);
        if (!pixels.empty())
        {
            Bitmap picture(pixelSize, pixelSize, PixelFormat32bppARGB);
            BitmapData data{};
            Rect all(0, 0, pixelSize, pixelSize);
            if (picture.LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &data) == Ok)
            {
                for (int y = 0; y < pixelSize; ++y)
                {
                    auto* row = static_cast<std::uint8_t*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
                    for (int x = 0; x < pixelSize; ++x)
                    {
                        const auto* in = &pixels[(static_cast<size_t>(y) * pixelSize + x) * 4];
                        row[x * 4] = in[2]; row[x * 4 + 1] = in[1]; row[x * 4 + 2] = in[0]; row[x * 4 + 3] = in[3];
                    }
                }
                picture.UnlockBits(&data);
                g.SetInterpolationMode(pixelSize >= width ? InterpolationModeHighQualityBicubic : InterpolationModeNearestNeighbor);
                g.SetPixelOffsetMode(PixelOffsetModeHalf);
                g.DrawImage(&picture, Rect(0, 0, width, height), 0, 0, pixelSize, pixelSize, UnitPixel);
            }
        }
    }
    Graphics target(dc);
    target.DrawImage(&buffer, static_cast<INT>(rect.left), static_cast<INT>(rect.top));
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT)
    {
        PAINTSTRUCT ps{};
        const HDC dc = BeginPaint(hwnd, &ps);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        try { PaintPreview(dc, rect); } catch (const std::exception&) {}
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcA(hwnd, message, w, l);
}

void Run(void (*action)())
{
    try { action(); }
    catch (const std::exception& e) { Status(e.what()); Logger::log(std::string("Minimap: ") + e.what()); }
}

LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_COMMAND)
    {
        const int id = LOWORD(w), code = HIWORD(w);
        if (id == kFloor && code == CBN_SELCHANGE) { Run([] { ShowBand(); Redraw(); }); return 0; }
        if ((id == kSize || id == kStyle) && code == CBN_SELCHANGE) { Run([] { Redraw(); }); return 0; }
        if (id == kFraming && code == CBN_SELCHANGE) { Run([] { Redraw(); }); return 0; }
        if ((id == kObjectives || id == kSpawns || id == kMeshes) && code == BN_CLICKED) { Run([] { Redraw(); }); return 0; }
        if (id == kApplyRange && code == BN_CLICKED) { Run(ApplyTyped); return 0; }
        if (id == kRefresh && code == BN_CLICKED) { Run([] { floors = InitialFloors(Map(true)); FillFloors(); ShowBand(); Redraw(); }); return 0; }
        if (id == kSaveImage && code == BN_CLICKED) { Run(SaveImage); return 0; }
        if (id == kSetInGame && code == BN_CLICKED) { Run(SetInGameMap); return 0; }
        if (id == kWriteBriefing && code == BN_CLICKED) { Run(WriteBriefing); return 0; }
    }
    if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (message == WM_NCDESTROY) { window = nullptr; pixels.clear(); cache = Read{}; filledGeneration = ~0u; }
    return DefWindowProcA(hwnd, message, w, l);
}

void Build()
{
    constexpr int preview = 512, left = 12 + preview + 14, column = 240, field = left + 72, fieldWidth = column - 72;
    auto label = [&](const char* text, int y) { Control("STATIC", text, SS_LEFT, -1, left, y + 3, 70, 16); };
    Control("ReloadedMinimapPreview", "", WS_BORDER, kPreview, 12, 12, preview, preview);
    label("Floors at Z:", 12);
    Control("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, kFloorList, field, 12, fieldWidth, 20);
    label("Show:", 38);
    Control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kFloor, field, 38, fieldWidth, 300);
    label("Heights:", 64);
    Control("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, kFrom, field, 64, 62, 20);
    Control("STATIC", "to", SS_CENTER, -1, field + 64, 67, 20, 16);
    Control("EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, kTo, field + 86, 64, fieldWidth - 86, 20);
    Control("BUTTON", "&Apply floors and heights", BS_PUSHBUTTON | WS_TABSTOP, kApplyRange, field, 88, fieldWidth, 22);
    label("Size:", 120);
    Control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, kSize, field, 120, fieldWidth, 200);
    for (const char* size : { "256 x 256 (the game's)", "512 x 512", "1024 x 1024" }) SendDlgItemMessageA(window, kSize, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(size));
    SendDlgItemMessageA(window, kSize, CB_SETCURSEL, 0, 0);
    label("Style:", 146);
    Control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, kStyle, field, 146, fieldWidth, 200);
    for (const auto& palette : Minimap::Palettes()) SendDlgItemMessageA(window, kStyle, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(palette.name));
    SendDlgItemMessageA(window, kStyle, CB_SETCURSEL, 0, 0);
    label("Framing:", 172);
    Control("COMBOBOX", "", CBS_DROPDOWNLIST | WS_TABSTOP, kFraming, field, 172, fieldWidth, 200);
    Control("BUTTON", "Objectives, flags and drop zones", BS_AUTOCHECKBOX | WS_TABSTOP, kObjectives, left, 202, column, 18);
    Control("BUTTON", "Spawns", BS_AUTOCHECKBOX | WS_TABSTOP, kSpawns, left, 222, column, 18);
    Control("BUTTON", "Static meshes", BS_AUTOCHECKBOX | WS_TABSTOP, kMeshes, left, 242, column, 18);
    CheckDlgButton(window, kObjectives, BST_CHECKED);
    CheckDlgButton(window, kMeshes, BST_CHECKED);
    Control("BUTTON", "&Refresh from the map", BS_PUSHBUTTON | WS_TABSTOP, kRefresh, left, 268, column, 24);
    Control("BUTTON", "&Set as the in-game map (every floor)", BS_PUSHBUTTON | WS_TABSTOP, kSetInGame, left, 304, column, 24);
    Control("BUTTON", "Write &Briefing to the interface package", BS_PUSHBUTTON | WS_TABSTOP, kWriteBriefing, left, 332, column, 24);
    Control("BUTTON", "Save &Image (PNG or TGA)...", BS_PUSHBUTTON | WS_TABSTOP, kSaveImage, left, 360, column, 24);
    Control("STATIC", "In game the map page shows the floor the player stands on. Briefing is the plan the lobby shows. "
                      "Both are 256 x 256.", SS_LEFT, -1, left, 392, column, 44);
    Control("STATIC", "", SS_LEFT, kStatus, left, 438, column, 86);
}
}

void Open(HWND owner)
{
    EnsureGdiplus();
    if (window)
    {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
        Run([] { Map(false); FillFloors(); Redraw(); });
        return;
    }
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSA wc{};
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpfnWndProc = Proc;
        wc.lpszClassName = "ReloadedMinimap";
        RegisterClassA(&wc);
        WNDCLASSA preview{};
        preview.hInstance = wc.hInstance;
        preview.hCursor = wc.hCursor;
        preview.lpfnWndProc = PreviewProc;
        preview.lpszClassName = "ReloadedMinimapPreview";
        RegisterClassA(&preview);
        registered = true;
    }
    RECT rc{ 0, 0, 12 + 512 + 14 + 240 + 12, 12 + 512 + 12 };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_TOOLWINDOW);
    window = CreateWindowExA(WS_EX_TOOLWINDOW, "ReloadedMinimap", "Generate Minimap", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, owner, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!window) throw std::runtime_error("Cannot open the minimap window.");
    Build();
    Run([] { Map(true); FillFloors(); ShowBand(); Redraw(); });
}

Json Apply(const Json& request)
{
    const Json given = request.is_object() ? request : Json::object();
    const auto map = ReadMap();
    if (map.scene.faces.empty()) throw std::runtime_error("The map has no built geometry to draw. Build it first (build.all or Rebuild Geometry), then set the minimap.");
    Options options;
    options.objectives = given.value("objectives", true);
    options.spawns = given.value("spawns", false);
    options.meshes = given.value("meshes", true);
    const auto framing = given.value("framing", std::string("gameplay"));
    if (framing == "gameplay") options.framing = 0;
    else if (framing == "everything") options.framing = 1;
    else if (framing == "current")
    {
        if (!Minimap::Usable(map.scene.camera) || !Minimap::TopDown(map.scene.camera))
            throw std::runtime_error("The map has no top-down minimap camera to keep; use framing \"gameplay\" or \"everything\".");
        options.framing = 2;
    }
    else throw std::runtime_error("framing is \"gameplay\", \"everything\" or \"current\".");
    const auto& palettes = Minimap::Palettes();
    if (given.contains("style"))
    {
        const auto& style = given.at("style");
        if (style.is_number_integer())
        {
            const auto index = style.get<int>();
            if (index < 0 || index >= static_cast<int>(palettes.size())) throw std::runtime_error("style is a palette index from 0 to " + std::to_string(palettes.size() - 1) + ".");
            options.palette = static_cast<size_t>(index);
        }
        else
        {
            const auto name = Workflow::Fold(style.get<std::string>());
            size_t found = palettes.size();
            for (size_t i = 0; i < palettes.size(); ++i) if (Workflow::Fold(palettes[i].name) == name) found = i;
            if (found == palettes.size())
            {
                std::string names;
                for (const auto& palette : palettes) names += (names.empty() ? "" : ", ") + std::string(palette.name);
                throw std::runtime_error("Unknown minimap style; choose one of: " + names + ".");
            }
            options.palette = found;
        }
    }
    std::vector<double> bases;
    if (given.contains("floors"))
    {
        for (const auto& z : given.at("floors"))
        {
            const double value = z.get<double>();
            if (!std::isfinite(value) || std::abs(value) > 1000000) throw std::runtime_error("Floor heights are numbers inside the world.");
            bases.push_back(value);
        }
        // The same limits as the window's floor list.
        std::sort(bases.begin(), bases.end());
        bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
        if (bases.size() > 32) throw std::runtime_error("A minimap takes at most 32 floors.");
    }
    else bases = InitialFloors(map);
    auto result = ApplyInGame(map, options, bases);
    const auto camera = CameraFor(map, options);
    const auto frame = Minimap::FrameOf(camera);
    result["camera"] = {{"centre", {frame.cx, frame.cy}}, {"size", frame.half * 2}};
    if (given.value("briefing", false))
    {
        auto briefing = options;
        const int floor = given.value("briefingFloor", 0);
        const auto bands = Minimap::Bands(bases);
        if (floor < 0 || floor > static_cast<int>(bands.size())) throw std::runtime_error("briefingFloor is 1 to " + std::to_string(bands.size()) + ", or 0 for the whole map.");
        briefing.band = floor ? bands[static_cast<size_t>(floor - 1)] : Minimap::Band{};
        const auto written = WriteBriefingPicture(map, briefing);
        result["briefing"] = {{"file", written.utc.string()}, {"width", written.texture.width}, {"height", written.texture.height}, {"backedUp", written.backedUp}};
    }
    if (window) Run([] { Redraw(true); });
    return result;
}

bool HandleCommand(UINT command)
{
    if (command != Command) return false;
    try { Open(GetActiveWindow()); }
    catch (const std::exception& e) { Logger::log(std::string("Minimap: ") + e.what()); if (window) Status(e.what()); }
    return true;
}
}
