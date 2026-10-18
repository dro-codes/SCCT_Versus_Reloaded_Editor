// Checking a map against the wiki: map_validate, over the open map or a level
// spec compiled offline. The rules themselves are LevelRulesModel.h.
#include "Common.h"
#include "../LevelRulesModel.h"
#if !defined(RE_MCP_STANDALONE) && __has_include("../LevelSpecCompiler.h")
#include "../LevelSpecCompiler.h"
#define RE_MCP_SPEC 1
#endif

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;
    using Workflow::Vector;

    const std::vector<std::string>& Groups()
    {
        static const std::vector<std::string> groups{"wiki", "design", "security", "mapcheck", "lighting", "release"};
        return groups;
    }
    // A bridge read that may fail without failing the check: what went wrong
    // is noted in sources, and the rules that need it stay quiet.
    Json Optional(ToolContext& c, Json& sources, const char* key, const std::string& op, const Json& args = Json::object(), unsigned timeoutMs = 120000)
    {
        try
        {
            auto result = Call(c, op, args, {}, false, timeoutMs);
            sources[key] = op;
            return result;
        }
        catch (const Bridge::Error& e)
        {
            // An editor that went away or is busy fails the whole check.
            if (e.code == Bridge::Code::NotConnected || e.code == Bridge::Code::Busy || e.code == Bridge::Code::Timeout) throw;
            sources[key] = op + " failed: " + e.what();
        }
        catch (const std::exception& e) { sources[key] = op + " failed: " + e.what(); }
        return Json();
    }
    // Every live actor with the properties the rules read, a page at a time.
    Json LiveActors(ToolContext& c, Json& sources)
    {
        Json actors = Json::array();
        Json query{{"fields", Rules::RuleFields()}, {"limit", 5000}};
        for (int page = 0; page < 40; ++page)
        {
            const auto result = Call(c, "actors.query", query, {}, false, 300000);
            for (const auto& a : result.value("actors", Json::array())) actors.push_back(a);
            if (!result.contains("next") || result.at("next").is_null()) break;
            query["cursor"] = result.at("next");
            c.progress(static_cast<double>(actors.size()), result.value("total", 0.0), "Reading actors for map_validate");
        }
        sources["actors"] = actors.size();
        return actors;
    }

    // The built level under what the rules measure: the floor under each
    // terminal and disk and whether geometry touches it (a few units out to
    // each side), and the floor under each GE's probe points (Ge::Probes).
    void Trace(ToolContext& c, Json& actors, Json& ges, Json& sources)
    {
        struct Probe { size_t index; int kind; Vector at; }; // kind 0 floor, 1 side, 2 GE floor
        Json segments = Json::array();
        std::vector<Probe> probes;
        auto down = [](const Vector& p) { return Json{{"from", p}, {"to", Vector{p[0], p[1], p[2] - 4096}}}; };
        for (size_t i = 0; i < actors.size(); ++i)
        {
            const auto cls = Lower(LastSegment(actors[i].value("class", std::string())));
            const double side = cls == "scomputerobjectivetrigger" ? 4 : cls == "sdisk" ? 12 : 0;
            if (side <= 0 || !actors[i].contains("position")) continue;
            Vector p{};
            try { p = Ge::Point(actors[i].at("position"), "position"); }
            catch (const std::exception&) { continue; }
            segments.push_back(down(p));
            probes.push_back({i, 0, p});
            for (const auto& d : {Vector{side, 0, 0}, Vector{-side, 0, 0}, Vector{0, side, 0}, Vector{0, -side, 0}})
            {
                segments.push_back({{"from", p}, {"to", Vector{p[0] + d[0], p[1] + d[1], p[2] + d[2]}}});
                probes.push_back({i, 1, p});
            }
        }
        Json* elements = ges.is_object() && ges.contains("elements") ? &ges["elements"] : &ges;
        if (elements->is_array())
            for (size_t i = 0; i < elements->size(); ++i)
                try
                {
                    for (const auto& p : Ge::Probes(Ge::FromJson((*elements)[i])))
                    {
                        segments.push_back(down(p));
                        probes.push_back({i, 2, p});
                    }
                }
                catch (const std::exception&) {}
        if (segments.empty()) return;
        auto trace = [&](const Json& list)
        {
            Json hits = Json::array();
            for (size_t at = 0; at < list.size(); at += 4096)
            {
                const auto chunk = Json(list.begin() + static_cast<std::ptrdiff_t>(at), list.begin() + static_cast<std::ptrdiff_t>(std::min(list.size(), at + 4096)));
                const auto result = Call(c, "scene.trace", {{"segments", chunk}}, {}, false, 300000);
                for (const auto& h : result.is_object() ? result.value("hits", Json::array()) : result) hits.push_back(h);
            }
            return hits;
        };
        Json hits;
        try { hits = trace(segments); }
        catch (const Bridge::Error& e)
        {
            if (e.code == Bridge::Code::NotConnected || e.code == Bridge::Code::Busy) throw;
            sources["trace"] = std::string("scene.trace failed: ") + e.what();
            return;
        }
        catch (const std::exception& e) { sources["trace"] = std::string("scene.trace failed: ") + e.what(); return; }
        if (hits.size() != probes.size()) { sources["trace"] = "scene.trace answered " + std::to_string(hits.size()) + " of " + std::to_string(probes.size()) + " segments"; return; }
        auto self = [&](const Json& hit, size_t index)
        {
            const auto actor = hit.value("actor", Json());
            return actor.is_object() && Lower(actor.value("path", std::string())) == Lower(actors[index].value("path", std::string()));
        };
        // A trace that starts inside the actor's own collision finds the actor:
        // those go again from just below it.
        Json again = Json::array();
        std::vector<size_t> againFor;
        for (size_t k = 0; k < probes.size(); ++k)
        {
            const auto& probe = probes[k];
            const auto& hit = hits[k];
            const bool found = hit.value("hit", false);
            if (probe.kind == 2)
            {
                auto& element = (*elements)[probe.index];
                if (!element.contains("$floors")) element["$floors"] = Json::array();
                element["$floors"].push_back({{"at", probe.at}, {"floor", found && hit.contains("at") ? hit.at("at")[2] : Json()}});
                continue;
            }
            auto& values = actors[probe.index]["values"];
            if (!values.is_object()) values = Json::object();
            if (probe.kind == 1)
            {
                if (!values.contains("$blocked")) values["$blocked"] = false;
                if (found && !self(hit, probe.index)) values["$blocked"] = true;
                continue;
            }
            if (!found) continue;
            if (self(hit, probe.index) && hit.contains("at"))
            {
                const Vector below{probe.at[0], probe.at[1], hit.at("at")[2].get<double>() - 1};
                again.push_back(down(below));
                againFor.push_back(probe.index);
            }
            else if (hit.contains("at")) values["$floor"] = hit.at("at")[2];
        }
        if (!again.empty())
            try
            {
                const auto second = trace(again);
                for (size_t k = 0; k < second.size() && k < againFor.size(); ++k)
                    if (second[k].value("hit", false) && second[k].contains("at")) actors[againFor[k]]["values"]["$floor"] = second[k].at("at")[2];
            }
            catch (const std::exception&) {}
        sources["trace"] = std::to_string(segments.size() + again.size()) + " segments traced";
    }

#ifdef RE_MCP_SPEC
    // Map Design pieces for the live map: the level spec level_build applied
    // last (the sidecar), compiled again. The bridge has no op that reads the
    // Map Design library's pieces themselves.
    Json SidecarScene(ToolContext& c, Json& sources)
    {
        const auto sidecar = Optional(c, sources, "design", "mcp.sidecar.get");
        const auto stored = sidecar.is_object() ? sidecar.value("sidecar", Json()) : Json();
        if (!stored.is_object())
        {
            if (sources.value("design", std::string()) == "mcp.sidecar.get") sources["design"] = "no level spec has been applied to this map, so there are no pieces to check";
            return Json();
        }
        const auto spec = stored.contains("spec") ? stored.at("spec") : stored;
        try
        {
            auto compiled = Spec::Compile(spec);
            sources["design"] = "pieces from the level spec level_build last applied (edits made by hand since are not seen)";
            return compiled.scene;
        }
        catch (const std::exception& e) { sources["design"] = std::string("the applied level spec no longer compiles: ") + e.what(); }
        return Json();
    }
#endif
    // The scct.map-changes documents a compiled plan applies, for Authoring::Validate.
    void PlanDocuments(const Json& plan, Json& scene)
    {
        if (scene.contains("documents") && scene.at("documents").is_array() && !scene.at("documents").empty()) return;
        Json documents = Json::array();
        for (const char* section : {"pre", "steps"})
            for (const auto& step : plan.is_object() ? plan.value(section, Json::array()) : Json::array())
                if (step.value("op", std::string()) == "authoring.apply" && step.contains("args") && step.at("args").contains("document")) documents.push_back(step.at("args").at("document"));
        if (!documents.empty()) scene["documents"] = documents;
    }

    bool Selected(const std::string& rule, const std::vector<std::string>& only)
    {
        if (only.empty()) return true;
        for (auto prefix : only)
        {
            if (rule == prefix) return true;
            if (!prefix.empty() && prefix.back() != '.') prefix += '.';
            if (rule.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }

    ToolResult Validate(const Json& a, ToolContext& c)
    {
        std::string profile = String(a, "profile");
        if (!profile.empty() && profile != "draft" && profile != "release") throw std::runtime_error("profile is draft or release.");
        std::set<std::string> include;
        for (const auto& group : Strings(a, "include"))
        {
            if (std::find(Groups().begin(), Groups().end(), group) == Groups().end())
                throw std::runtime_error("include takes " + Join(Groups()) + "; \"" + group + "\" is not one of them.");
            include.insert(group);
        }
        if (include.empty()) include.insert(Groups().begin(), Groups().end());
        const auto only = Strings(a, "rules");
        const bool withInfo = Bool(a, "info", true);
        const auto limit = static_cast<size_t>(Integer(a, "limit", 200, 1, 2000));
        for (const auto& mode : Strings(a, "modes"))
            if (mode != "story" && mode != "diskhunt" && mode != "deathmatch") throw std::runtime_error("modes are story, diskhunt and deathmatch.");

        Json sources = Json::object();
        Json compiled = Json::array(); // compiler diagnostics, offline
        Rules::Scene scene;
        std::string source;
        if (Has(a, "spec") || Has(a, "scene"))
        {
            Json sceneJson;
            if (Has(a, "scene"))
            {
                sceneJson = Object(a, "scene");
                source = "scene";
            }
            else
            {
#ifdef RE_MCP_SPEC
                const auto spec = Object(a, "spec");
                Spec::Compiled result;
                try { result = Spec::Compile(spec); }
                catch (const Spec::SpecError& e)
                {
                    return Fail(std::string("The level spec does not compile: ") + e.what(), {{"pointer", e.pointer}, {"next", Json::array({Next("level_spec_validate", "See every problem in the spec, with its JSON pointer.")})}});
                }
                sceneJson = result.scene;
                PlanDocuments(result.plan, sceneJson);
                source = "spec";
                if (profile.empty() && spec.contains("profile") && spec.at("profile").is_string()) profile = spec.at("profile").get<std::string>();
                for (const auto& d : result.diagnostics)
                {
                    Json f{{"severity", d.severity}, {"rule", d.rule}, {"wikiPage", d.wikiPage}, {"message", d.message}, {"actors", Json::array()}, {"source", "compiler"}};
                    if (!d.pointer.empty()) f["pointer"] = d.pointer;
                    if (!d.fix.is_null()) f["fix"] = d.fix;
                    compiled.push_back(f);
                }
                sources["spec"] = "compiled offline";
#else
                return Fail("This build of Reloaded.Editor.Mcp has no level-spec compiler, so a spec cannot be checked offline. Check the open map (leave spec out) or pass a rules scene.");
#endif
            }
            if (Has(a, "modes")) sceneJson["modes"] = a.at("modes");
            if (Has(a, "release")) sceneJson["release"] = Object(a, "release");
            // The editor's schema, when one is connected, catches values Map JSON cannot write.
            if (include.count("wiki") && !sceneJson.contains("schema") && c.bridge.Connected())
            {
                std::set<std::string> classes;
                for (const auto& actor : sceneJson.value("actors", Json::array()))
                    if (const auto cls = actor.value("class", std::string()); cls.find('.') != std::string::npos && classes.size() < 512) classes.insert(cls);
                if (!classes.empty())
                    try
                    {
                        sceneJson["schema"] = Call(c, "authoring.schema", {{"classes", Json(classes)}});
                        sources["schema"] = "authoring.schema";
                    }
                    catch (const std::exception& e) { sources["schema"] = std::string("authoring.schema failed: ") + e.what(); }
            }
            scene = Rules::SceneFromJson(sceneJson);
        }
        else
        {
            source = "live";
            const auto state = Call(c, "editor.state");
            if (state.is_object() && state.contains("open") && state.at("open") == false)
                return Fail("No map is open in the editor. Open one (map_file action open), or check a level spec offline by passing spec.",
                            {{"next", Json::array({Next("map_file", "Open a map.", {{"action", "open"}})})}});
            Json actors = LiveActors(c, sources);
            Json extra = Json::object();
            extra["mapFile"] = state.value("mapFile", std::string());
            // Built unless something says otherwise: editor.state's build count,
            // Map Check's BSP state, or the Map Design window's stale flag.
            bool built = true;
            if (state.contains("geometryBuilds") && state.at("geometryBuilds").is_number()) built = state.at("geometryBuilds").get<double>() > 0;
            if (include.count("wiki") || include.count("security"))
            {
                extra["level"] = Optional(c, sources, "level", "settings.inspect");
                extra["spawns"] = Optional(c, sources, "spawns", "design.spawns");
            }
            Json ges = include.count("wiki") || include.count("design") ? Optional(c, sources, "ges", "ge.list") : Json();
            if (include.count("release")) extra["mapSettings"] = Optional(c, sources, "mapSettings", "mapsettings.get");
#ifdef RE_MCP_SPEC
            if (include.count("design") || !Has(a, "modes"))
                if (const auto applied = SidecarScene(c, sources); applied.is_object())
                {
                    if (include.count("design")) extra["pieces"] = applied.value("pieces", Json::array());
                    extra["modes"] = applied.value("modes", Json::array());
                    extra["mapName"] = applied.value("mapName", std::string());
                }
#else
            sources["design"] = "this build has no level-spec compiler, so no Map Design pieces are checked";
#endif
            if (include.count("mapcheck"))
            {
                extra["mapCheck"] = Optional(c, sources, "mapCheck", "mapcheck.run", Json::object(), 300000);
                if (extra["mapCheck"].is_object() && extra["mapCheck"].contains("summary")) built = extra["mapCheck"]["summary"].value("bspBuilt", built);
            }
            if (include.count("lighting")) extra["lighting"] = Optional(c, sources, "lighting", "lighting.budget", Json::object(), 300000);
            // The Map Design window knows when brushes changed since its last build.
            if (include.count("design"))
                try
                {
                    const auto view = Call(c, "design.view");
                    if (view.is_object() && view.value("open", false) && view.value("stale", false)) built = false;
                }
                catch (const std::exception&) {}
            extra["geometryBuilt"] = built;
            if (include.count("wiki") && Bool(a, "trace", true))
            {
                if (built) Trace(c, actors, ges, sources);
                else sources["trace"] = "skipped: the geometry is not built, so heights and clearances under terminals, disks and GEs are not measured";
            }
            extra["ges"] = ges.is_null() ? Json::array() : ges;
            if (Has(a, "modes")) extra["modes"] = a.at("modes");
            if (Has(a, "release")) extra["release"] = Object(a, "release");
            scene = Rules::SceneFromQuery(actors, extra);
        }
        if (profile.empty()) profile = "draft";

        const auto findings = Rules::Check(scene, profile);
        std::set<std::pair<std::string, std::string>> seen;
        Json selected = Json::array();
        for (const auto& f : findings)
        {
            seen.insert({f.rule, f.message});
            if (!include.count(Rules::Group(f.rule)) || !Selected(f.rule, only) || (!withInfo && f.severity == "info")) continue;
            selected.push_back(Rules::ToJson(f));
        }
        // What the compiler found in the spec itself. It runs these rules on
        // the scene too: those copies are the findings above (in this
        // profile), so they are not repeated, nor brought back when include
        // left their group out.
        for (const auto& d : compiled)
        {
            const auto rule = d.value("rule", std::string());
            if (seen.count({rule, d.value("message", std::string())})) continue;
            if (Rules::FindRule(rule) && !include.count(Rules::Group(rule))) continue;
            if (!Selected(rule, only) || (!withInfo && d.value("severity", std::string()) == "info")) continue;
            selected.push_back(d);
        }
        auto rank = [](const Json& f) { const auto s = f.value("severity", std::string()); return s == "error" ? 0 : s == "warning" ? 1 : 2; };
        std::vector<Json> ordered(selected.begin(), selected.end());
        std::stable_sort(ordered.begin(), ordered.end(), [&](const Json& x, const Json& y) { return rank(x) < rank(y); });
        size_t errors = 0, warnings = 0, notes = 0;
        for (const auto& f : ordered) (rank(f) == 0 ? errors : rank(f) == 1 ? warnings : notes) += 1;
        Json shown = Json::array();
        for (size_t i = 0; i < ordered.size() && i < limit; ++i) shown.push_back(ordered[i]);

        Json next = Json::array();
        std::set<std::string> tools;
        for (const auto& f : shown)
        {
            if (rank(f) > 0 || next.size() >= 3) break;
            const auto tool = f.contains("fix") ? f.at("fix").value("tool", std::string()) : std::string();
            if (!tool.empty() && tools.insert(tool).second) next.push_back(Next(tool, "Fixes " + f.value("rule", std::string()) + " (see that finding's fix)."));
        }
        if (errors || warnings) next.push_back(Next("map_validate", "Check again after the fixes.", {{"profile", profile}}));
        else if (profile == "draft") next.push_back(Next("map_validate", "No errors or warnings in draft; check against the release profile before sharing the map.", {{"profile", "release"}}));

        const std::string what = source == "live" ? "the open map" : source == "spec" ? "the level spec (offline)" : "the scene";
        std::string summary = (profile == "release" ? "Release" : "Draft") + std::string(" check of ") + what + ": " + Count(errors, "error") + ", " + Count(warnings, "warning") + ", " + Count(notes, "note") + ".";
        if (ordered.size() > shown.size()) summary += " Showing the first " + std::to_string(shown.size()) + " (errors first); raise limit or narrow include or rules for the rest.";
        if (errors) summary += " Fix the errors first; each finding's fix names the tool and arguments, and wikiPage the scct://wiki page behind the rule.";
        else if (!warnings) summary += " Nothing to fix.";
        Json result{{"summary", {{"errors", errors}, {"warnings", warnings}, {"info", notes}, {"profile", profile}, {"source", source}, {"total", ordered.size()}, {"shown", shown.size()},
                                 {"include", Json(include)}, {"actors", scene.actors.size()}, {"pieces", scene.pieces.size()}, {"ges", scene.ges.size()}}},
                    {"findings", shown}, {"sources", sources}, {"next", next}};
        return Ok(result, summary);
    }
}

void RegisterValidateTools(Registry& registry)
{
    Tool t;
    t.name = "map_validate";
    t.title = "Check the map against the wiki";
    t.description =
        "Checks a Versus map against the SCCT Versus Reloaded wiki and returns what to fix. Read-only. Without spec it checks the map open in the editor "
        "(it reads the actors, Level Properties, Map Settings and GEs, runs Map Check and the lighting budget, and traces the built level for floor heights); "
        "with spec it compiles that scct.level-spec offline and checks what level_build would make, with no editor needed.\n"
        "What it checks (scct://rules lists every rule with its severities):\n"
        "- spawns: PlayerStarts for both teams (TeamNumber 0 spy, 1 merc), at least 3 each; spy starts not within 512 units of an objective, the teams not within 768 "
        "of each other, every objective within 4096 of a merc start; starts out of solid geometry; objectives the spies can reach through the blockout;\n"
        "- Story / Disk Hunt / Deathmatch: an SMission per mode (GameMode GM_Multi, GM_DiskHunt, GM_DeathMatch); Objectives and Triggers naming real actors of the right "
        "kind; no SObjective outside a mission; MinimumObjectives reachable; sector chains (top mission bChained True over GM_Undefined sectors); SFlag with 1-5 drop zones; "
        "SComputerObjectiveTrigger about 78 units above the floor, clear of collision, using DominationCaptureTime; SDisk Group 1-3, listed in the Disk Hunt mission, "
        "enough for ScoreLimit and Handicap; Deathmatch AtResetEvent switching security off; objective player counts;\n"
        "- CamNet: at least one SCamNetwork (mercs softlock without), exactly one bFirstCam, one closed NextCam/PrevCam loop, collision on;\n"
        "- security: detectors' Event reaching an SAlarm Tag, motion sensors with volumes, alarms fed and doing something, no SBombTargetObjectiveTrigger locked, "
        "MessagePriority 0-5, no SAlarmZoneInfo;\n"
        "- other actors: LightEffect LE_None/LE_Spotlight/LE_Sunlight (LE_ESpotShadow for Echelon), no bPickable, no duplicated SoftBody, movers with a static mesh, "
        "hidden tutorial and sprinkler sprites, ingredient collision and interaction sizes, Events that reach a Tag, fDeathPawnHeight and level SendAtTime;\n"
        "- release: Map Settings (created, MapName in capitals, RespawnTime 5-15, lives 0-10, TimeLimit 0-20, Modes and ModesStrings aligned), the <Map>-i interface "
        "textures (Menu/Briefing 256 DXT5, LoadingATT0/DEF0 512 DXT1), the map file name and release package names and .uas contents;\n"
        "- geometric events against the wiki's measurements: ledge grab 208-320 standing, 448 running, 512 co-op, 115 obstacle, 64 pull-up, 32 long; fence 88-128 "
        "(96 best); hand-over-hand 216 clearance; ladders an even number of bars, at least 8 (32 units a bar); zip line at most 384 up with 198 clearance (161 tucked); "
        "pipes and ladders from the lower end;\n"
        "- folded in: the Map Design check on blockout pieces (layout, traversal), Map Check rows (zone leaks among them) and the lighting budget.\n"
        "Each finding has severity (error, warning or info), rule, wikiPage (read scct://wiki/<page>), message, actors (paths, or spec ids offline) and usually fix: "
        "{tool, args, note?}, the tool call that repairs it (actors_edit edits on the live map; gameplay, security, environment, map_settings, geometric_events, "
        "blockout, map_build or release fragments otherwise). Findings come errors first; summary counts them all, findings shows up to limit.\n"
        "profile draft (default; a spec's own profile when spec is given) suits a map in progress; release is stricter (3 starts per team, Map Settings, interface "
        "textures, isolated rooms and unreachable objectives become errors). include runs only some groups, and makes only their editor calls: wiki, design, "
        "security, mapcheck, lighting, release. Fix the errors, then call map_validate again.";
    t.inputSchema = S::Input(
        {{"profile", S::Enum({"draft", "release"}, "Severities: draft for a map in progress (default), release before sharing it.")},
         {"include", S::Array(S::Enum(Groups(), "A group of checks."),
                              "Groups to run (default all): wiki (spawns, modes, objectives, CamNet, actors, level, GEs), design (Map Design pieces: layout, traversal, reachability), "
                              "security (alarms and detectors), mapcheck (the editor's Map Check, zone leaks), lighting (the lighting budget), release (Map Settings, interface "
                              "textures, file and package names).")},
         {"spec", S::Free("A scct.level-spec to check offline instead of the open map (scct://schema/level-spec).")},
         {"scene", S::Free("Advanced: a rules scene to check as it is (the shape scct://rules and LevelRulesModel.h describe).")},
         {"modes", S::Array(S::Enum({"story", "diskhunt", "deathmatch"}, "A game mode."), "The modes the map must play. Default: the spec's gameplay.modes, or Story.")},
         {"release", S::Free("Release facts to check: {packages: [name | {name, stock}], uas: {packages: [...]}, interface: {menu, briefing, loadingAtt, loadingDef}}.")},
         {"rules", S::Array({{"type", "string"}}, "Only these rules: ids such as camnet.loop, or prefixes such as ge, story or spawns.")},
         {"info", S::Boolean("Include info findings (summaries, travel times, recommendations). Default true.")},
         {"trace", S::Boolean("Live map: trace the built level for the floors under terminals, disks and GEs, which heights and clearances need. Default true.")},
         {"limit", S::Integer("Most findings to return, errors first. Default 200.", 1, 2000)}});
    Json finding{{"type", "object"},
                 {"properties",
                  {{"severity", S::Enum({"error", "warning", "info"}, "How much it matters in this profile.")},
                   {"rule", S::String("The rule id (scct://rules).")},
                   {"wikiPage", S::String("The wiki page behind the rule: read scct://wiki/<page>.")},
                   {"message", S::String("What is wrong and why.")},
                   {"actors", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Actor paths (spec ids offline)."}}},
                   {"fix", {{"type", "object"}, {"description", "The tool call that repairs it: {tool, args, note?}."}}},
                   {"pointer", S::String("Offline: the JSON pointer into the spec.")}}},
                 {"required", {"severity", "rule", "wikiPage", "message"}}};
    t.outputSchema = {{"type", "object"},
                      {"properties",
                       {{"summary", {{"type", "object"},
                                     {"properties", {{"errors", {{"type", "integer"}}}, {"warnings", {{"type", "integer"}}}, {"info", {{"type", "integer"}}}, {"profile", {{"type", "string"}}},
                                                     {"source", {{"type", "string"}}}, {"total", {{"type", "integer"}}}, {"shown", {{"type", "integer"}}}}},
                                     {"required", {"errors", "warnings", "info"}}}},
                        {"findings", {{"type", "array"}, {"items", finding}}},
                        {"sources", {{"type", "object"}, {"description", "What was read for the check, and what could not be."}}},
                        {"next", {{"type", "array"}}}}},
                      {"required", {"summary", "findings"}}};
    t.readOnly = true;
    t.idempotent = true;
    t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return Validate(a, c); }); };
    registry.Add(std::move(t));
}
}
