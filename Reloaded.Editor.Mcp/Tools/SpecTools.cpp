// Level specs: level_spec_validate checks one offline, without the editor,
// and level_build compiles one into a single mcp.plan (one Undo step). Every
// intent tool (IntentTools.cpp) builds through LevelBuild below too, so the
// wiki defaults, the checks and the wiring are the same whichever way a level
// is made. The compiler is LevelSpecCompiler.h, the rules LevelRulesModel.h
// and the plan picture PlanRender.h.
#include "Common.h"
#include "../Embedded.h"
#include "../LevelSpecCompiler.h"
#include "../PlanRender.h"
#include <cstring>

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;
    using Workflow::Fold;

    // A build can take minutes on a big map; build.all far longer.
    constexpr unsigned kBuildTimeout = 900000, kBuildAllTimeout = 3600000;
    constexpr size_t kPreviewRows = 300;
    // The specs LevelBuild saves for level_spec_validate that <work>/specs keeps.
    constexpr size_t kSpecFiles = 20;

    // Deletes all but the newest kSpecFiles .level.json files in folder.
    void PruneSpecs(const std::filesystem::path& folder)
    {
        std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
        std::error_code error;
        for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        {
            std::error_code entry;
            const auto name = PathText(it->path().filename());
            if (name.size() <= 11 || name.compare(name.size() - 11, 11, ".level.json") != 0 || !it->is_regular_file(entry)) continue;
            const auto written = it->last_write_time(entry);
            if (!entry) files.emplace_back(written, it->path());
        }
        if (files.size() <= kSpecFiles) return;
        std::sort(files.begin(), files.end()); // oldest first
        for (size_t i = 0; i + kSpecFiles < files.size(); ++i) std::filesystem::remove(files[i].second, error);
    }

    // A text field that may be missing, null or another type.
    std::string Text(const Json& object, const char* key)
    {
        if (!object.is_object() || !object.contains(key) || !object.at(key).is_string()) return {};
        return object.at(key).get<std::string>();
    }
    int Rank(const std::string& severity) { return severity == "error" ? 0 : severity == "warning" ? 1 : 2; }

    // ------------------------------------------------------------ the spec
    Json LoadSpec(const Json& a, ToolContext& c, std::string& source) { return Tools::LoadSpec(a, c, source, EmbeddedFiles()); }

    // The id of the nearest item on the way down a JSON pointer, so a
    // finding about /security/devices/3/at also names the device.
    std::string IdAt(const Json& spec, const std::string& pointer)
    {
        std::string id;
        const Json* at = &spec;
        size_t start = 1;
        while (start <= pointer.size())
        {
            const auto end = pointer.find('/', start);
            std::string token = pointer.substr(start, end == std::string::npos ? std::string::npos : end - start);
            for (size_t k = 0; (k = token.find('~', k)) != std::string::npos; ++k) token.replace(k, 2, token.compare(k, 2, "~1") == 0 ? "/" : "~");
            if (at->is_object() && at->contains(token)) at = &at->at(token);
            else if (at->is_array() && !token.empty() && token.size() < 9 && token.find_first_not_of("0123456789") == std::string::npos && std::stoul(token) < at->size()) at = &(*at)[std::stoul(token)];
            else break;
            if (at->is_object() && at->contains("id") && at->at("id").is_string()) id = at->at("id").get<std::string>();
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return id;
    }
    // offline: the spec is checked as a document (level_spec_validate,
    // level_build's own spec), so fixes are edits of it (Tools::SpecFix);
    // an intent tool's findings keep the tool calls that change the map.
    Json DiagnosticJson(const Spec::Diagnostic& d, const Json& spec, bool offline = false)
    {
        Json out{{"severity", d.severity}, {"rule", d.rule}, {"message", d.message}, {"wikiPage", d.wikiPage}};
        if (!d.pointer.empty())
        {
            out["pointer"] = d.pointer;
            const auto id = IdAt(spec, d.pointer);
            if (!id.empty()) out["id"] = id;
        }
        if (!d.fix.is_null()) out["fix"] = offline ? SpecFix(d.fix) : d.fix;
        return out;
    }
    // Findings, errors first; counts has every one, the list up to limit.
    Json Findings(const std::vector<Spec::Diagnostic>& diagnostics, const Json& spec, Json& counts, bool offline = false)
    {
        std::vector<Json> list;
        size_t n[3] = {0, 0, 0};
        for (const auto& d : diagnostics) { list.push_back(DiagnosticJson(d, spec, offline)); ++n[Rank(d.severity)]; }
        std::stable_sort(list.begin(), list.end(), [](const Json& x, const Json& y) { return Rank(Text(x, "severity")) < Rank(Text(y, "severity")); });
        counts = {{"errors", n[0]}, {"warnings", n[1]}, {"info", n[2]}};
        return Json(list);
    }
    Json Limit(const Json& list, size_t limit)
    {
        if (!list.is_array() || list.size() <= limit) return list;
        return Json(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(limit));
    }
    // A finding about a tool's part of the spec: under one of its pointers or
    // from one of its rule families.
    bool InPart(const Json& finding, const Json& part)
    {
        if (!part.is_object()) return true;
        const auto pointer = Text(finding, "pointer"), rule = Text(finding, "rule");
        for (const auto& p : part.value("pointers", Json::array()))
            if (p.is_string() && (pointer == p.get<std::string>() || pointer.rfind(p.get<std::string>() + "/", 0) == 0)) return true;
        for (const auto& r : part.value("rules", Json::array()))
            if (r.is_string() && rule.rfind(r.get<std::string>(), 0) == 0) return true;
        return false;
    }
    std::string PlanPicture(const Spec::Compiled& compiled, const Json& options)
    {
        auto scene = compiled.render;
        Render::AddBrushes(scene, compiled.sidecar.value("spec", Json::object()));
        return Render::PlanPng(scene, options);
    }
    Json PlanOutline(const Json& plan)
    {
        Json out{{"pre", 0}, {"steps", 0}, {"post", 0}, {"ops", Json::array()}};
        for (const char* section : {"pre", "steps", "post"})
        {
            if (!plan.contains(section)) continue;
            out[section] = plan.at(section).size();
            for (const auto& step : plan.at(section)) out["ops"].push_back(std::string(section) + " " + Text(step, "id") + ": " + Text(step, "op") + (step.value("optional", false) ? " (optional)" : ""));
        }
        return out;
    }

    // --------------------------------------------------- level_spec_validate
    ToolResult ValidateSpec(const Json& a, ToolContext& c)
    {
        std::string source;
        auto spec = LoadSpec(a, c, source);
        const auto profile = String(a, "profile");
        if (!profile.empty() && profile != "draft" && profile != "release") throw std::runtime_error("profile is draft or release.");
        if (!profile.empty()) spec["profile"] = profile;
        const auto limit = static_cast<size_t>(Integer(a, "limit", 300, 1, 5000));
        Spec::Compiled compiled;
        try { compiled = Spec::Compile(spec); }
        catch (const Spec::SpecError& e)
        {
            // A spec with the wrong shape is a finding, not a failure of the check.
            const Json finding{{"severity", "error"}, {"rule", "spec.shape"}, {"pointer", e.pointer.empty() ? std::string("/") : e.pointer}, {"message", e.what()}, {"wikiPage", ""}};
            Json result{{"valid", false}, {"source", source}, {"summary", {{"errors", 1}, {"warnings", 0}, {"info", 0}}}, {"diagnostics", Json::array({finding})},
                        {"next", Json::array({Next("level_spec_validate", "Fix the spec at " + finding.at("pointer").get<std::string>() + " (scct://schema/level-spec has every field) and check it again.",
                                                   source == "spec" ? Json() : Json{{"path", source}})})}};
            return Ok(result, "The spec does not have the scct.level-spec shape at " + finding.at("pointer").get<std::string>() + ": " + e.what() +
                                  " Nothing past the first shape error is checked.");
        }
        const auto normalised = compiled.sidecar.value("spec", Json::object());
        Json counts;
        const auto findings = Findings(compiled.diagnostics, normalised, counts, true);
        const size_t errors = counts.at("errors").get<size_t>(), warnings = counts.at("warnings").get<size_t>();
        const auto& summary = compiled.summary;
        Json overview{{"errors", errors}, {"warnings", warnings}, {"info", counts.at("info")}, {"profile", normalised.value("profile", std::string("draft"))},
                      {"map", summary.value("map", std::string())}, {"file", normalised.value("map", Json::object()).value("file", std::string())},
                      {"pieces", summary.value("pieces", 0)}, {"doorways", summary.value("doorways", 0)}, {"actors", summary.value("actors", 0)},
                      {"lifts", summary.value("lifts", 0)}, {"ges", summary.value("ges", 0)}, {"modes", summary.value("modes", Json::array())},
                      {"byKind", summary.value("byKind", Json::object())}, {"packages", summary.value("packages", Json::array())}, {"shown", std::min(limit, findings.size())}};
        // Fixes are edits of this spec: the intent tools would change the
        // open map instead, so they are not suggested here.
        Json next = Json::array();
        const Json again = source == "spec" ? Json() : Json{{"path", source}};
        if (errors)
            next.push_back(Next("level_spec_validate", "Fix the errors in the spec (each diagnostic's pointer says where; fix.spec lists the edits, merged at their pointer "
                                                       "as scct://docs/tools describes), then check it again.", again));
        else
        {
            next.push_back(Next("level_build", "Preview the build in the editor: preview rows and the plan.", {{"mode", "preview"}}));
            if (normalised.value("profile", std::string()) != "release") next.push_back(Next("level_spec_validate", "Check against the stricter release profile before sharing the map.", {{"profile", "release"}}));
        }
        Json result{{"valid", errors == 0}, {"source", source}, {"summary", overview}, {"diagnostics", Limit(findings, limit)}, {"plan", PlanOutline(compiled.plan)}, {"next", next}};
        std::string text = (errors ? "The spec has " : "The spec compiles: ") + Count(errors, "error") + ", " + Count(warnings, "warning") + ", " +
                           Count(counts.at("info").get<size_t>(), "note") + " (" + overview.at("profile").get<std::string>() + " profile). " +
                           Count(summary.value("pieces", 0u), "piece") + ", " + Count(summary.value("actors", 0u), "actor") + ", " + Count(summary.value("ges", 0u), "GE") +
                           "; the plan has " + std::to_string(compiled.plan.value("steps", Json::array()).size()) + " joined steps.";
        if (errors) text += " Errors block level_build apply; each has the JSON pointer of the part at fault and the wiki page of its rule.";
        if (findings.size() > limit) text += " Showing the first " + std::to_string(limit) + " findings, errors first.";
        auto r = Ok(result, text);
        if (Bool(a, "render", true))
        {
            Json options = Object(a, "options");
            options["maxEdge"] = Integer(a, "maxEdge", options.value("maxEdge", 1024), 64, 4096);
            if (!options.contains("title")) options["title"] = summary.value("map", std::string());
            const auto png = PlanPicture(compiled, options);
            r.structured["image"] = {{"bytes", png.size()}, {"note", "Top-down plan, north up, east right."}};
            r.content.push_back(ImageContent(Bridge::Base64(png)));
        }
        return r;
    }

    // ----------------------------------------------------------- the plan
    // Whether a plan changes brushes, so a geometry build is worth running.
    bool TouchesGeometry(const Json& plan)
    {
        for (const auto& step : plan.value("steps", Json::array()))
        {
            const auto op = Text(step, "op");
            if (op == "design.blockbatch" || op == "design.block" || op == "design.lift" || op == "design.unregister") return true;
            if (op != "authoring.apply") continue;
            const auto document = step.value("args", Json::object()).value("document", Json::object());
            for (const auto& o : document.value("operations", Json::array()))
            {
                const auto kind = Text(o, "op");
                if (kind == "create" && (o.contains("polygons") || o.contains("geometry"))) return true;
                if (kind == "delete")
                {
                    const auto cls = Fold(Text(o.value("actor", Json::object()), "class"));
                    if (cls.find("brush") != std::string::npos || cls.find("volume") != std::string::npos) return true;
                }
            }
        }
        return false;
    }
    // Nothing for the editor to do: the spec the map was built from, again.
    bool Unchanged(const Spec::Compiled& compiled, const Json& before)
    {
        if (!before.is_object() || Text(before, "hash") != Text(compiled.sidecar, "hash")) return false;
        // Package loads alone change nothing; imports and saves do.
        for (const auto& step : compiled.plan.value("pre", Json::array()))
            for (const auto& o : step.value("args", Json::object()).value("document", Json::object()).value("operations", Json::array()))
                if (Text(o, "op") != "load") return false;
        for (const auto& step : compiled.plan.value("steps", Json::array()))
        {
            const auto op = Text(step, "op");
            const auto* policy = Bridge::FindPolicy(op);
            if (op != "mcp.sidecar.set" && !(policy && policy->policy == Bridge::Policy::Read)) return false;
        }
        for (const auto& step : compiled.plan.value("post", Json::array()))
        {
            const auto op = Text(step, "op");
            if (op != "design.build" && op != "build.all" && op != "mapcheck.run" && op != "view.screenshot") return false;
        }
        return true;
    }
    // build "all" swaps the Map Design build for the full one; extra post
    // steps (map_settings' minimap) go before the save and screenshot.
    void Adjust(Json& plan, const std::string& build, const Json& extra)
    {
        if (!plan.contains("post")) plan["post"] = Json::array();
        auto& post = plan["post"];
        if (build == "all")
            for (auto& step : post) if (Text(step, "id") == "build") step["op"] = "build.all";
        for (const auto& step : extra)
        {
            auto at = std::find_if(post.begin(), post.end(), [](const Json& s) { return Text(s, "id") == "save" || Text(s, "id") == "screenshot"; });
            post.insert(at, step);
        }
        if (post.empty()) plan.erase("post");
    }
    // What an apply's mcp.plan expects, so the editor refuses it as stale if
    // the map moved on since the sidecar and the schema were read.
    Bridge::Expect Expected(const Json& state)
    {
        Bridge::Expect expect;
        auto whole = [&](const char* key)
        {
            if (!state.is_object() || !state.contains(key)) return false;
            const auto& v = state.at(key);
            return v.is_number_unsigned() || (v.is_number_integer() && v.get<long long>() >= 0);
        };
        if (whole("level") && whole("generation") && whole("contentRevision"))
        {
            expect.set = true;
            expect.level = state.at("level").get<uint64_t>();
            expect.generation = state.at("generation").get<unsigned>();
            expect.revision = state.at("contentRevision").get<unsigned>();
        }
        return expect;
    }
    const Json* StepIn(const Json& plan, const std::string& id)
    {
        for (const char* section : {"pre", "steps", "post"})
            if (plan.contains(section))
                for (const auto& step : plan.at(section)) if (Text(step, "id") == id) return &step;
        return nullptr;
    }

    // ------------------------------------------------------------ changes
    // The plan's effect by spec id, against the sidecar it replaces: what it
    // creates, changes (rebuilds, recreates, updates, links) and removes.
    // With a run's step results, each also gets the name the editor gave it.
    struct Effect { Json created = Json::array(), changed = Json::array(), removed = Json::array(); };
    const Json* Recorded(const Json& map, const std::string& id)
    {
        if (!map.is_object()) return nullptr;
        for (auto it = map.begin(); it != map.end(); ++it) if (Fold(it.key()) == Fold(id)) return &it.value();
        return nullptr;
    }
    Effect Changes(const Spec::Compiled& compiled, const Json& before, const std::map<std::string, Json>* results)
    {
        Effect out;
        const auto oldPieces = before.is_object() ? before.value("pieces", Json::object()) : Json::object();
        const auto oldActors = before.is_object() ? before.value("actors", Json::object()) : Json::object();
        const auto& now = compiled.sidecar;
        std::map<std::string, Json> about; // folded id -> what it is
        for (const auto& p : compiled.render.value("pieces", Json::array())) about[Fold(Text(p, "id"))] = {{"kind", "piece"}, {"piece", Text(p, "kind")}};
        for (const auto& actor : compiled.render.value("actors", Json::array()))
        {
            const auto role = Text(actor, "kind");
            if (role == "ge") continue;
            Json e{{"kind", role == "lift" ? "lift" : "actor"}, {"class", Text(actor, "cls")}};
            if (role != "other" && role != "lift") e["role"] = role;
            about[Fold(Text(actor, "id"))] = e;
        }
        auto entry = [&](const std::string& id, const Json& bound, const char* how)
        {
            Json e{{"id", id}};
            if (const auto found = about.find(Fold(id)); found != about.end()) for (auto it = found->second.begin(); it != found->second.end(); ++it) e[it.key()] = it.value();
            if (how) e["how"] = how;
            if (results && Plan::IsBind(bound))
                try
                {
                    const auto value = Plan::Bind(bound, *results);
                    if (IsIdentity(value)) e["path"] = value.at("path");
                }
                catch (const std::exception&) {}
            return e;
        };
        for (const std::string list : {"pieces", "actors"})
        {
            const auto& old = list == "pieces" ? oldPieces : oldActors;
            const auto built = now.value(list, Json::object());
            for (auto it = built.begin(); it != built.end(); ++it)
            {
                if (!Plan::IsBind(it.value())) continue; // kept as built
                if (Recorded(old, it.key())) out.changed.push_back(entry(it.key(), it.value(), list == "pieces" ? "rebuilt" : "recreated"));
                else out.created.push_back(entry(it.key(), it.value(), nullptr));
            }
            for (auto it = old.begin(); it != old.end(); ++it)
                if (!Recorded(built, it.key()))
                {
                    Json e{{"id", it.key()}, {"kind", list == "pieces" ? "piece" : "actor"}};
                    if (IsIdentity(it.value())) e["path"] = it.value().at("path");
                    out.removed.push_back(e);
                }
        }
        // Updates and links in the actors document.
        if (const auto* step = StepIn(compiled.plan, "actors"))
            for (const auto& o : step->at("args").at("document").value("operations", Json::array()))
            {
                const auto kind = Text(o, "op");
                if (kind == "update")
                {
                    const auto path = Text(o.value("actor", Json::object()), "path");
                    Json e{{"kind", "actor"}, {"path", path}, {"how", "updated"}, {"properties", Json::array()}};
                    for (auto it = oldActors.begin(); it != oldActors.end(); ++it)
                        if (IsIdentity(it.value()) && Fold(it.value().at("path").get<std::string>()) == Fold(path)) { e["id"] = it.key(); break; }
                    const auto properties = o.value("properties", Json::object());
                    for (auto it = properties.begin(); it != properties.end(); ++it) e["properties"].push_back(it.key());
                    out.changed.push_back(e);
                }
                else if (kind == "link")
                    out.changed.push_back({{"kind", "link"}, {"path", Text(o, "target")}, {"how", "linked"}, {"event", Text(o, "event")}, {"trigger", o.value("trigger", false)}});
            }
        // GEs are recorded by what they are; the new ones are appended.
        const auto oldGes = before.is_object() ? before.value("ges", Json::array()) : Json::array();
        for (const auto& ge : now.value("ges", Json::array()))
            if (std::find(oldGes.begin(), oldGes.end(), ge) == oldGes.end()) out.created.push_back({{"kind", "ge"}, {"command", ge.value("command", Json())}, {"points", ge.value("points", Json())}});
        // Map Settings and interface images are not actors: they are reported
        // when the spec's sections change.
        const auto oldSpec = before.is_object() ? before.value("spec", Json::object()) : Json::object();
        const auto newSpec = now.value("spec", Json::object());
        for (const auto& [key, id] : std::vector<std::pair<std::string, std::string>>{{"settings", "MapSettings"}, {"interface", "Interface"}})
            if (newSpec.contains(key) && newSpec.at(key) != oldSpec.value(key, Json()))
                out.changed.push_back({{"id", id}, {"kind", key}, {"how", "updated in the spec"}});
        if (const auto* edit = StepIn(compiled.plan, "settings_edit"))
        {
            Json properties = Json::array();
            const auto changes = edit->at("args").value("changes", Json::object());
            for (auto it = changes.begin(); it != changes.end(); ++it) properties.push_back(it.key());
            out.changed.push_back({{"id", "LevelInfo"}, {"kind", "level"}, {"how", "updated"}, {"properties", properties}});
        }
        return out;
    }
    // The lists, the first few hundred of each on a big build.
    void Lists(Json& report, const Effect& effect)
    {
        constexpr size_t kMost = 300;
        for (const auto& [key, list] : std::vector<std::pair<std::string, const Json*>>{{"created", &effect.created}, {"changed", &effect.changed}, {"removed", &effect.removed}})
        {
            report[key] = Limit(*list, kMost);
            if (list->size() > kMost) report[key + "Total"] = list->size();
        }
    }
    std::string Tally(const Json& list)
    {
        std::map<std::string, size_t> kinds;
        for (const auto& e : list) ++kinds[Text(e, "kind").empty() ? std::string("edit") : Text(e, "kind")];
        std::vector<std::string> parts;
        for (const auto& [kind, n] : kinds)
            parts.push_back(kind == "ge" ? Count(n, "GE") : kind == "level" ? std::string("the Level Properties") : kind == "settings" ? std::string("the Map Settings") :
                            kind == "interface" ? std::string("the interface textures") : Count(n, kind));
        return parts.empty() ? std::string("nothing") : Join(parts);
    }

    // ------------------------------------------------- Map Settings and more
    // What a person does when the bridge cannot write SMapSettings.
    Json MapSettingsSteps()
    {
        return Json::array({
            "If the map is new, save it (map_file action save_as), then close and reopen the editor (editor_restart): Map Settings stay unavailable on a new map until then.",
            "Right-click empty space in a viewport (easiest in Wireframe) and choose Map Settings.",
            "Enter the values below: MapName in capitals; | starts a new line; Modes[i] and ModesStrings[i] index-aligned (GameModeId GM_Multi for Story, GM_DiskHunt, GM_DeathMatch); RespawnTime 5-15, SpiesLives and SentriesLives 0-10, TimeLimit 0-20.",
            "Save the map (map_file action save): the editor writes the settings into Packages\\Textures\\<Map>-i.utc."});
    }
    // A property text as a person types it: "\"BLUE\"" is BLUE.
    std::string Plain(const Json& value)
    {
        if (!value.is_string()) return value.dump();
        auto text = value.get<std::string>();
        if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
        {
            text = text.substr(1, text.size() - 2);
            for (size_t k = 0; (k = text.find("\\\"", k)) != std::string::npos; ++k) text.erase(k, 1);
        }
        return text;
    }
    Json Checklist(const Json& changes)
    {
        Json lines = Json::array();
        for (auto it = changes.begin(); it != changes.end(); ++it)
        {
            if (!it.value().is_array()) { lines.push_back(it.key() + " = " + Plain(it.value())); continue; }
            for (size_t i = 0; i < it.value().size(); ++i)
            {
                std::string line = it.key() + "[" + std::to_string(i) + "]:";
                const auto& item = it.value()[i];
                if (item.is_object()) for (auto f = item.begin(); f != item.end(); ++f) line += " " + f.key() + " = " + Plain(f.value()) + ";";
                else line += " " + Plain(item);
                if (line.back() == ';') line.pop_back();
                lines.push_back(line);
            }
        }
        return lines;
    }
    // The optional post steps' outcome: Map Settings, interface textures, the
    // minimap. results is null for a preview.
    void FileSteps(const Json& plan, const std::map<std::string, Json>* results, const Json& failed, Json& report, Json& warnings)
    {
        for (const auto& step : plan.value("post", Json::array()))
        {
            const auto id = Text(step, "id"), op = Text(step, "op");
            const bool ran = results && results->count(id), failure = failed.is_object() && failed.contains(id);
            const auto error = failure && failed.at(id).is_string() ? failed.at(id).get<std::string>() : failure ? failed.at(id).dump() : std::string();
            if (op == "mapsettings.set")
            {
                const auto changes = step.at("args").value("changes", Json::object());
                Json block{{"applied", ran}, {"values", changes}, {"checklist", Checklist(changes)}};
                if (!results) block["note"] = "Runs on apply. This editor may not be able to write SMapSettings over the bridge; if not, the result lists the manual steps.";
                else if (ran) block["result"] = results->at(id);
                else
                {
                    block["error"] = error.empty() ? std::string("mapsettings.set did not run.") : error;
                    block["steps"] = MapSettingsSteps();
                    warnings.push_back("Map Settings were NOT written: the editor cannot set SMapSettings over the bridge. Enter mapSettings.checklist by hand (mapSettings.steps).");
                }
                report["mapSettings"] = block;
            }
            else if (op == "interface.texture")
            {
                Json entry{{"slot", step.at("args").value("slot", Json())}, {"file", step.at("args").value("file", Json())}, {"applied", ran}};
                if (ran) entry["result"] = results->at(id);
                if (failure) { entry["error"] = error; warnings.push_back("Interface texture " + Text(step.at("args"), "slot") + " was not imported: " + error); }
                if (!report.contains("interface")) report["interface"] = Json::array();
                report["interface"].push_back(entry);
            }
            else if (op == "minimap.apply")
            {
                Json block{{"applied", ran}, {"options", step.at("args")}};
                if (ran) block["result"] = results->at(id);
                if (failure) { block["error"] = error; warnings.push_back("The minimap was not set: " + error); }
                report["minimap"] = block;
            }
            else if (failure) warnings.push_back("Optional step " + id + " (" + op + ") failed: " + error);
        }
    }
    // Step results short enough to read; the big ones are summarised.
    Json Compact(const std::map<std::string, Json>& results)
    {
        Json out = Json::object();
        for (const auto& [id, result] : results)
        {
            if (id == "screenshot" || id == "check" || id == "sidecar") continue;
            const auto text = result.dump();
            if (text.size() <= 1500) { out[id] = result; continue; }
            Json brief{{"bytes", text.size()}};
            if (result.is_array()) brief["items"] = result.size();
            else if (result.is_object())
                for (auto it = result.begin(); it != result.end(); ++it)
                    brief[it.key()] = it.value().is_array() || it.value().is_object() ? Json(std::to_string(it.value().size()) + " entries") : it.value();
            out[id] = brief;
        }
        return out;
    }
    Json MapCheckBlock(const Json& check, Json& warnings)
    {
        auto rows = check.value("rows", Json::array());
        std::stable_sort(rows.begin(), rows.end(), [](const Json& x, const Json& y) { return Rank(Text(x, "severity")) < Rank(Text(y, "severity")); });
        Json block{{"summary", check.value("summary", Json::object())}, {"rows", Limit(rows, 40)}};
        if (rows.size() > 40) block["rowsTrimmed"] = rows.size();
        const auto summary = check.value("summary", Json::object());
        const auto errors = summary.is_object() && summary.contains("errors") && summary.at("errors").is_number() ? summary.at("errors").get<double>() : 0.0;
        if (errors > 0) warnings.push_back("Map Check found " + std::to_string(static_cast<long long>(errors)) + " error(s) after the build (mapCheck.rows).");
        return block;
    }

    // ----------------------------------------------------------- level_build
    ToolResult Build(const Json& a, ToolContext& c)
    {
        std::string source;
        const auto spec = LoadSpec(a, c, source);
        auto mode = String(a, "mode", "preview");
        if (mode != "preview" && mode != "apply") throw std::runtime_error("mode is preview or apply.");
        if (DryRun(a)) mode = "preview";
        Json request{{"tool", "level_build"}, {"mode", mode}, {"build", String(a, "build", "geometry")}, {"screenshot", Bool(a, "screenshot", true)}, {"force", Bool(a, "force", false)}};
        for (const char* key : {"sync", "brushMode", "save", "profile"}) if (Has(a, key)) request[key] = String(a, key);
        const auto map = spec.value("map", Json::object());
        request["label"] = Label(a, "level_build " + (map.is_object() ? Text(map, "name") : std::string()));
        std::vector<Content> images;
        const auto context = BuildContext(c, mode == "apply");
        // A merge adds to the stored spec, as the intent tools do: errors it
        // already had do not block.
        const auto sidecar = context.value("sidecar", Json());
        if (Text(request, "sync") == "merge" && sidecar.is_object() && sidecar.contains("spec")) request["baseline"] = sidecar.at("spec");
        auto report = LevelBuild(c, context, spec, request, images);
        report["source"] = source;
        return BuildResult(c, report, images);
    }
}

// --------------------------------------------------------------- shared
// What a build starts from: the editor's state, the sidecar the last build
// stored (whose spec the intent tools add to), less what the map no longer
// has, and whether an editor answered at all (a preview works without one).
Json Tools::BuildContext(ToolContext& c, bool needEditor)
{
    Json context{{"connected", false}, {"open", false}, {"state", Json::object()}, {"sidecar", Json()}, {"warnings", Json::array()}};
    Json state;
    try { state = Call(c, "editor.state"); }
    catch (const Bridge::Error& e)
    {
        if (needEditor || (e.code != Bridge::Code::NotConnected && e.code != Bridge::Code::Disabled)) throw;
        context["warnings"].push_back(std::string("No editor answered (") + e.what() + "), so this preview is the offline compile: no preview rows, and the level spec the map was last built from is not taken into account. " + Hint(e));
        return context;
    }
    context["connected"] = true;
    context["state"] = state.is_object() ? state : Json::object();
    if (state.is_object() && state.contains("open") && state.at("open") == false)
    {
        if (needEditor) throw std::runtime_error("No map is open in the editor. Create or open one first (map_file with action new or open), then build.");
        context["warnings"].push_back("No map is open in the editor, so this preview is the offline compile. map_file with action new or open gives the build a map.");
        return context;
    }
    context["open"] = true;
    Json sidecar;
    try
    {
        const auto got = Call(c, "mcp.sidecar.get");
        sidecar = got.is_object() ? got.value("sidecar", Json()) : Json();
    }
    catch (const Bridge::Error& e)
    {
        if (e.code == Bridge::Code::NotConnected || e.code == Bridge::Code::Busy || e.code == Bridge::Code::Timeout) throw;
        context["warnings"].push_back(std::string("The level spec stored with the map could not be read (") + e.what() + "); the spec is built as new.");
    }
    if (!sidecar.is_object() || Text(sidecar, "format") != "scct.level-sidecar") return context;
    // Drop what the last build made that the map no longer has (undone or
    // deleted by hand), so it is made again rather than edited.
    std::vector<std::string> paths;
    auto collect = [&](const Json& identity) { if (IsIdentity(identity)) paths.push_back(identity.at("path").get<std::string>()); };
    for (const auto& identity : sidecar.value("actors", Json::object())) collect(identity);
    for (const auto& record : sidecar.value("pieces", Json::object()))
        if (record.is_object()) for (const auto& member : record.value("members", Json::array())) collect(member);
    std::set<std::string> found;
    bool checked = true;
    try
    {
        for (size_t at = 0; at < paths.size() && at < 20000; at += 1000)
        {
            const Json names = std::vector<std::string>(paths.begin() + static_cast<std::ptrdiff_t>(at), paths.begin() + static_cast<std::ptrdiff_t>(std::min(paths.size(), at + 1000)));
            const auto result = Call(c, "actors.query", {{"names", names}, {"limit", 5000}});
            for (const auto& actor : result.value("actors", Json::array())) found.insert(Fold(Text(actor, "path")));
        }
    }
    catch (const Bridge::Error& e)
    {
        if (e.code == Bridge::Code::NotConnected || e.code == Bridge::Code::Busy || e.code == Bridge::Code::Timeout) throw;
        checked = false;
    }
    catch (const std::exception&) { checked = false; }
    if (!checked || paths.size() > 20000)
    {
        context["warnings"].push_back("Could not check which of the last build's actors are still in the map; an incremental build assumes they all are.");
        context["sidecar"] = sidecar;
        return context;
    }
    std::vector<std::string> gone;
    Json actors = Json::object(), pieces = Json::object();
    const auto builtActors = sidecar.value("actors", Json::object()), builtPieces = sidecar.value("pieces", Json::object());
    for (auto it = builtActors.begin(); it != builtActors.end(); ++it)
    {
        if (IsIdentity(it.value()) && !found.count(Fold(it.value().at("path").get<std::string>()))) { gone.push_back(it.key()); continue; }
        actors[it.key()] = it.value();
    }
    for (auto it = builtPieces.begin(); it != builtPieces.end(); ++it)
    {
        const auto members = it.value().is_object() ? it.value().value("members", Json::array()) : Json::array();
        size_t missing = 0;
        for (const auto& member : members) missing += IsIdentity(member) && !found.count(Fold(member.at("path").get<std::string>()));
        if (!members.empty() && missing == members.size()) { gone.push_back(it.key()); continue; }
        if (missing)
            context["warnings"].push_back("Piece " + it.key() + " has lost " + Count(missing, "brush", "brushes") + " since the last build; rebuild it by changing it in the spec, or with sync replace.");
        pieces[it.key()] = it.value();
    }
    if (!gone.empty())
    {
        std::vector<std::string> shown(gone.begin(), gone.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(gone.size(), 12)));
        context["warnings"].push_back(Count(gone.size(), "thing") + " the last build made " + (gone.size() == 1 ? "is" : "are") + " no longer in the map (undone or deleted): " + Join(shown) +
                                      (gone.size() > shown.size() ? ", ..." : "") + ". They are made again if the spec still has them.");
        context["gone"] = gone;
    }
    sidecar["actors"] = actors;
    sidecar["pieces"] = pieces;
    context["sidecar"] = sidecar;
    return context;
}

// Compiles spec against the context and previews or applies it. request:
//   tool, mode (preview|apply), sync, brushMode, build (none|geometry|all|auto),
//   screenshot (true|false|null: when built), save, force, profile, label,
//   part {pointers, rules} (the findings a tool reports as its own),
//   post [extra post steps], notes [warnings the caller found].
// Returns the report BuildResult turns into the tool result: {summary,
// created, changed, removed, warnings, validation, undo, next, ...} or
// {error, ...} when the build was refused; images gets the plan PNG
// (preview) or the screenshot (apply).
Json Tools::LevelBuild(ToolContext& c, const Json& context, const Json& spec, const Json& request, std::vector<Content>& images)
{
    const auto tool = Text(request, "tool").empty() ? std::string("level_build") : Text(request, "tool");
    const bool apply = Text(request, "mode") == "apply";
    const Json before = context.value("sidecar", Json());
    const bool haveSidecar = before.is_object();
    const bool mapOpen = context.value("open", false);
    const auto state = context.value("state", Json::object());
    Json warnings = context.value("warnings", Json::array());
    for (const auto& note : request.value("notes", Json::array())) warnings.push_back(note);

    Spec::CompileOptions options;
    options.sync = Text(request, "sync").empty() ? (haveSidecar ? "incremental" : "replace") : Text(request, "sync");
    options.brushMode = !Text(request, "brushMode").empty() ? Text(request, "brushMode") : haveSidecar && !Text(before, "brushMode").empty() ? Text(before, "brushMode") : "pieces";
    options.sidecar = before;
    const auto build = Text(request, "build").empty() ? std::string("geometry") : Text(request, "build");
    if (build != "none" && build != "geometry" && build != "all" && build != "auto") throw std::runtime_error("build is none, geometry or all.");
    options.build = build != "none";
    const bool shotAuto = !request.contains("screenshot") || !request.at("screenshot").is_boolean();
    options.screenshot = shotAuto ? options.build : request.at("screenshot").get<bool>();
    options.save = Text(request, "save");
    Json input = spec;
    if (!Text(request, "profile").empty())
    {
        const auto profile = Text(request, "profile");
        if (profile != "draft" && profile != "release") throw std::runtime_error("profile is draft or release.");
        input["profile"] = profile;
    }
    const auto label = Text(request, "label").empty() ? tool : Text(request, "label");

    auto compile = [&]
    {
        try { return Spec::Compile(input, options); }
        catch (const Spec::SpecError& e)
        {
            throw Bridge::Error(Bridge::Code::Invalid, std::string("The level spec does not have the scct.level-spec shape at ") + (e.pointer.empty() ? std::string("/") : e.pointer) + ": " + e.what() +
                                    " level_spec_validate checks a spec offline; scct://schema/level-spec has every field.",
                                "spec_shape", {{"pointer", e.pointer.empty() ? std::string("/") : e.pointer}});
        }
    };
    c.progress(0, 4, "Compiling the level spec");
    auto compiled = compile();
    // The editor's own schema completes structs and checks enum names
    // (GM_DiskHunt and the like) against the classes the map really has.
    if (mapOpen)
    {
        Json classes = Json::array();
        for (const auto& cls : compiled.summary.value("classes", Json::array())) if (cls.is_string() && cls.get<std::string>().find('.') != std::string::npos && classes.size() < 512) classes.push_back(cls);
        if (!classes.empty())
            try
            {
                c.progress(1, 4, "Reading the editor's schema for " + Count(classes.size(), "class", "classes"));
                options.schema = Call(c, "authoring.schema", {{"classes", classes}});
                compiled = compile();
            }
            catch (const Bridge::Error& e)
            {
                if (e.code == Bridge::Code::NotConnected || e.code == Bridge::Code::Busy || e.code == Bridge::Code::Timeout) throw;
                warnings.push_back(std::string("The editor's schema could not be read (") + e.what() + "); values are written as the spec gives them, unchecked.");
            }
    }
    if (build == "auto" && !TouchesGeometry(compiled.plan))
    {
        options.build = false;
        if (shotAuto) options.screenshot = false;
        compiled = compile();
    }
    Adjust(compiled.plan, build, request.value("post", Json::array()));
    const auto normalised = compiled.sidecar.value("spec", Json::object());
    // Map Settings and interface images are files the compiler writes on
    // every build; an intent tool for another part leaves them alone while
    // the spec's sections are as the last build had them.
    if (Text(request, "fileSteps") == "changed" && haveSidecar && compiled.plan.contains("post"))
    {
        const auto last = before.value("spec", Json::object());
        auto& post = compiled.plan["post"];
        for (size_t i = 0; i < post.size();)
        {
            const auto op = Text(post[i], "op");
            const char* section = op == "mapsettings.set" ? "settings" : op == "interface.texture" ? "interface" : nullptr;
            if (section && normalised.value(section, Json()) == last.value(section, Json())) post.erase(i);
            else ++i;
        }
        if (post.empty()) compiled.plan.erase("post");
    }
    const auto mapName = compiled.summary.value("map", std::string());

    Json counts;
    const auto all = Findings(compiled.diagnostics, normalised, counts, tool == "level_build");
    const size_t errors = counts.at("errors").get<size_t>(), specWarnings = counts.at("warnings").get<size_t>();
    // An intent tool builds a level a part at a time, so errors the spec
    // already had (no CamNet yet while the blockout goes in) are listed as
    // standing but do not block it; errors its change brings do.
    std::set<std::string> standing;
    auto keyOf = [](const Json& f) { return Text(f, "rule") + "\n" + Text(f, "pointer") + "\n" + Text(f, "message"); };
    if (request.contains("baseline") && request.at("baseline").is_object())
        try
        {
            Spec::CompileOptions plain;
            plain.schema = options.schema;
            plain.brushMode = options.brushMode;
            const auto base = Spec::Compile(request.at("baseline"), plain);
            const auto baseSpec = base.sidecar.value("spec", Json::object());
            for (const auto& d : base.diagnostics) if (d.severity == "error") standing.insert(keyOf(DiagnosticJson(d, baseSpec)));
        }
        catch (const std::exception&) {} // a base that does not compile has nothing standing
    const auto part = request.value("part", Json());
    Json validation = Json::array();
    size_t blocking = 0, held = 0;
    for (auto f : all)
    {
        const bool mine = InPart(f, part), error = Text(f, "severity") == "error";
        if (!mine && !error) continue;
        if (error && standing.count(keyOf(f))) { f["standing"] = true; ++held; }
        else if (error) ++blocking;
        validation.push_back(f);
    }
    const size_t elsewhere = all.size() - validation.size();
    const auto profile = normalised.value("profile", std::string("draft"));
    const auto summary = compiled.summary;
    Json report{{"mode", apply ? "apply" : "preview"}, {"map", mapName}, {"sync", options.sync}, {"brushMode", options.brushMode}, {"incremental", summary.value("incremental", false)},
                {"build", options.build ? (build == "all" ? "all" : "geometry") : "none"}, {"profile", profile}, {"units", normalised.value("units", Json::object())},
                {"findings", {{"errors", errors}, {"blocking", blocking}, {"standing", held}, {"warnings", specWarnings}, {"info", counts.at("info")}, {"shown", std::min<size_t>(validation.size(), 200)}, {"elsewhere", elsewhere}}},
                {"validation", Limit(validation, 200)}, {"counts", {{"pieces", summary.value("pieces", 0)}, {"actors", summary.value("actors", 0)}, {"ges", summary.value("ges", 0)},
                                                                    {"kept", summary.value("kept", 0)}, {"creates", summary.value("creates", 0)}, {"updates", summary.value("updates", 0)},
                                                                    {"deletes", summary.value("deletes", 0)}, {"newPieces", summary.value("newPieces", 0)}, {"replacedPieces", summary.value("replacedPieces", 0)}}},
                {"plan", PlanOutline(compiled.plan)}};
    Json next = Json::array();
    // The spec as built here, saved so level_spec_validate can show every
    // finding in it: an intent tool's is the stored spec with its change, and
    // a merge is laid over the stored spec as the compiler lays it, so the
    // file holds the candidate these findings are about. <work>/specs keeps
    // the newest kSpecFiles.
    std::string specFile;
    auto checkSpec = [&](const std::string& why)
    {
        if (specFile.empty() && !c.workDir.empty())
            try
            {
                const bool merged = options.sync == "merge" && haveSidecar && Text(before, "format") == "scct.level-sidecar" && before.contains("spec") && before.at("spec").is_object();
                const auto candidate = merged ? Spec::Merge(before.at("spec"), input) : input;
                static std::atomic<unsigned> serial{0};
                const auto stamp = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + "-" + std::to_string(++serial);
                const auto folder = c.workDir / "specs";
                std::filesystem::create_directories(folder);
                const auto file = folder / (tool + "-" + stamp + ".level.json");
                {
                    std::ofstream out(file, std::ios::binary);
                    out << candidate.dump(1);
                    if (out) specFile = PathText(file);
                }
                PruneSpecs(folder);
            }
            catch (const std::exception&) {}
        if (!specFile.empty()) report["specFile"] = specFile;
        Json step = Next("level_spec_validate", why + (specFile.empty() ? std::string(" (validation has this part's findings; give level_spec_validate the spec to see the rest)") : std::string()),
                         specFile.empty() ? Json() : Json{{"path", specFile}});
        if (std::none_of(next.begin(), next.end(), [](const Json& n) { return Text(n, "tool") == "level_spec_validate"; })) next.insert(next.begin(), step);
    };
    if (elsewhere) checkSpec(Count(elsewhere, "more finding") + " about other parts of the spec: this checks the whole spec offline.");
    std::string findingText = Count(errors, "error") + ", " + Count(specWarnings, "warning") + " in the spec";
    if (held) findingText += " (" + std::to_string(held) + " of the errors were there before this change: listed as standing, they do not block it)";
    if (errors) findingText += "; validation has each with its pointer and wiki page";

    if (haveSidecar && Unchanged(compiled, before) && request.value("post", Json::array()).empty())
    {
        report["summary"] = "Nothing to change: the map already has this spec (" + std::string(apply ? "nothing was sent to the editor" : "preview") + "). " + findingText + ".";
        report["created"] = Json::array();
        report["changed"] = Json::array();
        report["removed"] = Json::array();
        report["warnings"] = warnings;
        report["undo"] = Json();
        if (options.build) next.push_back(Next("map_build", "Rebuild the geometry anyway.", {{"what", build == "all" ? "all" : "geometry"}}));
        if (state.contains("dirty") && state.at("dirty") == true) next.push_back(Next("map_file", "The map has unsaved changes.", {{"action", Text(state, "mapFile").empty() ? "save_as" : "save"}}));
        report["next"] = next;
        return report;
    }

    if (!apply)
    {
        // Preview: the editor's dry run (no change; reads still run), plus the plan picture.
        auto effect = Changes(compiled, before, nullptr);
        bool checked = true;
        try { Plan::Check(compiled.plan); }
        catch (const std::exception& e) { checked = false; warnings.push_back(std::string("The editor was not asked for a preview: the plan is not valid (") + e.what() + ")."); }
        if (mapOpen && checked)
        {
            c.progress(2, 4, "Previewing in the editor");
            const auto dry = RunPlan(c, compiled.plan, label, true, kBuildTimeout);
            const auto rows = PreviewRows(dry);
            report["preview"] = Limit(rows, kPreviewRows);
            report["previewTotal"] = rows.size();
            if (dry.is_object() && dry.contains("errors") && dry.at("errors").is_object() && !dry.at("errors").empty())
            {
                report["previewErrors"] = dry.at("errors");
                for (auto it = dry.at("errors").begin(); it != dry.at("errors").end(); ++it)
                    warnings.push_back("Step " + it.key() + " would fail as the map is now: " + (it.value().is_string() ? it.value().get<std::string>() : it.value().dump()));
            }
        }
        FileSteps(compiled.plan, nullptr, Json(), report, warnings);
        const auto png = PlanPicture(compiled, {{"maxEdge", 1024}, {"title", mapName}});
        images.push_back(ImageContent(Bridge::Base64(png)));
        std::string text = "Preview of " + tool + " for " + (mapName.empty() ? std::string("the map") : mapName) + ", nothing changed: it would create " + Tally(effect.created) + ", change " +
                           Tally(effect.changed) + " and remove " + Tally(effect.removed) + (summary.value("kept", 0u) ? "; " + Count(summary.value("kept", 0u), "thing") + " stay as built. " : std::string(". ")) +
                           findingText + ".";
        if (report.contains("preview")) text += " " + Count(report.value("previewTotal", 0u), "preview row") + " from the editor.";
        text += " The image is the plan, north up.";
        if (blocking && !Bool(request, "force")) text += " " + Count(blocking, "error") + (blocking == 1 ? " blocks" : " block") + " apply; fix " + (blocking == 1 ? "it" : "them") + " first, or pass force:true.";
        else text += " Run again with mode \"apply\" to build it (one Undo step).";
        if (blocking) checkSpec("See every error with its pointer; fix them before applying.");
        else next.push_back(Next(tool, "Build it.", {{"mode", "apply"}}));
        report["summary"] = text;
        Lists(report, effect);
        report["warnings"] = warnings;
        report["undo"] = Json();
        report["next"] = next;
        return report;
    }

    if (blocking && !Bool(request, "force"))
    {
        report["error"] = "The level spec has " + Count(blocking, "error") + (blocking == 1 ? " that blocks" : " that block") +
                          " this build; nothing was built. Fix " + (blocking == 1 ? "it" : "them") + " (validation lists each error with its JSON pointer and wiki page), or pass force:true to build anyway.";
        report["warnings"] = warnings;
        checkSpec("Check the whole spec offline: every error with its pointer.");
        report["next"] = next;
        return report;
    }
    const auto ops = PlanOutline(compiled.plan);
    c.progress(2, 4, "Building in the editor: " + std::to_string(ops.at("pre").get<size_t>() + ops.at("steps").get<size_t>() + ops.at("post").get<size_t>()) + " plan steps");
    const auto result = RunPlan(c, compiled.plan, label, false, build == "all" ? kBuildAllTimeout : kBuildTimeout, Expected(state));
    // Read the undo record now: a later call would replace it.
    report["undo"] = Undo(c);
    for (const auto& w : BridgeWarnings(c)) warnings.push_back(w);
    c.progress(3, 4, "Reading the results");
    std::map<std::string, Json> results;
    if (result.is_object() && result.contains("steps") && result.at("steps").is_object())
        for (auto it = result.at("steps").begin(); it != result.at("steps").end(); ++it) results[it.key()] = it.value();
    const auto failed = result.is_object() ? result.value("errors", Json::object()) : Json::object();
    auto effect = Changes(compiled, before, &results);
    FileSteps(compiled.plan, &results, failed, report, warnings);
    if (results.count("check") && results.at("check").is_object()) report["mapCheck"] = MapCheckBlock(results.at("check"), warnings);
    if (results.count("screenshot") && results.at("screenshot").is_object())
    {
        const auto& shot = results.at("screenshot");
        if (!Text(shot, "png").empty())
        {
            images.push_back(ImageContent(Text(shot, "png"), Text(shot, "mimeType").empty() ? std::string("image/png") : Text(shot, "mimeType")));
            report["screenshot"] = {{"width", shot.value("width", Json())}, {"height", shot.value("height", Json())}, {"viewport", shot.value("viewport", Json("top"))}};
        }
    }
    report["steps"] = Compact(results);
    if (!failed.empty()) report["failedOptional"] = failed;
    // What to do next: a new map is saved, reopened and given its Map Settings.
    const bool saved = results.count("save") != 0;
    std::string mapFile = Text(state, "mapFile");
    if (saved && results.at("save").is_object() && !Text(results.at("save"), "file").empty()) mapFile = Text(results.at("save"), "file");
    if (mapFile.empty())
    {
        next.push_back(Next("map_file", "Save the new map: its file is the first four letters of the map name plus D, in Packages\\MapsEd (scct://wiki/map-packaging).", {{"action", "save_as"}, {"mapName", mapName}}));
        next.push_back(Next("editor_restart", "Map Settings exist only after a new map is saved, closed and opened again (scct://wiki/map-settings)."));
        next.push_back(Next("map_settings", "After the restart: MapName, briefs, legends and one Modes/ModesStrings entry per mode."));
    }
    else if (!saved) next.push_back(Next("map_file", "Save the changes.", {{"action", "save"}}));
    if (!options.build && TouchesGeometry(compiled.plan)) next.push_back(Next("map_build", "The brushes changed and this build did not rebuild them.", {{"what", "geometry"}}));
    next.push_back(Next("map_validate", "Check the built map against the wiki (live: traces floors, reads Map Settings).", {{"profile", profile}}));
    Lists(report, effect);
    report["warnings"] = warnings;
    report["next"] = next;
    std::string text = tool + " built " + (mapName.empty() ? std::string("the map") : mapName) + ": created " + Tally(effect.created) + ", changed " + Tally(effect.changed) + ", removed " +
                       Tally(effect.removed) + (summary.value("kept", 0u) ? "; " + Count(summary.value("kept", 0u), "thing") + " left as built." : std::string("."));
    const auto undo = report.at("undo");
    if (undo.is_object()) text += " One Undo step (editor_undo call " + std::to_string(undo.value("call", 0u)) + ").";
    if (report.contains("mapCheck"))
    {
        const auto mc = report.at("mapCheck").value("summary", Json::object());
        auto n = [&](const char* key) { return mc.is_object() && mc.contains(key) && mc.at(key).is_number() ? static_cast<size_t>(mc.at(key).get<double>()) : size_t{0}; };
        text += " Map Check: " + Count(n("errors"), "error") + ", " + Count(n("warnings"), "warning") + ".";
    }
    if (report.contains("mapSettings") && !report.at("mapSettings").value("applied", false)) text += " Map Settings were NOT written: enter mapSettings.checklist by hand.";
    text += " " + findingText + ".";
    if (blocking) text += " Built with force despite the errors.";
    report["summary"] = text;
    c.progress(4, 4, "Built");
    return report;
}

// The tool result for a LevelBuild report: the mutation shape, with the
// bridge's warnings (dialogs it answered) and the images.
ToolResult Tools::BuildResult(ToolContext& c, const Json& report, const std::vector<Content>& images)
{
    if (report.contains("error"))
    {
        Json details = report;
        details.erase("error");
        return Fail(report.at("error").get<std::string>(), details);
    }
    Json warnings = Json::array();
    auto add = [&](const Json& w) { if (std::find(warnings.begin(), warnings.end(), w) == warnings.end()) warnings.push_back(w); };
    for (const auto& w : report.value("warnings", Json::array())) add(w);
    for (const auto& w : BridgeWarnings(c)) add(w);
    const auto summary = Text(report, "summary");
    Json result{{"summary", summary}, {"created", report.value("created", Json::array())}, {"changed", report.value("changed", Json::array())}, {"warnings", warnings},
                {"undo", report.value("undo", Json())}, {"next", report.value("next", Json::array())}};
    for (auto it = report.begin(); it != report.end(); ++it) if (!result.contains(it.key())) result[it.key()] = it.value();
    auto r = Ok(result, summary);
    for (const auto& image : images) r.content.push_back(image);
    return r;
}

void RegisterSpecTools(Registry& registry)
{
    {
        Tool t;
        t.name = "level_spec_validate";
        t.title = "Check a level spec offline";
        t.description =
            "Checks a scct.level-spec without the editor: parses it (a wrong field is an error at its JSON pointer), compiles it exactly as level_build would, runs "
            "every wiki rule on the result (scct://rules) and draws the top-down plan PNG, north up, to compare with the source. Use it after writing or editing a "
            "spec, before level_build.\n"
            "spec, or path: a .level.json on this machine (absolute, or relative to the work directory) or scct://examples/<versus-starter | three-modes-sectors | "
            "diskhunt-arena | security-camnet | elevator-3floors>. profile release is stricter than draft.\n"
            "Returns valid, summary, diagnostics errors first {severity, rule, pointer, id, message, wikiPage (scct://wiki/<page>), fix}, the plan's steps and the "
            "image. A fix is an edit of this spec: {spec: [{pointer, merge | change}], note}. Errors block level_build apply. Units, ids and positions: "
            "scct://docs/tools.";
        t.inputSchema = S::Input({{"spec", S::Free("The scct.level-spec document (scct://schema/level-spec).")},
                                  {"path", S::String("Instead of spec: a .level.json file on this machine, or scct://examples/<name>.")},
                                  {"profile", S::Enum({"draft", "release"}, "Rule severities; default the spec's profile (draft when it has none).")},
                                  {"limit", S::Integer("Most diagnostics to list, errors first. Default 300.", 1, 5000)},
                                  {"render", S::Boolean("Include the plan PNG. Default true.")},
                                  {"maxEdge", S::Integer("The plan image's longer side in pixels, 64-4096. Default 1024.", 64, 4096)},
                                  {"options", S::Free("Plan renderer options, as plan_render's.")}});
        t.outputSchema = {{"type", "object"},
                          {"properties", {{"valid", {{"type", "boolean"}}}, {"source", {{"type", "string"}}}, {"summary", {{"type", "object"}}}, {"diagnostics", {{"type", "array"}}},
                                          {"plan", {{"type", "object"}}}, {"next", {{"type", "array"}}}}},
                          {"required", {"valid", "summary", "diagnostics"}}};
        t.readOnly = true;
        t.idempotent = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return ValidateSpec(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "level_build";
        t.title = "Build a level from a spec";
        t.description =
            "Builds a whole scct.level-spec in the open map as ONE Undo step (blockout pieces, every actor through one Map JSON document, player counts, GEs, Level "
            "Properties and the stored spec), then the geometry build, Map Check and a top-view screenshot. If one of those map changes fails, they are all "
            "rolled back; a failure after them (the build, the save) keeps them, and the error's details.rolledBack says which. Check the spec with "
            "level_spec_validate first.\n"
            "- spec or path, as level_spec_validate. mode preview (default; dryRun too): the editor's preview rows and the plan PNG, nothing changes; apply builds "
            "it, and errors block it unless force.\n"
            "- sync incremental (default when the map has a stored spec, map_query what:\"spec\"): only the difference by id is built, dropped ids are deleted; "
            "replace (default otherwise): what the last build made is deleted and the spec built afresh; merge: laid over the stored spec.\n"
            "- brushMode pieces|polygons; build geometry|all|none; screenshot; save (a map path in Packages\\MapsEd named as scct://wiki/map-packaging says, such as "
            "AquaD.sdc).\n"
            "Returns created, changed and removed by spec id, validation, mapCheck, mapSettings (when not written: the checklist), undo and next (a new map: "
            "map_file save_as, editor_restart, then map_settings). More: scct://docs/tool-reference#level_build.";
        t.inputSchema = S::Mutating(S::Input({{"spec", S::Free("The scct.level-spec document (scct://schema/level-spec).")},
                                              {"path", S::String("Instead of spec: a .level.json file on this machine, or scct://examples/<name>.")},
                                              {"mode", S::Enum({"preview", "apply"}, "Default preview.")},
                                              {"sync", S::Enum({"incremental", "replace", "merge"}, "Default incremental with a stored spec, else replace.")},
                                              {"brushMode", S::Enum({"pieces", "polygons"}, "Default pieces, or the stored spec's.")},
                                              {"build", S::Enum({"geometry", "all", "none"}, "Default geometry.")},
                                              {"screenshot", S::Boolean("Default true.")},
                                              {"save", S::String("Save the map to this path afterwards.")},
                                              {"force", S::Boolean("Apply despite errors.")},
                                              {"profile", S::Enum({"draft", "release"}, "Default the spec's own.")}}));
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return Build(a, c); }); };
        registry.Add(std::move(t));
    }
}
}
