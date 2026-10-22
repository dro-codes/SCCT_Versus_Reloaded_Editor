#pragma once
// Unreal T3D text (MAP EXPORT, Edit > Copy, the map-authoring export's
// geometryT3d, other UE1/UE2 games) to scct.level-spec. Engine-free. RE+'s
// RecoveredActorImport prepares whole native exports for MAP IMPORT and is
// not a reader of layouts, so this has its own small parser.
//
// CSG brushes (CsgOper CSG_Add or CSG_Subtract) become polygon brushes with
// their PrePivot, MainScale, Rotation and PostScale applied; the builder
// brush (no CsgOper) is skipped. Volumes become environment.volumes boxes.
// PlayerStarts become spawns (TeamNumber 0 spy, 1 merc), lights become
// lights, StaticMeshActors become geometry.meshes, and any other actor an
// environment.props entry with its class, position and yaw (with
// options.properties "all", its exported properties too; references to
// other actors in them may not resolve in the new level). LevelInfo,
// cameras and movers are left out. Options: scale (1), offset [x, y, z],
// geometry (true), actors (true), properties ("none" | "all").
#include "Common.h"
#include <functional>
#include <optional>

namespace Mcp::Import
{
    namespace T3d
    {
        struct Block
        {
            std::string type;
            std::vector<std::pair<std::string, std::string>> header;
            std::vector<std::pair<std::string, std::string>> properties;
            std::vector<std::string> lines;
            std::vector<Block> children;
            int line = 0;
            std::string Header(const std::string& key) const
            {
                for (const auto& [k, v] : header) if (Lower(k) == Lower(key)) return v;
                return {};
            }
            std::string Get(const std::string& key) const
            {
                for (const auto& [k, v] : properties) if (Lower(k) == Lower(key)) return v;
                return {};
            }
            bool Has(const std::string& key) const
            {
                for (const auto& kv : properties) if (Lower(kv.first) == Lower(key)) return true;
                return false;
            }
        };
        inline std::string Unquote(std::string v)
        {
            v = Trim(v);
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            return v;
        }
        // The text as nested Begin/End blocks; Key=Value lines are properties.
        inline Block Parse(const std::string& text)
        {
            Block root;
            root.type = "Root";
            std::vector<Block*> stack{&root};
            int number = 0;
            for (const auto& raw : Lines(text))
            {
                ++number;
                const auto line = Trim(raw);
                if (line.empty()) continue;
                const auto lower = Lower(line);
                if (StartsWith(lower, "begin ") || lower == "begin")
                {
                    if (stack.size() > 32) throw std::runtime_error("T3D blocks nest too deeply (line " + std::to_string(number) + ").");
                    Block b;
                    b.line = number;
                    std::vector<std::string> tokens;
                    std::string token;
                    bool quoted = false;
                    for (char c : line.substr(5))
                    {
                        if (c == '"') quoted = !quoted;
                        if (!quoted && std::isspace(static_cast<unsigned char>(c))) { if (!token.empty()) tokens.push_back(token); token.clear(); }
                        else token += c;
                    }
                    if (!token.empty()) tokens.push_back(token);
                    if (tokens.empty()) throw std::runtime_error("A T3D Begin has no block type (line " + std::to_string(number) + ").");
                    b.type = tokens[0];
                    for (size_t i = 1; i < tokens.size(); ++i)
                    {
                        const auto eq = tokens[i].find('=');
                        if (eq != std::string::npos) b.header.push_back({tokens[i].substr(0, eq), Unquote(tokens[i].substr(eq + 1))});
                    }
                    stack.back()->children.push_back(std::move(b));
                    stack.push_back(&stack.back()->children.back());
                    continue;
                }
                if (StartsWith(lower, "end ") || lower == "end")
                {
                    if (stack.size() == 1) throw std::runtime_error("A T3D End has no Begin (line " + std::to_string(number) + ").");
                    stack.pop_back();
                    continue;
                }
                // Key=Value, where the key is a name with an optional (index) or [index].
                size_t k = 0;
                while (k < line.size() && (std::isalnum(static_cast<unsigned char>(line[k])) || line[k] == '_')) ++k;
                if (k > 0 && k < line.size() && (line[k] == '(' || line[k] == '['))
                {
                    const auto close = line.find(line[k] == '(' ? ')' : ']', k);
                    if (close != std::string::npos && close + 1 < line.size() && line[close + 1] == '=') k = close + 1;
                }
                if (k > 0 && k < line.size() && line[k] == '=') stack.back()->properties.push_back({line.substr(0, k), line.substr(k + 1)});
                else stack.back()->lines.push_back(line);
            }
            if (stack.size() != 1) throw std::runtime_error("The T3D ends inside a " + stack.back()->type + " block that starts on line " + std::to_string(stack.back()->line) + ".");
            return root;
        }
        // A T3D value as a map-changes value: (X=1,Y=2) -> {"X": "1", "Y": "2"},
        // ((A=1),(A=2)) -> [{"A": "1"}, {"A": "2"}], anything else the text.
        inline Json Value(const std::string& raw)
        {
            const auto t = Trim(raw);
            if (t.size() < 2 || t.front() != '(' || t.back() != ')') return t;
            const auto inner = t.substr(1, t.size() - 2);
            std::vector<std::string> parts;
            int depth = 0;
            bool quoted = false;
            size_t start = 0;
            for (size_t i = 0; i <= inner.size(); ++i)
            {
                const char c = i < inner.size() ? inner[i] : ',';
                if (c == '"') quoted = !quoted;
                else if (!quoted && c == '(') ++depth;
                else if (!quoted && c == ')') --depth;
                else if (!quoted && depth == 0 && c == ',') { parts.push_back(Trim(inner.substr(start, i - start))); start = i + 1; }
            }
            // Where a part's top-level = is, if it has one.
            auto equals = [](const std::string& part)
            {
                int d = 0;
                bool q = false;
                for (size_t i = 0; i < part.size(); ++i)
                {
                    const char c = part[i];
                    if (c == '"') q = !q;
                    else if (!q && c == '(') ++d;
                    else if (!q && c == ')') --d;
                    else if (!q && d == 0 && c == '=') return i;
                }
                return std::string::npos;
            };
            bool keyed = !parts.empty();
            for (const auto& part : parts) keyed = keyed && equals(part) != std::string::npos;
            if (keyed)
            {
                Json out = Json::object();
                for (const auto& part : parts) { const auto eq = equals(part); out[Trim(part.substr(0, eq))] = Value(part.substr(eq + 1)); }
                return out;
            }
            Json out = Json::array();
            for (const auto& part : parts) if (!part.empty()) out.push_back(Value(part));
            return out;
        }
        inline Vector VectorFrom(const Json& v, Vector fallback)
        {
            if (!v.is_object()) return fallback;
            Vector out = fallback;
            const char* keys[3] = {"X", "Y", "Z"};
            for (size_t i = 0; i < 3; ++i)
                for (auto it = v.begin(); it != v.end(); ++it)
                    if (Lower(it.key()) == Lower(keys[i]) && it.value().is_string()) out[i] = NumberOr(it.value().get<std::string>(), out[i]);
            return out;
        }
        inline Vector VectorOf(const std::string& raw, Vector fallback) { return VectorFrom(Value(raw), fallback); }
        inline Workflow::Rotation RotationOf(const std::string& raw)
        {
            const auto v = Value(raw);
            Workflow::Rotation r{0, 0, 0};
            if (!v.is_object()) return r;
            const char* keys[3] = {"Pitch", "Yaw", "Roll"};
            for (size_t i = 0; i < 3; ++i)
                for (auto it = v.begin(); it != v.end(); ++it)
                    if (Lower(it.key()) == Lower(keys[i]) && it.value().is_string()) r[i] = static_cast<int>(std::lround(NumberOr(it.value().get<std::string>(), 0)));
            return r;
        }
        // MainScale=(Scale=(X=..,Y=..,Z=..),SheerAxis=..): its Scale.
        inline Vector ScaleOf(const std::string& raw)
        {
            if (raw.empty()) return {1, 1, 1};
            const auto v = Value(raw);
            if (v.is_object())
                for (auto it = v.begin(); it != v.end(); ++it)
                    if (Lower(it.key()) == "scale") return VectorFrom(it.value(), {1, 1, 1});
            return {1, 1, 1};
        }
        // Every Actor block, wherever it is (Map, Level, or loose).
        inline void Actors(const Block& b, std::vector<const Block*>& out)
        {
            for (const auto& c : b.children)
            {
                if (Lower(c.type) == "actor") out.push_back(&c);
                else Actors(c, out);
            }
        }
        // A brush actor's polygons in its local space (Location not applied).
        inline Json Polygons(const Block& actor, double scale, size_t& polygons)
        {
            Json out = Json::array();
            const Vector pivot = VectorOf(actor.Get("PrePivot"), {0, 0, 0}), main = ScaleOf(actor.Get("MainScale")), post = ScaleOf(actor.Get("PostScale"));
            const auto rotation = RotationOf(actor.Get("Rotation"));
            std::function<void(const Block&)> walk = [&](const Block& b)
            {
                for (const auto& c : b.children)
                {
                    if (Lower(c.type) != "polygon") { walk(c); continue; }
                    Json vertices = Json::array();
                    for (const auto& l : c.lines)
                    {
                        if (!StartsWith(Lower(l), "vertex")) continue;
                        const auto n = Numbers(l.substr(6));
                        if (n.size() != 3) continue;
                        Vector v{(n[0] - pivot[0]) * main[0], (n[1] - pivot[1]) * main[1], (n[2] - pivot[2]) * main[2]};
                        if (rotation != Workflow::Rotation{0, 0, 0}) v = Workflow::TransformPoint(v, {{0, 0, 0}, rotation});
                        vertices.push_back(Point(v[0] * post[0] * scale, v[1] * post[1] * scale, v[2] * post[2] * scale));
                    }
                    if (vertices.size() < 3) continue;
                    Json polygon{{"vertices", vertices}};
                    const auto texture = c.Header("Texture");
                    if (!texture.empty() && Lower(texture) != "none") polygon["texture"] = texture;
                    double flags = 0;
                    if (ParseNumber(c.Header("Flags"), flags) && flags > 0 && flags <= 4294967295.0) polygon["flags"] = static_cast<long long>(flags);
                    // Mirrored scales turn the faces inside out; wind them back.
                    if (main[0] * main[1] * main[2] * post[0] * post[1] * post[2] < 0)
                    {
                        auto& v = polygon["vertices"];
                        std::reverse(v.begin(), v.end());
                    }
                    out.push_back(polygon);
                    ++polygons;
                }
            };
            walk(actor);
            return out;
        }
    }
    inline Json ImportT3d(const std::string& text, const Json& options, Json& report)
    {
        Report(report);
        const double scale = Option(options, "scale", 1.0, 1e-6, 1e6);
        Vector offset{0, 0, 0};
        if (options.is_object() && options.contains("offset"))
        {
            const auto& v = options.at("offset");
            if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) throw std::runtime_error("Option offset is [x, y, z].");
            offset = {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
        }
        const bool geometry = Option(options, "geometry", true), actorsWanted = Option(options, "actors", true);
        const auto properties = Option(options, "properties", "none");
        if (properties != "none" && properties != "all") throw std::runtime_error("Option properties is none or all.");
        const auto root = T3d::Parse(text);
        std::vector<const T3d::Block*> actors;
        T3d::Actors(root, actors);
        if (actors.empty()) throw std::runtime_error("The T3D has no Begin Actor blocks.");
        Json spec = NewSpec(options, "t3d");
        Section(spec, {"defaults"})["construction"] = "Carve";
        Ids ids;
        std::map<std::string, long long> left;
        long long polygons = 0, neutral = 0;
        bool sawBuilder = false;
        for (const auto* a : actors)
        {
            auto cls = a->Header("Class");
            const auto name = a->Header("Name");
            const auto shortClass = Lower(cls.substr(cls.find_last_of('.') == std::string::npos ? 0 : cls.find_last_of('.') + 1));
            const auto location = T3d::VectorOf(a->Get("Location"), {0, 0, 0});
            const Vector at{location[0] * scale + offset[0], location[1] * scale + offset[1], location[2] * scale + offset[2]};
            const auto rotation = T3d::RotationOf(a->Get("Rotation"));
            const double yaw = std::round(std::fmod(rotation[1] * 360.0 / 65536.0 + 720, 360) * 10) / 10;
            auto tag = T3d::Unquote(a->Get("Tag"));
            const auto id = ids.Make(!tag.empty() && Lower(tag) != "none" && Lower(tag) != shortClass ? tag : name, shortClass.empty() ? std::string("Actor") : Ids::Clean(shortClass));
            const auto csg = a->Get("CsgOper");
            bool hasPolygons = false;
            std::function<bool(const T3d::Block&)> anyPolygon = [&](const T3d::Block& b) { for (const auto& c : b.children) if (Lower(c.type) == "polygon" || anyPolygon(c)) return true; return false; };
            hasPolygons = anyPolygon(*a);
            if (shortClass == "levelinfo" || shortClass == "camera" || shortClass == "editorcamera") { left[shortClass]++; continue; }
            if (shortClass == "brush" || (hasPolygons && (Lower(csg) == "csg_add" || Lower(csg) == "csg_subtract")))
            {
                if (csg.empty() || Lower(csg) == "csg_active") { sawBuilder = true; continue; }
                if (Lower(csg) != "csg_add" && Lower(csg) != "csg_subtract") { left["intersect and deintersect brushes"]++; continue; }
                if (!geometry) { left["brushes (geometry false)"]++; continue; }
                size_t count = 0;
                auto polys = T3d::Polygons(*a, scale, count);
                if (polys.empty()) { left["brushes without polygons"]++; continue; }
                Json brush{{"id", id}, {"csg", Lower(csg) == "csg_add" ? "CSG_Add" : "CSG_Subtract"}, {"at", Point(at[0], at[1], at[2])}, {"polygons", polys}};
                double flags = 0;
                if (ParseNumber(a->Get("PolyFlags"), flags) && flags > 0 && flags <= 4294967295.0) brush["polyFlags"] = static_cast<long long>(flags);
                if (shortClass != "brush") brush["class"] = cls;
                List(spec, {"geometry", "brushes"}).push_back(brush);
                polygons += static_cast<long long>(count);
                Count(report, "brushes");
                continue;
            }
            if (hasPolygons && (EndsWith(shortClass, "volume") || shortClass == "zoneinfo"))
            {
                // A volume as the box around its polygons.
                size_t count = 0;
                const auto polys = T3d::Polygons(*a, scale, count);
                double lo[3] = {1e18, 1e18, 1e18}, hi[3] = {-1e18, -1e18, -1e18};
                for (const auto& p : polys) for (const auto& v : p.at("vertices")) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], v[k].get<double>() + at[static_cast<size_t>(k)]); hi[k] = std::max(hi[k], v[k].get<double>() + at[static_cast<size_t>(k)]); }
                if (polys.empty()) continue;
                Json volume{{"id", id}, {"class", cls}, {"box", {{"min", Point(lo[0], lo[1], lo[2])}, {"max", Point(hi[0], hi[1], hi[2])}}}};
                const auto locationName = a->Get("LocationName");
                if (!locationName.empty()) volume["locationName"] = T3d::Unquote(locationName);
                List(spec, {"environment", "volumes"}).push_back(volume);
                Count(report, "volumes");
                continue;
            }
            if (!actorsWanted) { left["actors (actors false)"]++; continue; }
            if (shortClass == "mover" || EndsWith(shortClass, "mover")) { left["movers"]++; continue; }
            if (shortClass == "playerstart")
            {
                const auto team = Trim(a->Get("TeamNumber"));
                const bool spy = team == "0" || (team.empty() && neutral++ % 2 == 0);
                Json start{{"id", id}, {"at", Point(at[0], at[1], at[2])}, {"yaw", Num(yaw)}};
                List(spec, {"spawns", spy ? "spy" : "merc"}).push_back(start);
                Count(report, spy ? "spyStarts" : "mercStarts");
                continue;
            }
            if (shortClass == "light" || shortClass == "spotlight" || shortClass == "striggerlight" || shortClass == "triggerlight" || shortClass == "sunlight")
            {
                const auto effect = Lower(a->Get("LightEffect"));
                Json light{{"id", id}, {"preset", effect == "le_spotlight" || shortClass == "spotlight" ? "spot" : effect == "le_sunlight" || shortClass == "sunlight" ? "sun" : "ceiling"},
                           {"at", Point(at[0], at[1], at[2])}};
                for (const auto& [key, field] : std::vector<std::pair<const char*, const char*>>{{"LightBrightness", "brightness"}, {"LightHue", "hue"}, {"LightSaturation", "saturation"}})
                {
                    double n = 0;
                    if (ParseNumber(a->Get(key), n)) light[field] = Num(std::clamp(n, 0.0, 255.0));
                }
                if (light.at("preset") != "ceiling") light["yaw"] = Num(yaw);
                if (shortClass == "striggerlight" || shortClass == "triggerlight") light["switchable"] = true;
                List(spec, {"environment", "lights"}).push_back(light);
                Count(report, "lights");
                continue;
            }
            const auto mesh = T3d::Unquote(a->Get("StaticMesh"));
            Json scaled;
            if (a->Has("DrawScale3D")) { const auto s = T3d::VectorOf(a->Get("DrawScale3D"), {1, 1, 1}); scaled = Json::array({Num(s[0]), Num(s[1]), Num(s[2])}); }
            else if (a->Has("DrawScale")) scaled = Num(NumberOr(a->Get("DrawScale"), 1));
            if (shortClass == "staticmeshactor" && !mesh.empty() && Lower(mesh) != "none")
            {
                Json item{{"id", id}, {"mesh", mesh}, {"at", Point(at[0], at[1], at[2])}, {"yaw", Num(yaw)}};
                if (!scaled.is_null()) item["scale"] = scaled;
                if (rotation[0] || rotation[2]) item["props"] = {{"Rotation", {{"Pitch", std::to_string(rotation[0])}, {"Yaw", std::to_string(rotation[1])}, {"Roll", std::to_string(rotation[2])}}}};
                List(spec, {"geometry", "meshes"}).push_back(item);
                Count(report, "meshes");
                continue;
            }
            Json item{{"id", id}, {"class", cls}, {"at", Point(at[0], at[1], at[2])}, {"yaw", Num(yaw)}};
            if (!mesh.empty() && Lower(mesh) != "none") item["mesh"] = mesh;
            if (!scaled.is_null()) item["scale"] = scaled;
            if (properties == "all")
            {
                Json values = Json::object();
                std::map<std::string, Json> arrays;
                static const std::set<std::string> skip{"location", "rotation", "name", "class", "level", "region", "tag", "brush", "staticmesh", "drawscale", "drawscale3d",
                                                        "prepivot", "mainscale", "postscale", "tempscale", "base", "touching", "physicsvolume", "zone", "bselected"};
                for (const auto& [key, value] : a->properties)
                {
                    const auto paren = key.find_first_of("([");
                    const auto base = paren == std::string::npos ? key : key.substr(0, paren);
                    if (skip.count(Lower(base))) continue;
                    if (paren == std::string::npos) { values[key] = T3d::Value(value); continue; }
                    const auto index = static_cast<size_t>(NumberOr(key.substr(paren + 1, key.size() - paren - 2), 0));
                    auto& list = arrays[base];
                    if (!list.is_array()) list = Json::array();
                    while (list.size() <= index && list.size() < 4096) list.push_back("");
                    if (index < list.size()) list[index] = T3d::Value(value);
                }
                for (auto& [key, list] : arrays) values[key] = list;
                if (!values.empty()) item["properties"] = values;
            }
            List(spec, {"environment", "props"}).push_back(item);
            Count(report, "actors");
        }
        if (sawBuilder) Assume(report, "The builder brush (a Brush with no CsgOper) was skipped.");
        for (const auto& [what, n] : left) Warn(report, std::to_string(n) + " " + what + " were left out.");
        if (polygons) Count(report, "polygons", polygons);
        if (neutral) Assume(report, "Player starts without a TeamNumber alternate between spies and mercs.");
        if (properties == "all") Warn(report, "Exported properties were copied as text; references to other actors (Class'MyLevel.Name') may not resolve in the new level.");
        if (scale != 1) Assume(report, "Positions and brushes are scaled by " + Num(scale).dump() + ".");
        spec["provenance"]["notes"] = report.at("assumptions");
        Prune(spec);
        return spec;
    }
}
