#pragma once
// A map-authoring export (scct.map-authoring: View > Export Map to JSON, the
// authoring.export op) as a scct.level-spec. Engine-free.
//
// mode "refs" (default): the map stays as it is. Every exported actor
// becomes an existing.refs entry ({path, class, tag}) named by its Tag, or
// its object name, so a spec that adds to the map can link to, wire up and
// edit what is there. report.sections counts what each actor would be in a
// spec (spawns, objectives, cameras, ...).
// mode "recreate": the map rebuilt as a spec, to port it to a new map: the
// geometry (geometryT3d's brushes and volumes, through T3d.h), player
// starts, objectives (terminals, flags with their nearest drop zones,
// bombs), disks, cameras, lasers, CamNet, alarms, lights, static meshes and
// ingredients in their sections, and every other actor in environment.props
// with its class, position and yaw (with options.values true, its exported
// values too, though references to other actors may not resolve). Missions,
// objectives' logic and magic events are left to the compiler, which writes
// its own.
#include "../../Reloaded.Editor/McpBridgeModel.h"
#include "T3d.h"

namespace Mcp::Import
{
    namespace AuthoringDetail
    {
        inline std::string ShortClass(const std::string& cls) { return Lower(cls.substr(cls.find_last_of('.') == std::string::npos ? 0 : cls.find_last_of('.') + 1)); }
        // What an actor class is in a spec.
        inline std::string SectionOf(const std::string& cls)
        {
            const auto c = ShortClass(cls);
            if (c == "playerstart") return "spawn";
            if (c == "scomputerobjectivetrigger") return "terminal";
            if (c == "sflagdropzone") return "dropzone";
            if (c == "sflag") return "flag";
            if (c == "sbombtargetobjectivetrigger") return "bomb";
            if (c == "sdisk") return "disk";
            if (c == "scamera") return "camera";
            if (c == "slasertrigger") return "laser";
            if (c == "scamnetwork") return "camnet";
            if (c == "salarm") return "alarm";
            if (c == "light" || c == "spotlight" || c == "striggerlight" || c == "sunlight") return "light";
            if (c == "staticmeshactor") return "mesh";
            if (c == "sammostock") return "ammo";
            if (c == "sinfirmary") return "infirmary";
            if (c == "smission" || c == "sobjective" || c == "smagicevent") return "logic";
            if (c == "brush" || EndsWith(c, "volume")) return "geometry";
            return "other";
        }
        inline std::string Text(const Json& values, const char* key)
        {
            if (!values.is_object()) return {};
            for (auto it = values.begin(); it != values.end(); ++it)
                if (Lower(it.key()) == Lower(key)) return it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            return {};
        }
        inline std::string Unquote(std::string v) { if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2); return v; }
    }
    inline Json ImportAuthoring(const std::string& text, const Json& options, Json& report)
    {
        using namespace AuthoringDetail;
        Report(report);
        const auto doc = Bridge::Parse(text);
        if (!doc.is_object() || doc.value("format", std::string()) != "scct.map-authoring") throw std::runtime_error("This is not a map-authoring export (format scct.map-authoring).");
        const auto mode = Option(options, "mode", "refs");
        if (mode != "refs" && mode != "recreate") throw std::runtime_error("Option mode is refs or recreate.");
        const bool withValues = Option(options, "values", false);
        Json named = options.is_object() ? options : Json::object();
        if (!named.contains("name"))
        {
            const auto key = doc.value("map", std::string());
            named["name"] = StartsWith(key, "unsaved:") ? std::string("UNSAVED") : Stem(key);
        }
        Json spec = NewSpec(named, "map-authoring");
        spec["provenance"]["source"]["map"] = doc.value("map", std::string());
        Ids ids;
        Json sections = Json::object();
        struct Placed { std::string id, section, cls; Vector at; double yaw; Json values; };
        std::vector<Placed> placed;
        long long components = 0;
        for (const auto& item : doc.value("actors", Json::array()))
        {
            if (!item.is_object() || !item.contains("actor") || !item.contains("position")) { ++components; continue; }
            const auto& identity = item.at("actor");
            const auto path = identity.value("path", std::string()), cls = identity.value("class", std::string());
            const auto values = item.value("values", Json::object());
            const auto name = path.substr(path.find_last_of('.') == std::string::npos ? 0 : path.find_last_of('.') + 1);
            const auto tag = Text(values, "Tag");
            const auto shortClass = ShortClass(cls);
            const bool useTag = !tag.empty() && Lower(tag) != "none" && Lower(tag) != shortClass;
            const auto id = ids.Make(useTag ? tag : name, "Actor");
            const auto section = SectionOf(cls);
            sections[section] = sections.value(section, 0) + 1;
            Vector at{0, 0, 0};
            const auto& position = item.at("position");
            if (position.is_array() && position.size() == 3) for (size_t k = 0; k < 3; ++k) at[k] = position[k].is_number() ? position[k].get<double>() : 0.0;
            double yaw = 0;
            if (item.contains("rotation") && item.at("rotation").is_array() && item.at("rotation").size() == 3 && item.at("rotation")[1].is_number())
                yaw = std::round(std::fmod(item.at("rotation")[1].get<double>() * 360.0 / 65536.0 + 720, 360) * 10) / 10;
            if (mode == "refs")
            {
                Json ref{{"path", path}, {"class", cls}};
                if (!tag.empty() && Lower(tag) != "none") ref["tag"] = tag;
                Section(spec, {"existing", "refs"})[id] = ref;
                continue;
            }
            placed.push_back({id, section, cls, at, yaw, values});
        }
        report["sections"] = sections;
        if (components) Count(report, "components", components);
        if (mode == "refs")
        {
            Count(report, "refs", static_cast<long long>(spec.contains("existing") ? spec.at("existing").at("refs").size() : 0));
            Assume(report, "Every exported actor is an existing.refs entry named by its Tag (or its object name when the Tag is the class's), so new spec items can link to it; nothing existing is rebuilt.");
            spec["provenance"]["notes"] = report.at("assumptions");
            return spec;
        }
        // Recreate: geometry first, from the export's T3D.
        if (doc.contains("geometryT3d") && doc.at("geometryT3d").is_string() && !doc.at("geometryT3d").get<std::string>().empty())
        {
            Json geometryReport = Json::object();
            const auto geometry = ImportT3d(doc.at("geometryT3d").get<std::string>(), {{"actors", false}}, geometryReport);
            for (const char* key : {"brushes"})
                if (geometry.contains("geometry") && geometry.at("geometry").contains(key))
                    for (const auto& b : geometry.at("geometry").at(key)) { auto copy = b; copy["id"] = ids.Make(b.at("id").get<std::string>(), "Brush"); List(spec, {"geometry", key}).push_back(copy); }
            if (geometry.contains("environment") && geometry.at("environment").contains("volumes"))
                for (const auto& v : geometry.at("environment").at("volumes")) { auto copy = v; copy["id"] = ids.Make(v.at("id").get<std::string>(), "Volume"); List(spec, {"environment", "volumes"}).push_back(copy); }
            Count(report, "brushes", geometryReport.at("counts").value("brushes", 0LL));
            Count(report, "volumes", geometryReport.at("counts").value("volumes", 0LL));
        }
        else Warn(report, "The export has no geometryT3d (" + doc.value("geometryUnavailable", std::string("not exported")) + "), so the spec has no geometry.");
        Section(spec, {"defaults"})["construction"] = "Carve";
        auto point = [](const Vector& v) { return Point(v[0], v[1], v[2]); };
        // Flags first, so drop zones join the nearest.
        std::map<std::string, Json> flagZones;
        for (const auto& p : placed)
        {
            if (p.section != "dropzone") continue;
            const Placed* best = nullptr;
            double bestD = 1e18;
            for (const auto& f : placed)
                if (f.section == "flag")
                {
                    const double d = std::hypot(f.at[0] - p.at[0], f.at[1] - p.at[1]) + std::abs(f.at[2] - p.at[2]) * 4;
                    if (d < bestD) { bestD = d; best = &f; }
                }
            if (!best) { Warn(report, "Flag drop zones without any flag were left out."); continue; }
            auto& zones = flagZones[best->id];
            if (!zones.is_array()) zones = Json::array();
            if (zones.size() < 5) zones.push_back({{"id", p.id}, {"at", point(p.at)}});
        }
        int disks = 0, camnets = 0, objectives = 0;
        long long others = 0, logic = 0;
        for (const auto& p : placed)
        {
            const auto& s = p.section;
            if (s == "spawn")
            {
                const bool spy = Text(p.values, "TeamNumber") == "0";
                List(spec, {"spawns", spy ? "spy" : "merc"}).push_back({{"id", p.id}, {"at", point(p.at)}, {"yaw", Num(p.yaw)}});
                Count(report, spy ? "spyStarts" : "mercStarts");
            }
            else if (s == "terminal" || s == "flag" || s == "bomb")
            {
                Json trigger{{"id", p.id}, {"kind", s}, {"at", point(p.at)}, {"yaw", Num(p.yaw)}};
                const auto triggerName = Unquote(Text(p.values, "TriggerName"));
                if (!triggerName.empty()) trigger["triggerName"] = triggerName;
                double n = 0;
                if (s == "terminal" && ParseNumber(Text(p.values, "DominationCaptureTime"), n)) trigger["dominationCaptureTime"] = Num(n);
                if (s == "bomb" && ParseNumber(Text(p.values, "CaptureTime"), n)) trigger["captureTime"] = Num(n);
                if (s == "flag")
                {
                    if (ParseNumber(Text(p.values, "ResetTime"), n)) trigger["resetTime"] = Num(n);
                    if (flagZones.count(p.id)) trigger["dropZones"] = flagZones.at(p.id);
                }
                List(spec, {"gameplay", "story", "objectives"}).push_back({{"id", ids.Next("Objective")}, {"name", triggerName.empty() ? p.id : triggerName}, {"triggers", Json::array({trigger})}});
                ++objectives;
            }
            else if (s == "disk")
            {
                double group = 0;
                const int g = ParseNumber(Text(p.values, "Group"), group) && group >= 1 && group <= 3 ? static_cast<int>(group) : disks % 3 + 1;
                List(spec, {"gameplay", "diskhunt", "disks"}).push_back({{"id", p.id}, {"at", point(p.at)}, {"group", g}});
                ++disks;
            }
            else if (s == "camera" || s == "laser")
                List(spec, {"security", "devices"}).push_back({{"id", p.id}, {"kind", s}, {"at", point(p.at)}, {"facing", Num(p.yaw)}});
            else if (s == "camnet")
            {
                List(spec, {"security", "camnet"}).push_back({{"id", p.id}, {"at", point(p.at)}, {"yaw", Num(p.yaw)}, {"first", Lower(Text(p.values, "bFirstCam")) == "true"}});
                ++camnets;
            }
            else if (s == "alarm") List(spec, {"security", "alarms"}).push_back({{"id", p.id}, {"at", point(p.at)}});
            else if (s == "light")
            {
                const auto effect = Lower(Text(p.values, "LightEffect"));
                Json light{{"id", p.id}, {"preset", effect == "le_spotlight" ? "spot" : effect == "le_sunlight" ? "sun" : "ceiling"}, {"at", point(p.at)}};
                for (const auto& [key, field] : std::vector<std::pair<const char*, const char*>>{{"LightBrightness", "brightness"}, {"LightHue", "hue"}, {"LightSaturation", "saturation"}})
                {
                    double n = 0;
                    if (ParseNumber(Text(p.values, key), n)) light[field] = Num(std::clamp(n, 0.0, 255.0));
                }
                if (ShortClass(p.cls) == "striggerlight") light["switchable"] = true;
                List(spec, {"environment", "lights"}).push_back(light);
            }
            else if (s == "mesh")
            {
                const auto mesh = Text(p.values, "StaticMesh");
                if (mesh.empty() || Lower(mesh) == "none") continue;
                List(spec, {"geometry", "meshes"}).push_back({{"id", p.id}, {"mesh", mesh}, {"at", point(p.at)}, {"yaw", Num(p.yaw)}});
            }
            else if (s == "ammo" || s == "infirmary") List(spec, {"environment", "ingredients"}).push_back({{"id", p.id}, {"kind", s}, {"at", point(p.at)}, {"yaw", Num(p.yaw)}});
            else if (s == "logic") ++logic;
            else if (s == "other")
            {
                Json item{{"id", p.id}, {"class", p.cls}, {"at", point(p.at)}, {"yaw", Num(p.yaw)}};
                if (withValues)
                {
                    Json values = p.values;
                    for (const char* key : {"Location", "Rotation", "Tag"}) values.erase(key);
                    if (!values.empty()) item["properties"] = values;
                }
                List(spec, {"environment", "props"}).push_back(item);
                ++others;
            }
        }
        Json modes = Json::array();
        if (objectives) modes.push_back("story");
        if (disks) modes.push_back("diskhunt");
        if (!modes.empty()) Section(spec, {"gameplay"})["modes"] = modes;
        if (camnets && !std::any_of(placed.begin(), placed.end(), [](const Placed& p) { return p.section == "camnet" && Lower(Text(p.values, "bFirstCam")) == "true"; }))
            Warn(report, "No CamNet camera was bFirstCam; mark one first in the spec.");
        if (logic) Warn(report, std::to_string(logic) + " missions, objectives' logic and magic events were left out; the compiler writes the missions for the objectives it finds.");
        if (others) Assume(report, std::to_string(others) + " other actors are kept as environment.props with their class, position and yaw" + (withValues ? " and exported values." : " (values true copies their exported values)."));
        Assume(report, "The map is recreated as a spec for a new map; building it into the same map would duplicate every actor.");
        spec["provenance"]["notes"] = report.at("assumptions");
        Prune(spec);
        return spec;
    }
}
