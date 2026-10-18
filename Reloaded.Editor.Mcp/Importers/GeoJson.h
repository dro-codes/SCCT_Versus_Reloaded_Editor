#pragma once
// GeoJSON (RFC 7946) floor plans to scct.level-spec. Engine-free.
//
// Polygons and MultiPolygons are outlines (rooms by default: a rectangle
// becomes a Map Design piece, any other shape convex polygon brushes, and a
// hole in a room a solid pillar); LineStrings are lines; Points are markers.
// A feature's properties give its kind (kind, type or category), id, name,
// storey (or level: a number, or a name that storeys are numbered by in
// order of appearance), and yaw, h, z, height, mesh, class, sound, radius,
// brightness, h0, h1 and prop:<Name> as in PlanShapes.h.
// Coordinates (options.units): "auto" reads longitude and latitude when
// every coordinate fits them and the extent is under a degree, else metres;
// "degrees", "m" (uuPerMetre, 100, each) or "uu". Degrees are projected
// equirectangularly about the centre. The plan is centred on the origin
// unless center is false. Storey, room and door options are GridPlanModel's.
#include "../../Reloaded.Editor/McpBridgeModel.h"
#include "PlanShapes.h"

namespace Mcp::Import
{
    inline Json ImportGeoJson(const std::string& text, const Json& options, Json& report)
    {
        Report(report);
        const auto o = Grid::ReadOptions(options);
        const auto doc = Bridge::Parse(text);
        // Features, whatever wraps them.
        std::vector<std::pair<Json, Json>> features; // geometry, properties
        std::function<void(const Json&, const Json&)> collect = [&](const Json& item, const Json& properties)
        {
            if (!item.is_object()) return;
            const auto type = item.value("type", std::string());
            if (type == "FeatureCollection") { for (const auto& f : item.value("features", Json::array())) collect(f, Json::object()); }
            else if (type == "Feature") { if (item.contains("geometry") && !item.at("geometry").is_null()) collect(item.at("geometry"), item.value("properties", Json::object()).is_object() ? item.value("properties", Json::object()) : Json::object()); }
            else if (type == "GeometryCollection") { for (const auto& g : item.value("geometries", Json::array())) collect(g, properties); }
            else if (!type.empty()) features.push_back({item, properties});
        };
        collect(doc, Json::object());
        if (features.empty()) throw std::runtime_error("The GeoJSON has no geometry (expected a FeatureCollection, Feature or geometry object).");
        // Every coordinate, to choose units and the projection centre.
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        std::function<void(const Json&)> scan = [&](const Json& c)
        {
            if (c.is_array() && c.size() >= 2 && c[0].is_number() && c[1].is_number())
            {
                x0 = std::min(x0, c[0].get<double>()); x1 = std::max(x1, c[0].get<double>());
                y0 = std::min(y0, c[1].get<double>()); y1 = std::max(y1, c[1].get<double>());
            }
            else if (c.is_array()) for (const auto& item : c) scan(item);
        };
        for (const auto& feature : features) scan(feature.first.value("coordinates", Json::array()));
        if (x0 > x1) throw std::runtime_error("The GeoJSON's geometries have no coordinates.");
        auto units = Lower(Option(options, "units", "auto"));
        const double uuPerMetre = Option(options, "uuPerMetre", 100.0, 1, 10000);
        if (units == "auto")
        {
            const bool lonLat = x0 >= -180 && x1 <= 180 && y0 >= -90 && y1 <= 90 && (x1 - x0) < 1 && (y1 - y0) < 1;
            units = lonLat ? "degrees" : "m";
            Assume(report, lonLat ? "The coordinates look like longitude and latitude; they are projected to metres about the plan's centre."
                                  : "The coordinates are read as metres (units \"uu\" reads them as Unreal units).");
        }
        if (units != "degrees" && units != "m" && units != "uu") throw std::runtime_error("Option units is auto, degrees, m or uu.");
        const double lon0 = (x0 + x1) / 2, lat0 = (y0 + y1) / 2;
        constexpr double rad = 3.14159265358979323846 / 180;
        auto world = [&](const Json& c) -> Grid::P2
        {
            const double x = c[0].get<double>(), y = c[1].get<double>();
            if (units == "degrees") return {(x - lon0) * 111320.0 * std::cos(lat0 * rad) * uuPerMetre, (y - lat0) * 110540.0 * uuPerMetre};
            if (units == "m") return {x * uuPerMetre, y * uuPerMetre};
            return {x, y};
        };
        if (units != "uu") Assume(report, "One metre is " + Num(uuPerMetre).dump() + " Unreal units.");
        Grid::Plan plan;
        plan.format = "geojson";
        // Storeys: numbers as given, names numbered in order of appearance.
        std::map<std::string, int> named;
        auto storeyOf = [&](const Json& properties) -> std::string
        {
            for (const char* key : {"storey", "level", "floor", "level_id"})
            {
                if (!properties.contains(key) || properties.at(key).is_null()) continue;
                const auto& v = properties.at(key);
                int number = 0;
                if (v.is_number()) number = static_cast<int>(std::lround(v.get<double>()));
                else
                {
                    const auto name = v.is_string() ? v.get<std::string>() : v.dump();
                    double n = 0;
                    if (ParseNumber(name, n)) number = static_cast<int>(std::lround(n));
                    else if (named.count(name)) number = named.at(name);
                    else { number = static_cast<int>(named.size()); named[name] = number; }
                }
                const auto id = Grid::StoreyId(number);
                if (!plan.Storey(id)) { plan.storeys.push_back({id, number, number * o.storeyHeight, v.is_string() ? v.get<std::string>() : std::string()}); plan.ids.Reserve(id); }
                return id;
            }
            const auto id = Grid::StoreyId(0);
            if (!plan.Storey(id)) { plan.storeys.push_back({id, 0, 0, ""}); plan.ids.Reserve(id); }
            return id;
        };
        long long count = 0, holes = 0;
        for (const auto& feature : features)
        {
            // Named references, not structured bindings: the lambdas below capture them.
            const Json& geometry = feature.first;
            const Json& properties = feature.second;
            const auto storey = storeyOf(properties);
            auto get = [&](const char* key) -> std::string
            {
                const std::string k = key;
                auto read = [&](const char* name) -> std::string
                {
                    if (!properties.contains(name) || properties.at(name).is_null()) return {};
                    const auto& v = properties.at(name);
                    return v.is_string() ? v.get<std::string>() : v.dump();
                };
                if (k == "kind") { auto v = read("kind"); if (v.empty()) v = read("type"); if (v.empty()) v = read("category"); return v; }
                return read(key);
            };
            auto a = Shapes::ReadAttributes(get);
            for (auto it = properties.begin(); it != properties.end(); ++it)
                if (StartsWith(it.key(), "prop:") && it.key().size() > 5) a.props[it.key().substr(5)] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            const auto type = geometry.value("type", std::string());
            const auto& coords = geometry.value("coordinates", Json::array());
            auto ring = [&](const Json& points)
            {
                std::vector<Grid::P2> out;
                for (const auto& p : points) if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number()) out.push_back(world(p));
                if (out.size() > 1 && std::hypot(out.front()[0] - out.back()[0], out.front()[1] - out.back()[1]) < 1e-6) out.pop_back();
                return out;
            };
            auto polygon = [&](const Json& rings)
            {
                if (!rings.is_array() || rings.empty()) return;
                Shapes::AddClosed(plan, o, ring(rings[0]), a, storey, report);
                // Holes in a room are solid: pillars and courtyards walled off.
                const auto kind = a.kind.empty() ? std::string("Room") : a.kind;
                for (size_t i = 1; i < rings.size(); ++i)
                {
                    if (!Shapes::SpaceKind(kind)) break;
                    Shapes::Attributes solid;
                    solid.kind = "solid";
                    solid.height = a.height;
                    solid.z = a.z;
                    solid.id = a.id.empty() ? std::string() : a.id + "_Hole" + std::to_string(i);
                    Shapes::AddClosed(plan, o, ring(rings[i]), solid, storey, report);
                    ++holes;
                }
            };
            ++count;
            if (type == "Polygon") polygon(coords);
            else if (type == "MultiPolygon") for (const auto& p : coords) polygon(p);
            else if (type == "LineString") Shapes::AddOpen(plan, o, ring(coords), a, storey, report);
            else if (type == "MultiLineString") for (const auto& l : coords) Shapes::AddOpen(plan, o, ring(l), a, storey, report);
            else if (type == "Point") { if (coords.is_array() && coords.size() >= 2) Shapes::AddPoint(plan, world(coords), a, storey, report); }
            else if (type == "MultiPoint") { for (const auto& p : coords) Shapes::AddPoint(plan, world(p), a, storey, report); }
            else { --count; Warn(report, "GeoJSON " + type + " geometries are not read."); }
        }
        if (holes) Assume(report, "Holes in rooms became solid pillars (CSG_Add prisms).");
        if (o.center && Shapes::Center(plan)) Assume(report, "The plan is centred on the origin (center false keeps the source coordinates).");
        Count(report, "features", count);
        Count(report, "storeys", static_cast<long long>(plan.storeys.size()));
        if (plan.spaces.empty() && plan.prisms.empty()) Warn(report, "The GeoJSON has no rooms; Polygon features are rooms unless their kind says otherwise.");
        return Grid::ToSpec(plan, o, options, report);
    }
}
