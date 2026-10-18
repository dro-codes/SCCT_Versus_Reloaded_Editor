#pragma once
// Drawn shapes as plan elements, for the importers whose sources are vector
// drawings (SVG, Tiled objects, GeoJSON): an outline, a line or a point in
// world units, with a kind and a few attributes, becomes a room, a solid, a
// doorway, a marker, a laser or a geometric event in a Grid::Plan, which
// Grid::ToSpec then writes as a level spec. Engine-free.
//
// Kinds (any spelling Grid::Detail::KindOf knows):
//   spaces   room, corridor, vent, crawlway (carved), platform, stairs, ramp
//            (rectangles become pieces; other outlines become convex brushes)
//   solid    wall, pillar, column, obstacle: an added (CSG_Add) prism
//   door     a doorway through the wall the outline or line crosses
//   markers  spy, merc, terminal, flag, dropzone, bomb, disk, camera, camnet,
//            laser, motion, presence, mine, alarm, light, ammo, infirmary,
//            glass, extinguisher, projector, sprinkler, puddle, balltrap,
//            sound, mesh (with mesh), actor (with class)
//   lines    laser (a beam), ledge, hoh, pipe, ladder, zipline, fence, pole
// A marker drawn as a line stands at the line's first point facing along it.
#include "../GridPlanModel.h"

namespace Mcp::Import::Shapes
{
    using Grid::P2;
    struct Attributes
    {
        std::string kind, id, name, mesh, cls, sound;
        bool explicitKind = false;
        std::optional<double> yaw, h, z, height, radius, brightness, h0, h1;
        bool portal = false, portalGiven = false;
        Json props = Json::object();
    };
    inline bool SpaceKind(const std::string& kind) { return kind == "Room" || kind == "Corridor" || kind == "Vent" || kind == "Crawlway"; }
    inline bool PieceKind(const std::string& kind) { return SpaceKind(kind) || kind == "Platform" || kind == "Stairs" || kind == "Ramp"; }
    inline bool LineKind(const std::string& kind) { return kind == "laser" || Grid::Detail::GeKinds().count(kind) != 0; }
    inline bool MarkerKind(const std::string& kind)
    {
        static const std::set<std::string> kinds{"spy", "merc", "terminal", "flag", "dropzone", "bomb", "disk", "camera", "camnet", "laser", "motion", "presence", "mine",
                                                 "alarm", "light", "ammo", "infirmary", "glass", "extinguisher", "projector", "sprinkler", "puddle", "balltrap", "sound",
                                                 "mesh", "actor"};
        return kinds.count(kind) != 0;
    }
    // The kind word in an id, label or name: "room-3" -> Room, "spyStart2" -> spy.
    inline std::string KindWord(const std::string& text)
    {
        std::string word;
        for (unsigned char c : text)
        {
            if (std::isalpha(c)) word += static_cast<char>(c);
            else if (c == ' ' && word.empty()) continue;
            else if (!word.empty()) break;
        }
        return Grid::Detail::KindOf(word);
    }
    // A storey number in a layer's name: "storey 1", "Floor-2", "level_0".
    inline std::optional<int> StoreyNumber(const std::string& text)
    {
        static const std::regex pattern(R"((?:storey|story|floor|level)[\s_-]*(-?\d+))", std::regex::icase);
        std::smatch m;
        if (std::regex_search(text, m, pattern)) return std::stoi(m[1].str());
        return std::nullopt;
    }
    inline double DefaultHeight(const std::string& kind, const Grid::Options& o)
    {
        if (kind == "Vent") return o.ventHeight;
        if (kind == "Crawlway") return std::min(o.ventHeight, 104.0);
        if (kind == "Platform") return 64;
        if (kind == "Stairs" || kind == "Ramp") return o.storeyHeight;
        return o.roomHeight;
    }
    inline P2 Centroid(const std::vector<P2>& points)
    {
        const auto clean = Grid::Clean(points);
        const double area = Grid::SignedArea(clean);
        if (clean.size() >= 3 && std::abs(area) > 1e-6)
        {
            double cx = 0, cy = 0;
            for (size_t i = 0; i < clean.size(); ++i)
            {
                const auto& p = clean[i];
                const auto& q = clean[(i + 1) % clean.size()];
                const double f = p[0] * q[1] - q[0] * p[1];
                cx += (p[0] + q[0]) * f;
                cy += (p[1] + q[1]) * f;
            }
            return {cx / (6 * area), cy / (6 * area)};
        }
        double x = 0, y = 0;
        for (const auto& p : points) { x += p[0]; y += p[1]; }
        return points.empty() ? P2{0, 0} : P2{x / static_cast<double>(points.size()), y / static_cast<double>(points.size())};
    }
    inline void AddPoint(Grid::Plan& plan, const P2& p, const Attributes& a, const std::string& storey, Json& report)
    {
        if (!MarkerKind(a.kind))
        {
            Warn(report, a.kind.empty() ? "Points without a kind were left out; give them one (spy, merc, terminal, camera, light, ...)."
                                        : "A " + a.kind + " needs an outline, not a point, so it was left out.");
            return;
        }
        Grid::Marker m;
        m.kind = a.kind; m.id = a.id; m.name = a.name; m.storey = storey; m.x = p[0]; m.y = p[1];
        m.yaw = a.yaw; m.h = a.h; m.radius = a.radius; m.brightness = a.brightness;
        if (a.z) m.z = plan.StoreyZ(storey) + *a.z;
        m.mesh = a.mesh; m.cls = a.cls; m.sound = a.sound; m.props = a.props;
        plan.markers.push_back(m);
    }
    inline void AddDoor(Grid::Plan& plan, const P2& centre, double width, double depth, bool alongX, const Attributes& a, const std::string& storey)
    {
        Grid::Door d;
        d.id = plan.ids.Make(a.id, "Door");
        d.name = a.name; d.storey = storey; d.x = centre[0]; d.y = centre[1]; d.width = width; d.depth = depth; d.alongX = alongX;
        if (a.portalGiven) d.portal = a.portal;
        plan.doors.push_back(d);
    }
    inline void AddOpen(Grid::Plan& plan, const Grid::Options& o, const std::vector<P2>& line, const Attributes& a, const std::string& storey, Json& report);
    // A closed outline.
    inline void AddClosed(Grid::Plan& plan, const Grid::Options& o, const std::vector<P2>& outline, Attributes a, const std::string& storey, Json& report)
    {
        if (a.kind.empty()) a.kind = "Room";
        const auto clean = Grid::Clean(outline);
        if (clean.size() < 3) { Warn(report, "Outlines with no area were left out."); return; }
        const double z = plan.StoreyZ(storey) + (a.z ? *a.z : 0.0);
        if (PieceKind(a.kind) || a.kind == "solid")
        {
            const double height = a.height ? *a.height : DefaultHeight(a.kind, o);
            const auto rect = Grid::AxisRectangle(clean);
            if (rect && a.kind != "solid")
            {
                Grid::Space s;
                s.kind = a.kind; s.explicitKind = a.explicitKind; s.name = a.name; s.storey = storey;
                s.x0 = (*rect)[0]; s.y0 = (*rect)[1]; s.x1 = (*rect)[2]; s.y1 = (*rect)[3];
                s.z = z; s.height = height; s.material = o.material;
                Grid::Shape(s, o);
                s.id = plan.ids.Make(a.id, s.kind);
                plan.spaces.push_back(s);
                return;
            }
            std::vector<std::vector<P2>> parts;
            try { parts = Grid::ConvexParts(clean); }
            catch (const std::exception& e) { Warn(report, std::string(e.what()) + (a.id.empty() ? std::string() : " (" + a.id + ")")); return; }
            if (!SpaceKind(a.kind) && a.kind != "solid")
                Warn(report, "A " + a.kind + " that is not a rectangle became a solid prism; draw stairs, ramps and platforms as rectangles to get Map Design pieces.");
            for (size_t i = 0; i < parts.size(); ++i)
            {
                Grid::Prism prism;
                prism.id = plan.ids.Make(a.id.empty() ? std::string() : (i ? a.id + "_" + std::to_string(i + 1) : a.id), SpaceKind(a.kind) ? a.kind : std::string("Solid"));
                prism.csg = SpaceKind(a.kind) ? "CSG_Subtract" : "CSG_Add";
                prism.storey = storey; prism.name = a.name; prism.points = parts[i]; prism.z = z; prism.height = height; prism.material = o.material;
                plan.prisms.push_back(prism);
            }
            return;
        }
        if (a.kind == "door")
        {
            double x0 = clean[0][0], x1 = x0, y0 = clean[0][1], y1 = y0;
            for (const auto& p : clean) { x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]); y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]); }
            const bool alongX = x1 - x0 >= y1 - y0;
            AddDoor(plan, {(x0 + x1) / 2, (y0 + y1) / 2}, alongX ? x1 - x0 : y1 - y0, alongX ? y1 - y0 : x1 - x0, alongX, a, storey);
            return;
        }
        if (LineKind(a.kind) && !MarkerKind(a.kind))
        {
            // A geometric event drawn as a thin box: its long middle line.
            double x0 = clean[0][0], x1 = x0, y0 = clean[0][1], y1 = y0;
            for (const auto& p : clean) { x0 = std::min(x0, p[0]); x1 = std::max(x1, p[0]); y0 = std::min(y0, p[1]); y1 = std::max(y1, p[1]); }
            const bool alongX = x1 - x0 >= y1 - y0;
            AddOpen(plan, o, alongX ? std::vector<P2>{{x0, (y0 + y1) / 2}, {x1, (y0 + y1) / 2}} : std::vector<P2>{{(x0 + x1) / 2, y0}, {(x0 + x1) / 2, y1}}, a, storey, report);
            return;
        }
        AddPoint(plan, Centroid(clean), a, storey, report);
    }
    // An open line.
    inline void AddOpen(Grid::Plan& plan, const Grid::Options& o, const std::vector<P2>& line, const Attributes& a, const std::string& storey, Json& report)
    {
        if (line.size() < 2) { if (line.size() == 1) AddPoint(plan, line[0], a, storey, report); return; }
        if (LineKind(a.kind))
        {
            for (size_t i = 0; i + 1 < line.size(); ++i)
            {
                Grid::Segment s;
                s.kind = a.kind; s.id = line.size() > 2 && !a.id.empty() ? a.id + "_" + std::to_string(i + 1) : a.id; s.storey = storey;
                s.x0 = line[i][0]; s.y0 = line[i][1]; s.x1 = line[i + 1][0]; s.y1 = line[i + 1][1];
                s.h0 = a.h0 ? a.h0 : a.h; s.h1 = a.h1 ? a.h1 : a.h;
                plan.segments.push_back(s);
            }
            return;
        }
        if (a.kind == "door")
        {
            for (size_t i = 0; i + 1 < line.size(); ++i)
            {
                const double dx = line[i + 1][0] - line[i][0], dy = line[i + 1][1] - line[i][1];
                const bool alongX = std::abs(dx) >= std::abs(dy);
                AddDoor(plan, {(line[i][0] + line[i + 1][0]) / 2, (line[i][1] + line[i + 1][1]) / 2}, std::hypot(dx, dy), 0, alongX, a, storey);
            }
            return;
        }
        if (PieceKind(a.kind) || a.kind == "solid")
        {
            if (line.size() >= 3) AddClosed(plan, o, line, a, storey, report);
            else Warn(report, "A " + a.kind + " drawn as a single line was left out; draw it as an outline.");
            return;
        }
        if (MarkerKind(a.kind))
        {
            // The marker stands at the first point, facing along the line.
            Attributes facing = a;
            if (!facing.yaw) facing.yaw = Degrees(std::atan2(line[1][1] - line[0][1], line[1][0] - line[0][0]));
            AddPoint(plan, line[0], facing, storey, report);
            return;
        }
        Warn(report, "Lines without a kind were left out; give them one (laser, ledge, door, ...).");
    }
    // Room names from labels inside them.
    inline void NameSpaces(Grid::Plan& plan, double x, double y, const std::string& storey, const std::string& text)
    {
        const auto name = Trim(text);
        if (name.empty()) return;
        for (auto& s : plan.spaces)
            if (Lower(s.storey) == Lower(storey) && s.name.empty() && x >= s.x0 && x <= s.x1 && y >= s.y0 && y <= s.y1) { s.name = name; return; }
        for (auto& p : plan.prisms)
            if (Lower(p.storey) == Lower(storey) && p.name.empty() && Grid::Inside(p.points, x, y)) { p.name = name; return; }
    }
    // Everything the plan holds, moved.
    inline void Shift(Grid::Plan& plan, double dx, double dy)
    {
        for (auto& s : plan.spaces) { s.x0 += dx; s.x1 += dx; s.y0 += dy; s.y1 += dy; }
        for (auto& p : plan.prisms) for (auto& q : p.points) { q[0] += dx; q[1] += dy; }
        for (auto& m : plan.markers) { m.x += dx; m.y += dy; }
        for (auto& s : plan.segments) { s.x0 += dx; s.x1 += dx; s.y0 += dy; s.y1 += dy; }
        for (auto& d : plan.doors) { d.x += dx; d.y += dy; }
    }
    // Moves the plan's middle to the origin (to whole units); returns false
    // when the plan is empty.
    inline bool Center(Grid::Plan& plan)
    {
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        auto grow = [&](double x, double y) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); };
        for (const auto& s : plan.spaces) { grow(s.x0, s.y0); grow(s.x1, s.y1); }
        for (const auto& p : plan.prisms) for (const auto& q : p.points) grow(q[0], q[1]);
        for (const auto& m : plan.markers) grow(m.x, m.y);
        for (const auto& s : plan.segments) { grow(s.x0, s.y0); grow(s.x1, s.y1); }
        for (const auto& d : plan.doors) grow(d.x, d.y);
        if (x0 > x1) return false;
        Shift(plan, -std::round((x0 + x1) / 2), -std::round((y0 + y1) / 2));
        return true;
    }
    // Attributes from a bag of named values (SVG data-*, Tiled properties,
    // GeoJSON properties): kind, id, name, yaw, h, z, height, mesh, class,
    // sound, radius, brightness, h0, h1, portal, and prop:<Name>/prop-<Name>
    // for raw actor properties. get returns "" for a missing name.
    template<class Get> Attributes ReadAttributes(Get get)
    {
        Attributes a;
        const auto kind = get("kind");
        if (!kind.empty()) { a.kind = Grid::Detail::KindOf(kind); a.explicitKind = !a.kind.empty(); }
        a.id = get("id");
        a.name = get("name");
        a.mesh = get("mesh");
        a.cls = get("class");
        a.sound = get("sound");
        auto number = [&](const char* key) -> std::optional<double> { double n = 0; if (ParseNumber(get(key), n)) return n; return std::nullopt; };
        a.yaw = number("yaw"); a.h = number("h"); a.z = number("z"); a.height = number("height"); a.radius = number("radius"); a.brightness = number("brightness");
        a.h0 = number("h0"); a.h1 = number("h1");
        const auto portal = Lower(get("portal"));
        if (!portal.empty()) { a.portalGiven = true; a.portal = portal == "true" || portal == "1" || portal == "yes"; }
        return a;
    }
}
