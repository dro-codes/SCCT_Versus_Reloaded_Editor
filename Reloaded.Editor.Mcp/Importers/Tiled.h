#pragma once
// Tiled maps (.tmj JSON or .tmx XML, orthogonal) to scct.level-spec.
// Engine-free; base64, zlib and gzip layer data are decoded here, zstd is not.
//
// Tile layers become the grid of GridPlanModel.h: each tile means a legend
// entry ("floor", "solid", "spy", "door", ...), taken from options.tiles
// {"<gid>": meaning}, else the tile's kind/legend property or its type
// (class), else the layer's kind property or name ("Walls", "Floor"), else
// floor for any tile and solid for none. Later layers draw over earlier
// ones, except that floor never covers a marker. Each tile is options.cell
// (128) units. A layer's (or its group's) storey property, or a name such
// as "storey 1", picks the storey.
// Object layers add what PlanShapes.h makes of an object: its kind is its
// type or class, a kind property, or its name; rectangles and polygons are
// outlines, polylines are lines, points and ellipses are points. Properties
// name, yaw, h, z, height, mesh, class, sound, radius, brightness, h0, h1
// and prop:<Name> work as in SVG. Objects use the tile grid's frame, so a
// marker drawn on a tile lands in that tile's room.
#include "../../Reloaded.Editor/McpBridgeModel.h"
#include "../ImageCodec.h"
#include "PlanShapes.h"
#include "Xml.h"
#include <climits>

namespace Mcp::Import
{
    namespace TiledDetail
    {
        inline std::string PropertyOf(const Json& item, const std::string& name)
        {
            if (!item.is_object() || !item.contains("properties")) return {};
            const auto& props = item.at("properties");
            auto text = [](const Json& v) { return v.is_string() ? v.get<std::string>() : v.is_null() ? std::string() : v.dump(); };
            if (props.is_array())
            {
                for (const auto& p : props)
                    if (p.is_object() && Lower(p.value("name", std::string())) == Lower(name)) return p.contains("value") ? text(p.at("value")) : std::string();
            }
            else if (props.is_object())
                for (auto it = props.begin(); it != props.end(); ++it) if (Lower(it.key()) == Lower(name)) return text(it.value());
            return {};
        }
        // Layer data as global tile ids.
        inline std::vector<uint32_t> Gids(const Json& data, const std::string& encoding, const std::string& compression, size_t expected)
        {
            std::vector<uint32_t> out;
            if (data.is_array())
            {
                for (const auto& v : data) out.push_back(v.is_number() ? static_cast<uint32_t>(v.get<double>()) : 0u);
                return out;
            }
            if (!data.is_string()) throw std::runtime_error("A Tiled layer's data is neither an array nor text.");
            const auto text = data.get<std::string>();
            if (encoding == "csv")
            {
                for (double n : Numbers(text)) out.push_back(static_cast<uint32_t>(n));
                return out;
            }
            if (encoding != "base64") throw std::runtime_error("Tiled layer encoding \"" + encoding + "\" is not supported; save the map with CSV or Base64 layer data.");
            std::string bytes = Bridge::Unbase64(text);
            const size_t limit = std::max<size_t>(expected * 4, 16);
            if (compression == "zlib") bytes = Image::ZlibDecompress(bytes, limit);
            else if (compression == "gzip") bytes = Image::GzipDecompress(bytes, limit);
            else if (!compression.empty()) throw std::runtime_error("Tiled layer compression \"" + compression + "\" is not supported; save the map with zlib, gzip or no compression.");
            for (size_t i = 0; i + 3 < bytes.size(); i += 4)
                out.push_back(static_cast<uint32_t>(static_cast<uint8_t>(bytes[i])) | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[i + 1])) << 8) |
                              (static_cast<uint32_t>(static_cast<uint8_t>(bytes[i + 2])) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[i + 3])) << 24));
            return out;
        }
        // A TMX file as the TMJ structure, so one reader handles both.
        inline Json Properties(const Xml::Node& node)
        {
            Json props = Json::array();
            for (const auto& c : node.children)
                if (c.Local() == "properties")
                    for (const auto& p : c.children)
                        if (p.Local() == "property") props.push_back({{"name", p.Get("name")}, {"type", p.Get("type", "string")}, {"value", p.Has("value") ? p.Get("value") : Trim(p.AllText())}});
            return props;
        }
        inline double Attr(const Xml::Node& n, const char* key, double fallback = 0) { return NumberOr(n.Get(key), fallback); }
        inline Json FromTmx(const Xml::Node& map)
        {
            Json out{{"orientation", map.Get("orientation", "orthogonal")}, {"width", Attr(map, "width")}, {"height", Attr(map, "height")},
                     {"tilewidth", Attr(map, "tilewidth", 32)}, {"tileheight", Attr(map, "tileheight", 32)}, {"infinite", map.Get("infinite") == "1"},
                     {"tilesets", Json::array()}, {"layers", Json::array()}, {"properties", Properties(map)}};
            std::function<Json(const Xml::Node&)> layer = [&](const Xml::Node& n) -> Json
            {
                const auto tag = n.Local();
                Json l{{"name", n.Get("name")}, {"properties", Properties(n)}, {"visible", n.Get("visible", "1") != "0"}};
                if (tag == "layer")
                {
                    l["type"] = "tilelayer";
                    l["width"] = Attr(n, "width");
                    l["height"] = Attr(n, "height");
                    for (const auto& d : n.children)
                    {
                        if (d.Local() != "data") continue;
                        const auto encoding = d.Get("encoding"), compression = d.Get("compression");
                        auto decode = [&](const Xml::Node& holder, size_t expected)
                        {
                            if (encoding.empty())
                            {
                                Json gids = Json::array();
                                for (const auto& t : holder.children) if (t.Local() == "tile") gids.push_back(Attr(t, "gid"));
                                return gids;
                            }
                            return Json(Gids(Json(Trim(holder.text)), encoding, compression, expected));
                        };
                        bool chunked = false;
                        for (const auto& c : d.children)
                            if (c.Local() == "chunk")
                            {
                                chunked = true;
                                const auto w = static_cast<size_t>(Attr(c, "width")), h = static_cast<size_t>(Attr(c, "height"));
                                l["chunks"].push_back({{"x", Attr(c, "x")}, {"y", Attr(c, "y")}, {"width", w}, {"height", h}, {"data", decode(c, w * h)}});
                            }
                        if (!chunked) l["data"] = decode(d, static_cast<size_t>(Attr(n, "width") * Attr(n, "height")));
                    }
                }
                else if (tag == "objectgroup")
                {
                    l["type"] = "objectgroup";
                    l["objects"] = Json::array();
                    for (const auto& o : n.children)
                    {
                        if (o.Local() != "object") continue;
                        Json obj{{"id", Attr(o, "id")}, {"name", o.Get("name")}, {"type", o.Get("type", o.Get("class"))}, {"x", Attr(o, "x")}, {"y", Attr(o, "y")},
                                 {"width", Attr(o, "width")}, {"height", Attr(o, "height")}, {"rotation", Attr(o, "rotation")}, {"properties", Properties(o)}};
                        if (o.Has("gid")) obj["gid"] = Attr(o, "gid");
                        for (const auto& shape : o.children)
                        {
                            const auto s = shape.Local();
                            if (s == "point") obj["point"] = true;
                            else if (s == "ellipse") obj["ellipse"] = true;
                            else if (s == "polygon" || s == "polyline")
                            {
                                Json points = Json::array();
                                const auto v = Numbers(shape.Get("points"));
                                for (size_t i = 0; i + 1 < v.size(); i += 2) points.push_back({{"x", v[i]}, {"y", v[i + 1]}});
                                obj[s] = points;
                            }
                        }
                        l["objects"].push_back(obj);
                    }
                }
                else if (tag == "group")
                {
                    l["type"] = "group";
                    l["layers"] = Json::array();
                    for (const auto& c : n.children) if (c.Local() == "layer" || c.Local() == "objectgroup" || c.Local() == "group") l["layers"].push_back(layer(c));
                }
                return l;
            };
            for (const auto& c : map.children)
            {
                const auto tag = c.Local();
                if (tag == "tileset")
                {
                    Json ts{{"firstgid", Attr(c, "firstgid", 1)}, {"name", c.Get("name")}, {"tiles", Json::array()}, {"properties", Properties(c)}};
                    if (c.Has("source")) ts["source"] = c.Get("source");
                    for (const auto& t : c.children)
                        if (t.Local() == "tile") ts["tiles"].push_back({{"id", Attr(t, "id")}, {"type", t.Get("type", t.Get("class"))}, {"properties", Properties(t)}});
                    out["tilesets"].push_back(ts);
                }
                else if (tag == "layer" || tag == "objectgroup" || tag == "group") out["layers"].push_back(layer(c));
            }
            return out;
        }
    }
    inline Json ImportTiled(const std::string& text, const Json& options, Json& report)
    {
        using namespace TiledDetail;
        Report(report);
        const auto o = Grid::ReadOptions(options);
        Json map;
        const auto trimmed = Trim(StripBom(text));
        if (!trimmed.empty() && trimmed[0] == '<')
        {
            const auto root = Xml::Parse(text);
            if (root.Local() != "map") throw std::runtime_error("The XML's root is <" + root.name + ">, not a Tiled <map>.");
            map = FromTmx(root);
        }
        else map = Bridge::Parse(text);
        if (!map.is_object() || !map.contains("layers")) throw std::runtime_error("This is not a Tiled map (it has no layers).");
        if (map.value("orientation", std::string("orthogonal")) != "orthogonal") Warn(report, "Only orthogonal Tiled maps are read as a grid; this one is " + map.value("orientation", std::string()) + ", so its layout will be skewed.");
        const double tw = std::max(1.0, map.value("tilewidth", 32.0)), th = std::max(1.0, map.value("tileheight", 32.0));
        // Tile meanings.
        struct Tileset { uint32_t first = 1; std::map<uint32_t, char> codes; };
        std::vector<Tileset> tilesets;
        for (const auto& ts : map.value("tilesets", Json::array()))
        {
            Tileset t;
            t.first = static_cast<uint32_t>(ts.value("firstgid", 1.0));
            if (ts.contains("source")) Warn(report, "External tileset " + ts.at("source").get<std::string>() + " cannot be read from here; its tiles count as floor unless options.tiles says otherwise.");
            for (const auto& tile : ts.value("tiles", Json::array()))
            {
                std::string meaning;
                for (const char* key : {"kind", "legend", "meaning", "char"}) if (meaning.empty()) meaning = PropertyOf(tile, key);
                if (meaning.empty()) meaning = tile.value("type", tile.value("class", std::string()));
                const char code = meaning.empty() ? '\0' : Grid::CodeOf(meaning);
                if (code) t.codes[static_cast<uint32_t>(tile.value("id", 0.0))] = code;
                else if (!meaning.empty()) Warn(report, "Tile meaning \"" + meaning + "\" is unknown and was read as floor.");
            }
            tilesets.push_back(t);
        }
        std::sort(tilesets.begin(), tilesets.end(), [](const Tileset& a, const Tileset& b) { return a.first < b.first; });
        std::map<uint32_t, char> custom;
        if (options.is_object() && options.contains("tiles"))
        {
            if (!options.at("tiles").is_object()) throw std::runtime_error("Option tiles maps tile ids to meanings, e.g. {\"3\": \"floor\"}.");
            for (auto it = options.at("tiles").begin(); it != options.at("tiles").end(); ++it)
            {
                const char code = it.value().is_string() ? Grid::CodeOf(it.value().get<std::string>()) : '\0';
                if (!code) throw std::runtime_error("Option tiles: \"" + it.value().dump() + "\" is not a legend meaning.");
                custom[static_cast<uint32_t>(NumberOr(it.key(), 0))] = code;
            }
        }
        auto codeOf = [&](uint32_t raw, char layerCode) -> char
        {
            const uint32_t gid = raw & 0x0FFFFFFFu; // flip flags off
            if (!gid) return '\0';
            if (custom.count(gid)) return custom.at(gid);
            if (layerCode) return layerCode;
            for (auto it = tilesets.rbegin(); it != tilesets.rend(); ++it)
                if (gid >= it->first)
                {
                    const auto found = it->codes.find(gid - it->first);
                    return found == it->codes.end() ? '.' : found->second;
                }
            return '.';
        };
        // Flatten groups, carrying their storey and visibility.
        struct Flat { const Json* layer; int storey; };
        std::vector<Flat> flat;
        std::function<void(const Json&, std::optional<int>)> walk = [&](const Json& layers, std::optional<int> storey)
        {
            for (const auto& l : layers)
            {
                if (!l.value("visible", true)) continue;
                std::optional<int> s = storey;
                const auto property = PropertyOf(l, "storey").empty() ? PropertyOf(l, "floor") : PropertyOf(l, "storey");
                double n = 0;
                if (ParseNumber(property, n)) s = static_cast<int>(n);
                else if (const auto named = Shapes::StoreyNumber(l.value("name", std::string()))) s = *named;
                // Pointers into the map are kept, so walk the group's own array, never a copy.
                if (l.value("type", std::string()) == "group") { if (l.contains("layers") && l.at("layers").is_array()) walk(l.at("layers"), s); }
                else flat.push_back({&l, s.value_or(0)});
            }
        };
        walk(map.at("layers"), std::nullopt);
        // The tile grid of each storey.
        long long minX = 0, minY = 0, maxX = static_cast<long long>(map.value("width", 0.0)), maxY = static_cast<long long>(map.value("height", 0.0));
        const bool infinite = map.value("infinite", false);
        if (infinite)
        {
            minX = minY = LLONG_MAX; maxX = maxY = LLONG_MIN;
            for (const auto& f : flat)
                for (const auto& c : f.layer->value("chunks", Json::array()))
                {
                    const auto x = static_cast<long long>(c.value("x", 0.0)), y = static_cast<long long>(c.value("y", 0.0));
                    minX = std::min(minX, x); minY = std::min(minY, y);
                    maxX = std::max(maxX, x + static_cast<long long>(c.value("width", 0.0))); maxY = std::max(maxY, y + static_cast<long long>(c.value("height", 0.0)));
                }
            if (minX > maxX) minX = minY = maxX = maxY = 0;
        }
        const long long W = maxX - minX, H = maxY - minY;
        if (W > 1024 || H > 1024) throw std::runtime_error("The Tiled map is " + std::to_string(W) + "x" + std::to_string(H) + " tiles; 1024 a side is the most.");
        std::map<int, std::vector<std::string>> grids;
        long long tileLayers = 0;
        for (const auto& f : flat)
        {
            const auto& l = *f.layer;
            if (l.value("type", std::string()) != "tilelayer") continue;
            ++tileLayers;
            auto& g = grids[f.storey];
            if (g.empty()) g.assign(static_cast<size_t>(H), std::string(static_cast<size_t>(W), '#'));
            std::string layerKind = PropertyOf(l, "kind");
            if (layerKind.empty()) layerKind = PropertyOf(l, "legend");
            char layerCode = layerKind.empty() ? '\0' : Grid::CodeOf(layerKind);
            if (!layerCode && layerKind.empty()) layerCode = Grid::CodeOf(l.value("name", std::string()));
            if (layerCode == '\0' && l.value("name", std::string()).size() > 1)
            {
                auto name = Lower(l.value("name", std::string()));
                if (name.size() > 3 && name.back() == 's') layerCode = Grid::CodeOf(name.substr(0, name.size() - 1));
            }
            auto paint = [&](long long x, long long y, uint32_t raw)
            {
                const char code = codeOf(raw, layerCode);
                if (!code || x < minX || y < minY || x >= maxX || y >= maxY) return;
                char& cell = g[static_cast<size_t>(y - minY)][static_cast<size_t>(x - minX)];
                if (code == '.' && Grid::MarkerCode(cell)) return;
                cell = code;
            };
            const auto encoding = l.value("encoding", std::string("csv")), compression = l.value("compression", std::string());
            if (l.contains("chunks"))
                for (const auto& c : l.at("chunks"))
                {
                    const auto cw = static_cast<long long>(c.value("width", 0.0)), cx = static_cast<long long>(c.value("x", 0.0)), cy = static_cast<long long>(c.value("y", 0.0));
                    const auto gids = Gids(c.at("data"), encoding, compression, static_cast<size_t>(cw * static_cast<long long>(c.value("height", 0.0))));
                    for (size_t i = 0; i < gids.size(); ++i) paint(cx + static_cast<long long>(i) % std::max(1LL, cw), cy + static_cast<long long>(i) / std::max(1LL, cw), gids[i]);
                }
            else if (l.contains("data"))
            {
                const auto lw = static_cast<long long>(l.value("width", static_cast<double>(W))), lx = static_cast<long long>(l.value("x", 0.0)), ly = static_cast<long long>(l.value("y", 0.0));
                const auto gids = Gids(l.at("data"), encoding, compression, static_cast<size_t>(lw * static_cast<long long>(l.value("height", static_cast<double>(H)))));
                for (size_t i = 0; i < gids.size(); ++i) paint(lx + static_cast<long long>(i) % std::max(1LL, lw), ly + static_cast<long long>(i) / std::max(1LL, lw), gids[i]);
            }
        }
        Grid::Plan plan;
        Grid::P2 corner{0, 0};
        if (!grids.empty())
        {
            std::vector<Grid::Layer> layers;
            for (const auto& [number, rows] : grids) layers.push_back({number, rows});
            plan = Grid::FromLayers(layers, o, report);
            corner = o.center ? Grid::P2{-static_cast<double>(W) * o.cell / 2, static_cast<double>(H) * o.cell / 2} : o.origin;
        }
        plan.format = "tiled";
        // Objects, in the tile grid's frame.
        auto world = [&](double px, double py) { return Grid::P2{corner[0] + (px / tw - static_cast<double>(minX)) * o.cell, corner[1] - (py / th - static_cast<double>(minY)) * o.cell}; };
        long long objects = 0;
        for (const auto& f : flat)
        {
            const auto& l = *f.layer;
            if (l.value("type", std::string()) != "objectgroup") continue;
            const auto storey = Grid::StoreyId(f.storey);
            if (!plan.Storey(storey)) { plan.storeys.push_back({storey, f.storey, f.storey * o.storeyHeight, ""}); plan.ids.Reserve(storey); }
            const auto layerKind = PropertyOf(l, "kind");
            for (const auto& obj : l.value("objects", Json::array()))
            {
                if (!obj.value("visible", true)) continue;
                ++objects;
                auto get = [&](const char* key) -> std::string
                {
                    const std::string k = key;
                    if (k == "kind")
                    {
                        auto kind = PropertyOf(obj, "kind");
                        if (kind.empty()) kind = obj.value("type", obj.value("class", std::string()));
                        if (kind.empty()) kind = layerKind;
                        return kind;
                    }
                    if (k == "id") { const auto id = PropertyOf(obj, "id"); return id.empty() ? obj.value("name", std::string()) : id; }
                    if (k == "name") return obj.value("name", std::string());
                    return PropertyOf(obj, key);
                };
                auto a = Shapes::ReadAttributes(get);
                // A tile object means what its tile means.
                if (a.kind.empty() && obj.contains("gid") && obj.at("gid").is_number())
                    if (const char code = codeOf(static_cast<uint32_t>(obj.at("gid").get<double>()), '\0')) { a.kind = Grid::Detail::KindOf(Grid::NameOf(code)); a.explicitKind = !a.kind.empty(); }
                if (a.kind.empty()) { a.kind = Shapes::KindWord(obj.value("name", std::string())); a.explicitKind = !a.kind.empty(); }
                if (obj.contains("properties") && obj.at("properties").is_array())
                    for (const auto& p : obj.at("properties"))
                    {
                        const auto name = p.value("name", std::string());
                        if (StartsWith(name, "prop:") && name.size() > 5) a.props[name.substr(5)] = p.contains("value") ? (p.at("value").is_string() ? p.at("value").get<std::string>() : p.at("value").dump()) : std::string();
                    }
                const double x = obj.value("x", 0.0), y = obj.value("y", 0.0), w = obj.value("width", 0.0), h = obj.value("height", 0.0);
                if (std::abs(obj.value("rotation", 0.0)) > 0.01) Warn(report, "Rotated Tiled objects were read unrotated.");
                if (obj.value("point", false) || ((w == 0 || h == 0) && !obj.contains("polygon") && !obj.contains("polyline")))
                    Shapes::AddPoint(plan, world(x, y), a, storey, report);
                else if (obj.contains("polygon") || obj.contains("polyline"))
                {
                    std::vector<Grid::P2> points;
                    for (const auto& p : obj.contains("polygon") ? obj.at("polygon") : obj.at("polyline")) points.push_back(world(x + p.value("x", 0.0), y + p.value("y", 0.0)));
                    if (obj.contains("polygon")) Shapes::AddClosed(plan, o, points, a, storey, report);
                    else Shapes::AddOpen(plan, o, points, a, storey, report);
                }
                else if (obj.value("ellipse", false) || obj.contains("gid"))
                {
                    // A tile object's position is its bottom-left corner.
                    const double cy = obj.contains("gid") ? y - h / 2 : y + h / 2;
                    if (!a.kind.empty() && !Shapes::MarkerKind(a.kind)) Shapes::AddClosed(plan, o, {world(x, cy - h / 2), world(x + w, cy - h / 2), world(x + w, cy + h / 2), world(x, cy + h / 2)}, a, storey, report);
                    else Shapes::AddPoint(plan, world(x + w / 2, cy), a, storey, report);
                }
                else Shapes::AddClosed(plan, o, {world(x, y), world(x + w, y), world(x + w, y + h), world(x, y + h)}, a, storey, report);
            }
        }
        if (grids.empty() && o.center) Shapes::Center(plan);
        Count(report, "tileLayers", tileLayers);
        Count(report, "objects", objects);
        Count(report, "storeys", static_cast<long long>(plan.storeys.size()));
        if (tileLayers) Assume(report, "Each Tiled tile is " + Num(o.cell).dump() + " units; tiles without a meaning are floor and empty tiles solid.");
        if (plan.spaces.empty() && plan.prisms.empty()) Warn(report, "The Tiled map has no floor; paint floor tiles or draw room objects.");
        return Grid::ToSpec(plan, o, options, report);
    }
}
