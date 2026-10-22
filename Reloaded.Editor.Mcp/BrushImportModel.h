#pragma once
// Convex brushes from Hammer (.vmf) and Quake-family (.map: Quake, Half-Life
// Valve 220, Quake 3 brushDef, Doom 3 brushDef3) files as SCCT polygon
// brushes. Engine-free and Windows-free.
//
// A brush there is the intersection of half-spaces, one per face plane. Each
// face's polygon is a huge square on its plane clipped by every other plane;
// redundant planes leave nothing and are dropped. The polygons wind the way
// the editor's brush faces do: (v1 - v0) x (v2 - v0) points out of the brush
// (docs/MapAuthoring.md, Design::Box). Faces over 16 vertices are split into
// fans, which the editor's polygon importer needs.
//
// Those engines build levels from solid brushes in a void; SCCT's world is
// solid and rooms are carved out of it. So the level is inverted: one
// subtractive hull over the bounds of every brush comes first, then each
// brush is added back (CSG_Add) inside it, giving the same walls and floors.
//
// Options: scale (2.5: a Hammer or Half-Life player is 72 units tall and an
// SCCT one about 180; Quake's is 56, so 3.2 suits Quake maps), mirrorY (true:
// those engines are right-handed and Unreal left-handed, so Y is flipped to
// keep the level as it plays; false keeps it as Hammer's top view shows it),
// offset [x, y, z] (added after scaling), center (false: true moves the
// bounds' centre to the origin and the lowest floor to z 0), margin (0: the
// hull's extra size), textures {sourceName: "Package.Group.Name"} and
// material (the texture of faces textures does not map), entities (true:
// player starts and lights become spawns and lights).
#include "Importers/Common.h"
#include <cstring>
#include <functional>
#include <optional>
#include <utility>

namespace Mcp::Brushes
{
    using Json = Workflow::Json;
    using Vector = Workflow::Vector;

    // normal . p = dist on the plane; the normal points out of the brush.
    struct Plane { Vector normal{}; double dist = 0; std::string texture; };
    struct Brush { std::vector<Plane> planes; std::string source; int line = 0; bool planeEquations = false; };
    struct Entity
    {
        std::string cls;
        std::vector<std::pair<std::string, std::string>> keys;
        std::vector<Brush> brushes;
        int line = 0;
        std::string Get(const std::string& key) const
        {
            for (const auto& [k, v] : keys) if (Import::Lower(k) == Import::Lower(key)) return v;
            return {};
        }
    };
    struct Polygon { std::vector<Vector> vertices; std::string texture; };

    // --- Vectors ---------------------------------------------------------------
    inline Vector Sub(const Vector& a, const Vector& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
    inline Vector Add(const Vector& a, const Vector& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
    inline Vector Scale(const Vector& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
    inline double Dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    inline Vector CrossV(const Vector& a, const Vector& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
    inline double Length(const Vector& a) { return std::sqrt(Dot(a, a)); }

    // Quake and Hammer list three points on each face so that
    // (p0 - p1) x (p2 - p1) points out of the brush.
    inline std::optional<Plane> PlaneFromPoints(const Vector& p0, const Vector& p1, const Vector& p2, const std::string& texture = {})
    {
        const auto n = CrossV(Sub(p0, p1), Sub(p2, p1));
        const double length = Length(n);
        if (!(length > 1e-9) || !std::isfinite(length)) return std::nullopt;
        Plane plane;
        plane.normal = Scale(n, 1 / length);
        plane.dist = Dot(plane.normal, p1);
        plane.texture = texture;
        return plane;
    }
    // The part of a convex polygon behind a plane (normal . p <= dist).
    inline std::vector<Vector> Clip(const std::vector<Vector>& polygon, const Plane& plane, double epsilon = 0.01)
    {
        std::vector<Vector> out;
        const size_t n = polygon.size();
        for (size_t i = 0; i < n; ++i)
        {
            const auto& a = polygon[i];
            const auto& b = polygon[(i + 1) % n];
            const double da = Dot(plane.normal, a) - plane.dist, db = Dot(plane.normal, b) - plane.dist;
            if (da <= epsilon) out.push_back(a);
            if ((da < -epsilon && db > epsilon) || (da > epsilon && db < -epsilon))
            {
                const double t = da / (da - db);
                out.push_back(Add(a, Scale(Sub(b, a), t)));
            }
        }
        return out;
    }
    // Tidy a face: points welded, coordinates rounded to a thousandth (whole
    // numbers kept whole), repeats and collinear points removed.
    inline std::vector<Vector> Tidy(const std::vector<Vector>& polygon)
    {
        std::vector<Vector> out;
        for (auto v : polygon)
        {
            for (auto& c : v)
            {
                const double r = std::round(c);
                c = std::abs(c - r) < 1e-3 ? r : std::round(c * 1000) / 1000;
                if (c == 0) c = 0; // no -0
            }
            if (out.empty() || Length(Sub(v, out.back())) > 0.01) out.push_back(v);
        }
        while (out.size() > 1 && Length(Sub(out.front(), out.back())) <= 0.01) out.pop_back();
        for (bool changed = true; changed && out.size() >= 3;)
        {
            changed = false;
            for (size_t i = 0; i < out.size() && out.size() >= 3; ++i)
            {
                const auto& a = out[(i + out.size() - 1) % out.size()];
                const auto& b = out[i];
                const auto& c = out[(i + 1) % out.size()];
                const double area = Length(CrossV(Sub(b, a), Sub(c, b)));
                if (area < 1e-4 * std::max(1.0, Length(Sub(b, a)) * Length(Sub(c, b))))
                {
                    out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    break;
                }
            }
        }
        return out;
    }
    // The faces of the convex solid the planes bound. Planes that repeat
    // another are skipped; a plane that bounds nothing gives no face.
    inline std::vector<Polygon> Faces(const std::vector<Plane>& planes, double extent = 1048576)
    {
        std::vector<Polygon> faces;
        std::vector<const Plane*> unique;
        for (const auto& p : planes)
        {
            bool repeat = false;
            for (const auto* q : unique)
                if (Dot(p.normal, q->normal) > 1 - 1e-9 && std::abs(p.dist - q->dist) < 0.01) repeat = true;
            if (!repeat) unique.push_back(&p);
        }
        for (const auto* plane : unique)
        {
            const auto& n = plane->normal;
            // u x v = n, so the square winds with its normal outward.
            const Vector axis = std::abs(n[2]) < 0.9 ? Vector{0, 0, 1} : Vector{1, 0, 0};
            auto u = CrossV(axis, n);
            u = Scale(u, 1 / Length(u));
            const auto v = CrossV(n, u);
            const auto centre = Scale(n, plane->dist);
            std::vector<Vector> polygon{Add(centre, Add(Scale(u, -extent), Scale(v, -extent))), Add(centre, Add(Scale(u, extent), Scale(v, -extent))),
                                        Add(centre, Add(Scale(u, extent), Scale(v, extent))), Add(centre, Add(Scale(u, -extent), Scale(v, extent)))};
            for (const auto* other : unique)
            {
                if (other == plane) continue;
                polygon = Clip(polygon, *other);
                if (polygon.size() < 3) break;
            }
            polygon = Tidy(polygon);
            if (polygon.size() < 3) continue;
            // Keep the winding the plane's normal asks for.
            Vector area{};
            for (size_t i = 1; i + 1 < polygon.size(); ++i) area = Add(area, CrossV(Sub(polygon[i], polygon[0]), Sub(polygon[i + 1], polygon[0])));
            if (Length(area) < 0.5) continue;
            if (Dot(area, n) < 0) std::reverse(polygon.begin(), polygon.end());
            faces.push_back({polygon, plane->texture});
        }
        return faces;
    }
    // Faces over 16 vertices as fans of at most 16.
    inline std::vector<Polygon> Split16(const std::vector<Polygon>& faces)
    {
        std::vector<Polygon> out;
        for (const auto& f : faces)
        {
            if (f.vertices.size() <= 16) { out.push_back(f); continue; }
            for (size_t first = 1; first + 1 < f.vertices.size();)
            {
                const size_t last = std::min(f.vertices.size() - 1, first + 14);
                Polygon piece{{f.vertices[0]}, f.texture};
                for (size_t k = first; k <= last; ++k) piece.vertices.push_back(f.vertices[k]);
                out.push_back(piece);
                first = last;
            }
        }
        return out;
    }

    // --- Reading files ----------------------------------------------------------
    namespace Detail
    {
        struct Token { enum Kind { Word, String, Open, Close, ParenOpen, ParenClose, BracketOpen, BracketClose, End } kind = End; std::string text; int line = 0; };
        // Words, quoted strings and brackets; // comments skipped.
        class Lexer
        {
        public:
            explicit Lexer(const std::string& source) : text(source) {}
            Token Next()
            {
                for (;;)
                {
                    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) { if (text[at] == '\n') ++line; ++at; }
                    if (at + 1 < text.size() && text[at] == '/' && text[at + 1] == '/') { while (at < text.size() && text[at] != '\n') ++at; continue; }
                    break;
                }
                Token t;
                t.line = line;
                if (at >= text.size()) return t;
                const char c = text[at];
                const char* single = "{}()[]";
                if (const char* found = std::strchr(single, c); found && c)
                {
                    ++at;
                    t.kind = static_cast<Token::Kind>(Token::Open + (found - single));
                    t.text = std::string(1, c);
                    return t;
                }
                if (c == '"')
                {
                    ++at;
                    t.kind = Token::String;
                    while (at < text.size() && text[at] != '"') { if (text[at] == '\n') ++line; t.text += text[at++]; }
                    if (at >= text.size()) throw std::runtime_error("A quoted string that starts on line " + std::to_string(t.line) + " never ends.");
                    ++at;
                    return t;
                }
                t.kind = Token::Word;
                while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at])) && !std::strchr("{}()\"", text[at])) t.text += text[at++];
                return t;
            }
            Token Peek() { const auto saveAt = at; const auto saveLine = line; auto t = Next(); at = saveAt; line = saveLine; return t; }
        private:
            const std::string& text;
            size_t at = 0;
            int line = 1;
        };
        inline double Number(const Token& t)
        {
            double n = 0;
            if ((t.kind != Token::Word && t.kind != Token::String) || !Import::ParseNumber(t.text, n))
                throw std::runtime_error("Line " + std::to_string(t.line) + ": expected a number, found \"" + t.text + "\".");
            return n;
        }
        // Textures that are not solid surfaces: triggers, clips, hints, skips,
        // origins, portals and liquids.
        inline bool Helper(const std::string& texture)
        {
            const auto t = Import::Lower(texture);
            static const char* parts[] = {"toolstrigger", "toolsclip", "toolsplayerclip", "toolsnpcclip", "toolshint", "toolsskip", "toolsorigin", "toolsareaportal",
                                          "toolsoccluder", "toolsfog", "toolsblocklight", "toolsblockbullets", "toolsinvisibleladder", "common/clip", "common/trigger",
                                          "common/hint", "common/skip", "common/origin", "common/areaportal", "common/weapclip", "common/fullclip", "common/botclip"};
            for (const char* p : parts) if (t.find(p) != std::string::npos) return true;
            return t == "clip" || t == "trigger" || t == "hint" || t == "skip" || t == "origin" || t == "aaatrigger";
        }
        inline bool Liquid(const std::string& texture)
        {
            const auto t = Import::Lower(texture);
            return (!t.empty() && t[0] == '*') || t.find("water") != std::string::npos || t.find("lava") != std::string::npos || t.find("slime") != std::string::npos;
        }
    }
    // Hammer's KeyValues: world { solid { side { "plane" "(..) (..) (..)" ... } } } and entity blocks.
    inline std::vector<Entity> ParseVmf(const std::string& text, Json& report)
    {
        using namespace Detail;
        struct Node { std::string name; std::vector<std::pair<std::string, std::string>> keys; std::vector<Node> children; int line = 0; };
        Lexer lexer(text);
        std::function<void(Node&, int)> block = [&](Node& node, int depth)
        {
            if (depth > 64) throw std::runtime_error("The VMF nests blocks too deeply (line " + std::to_string(node.line) + ").");
            for (;;)
            {
                auto t = lexer.Next();
                if (t.kind == Token::Close) return;
                if (t.kind == Token::End)
                {
                    if (depth == 0) return;
                    throw std::runtime_error("The VMF ends inside the \"" + node.name + "\" block that starts on line " + std::to_string(node.line) + ".");
                }
                if (t.kind != Token::Word && t.kind != Token::String) throw std::runtime_error("Line " + std::to_string(t.line) + " of the VMF is not a key, a value or a block.");
                auto next = lexer.Peek();
                if (next.kind == Token::Open)
                {
                    lexer.Next();
                    Node child;
                    child.name = Import::Lower(t.text);
                    child.line = t.line;
                    block(child, depth + 1);
                    node.children.push_back(std::move(child));
                }
                else if (next.kind == Token::String || next.kind == Token::Word) node.keys.push_back({t.text, lexer.Next().text});
                else throw std::runtime_error("Line " + std::to_string(t.line) + " of the VMF: \"" + t.text + "\" has no value.");
            }
        };
        Node root;
        block(root, 0);
        std::vector<Entity> entities;
        long long displacements = 0, hidden = 0;
        auto get = [](const Node& n, const std::string& key)
        {
            for (const auto& [k, v] : n.keys) if (Import::Lower(k) == key) return v;
            return std::string();
        };
        std::function<void(const Node&, Entity&, bool)> solids = [&](const Node& n, Entity& e, bool inHidden)
        {
            for (const auto& child : n.children)
            {
                if (child.name == "hidden") { solids(child, e, true); continue; }
                if (child.name != "solid") continue;
                if (inHidden) ++hidden;
                Brush brush;
                brush.line = child.line;
                brush.source = "solid " + get(child, "id");
                for (const auto& side : child.children)
                {
                    if (side.name != "side") continue;
                    for (const auto& sub : side.children) if (sub.name == "dispinfo") ++displacements;
                    const auto numbers = Import::Numbers([&] { auto p = get(side, "plane"); std::replace(p.begin(), p.end(), '(', ' '); std::replace(p.begin(), p.end(), ')', ' '); return p; }());
                    if (numbers.size() != 9) throw std::runtime_error("Line " + std::to_string(side.line) + " of the VMF: a side's plane needs three points.");
                    if (const auto plane = PlaneFromPoints({numbers[0], numbers[1], numbers[2]}, {numbers[3], numbers[4], numbers[5]}, {numbers[6], numbers[7], numbers[8]}, get(side, "material")))
                        brush.planes.push_back(*plane);
                }
                e.brushes.push_back(std::move(brush));
            }
        };
        for (const auto& child : root.children)
        {
            if (child.name != "world" && child.name != "entity" && child.name != "hidden") continue;
            const Node* node = &child;
            // A hidden entity is wrapped: hidden { entity { ... } }.
            if (child.name == "hidden")
            {
                for (const auto& inner : child.children)
                    if (inner.name == "entity")
                    {
                        Entity e;
                        e.keys = inner.keys;
                        e.cls = Import::Lower(get(inner, "classname"));
                        e.line = inner.line;
                        solids(inner, e, true);
                        entities.push_back(std::move(e));
                        ++hidden;
                    }
                continue;
            }
            Entity e;
            e.keys = node->keys;
            e.cls = child.name == "world" ? "worldspawn" : Import::Lower(get(*node, "classname"));
            e.line = node->line;
            solids(*node, e, false);
            entities.push_back(std::move(e));
        }
        if (entities.empty()) throw std::runtime_error("The file has no world or entity blocks, so it is not a Hammer VMF.");
        if (displacements) Import::Warn(report, std::to_string(displacements) + " displacement faces were imported flat (displacements are not supported).");
        if (hidden) Import::Warn(report, std::to_string(hidden) + " hidden objects were imported too.");
        return entities;
    }
    // Quake-family .map: { "key" "value" { ( x y z ) ( x y z ) ( x y z ) TEXTURE ... } }.
    inline std::vector<Entity> ParseQuake(const std::string& text, Json& report)
    {
        using namespace Detail;
        Lexer lexer(text);
        std::vector<Entity> entities;
        long long patches = 0;
        auto expect = [&](Token::Kind kind, const char* what)
        {
            auto t = lexer.Next();
            if (t.kind != kind) throw std::runtime_error("Line " + std::to_string(t.line) + " of the map: expected " + what + ", found \"" + t.text + "\".");
            return t;
        };
        auto skipGroup = [&](Token::Kind open, Token::Kind close)
        {
            for (int depth = 1; depth > 0;)
            {
                const auto t = lexer.Next();
                if (t.kind == Token::End) throw std::runtime_error("The map file ends inside a bracketed group.");
                if (t.kind == open) ++depth;
                else if (t.kind == close) --depth;
            }
        };
        auto point = [&]
        {
            std::vector<double> n;
            for (;;)
            {
                auto t = lexer.Next();
                if (t.kind == Token::ParenClose) break;
                n.push_back(Number(t));
                if (n.size() > 4) throw std::runtime_error("Line " + std::to_string(t.line) + " of the map: a point has too many numbers.");
            }
            return n;
        };
        // Planes until the brush's closing brace; brushDef and brushDef3
        // carry their texture matrix in nested parentheses.
        auto planes = [&](Brush& brush)
        {
            for (;;)
            {
                auto t = lexer.Next();
                if (t.kind == Token::Close) return;
                if (t.kind != Token::ParenOpen) throw std::runtime_error("Line " + std::to_string(t.line) + " of the map: expected a face \"( x y z )\", found \"" + t.text + "\".");
                const auto first = point();
                Vector p[3]{};
                bool equation = false;
                Plane plane;
                if (first.size() == 4)
                {
                    // brushDef3: ( a b c d ) with a x + b y + c z + d = 0.
                    equation = true;
                    const Vector n{first[0], first[1], first[2]};
                    const double len = Length(n);
                    if (len < 1e-9) throw std::runtime_error("Line " + std::to_string(t.line) + " of the map: a plane with no normal.");
                    plane.normal = Scale(n, 1 / len);
                    plane.dist = -first[3] / len;
                }
                else
                {
                    if (first.size() != 3) throw std::runtime_error("Line " + std::to_string(t.line) + " of the map: a point needs x, y and z.");
                    p[0] = {first[0], first[1], first[2]};
                    for (int k = 1; k < 3; ++k)
                    {
                        expect(Token::ParenOpen, "\"(\"");
                        const auto q = point();
                        if (q.size() != 3) throw std::runtime_error("Line " + std::to_string(t.line) + " of the map: a point needs x, y and z.");
                        p[k] = {q[0], q[1], q[2]};
                    }
                }
                if (lexer.Peek().kind == Token::ParenOpen) { lexer.Next(); skipGroup(Token::ParenOpen, Token::ParenClose); }
                const auto texture = lexer.Next();
                if (texture.kind != Token::Word && texture.kind != Token::String) throw std::runtime_error("Line " + std::to_string(texture.line) + " of the map: a face has no texture.");
                // Offsets, rotation, scale, Valve 220 axes, Quake 3 flags.
                for (auto next = lexer.Peek(); next.kind == Token::Word || next.kind == Token::BracketOpen || next.kind == Token::BracketClose; next = lexer.Peek()) lexer.Next();
                if (equation)
                {
                    plane.texture = texture.text;
                    brush.planes.push_back(plane);
                    brush.planeEquations = true;
                }
                else if (const auto made = PlaneFromPoints(p[0], p[1], p[2], texture.text)) brush.planes.push_back(*made);
            }
        };
        for (;;)
        {
            auto t = lexer.Next();
            if (t.kind == Token::End) break;
            if (t.kind != Token::Open) throw std::runtime_error("Line " + std::to_string(t.line) + " of the map: expected an entity \"{\", found \"" + t.text + "\".");
            Entity e;
            e.line = t.line;
            for (;;)
            {
                auto inner = lexer.Next();
                if (inner.kind == Token::Close) break;
                if (inner.kind == Token::End) throw std::runtime_error("The map file ends inside the entity that starts on line " + std::to_string(e.line) + ".");
                if (inner.kind == Token::String || (inner.kind == Token::Word && lexer.Peek().kind == Token::String))
                {
                    const auto value = lexer.Next();
                    e.keys.push_back({inner.text, value.text});
                    continue;
                }
                if (inner.kind != Token::Open) throw std::runtime_error("Line " + std::to_string(inner.line) + " of the map: expected a key or a brush, found \"" + inner.text + "\".");
                Brush brush;
                brush.line = inner.line;
                brush.source = "brush at line " + std::to_string(inner.line);
                const auto head = lexer.Peek();
                if (head.kind == Token::Word && (Import::Lower(head.text) == "patchdef2" || Import::Lower(head.text) == "patchdef3" || Import::Lower(head.text) == "patchdefws"))
                {
                    lexer.Next();
                    expect(Token::Open, "\"{\"");
                    skipGroup(Token::Open, Token::Close);
                    expect(Token::Close, "\"}\"");
                    ++patches;
                    continue;
                }
                if (head.kind == Token::Word && (Import::Lower(head.text) == "brushdef" || Import::Lower(head.text) == "brushdef3"))
                {
                    lexer.Next();
                    expect(Token::Open, "\"{\"");
                    planes(brush);
                    expect(Token::Close, "\"}\"");
                }
                else planes(brush);
                e.brushes.push_back(std::move(brush));
            }
            e.cls = Import::Lower(e.Get("classname"));
            entities.push_back(std::move(e));
        }
        if (entities.empty()) throw std::runtime_error("The map file has no entities.");
        if (patches) Import::Warn(report, std::to_string(patches) + " curved patches were left out (only brushes are imported).");
        return entities;
    }

    // --- The level spec ---------------------------------------------------------
    struct Options
    {
        double scale = 2.5;
        bool mirrorY = true, center = false, entities = true;
        Vector offset{0, 0, 0};
        double margin = 0;
        Json textures = Json::object();
        std::string material;
    };
    inline Options ReadOptions(const Json& options)
    {
        Options o;
        o.scale = Import::Option(options, "scale", o.scale, 0.001, 1000);
        o.mirrorY = Import::Option(options, "mirrorY", o.mirrorY);
        o.center = Import::Option(options, "center", o.center);
        o.entities = Import::Option(options, "entities", o.entities);
        o.margin = Import::Option(options, "margin", o.margin, 0, 65536);
        o.material = Import::Option(options, "material", "");
        if (options.is_object() && options.contains("offset"))
        {
            const auto& v = options.at("offset");
            if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) throw std::runtime_error("Option offset is [x, y, z].");
            o.offset = {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
        }
        if (options.is_object() && options.contains("textures"))
        {
            if (!options.at("textures").is_object()) throw std::runtime_error("Option textures maps source texture names to Package.Group.Name.");
            for (auto it = options.at("textures").begin(); it != options.at("textures").end(); ++it) o.textures[Import::Lower(it.key())] = it.value();
        }
        return o;
    }
    namespace Detail
    {
        inline const std::set<std::string>& SolidEntities()
        {
            static const std::set<std::string> classes{"worldspawn", "func_detail", "func_wall", "func_brush", "func_group", "func_static", "func_detail_wall",
                                                        "func_lod", "func_wall_toggle", "func_breakable", "func_breakable_surf", "func_illusionary_solid", "detail"};
            return classes;
        }
    }
    // The entities' brushes inverted for SCCT, plus starts and lights.
    inline Json ToSpec(const std::vector<Entity>& entities, const Json& options, Json& report, const std::string& format)
    {
        using namespace Detail;
        Import::Report(report);
        const auto o = ReadOptions(options);
        Json spec = Import::NewSpec(options, format);
        Import::Section(spec, {"defaults"})["construction"] = "Carve";
        if (!o.material.empty()) spec["defaults"]["material"] = o.material;
        const double my = o.mirrorY ? -1 : 1;
        auto toPlane = [&](const Plane& p)
        {
            Plane out = p;
            out.normal = {p.normal[0], p.normal[1] * my, p.normal[2]};
            out.dist = p.dist * o.scale;
            return out;
        };
        auto toPoint = [&](const Vector& v) { return Vector{v[0] * o.scale, v[1] * o.scale * my, v[2] * o.scale}; };
        struct Built { std::vector<Polygon> faces; Vector lo{}, hi{}; };
        std::vector<Built> built;
        std::map<std::string, long long> skipped;
        std::set<std::string> unmapped;
        long long brushes = 0, broken = 0;
        Vector lo{1e18, 1e18, 1e18}, hi{-1e18, -1e18, -1e18};
        for (const auto& e : entities)
        {
            if (e.brushes.empty()) continue;
            if (!SolidEntities().count(e.cls)) { skipped[e.cls.empty() ? std::string("(no class)") : e.cls] += static_cast<long long>(e.brushes.size()); continue; }
            for (const auto& b : e.brushes)
            {
                ++brushes;
                bool helper = !b.planes.empty(), liquid = false;
                for (const auto& p : b.planes) { helper = helper && Helper(p.texture); liquid = liquid || Liquid(p.texture); }
                if (helper) { skipped["tool brushes (clip, trigger, hint, skip)"]++; continue; }
                if (liquid) { skipped["liquid brushes"]++; continue; }
                std::vector<Plane> planes;
                for (const auto& p : b.planes) planes.push_back(toPlane(p));
                auto faces = Faces(planes);
                if (faces.size() < 4 && b.planeEquations)
                {
                    // Doom 3 planes may face inward.
                    for (auto& p : planes) { p.normal = Scale(p.normal, -1); p.dist = -p.dist; }
                    faces = Faces(planes);
                }
                if (faces.size() < 4) { ++broken; continue; }
                Built item;
                item.lo = {1e18, 1e18, 1e18};
                item.hi = {-1e18, -1e18, -1e18};
                for (const auto& f : faces)
                    for (const auto& v : f.vertices)
                        for (size_t k = 0; k < 3; ++k) { item.lo[k] = std::min(item.lo[k], v[k]); item.hi[k] = std::max(item.hi[k], v[k]); }
                bool tooBig = false;
                for (size_t k = 0; k < 3; ++k) tooBig = tooBig || item.hi[k] - item.lo[k] > 500000;
                if (tooBig) { ++broken; continue; }
                for (size_t k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], item.lo[k]); hi[k] = std::max(hi[k], item.hi[k]); }
                item.faces = Split16(faces);
                built.push_back(std::move(item));
            }
        }
        if (broken) Import::Warn(report, std::to_string(broken) + " brushes are not closed convex solids (or are far too large) and were left out.");
        for (const auto& [cls, n] : skipped) Import::Warn(report, std::to_string(n) + " brushes of " + cls + " were left out (not static solid geometry).");
        Import::Count(report, "sourceBrushes", brushes);
        // Where the level lands: as authored (scaled, mirrored), or centred.
        Vector shift = o.offset;
        if (o.center && !built.empty()) shift = Add(shift, Vector{-(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -lo[2]});
        auto move = [&](const Vector& v) { return Add(v, shift); };
        auto& list = Import::List(spec, {"geometry", "brushes"});
        Import::Ids ids;
        auto textureOf = [&](const std::string& source) -> std::string
        {
            const auto key = Import::Lower(source);
            if (o.textures.contains(key) && o.textures.at(key).is_string()) return o.textures.at(key).get<std::string>();
            if (!source.empty() && !Helper(source)) unmapped.insert(source);
            return o.material;
        };
        auto polygonJson = [&](const Polygon& p, const Vector& at)
        {
            Json vertices = Json::array();
            for (const auto& v : p.vertices) { const auto w = Sub(move(v), at); vertices.push_back(Import::Point(w[0], w[1], w[2])); }
            Json polygon{{"vertices", vertices}};
            const auto texture = textureOf(p.texture);
            if (!texture.empty()) polygon["texture"] = texture;
            return polygon;
        };
        if (!built.empty())
        {
            // The hull: everything inside the bounds is carved, then the
            // brushes are added back.
            Vector a = move(lo), b = move(hi);
            for (size_t k = 0; k < 3; ++k) { a[k] -= o.margin; b[k] += o.margin; }
            const Vector at{std::round((a[0] + b[0]) / 2), std::round((a[1] + b[1]) / 2), std::round(a[2])};
            Json polygons = Json::array();
            const int corners[6][4] = {{1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3}, {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}};
            for (const auto& face : corners)
            {
                Json vertices = Json::array();
                for (int c : face) vertices.push_back(Import::Point((c & 1 ? b[0] : a[0]) - at[0], (c & 2 ? b[1] : a[1]) - at[1], (c & 4 ? b[2] : a[2]) - at[2]));
                Json polygon{{"vertices", vertices}};
                if (!o.material.empty()) polygon["texture"] = o.material;
                polygons.push_back(polygon);
            }
            list.push_back({{"id", ids.Make("Hull", "Hull")}, {"csg", "CSG_Subtract"}, {"at", Import::Point(at[0], at[1], at[2])}, {"polygons", polygons}});
            for (const auto& item : built)
            {
                const Vector c = move(Scale(Add(item.lo, item.hi), 0.5));
                const Vector at2{std::round(c[0]), std::round(c[1]), std::round(c[2])};
                Json polygons2 = Json::array();
                for (const auto& f : item.faces) polygons2.push_back(polygonJson(f, at2));
                list.push_back({{"id", ids.Next("Brush")}, {"csg", "CSG_Add"}, {"at", Import::Point(at2[0], at2[1], at2[2])}, {"polygons", polygons2}});
            }
            Import::Count(report, "brushes", static_cast<long long>(built.size()));
            const Vector size = Sub(hi, lo);
            Import::Assume(report, "The level is " + Import::Num(size[0]).dump() + " x " + Import::Num(size[1]).dump() + " x " + Import::Num(size[2]).dump() +
                                       " units after scaling by " + Import::Num(o.scale).dump() + "; a hull carves those bounds and every brush is added back inside it.");
        }
        else Import::Warn(report, "No solid brushes were found, so the spec has no geometry.");
        Import::Assume(report, std::string("Coordinates are scaled by ") + Import::Num(o.scale).dump() +
                                   " (Hammer and Half-Life players are 72 units tall, SCCT's about 180; use 3.2 for Quake maps)" +
                                   (o.mirrorY ? "; Y is mirrored because Unreal is left-handed, so the level plays as it did (mirrorY false keeps the editor's top view instead)." : "; Y is kept as authored (mirrorY false), so the level plays mirrored."));
        if (!unmapped.empty())
        {
            std::string names;
            size_t shown = 0;
            for (const auto& n : unmapped) { if (shown++ == 12) { names += ", ..."; break; } names += (names.empty() ? "" : ", ") + n; }
            Import::Warn(report, std::to_string(unmapped.size()) + " source textures have no SCCT texture (" + names + "); map them with the textures option" +
                                     (o.material.empty() ? " or set material." : "; they use material."));
        }
        // Starts and lights.
        if (o.entities)
        {
            int neutral = 0;
            long long points = 0;
            for (const auto& e : entities)
            {
                if (!e.brushes.empty()) continue;
                const auto origin = Import::Numbers(e.Get("origin"));
                if (origin.size() != 3 || e.cls.empty() || e.cls == "worldspawn") continue;
                double yaw = Import::NumberOr(e.Get("angle"), 0);
                if (const auto angles = Import::Numbers(e.Get("angles")); angles.size() == 3) yaw = angles[1];
                yaw = std::fmod(yaw * my + 720, 360);
                const auto at = move(toPoint({origin[0], origin[1], origin[2]}));
                const auto& c = e.cls;
                const auto team = Import::NumberOr(e.Get("TeamNum"), Import::NumberOr(e.Get("team_no"), 0));
                std::string side;
                if (c == "info_player_terrorist" || c == "info_player_rebel" || c == "info_player_axis" || c == "info_player_red" || (c == "info_player_teamspawn" && team == 2)) side = "spy";
                else if (c == "info_player_counterterrorist" || c == "info_player_combine" || c == "info_player_allies" || c == "info_player_blue" || (c == "info_player_teamspawn" && team == 3)) side = "merc";
                else if (c == "info_player_start" || c == "info_player_deathmatch" || c == "info_player_coop" || c == "info_player_teamspawn") side = neutral++ % 2 ? "merc" : "spy";
                if (!side.empty())
                {
                    // Their origins are at the feet (Hammer) or 24 above (Quake);
                    // an SCCT start stands 48 above the floor.
                    const double feet = format == "vmf" ? at[2] : at[2] - 24 * o.scale;
                    Import::List(spec, {"spawns", side == "spy" ? "spy" : "merc"}).push_back(
                        {{"id", ids.Next(side == "spy" ? "SpyStart" : "MercStart")}, {"at", Import::Point(at[0], at[1], feet + 48)}, {"yaw", Import::Num(std::round(yaw * 10) / 10)}});
                    Import::Count(report, side == "spy" ? "spyStarts" : "mercStarts");
                    continue;
                }
                if (c == "light" || c == "light_spot" || c == "light_dynamic" || c == "light_torch_small_walltorch" || c == "light_fluoro" || c == "light_flame_large_yellow")
                {
                    Json light{{"id", ids.Next("Light")}, {"preset", c == "light_spot" ? "spot" : "ceiling"}, {"at", Import::Point(at[0], at[1], at[2])}};
                    if (c == "light_spot") light["yaw"] = Import::Num(std::round(yaw * 10) / 10);
                    Import::List(spec, {"environment", "lights"}).push_back(light);
                    Import::Count(report, "lights");
                    continue;
                }
                ++points;
            }
            if (neutral) Import::Assume(report, "Starts without a team alternate between spies and mercs.");
            if (report.at("counts").value("spyStarts", 0) || report.at("counts").value("mercStarts", 0))
                Import::Assume(report, "Terrorist, rebel, red and TeamNum 2 starts became spy starts; counter-terrorist, combine, blue and TeamNum 3 starts became merc starts.");
            if (points) Import::Warn(report, std::to_string(points) + " other point entities (props, triggers, sounds) were left out.");
            if (report.at("counts").value("lights", 0)) Import::Section(spec, {"defaults"})["lights"] = "none";
        }
        spec["provenance"]["notes"] = report.at("assumptions");
        Import::Prune(spec);
        return spec;
    }
    inline Json ImportVmf(const std::string& text, const Json& options, Json& report) { Import::Report(report); return ToSpec(ParseVmf(text, report), options, report, "vmf"); }
    inline Json ImportQuake(const std::string& text, const Json& options, Json& report) { Import::Report(report); return ToSpec(ParseQuake(text, report), options, report, "map"); }
}
