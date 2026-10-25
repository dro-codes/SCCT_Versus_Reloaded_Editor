// The intent tools, one per part of a level: blockout, gameplay, security,
// mechanisms, events, environment, geometric_events and map_settings. Each
// takes arguments shaped like its part of a scct.level-spec, lays them over
// the spec the map was last built from (the sidecar; a map without one starts
// a new spec) and builds the result incrementally through level_build's path
// (SpecTools.cpp): the same wiki defaults, checks and wiring, and one Undo
// step per call.
#include "Common.h"
#include "../LevelSpecModel.h"

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;
    using Workflow::Fold;

    // One argument and where it goes in the spec. A list is merged item by
    // item, matched on key ("id", another field, "a+b" for a pair, "" to only
    // add); an object is merged field by field. place false: the tool's
    // adjust step handles it.
    struct Field
    {
        std::string name;
        std::vector<std::string> path;
        std::string note;   // the schema's line, when the description's field list does not say it
        std::string key = "id";
        bool place = true, list = false;
    };
    using Adjust = std::function<void(ToolContext&, const Json& args, Json& spec, const Json& before, Json& notes, Json& post)>;
    struct Intent
    {
        std::string name, title, description;
        std::vector<Field> fields;
        std::vector<std::string> sections, rules; // where its items live; its rule families
        Json extra = Json::object();              // more input properties
        bool brushMode = false;
        Adjust adjust;
        std::function<void(ToolContext&, Json& report)> after;
        std::function<ToolResult(ToolContext&)> empty; // called with nothing to change
        std::string fileSteps = "changed";        // Map Settings and interface images: rewritten only when their sections change
    };

    std::string Text(const Json& object, const char* key)
    {
        if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) return {};
        return object.at(key).get<std::string>();
    }
    std::string Upper(std::string text)
    {
        for (auto& ch : text) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return text;
    }

    // ---------------------------------------------------------- schemas
    // An argument's outline: its type, and an item's id where a list
    // matches by id, with a line saying what it is. The tool's description
    // lists the fields; the merged spec is checked in full (Spec::Parse)
    // with the JSON pointer of any problem, and scct://schema/level-spec has
    // every field, so tools/list stays small.
    const Json& Resolved(const Json& schema)
    {
        if (!schema.is_object() || !schema.contains("$ref") || !schema.at("$ref").is_string()) return schema;
        const auto ref = schema.at("$ref").get<std::string>();
        const auto& defs = Spec::Schema().at("$defs");
        const auto name = ref.substr(ref.find_last_of('/') + 1);
        return defs.contains(name) ? defs.at(name) : schema;
    }
    Json Outline(const std::vector<std::string>& path, const std::string& description)
    {
        const Json* at = &Spec::Schema().at("properties").at(path.front());
        for (size_t i = 1; i < path.size(); ++i) at = &Resolved(*at).at("properties").at(path[i]);
        const auto& schema = Resolved(*at);
        Json out = Json::object();
        if (schema.contains("type")) out["type"] = schema.at("type");
        if (schema.contains("items"))
        {
            const auto& items = Resolved(schema.at("items"));
            Json item = Json::object();
            if (items.contains("type")) item["type"] = items.at("type");
            if (items.contains("enum")) item["enum"] = items.at("enum");
            const auto required = items.value("required", Json::array());
            if (std::find(required.begin(), required.end(), Json("id")) != required.end()) item["required"] = Json::array({"id"});
            out["items"] = item;
        }
        // The description lists the fields; a line here only where it says more.
        if (!description.empty()) out["description"] = description;
        return out;
    }
    Json InputSchema(const Intent& t)
    {
        Json properties = Json::object();
        for (const auto& f : t.fields) properties[f.name] = Outline(f.path, f.note);
        for (auto it = t.extra.begin(); it != t.extra.end(); ++it) properties[it.key()] = it.value();
        // The common arguments, which the description's last paragraph explains.
        if (t.brushMode) properties["brushMode"] = {{"type", "string"}, {"enum", {"pieces", "polygons"}}};
        properties["mode"] = {{"type", "string"}, {"enum", {"apply", "preview"}}};
        properties["remove"] = {{"type", "array"}, {"items", {{"type", "string"}}}};
        properties["existing"] = S::Free("{refs: {id: {path, class} | {tag}}}: live actors named by id.");
        properties["build"] = {{"type", "string"}, {"enum", {"auto", "geometry", "all", "none"}}};
        properties["screenshot"] = {{"type", "boolean"}};
        properties["save"] = {{"type", "string"}};
        properties["force"] = {{"type", "boolean"}};
        if (!properties.contains("mapName")) properties["mapName"] = {{"type", "string"}};
        properties["dryRun"] = {{"type", "boolean"}};
        properties["label"] = {{"type", "string"}};
        return S::Input(properties);
    }

    // ------------------------------------------------------------ merging
    // The text an item is matched on: its key fields, folded.
    std::string KeyOf(const Json& item, const std::string& key)
    {
        if (key.empty() || !item.is_object()) return {};
        std::string out;
        for (size_t start = 0;;)
        {
            const auto plus = key.find('+', start);
            const auto part = key.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            if (!item.contains(part) || !item.at(part).is_string()) return {};
            out += Fold(item.at(part).get<std::string>()) + "\n";
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        return out;
    }
    // A list's items given by an argument: one that matches an item already
    // there updates it field by field; the rest are added, unless the same
    // item is there already.
    Json MergeList(Json list, const Json& items, const std::string& key)
    {
        if (!list.is_array()) list = list.is_null() ? Json::array() : Json::array({list});
        for (const auto& item : items)
        {
            const auto name = KeyOf(item, key);
            auto found = name.empty() ? list.end() : std::find_if(list.begin(), list.end(), [&](const Json& x) { return KeyOf(x, key) == name; });
            if (found == list.end()) { if (std::find(list.begin(), list.end(), item) == list.end()) list.push_back(item); continue; }
            const auto spelling = found->is_object() && found->contains("id") ? found->at("id") : Json();
            *found = Spec::Merge(*found, item);
            if (!spelling.is_null()) (*found)["id"] = spelling; // other items name it this way
        }
        return list;
    }
    void Place(Json& spec, const Field& f, const Json& value)
    {
        Json* at = &spec;
        for (size_t i = 0; i + 1 < f.path.size(); ++i)
        {
            auto& next = (*at)[f.path[i]];
            if (!next.is_object()) next = Json::object();
            at = &next;
        }
        auto& slot = (*at)[f.path.back()];
        if (value.is_array() || (f.list && value.is_object())) slot = MergeList(slot, value.is_array() ? value : Json::array({value}), f.key);
        else if (value.is_object() && slot.is_object()) slot = Spec::Merge(slot, value);
        else slot = value;
    }
    // Takes the items with these ids out of the tool's sections, wherever
    // they are nested (an objective's trigger, a team's start).
    void Remove(Json& spec, const std::vector<std::string>& ids, const std::vector<std::string>& sections)
    {
        std::set<std::string> wanted, removed;
        for (const auto& id : ids) wanted.insert(Fold(id));
        std::function<void(Json&)> walk = [&](Json& node)
        {
            if (node.is_array())
            {
                for (size_t i = 0; i < node.size();)
                {
                    const auto& item = node[i];
                    if (item.is_object() && item.contains("id") && item.at("id").is_string() && wanted.count(Fold(item.at("id").get<std::string>())))
                    {
                        removed.insert(Fold(item.at("id").get<std::string>()));
                        node.erase(i);
                        continue;
                    }
                    walk(node[i]);
                    ++i;
                }
            }
            else if (node.is_object())
                for (auto it = node.begin(); it != node.end(); ++it)
                    if (it.key() != "props" && it.key() != "properties" && it.key() != "document") walk(it.value());
        };
        for (const auto& section : sections)
        {
            const Json::json_pointer pointer(section);
            if (spec.contains(pointer)) walk(spec[pointer]);
        }
        std::vector<std::string> missing;
        for (const auto& id : ids) if (!removed.count(Fold(id))) missing.push_back(id);
        if (!missing.empty())
            throw std::runtime_error("Nothing with the id " + Join(missing) + " is in this part of the level spec (" + Join(sections) + "); map_query what:\"spec\" shows the spec the map was built from.");
    }
    // The spec a map without one starts from: its name, from mapName or the
    // map's file.
    Json NewSpec(const Json& context, const std::string& mapName, Json& notes)
    {
        const auto file = Text(context.value("state", Json::object()), "mapFile");
        auto stem = file.substr(file.find_last_of("\\/") == std::string::npos ? 0 : file.find_last_of("\\/") + 1);
        if (const auto dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
        const bool fileName = !stem.empty() && stem.size() <= 60 && stem.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos;
        const auto name = !mapName.empty() ? mapName : fileName ? Upper(stem) : std::string("UNTITLED");
        Json map{{"name", name}};
        if (fileName) map["file"] = stem;
        if (context.value("open", false))
            notes.push_back("The map has no level spec from an earlier build, so this starts one named " + name +
                            ". Actors already in the map are left alone; existing.refs names any the arguments must reach.");
        return {{"format", Spec::kFormat}, {"version", Spec::kVersion}, {"map", map}};
    }

    // ---------------------------------------------------------------- run
    ToolResult Run(const Intent& t, const Json& a, ToolContext& c)
    {
        auto mode = String(a, "mode", "apply");
        if (mode != "preview" && mode != "apply") throw std::runtime_error("mode is apply or preview.");
        if (DryRun(a)) mode = "preview";
        const auto build = String(a, "build", "auto");
        if (build != "auto" && build != "geometry" && build != "all" && build != "none") throw std::runtime_error("build is auto, geometry, all or none.");
        static const std::set<std::string> common{"mode", "dryRun", "label", "build", "screenshot", "save", "force", "mapName", "brushMode"};
        bool given = false;
        for (auto it = a.begin(); it != a.end(); ++it) given = given || ((!common.count(it.key()) || t.extra.contains(it.key())) && !it.value().is_null());
        if (!given)
        {
            if (t.empty) return t.empty(c);
            std::vector<std::string> names;
            for (const auto& f : t.fields) names.push_back(f.name);
            throw std::runtime_error(t.name + " needs something to change: " + Join(names) + " or remove.");
        }
        const auto context = BuildContext(c, mode == "apply");
        Json notes = Json::array(), post = Json::array();
        const auto sidecar = context.value("sidecar", Json());
        const auto before = sidecar.is_object() && sidecar.contains("spec") && sidecar.at("spec").is_object() ? sidecar.at("spec") : NewSpec(context, String(a, "mapName"), notes);
        auto spec = before;
        for (const auto& f : t.fields) if (f.place && Has(a, f.name.c_str())) Place(spec, f, a.at(f.name));
        if (Has(a, "existing")) spec["existing"] = Spec::Merge(spec.value("existing", Json::object()), Object(a, "existing"));
        if (t.adjust) t.adjust(c, a, spec, before, notes, post);
        const auto removing = Strings(a, "remove");
        if (!removing.empty()) Remove(spec, removing, t.sections);
        Json request{{"tool", t.name}, {"mode", mode}, {"sync", sidecar.is_object() ? "incremental" : "replace"}, {"build", build}, {"force", Bool(a, "force", false)},
                     {"label", Label(a, t.name)}, {"part", {{"pointers", t.sections}, {"rules", t.rules}}}, {"post", post}, {"notes", notes}, {"baseline", before}, {"fileSteps", t.fileSteps}};
        if (Has(a, "screenshot")) request["screenshot"] = Bool(a, "screenshot");
        if (Has(a, "save")) request["save"] = String(a, "save");
        if (t.brushMode && Has(a, "brushMode")) request["brushMode"] = String(a, "brushMode");
        std::vector<Content> images;
        auto report = LevelBuild(c, context, spec, request, images);
        if (t.after && !report.contains("error") && context.value("open", false)) t.after(c, report);
        return BuildResult(c, report, images);
    }

    // -------------------------------------------------------- tool parts
    // The modes a spec plays: its list, or the mode sections it has.
    std::vector<std::string> Modes(const Json& spec)
    {
        std::vector<std::string> out;
        const auto gameplay = spec.value("gameplay", Json::object());
        if (!gameplay.is_object()) return out;
        for (const auto& m : gameplay.value("modes", Json::array())) if (m.is_string()) out.push_back(m.get<std::string>());
        if (out.empty())
            for (const char* m : {"story", "diskhunt", "deathmatch"}) if (gameplay.contains(m)) out.push_back(m);
        return out;
    }
    // gameplay: modes add to the modes the level plays, a mode section given
    // adds its mode, and removeModes drops modes with their sections.
    void GameplayModes(ToolContext&, const Json& a, Json& spec, const Json& before, Json& notes, Json&)
    {
        const auto was = Modes(before);
        auto modes = was;
        auto add = [&](const std::string& m) { if (std::find(modes.begin(), modes.end(), m) == modes.end()) modes.push_back(m); };
        for (const auto& m : Strings(a, "modes")) add(m);
        for (const char* m : {"story", "diskhunt", "deathmatch"})
            if (Has(a, m) && std::find(modes.begin(), modes.end(), m) == modes.end())
            {
                add(m);
                if (!was.empty()) notes.push_back(std::string("Added ") + m + " to gameplay.modes, since its section was given.");
            }
        for (const auto& m : Strings(a, "removeModes"))
        {
            if (m != "story" && m != "diskhunt" && m != "deathmatch") throw std::runtime_error("removeModes takes story, diskhunt and deathmatch.");
            modes.erase(std::remove(modes.begin(), modes.end(), m), modes.end());
            if (spec.contains("gameplay") && spec.at("gameplay").is_object() && spec.at("gameplay").contains(m))
            {
                spec["gameplay"].erase(m);
                notes.push_back("Removed the " + m + " section, with its mission and what it placed.");
            }
        }
        if (modes != was || Has(a, "modes") || Has(a, "removeModes")) spec["gameplay"]["modes"] = modes;
    }
    // map_settings: Map Settings fields one by one or as one object, modes
    // matched by mode, interface files found on this machine, the minimap.
    void MapSettings(ToolContext& c, const Json& a, Json& spec, const Json&, Json& notes, Json& post)
    {
        Json given = Json::object();
        for (const char* key : {"settings", "mapSettings"}) if (Has(a, key)) given = Spec::Merge(given, Object(a, key));
        for (const char* key : {"mapName", "briefSpy", "briefMerc", "legend", "legendInBrief"}) if (Has(a, key)) given[key] = a.at(key);
        Json modes = given.contains("modes") && given.at("modes").is_array() ? given.at("modes") : Json::array();
        given.erase("modes");
        for (const auto& m : List(a, "modes")) modes.push_back(m);
        const auto drop = Strings(a, "removeModes");
        if (!given.empty() || !modes.empty() || !drop.empty())
        {
            auto settings = spec.value("settings", Json::object());
            if (!settings.is_object()) settings = Json::object();
            settings = Spec::Merge(settings, given);
            auto list = MergeList(settings.value("modes", Json::array()), modes, "mode");
            for (const auto& m : drop)
            {
                const auto before = list.size();
                list.erase(std::remove_if(list.begin(), list.end(), [&](const Json& x) { return Text(x, "mode") == m; }), list.end());
                if (list.size() == before) throw std::runtime_error("Map Settings have no " + m + " mode to remove.");
            }
            settings["modes"] = list;
            spec["settings"] = settings;
        }
        // Interface images are read by the editor on this machine.
        if (Has(a, "interface") && spec.contains("interface") && spec.at("interface").is_object())
            for (auto it = spec["interface"].begin(); it != spec["interface"].end(); ++it)
            {
                if (!it.value().is_string() || !Object(a, "interface").contains(it.key())) continue;
                const auto text = it.value().get<std::string>();
                const std::filesystem::path path(std::u8string(text.begin(), text.end()));
                std::error_code error;
                if (path.is_relative() && !c.workDir.empty() && std::filesystem::exists(c.workDir / path, error)) it.value() = PathText(c.workDir / path);
                else if (!std::filesystem::exists(path, error))
                    notes.push_back("interface." + it.key() + ": " + text + " is not a file on this machine; give the image's full path (image_convert prepares one).");
            }
        if (Has(a, "minimap"))
        {
            const auto& minimap = a.at("minimap");
            if (minimap.is_object() || (minimap.is_boolean() && minimap.get<bool>()))
                post.push_back({{"id", "minimap"}, {"op", "minimap.apply"}, {"args", minimap.is_object() ? minimap : Json::object()}, {"optional", true}});
            else if (!minimap.is_boolean()) throw std::runtime_error("minimap is true or the minimap options {floors, framing, style, objectives, spawns, meshes}.");
        }
    }
    // geometric_events: GEs are added to the spec's list (one with an id
    // updates that GE), never replacing it, since the bridge cannot delete one.
    void AddGes(ToolContext&, const Json& a, Json& spec, const Json&, Json&, Json&)
    {
        if (Has(a, "ges")) spec["ges"] = MergeList(spec.value("ges", Json::array()), List(a, "ges"), "id");
    }
    void ListAfter(ToolContext& c, Json& report)
    {
        try { report["geList"] = Call(c, "ge.list"); }
        catch (const std::exception& e) { report["geList"] = {{"error", e.what()}}; }
    }
    ToolResult ListGes(ToolContext& c)
    {
        const auto listed = Call(c, "ge.list");
        Json spec = Json::array();
        try
        {
            const auto got = Call(c, "mcp.sidecar.get");
            const auto sidecar = got.is_object() ? got.value("sidecar", Json()) : Json();
            if (sidecar.is_object() && sidecar.contains("spec") && sidecar.at("spec").is_object()) spec = sidecar.at("spec").value("ges", Json::array());
        }
        catch (const std::exception&) {}
        const auto elements = listed.is_object() ? listed.value("elements", Json::array()) : listed;
        const auto summary = "The map has " + Count(elements.is_array() ? elements.size() : 0, "GE") + " that Map Design or this server added (the editor keeps no list of others), and its level spec " +
                             Count(spec.size(), "GE") + ". Nothing changed.";
        return Ok({{"summary", summary}, {"listed", listed}, {"spec", spec}, {"created", Json::array()}, {"changed", Json::array()}, {"warnings", Json::array()}, {"undo", Json()},
                   {"next", Json::array({Next("map_validate", "Check the GEs against the wiki's measurements.", {{"include", Json::array({"wiki"})}, {"rules", Json::array({"ge"})}})})}},
                  summary);
    }

    // ------------------------------------------------------- descriptions
    const char* const kHow =
        "\n"
        "Merged into the map's stored level spec (lists by id) and built as ONE Undo step; preview or dryRun changes nothing (scct://docs/tools: positions, "
        "merging, remove, existing, force). Every field and the wiki defaults: scct://docs/tool-reference#";

    std::vector<Intent> Intents()
    {
        std::vector<Intent> out;
        {
            Intent t;
            t.name = "blockout";
            t.title = "Lay out rooms and corridors";
            t.description =
                "Lays out the level's spaces as Map Design pieces (scct://docs/map-design): rooms carved out of the solid world, corridors, vents, crawlways, doorways, "
                "stairs, ramps and platforms, plus polygon brushes, per-face textures and static meshes (scct://wiki/static-meshes). The Map Design checks run on the result and the geometry is "
                "rebuilt.\n"
                "- pieces [{id, kind: Room|Corridor|Vent|Crawlway|Doorway|Platform|Stairs|Stairs L|Stairs U|Spiral|Ramp, size: [width, length, height], at: [x, y], "
                "storey, yaw (90s), name, material, construction, thickness, ceiling, steps, portal, attach: {to, side: north|south|east|west, offset}}]\n"
                "- doorways [{id, between: [a, b] | host + side + offset, size: [width, height] (96 x 128 fits a player), sill, depth, portal, name}]\n"
                "- storeys [{id, z, name}]; vertical [{kind: stairs|lift, from, to, at: {in, u, v}, shape, width}]\n"
                "- brushes [{id, csg: CSG_Add|CSG_Subtract, polyFlags, class, at, polygons: [{vertices, texture}]}]; surfaces [{brush, faces: "
                "[floor|ceiling|+X|-X|+Y|-Y], texture}] (named by where the face's normal points: a carved room's east wall is -X); meshes [{id, mesh, at, yaw, scale, "
                "fit: floor}]\n"
                "- defaults {construction, thickness, material, lights: rooms|none, startsPerTeam}";
            t.fields = {{"pieces", {"geometry", "pieces"}, ""},
                        {"doorways", {"geometry", "doorways"}, ""},
                        {"vertical", {"geometry", "vertical"}, ""},
                        {"brushes", {"geometry", "brushes"}, ""},
                        {"surfaces", {"geometry", "surfaces"}, "Applied after the build.", ""},
                        {"meshes", {"geometry", "meshes"}, ""},
                        {"storeys", {"storeys"}, ""},
                        {"defaults", {"defaults"}, ""}};
            t.sections = {"/geometry", "/storeys", "/defaults"};
            t.rules = {"geometry.", "design.", "layout.", "position.", "mapcheck."};
            t.brushMode = true;
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "gameplay";
            t.title = "Set spawns and game modes";
            t.description =
                "Spawns and game modes (scct://wiki/spawn-points, scct://wiki/game-modes): PlayerStarts (TeamNumber 0 spy, 1 merc; at least 3 per team), the Story "
                "mission with its objectives and their triggers, sectors, Disk Hunt and Deathmatch. Placement against the objectives is checked.\n"
                "- spawns {spy, merc: {in: roomId | [roomIds], count (4), facing: {toward}, spacing} | [{id, at, yaw | facing}], toggles: [{id, team: spy|merc, in | "
                "at, enabledBy}]}\n"
                "- modes [story|diskhunt|deathmatch] are added; removeModes drops modes with their sections\n"
                "- story {mission {id, name, description (Spy), descriptionDef (Merc), minimumObjectives, chained, atResetEvent}, objectives [{id, name, description, "
                "descriptionDef, visibilityZone, at, triggers [{id, kind: terminal|flag|bomb (terminal = the ND133 neutralized with the Spy's placed modem, uses dominationCaptureTime; bomb = a planted bomb with a fuse the Mercs defuse, uses captureTime), at, dominationCaptureTime, reach (terminal: the modem's reach to the ND133), dropZones (1-5) [{id, at} | {ref: id}] "
                "(flags share a zone: one makes it with an id, the others name it by ref), resetTime, captureTime, triggerName}]}], sectors [{id, name, brief, briefDefend, objectives, required, actions [{kind: open|light|sound|announce|trigger, target, "
                "hold, ...}]}]}\n"
                "- diskhunt {disks: {in: [roomIds], count} | [{id, at, group}], resetTime, mission}; deathmatch {disableSecurity, mission}\n"
                "- playerCounts [{objective, min, max, open}] (by objective); targets {mission, sizes: [8 counts, 1v1 to 8v8]}";
            t.fields = {{"spawns", {"spawns"}, ""},
                        {"modes", {"gameplay", "modes"}, "", "id", false},
                        {"story", {"gameplay", "story"}, ""},
                        {"diskhunt", {"gameplay", "diskhunt"}, ""},
                        {"deathmatch", {"gameplay", "deathmatch"}, ""},
                        {"playerCounts", {"gameplay", "playerCounts"}, "", "objective"},
                        {"targets", {"gameplay", "targets"}, "", "mission", true, true}};
            t.extra["removeModes"] = {{"type", "array"}, {"items", {{"type", "string"}, {"enum", {"story", "diskhunt", "deathmatch"}}}}};
            t.sections = {"/spawns", "/gameplay"};
            t.rules = {"spawns.", "story.", "modes.", "objectives.", "diskhunt.", "deathmatch.", "objectiveplayers.", "players.", "info.travel", "layout.unreachable"};
            t.adjust = GameplayModes;
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "security";
            t.title = "Add alarms, detectors and cameras";
            t.description =
                "Alarms, detectors and the camera network (scct://wiki/alarms, scct://wiki/level-ingredients). Each detector's Event is its alarm's Tag; the cameras "
                "form one closed NextCam/PrevCam loop in list order with one bFirstCam (a Versus map needs one, or the Mercs softlock). Doors to lock come from "
                "mechanisms.\n"
                "- alarms [{id, name, description (Merc HUD text), descriptionSpy, duration, priority 0-5, outputs [ids or Tags], lockMovers [{mover, action}], "
                "lockObjectives [ids], at}]\n"
                "- devices [{id, kind: laser|camera|motion|presence|mine, at | in | box, facing, alarm, detected: spy|merc|both, length, radius, height, coneAngle, "
                "maxDistance, patrolAngle, rotationVelocity, detectionDelay, hideLaser, spawnPercent}]\n"
                "- camnet [{id, camName, at, yaw | facing, first, constraints {pitch: [min, max], yaw: [min, max]}}]";
            t.fields = {{"alarms", {"security", "alarms"}, ""},
                        {"devices", {"security", "devices"}, ""},
                        {"camnet", {"security", "camnet"}, ""}};
            t.sections = {"/security"};
            t.rules = {"security.", "camnet.", "alarm.", "volume.alarm-zone", "zones.alarm-zone"};
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "mechanisms";
            t.title = "Add doors, lifts and buttons";
            t.description =
                "Doors, movers, elevators, buttons and hacking panels (scct://wiki/movers, scct://wiki/elevators, scct://wiki/buttons-and-hacking-panels).\n"
                "- doors [{id, mesh, at: {doorway} | position, yaw, open: {offset} | {yaw}, moveTime, stayOpen, trigger: {kind: button|panel|presence|event, hackTime, "
                "usableBy: spy|merc|both, at, radius, text}}]: Movers, Key 0 closed, TriggerOpenTimed\n"
                "- movers [{id, class, mesh, at, yaw, keys [{offset, yaw, pitch, roll}], moveTime, stayOpen, initialState, encroach}]\n"
                "- lifts [{id, shaft, storeys, start, mesh, moveTime, doorMesh, carDoorMesh, doorOpen: {offset} | {yaw}, doorMoveTime, doors: [{storey, doorway | at, "
                "yaw, mesh}], switches, carSwitch}]: an SLift. With doorMesh each storey's opening gets an external door (Key 0 open, Key 1 closed) and the car an "
                "SLiftDoor, which the SLift runs with a call switch per storey and one in the car. Never make lift doors with doors: those open on proximity, over an "
                "empty shaft.\n"
                "- buttons [{id, kind: button|panel, at, yaw | facing, targets, hackTime, usableBy, text, mesh}]: SInterrupteurs whose Event is the target's Tag";
            t.fields = {{"doors", {"mechanisms", "doors"}, ""},
                        {"movers", {"mechanisms", "movers"}, ""},
                        {"lifts", {"mechanisms", "lifts"}, ""},
                        {"buttons", {"mechanisms", "buttons"}, ""}};
            t.sections = {"/mechanisms"};
            t.rules = {"door.", "mover.", "movers.", "elevator.", "button.", "lift.", "ingredients.interaction"};
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "events";
            t.title = "Wire events";
            t.description =
                "Wiring between things, and timed and scripted events (scct://wiki/events, scct://wiki/level-properties). map_query what:\"wiring\" shows the live "
                "links.\n"
                "- links [{from, to, delay, untrigger}] (by from and to): from's Event becomes to's Tag\n"
                "- magic [{id, document (scct://schema/smagic-event), at}]; messages [{id, spy, merc, time, priority, at}]; shakers and noises [{id, class, at, "
                "properties}]\n"
                "- level [{event, sendAtTime}] (by event): sent by the LevelInfo; sendAtTime is negative, counted back from the end\n"
                "- triggerLights [{light, object, initiallyOn}] (by light)";
            t.fields = {{"links", {"events", "links"}, "", "from+to"},
                        {"magic", {"events", "magic"}, ""},
                        {"messages", {"events", "messages"}, ""},
                        {"shakers", {"events", "shakers"}, ""},
                        {"noises", {"events", "noises"}, ""},
                        {"level", {"events", "level"}, "", "event"},
                        {"triggerLights", {"events", "triggerLights"}, "", "light"}};
            t.sections = {"/events"};
            t.rules = {"events.", "level.send-at-time"};
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "environment";
            t.title = "Add lights, sound, zones and props";
            t.description =
                "Lights, sound, zones, volumes and what dresses a level (scct://wiki/lighting, sound-actors, zones-and-volumes, level-ingredients, gameplay-objects, "
                "vision-mode-properties, softbody-actors and the others). Zones and volumes change brushes, so the geometry is rebuilt.\n"
                "- lights [{id, preset: ceiling|spot|sun|echelon-spot, in | at, radius, brightness, hue, saturation, cone (spot preset: LightCone 0-255), corona, "
                "switchable, yaw, pitch}]\n"
                "- sounds [{id, class, sound, at, radius, volume}]; zones [{id, locationName, spaces: [pieceIds], portals}]\n"
                "- volumes [{id, class: Volume|SDamageVolume|ERainVolume|BlockingVolume, box: {min, max} | {in, height}, locationName, priority}]\n"
                "- ingredients [{id, kind: ammo|infirmary|glass|extinguisher|projector|sprinkler|puddle|balltrap, in | at, yaw, persistentLight (projector: keep its Echelon light on when unused)}]; objects [{id, mesh, at, yaw, "
                "damageable, hitPoints}]; meshes [{id, mesh, at, yaw, scale, fit}]\n"
                "- vision [{target, heatIntensity, heatOpacity, heatRadius, emfIntensity, emfOpacity, hideInNormalVision (shows only in Thermal/EMF; pair with bHidden + an event to reveal on a hack), properties}] (by target)\n"
                "- props, fish, animated, tutorials: [{id, class, mesh, at, yaw, scale, properties}]; softbody the same, class ESBPatchActor, ESBStripDoorActor "
                "(the wiki's ESBStripeDoorActor), ESBRopeActor or ESBChainActor, mesh only for ESBChainActor (its links); never copied";
            t.fields = {{"lights", {"environment", "lights"}, ""},
                        {"sounds", {"environment", "sounds"}, ""},
                        {"zones", {"zones"}, ""},
                        {"volumes", {"environment", "volumes"}, ""},
                        {"ingredients", {"environment", "ingredients"}, "Matched by id when they have one, else added."},
                        {"objects", {"environment", "objects"}, ""},
                        {"meshes", {"geometry", "meshes"}, ""},
                        {"vision", {"environment", "vision"}, "", "target"},
                        {"props", {"environment", "props"}, ""},
                        {"fish", {"environment", "fish"}, ""},
                        {"softbody", {"environment", "softbody"}, ""},
                        {"animated", {"environment", "animated"}, ""},
                        {"tutorials", {"environment", "tutorials"}, ""}};
            t.sections = {"/environment", "/zones", "/geometry/meshes"};
            t.rules = {"light.", "lighting.", "sound.", "volume.", "ingredient.", "ingredients.", "object.", "objects.", "softbody.", "fish.", "tutorials.", "zones."};
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "geometric_events";
            t.title = "Add ledges, pipes, ladders and wires";
            t.description =
                "Geometric events, the ledges, pipes and wires players climb (scct://wiki/geometric-events), checked against the wiki's measurements. With no ges, "
                "geTriggers or remove it only lists the GEs.\n"
                "- ges [{id, kind: ledge|hoh|pipe|ladder|zipline|fence|pole, points}]: two points along the edge; three for pipe and ladder (bottom, top, the side "
                "climbed from). They are added to the spec's list; the bridge cannot delete a GE (delete it in GE mode).\n"
                "- geTriggers [{id, toggle, geMask, at}]: SGETriggers";
            t.fields = {{"ges", {"ges"}, "", "id", false},
                        {"geTriggers", {"geTriggers"}, ""}};
            t.sections = {"/ges", "/geTriggers"};
            t.rules = {"ge.", "incremental.ge"};
            t.adjust = AddGes;
            t.after = ListAfter;
            t.empty = ListGes;
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "map_settings";
            t.title = "Set Map Settings and Level Properties";
            t.description =
                "Level Properties, Map Settings, interface textures and the in-game minimap (scct://wiki/level-properties, scct://wiki/map-settings). Map Settings "
                "exist only once a new map is saved, closed and reopened (map_file save_as, editor_restart); when the bridge cannot write them, the result's "
                "mapSettings has the checklist and manual steps. Save the map afterwards.\n"
                "- level {song, deathPawnHeight, motionTracking: standard|enhanced, events [{event, sendAtTime}], geActivationMask}\n"
                "- mapName (capitals), briefSpy, briefMerc, legend, legendInBrief; modes [{mode: story|diskhunt|deathmatch, respawn, timeLimit, spiesLives, "
                "sentriesLives, scoreLimit, handicap, numDisksMax, strings {explicationSpy, explicationMerc, briefSpy, briefMerc, legendSpy, legendMerc}}] (by mode; "
                "removeModes); or all of it as mapSettings\n"
                "- interface {menu, briefing, loadingAtt, loadingDef}: image files on this machine\n"
                "- minimap true | {floors, framing, style, objectives, spawns, meshes, briefing, briefingFloor}";
            const auto& settings = Spec::Schema().at("properties").at("settings").at("properties");
            t.fields = {{"level", {"level"}, ""},
                        {"interface", {"interface"}, ""},
                        {"mapSettings", {"settings"}, "", "id", false},
                        {"settings", {"settings"}, "The same as mapSettings.", "id", false},
                        {"modes", {"settings", "modes"}, "", "mode", false}};
            for (const auto& [key, text] : std::vector<std::pair<std::string, std::string>>{
                     {"mapName", "Map Settings MapName, in capitals (also a new spec's name)."},
                     {"briefSpy", "ExpliMapInBriefSpy, all modes."},
                     {"briefMerc", "ExpliMapInBriefSentry, all modes."},
                     {"legend", "LegendMap; | breaks lines."},
                     {"legendInBrief", "LegendMapInBrief; | breaks lines."}})
            {
                auto schema = settings.at(key);
                schema["description"] = text;
                t.extra[key] = schema;
            }
            t.extra["removeModes"] = {{"type", "array"}, {"items", {{"type", "string"}, {"enum", {"story", "diskhunt", "deathmatch"}}}}};
            t.extra["minimap"] = {{"type", Json::array({"boolean", "object"})}};
            t.sections = {"/settings", "/level", "/interface"};
            t.rules = {"settings.", "level.", "interface.", "release."};
            t.adjust = MapSettings;
            t.fileSteps = "always";
            out.push_back(std::move(t));
        }
        return out;
    }
}

void RegisterIntentTools(Registry& registry)
{
    for (auto& intent : Intents())
    {
        Tool t;
        t.name = intent.name;
        t.title = intent.title;
        t.description = intent.description + kHow + intent.name + ".";
        t.inputSchema = InputSchema(intent);
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        auto shared = std::make_shared<Intent>(std::move(intent));
        t.run = [shared](const Json& a, ToolContext& c) { return Guarded([&] { return Run(*shared, a, c); }); };
        registry.Add(std::move(t));
    }
}
}
