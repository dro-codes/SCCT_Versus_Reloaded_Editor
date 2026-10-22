#pragma once
// mcp.plan: several bridge ops run as one call, with the map changes in one
// Undo step. Engine-free; McpNative.inl runs a checked plan.
//
//   {"label": "...", "dryRun": false,
//    "pre":   [step...]   files and packages first (loads, imports, saves);
//                         these cannot be undone
//    "steps": [step...]   joinable map edits, all in one Undo step
//    "post":  [step...]   builds, surface texturing, saves, screenshots, each
//                         on its own; a failure here keeps the steps' changes}
//   step: {"id": "s1", "op": "design.blockbatch", "args": {...}, "optional": false}
//
// A step's args may hold {"$bind": {"step": "s1", "ptr": "/0/members"}}
// anywhere: the value at that JSON pointer in an earlier step's result.
//
// Order inside steps: every step that deletes actors runs before the first
// step that pastes any. Undo of a transaction that pastes and then deletes
// faulted the editor while restoring the level's actor list
// (MapAuthoringNative.inl), so a plan that would do that is refused.
#include "McpBridgeModel.h"
#include <map>
#include <set>

namespace Mcp::Plan
{
    using Json = Workflow::Json;
    struct Step { std::string id, op; Json args = Json::object(); bool optional = false; };
    struct Parsed { std::string label; bool dryRun = false; std::vector<Step> pre, steps, post; };
    enum class Section { Pre, Steps, Post };
    inline const char* SectionName(Section section) { return section == Section::Pre ? "pre" : section == Section::Steps ? "steps" : "post"; }

    inline bool ValidId(const std::string& id)
    {
        if (id.empty() || id.size() > 63 || std::isdigit(static_cast<unsigned char>(id[0]))) return false;
        for (unsigned char c : id) if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') return false;
        return true;
    }
    inline Parsed Parse(const Json& plan)
    {
        if (!plan.is_object()) throw std::runtime_error("A plan is a JSON object.");
        static const std::set<std::string> keys{"label", "dryRun", "pre", "steps", "post"};
        for (auto it = plan.begin(); it != plan.end(); ++it)
            if (!keys.count(it.key())) throw std::runtime_error("Unknown plan field \"" + it.key() + "\".");
        Parsed parsed;
        parsed.label = plan.value("label", std::string());
        parsed.dryRun = plan.value("dryRun", false);
        std::set<std::string> ids;
        size_t total = 0;
        auto read = [&](const char* name, std::vector<Step>& into)
        {
            if (!plan.contains(name)) return;
            const auto& list = plan.at(name);
            if (!list.is_array()) throw std::runtime_error(std::string("Plan \"") + name + "\" must be an array.");
            for (const auto& item : list)
            {
                if (!item.is_object() || !item.contains("op") || !item.at("op").is_string()) throw std::runtime_error(std::string("Every step in \"") + name + "\" needs an \"op\".");
                for (auto it = item.begin(); it != item.end(); ++it)
                    if (it.key() != "id" && it.key() != "op" && it.key() != "args" && it.key() != "optional")
                        throw std::runtime_error("Unknown step field \"" + it.key() + "\".");
                Step step;
                step.op = item.at("op").get<std::string>();
                step.id = item.value("id", std::string(name) + std::to_string(into.size() + 1));
                if (!ValidId(step.id)) throw std::runtime_error("Invalid step id \"" + step.id + "\".");
                if (!ids.insert(step.id).second) throw std::runtime_error("Two steps share the id \"" + step.id + "\".");
                if (item.contains("args"))
                {
                    if (!item.at("args").is_object()) throw std::runtime_error("Step \"" + step.id + "\" args must be an object.");
                    step.args = item.at("args");
                }
                step.optional = item.value("optional", false);
                if (step.op == "mcp.plan") throw std::runtime_error("A plan cannot contain another plan.");
                into.push_back(std::move(step));
                if (++total > Bridge::kMaxPlanSteps) throw std::runtime_error("A plan holds at most " + std::to_string(Bridge::kMaxPlanSteps) + " steps.");
            }
        };
        read("pre", parsed.pre);
        read("steps", parsed.steps);
        read("post", parsed.post);
        if (!total) throw std::runtime_error("The plan has no steps.");
        return parsed;
    }

    // What a step does to the map's actor list, from its op and arguments.
    inline bool DocumentHas(const Json& args, const char* op)
    {
        if (!args.contains("document") || !args.at("document").is_object()) return false;
        const auto& document = args.at("document");
        if (!document.contains("operations") || !document.at("operations").is_array()) return false;
        for (const auto& operation : document.at("operations")) if (operation.is_object() && operation.value("op", std::string()) == op) return true;
        return false;
    }
    inline bool PackageOps(const Step& step)
    {
        return step.op == "authoring.apply" && (DocumentHas(step.args, "load") || DocumentHas(step.args, "texture") || DocumentHas(step.args, "save"));
    }
    // A document of package loads, imports and saves only: it belongs in pre.
    inline bool PackageOnly(const Step& step)
    {
        if (step.op != "authoring.apply" || !PackageOps(step)) return false;
        for (const auto& operation : step.args.at("document").at("operations"))
        {
            const auto op = operation.is_object() ? operation.value("op", std::string()) : std::string();
            if (op != "load" && op != "texture" && op != "save") return false;
        }
        return true;
    }
    inline bool Replaces(const Json& args)
    {
        if (args.contains("previous") && !args.at("previous").is_null()) return true;
        if (args.contains("items") && args.at("items").is_array())
            for (const auto& item : args.at("items")) if (item.is_object() && item.contains("previous") && !item.at("previous").is_null()) return true;
        return false;
    }
    inline bool Deletes(const Step& step)
    {
        if (step.op == "authoring.apply") return DocumentHas(step.args, "delete");
        if (step.op == "security.delete" || step.op == "stairs.remove" || step.op == "stairs.smooth") return true;
        if (step.op == "design.block" || step.op == "design.blockbatch") return Replaces(step.args);
        return false;
    }
    inline bool Pastes(const Step& step)
    {
        static const std::set<std::string> pasting{
            "design.block", "design.blockbatch", "design.lift", "design.tempstart", "start.create", "security.create",
            "security.sensor", "objective.add", "magic.create", "magic.component", "camera.add", "assembly.place",
            "emitterlib.place", "conveyor.add", "placement.copy", "stage.apply", "objectiveplayers.set",
            "objectiveplayers.targets", "vertex.portal", "stairs.smooth", "ge.add"};
        if (step.op == "authoring.apply") return DocumentHas(step.args, "create") || DocumentHas(step.args, "component");
        return pasting.count(step.op) != 0;
    }

    // Where each op may run. Reads go anywhere. Joinable edits belong in
    // steps; edits that keep their own Undo step, builds, files and processes
    // in post (files also in pre). Raw commands never run inside a plan.
    inline void CheckPolicies(const Parsed& plan)
    {
        using Bridge::Policy;
        auto check = [](const Step& step, Section section)
        {
            const auto* policy = Bridge::FindPolicy(step.op);
            if (!policy) throw std::runtime_error("Step \"" + step.id + "\": the bridge has no op \"" + step.op + "\".");
            const auto where = std::string(" (step \"") + step.id + "\" in " + SectionName(section) + ")";
            switch (policy->policy)
            {
            case Policy::Blocked: throw std::runtime_error(step.op + " is not available over the bridge" + where + ".");
            case Policy::Raw: throw std::runtime_error(step.op + " cannot run inside a plan" + where + ".");
            case Policy::Read: return;
            case Policy::Undoable:
                if (step.op == "undo.call" || step.op == "undo.steps" || step.op == "redo.steps") throw std::runtime_error("Undo and redo cannot run inside a plan" + where + ".");
                if (section == Section::Pre && !PackageOnly(step)) throw std::runtime_error(step.op + " changes the map; put it in steps" + where + ".");
                if (section == Section::Steps && !policy->joinable) throw std::runtime_error(step.op + " keeps its own Undo step; put it in post" + where + ".");
                if (section == Section::Steps && PackageOps(step)) throw std::runtime_error("Package loads, imports and saves cannot be undone; put that document in pre" + where + ".");
                return;
            case Policy::File:
                if (section == Section::Steps) throw std::runtime_error(step.op + " writes files and cannot be undone; put it in pre or post" + where + ".");
                return;
            case Policy::Build:
            case Policy::Process:
                if (section != Section::Post) throw std::runtime_error(step.op + " belongs in post" + where + ".");
                return;
            }
        };
        for (const auto& step : plan.pre) check(step, Section::Pre);
        for (const auto& step : plan.steps) check(step, Section::Steps);
        for (const auto& step : plan.post) check(step, Section::Post);
    }
    inline void CheckOrder(const Parsed& plan)
    {
        const Step* pasted = nullptr;
        for (const auto& step : plan.steps)
        {
            if (Deletes(step) && pasted)
                throw std::runtime_error("Step \"" + step.id + "\" deletes or replaces actors after step \"" + pasted->id +
                                         "\" pasted some; put every delete and replacement first, in one step.");
            if (Pastes(step) && !pasted) pasted = &step;
        }
    }
    inline Parsed Check(const Json& plan)
    {
        auto parsed = Parse(plan);
        CheckPolicies(parsed);
        CheckOrder(parsed);
        return parsed;
    }

    // {"$bind": {"step": id, "ptr": "/json/pointer"}} replaced by that value in
    // an earlier step's result; a step that has not run yet is an error.
    inline bool IsBind(const Json& value)
    {
        return value.is_object() && value.size() == 1 && value.contains("$bind") && value.at("$bind").is_object();
    }
    inline Json Bind(const Json& value, const std::map<std::string, Json>& results)
    {
        if (IsBind(value))
        {
            const auto& bind = value.at("$bind");
            const auto step = bind.value("step", std::string());
            const auto at = results.find(step);
            if (at == results.end()) throw std::runtime_error("$bind names step \"" + step + "\", which has not run before this one.");
            const auto pointer = bind.value("ptr", std::string());
            if (pointer.empty()) return at->second;
            try
            {
                const Json::json_pointer path(pointer);
                if (!at->second.contains(path)) throw std::runtime_error("missing");
                return at->second.at(path);
            }
            catch (const std::exception&) { throw std::runtime_error("$bind: step \"" + step + "\" has nothing at " + pointer + "."); }
        }
        if (value.is_object())
        {
            Json out = Json::object();
            for (auto it = value.begin(); it != value.end(); ++it) out[it.key()] = Bind(it.value(), results);
            return out;
        }
        if (value.is_array())
        {
            Json out = Json::array();
            for (const auto& item : value) out.push_back(Bind(item, results));
            return out;
        }
        return value;
    }
    // One op as a plan, so a single change still goes through the same checks,
    // approval and Undo labelling. Reads and files keep their own section.
    inline Json Single(const std::string& op, const Json& args, const std::string& label)
    {
        const auto* policy = Bridge::FindPolicy(op);
        Json step{{"id", "s1"}, {"op", op}, {"args", args.is_null() ? Json::object() : args}};
        const Step parsed{"s1", op, step.at("args")};
        const bool joined = policy && policy->policy == Bridge::Policy::Undoable && policy->joinable && !PackageOps(parsed);
        const bool first = policy && (policy->policy == Bridge::Policy::File || PackageOnly(parsed));
        Json plan{{"label", label.empty() ? op : label}};
        plan[joined ? "steps" : first ? "pre" : "post"] = Json::array({step});
        return plan;
    }
    // Every step in run order, for previews and history.
    inline std::vector<std::pair<Section, const Step*>> Ordered(const Parsed& plan)
    {
        std::vector<std::pair<Section, const Step*>> out;
        for (const auto& step : plan.pre) out.push_back({Section::Pre, &step});
        for (const auto& step : plan.steps) out.push_back({Section::Steps, &step});
        for (const auto& step : plan.post) out.push_back({Section::Post, &step});
        return out;
    }
    inline bool Mutates(const Parsed& plan)
    {
        for (const auto& [section, step] : Ordered(plan))
        {
            (void)section;
            const auto* policy = Bridge::FindPolicy(step->op);
            if (policy && Bridge::Mutates(policy->policy)) return true;
        }
        return false;
    }
    // Ops that run only as their own call, never in a plan a client sends:
    // only that call can cancel them (Bridge::Cancellable) and waits as long
    // as they take. Single's plan of such a call is that call, so Check takes
    // it; the bridge refuses a client's plan by this. "" when none is there.
    inline std::string NotInPlan(const Parsed& plan)
    {
        for (const auto& [section, step] : Ordered(plan))
            if (Bridge::Cancellable(step->op))
                return step->op + " cannot run inside a plan (step \"" + step->id + "\" in " + SectionName(section) +
                       "): only its own call can cancel it or wait as long as it takes. Call it on its own (map.recover: the map_recover tool).";
        return {};
    }
}
