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
        std::string description;
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
    // The spec's shared definitions in short: the server checks only an
    // argument's outline here, and the merged spec is checked in full
    // (Spec::Parse) with the JSON pointer of any problem.
    Json Short(const std::string& name)
    {
        if (name == "id") return {{"type", "string"}};
        if (name == "ids") return {{"type", "array"}, {"items", {{"type", "string"}}}};
        if (name == "height") return {{"type", "string"}, {"description", "\"floor+N\" or \"ceiling-N\""}};
        if (name == "coordinate") return {{"type", Json::array({"number", "string"})}};
        if (name == "vec2" || name == "vec3") return {{"type", "array"}, {"items", {{"type", "number"}}}};
        if (name == "point") return {{"type", "array"}, {"description", "[x, y] or [x, y, z]"}};
        if (name == "position") return {{"type", Json::array({"array", "object"})}, {"description", "[x, y, z] | {in, u, v, h} | {near, offset} | {doorway}"}};
        if (name == "inPosition") return {{"type", "object"}, {"description", "{in: pieceId, u, v, h}"}};
        if (name == "target") return {{"type", Json::array({"string", "array", "object"})}, {"description", "An id or a position."}};
        if (name == "facing") return {{"type", Json::array({"number", "object"})}, {"description", "Yaw in degrees, {yaw} or {toward: id | position}."}};
        if (name == "props") return {{"type", "object"}, {"description", "Raw property values: leaves are strings, structs objects, {\"$ref\": id} for actors."}};
        if (name == "generic") return {{"type", "object"}, {"description", "{id, class, mesh, at, yaw, scale, properties}"}};
        if (name == "mission") return {{"type", "object"}, {"description", "{id, name, description (the Spy text), descriptionDef (the Merc text), minimumObjectives, chained, atResetEvent, at, props}"}};
        return Json::object();
    }
    // The spec's own schema, its references in short, with "required"
    // loosened to the id where an item has one: an argument may change one
    // field of an item it names.
    Json Loose(Json schema)
    {
        if (schema.is_array()) { for (auto& item : schema) item = Loose(item); return schema; }
        if (!schema.is_object()) return schema;
        if (schema.contains("$ref") && schema.at("$ref").is_string())
        {
            const auto ref = schema.at("$ref").get<std::string>();
            auto out = Short(ref.substr(ref.find_last_of('/') + 1));
            for (auto it = schema.begin(); it != schema.end(); ++it) if (it.key() != "$ref") out[it.key()] = it.value();
            return out;
        }
        if (schema.contains("required") && schema.at("required").is_array() && schema.contains("properties") && schema.at("properties").is_object() && schema.at("properties").contains("id"))
        {
            const auto& required = schema.at("required");
            if (std::find(required.begin(), required.end(), Json("id")) != required.end()) schema["required"] = Json::array({"id"});
        }
        for (auto it = schema.begin(); it != schema.end(); ++it)
            if (it.key() != "required" && it.key() != "enum") it.value() = Loose(it.value());
        return schema;
    }
    Json Part(const std::vector<std::string>& path, const std::string& description)
    {
        const Json* at = &Spec::Schema().at("properties").at(path.front());
        for (size_t i = 1; i < path.size(); ++i) at = &at->at("properties").at(path[i]);
        auto schema = Loose(*at);
        schema["description"] = description;
        return schema;
    }
    Json InputSchema(const Intent& t)
    {
        Json properties = Json::object();
        for (const auto& f : t.fields) properties[f.name] = Part(f.path, f.description);
        for (auto it = t.extra.begin(); it != t.extra.end(); ++it) properties[it.key()] = it.value();
        if (t.brushMode) properties["brushMode"] = S::Enum({"pieces", "polygons"}, "Map Design pieces (default, or what the map was built with) or plain polygon brushes.");
        properties["mode"] = S::Enum({"apply", "preview"}, "apply (default), or preview (also dryRun:true), which changes nothing.");
        properties["remove"] = S::Array({{"type", "string"}}, "Ids of this part's items to delete.");
        properties["existing"] = Part({"existing"}, "Live actors to refer to by id: {refs: {id: {path, class} | {tag}}}.");
        properties["build"] = S::Enum({"auto", "geometry", "all", "none"}, "auto (default: geometry when brushes changed), geometry, all (with lighting) or none.");
        properties["screenshot"] = S::Boolean("A top-view screenshot afterwards. Default: when it builds.");
        properties["save"] = S::String("Save the map to this path afterwards (Packages\\MapsEd\\<Name>D.sdc).");
        properties["force"] = S::Boolean("Apply despite errors the change brings.");
        if (!properties.contains("mapName")) properties["mapName"] = S::String("For a map with no level spec yet: the new spec's name (capitals). Default: the map file's.");
        return S::Mutating(S::Input(properties));
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
        "\nHow it works: the arguments are laid over the level spec the map was last built from (map_query what:\"spec\"; a map without one starts a new "
        "spec). List items match by id (vision by target, links by from and to, level events by event, Map Settings modes by mode): a match is updated "
        "field by field, the rest are added. remove [ids] takes items out. Then only what changed is built, as level_build sync:\"incremental\" does, in "
        "ONE Undo step. mode apply (default), or preview (also dryRun:true): the editor's preview rows and a plan PNG, no change. Errors the change brings "
        "block apply unless force:true; errors the spec already had are listed as standing. The result has created, changed and removed by id (with the "
        "actor paths), validation (this part's findings: JSON pointer into the spec, wikiPage for scct://wiki/<page>, fix), undo and next.\n"
        "Positions: [x, y, z] (z may be \"floor+N\"), {in: pieceId, u, v (-0.5..0.5), h: \"floor+N\" | \"ceiling-N\"}, {near: id, offset} or {doorway: "
        "id}. Lengths are in the spec's units (Unreal units by default), yaw in degrees (east 0, north 90), +X east, +Y north, Z up. Each id "
        "([A-Za-z_][A-Za-z0-9_]{0,62}, unique ignoring case) becomes the actor's Tag; existing.refs names live actors the spec did not make.";

    std::vector<Intent> Intents()
    {
        std::vector<Intent> out;
        {
            Intent t;
            t.name = "blockout";
            t.title = "Lay out rooms and corridors";
            t.description =
                "Lays out the level's spaces as Map Design pieces, the RE+ blockout the Map Design panel can keep editing (scct://docs/map-design): rooms, corridors, "
                "vents, crawlways, doorways, stairs, ramps and platforms, plus polygon brushes, per-face textures and static meshes.\n"
                "- pieces [{id, kind: Room|Corridor|Vent|Crawlway|Doorway|Platform|Stairs|Stairs L|Stairs U|Spiral|Ramp, size: [width (local X), length (local Y), "
                "height], at: [x, y] (the centre; z from storey), storey, yaw (multiples of 90 keep walls on the compass), name, material (Package.Group.Name), "
                "attach: {to, side: north|south|east|west, offset} (flush against another piece's wall, running away from it), steps}]. The world is solid: rooms "
                "are carved out of it (construction Carve, walls defaults.thickness, 16), corridors cut through the walls they join, vents are crouch height (125) "
                "and crawlways 105. Additive stairs, ramps and platforms belong inside carved space.\n"
                "- doorways [{id, between: [a, b] | host + side + offset, size: [width, height], sill, portal}]: openings cut from a wall's inside face; portal puts "
                "a zone portal in it (default true, or only where two zones meet when zones are given). 96 x 128 fits a standing player (180 tall).\n"
                "- storeys [{id, z}] and vertical [{kind: stairs|lift, from: storeyId, to, at: {in, u, v}, shape}] join floors.\n"
                "- brushes [{id, csg: CSG_Add|CSG_Subtract, polyFlags, at, polygons: [{vertices: [[x, y, z]...], texture}]}]: polygon brushes. surfaces "
                "[{brush, faces: [floor|ceiling|+X|-X|+Y|-Y], texture}] texture a piece's faces after the build; a face is named by where its normal points, so a "
                "carved room's east wall is -X. meshes [{id, mesh: StaticMesh'Pkg.Group.Name', at, yaw, scale, fit: floor}] place StaticMeshActors.\n"
                "- defaults {construction, thickness, material, lights: rooms (a ceiling light in each room) | none, startsPerTeam}.\n"
                "The Map Design checks run on the result (validation): steps at most 35 high, slopes at most 45 degrees, standing clearance 96 x 180, crouch 125, "
                "rooms reachable through doorways. brushMode pieces (default) or polygons. The geometry is rebuilt afterwards. Spawns, objectives and the rest "
                "go in through gameplay, security, mechanisms, events and environment, which place things {in: pieceId}.";
            t.fields = {{"pieces", {"geometry", "pieces"}, "Blockout pieces, matched by id."},
                        {"doorways", {"geometry", "doorways"}, "Openings between pieces, matched by id."},
                        {"vertical", {"geometry", "vertical"}, "Stairs or lifts between storeys."},
                        {"brushes", {"geometry", "brushes"}, "Polygon brushes, matched by id."},
                        {"surfaces", {"geometry", "surfaces"}, "Per-face textures on pieces, applied after the build.", ""},
                        {"meshes", {"geometry", "meshes"}, "StaticMeshActors, matched by id."},
                        {"storeys", {"storeys"}, "Floors: [{id, z, name}], matched by id."},
                        {"defaults", {"defaults"}, "Spec defaults: construction, thickness, material, lights, startsPerTeam."}};
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
                "Who spawns where and what each mode plays (scct://wiki/spawn-points, scct://wiki/game-modes).\n"
                "- spawns {spy, merc: {in: roomId | [roomIds], count (default defaults.startsPerTeam, 4), facing: {toward: id | position}, spacing} | [{id, at, "
                "yaw | facing}], toggles: [{id, team: spy|merc, in | at, enabledBy (an event Tag)}]}: PlayerStarts with TeamNumber 0 (spy) and 1 (merc) on a "
                "128-unit lattice at floor+48; the wiki wants at least 3 per team (an error in release). toggles are STriggerPlayerStarts whose Tag is the "
                "toggling event.\n"
                "- modes [story, diskhunt, deathmatch] add to the modes the level plays (giving a mode's section adds it too); removeModes drops modes together "
                "with their sections.\n"
                "- story {mission {id, name, description (the Spy text), descriptionDef (the Merc text), minimumObjectives, chained, atResetEvent}, objectives "
                "[{id, name, description, descriptionDef, visibilityZone (a zone id), at, triggers [{id, kind: terminal|flag|bomb, at, dominationCaptureTime "
                "(terminal), dropZones [{at}] 1-5 and resetTime (flag), captureTime (bomb), triggerName}]}], sectors [{id, name, brief, briefDefend, objectives "
                "[ids], required, actions [{kind: open|light|sound|announce|trigger, target, ...}]}]}: an SMission with GameMode GM_Multi listing its SObjectives by "
                "exact name, each SObjective listing its Triggers; a terminal is an SComputerObjectiveTrigger at floor+78 using DominationCaptureTime "
                "(CaptureTime on a terminal warns); a flag is an SFlag with 1-5 SFlagDropZones; a bomb target an SBombTargetObjectiveTrigger with CaptureTime. "
                "Sectors chain under a GM_Multi mission with bChained True, each sector mission GM_Undefined with bChained False, with Zone<N>_Complete "
                "SMagicEvents and Zone<N>_Announce SAlarms.\n"
                "- diskhunt {disks: {in: [roomIds], count} | [{id, at, group}], resetTime, mission}: an SMission GM_DiskHunt listing every SDisk, Group cycling "
                "1/2/3, disks kept clear of geometry; the Map Settings ScoreLimit (map_settings) must not exceed the disks placed.\n"
                "- deathmatch {disableSecurity, mission}: an SMission GM_DeathMatch whose AtResetEvent fires an SMagicEvent that switches the security off.\n"
                "- playerCounts [{objective, min, max, open}] (matched by objective) and targets {mission, sizes: [8 counts, 1v1 to 8v8]}: objective player "
                "limits.\n"
                "Placement is checked too: spy starts not within 512 units of an objective, the teams not within 768 of each other, every objective within 4096 "
                "of a merc start, objectives reachable from the spawns.";
            t.fields = {{"spawns", {"spawns"}, "Player starts per team and toggled starts."},
                        {"modes", {"gameplay", "modes"}, "Modes to add to those the level plays.", "id", false},
                        {"story", {"gameplay", "story"}, "Story mode: mission, objectives with their triggers, sectors."},
                        {"diskhunt", {"gameplay", "diskhunt"}, "Disk Hunt: disks, resetTime, mission."},
                        {"deathmatch", {"gameplay", "deathmatch"}, "Deathmatch: disableSecurity, mission."},
                        {"playerCounts", {"gameplay", "playerCounts"}, "Objective player limits, matched by objective.", "objective"},
                        {"targets", {"gameplay", "targets"}, "Objectives to win per lobby size, matched by mission.", "mission", true, true}};
            t.extra["removeModes"] = S::Array(S::Enum({"story", "diskhunt", "deathmatch"}, "A mode."), "Modes the level stops playing; their sections go too.");
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
                "Alarms, detectors and the camera network (scct://wiki/alarms, scct://wiki/level-ingredients).\n"
                "- alarms [{id, name, description (the Merc HUD text), descriptionSpy, duration, priority 0-5 (5 when it locks something, 2-4 otherwise), outputs "
                "[ids or Tags it triggers: doors, lights, events], lockMovers [{mover: a door id, action}], lockObjectives [objective trigger ids]}]: SAlarms. "
                "outputs fill Events, lockMovers the complete MoversToLock structs (ActionBeforeLock default ActionBeforeLock_None), lockObjectives "
                "ObjectiveTriggersToLock; a bomb target (SBombTargetObjectiveTrigger) is never locked.\n"
                "- devices [{id, kind: laser|camera|motion|presence|mine, at | in | box, facing, alarm (an alarm id), detected: spy|merc|both, length, radius, "
                "height, coneAngle, maxDistance, patrolAngle, rotationVelocity, detectionDelay, hideLaser, spawnPercent, props}]: each detector's Event is its "
                "alarm's Tag; detected sets DetectedClass (SPawnAttaque, SPawnDefense, SBasePawn). A motion sensor is an SVolumetricSensor plus its detection "
                "Volume, a polygon brush sized to box ({min, max} or {in: roomId, height}). Lasers and presence detectors sit 48 units above the floor, cameras "
                "at 200.\n"
                "- camnet [{id, camName, at, yaw | facing, first, constraints {pitch: [min, max], yaw: [min, max]}}]: SCamNetwork cameras wired NextCam/PrevCam "
                "as one closed loop in list order (new cameras join the end of the loop), exactly one bFirstCam (first, else the first listed), bBlockActors and "
                "bBlockPlayers True. A Versus map needs at least one, or the Mercs softlock.\n"
                "Checked: detectors wired to an alarm, alarms fed and doing something, motion sensors with a volume, MessagePriority 0-5, no SAlarmZoneInfo (RE+ "
                "never makes one), the CamNet loop. Doors to lock come from mechanisms.";
            t.fields = {{"alarms", {"security", "alarms"}, "SAlarms, matched by id."},
                        {"devices", {"security", "devices"}, "Detectors (laser, camera, motion, presence, mine), matched by id."},
                        {"camnet", {"security", "camnet"}, "CamNet cameras in loop order, matched by id."}};
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
                "- doors [{id, mesh: StaticMesh'Pkg.Group.Name', at: {doorway: id} | position, yaw, open: {offset: [x, y, z]} | {yaw: degrees}, moveTime, stayOpen, "
                "trigger: {kind: button|panel|presence|event, hackTime, usableBy: spy|merc|both, at, radius, text}}]: static-mesh Movers, Key 0 closed and Key 1 "
                "open (KeyPos/KeyRot), NumKeys 2, InitialState TriggerOpenTimed, MoverEncroachType ME_WaitAndRetryWhenEncroach, left at Key 0. A presence "
                "trigger is an SPresenceTrigger whose Event is the door's Tag; button and panel make an SInterrupteur; event leaves it to a Tag.\n"
                "- movers [{id, class, mesh, at, yaw, keys [{offset, yaw, pitch, roll}], moveTime, stayOpen, initialState, encroach}]: other static-mesh movers "
                "(brush movers are not supported).\n"
                "- lifts [{id, shaft: pieceId, storeys: [storeyIds], start, mesh, moveTime, switches}]: with a mesh an SLift with one key per storey (start first) "
                "and a TriggerToggle call switch per floor in DoorsAndSwitches; without a mesh a two-storey Map Design lift platform. Lift doors are not made: add "
                "them as doors (external doors Key 0 open, Key 1 closed).\n"
                "- buttons [{id, kind: button|panel, at, yaw | facing, targets: [ids or Tags], hackTime, usableBy, text, mesh}]: an SInterrupteur with "
                "fInteractionHeight 120, fInteractionRadius 60, bInteractInFront, UseRelativeLocation 55,0,0 (button) or 39.5,3,0 (panel) and UseRelativeRotation "
                "yaw 32768; its Event is the target's Tag, several targets fan out through an SMagicEvent; usableBy spy U_SPAWNOFFENSE, merc U_SPAWNDEFENSE, both "
                "U_BOTH (a merc panel with a hack time warns: the wiki calls it unsupported).\n"
                "Alarms lock doors with security alarms[].lockMovers.";
            t.fields = {{"doors", {"mechanisms", "doors"}, "Doors, matched by id."},
                        {"movers", {"mechanisms", "movers"}, "Other static-mesh movers, matched by id."},
                        {"lifts", {"mechanisms", "lifts"}, "Elevators, matched by id."},
                        {"buttons", {"mechanisms", "buttons"}, "Buttons and hacking panels, matched by id."}};
            t.sections = {"/mechanisms"};
            t.rules = {"door.", "mover.", "movers.", "elevator.", "button.", "lift.", "ingredients.interaction"};
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "events";
            t.title = "Wire events";
            t.description =
                "Wiring between things, and timed and scripted events (scct://wiki/events, scct://wiki/level-properties).\n"
                "- links [{from, to, delay, untrigger}] (matched by from and to): from's Event becomes to's Tag; a delay goes through an SMagicEvent action.\n"
                "- magic [{id, document, at}]: SMagicEvents from scct.smagic-event documents (scct://schema/smagic-event, the SMagicEvent Workbench format).\n"
                "- messages [{id, spy, merc, time, priority, at}]: SMessageTriggers, HUD text per team.\n"
                "- shakers and noises [{id, class, at, properties}]: SShaker and SNotifyNoise.\n"
                "- level [{event, sendAtTime}] (matched by event): events the LevelInfo sends at a time counted back from the end of the match; use a negative "
                "time, since 0 or more never fires.\n"
                "- triggerLights [{light (an environment light id), object (an SGamePlayObject id), initiallyOn}] (matched by light): the light becomes an "
                "STriggerLight in the object's ObjectLights.\n"
                "Ids are Tags, so links stay consistent; map_query what:\"wiring\" lists the live links and the unresolved ones.";
            t.fields = {{"links", {"events", "links"}, "Event links, matched by from and to.", "from+to"},
                        {"magic", {"events", "magic"}, "SMagicEvents, matched by id."},
                        {"messages", {"events", "messages"}, "SMessageTriggers, matched by id."},
                        {"shakers", {"events", "shakers"}, "SShakers, matched by id."},
                        {"noises", {"events", "noises"}, "SNotifyNoises, matched by id."},
                        {"level", {"events", "level"}, "Level events {event, sendAtTime}, matched by event.", "event"},
                        {"triggerLights", {"events", "triggerLights"}, "Switchable lights, matched by light.", "light"}};
            t.sections = {"/events"};
            t.rules = {"events.", "level.send-at-time"};
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "environment";
            t.title = "Add lights, sound, zones and props";
            t.description =
                "Lights, sound, zones, volumes and what dresses a level (scct://wiki/lighting, sound-actors, zones-and-volumes, level-ingredients, "
                "gameplay-objects, vision-mode-properties, static-meshes, fish, softbody-actors, animated-meshes, map-tutorials).\n"
                "- lights [{id, preset: ceiling (LE_None) | spot (LE_Spotlight) | sun (LE_Sunlight; needs a Fake Backdrop in the zone) | echelon-spot "
                "(LE_ESpotShadow, EchelonNormalInfluence 0.9), in | at, radius, brightness, hue, saturation (0-255), corona, switchable (an STriggerLight events "
                "can switch), yaw, pitch}]. Static lights only use LE_None, LE_Spotlight or LE_Sunlight; they bake with map_build what:\"all\".\n"
                "- sounds [{id, class (SoundTrigger, MusicTrigger, SAmbientSoundTrigger, AmbientSound), sound: Sound'Pkg.Group.Name' (already loaded), at, radius, "
                "volume 0-255}].\n"
                "- zones [{id, locationName, spaces: [pieceIds], portals}]: a ZoneInfo with LocationName in each zone and zone portals in the doorways where zones "
                "meet; put every spawn room in a zone.\n"
                "- volumes [{id, class: Volume|SDamageVolume|ERainVolume|BlockingVolume, box: {min, max} | {in, height}, locationName, priority}]: polygon brushes "
                "sized to the box.\n"
                "- ingredients [{id, kind: ammo (SAmmoStock, interaction 60 high / 120 radius) | infirmary (SInfirmary, blocking, 120 / 60) | glass (SVitre) | "
                "extinguisher | projector | sprinkler (hidden) | puddle | balltrap, in | at, yaw}].\n"
                "- objects [{id, mesh, at, yaw, damageable, hitPoints}]: SGamePlayObjects, never bPickable. meshes [{id, mesh, at, yaw, scale, fit: floor}]: "
                "StaticMeshActors.\n"
                "- vision [{target, heatIntensity, heatOpacity, heatRadius, emfIntensity, emfOpacity (0-1), properties}] (matched by target): thermal and EMF "
                "vision values on a spec or existing actor.\n"
                "- props, fish (SFish: bPilote on the leader, Pilot on followers), softbody (always set class: ESBPatchActor, ESBStripeDoorActor, ESBRopeActor or "
                "ESBChainActor; never copy one), animated (SAnimatedMesh) and tutorials (SZoneTips, SHelpPath, SEndZoneTip): [{id, class, mesh, at, yaw, scale, "
                "properties}] with raw property values (leaves are strings, structs objects, {\"$ref\": id} for actors).\n"
                "Zones and volumes change brushes, so the geometry is rebuilt (build auto).";
            t.fields = {{"lights", {"environment", "lights"}, "Lights, matched by id."},
                        {"sounds", {"environment", "sounds"}, "Sound actors, matched by id."},
                        {"zones", {"zones"}, "Zones: [{id, locationName, spaces}], matched by id."},
                        {"volumes", {"environment", "volumes"}, "Volumes, matched by id."},
                        {"ingredients", {"environment", "ingredients"}, "Level ingredients; matched by id when they have one, else added."},
                        {"objects", {"environment", "objects"}, "SGamePlayObjects, matched by id."},
                        {"meshes", {"geometry", "meshes"}, "StaticMeshActors, matched by id."},
                        {"vision", {"environment", "vision"}, "Vision values on actors, matched by target.", "target"},
                        {"props", {"environment", "props"}, "Other actors {id, class, at, properties}, matched by id."},
                        {"fish", {"environment", "fish"}, "Fish, matched by id."},
                        {"softbody", {"environment", "softbody"}, "SoftBody actors, matched by id."},
                        {"animated", {"environment", "animated"}, "Animated meshes, matched by id."},
                        {"tutorials", {"environment", "tutorials"}, "Tutorial actors, matched by id."}};
            t.sections = {"/environment", "/zones", "/geometry/meshes"};
            t.rules = {"light.", "lighting.", "sound.", "volume.", "ingredient.", "ingredients.", "object.", "objects.", "softbody.", "fish.", "tutorials.", "zones."};
            out.push_back(std::move(t));
        }
        {
            Intent t;
            t.name = "geometric_events";
            t.title = "Add ledges, pipes, ladders and wires";
            t.description =
                "Geometric events, the ledges, pipes and wires players climb (scct://wiki/geometric-events).\n"
                "- ges [{id, kind: ledge|hoh|pipe|ladder|zipline|fence|pole, points}]: each runs the editor's GE ADD (ledge LG, hoh HOH, pipe PIPE, ladder LADDER, "
                "zipline ZL, fence FENCE, pole POLE). Two points along the edge for ledge, hoh, zipline, fence and pole; three for pipe and ladder: bottom, top, "
                "then the side the player climbs from. Pipes and ladders are put low end first with the side point at that height, as the game needs. New GEs are "
                "added to the spec's list (one with an id updates that GE). The bridge cannot delete GEs: one taken out of the spec stays in the map until it is "
                "deleted in GE mode.\n"
                "- geTriggers [{id, toggle, geMask, at}]: SGETriggers that switch GE groups; the starting mask is map_settings level.geActivationMask. GE IDs are "
                "set by hand in the editor (Set GE ID).\n"
                "The wiki's measurements are checked (validation): ledge grab 208-320 standing, 448 running, 512 co-op, obstacles 115, pull-up 64, a ledge at "
                "least 32 long; fences 88-128 high (96 best) with a bClimbable texture; hand-over-hand 216 clearance; ladders an even number of bars, at least 8 "
                "(32 units a bar); ziplines at most 384 up with 198 clearance; pipes and ladders from the low end. map_validate measures them again on the built "
                "map.\n"
                "Called with no ges, geTriggers or remove, it only lists the GEs: those Map Design or this server added (ge.list; the editor keeps no list of "
                "others) and the spec's. After a change the result has geList too.";
            t.fields = {{"ges", {"ges"}, "GEs to add (or update, by id).", "id", false},
                        {"geTriggers", {"geTriggers"}, "SGETriggers, matched by id."}};
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
                "Level Properties, Map Settings, interface textures and the in-game minimap (scct://wiki/level-properties, scct://wiki/map-settings).\n"
                "- level {song, deathPawnHeight (fDeathPawnHeight: below everything players use, or 0 for off), motionTracking: standard (MTV 52/85) | enhanced "
                "(8/8), events [{event, sendAtTime (negative: seconds before the end)}] (replaces the list), geActivationMask (the starting GE mask)}: the "
                "LevelInfo, written with settings.edit inside the Undo step.\n"
                "- Map Settings (SMapSettings): mapName (in capitals), briefSpy and briefMerc (shared by every mode), legend (date, place and a one-line story; | "
                "breaks lines), legendInBrief (the numbered minimap zones), modes [{mode: story|diskhunt|deathmatch, respawn 5-15, timeLimit 0-20 minutes, "
                "spiesLives and sentriesLives 0-10 (0 is unlimited), scoreLimit, handicap, numDisksMax (Disk Hunt; at most the disks placed), strings "
                "{explicationSpy, explicationMerc, briefSpy, briefMerc, legendSpy, legendMerc}}] matched by mode (removeModes drops one); or all of it as one "
                "object in mapSettings (also called settings). Modes[] and ModesStrings[] are written index-aligned with GameModeId GM_Multi, GM_DiskHunt or "
                "GM_DeathMatch, and LegendModeInBrief* defaults to the Ubisoft text.\n"
                "  SMapSettings exist only once a new map is saved, closed and opened again: map_file action save_as, editor_restart, then this tool. This editor "
                "build may not be able to write them over the bridge: then the result's mapSettings says applied false and gives the checklist of values and the "
                "manual steps (right-click empty viewport space > Map Settings), and the values stay in the spec for the next try.\n"
                "- interface {menu, briefing (256 x 256, DXT5 with alpha), loadingAtt, loadingDef (512 x 512, DXT1)}: image files on this machine (full paths, or "
                "relative to the work directory) imported into the map's <Map>-i package (interface.texture; PNG, BMP, JPEG, GIF and TIFF are resized, TGA must "
                "already be the right size; image_convert prepares them).\n"
                "- minimap true or {floors: [z...], framing: gameplay|everything|current, style, objectives, spawns, meshes, briefing, briefingFloor}: draws the "
                "in-game (F7) map from the built geometry into MapSnapshotInfo (minimap.apply).\n"
                "Checked: MapName in capitals, the ranges above, one Modes and ModesStrings entry per mode, Disk Hunt ScoreLimit and Handicap against the disks, "
                "SendAtTime negative, interface texture formats. Save the map afterwards (map_file action save).";
            const auto& settings = Spec::Schema().at("properties").at("settings").at("properties");
            t.fields = {{"level", {"level"}, "Level Properties, merged field by field."},
                        {"interface", {"interface"}, "Interface images by slot: menu, briefing, loadingAtt, loadingDef."},
                        {"mapSettings", {"settings"}, "All the Map Settings as one object (the spec's settings section).", "id", false},
                        {"settings", {"settings"}, "The same as mapSettings.", "id", false},
                        {"modes", {"settings", "modes"}, "Map Settings per mode, matched by mode.", "mode", false}};
            for (const auto& [key, text] : std::vector<std::pair<std::string, std::string>>{
                     {"mapName", "Map Settings MapName, in capitals (it also names a new spec)."},
                     {"briefSpy", "Map Settings ExpliMapInBriefSpy: the Spy brief, shared by every mode."},
                     {"briefMerc", "Map Settings ExpliMapInBriefSentry: the Merc brief, shared by every mode."},
                     {"legend", "Map Settings LegendMap: date, place and a one-line story; | breaks lines."},
                     {"legendInBrief", "Map Settings LegendMapInBrief: the numbered minimap zones; | breaks lines."}})
            {
                auto schema = settings.at(key);
                schema["description"] = text;
                t.extra[key] = schema;
            }
            t.extra["removeModes"] = S::Array(S::Enum({"story", "diskhunt", "deathmatch"}, "A mode."), "Map Settings modes to drop.");
            t.extra["minimap"] = {{"anyOf", Json::array({{{"type", "boolean"}}, S::Free("minimap.apply options.")})},
                                  {"description", "Set the in-game minimap from the built geometry: true, or {floors, framing, style, objectives, spawns, meshes, briefing, briefingFloor}."}};
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
        t.description = intent.description + kHow;
        t.inputSchema = InputSchema(intent);
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        auto shared = std::make_shared<Intent>(std::move(intent));
        t.run = [shared](const Json& a, ToolContext& c) { return Guarded([&] { return Run(*shared, a, c); }); };
        registry.Add(std::move(t));
    }
}
}
