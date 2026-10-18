// Escape hatches: direct actor edits, raw map-changes documents, any bridge
// op, and console commands. The intent tools and level_build cover most
// work; these reach what they do not.
#include "Common.h"
#include "../../Reloaded.Editor/McpSafetyModel.h"
#include <set>

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;

    std::string PathOf(const Json& identity) { return identity.at("path").get<std::string>(); }
    Json RotationArg(const Json& value)
    {
        if (!value.is_array() || value.size() != 3) throw std::runtime_error("rotation must be [pitch, yaw, roll] in Unreal units (65536 per turn).");
        Json out = Json::array();
        for (const auto& v : value)
        {
            if (!v.is_number() || std::abs(v.get<double>()) > 10000000) throw std::runtime_error("rotation must be [pitch, yaw, roll] in Unreal units (65536 per turn).");
            out.push_back(static_cast<long long>(std::llround(v.get<double>())));
        }
        return out;
    }

    // ----------------------------------------------------------- actors_edit
    // Each edit becomes part of one mcp.plan: set and delete as one version 3
    // map-changes document (whose deletes run first), then moves, turns,
    // property pastes, Tag renames, drops and, last, copies (which paste
    // actors, so nothing may delete after them).
    ToolResult ActorsEdit(const Json& a, ToolContext& c)
    {
        const auto edits = a.value("edits", Json::array());
        if (!edits.is_array() || edits.empty()) return Fail("edits is a list of {action, actor, ...}.");
        const bool dry = DryRun(a), allowDeletes = Bool(a, "allowDeletes");
        const auto label = Label(a, "actors_edit");
        // Every actor named, resolved in one query.
        Json named = Json::array();
        for (const auto& e : edits)
        {
            if (!e.is_object() || !e.contains("actor")) throw std::runtime_error("Every edit needs actor (a path, an object name or {path, class}).");
            named.push_back(e.at("actor"));
            if (e.value("action", std::string()) == "copy_properties")
            {
                if (!e.contains("from")) throw std::runtime_error("copy_properties needs from: the actor whose properties are copied.");
                named.push_back(e.at("from"));
            }
        }
        const auto identities = Resolve(c, named);
        size_t at = 0;
        std::map<std::string, Json> updates;          // path -> {identity, properties}
        std::vector<std::string> updateOrder;
        std::vector<Json> deletes, moves, turns, renames, pastes, drops, copies;
        Json changed = Json::array(), warnings = Json::array();
        std::set<std::string> deleted;
        for (const auto& e : edits)
        {
            const auto action = e.value("action", std::string());
            const auto identity = identities.at(at++);
            const auto path = PathOf(identity);
            if (action == "set")
            {
                const auto properties = e.value("properties", Json::object());
                if (!properties.is_object() || properties.empty()) throw std::runtime_error("set needs properties: {\"Property\": \"value\"} for " + path + ".");
                auto& update = updates[Lower(path)];
                if (update.is_null()) { update = {{"actor", identity}, {"properties", Json::object()}}; updateOrder.push_back(Lower(path)); }
                for (auto it = properties.begin(); it != properties.end(); ++it) update["properties"][it.key()] = it.value();
            }
            else if (action == "delete")
            {
                if (!allowDeletes) throw std::runtime_error("Deleting " + path + " needs allowDeletes: true on the call; deletes are never implied.");
                deletes.push_back(identity);
                deleted.insert(Lower(path));
            }
            else if (action == "move") moves.push_back({{"actor", identity}, {"delta", Vector3(e.value("delta", Json()), "move delta for " + path)}});
            else if (action == "turn") turns.push_back({{"actor", identity}, {"rotation", RotationArg(e.value("rotation", Json()))}});
            else if (action == "rename_tag")
            {
                const auto tag = e.value("tag", std::string());
                if (tag.empty()) throw std::runtime_error("rename_tag needs tag, the new Tag for " + path + ".");
                renames.push_back({{"actor", identity}, {"tag", tag}});
            }
            else if (action == "copy_properties")
            {
                const auto from = identities.at(at++);
                pastes.push_back({{"from", from}, {"target", identity}, {"only", e.value("only", Json::array())}});
            }
            else if (action == "drop") drops.push_back({{"actor", identity}, {"surface", e.value("surface", 0)}, {"align", e.value("align", false)}});
            else if (action == "copy")
            {
                Json list = Json::array();
                for (const auto& offset : e.value("offsets", Json::array()))
                {
                    Json copy{{"after", Vector3(offset, "copy offset for " + path)}};
                    if (e.contains("yaw")) { copy["yaw"] = e.at("yaw"); copy["turn"] = true; copy["pivot"] = Vector3(e.value("pivot", Json::array({0, 0, 0})), "copy pivot"); }
                    list.push_back(copy);
                }
                if (list.empty()) throw std::runtime_error("copy needs offsets: [[dx, dy, dz], ...], one per copy of " + path + ".");
                copies.push_back({{"members", Json::array({identity})}, {"copies", list}});
            }
            else throw std::runtime_error("action is set, move, turn, copy, drop, delete, copy_properties or rename_tag (got \"" + action + "\").");
            if (action != "copy") changed.push_back(path);
        }
        for (const auto& key : updateOrder)
            if (deleted.count(key)) throw std::runtime_error("An actor cannot be both deleted and updated in one call: " + PathOf(updates[key].at("actor")) + ".");

        Json plan{{"steps", Json::array()}};
        int n = 0;
        auto add = [&](const std::string& op, Json args) { plan["steps"].push_back(Step("e" + std::to_string(++n), op, std::move(args))); };
        if (!updates.empty() || !deletes.empty())
        {
            // The values every update and delete is checked against, read now.
            Json operations = Json::array();
            for (const auto& identity : deletes)
                operations.push_back({{"op", "delete"}, {"actor", identity}, {"before", Call(c, "magic.inspect", {{"actor", identity}}).value("values", Json::object())}});
            for (const auto& key : updateOrder)
            {
                const auto& update = updates[key];
                operations.push_back({{"op", "update"}, {"actor", update.at("actor")}, {"before", Call(c, "magic.inspect", {{"actor", update.at("actor")}}).value("values", Json::object())},
                                      {"properties", update.at("properties")}});
            }
            const auto map = Call(c, "authoring.mapid").value("map", std::string());
            Json document{{"format", "scct.map-changes"}, {"version", 3}, {"map", map}, {"description", label}, {"operations", operations}};
            if (!deletes.empty()) document["allowDeletes"] = true;
            add("authoring.apply", {{"document", document}});
        }
        if (!moves.empty()) add("placement.move", {{"moves", moves}, {"label", label}});
        if (!turns.empty()) add("placement.turn", {{"changes", turns}, {"label", label}});
        for (const auto& paste : pastes)
        {
            auto copied = Call(c, "props.copy", {{"actor", paste.at("from")}});
            auto properties = copied.value("properties", Json::array());
            if (!paste.at("only").empty())
            {
                std::set<std::string> only;
                for (const auto& name : paste.at("only")) only.insert(Lower(name.get<std::string>()));
                Json kept = Json::array();
                for (const auto& p : properties) if (only.count(Lower(p.value("name", std::string())))) kept.push_back(p);
                properties = kept;
            }
            if (properties.empty()) throw std::runtime_error("Nothing to copy from " + PathOf(paste.at("from")) + (paste.at("only").empty() ? "." : " among the properties named in only."));
            add("props.paste", {{"targets", Json::array({paste.at("target")})}, {"properties", properties}});
        }
        for (const auto& rename : renames)
        {
            const auto preview = Call(c, "tag.preview", {{"actor", rename.at("actor")}, {"tag", rename.at("tag")}});
            warnings.push_back("Renaming the Tag of " + PathOf(rename.at("actor")) + " also changes " + Count(preview.value("changes", Json::array()).size(), "Tag or Event slot") +
                               " that named the old Tag.");
            add("tag.rename", {{"preview", preview}});
        }
        for (const auto& drop : drops)
        {
            add("select", {{"actors", Json::array({drop.at("actor")})}});
            add("placement.drop", {{"surface", drop.at("surface")}, {"align", drop.at("align")}});
        }
        for (const auto& copy : copies)
        {
            Json args = copy;
            args["label"] = label;
            add("placement.copy", args);
        }
        const auto result = RunPlan(c, plan, label, dry);
        Outcome o;
        if (dry)
        {
            const auto rows = PreviewRows(result);
            o.summary = "Dry run: " + Count(plan["steps"].size(), "step") + ", " + Count(rows.size(), "preview row") + "; nothing changed.";
            o.extra = {{"preview", rows}, {"plan", plan}};
            o.warnings = warnings;
            return Done(c, o, false);
        }
        const auto steps = result.value("steps", Json::object());
        for (auto it = steps.begin(); it != steps.end(); ++it)
            if (it.value().is_object() && it.value().contains("created"))
                for (const auto& created : (it.value().at("created").is_array() ? it.value().at("created") : Json::array({it.value().at("created")}))) o.created.push_back(created);
        o.summary = "Edited " + Count(changed.size(), "actor") + (o.created.empty() ? "" : ", created " + Count(o.created.size(), "copy", "copies")) + " in one Undo step.";
        if (!deletes.empty()) o.summary += " Deleted " + Count(deletes.size(), "actor") + ".";
        o.changed = changed;
        o.warnings = warnings;
        o.extra = {{"steps", steps}};
        if (!moves.empty() || !deletes.empty()) o.next.push_back(Next("map_build", "Moved or deleted brushes need a geometry rebuild.", {{"what", "geometry"}}));
        o.next.push_back(Next("map_validate", "Check the map against the wiki rules."));
        return Done(c, o, true);
    }

    // ----------------------------------------------------- map_changes_apply
    ToolResult ChangesApply(const Json& a, ToolContext& c)
    {
        if (!Has(a, "document") || !a.at("document").is_object()) return Fail("document is a scct.map-changes object (docs: scct://docs/map-authoring, schema scct://schema/map-changes).");
        const auto& document = a.at("document");
        if (document.value("format", std::string()) != "scct.map-changes")
            return Fail("document.format must be \"scct.map-changes\" (a scct.level-spec goes to level_build; a scct.map-authoring export is not a change file).");
        if (!document.contains("operations") || !document.at("operations").is_array() || document.at("operations").empty()) return Fail("document.operations is a non-empty list.");
        const bool dry = DryRun(a);
        const auto label = Label(a, "map_changes_apply");
        Json args{{"document", document}};
        if (Has(a, "baseDirectory")) args["baseDirectory"] = String(a, "baseDirectory");
        Outcome o;
        if (dry)
        {
            const auto preview = Call(c, "authoring.preview", args, label, false, 600000);
            const auto rows = preview.value("rows", Json::array());
            Json shown = rows.size() > 300 ? Json(rows.begin(), rows.begin() + 300) : rows;
            o.summary = "Preview: " + preview.value("tally", Count(rows.size(), "change")) + ". Nothing changed; call again without dryRun to apply.";
            o.extra = {{"rows", shown}, {"rowCount", rows.size()}, {"tally", preview.value("tally", Json())}};
            if (preview.contains("warnings")) o.warnings = preview.at("warnings");
            return Done(c, o, false);
        }
        const auto result = Call(c, "authoring.apply", args, label, false, 900000);
        if (result.is_object() && result.contains("identities") && result.at("identities").is_object())
            for (auto it = result.at("identities").begin(); it != result.at("identities").end(); ++it) o.created.push_back({{"id", it.key()}, {"identity", it.value()}});
        Json deletedList = result.is_object() ? result.value("deleted", Json::array()) : Json::array();
        for (const auto& op : document.at("operations"))
            if (op.is_object() && op.value("op", std::string()) == "update" && op.contains("actor") && op.at("actor").is_object()) o.changed.push_back(op.at("actor").value("path", std::string()));
        o.summary = "Applied the change file in one Undo step: " + Count(o.created.size(), "object") + " created, " + Count(o.changed.size(), "update") + ", " + Count(deletedList.size(), "delete") + ".";
        o.extra = {{"deleted", deletedList}, {"result", result}};
        bool brushes = !deletedList.empty();
        for (const auto& op : document.at("operations")) brushes = brushes || (op.is_object() && op.contains("geometry"));
        if (brushes) o.next.push_back(Next("map_build", "Brushes changed: rebuild geometry.", {{"what", "geometry"}}));
        o.next.push_back(Next("map_validate", "Check the map against the wiki rules."));
        return Done(c, o, true);
    }

    // ------------------------------------------------------ workflow_request
    ToolResult Request(const Json& a, ToolContext& c)
    {
        const auto op = RequireString(a, "op");
        const auto args = Object(a, "args");
        const auto* policy = Bridge::FindPolicy(op);
        if (!policy) return Fail("The bridge has no op \"" + op + "\". scct://docs/ops lists every op with its policy and arguments.");
        if (policy->policy == Bridge::Policy::Blocked) return Fail(op + " is not available over MCP: it opens editor windows, undoes blindly or skips the staleness checks (scct://docs/ops).");
        if (policy->policy == Bridge::Policy::Raw) return Fail("Console commands go through editor_exec.");
        const bool dry = DryRun(a);
        const auto label = Label(a, "workflow_request " + op);
        const auto result = op == "mcp.plan" ? RunPlan(c, args, label, dry) : Call(c, op, args, label, dry, 900000);
        if (!Bridge::Mutates(policy->policy)) return Ok({{"op", op}, {"result", result}}, op + " (read) returned.");
        Outcome o;
        o.summary = dry ? "Dry run of " + op + ": nothing changed." : "Ran " + op + ".";
        o.extra = {{"op", op}, {"policy", Bridge::PolicyName(policy->policy)}, {"result", result}};
        if (dry) o.extra["preview"] = PreviewRows(result);
        o.next.push_back(Next("map_validate", "Check the map against the wiki rules."));
        return Done(c, o, !dry);
    }

    // ----------------------------------------------------------- editor_exec
    ToolResult Exec(const Json& a, ToolContext& c)
    {
        const auto command = RequireString(a, "command");
        if (command.size() > 1000 || command.find_first_of("\r\n") != std::string::npos) return Fail("command is one console line of at most 1000 characters.");
        if (DryRun(a)) return Ok({{"dryRun", true}, {"command", command}}, "Dry run: would run the console command \"" + command + "\"; nothing changed.");
        const auto result = Call(c, "exec", {{"command", command}}, Label(a, "editor_exec"), false, 300000);
        Outcome o;
        o.summary = std::string(result.value("ok", false) ? "The editor ran" : "The editor did not accept") + " \"" + command + "\".";
        o.extra = {{"ok", result.value("ok", false)}, {"output", result.value("output", std::string())}};
        return Done(c, o, true);
    }
}

void RegisterEscapeTools(Registry& registry)
{
    {
        Tool t;
        t.name = "actors_edit";
        t.title = "Edit actors";
        t.description =
            "Direct edits to existing actors, all in one Undo step. Each edit is {action, actor (path such as MyLevel.Light12, object name, or {path, class})}:\n"
            "- set {properties: {Prop: value}}: values in Map JSON form (every leaf a string; strings JSON-quoted, such as \"\\\"Door\\\"\"; enums bare; "
            "bools \"True\"/\"False\"; structs complete, such as Location {\"X\":\"0\",\"Y\":\"0\",\"Z\":\"48\"}; references Class'MyLevel.Name'). The current values "
            "are read first and the edit is refused if the actor changes before it applies.\n"
            "- move {delta:[x,y,z]}, turn {rotation:[pitch,yaw,roll]} (absolute, Unreal units, 65536 per turn), drop {surface (0: the first hit), align} (onto "
            "what is below), copy {offsets:[[dx,dy,dz]...], yaw?, pivot?} (one copy per offset; yaw in Unreal units about pivot).\n"
            "- delete (needs allowDeletes: true; references to it are left dangling, see map_query wiring).\n"
            "- copy_properties {from, only?:[names]}: paste another actor's editable properties.\n"
            "- rename_tag {tag}: renames the Tag (letters, digits, underscore; unique) and every Event or event group that named it.\n"
            "Follow the wiki's values (scct://wiki/*); for new gameplay prefer the intent tools or level_build, which apply the wiki defaults.";
        // The description says what each action's fields mean.
        Json edit = S::Object({{"action", {{"type", "string"}, {"enum", {"set", "move", "turn", "copy", "drop", "delete", "copy_properties", "rename_tag"}}}},
                               {"actor", S::Actor({})},
                               {"properties", {{"type", "object"}}},
                               {"delta", S::Vector3({})},
                               {"rotation", S::Vector3({})},
                               {"offsets", {{"type", "array"}, {"items", S::Vector3({})}, {"maxItems", 1000}}},
                               {"yaw", {{"type", "integer"}, {"minimum", -65536}, {"maximum", 65536}}},
                               {"pivot", S::Vector3({})},
                               {"from", S::Actor({})},
                               {"only", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                               {"surface", {{"type", "integer"}, {"minimum", 0}, {"maximum", 64}}},
                               {"align", {{"type", "boolean"}}},
                               {"tag", {{"type", "string"}}}},
                              {"action", "actor"});
        t.inputSchema = S::Mutating(S::Input({{"edits", S::Array(edit, "The edits, applied together.", 1000)},
                                              {"allowDeletes", S::Boolean("Required for any delete. Default false.")}},
                                             {"edits"}));
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return ActorsEdit(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "map_changes_apply";
        t.title = "Apply a map-changes file";
        t.description =
            "Applies a scct.map-changes document (docs: scct://docs/map-authoring; schema: scct://schema/map-changes) in one Undo step: create actors and "
            "components, update with before, link events, version 2 package loads, texture imports and saves (run first, not undoable) and polygon brushes, "
            "version 3 deletes (allowDeletes and the exact map id; map \"*\" is refused for deletes). dryRun returns the editor's preview rows (Add, Modify, "
            "Move, Delete, Link, Package, Surface) without changing anything. The result maps each create id to the actor made (created[].identity). "
            "Prefer level_build for whole levels: it writes these documents with the wiki defaults.";
        t.inputSchema = S::Mutating(S::Input({{"document", S::Free("The scct.map-changes document.")},
                                              {"baseDirectory", S::String("Folder that relative texture file paths in the document are relative to.")}},
                                             {"document"}));
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return ChangesApply(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "workflow_request";
        t.title = "Call a bridge op";
        t.description =
            "Runs one RE+ workflow op by name, {op, args}, for what no other tool covers. scct://docs/ops lists every op with its policy and arguments. The "
            "bridge's op policies still apply: blocked ops (UI windows, light.set, blind undo) are refused, console commands go through editor_exec, edits "
            "are one Undo step and honour approval and dryRun, and mcp.plan plans are checked for order first. Results come back unchanged.";
        t.inputSchema = S::Mutating(S::Input({{"op", S::String("The op, such as security.actors or camera.order.")}, {"args", S::Free("The op's arguments.")}}, {"op"}));
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return Request(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "editor_exec";
        t.title = "Run a console command";
        // The editor's own deny list (McpSafetyModel.h), so this never drifts from what it refuses.
        std::string denied;
        for (const auto* command : Safety::DeniedCommands()) denied += (denied.empty() ? "" : ", ") + std::string(command);
        t.description =
            "Runs one editor console command and returns its output. Off unless the user sets [Mcp] AllowRawExec=1 in System\\Reloaded_Editor.ini. Refused: " + denied +
            " (use map_file, editor_undo and the other tools), FILE= outside the work folder and [Mcp] WriteRoots, and PACKAGE= other than the map's own. "
            "Console commands bypass the staleness checks and may not be undoable: a last resort.";
        t.inputSchema = S::Mutating(S::Input({{"command", S::String("The console command, one line.")}}, {"command"}));
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return Exec(a, c); }); };
        registry.Add(std::move(t));
    }
}
}
