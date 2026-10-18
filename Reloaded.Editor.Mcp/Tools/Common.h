#pragma once
// What every Tools/*.cpp shares: argument readers whose errors name the
// argument, schema builders, editor calls that report progress, the result a
// mutating tool returns ({summary, created, changed, warnings, undo, next})
// and how bridge errors read to the agent. Portable.
#include "../McpTool.h"
#include "../../Reloaded.Editor/McpGuardModel.h"
#include "../../Reloaded.Editor/McpPlanModel.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

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
            Json out{{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
            if (!description.empty()) out["description"] = description;
            return out;
        }
        // {path: "MyLevel.Light12", class: "Engine.Light"}.
        inline Json Identity(const std::string& description)
        {
            return Object({{"path", {{"type", "string"}}}, {"class", {{"type", "string"}}}}, {"path", "class"}, description);
        }
        // An actor by path ("MyLevel.Light12"), object name, or identity.
        inline Json Actor(const std::string& description)
        {
            Json out{{"anyOf", Json::array({{{"type", "string"}}, Identity({})})}};
            if (!description.empty()) out["description"] = description;
            return out;
        }
        // A tool's inputSchema: a closed object.
        inline Json Input(Json properties, const std::vector<std::string>& required = {}) { return Object(std::move(properties), required); }
        // dryRun and label, which every mutating tool takes.
        inline Json Mutating(Json schema)
        {
            schema["properties"]["dryRun"] = {{"type", "boolean"}};
            schema["properties"]["label"] = String("Undo History name.");
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
    // What one exe run remembers between tools: the bridge calls of this
    // server that made Undo steps, newest last (editor_undo with no
    // arguments takes the newest; one it undid moves to undone, and comes
    // back when redone), and where editor_log stopped. A new editor process
    // numbers its calls afresh, so editor_launch and editor_restart forget.
    struct Session
    {
        struct Made { unsigned call = 0; int steps = 0; std::string op; };
        std::mutex lock;
        std::vector<Made> calls, undone;
        uint64_t logSeen = 0;
        void Push(unsigned call, int steps, const std::string& op)
        {
            if (!call) return;
            Forget(call);
            calls.push_back({call, steps, op});
            undone.clear(); // a new change ends Redo
        }
        void Forget(unsigned call)
        {
            for (auto* list : {&calls, &undone})
                list->erase(std::remove_if(list->begin(), list->end(), [&](const Made& m) { return m.call == call; }), list->end());
        }
        void Reset() { calls.clear(); undone.clear(); }
    };
    inline Session& Memory()
    {
        static Session session;
        return session;
    }

    // ---------------------------------------------------------- cancelling
    // The running request's cancel flag, which McpServer.cpp points at for
    // its worker thread and sets when the client cancels the request. Editor
    // calls stop by themselves (BridgeClient::Cancel); a tool that waits or
    // loops without calling the editor asks here, and ToolContext::progress
    // throws once the request is cancelled.
    inline const std::atomic<bool>*& CancelFlag()
    {
        thread_local const std::atomic<bool>* flag = nullptr;
        return flag;
    }
    inline bool Cancelled()
    {
        const auto* flag = CancelFlag();
        return flag && flag->load();
    }
    inline void StopIfCancelled(const std::string& what)
    {
        if (Cancelled()) throw Bridge::Error(Bridge::Code::Cancelled, what + " was cancelled.");
    }
    // Sleeps ms in short slices, throwing as soon as the request is cancelled.
    inline void Pause(unsigned ms, const std::string& what)
    {
        for (unsigned slept = 0; slept < ms; slept += 50)
        {
            StopIfCancelled(what);
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(50u, ms - slept)));
        }
        StopIfCancelled(what);
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
    // One bridge call; its progress messages go to the MCP client. With
    // expect set, the editor refuses the call as stale if the map moved on.
    inline Json Call(ToolContext& c, const std::string& op, const Json& args = Json::object(), const std::string& label = {}, bool dryRun = false, unsigned timeoutMs = 120000,
                     const Bridge::Expect& expect = {})
    {
        CallOptions options;
        options.label = label;
        options.dryRun = dryRun;
        options.expect = expect;
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
            memory.Push(call, undo.value("steps", 0), last.value("op", std::string()));
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
        if (e.code == C::Busy && e.reason == "approval_pending")
            return "A change is waiting for the user's approval in the editor, which takes no other call meanwhile: call approval_wait with its ticket "
                   "(details.ticket, or the ticket of the awaiting_approval error), or ask the user to answer the review window.";
        if (e.code == C::Busy && e.reason == "another_client") return "Close the other MCP client, or ask the user to use RE+ Tools > MCP > Disconnect client.";
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
    // Nothing in the map answers to a name: the resource read of a missing
    // actor is "not found", not a failure.
    struct NotFound : std::runtime_error { using std::runtime_error::runtime_error; };
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
            if (matches.empty()) throw NotFound("No actor named \"" + a.get<std::string>() + "\" is in the map. map_query with what \"actors\" lists them.");
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
    inline Json RunPlan(ToolContext& c, Json plan, const std::string& label, bool dryRun, unsigned timeoutMs = 600000, const Bridge::Expect& expect = {})
    {
        plan["label"] = label;
        if (dryRun) plan["dryRun"] = true;
        Plan::Check(plan);
        return Call(c, "mcp.plan", plan, label, dryRun, timeoutMs, expect);
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

    // ---------------------------------------------------------- level builds
    // level_build's path (SpecTools.cpp), which every intent tool
    // (IntentTools.cpp) builds through too. BuildContext is what a build
    // starts from: the editor's state, the stored sidecar less what the map no
    // longer has, and whether an editor answered. LevelBuild compiles spec
    // against it and previews or applies it, returning the report BuildResult
    // turns into the tool result; images gets the plan PNG or the screenshot.
    Json BuildContext(ToolContext& c, bool needEditor);
    Json LevelBuild(ToolContext& c, const Json& context, const Json& spec, const Json& request, std::vector<Content>& images);
    ToolResult BuildResult(ToolContext& c, const Json& report, const std::vector<Content>& images);

    // ------------------------------------------------------- spec documents
    // spec as given, or read from path: a .level.json on this machine (a
    // relative path is looked for in the work directory first) or an
    // embedded example, scct://examples/<name>. files is EmbeddedFiles()
    // (Embedded.h), passed in so only the tools that read specs compile the
    // embedded data.
    template<class Files> std::vector<std::string> ExampleUris(const Files& files)
    {
        std::vector<std::string> out;
        for (const auto& file : files)
        {
            std::string path = file.path;
            if (path.rfind("examples/", 0) != 0) continue;
            for (const std::string suffix : {".level.json", ".json"})
                if (path.size() > suffix.size() && path.compare(path.size() - suffix.size(), std::string::npos, suffix) == 0) { path.resize(path.size() - suffix.size()); break; }
            out.push_back("scct://" + path);
        }
        return out;
    }
    template<class Files> Json LoadSpec(const Json& a, ToolContext& c, std::string& source, const Files& files)
    {
        if (Has(a, "spec"))
        {
            if (Has(a, "path")) throw std::runtime_error("Give spec or path, not both.");
            if (!a.at("spec").is_object()) throw std::runtime_error("spec must be a scct.level-spec object (scct://schema/level-spec).");
            source = "spec";
            return a.at("spec");
        }
        const auto path = String(a, "path");
        if (path.empty()) throw std::runtime_error("Give spec (a scct.level-spec document) or path (a .level.json file, or scct://examples/<name>).");
        source = path;
        std::string text;
        if (path.rfind("scct://", 0) == 0)
        {
            const auto wanted = path.substr(7);
            bool found = false;
            for (const auto& file : files)
            {
                const std::string name = file.path;
                if (name != wanted && name != wanted + ".level.json" && name != wanted + ".json") continue;
                text.assign(reinterpret_cast<const char*>(file.data), file.size);
                found = true;
                break;
            }
            if (!found)
                throw std::runtime_error("No embedded spec " + path + ". path takes a file on this machine or one of the examples, " + Join(ExampleUris(files)) +
                                         (path.rfind("scct://map/", 0) == 0 ? "; the live map's spec is map_query what:\"spec\"." : "."));
        }
        else
        {
            auto file = std::filesystem::path(std::u8string(path.begin(), path.end()));
            std::error_code error;
            if (file.is_relative() && !c.workDir.empty() && std::filesystem::exists(c.workDir / file, error)) file = c.workDir / file;
            std::ifstream in(file, std::ios::binary);
            if (!in) throw std::runtime_error("Cannot open " + path + ". Give a full path, or one relative to the work directory " + PathText(c.workDir) + ".");
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            if (text.size() > Bridge::kMaxDocument) throw std::runtime_error(path + " is larger than a level spec can be (128 MiB).");
        }
        Json spec;
        try { spec = Json::parse(text); }
        catch (const Json::parse_error& e) { throw std::runtime_error(path + " is not JSON: " + e.what()); }
        if (!spec.is_object()) throw std::runtime_error(path + " does not hold a scct.level-spec object.");
        return spec;
    }

    // Where each intent tool's arguments go in a scct.level-spec (the
    // tools' own field lists, IntentTools.cpp; McpSpecToolsTests keeps the
    // two in step). "" for an argument with no section of its own.
    inline std::string SpecPointer(const std::string& tool, const std::string& argument)
    {
        static const std::map<std::string, std::map<std::string, std::string>> table{
            {"blockout", {{"pieces", "/geometry/pieces"}, {"doorways", "/geometry/doorways"}, {"vertical", "/geometry/vertical"}, {"brushes", "/geometry/brushes"},
                          {"surfaces", "/geometry/surfaces"}, {"meshes", "/geometry/meshes"}, {"storeys", "/storeys"}, {"defaults", "/defaults"}}},
            {"gameplay", {{"spawns", "/spawns"}, {"modes", "/gameplay/modes"}, {"story", "/gameplay/story"}, {"diskhunt", "/gameplay/diskhunt"},
                          {"deathmatch", "/gameplay/deathmatch"}, {"playerCounts", "/gameplay/playerCounts"}, {"targets", "/gameplay/targets"}}},
            {"security", {{"alarms", "/security/alarms"}, {"devices", "/security/devices"}, {"camnet", "/security/camnet"}}},
            {"mechanisms", {{"doors", "/mechanisms/doors"}, {"movers", "/mechanisms/movers"}, {"lifts", "/mechanisms/lifts"}, {"buttons", "/mechanisms/buttons"}}},
            {"events", {{"links", "/events/links"}, {"magic", "/events/magic"}, {"messages", "/events/messages"}, {"shakers", "/events/shakers"},
                        {"noises", "/events/noises"}, {"level", "/events/level"}, {"triggerLights", "/events/triggerLights"}}},
            {"environment", {{"lights", "/environment/lights"}, {"sounds", "/environment/sounds"}, {"zones", "/zones"}, {"volumes", "/environment/volumes"},
                             {"ingredients", "/environment/ingredients"}, {"objects", "/environment/objects"}, {"meshes", "/geometry/meshes"},
                             {"vision", "/environment/vision"}, {"props", "/environment/props"}, {"fish", "/environment/fish"}, {"softbody", "/environment/softbody"},
                             {"animated", "/environment/animated"}, {"tutorials", "/environment/tutorials"}}},
            {"geometric_events", {{"ges", "/ges"}, {"geTriggers", "/geTriggers"}}},
            {"map_settings", {{"level", "/level"}, {"interface", "/interface"}, {"mapSettings", "/settings"}, {"settings", "/settings"}, {"modes", "/settings/modes"},
                              {"mapName", "/settings/mapName"}, {"briefSpy", "/settings/briefSpy"}, {"briefMerc", "/settings/briefMerc"}, {"legend", "/settings/legend"},
                              {"legendInBrief", "/settings/legendInBrief"}}}};
        const auto found = table.find(tool);
        if (found == table.end()) return {};
        const auto field = found->second.find(argument);
        return field == found->second.end() ? std::string() : field->second;
    }
    // A finding's fix for a spec checked offline (level_spec_validate,
    // map_validate with spec or path): an intent tool would change the open
    // map's stored spec and build it, not the document under check, so its
    // arguments become edits of that document, {spec: [{pointer, merge}]},
    // merged at each pointer as the intent tools merge (objects field by
    // field, lists item by item by id). A level_build fix is its note; any
    // other tool's fix is for the built map and says so (live: true).
    inline Json SpecFix(const Json& fix)
    {
        if (!fix.is_object() || !fix.contains("tool") || !fix.at("tool").is_string()) return fix;
        const auto tool = fix.at("tool").get<std::string>();
        const auto args = fix.value("args", Json::object());
        Json out = Json::object();
        if (tool == "level_build")
        {
            out["spec"] = Json::array();
            out["note"] = fix.value("note", std::string("Change the spec, then check it again."));
            return out;
        }
        Json edits = Json::array();
        bool all = args.is_object() && !args.empty();
        if (args.is_object())
            for (auto it = args.begin(); it != args.end() && all; ++it)
            {
                const auto pointer = SpecPointer(tool, it.key());
                if (pointer.empty()) { all = false; break; }
                // Prose instead of a value says what to write there (only
                // the Map Settings texts are strings themselves).
                const bool text = pointer.rfind("/settings/", 0) == 0 && pointer != "/settings/modes";
                if (it.value().is_string() && !text)
                    edits.push_back({{"pointer", pointer}, {"change", it.value()}});
                else edits.push_back({{"pointer", pointer}, {"merge", it.value()}});
            }
        if (!all)
        {
            out = fix;
            out["live"] = true;
            out["note"] = fix.value("note", std::string()) + (fix.contains("note") ? " " : "") + "This applies to the map once the spec is built (level_build).";
            return out;
        }
        out["spec"] = edits;
        if (fix.contains("note")) out["note"] = fix.at("note");
        return out;
    }

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
