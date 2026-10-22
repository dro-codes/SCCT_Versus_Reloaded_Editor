// Reading the map: map_query, view_screenshot.
#include "Common.h"

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;

    // The first limit items; meta says how many there were.
    Json Page(const Json& list, size_t limit, Json& meta)
    {
        if (!list.is_array()) return list;
        meta["total"] = list.size();
        if (list.size() <= limit) return list;
        meta["truncated"] = true;
        meta["shown"] = limit;
        return Json(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(limit));
    }
    Json WithoutSchema(Json inspected, bool schema)
    {
        if (!schema && inspected.is_object()) inspected.erase("schema");
        return inspected;
    }
    std::string Team(const std::string& team) { return team == "0" ? "spy" : team == "1" ? "merc" : "team " + team; }
    // Every live actor as actors.query returns them, a page at a time.
    Json AllActors(ToolContext& c, Json query, size_t cap, Json& meta)
    {
        Json actors = Json::array();
        query["limit"] = 5000;
        for (;;)
        {
            const auto page = Call(c, "actors.query", query);
            for (const auto& a : page.value("actors", Json::array())) actors.push_back(a);
            meta["total"] = page.value("total", actors.size());
            if (!page.contains("next") || page.at("next").is_null() || actors.size() >= cap) break;
            query["cursor"] = page.at("next");
        }
        if (actors.size() > cap) { meta["truncated"] = true; actors.erase(actors.begin() + static_cast<std::ptrdiff_t>(cap), actors.end()); }
        return actors;
    }
    Json Filters(const Json& a)
    {
        Json q = Json::object();
        for (const char* key : {"classes", "tags", "names", "near", "box", "selected", "fields", "cursor"}) if (Has(a, key)) q[key] = a.at(key);
        return q;
    }
    Json Bounds(const Json& edges)
    {
        Workflow::Vector low{}, high{};
        bool any = false;
        for (const auto& edge : edges)
            for (const auto& point : edge)
            {
                if (!point.is_array() || point.size() != 3) continue;
                for (size_t k = 0; k < 3; ++k)
                {
                    const double v = point[k].get<double>();
                    if (!any) low[k] = high[k] = v;
                    else { low[k] = std::min(low[k], v); high[k] = std::max(high[k], v); }
                }
                any = true;
            }
        return any ? Json{{"min", low}, {"max", high}} : Json();
    }
    // design.scene without its wireframes: each brush's bounds instead.
    Json DesignItems(const Json& scene)
    {
        Json out = Json::array();
        for (auto item : scene)
        {
            if (item.contains("edges"))
            {
                const auto bounds = Bounds(item.at("edges"));
                if (!bounds.is_null()) item["bounds"] = bounds;
                item["edgeCount"] = item.at("edges").size();
                item.erase("edges");
            }
            out.push_back(std::move(item));
        }
        return out;
    }

    // ------------------------------------------------------------ map_query
    ToolResult Query(const Json& a, ToolContext& c)
    {
        const auto what = RequireString(a, "what");
        const auto limit = static_cast<size_t>(Integer(a, "limit", what == "actors" ? 200 : 500, 1, 5000));
        const bool schema = Bool(a, "includeSchema");
        Json meta = Json::object();
        if (what == "summary")
        {
            auto summary = MapSummary(c);
            std::string text = "No map is open.";
            if (summary.value("open", false))
            {
                const auto& m = summary.at("map");
                text = (m.value("file", std::string()).empty() ? std::string("Unsaved map") : m.value("file", std::string())) + (m.value("dirty", false) ? " (unsaved changes)" : "") +
                       ": " + Count(summary.at("actors").value("total", size_t{0}), "actor") + "; spawns " + std::to_string(summary.at("spawns").value("spy", 0)) + " spy / " +
                       std::to_string(summary.at("spawns").value("merc", 0)) + " merc; " + Count(summary.at("story").value("missions", Json::array()).size(), "mission") + ", " +
                       Count(summary.at("story").value("objectives", size_t{0}), "objective") + "; " + Count(summary.at("cameras").value("count", size_t{0}), "CamNet camera") + ".";
            }
            return Ok(summary, text);
        }
        if (what == "actors")
        {
            auto q = Filters(a);
            q["limit"] = limit;
            const auto r = Call(c, "actors.query", q);
            const auto shown = r.value("actors", Json::array()).size();
            std::string text = "Showing " + std::to_string(shown) + " of " + std::to_string(r.value("total", shown)) + " matching actors";
            if (r.contains("next") && !r.at("next").is_null()) text += "; pass cursor " + r.at("next").dump() + " for the next page";
            return Ok(r, text + ".");
        }
        if (what == "actor")
        {
            if (!Has(a, "actor")) return Fail("what actor needs actor: a path such as MyLevel.Light12, an object name, or {path, class}.");
            const auto identity = Resolve(c, Json::array({a.at("actor")})).at(0);
            auto inspected = WithoutSchema(Call(c, "magic.inspect", {{"actor", identity}}), schema);
            try
            {
                auto links = Call(c, "connections", {{"actor", identity.at("path")}});
                Json linkMeta = Json::object();
                inspected["links"] = Page(links, 100, linkMeta);
                if (linkMeta.value("truncated", false)) inspected["linksTotal"] = linkMeta.at("total");
            }
            catch (const std::exception& e) { inspected["linksError"] = e.what(); }
            return Ok(inspected, identity.at("path").get<std::string>() + " (" + identity.at("class").get<std::string>() + "): " +
                                     Count(inspected.value("values", Json::object()).size(), "editable property", "editable properties") +
                                     ". Edit with actors_edit (action set); values are the before every edit must match.");
        }
        if (what == "level")
        {
            auto settings = WithoutSchema(Call(c, "settings.inspect"), schema);
            return Ok(settings, "The LevelInfo's editable properties (Level Properties in the wiki: level-properties).");
        }
        if (what == "schema")
        {
            const auto classes = Strings(a, "classes");
            if (classes.empty()) return Fail("what schema needs classes, such as [\"SBase.SMission\", \"Engine.PlayerStart\"].");
            const auto r = Call(c, "authoring.schema", {{"classes", classes}});
            return Ok(r, "Property schemas of " + Join(classes) + ": kinds, enum choices and whether Map JSON can write each property.");
        }
        if (what == "assets")
        {
            const auto kind = String(a, "kind", "staticmesh");
            Json q{{"kind", kind}, {"limit", limit}};
            if (Has(a, "query")) q["query"] = String(a, "query");
            const auto r = Call(c, "assets.search", q);
            return Ok(r, Count(r.value("assets", Json::array()).size(), "loaded " + kind + " asset") + (Has(a, "query") ? " matching \"" + String(a, "query") + "\"" : std::string()) +
                             ". Only loaded packages are searched; load more with a map-changes load operation.");
        }
        if (what == "wiring")
        {
            std::string path;
            if (Has(a, "actor")) path = Resolve(c, Json::array({a.at("actor")})).at(0).at("path").get<std::string>();
            const auto rows = Call(c, "connections", {{"actor", path}});
            size_t unresolved = 0;
            for (const auto& row : rows) unresolved += !row.value("resolved", true);
            auto shown = Page(rows, limit, meta);
            return Ok({{"links", shown}, {"meta", meta}, {"unresolved", unresolved}},
                      Count(rows.size(), "link") + (path.empty() ? "" : " touching " + path) + ", " + std::to_string(unresolved) +
                      " unresolved (an Event no Tag answers, or a reference to a missing actor; the wiki: events).");
        }
        if (what == "spawns")
        {
            Json teams{{"spy", Json::array()}, {"merc", Json::array()}, {"other", Json::array()}};
            for (const auto& item : Call(c, "objective.actors"))
            {
                if (item.value("kind", std::string()) != "Player start") continue;
                const auto team = Team(item.value("team", std::string()));
                teams[team == "spy" || team == "merc" ? team : "other"].push_back(item);
            }
            return Ok(teams, std::to_string(teams["spy"].size()) + " spy and " + std::to_string(teams["merc"].size()) +
                                 " merc player starts. The wiki (spawn-points): PlayerStart TeamNumber 0 is Spy and 1 is Mercenary, with at least 3 per team so 2v2 and 3v1 matches can spawn; STriggerPlayerStart is switched by its Tag.");
        }
        if (what == "missions")
        {
            Json out{{"missions", Json::array()}, {"objectives", Json::array()}, {"triggers", Json::array()}};
            for (const auto& item : Call(c, "objective.actors"))
            {
                const auto kind = item.value("kind", std::string());
                if (kind == "Player start") continue;
                out[kind == "Mission" ? "missions" : kind == "Objective" ? "objectives" : "triggers"].push_back(item);
            }
            try { out["stages"] = Call(c, "stage.read"); } catch (const std::exception& e) { out["stagesError"] = e.what(); }
            return Ok(out, Count(out["missions"].size(), "mission") + ", " + Count(out["objectives"].size(), "objective") + ", " + Count(out["triggers"].size(), "objective device") +
                               ". The wiki (game-modes): Story needs an SMission with GameMode GM_Multi whose Objectives list each SObjective by exact name; each SObjective's Triggers list its SComputerObjectiveTrigger, SFlag or SBombTargetObjectiveTrigger by exact name.");
        }
        if (what == "security")
        {
            const auto items = Call(c, "security.actors");
            std::map<std::string, size_t> kinds;
            for (const auto& item : items) ++kinds[item.value("kind", std::string("?"))];
            Json counts = Json::object();
            for (const auto& [k, n] : kinds) counts[k] = n;
            return Ok({{"actors", Page(items, limit, meta)}, {"counts", counts}, {"meta", meta}},
                      Count(items.size(), "security actor") + ". The wiki (alarms): a detector sends its Event when it sees a player, and the SAlarm whose Tag matches goes off; SBombTargetObjectiveTrigger cannot be locked.");
        }
        if (what == "cameras")
        {
            Json cameras = Json::array();
            size_t first = 0;
            for (const auto& camera : Call(c, "camera.snapshot"))
            {
                cameras.push_back(WithoutSchema(camera, schema));
                first += Lower(camera.value("values", Json::object()).value("bFirstCam", std::string())) == "true";
            }
            return Ok({{"cameras", cameras}, {"firstCams", first}},
                      Count(cameras.size(), "CamNet camera") + ", " + std::to_string(first) +
                      " marked bFirstCam. Mercs need a camera network: at least one SCamNetwork, exactly one bFirstCam, NextCam/PrevCam closing the loop.");
        }
        if (what == "lights")
        {
            const auto lights = Call(c, "light.list");
            return Ok({{"lights", Page(lights, limit, meta)}, {"meta", meta}},
                      Count(lights.size(), "light") + ". The wiki (lighting): static lights use only LE_None, LE_Spotlight or LE_Sunlight.");
        }
        if (what == "stages")
        {
            const auto stages = Call(c, "stage.read");
            return Ok(stages, "Story sectors (scct.stages): zones of objectives that unlock in turn.");
        }
        if (what == "design")
        {
            auto items = DesignItems(Call(c, "design.scene", Json::object(), {}, false, 300000));
            size_t brushes = 0;
            for (const auto& item : items) brushes += item.contains("bounds");
            Json out{{"items", Page(items, limit, meta)}, {"meta", meta}, {"brushes", brushes}};
            try { out["builds"] = Call(c, "design.builds"); } catch (const std::exception&) {}
            return Ok(out, Count(items.size(), "actor") + " in the design view, " + Count(brushes, "brush", "brushes") + " (bounds in Unreal units; csg 1 adds, 2 subtracts).");
        }
        if (what == "ges")
        {
            const auto ges = Call(c, "ge.list");
            return Ok(ges, "Geometric events (ledges, pipes, ladders, ziplines, fences, poles) that Map Design or MCP added. The wiki: geometric-events.");
        }
        return Fail("Unknown what \"" + what + "\".");
    }

    // ------------------------------------------------------ view_screenshot
    ToolResult Screenshot(const Json& a, ToolContext& c)
    {
        const auto preset = String(a, "preset");
        Json q{{"viewport", String(a, "viewport", preset == "overview" ? "top" : "perspective")}, {"maxEdge", Integer(a, "maxEdge", 1024, 64, 2048)}};
        if (Has(a, "camera")) q["camera"] = a.at("camera");
        Json frame = Json::array();
        if (Has(a, "frame")) frame = Resolve(c, List(a, "frame"));
        if (!preset.empty() && preset != "overview")
        {
            const auto colon = preset.find(':');
            const auto kind = preset.substr(0, colon), value = colon == std::string::npos ? std::string() : Lower(preset.substr(colon + 1));
            const auto items = Call(c, "objective.actors");
            for (const auto& item : items)
            {
                bool take = false;
                if (kind == "spawn") take = item.value("kind", std::string()) == "Player start" && Team(item.value("team", std::string())) == value;
                else if (kind == "objective")
                    take = item.value("kind", std::string()) != "Player start" && (Lower(item.value("name", std::string())) == value || Lower(item.value("tag", std::string())) == value ||
                                                                                    Lower(item.value("path", std::string())) == value || Lower(LastSegment(item.value("path", std::string()))) == value);
                else return Fail("preset is overview, spawn:spy, spawn:merc or objective:<name, Tag or path>.");
                if (take) frame.push_back(Identity(item));
            }
            if (frame.empty()) return Fail("Nothing in the map matches preset " + preset + ".");
        }
        if (!frame.empty()) q["frame"] = frame;
        const auto shot = Call(c, "view.screenshot", q, {}, false, 120000);
        ToolResult r;
        r.structured = {{"viewport", shot.value("viewport", q.at("viewport"))}, {"width", shot.value("width", 0)}, {"height", shot.value("height", 0)},
                        {"framed", frame.size()}, {"preset", preset}};
        r.content.push_back(TextContent("Screenshot of the " + r.structured.at("viewport").get<std::string>() + " viewport, " + std::to_string(shot.value("width", 0)) + "x" +
                                        std::to_string(shot.value("height", 0)) + (frame.empty() ? "" : ", framing " + Count(frame.size(), "actor")) + "."));
        r.content.push_back(ImageContent(shot.value("png", std::string()), shot.value("mimeType", std::string("image/png"))));
        return r;
    }
}

Json Tools::MapSummary(ToolContext& c)
{
    const auto state = Call(c, "editor.state");
    if (!state.value("open", true)) return {{"open", false}, {"state", state}};
    Json warnings = Json::array();
    Json summary{{"open", true}};
    summary["map"] = {{"file", state.value("mapFile", std::string())}, {"key", state.value("mapKey", std::string())}, {"authoring", state.value("map", std::string())},
                      {"dirty", state.value("dirty", false)}, {"level", state.value("level", Json())}, {"generation", state.value("generation", Json())},
                      {"revision", state.value("revision", Json())}, {"geometryBuilds", state.value("geometryBuilds", Json())},
                      {"canUndo", state.value("canUndo", false)}, {"canRedo", state.value("canRedo", false)}};
    Json actors{{"total", 0}, {"classes", Json::object()}};
    try
    {
        Json meta = Json::object();
        const auto all = AllActors(c, Json::object(), 40000, meta);
        std::map<std::string, size_t> classes;
        for (const auto& item : all) ++classes[item.value("class", std::string("?"))];
        std::vector<std::pair<std::string, size_t>> sorted(classes.begin(), classes.end());
        std::stable_sort(sorted.begin(), sorted.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
        size_t other = 0;
        for (size_t i = 0; i < sorted.size(); ++i)
            if (i < 40) actors["classes"][sorted[i].first] = sorted[i].second;
            else other += sorted[i].second;
        if (other) actors["classes"]["(other classes)"] = other;
        actors["total"] = meta.value("total", all.size());
    }
    catch (const std::exception& e) { warnings.push_back(std::string("actors.query: ") + e.what()); }
    summary["actors"] = actors;
    Json spawns{{"spy", 0}, {"merc", 0}, {"other", 0}};
    Json story{{"missions", Json::array()}, {"objectives", 0}, {"devices", Json::object()}};
    try
    {
        for (const auto& item : Call(c, "objective.actors"))
        {
            const auto kind = item.value("kind", std::string());
            if (kind == "Player start")
            {
                const auto team = Team(item.value("team", std::string()));
                spawns[team == "spy" || team == "merc" ? team : "other"] = spawns[team == "spy" || team == "merc" ? team : "other"].get<int>() + 1;
            }
            else if (kind == "Mission")
                story["missions"].push_back({{"path", item.value("path", std::string())}, {"name", item.value("name", std::string())}, {"tag", item.value("tag", std::string())},
                                             {"objectives", item.value("objectives", Json::array()).size()}, {"minimum", item.value("minimum", 0)}});
            else if (kind == "Objective") story["objectives"] = story["objectives"].get<int>() + 1;
            else story["devices"][kind] = story["devices"].value(kind, 0) + 1;
        }
    }
    catch (const std::exception& e) { warnings.push_back(std::string("objective.actors: ") + e.what()); }
    summary["spawns"] = spawns;
    summary["story"] = story;
    Json security = Json::object();
    try { for (const auto& item : Call(c, "security.actors")) { const auto kind = item.value("kind", std::string("?")); security[kind] = security.value(kind, 0) + 1; } }
    catch (const std::exception& e) { warnings.push_back(std::string("security.actors: ") + e.what()); }
    summary["security"] = security;
    Json cameras{{"count", 0}, {"firstCams", 0}};
    try
    {
        const auto snapshot = Call(c, "camera.snapshot");
        cameras["count"] = snapshot.size();
        int first = 0;
        for (const auto& camera : snapshot) first += Lower(camera.value("values", Json::object()).value("bFirstCam", std::string())) == "true";
        cameras["firstCams"] = first;
    }
    catch (const std::exception& e) { warnings.push_back(std::string("camera.snapshot: ") + e.what()); }
    summary["cameras"] = cameras;
    Json notes = Json::array();
    if (spawns["spy"].get<int>() < 3 || spawns["merc"].get<int>() < 3) notes.push_back("Fewer than 3 player starts for a team (wiki: spawn-points asks for at least 3 each).");
    if (story["missions"].empty()) notes.push_back("No SMission: Story mode cannot be played (wiki: game-modes).");
    if (cameras["count"].get<int>() == 0) notes.push_back("No SCamNetwork camera: Mercs need a camera network.");
    summary["notes"] = notes;
    summary["warnings"] = warnings;
    return summary;
}

void RegisterQueryTools(Registry& registry)
{
    {
        Tool t;
        t.name = "map_query";
        t.title = "Read the map";
        t.description =
            "Reads the open map without changing it. what:\n"
            "- summary: file, unsaved state, revision, actor counts by class, spawns per team, missions, objectives, security, cameras, quick notes.\n"
            "- actors: live actors filtered by classes (IsA, any of), tags, names, near {at, radius}, box {min, max}, selected; fields [names] or \"all\" adds their "
            "values; paged by limit (default 200) and cursor (the previous result's next).\n"
            "- actor: one actor (path, name or {path, class}) with every editable value and its links (includeSchema: its property schema); these values are the "
            "before that actors_edit and map-changes updates must match.\n"
            "- level (Level Properties), schema (property schemas, enum choices, writability of classes), assets (loaded assets of kind matching query), wiring "
            "(Tag/Event links, with the unresolved ones), spawns, missions, security, cameras, lights, stages (story sectors), design (blockout brushes with "
            "bounds), ges.";
        t.inputSchema = S::Input(
            {{"what", S::Enum({"summary", "actors", "actor", "level", "schema", "assets", "wiring", "spawns", "missions", "security", "cameras", "lights", "stages", "design", "ges"},
                              "What to read.")},
             {"classes", S::Array({{"type", "string"}}, "actors: class names or paths (IsA). schema: the classes.")},
             {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
             {"names", {{"type", "array"}, {"items", {{"type", "string"}}}}},
             {"near", S::Object({{"at", S::Vector3({})}, {"radius", {{"type", "number"}}}}, {"at", "radius"})},
             {"box", S::Object({{"min", S::Vector3({})}, {"max", S::Vector3({})}}, {"min", "max"})},
             {"selected", {{"type", "boolean"}}},
             {"fields", {{"anyOf", Json::array({{{"type", "array"}, {"items", {{"type", "string"}}}}, {{"type", "string"}, {"enum", {"all"}}}})}}},
             {"limit", S::Integer("Most items to return.", 1, 5000)},
             {"cursor", {{"type", "integer"}, {"minimum", 0}}},
             {"actor", S::Actor("actor, wiring: the actor.")},
             {"includeSchema", {{"type", "boolean"}}},
             {"kind", S::Enum({"staticmesh", "material", "texture", "sound", "class"}, "assets: default staticmesh.")},
             {"query", S::String("assets: text in the asset path.")}},
            {"what"});
        t.readOnly = true;
        t.idempotent = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return Query(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "view_screenshot";
        t.title = "Screenshot a viewport";
        t.description =
            "A PNG of an editor viewport, returned as an image. viewport perspective (default) or top; maxEdge caps the longer side (64-2048, default 1024). "
            "camera {location:[x,y,z], rotation:[pitch,yaw,roll]} places the viewport camera for the shot; frame [actors] points it at those actors; presets: "
            "overview (the top view), spawn:spy, spawn:merc (that team's player starts), objective:<name, Tag or path> (an objective and its devices). The "
            "camera is put back afterwards. Needs the editor window restored, not minimised.";
        t.inputSchema = S::Input({{"viewport", S::Enum({"perspective", "top"}, "Default perspective (top for overview).")},
                                  {"maxEdge", S::Integer("Longer side in pixels. Default 1024.", 64, 2048)},
                                  {"camera", S::Object({{"location", S::Vector3({})}, {"rotation", S::Vector3("Pitch, yaw, roll: 65536 per turn.")}})},
                                  {"frame", S::Array(S::Actor({}), "Actors to frame.", 2000)},
                                  {"preset", S::String("overview, spawn:spy, spawn:merc or objective:<name>.")}});
        t.readOnly = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return Screenshot(a, c); }); };
        registry.Add(std::move(t));
    }
}
}
