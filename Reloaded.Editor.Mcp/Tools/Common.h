#pragma once
// What every Tools/*.cpp shares: argument readers whose errors name the
// argument, schema builders, editor calls that report progress, the result a
// mutating tool returns ({summary, created, changed, warnings, undo, next})
// and how bridge errors read to the agent. Portable.
#include "../McpTool.h"
#include "../../Reloaded.Editor/McpGuardModel.h"
#include "../../Reloaded.Editor/McpPlanModel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace Mcp::Tools
{
    using Json = Workflow::Json;

    inline std::string Lower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }
    inline std::string Count(size_t n, const std::string& one, const std::string& many = {})
    {
        return std::to_string(n) + " " + (n == 1 ? one : many.empty() ? one + "s" : many);
    }
    // A path as UTF-8 for JSON and messages (path::string() is the ANSI code page on Windows).
    inline std::string PathText(const std::filesystem::path& path)
    {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }
    inline std::string Join(const std::vector<std::string>& items, const std::string& separator = ", ")
    {
        std::string out;
        for (const auto& item : items) out += (out.empty() ? "" : separator) + item;
        return out;
    }

    // ------------------------------------------------------------ arguments
    // Absent and null mean the same: not given.
    inline bool Has(const Json& a, const char* key) { return a.is_object() && a.contains(key) && !a.at(key).is_null(); }
    inline std::string String(const Json& a, const char* key, const std::string& fallback = {})
    {
        if (!Has(a, key)) return fallback;
        const auto& v = a.at(key);
        if (!v.is_string()) throw std::runtime_error(std::string(key) + " must be a string.");
        return v.get<std::string>();
    }
    inline std::string RequireString(const Json& a, const char* key)
    {
        auto v = String(a, key);
        if (v.empty()) throw std::runtime_error(std::string(key) + " is required.");
        return v;
    }
    inline bool Bool(const Json& a, const char* key, bool fallback = false)
    {
        if (!Has(a, key)) return fallback;
        if (!a.at(key).is_boolean()) throw std::runtime_error(std::string(key) + " must be true or false.");
        return a.at(key).get<bool>();
    }
    inline long long Integer(const Json& a, const char* key, long long fallback, long long low, long long high)
    {
        if (!Has(a, key)) return fallback;
        const auto& v = a.at(key);
        if (!v.is_number() || (v.is_number_float() && std::floor(v.get<double>()) != v.get<double>()))
            throw std::runtime_error(std::string(key) + " must be a whole number.");
        const double n = v.get<double>();
        if (n < static_cast<double>(low) || n > static_cast<double>(high))
            throw std::runtime_error(std::string(key) + " must be from " + std::to_string(low) + " to " + std::to_string(high) + ".");
        return static_cast<long long>(n);
    }
    inline double Number(const Json& a, const char* key, double fallback)
    {
        if (!Has(a, key)) return fallback;
        if (!a.at(key).is_number() || !std::isfinite(a.at(key).get<double>())) throw std::runtime_error(std::string(key) + " must be a number.");
        return a.at(key).get<double>();
    }
    inline Json Object(const Json& a, const char* key)
    {
        if (!Has(a, key)) return Json::object();
        if (!a.at(key).is_object()) throw std::runtime_error(std::string(key) + " must be an object.");
        return a.at(key);
    }
    // A list, or a single value as a list of one.
    inline Json List(const Json& a, const char* key)
    {
        if (!Has(a, key)) return Json::array();
        const auto& v = a.at(key);
        return v.is_array() ? v : Json::array({v});
    }
    inline std::vector<std::string> Strings(const Json& a, const char* key)
    {
        std::vector<std::string> out;
        for (const auto& v : List(a, key))
        {
            if (!v.is_string()) throw std::runtime_error(std::string(key) + " must be a string or a list of strings.");
            out.push_back(v.get<std::string>());
        }
        return out;
    }
    inline Workflow::Vector Vector3(const Json& v, const std::string& what)
    {
        if (!v.is_array() || v.size() != 3) throw std::runtime_error(what + " must be [x, y, z].");
        Workflow::Vector out{};
        for (size_t i = 0; i < 3; ++i)
        {
            if (!v[i].is_number() || !std::isfinite(v[i].get<double>()) || std::abs(v[i].get<double>()) > 1000000)
                throw std::runtime_error(what + " must be [x, y, z] in Unreal units, inside the world.");
            out[i] = v[i].get<double>();
        }
        return out;
    }
    inline bool DryRun(const Json& a) { return Bool(a, "dryRun", false); }
    inline std::string Label(const Json& a, const std::string& fallback)
    {
        auto label = String(a, "label");
        if (label.size() > 120) label.resize(120);
        return label.empty() ? fallback : label;
    }

    // -------------------------------------------------------------- schemas
    namespace Schema
    {
        inline Json String(const std::string& description) { return {{"type", "string"}, {"description", description}}; }
        inline Json Integer(const std::string& description, long long minimum, long long maximum)
        {
            return {{"type", "integer"}, {"description", description}, {"minimum", minimum}, {"maximum", maximum}};
        }
        inline Json Number(const std::string& description) { return {{"type", "number"}, {"description", description}}; }
        inline Json Boolean(const std::string& description) { return {{"type", "boolean"}, {"description", description}}; }
        inline Json Enum(const std::vector<std::string>& values, const std::string& description)
        {
            return {{"type", "string"}, {"enum", values}, {"description", description}};
        }
        inline Json Array(Json items, const std::string& description, size_t maxItems = 0)
        {
            Json out{{"type", "array"}, {"items", std::move(items)}, {"description", description}};
            if (maxItems) out["maxItems"] = maxItems;
            return out;
        }
        // An object whose fields are listed (closed: others are refused).
        inline Json Object(Json properties, const std::vector<std::string>& required = {}, const std::string& description = {})
        {
            Json out{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
            if (!required.empty()) out["required"] = required;
            if (!description.empty()) out["description"] = description;
            return out;
        }
        // Any JSON object (documents, property maps).
        inline Json Free(const std::string& description) { return {{"type", "object"}, {"description", description}}; }
        inline Json Vector3(const std::string& description)
        {
            return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", description}};
        }
        inline Json Identity(const std::string& description)
        {
            return Object({{"path", String("Object path, such as MyLevel.Light12.")}, {"class", String("Class path, such as Engine.Light.")}}, {"path", "class"}, description);
        }
        // An actor by path ("MyLevel.Light12"), object name, or identity.
        inline Json Actor(const std::string& description)
        {
            return {{"anyOf", Json::array({{{"type", "string"}}, Identity("The actor's identity.")})}, {"description", description}};
        }
        // A tool's inputSchema: a closed object.
        inline Json Input(Json properties, const std::vector<std::string>& required = {}) { return Object(std::move(properties), required); }
        // dryRun and label, which every mutating tool takes.
        inline Json Mutating(Json schema)
        {
            schema["properties"]["dryRun"] = Boolean("Preview only: report what would change (preview rows, the plan) and change nothing. Default false.");
            schema["properties"]["label"] = String("Short name for the Undo History entry (\"MCP <n>: <label>\") and the call history. Default: the tool name.");
            return schema;
        }
        // The result every mutating tool returns.
        inline Json MutationOutput()
        {
            return {{"type", "object"},
                    {"properties",
                     {{"summary", {{"type", "string"}}},
                      {"created", {{"type", "array"}}},
                      {"changed", {{"type", "array"}}},
                      {"warnings", {{"type", "array"}}},
                      {"undo", {{"type", {"object", "null"}}}},
                      {"next", {{"type", "array"}}}}}};
        }
    }

    // ------------------------------------------------------- session memory
    // What one exe run remembers between tools: the last bridge call that
    // made an Undo step (editor_undo's default) and where editor_log stopped.
    struct Session
    {
        std::mutex lock;
        unsigned lastCall = 0;
        std::string lastLabel;
        uint64_t logSeen = 0;
    };
    inline Session& Memory()
    {
        static Session session;
        return session;
    }

    // --------------------------------------------------------- editor calls
    inline std::string ProgressText(const std::string& op, const Json& message)
    {
        const auto stage = message.value("stage", std::string());
        if (stage == "awaiting_approval")
            return "Waiting for the user to approve the change in the editor (ticket " +
                   (message.contains("ticket") ? (message.at("ticket").is_string() ? message.at("ticket").get<std::string>() : message.at("ticket").dump()) : std::string("?")) + ").";
        std::string text = op + (stage.empty() ? std::string() : ": " + stage);
        if (message.contains("message") && message.at("message").is_string()) text += " - " + message.at("message").get<std::string>();
        if (message.contains("step") && message.at("step").is_string()) text += " (" + message.at("step").get<std::string>() + ")";
        return text;
    }
    // One bridge call; its progress messages go to the MCP client.
    inline Json Call(ToolContext& c, const std::string& op, const Json& args = Json::object(), const std::string& label = {}, bool dryRun = false, unsigned timeoutMs = 120000)
    {
        CallOptions options;
        options.label = label;
        options.dryRun = dryRun;
        options.timeoutMs = timeoutMs;
        auto steps = std::make_shared<int>(0);
        options.progress = [&c, steps, op](const Json& message) { c.progress(++*steps, 0, ProgressText(op, message)); };
        return c.bridge.Call(op, args.is_null() ? Json::object() : args, options);
    }
    // The last reply's call record (BridgeClient.h): undo, log, dialogs, warnings.
    inline Json LastCall(ToolContext& c)
    {
        try
        {
            const auto state = c.bridge.State();
            return state.is_object() && state.contains("lastCall") ? state.at("lastCall") : Json::object();
        }
        catch (const std::exception&) { return Json::object(); }
    }
    // How to undo the last call, when it made an Undo step.
    inline Json Undo(ToolContext& c)
    {
        const auto last = LastCall(c);
        const auto undo = last.value("undo", Json());
        if (!undo.is_object() || undo.value("steps", 0) <= 0) return Json();
        const auto call = undo.value("call", 0u);
        {
            auto& memory = Memory();
            std::lock_guard<std::mutex> guard(memory.lock);
            memory.lastCall = call;
            memory.lastLabel = last.value("op", std::string());
        }
        return {{"call", call}, {"steps", undo.value("steps", 0)}, {"how", "editor_undo with call " + std::to_string(call) + " reverts this whole call while it is the newest change"}};
    }
    // Warnings the editor attached to the last reply (dialogs it answered too).
    inline Json BridgeWarnings(ToolContext& c)
    {
        Json out = Json::array();
        const auto last = LastCall(c);
        for (const auto& w : last.value("warnings", Json::array())) out.push_back(w.is_string() ? w : Json(w.dump()));
        for (const auto& d : last.value("dialogs", Json::array()))
            out.push_back("The editor answered a dialog during the call: " + (d.is_object() ? d.value("title", std::string()) + " - " + d.value("text", std::string()) + " (" + d.value("answer", std::string("closed")) + ")" : d.dump()));
        return out;
    }
    inline Json Next(const std::string& tool, const std::string& why, const Json& args = Json())
    {
        Json step{{"tool", tool}, {"why", why}};
        if (!args.is_null()) step["args"] = args;
        return step;
    }

    // -------------------------------------------------------------- results
    struct Outcome
    {
        std::string summary;
        Json created = Json::array(), changed = Json::array(), warnings = Json::array(), next = Json::array();
        Json extra = Json::object(); // more fields for the structured result
    };
    // A mutating tool's result. mutated is false for a dry run or a no-op,
    // which then carries no undo block.
    inline ToolResult Done(ToolContext& c, Outcome o, bool mutated = true)
    {
        auto warnings = o.warnings;
        for (const auto& w : BridgeWarnings(c)) warnings.push_back(w);
        Json result{{"summary", o.summary}, {"created", o.created}, {"changed", o.changed}, {"warnings", warnings},
                    {"undo", mutated ? Undo(c) : Json()}, {"next", o.next}};
        for (auto it = o.extra.begin(); it != o.extra.end(); ++it) result[it.key()] = it.value();
        return Ok(result, o.summary);
    }
    // How each bridge error reads to the agent: what happened and what to do.
    inline std::string Hint(const Bridge::Error& e)
    {
        namespace C = Bridge::Code;
        if (e.code == C::Busy) return Guard::BusyHint(e.reason) + " Wait a moment and try again.";
        if (e.code == C::Stale) return "The map changed since it was read. Read it again (map_query) and repeat the change against what is there now.";
        if (e.code == C::NotConnected) return "editor_status shows the running editors; editor_launch starts one.";
        if (e.code == C::Disabled) return "The editor's MCP server is off. Only the user can turn it on: RE+ Tools > MCP > Enable MCP server.";
        if (e.code == C::Denied) return "The user declined the change in the editor's review window. Ask the user what to change before trying again.";
        if (e.code == C::DialogBlocked) return "A dialog opened during the change; it was answered with its most conservative button and the change was rolled back. details names the dialog.";
        if (e.code == C::Timeout && e.reason == "awaiting_approval") return "Call approval_wait with the ticket in details.";
        if (e.code == C::Refused && e.reason == "raw_exec_off") return "Only the user can allow raw commands, with [Mcp] AllowRawExec=1 in System\\Reloaded_Editor.ini.";
        if (e.code == C::ProtocolMismatch) return "Install Reloaded.Editor.dll and Reloaded.Editor.Mcp.exe from the same RE+ release.";
        return {};
    }
    inline ToolResult ErrorResult(const std::exception& e)
    {
        if (const auto* b = dynamic_cast<const Bridge::Error*>(&e))
        {
            Json details{{"code", b->code}, {"retryable", Bridge::Retryable(b->code)}};
            if (!b->reason.empty()) details["reason"] = b->reason;
            if (!b->details.is_null()) details["details"] = b->details;
            const auto hint = Hint(*b);
            if (!hint.empty()) details["hint"] = hint;
            std::string message = b->what();
            if (!hint.empty() && message.find(hint) == std::string::npos) message += " " + hint;
            return Fail(message, details);
        }
        return Fail(e.what());
    }
    // Runs a tool body; any exception becomes an isError result.
    template<class Body> ToolResult Guarded(Body body)
    {
        try { return body(); }
        catch (const std::exception& e) { return ErrorResult(e); }
    }

    // --------------------------------------------------------------- actors
    inline bool IsIdentity(const Json& v)
    {
        return v.is_object() && v.contains("path") && v.contains("class") && v.at("path").is_string() && v.at("class").is_string();
    }
    inline Json Identity(const Json& v) { return {{"path", v.at("path")}, {"class", v.at("class")}}; }
    inline std::string LastSegment(const std::string& path)
    {
        const auto dot = path.find_last_of('.');
        return dot == std::string::npos ? path : path.substr(dot + 1);
    }
    // Actors named by path, object name or identity, as identities, from one
    // actors.query. Every name must match exactly one live actor.
    inline Json Resolve(ToolContext& c, const Json& actors)
    {
        Json names = Json::array();
        for (const auto& a : actors)
        {
            if (IsIdentity(a)) continue;
            if (!a.is_string() || a.get<std::string>().empty()) throw std::runtime_error("Name an actor by its path (\"MyLevel.Light12\"), object name, or {path, class}.");
            names.push_back(a);
        }
        Json found = Json::array();
        if (!names.empty())
        {
            const auto q = Call(c, "actors.query", {{"names", names}, {"limit", std::min<size_t>(5000, names.size() * 4 + 16)}});
            found = q.value("actors", Json::array());
        }
        Json out = Json::array();
        for (const auto& a : actors)
        {
            if (IsIdentity(a)) { out.push_back(Identity(a)); continue; }
            const auto name = Lower(a.get<std::string>());
            std::vector<Json> matches;
            for (const auto& f : found)
                if (Lower(f.value("path", std::string())) == name) matches.push_back(f);
            if (matches.empty())
                for (const auto& f : found)
                    if (Lower(f.value("name", std::string())) == name || Lower(LastSegment(f.value("path", std::string()))) == name) matches.push_back(f);
            if (matches.empty()) throw std::runtime_error("No actor named \"" + a.get<std::string>() + "\" is in the map. map_query with what \"actors\" lists them.");
            if (matches.size() > 1) throw std::runtime_error("\"" + a.get<std::string>() + "\" names several actors; use the full path.");
            out.push_back({{"path", matches.front().at("path")}, {"class", matches.front().at("class")}});
        }
        return out;
    }

    // ---------------------------------------------------------------- plans
    inline Json Step(const std::string& id, const std::string& op, Json args, bool optional = false)
    {
        Json step{{"id", id}, {"op", op}, {"args", args.is_null() ? Json::object() : std::move(args)}};
        if (optional) step["optional"] = true;
        return step;
    }
    // A plan checked here first (McpPlanModel.h), so a misordered plan fails
    // before it reaches the editor, then run as one bridge call.
    inline Json RunPlan(ToolContext& c, Json plan, const std::string& label, bool dryRun, unsigned timeoutMs = 600000)
    {
        plan["label"] = label;
        if (dryRun) plan["dryRun"] = true;
        Plan::Check(plan);
        return Call(c, "mcp.plan", plan, label, dryRun, timeoutMs);
    }
    // A dry run's rows, and their count, for summaries.
    inline Json PreviewRows(const Json& result)
    {
        if (result.is_object() && result.contains("preview") && result.at("preview").is_array()) return result.at("preview");
        if (result.is_object() && result.contains("rows") && result.at("rows").is_array()) return result.at("rows");
        return Json::array();
    }

    // The open map at a glance: map_query summary and scct://map/summary
    // (QueryTools.cpp).
    Json MapSummary(ToolContext& c);

    // ---------------------------------------------------------------- files
    // One section of an INI file (Reloaded_Editor.ini), keys lower-cased.
    // The editor writes it through the profile API, so ANSI or UTF-16.
    inline std::map<std::string, std::string> IniSection(const std::filesystem::path& file, const std::string& section)
    {
        std::map<std::string, std::string> out;
        std::ifstream in(file, std::ios::binary);
        if (!in) return out;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xff && static_cast<unsigned char>(text[1]) == 0xfe)
        {
            std::string narrow;
            for (size_t i = 2; i + 1 < text.size(); i += 2) narrow += text[i + 1] ? '?' : text[i];
            text = narrow;
        }
        std::istringstream lines(text);
        std::string line;
        bool inside = false;
        const auto wanted = Lower(section);
        auto trim = [](std::string s)
        {
            const auto first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return std::string();
            return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
        };
        while (std::getline(lines, line))
        {
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[')
            {
                const auto close = line.find(']');
                inside = close != std::string::npos && Lower(trim(line.substr(1, close - 1))) == wanted;
                continue;
            }
            const auto equals = line.find('=');
            if (inside && equals != std::string::npos) out[Lower(trim(line.substr(0, equals)))] = trim(line.substr(equals + 1));
        }
        return out;
    }
    inline bool IniTrue(const std::map<std::string, std::string>& section, const std::string& key)
    {
        const auto found = section.find(key);
        return found != section.end() && (found->second == "1" || Lower(found->second) == "true" || Lower(found->second) == "yes");
    }
}
