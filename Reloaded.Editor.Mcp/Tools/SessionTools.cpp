// Session tools: the editor connection, starting and restarting the editor,
// Undo, approval, the log, map files and builds.
#include "Common.h"
#include "../BridgeClient.h"
#include "../../Reloaded.Editor/Version.h"
#include <chrono>
#include <thread>

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;
    using Clock = std::chrono::steady_clock;

    void Pause(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
    double Elapsed(Clock::time_point since) { return std::chrono::duration<double>(Clock::now() - since).count(); }
    std::filesystem::path EditorExe(const ToolContext& c) { return c.systemDir / "Reloaded_Editor.exe"; }
    std::filesystem::path EditorIni(const ToolContext& c) { return c.systemDir / "Reloaded_Editor.ini"; }

    // [Mcp] as the editor will read it at startup.
    Json IniSettings(const ToolContext& c)
    {
        std::error_code error;
        const auto ini = EditorIni(c);
        const auto mcp = IniSection(ini, "Mcp");
        return {{"ini", PathText(ini)}, {"found", std::filesystem::exists(ini, error)}, {"enabled", IniTrue(mcp, "enabled")},
                {"requireApproval", IniTrue(mcp, "requireapproval")}, {"allowRawExec", IniTrue(mcp, "allowrawexec")}, {"allowPlay", IniTrue(mcp, "allowplay")}};
    }
    bool Listed(const Json& editors, unsigned long pid)
    {
        for (const auto& e : editors) if (e.is_object() && e.value("pid", 0ul) == pid) return true;
        return false;
    }
    std::string MapText(const Json& state)
    {
        if (!state.value("open", true)) return "no map open";
        const auto file = state.value("mapFile", std::string());
        std::string text = file.empty() ? "an unsaved map" : file;
        if (state.value("dirty", false)) text += " (unsaved changes)";
        return text;
    }
    Json State(ToolContext& c) { return Call(c, "editor.state"); }

    // Starts the editor with -reMcp and waits until its pipe answers hello.
    Json Launch(ToolContext& c, unsigned waitSeconds, bool force)
    {
        const auto exe = EditorExe(c);
        std::error_code error;
        if (!std::filesystem::exists(exe, error))
            throw std::runtime_error("Reloaded_Editor.exe is not in " + PathText(c.systemDir) + ". Reloaded.Editor.Mcp.exe must stay in the game's System folder, beside the editor.");
        const auto settings = IniSettings(c);
        if (!force && !settings.at("enabled").get<bool>())
            throw std::runtime_error("The editor's MCP server is off ([Mcp] Enabled is not 1 in " + settings.at("ini").get<std::string>() +
                                     "). Only the user can turn it on: open the editor and tick RE+ Tools > MCP > Enable MCP server, then call editor_launch again "
                                     "(or pass force: true if it was enabled another way).");
        const auto pid = Process().launch(exe, "-reMcp", c.systemDir);
        const auto started = Clock::now();
        Json hello;
        std::string last;
        for (;;)
        {
            const double waited = Elapsed(started);
            c.progress(std::min<double>(waited, waitSeconds), waitSeconds, "Waiting for the editor (process " + std::to_string(pid) + ") to open its MCP pipe");
            if (Listed(c.bridge.Editors(), pid))
            {
                c.bridge.Select(pid);
                try { hello = c.bridge.Hello(); break; }
                catch (const Bridge::Error& e) { last = e.what(); }
            }
            if (!Process().running(pid) && !Listed(c.bridge.Editors(), pid))
                throw std::runtime_error("The editor (process " + std::to_string(pid) + ") exited while starting. Ask the user to start Reloaded_Editor.exe and look for an error.");
            if (waited >= waitSeconds)
                throw Bridge::Error(Bridge::Code::Timeout, "The editor started (process " + std::to_string(pid) + ") but its MCP pipe did not answer within " + std::to_string(waitSeconds) +
                                    " s. A window may be waiting for the user (an update offer, crash recovery, a missing package): ask the user to answer it, then call editor_status." +
                                    (last.empty() ? "" : " Last error: " + last), "startup", Json{{"pid", pid}});
            Pause(1000);
        }
        Json out{{"pid", pid}, {"hello", hello}, {"seconds", std::round(Elapsed(started) * 10) / 10}};
        try { out["state"] = State(c); } catch (const std::exception& e) { out["stateError"] = e.what(); }
        return out;
    }

    ToolResult EditorStatus(const Json& a, ToolContext& c)
    {
        Json result{{"server", {{"name", "scct-reloaded"}, {"version", RE_PLUS_VERSION}, {"systemDir", PathText(c.systemDir)}, {"workDir", PathText(c.workDir)}}}};
        std::error_code error;
        result["server"]["editorExe"] = std::filesystem::exists(EditorExe(c), error);
        result["settings"] = IniSettings(c);
        auto editors = c.bridge.Editors();
        result["editors"] = editors;
        if (Has(a, "pid"))
        {
            const auto pid = static_cast<unsigned long>(Integer(a, "pid", 0, 1, 0xffffffffLL));
            if (!Listed(editors, pid)) return Fail("No editor with process id " + std::to_string(pid) + " has an MCP pipe.", result);
            c.bridge.Select(pid);
        }
        Json next = Json::array();
        std::string summary;
        try
        {
            const auto hello = c.bridge.Hello();
            const auto state = State(c);
            result["connected"] = true;
            result["bridge"] = {{"dll", hello.value("dll", std::string())}, {"pid", hello.value("pid", 0ul)}, {"protocol", hello.value("protocol", Json())},
                                {"approval", hello.value("approval", false)}, {"enabled", hello.value("enabled", true)}, {"limits", hello.value("limits", Json())},
                                {"ops", hello.contains("ops") ? hello.at("ops").size() : 0}};
            result["state"] = state;
            summary = "Connected to RE+ " + hello.value("dll", std::string("?")) + " (process " + std::to_string(hello.value("pid", 0ul)) + "): " + MapText(state) +
                      ". Approval of changes is " + (hello.value("approval", false) ? "on: the user reviews each change in the editor." : "off.");
            if (!state.value("open", true))
                next.push_back(Next("map_file", "No map is open: start one with action new, or open an existing .sdc.", {{"action", "new"}}));
            else
                next.push_back(Next("map_query", "See what the map holds.", {{"what", "summary"}}));
            next.push_back({{"read", "scct://wiki/index"}, {"why", "Read the wiki digests and scct://rules before designing: every gameplay default and check comes from the wiki."}});
        }
        catch (const Bridge::Error& e)
        {
            result["connected"] = false;
            result["error"] = {{"code", e.code}, {"message", e.what()}, {"reason", e.reason}};
            summary = std::string("No editor connected: ") + e.what();
            if (e.code == Bridge::Code::Invalid && e.reason == "several_editors") next.push_back(Next("editor_status", "Choose one of the running editors by pid."));
            else if (editors.empty()) next.push_back(Next("editor_launch", result["settings"].value("enabled", false)
                ? "Start the editor; its MCP server is enabled in Reloaded_Editor.ini."
                : "Start the editor, after the user turns on RE+ Tools > MCP > Enable MCP server."));
        }
        result["next"] = next;
        return Ok(result, summary);
    }

    ToolResult EditorLaunch(const Json& a, ToolContext& c)
    {
        const auto waitSeconds = static_cast<unsigned>(Integer(a, "waitSeconds", 180, 10, 600));
        const bool force = Bool(a, "force");
        const auto editors = c.bridge.Editors();
        Outcome o;
        if (!editors.empty() && !force)
        {
            o.summary = "An editor with an MCP pipe is already running; editor_status connects to it.";
            o.extra = {{"alreadyRunning", true}, {"editors", editors}};
            o.next.push_back(Next("editor_status", "Connect to the running editor (pass pid when there are several)."));
            return Done(c, o, false);
        }
        if (DryRun(a))
        {
            o.summary = "Dry run: would start " + PathText(EditorExe(c)) + " -reMcp and wait up to " + std::to_string(waitSeconds) + " s for its MCP pipe; nothing was started.";
            o.extra = {{"settings", IniSettings(c)}};
            return Done(c, o, false);
        }
        auto launched = Launch(c, waitSeconds, force);
        if (Has(a, "map"))
        {
            try { launched["state"] = Call(c, "map.open", {{"path", RequireString(a, "map")}}, Label(a, "editor_launch"), false, 600000); }
            catch (const std::exception& e) { o.warnings.push_back(std::string("The editor started, but the map did not open: ") + e.what()); }
        }
        o.summary = "The editor is running (process " + std::to_string(launched.value("pid", 0ul)) + ") and connected: " + MapText(launched.value("state", Json::object())) + ".";
        o.extra = launched;
        o.next.push_back(Next("map_query", "See what the map holds.", {{"what", "summary"}}));
        return Done(c, o, false);
    }

    ToolResult EditorRestart(const Json& a, ToolContext& c)
    {
        const bool save = Bool(a, "save", true);
        const auto waitSeconds = static_cast<unsigned>(Integer(a, "waitSeconds", 180, 10, 600));
        const auto label = Label(a, "editor_restart");
        const auto state = State(c);
        const auto file = state.value("mapFile", std::string());
        const bool dirty = state.value("dirty", false);
        if (dirty && save && file.empty())
            return Fail("The map has never been saved, so a restart would lose it. Save it first with map_file action save_as (Packages\\MapsEd, four letters plus D, such as AquaD).");
        std::vector<std::string> plan;
        if (dirty) plan.push_back(save ? "save " + file : "discard the unsaved changes");
        plan.push_back("close the editor");
        plan.push_back("start it again with -reMcp");
        if (!file.empty()) plan.push_back("reopen " + file);
        Outcome o;
        if (DryRun(a))
        {
            o.summary = "Dry run: would " + Join(plan, ", then ") + ".";
            o.extra = {{"steps", plan}, {"state", state}};
            return Done(c, o, false);
        }
        if (dirty && save)
        {
            c.progress(0, 4, "Saving " + file);
            const auto saved = Call(c, "map.save", Json::object(), label, false, 600000);
            o.changed.push_back(saved.value("file", file));
        }
        const auto hello = c.bridge.Hello();
        const auto pid = hello.value("pid", 0ul);
        c.progress(1, 4, "Closing the editor");
        Call(c, "editor.quit", {{"discard", dirty && !save}}, label);
        c.bridge.Disconnect();
        const auto closing = Clock::now();
        while (Listed(c.bridge.Editors(), pid) || (pid && !Process().waitExit(pid, 1000)))
        {
            if (Elapsed(closing) > 90)
                return Fail("The editor (process " + std::to_string(pid) + ") did not close within 90 s. A dialog may be asking the user something; ask the user to look at the editor, then call editor_status.",
                            {{"pid", pid}});
            Pause(250);
        }
        c.progress(2, 4, "Starting the editor");
        auto launched = Launch(c, waitSeconds, true);
        if (!file.empty())
        {
            c.progress(3, 4, "Reopening " + file);
            launched["state"] = Call(c, "map.open", {{"path", file}}, label, false, 600000);
        }
        o.summary = "Restarted the editor (process " + std::to_string(pid) + " -> " + std::to_string(launched.value("pid", 0ul)) + ")" +
                    (file.empty() ? "." : " and reopened " + file + ".");
        o.extra = {{"previousPid", pid}, {"pid", launched.value("pid", 0ul)}, {"state", launched.value("state", Json())}};
        o.next.push_back(Next("map_query", "Map Settings (SMapSettings) can be edited now that the saved map was reopened.", {{"what", "summary"}}));
        return Done(c, o, false);
    }

    ToolResult EditorUndo(const Json& a, ToolContext& c)
    {
        const auto label = Label(a, "editor_undo");
        const bool redo = Bool(a, "redo");
        const bool hasSteps = Has(a, "steps");
        const auto steps = Integer(a, "steps", 1, 1, 100);
        unsigned call = Has(a, "call") ? static_cast<unsigned>(Integer(a, "call", 0, 1, 0xffffffffLL)) : 0;
        if (!call && !redo && !hasSteps)
        {
            auto& memory = Memory();
            std::lock_guard<std::mutex> guard(memory.lock);
            call = memory.lastCall;
        }
        std::string op = "undo.steps";
        Json args{{"count", steps}};
        std::string what = "undo " + Count(static_cast<size_t>(steps), "step");
        if (call && !redo) { op = "undo.call"; args = {{"call", call}}; what = "undo MCP call " + std::to_string(call); }
        else if (redo) { op = "redo.steps"; what = "redo " + Count(static_cast<size_t>(steps), "step"); }
        const auto before = State(c);
        if (DryRun(a))
        {
            Outcome o;
            o.summary = "Dry run: would " + what + ". canUndo " + (before.value("canUndo", false) ? "true" : "false") + ", canRedo " + (before.value("canRedo", false) ? "true" : "false") + ".";
            o.extra = {{"op", op}, {"args", args}, {"state", before}};
            return Done(c, o, false);
        }
        const auto result = Call(c, op, args, label);
        const auto after = State(c);
        if (op == "undo.call")
        {
            auto& memory = Memory();
            std::lock_guard<std::mutex> guard(memory.lock);
            if (memory.lastCall == call) memory.lastCall = 0;
        }
        Outcome o;
        o.summary = "Did " + what + ". Revision " + std::to_string(before.value("revision", 0)) + " -> " + std::to_string(after.value("revision", 0)) + "; " + MapText(after) + ".";
        o.extra = {{"op", op}, {"result", result}, {"state", after}};
        if (!redo) o.next.push_back(Next("editor_undo", "Put it back with redo.", {{"redo", true}, {"steps", op == "undo.call" ? result.value("steps", 1) : steps}}));
        return Done(c, o, false);
    }

    ToolResult ApprovalWait(const Json& a, ToolContext& c)
    {
        const auto ticket = Has(a, "ticket") && !a.at("ticket").is_string() ? a.at("ticket").dump() : RequireString(a, "ticket");
        const auto timeout = static_cast<unsigned>(Integer(a, "timeoutSeconds", 50, 1, 600));
        const auto started = Clock::now();
        for (;;)
        {
            const double waited = Elapsed(started);
            const auto left = static_cast<unsigned>(std::max(0.0, timeout - waited) * 1000);
            c.progress(std::min<double>(waited, timeout), timeout, "Waiting for the user to approve ticket " + ticket + " in the editor");
            const auto status = Call(c, "approval.status", {{"ticket", ticket}, {"waitMs", std::min(left, 5000u)}}, {}, false, std::min(left, 5000u) + 30000);
            // The client's own record of a call it stopped waiting for has
            // pending/ok/result/error; the editor's ticket has status and outcome.
            const auto state = status.value("status", std::string());
            const bool pending = status.contains("pending") ? status.value("pending", true) : (state.empty() || state == "pending" || state == "approved");
            if (!pending)
            {
                const auto outcome = status.contains("outcome") && status.at("outcome").is_object() ? status.at("outcome") : status;
                const bool settled = state == "denied" || state == "stale" || state == "cancelled";
                if (settled || !outcome.value("ok", state == "done"))
                {
                    const auto error = outcome.value("error", Json::object());
                    const auto code = settled ? (state == "stale" ? std::string(Bridge::Code::Stale) : state == "cancelled" ? std::string(Bridge::Code::Cancelled) : std::string(Bridge::Code::Denied))
                                              : error.value("code", std::string(Bridge::Code::EditorError));
                    const auto message = error.value("message", std::string(state == "denied" ? "The user declined the change." : state == "stale" ? "The map changed while the change waited for approval; it was not applied."
                                                                          : state == "cancelled" ? "The review was cancelled; the change was not applied." : "The change was not applied."));
                    return ErrorResult(Bridge::Error(code, message, error.value("reason", std::string()), error.contains("details") ? error.at("details") : Json(Json{{"ticket", ticket}})));
                }
                Outcome o;
                o.summary = "Approved and applied (ticket " + ticket + ").";
                o.extra = {{"ticket", ticket}, {"approved", true}, {"result", outcome.value("result", Json())}};
                const auto undo = outcome.value("undo", Json());
                if (undo.is_object() && undo.value("steps", 0) > 0)
                {
                    const auto call = undo.value("call", 0u);
                    {
                        auto& memory = Memory();
                        std::lock_guard<std::mutex> guard(memory.lock);
                        memory.lastCall = call;
                    }
                    o.extra["undo"] = {{"call", call}, {"steps", undo.value("steps", 0)}, {"how", "editor_undo with call " + std::to_string(call) + " reverts this whole call while it is the newest change"}};
                }
                o.next.push_back(Next("map_validate", "Check the map against the wiki rules."));
                return Done(c, o, false);
            }
            if (waited >= timeout)
                return Ok({{"ticket", ticket}, {"pending", true}, {"status", status}},
                          "Still waiting for the user's approval (ticket " + ticket + "). Call approval_wait again, or ask the user to answer the review window in the editor.");
            if (left > 1000) Pause(1000);
        }
    }

    ToolResult EditorLog(const Json& a, ToolContext& c)
    {
        const auto max = static_cast<size_t>(Integer(a, "max", 100, 1, 2000));
        const auto level = String(a, "level", "all");
        const auto contains = Lower(String(a, "contains"));
        uint64_t since = static_cast<uint64_t>(Integer(a, "since", 0, 0, 0x7fffffffffffffffLL));
        if (Bool(a, "newOnly"))
        {
            auto& memory = Memory();
            std::lock_guard<std::mutex> guard(memory.lock);
            since = std::max(since, memory.logSeen);
        }
        const auto tail = Call(c, "log.tail", {{"since", since}, {"max", 2000}});
        Json lines = Json::array();
        for (const auto& line : tail.value("lines", Json::array()))
        {
            const auto lineLevel = Lower(line.value("level", std::string()));
            if (level == "error" && lineLevel.find("error") == std::string::npos && lineLevel.find("critical") == std::string::npos) continue;
            if (level == "warning" && lineLevel.find("warn") == std::string::npos && lineLevel.find("error") == std::string::npos && lineLevel.find("critical") == std::string::npos) continue;
            if (!contains.empty() && Lower(line.value("text", std::string())).find(contains) == std::string::npos) continue;
            lines.push_back(line);
        }
        const size_t matched = lines.size();
        if (lines.size() > max) lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - max));
        const auto last = tail.value("last", uint64_t{0});
        {
            auto& memory = Memory();
            std::lock_guard<std::mutex> guard(memory.lock);
            memory.logSeen = std::max(memory.logSeen, last);
        }
        std::string text;
        for (const auto& line : lines) text += "[" + line.value("level", std::string()) + "] " + line.value("text", std::string()) + "\n";
        ToolResult r;
        r.structured = {{"lines", lines}, {"matched", matched}, {"last", last}, {"since", since}};
        r.content.push_back(TextContent(Count(lines.size(), "line") + (matched > lines.size() ? " (newest " + std::to_string(lines.size()) + " of " + std::to_string(matched) + ")" : "") +
                                        "; pass since " + std::to_string(last) + " for newer ones.\n\n" + text));
        return r;
    }

    // The wiki's file name: the first four letters of the map name and D.
    std::string FileStem(const std::string& mapName)
    {
        std::string letters;
        for (unsigned char ch : mapName) if (std::isalpha(ch) && ch < 0x80 && letters.size() < 4) letters += static_cast<char>(letters.empty() ? std::toupper(ch) : std::tolower(ch));
        if (letters.empty()) throw std::runtime_error("The map name \"" + mapName + "\" has no ASCII letters to name the file after; pass path, such as \"AquaD\".");
        return letters + "D";
    }
    std::string MapFileName(const Json& a)
    {
        std::string name;
        if (Has(a, "path")) name = RequireString(a, "path");
        else if (Has(a, "mapName")) name = FileStem(RequireString(a, "mapName"));
        else throw std::runtime_error("save_as needs path (\"AquaD\" or \"AquaD.sdc\", saved in Packages\\MapsEd) or mapName (the file is then named after its first four letters plus D, as the wiki's map-packaging page recommends).");
        if (!Bridge::EngineSafePath(name))
            throw std::runtime_error("Map file names and paths are plain ASCII without quotes (the engine reads only that): \"" + name + "\".");
        const std::filesystem::path file(name);
        const auto extension = Lower(file.extension().string());
        auto stem = extension == ".sdc" ? file.stem().string() : file.filename().string();
        if (extension != ".sdc") name += ".sdc";
        if (stem.empty() || stem.size() > 60 || stem.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            throw std::runtime_error("A map's file name is letters, digits and underscores with no spaces (its packages and loading screens are named after it; "
                                     "the wiki: four letters and D, such as AquaD): \"" + stem + "\".");
        return name;
    }

    ToolResult MapFile(const Json& a, ToolContext& c)
    {
        const auto action = RequireString(a, "action");
        const auto label = Label(a, "map_file " + action);
        const bool dry = DryRun(a), discard = Bool(a, "discardUnsaved");
        const auto state = State(c);
        Outcome o;
        // The bridge refuses too; asking first gives the reason before anything is sent.
        auto unsaved = [&](const char* doing) -> std::string
        {
            if (!state.value("dirty", false) || discard) return {};
            return std::string("The open map has unsaved changes. Save it (map_file action save) or pass discardUnsaved: true to ") + doing + " and lose them.";
        };
        if (action == "new")
        {
            if (const auto why = unsaved("start a new map"); !why.empty()) return Fail(why, {{"state", state}});
            const auto result = Call(c, "map.new", {{"discard", discard}}, label, dry, 300000);
            o.summary = dry ? "Dry run: would start a new, empty map." : "Started a new, empty map. It has no file until map_file save_as.";
            o.extra = {{"state", result}};
            o.next.push_back(Next("level_spec_validate", "Write the level as a scct.level-spec (scct://schema/level-spec, scct://examples/versus-starter) and check it offline."));
            o.next.push_back(Next("map_file", "Name and save the map early: the wiki's Map Settings only work once a new map has been saved, the editor restarted and the map reopened.", {{"action", "save_as"}}));
            return Done(c, o, false);
        }
        if (action == "open")
        {
            const auto path = RequireString(a, "path");
            if (const auto why = unsaved("open another map"); !why.empty()) return Fail(why, {{"state", state}});
            const auto result = Call(c, "map.open", {{"path", path}, {"discard", discard}}, label, dry, 600000);
            o.summary = dry ? "Dry run: would open " + path + "." : "Opened " + result.value("mapFile", path) + ".";
            o.extra = {{"state", result}};
            for (const auto& w : result.value("warnings", Json::array())) o.warnings.push_back(w);
            o.next.push_back(Next("map_query", "See what the map holds.", {{"what", "summary"}}));
            return Done(c, o, false);
        }
        if (action == "save" || action == "save_as")
        {
            Json args = Json::object();
            if (action == "save_as") args["path"] = MapFileName(a);
            else if (state.value("mapFile", std::string()).empty())
                return Fail("The map has no file yet. Use action save_as with mapName (named after its first four letters plus D, such as AquaD) or path.");
            if (Bool(a, "overwrite")) args["overwrite"] = true;
            const auto result = Call(c, "map.save", args, label, dry, 900000);
            const auto file = result.value("file", args.value("path", state.value("mapFile", std::string())));
            o.summary = dry ? "Dry run: would save the map as " + file + "." : "Saved the map to " + file + ".";
            o.changed.push_back(file);
            o.extra = {{"file", file}, {"result", result}};
            if (!dry && action == "save_as" && state.value("mapFile", std::string()).empty())
                o.next.push_back(Next("editor_restart", "The wiki (map-settings): Map Settings work only after a new map has been saved, the editor closed and the map reopened."));
            o.next.push_back(Next("map_validate", "Check the map against the wiki rules before sharing it.", {{"profile", "release"}}));
            return Done(c, o, false);
        }
        return Fail("action is new, open, save or save_as.");
    }

    std::string MapCheckLine(const Json& check)
    {
        std::map<std::string, size_t> counts;
        for (const auto& row : check.value("rows", Json::array())) ++counts[row.value("severity", std::string("info"))];
        return "Map Check: " + Count(counts["error"], "error") + ", " + Count(counts["warning"], "warning") + ", " + std::to_string(counts["info"]) + " info.";
    }

    ToolResult MapBuild(const Json& a, ToolContext& c)
    {
        const auto what = String(a, "what", "geometry");
        const bool check = Bool(a, "check", true), dry = DryRun(a);
        const auto label = Label(a, "map_build " + what);
        if (what != "geometry" && what != "all") return Fail("what is geometry or all.");
        const auto op = what == "all" ? "build.all" : "design.build";
        Outcome o;
        c.progress(0, check ? 2 : 1, what == "all" ? "Building everything (geometry, lighting, paths)" : "Building geometry");
        const auto build = Call(c, op, Json::object(), label, dry, what == "all" ? 3600000u : 900000u);
        if (dry)
        {
            o.summary = std::string("Dry run: would run ") + op + (check ? " and then Map Check." : ".");
            o.extra = {{"preview", PreviewRows(build)}};
            return Done(c, o, false);
        }
        Json report{{"build", build}};
        if (build.is_object() && build.contains("log") && build.at("log").is_array() && build.at("log").size() > 60)
        {
            Json tail = Json::array();
            for (size_t i = build.at("log").size() - 60; i < build.at("log").size(); ++i) tail.push_back(build.at("log")[i]);
            report["build"]["log"] = tail;
            report["build"]["logTrimmed"] = true;
        }
        o.summary = what == "all" ? "Built everything" : "Built geometry";
        if (build.is_object() && build.contains("seconds")) o.summary += " in " + build.at("seconds").dump() + " s";
        o.summary += ".";
        if (check)
        {
            c.progress(1, 2, "Running Map Check");
            try
            {
                auto mapcheck = Call(c, "mapcheck.run", Json::object(), {}, false, 300000);
                o.summary += " " + MapCheckLine(mapcheck);
                auto rows = mapcheck.value("rows", Json::array());
                std::stable_sort(rows.begin(), rows.end(), [](const Json& x, const Json& y)
                {
                    auto rank = [](const Json& r) { const auto s = r.value("severity", std::string()); return s == "error" ? 0 : s == "warning" ? 1 : 2; };
                    return rank(x) < rank(y);
                });
                if (rows.size() > 80) { mapcheck["rowsTrimmed"] = rows.size(); rows.erase(rows.begin() + 80, rows.end()); }
                mapcheck["rows"] = rows;
                report["mapcheck"] = mapcheck;
            }
            catch (const std::exception& e) { o.warnings.push_back(std::string("Map Check did not run: ") + e.what()); }
        }
        o.extra = report;
        o.changed.push_back(what == "all" ? "geometry, lighting and paths" : "geometry (BSP)");
        o.next.push_back(Next("map_validate", "Check the map against the wiki rules."));
        o.next.push_back(Next("view_screenshot", "Look at the result.", {{"preset", "overview"}}));
        return Done(c, o, false);
    }
}

void RegisterSessionTools(Registry& registry)
{
    {
        Tool t;
        t.name = "editor_status";
        t.title = "Editor status";
        t.description =
            "Start here. Connects to the running RE+ editor (Reloaded_Editor.exe) and reports: whether it is connected, the open map (file, unsaved changes, "
            "level/generation/revision, Undo state), whether the user requires approval for changes, the [Mcp] settings in System\\Reloaded_Editor.ini, and "
            "every editor with an MCP pipe. With several editors running, pass pid to choose one. When nothing is connected it says why and what to do "
            "(editor_launch, or ask the user to enable RE+ Tools > MCP > Enable MCP server, which only the user can turn on). Read-only.";
        t.inputSchema = S::Input({{"pid", S::Integer("Process id of the editor to use, from editors[].pid, when several are running.", 1, 0xffffffffLL)}});
        t.readOnly = true;
        t.idempotent = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return EditorStatus(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "editor_launch";
        t.title = "Start the editor";
        t.description =
            "Starts Reloaded_Editor.exe from the game's System folder with -reMcp and waits (default 180 s) until its MCP pipe answers, reporting progress. "
            "Needs [Mcp] Enabled=1 in System\\Reloaded_Editor.ini, which only the user sets (RE+ Tools > MCP > Enable MCP server); refuses otherwise unless force. "
            "Does nothing when an editor is already running (use editor_status). Optional map opens a .sdc once started (a bare name such as \"AquaD\" is looked "
            "up in Packages\\MapsEd). If the editor shows a startup window (update offer, crash recovery) the wait times out: ask the user to answer it. Windows only.";
        t.inputSchema = S::Mutating(S::Input({{"waitSeconds", S::Integer("How long to wait for the pipe, 10-600 s. Default 180.", 10, 600)},
                                              {"map", S::String("A map to open once started: a file in Packages\\MapsEd (\"AquaD\") or a full path to a .sdc.")},
                                              {"force", S::Boolean("Start even when the INI says the MCP server is off or another editor is running. Default false.")}}));
        t.outputSchema = S::MutationOutput();
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return EditorLaunch(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "editor_restart";
        t.title = "Restart the editor";
        t.description =
            "Saves the map (when it has unsaved changes), closes the editor, starts it again and reopens the same map. The wiki (map-settings) requires this "
            "after a new map is first saved: Map Settings (SMapSettings) cannot be opened until the saved map has been reopened in a fresh editor. "
            "save: false closes without saving and loses unsaved changes. A map that was never saved is refused (save it with map_file save_as first). "
            "Takes about as long as an editor start; progress is reported.";
        t.inputSchema = S::Mutating(S::Input({{"save", S::Boolean("Save unsaved changes before closing. Default true; false discards them.")},
                                              {"waitSeconds", S::Integer("How long to wait for the new editor's pipe, 10-600 s. Default 180.", 10, 600)}}));
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return EditorRestart(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "editor_undo";
        t.title = "Undo or redo";
        t.description =
            "Undoes changes. Every MCP change is one Undo step labelled \"MCP <n>: <label>\" in Undo History. With no arguments it undoes the last MCP call this "
            "server made (by its call number, so it refuses rather than undo something newer the user did); call undoes a given MCP call (each mutating "
            "tool's result has undo.call) while its steps are still the newest; steps undoes that many Undo steps whatever made them; redo with steps "
            "redoes. Returns the editor state before and after.";
        t.inputSchema = S::Mutating(S::Input({{"call", S::Integer("An MCP call number (undo.call in a tool result) to undo as a whole.", 1, 0xffffffffLL)},
                                              {"steps", S::Integer("Undo (or with redo, redo) this many steps, 1-100.", 1, 100)},
                                              {"redo", S::Boolean("Redo instead of undo. Default false.")}}));
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return EditorUndo(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "approval_wait";
        t.title = "Wait for approval";
        t.description =
            "When the user has turned on RE+ Tools > MCP > Require approval, each change opens a review window in the editor. A tool that waited about 50 s "
            "without an answer fails with code timeout, reason awaiting_approval and a ticket; call this with that ticket to keep waiting (default 50 s, "
            "progress is reported). Returns the change's result once approved, an error once declined (denied) or when the map changed meanwhile (stale), "
            "or pending: true when there is still no answer.";
        t.inputSchema = S::Input({{"ticket", S::String("The ticket from the awaiting_approval error's details.")},
                                  {"timeoutSeconds", S::Integer("How long to wait this time, 1-600 s. Default 50.", 1, 600)}},
                                 {"ticket"});
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return ApprovalWait(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "editor_log";
        t.title = "Editor log";
        t.description =
            "The editor's log (the last 2,000 lines the engine and RE+ wrote), newest last. Filter by level (warning includes errors) or contains; since "
            "returns only lines after that sequence number, and newOnly continues where the previous editor_log stopped. Useful after a build, a failed "
            "change or a dialog. Read-only.";
        t.inputSchema = S::Input({{"since", S::Integer("Only lines after this sequence number (last in a previous result).", 0, 0x7fffffffffffffffLL)},
                                  {"newOnly", S::Boolean("Continue after the last line a previous editor_log returned.")},
                                  {"max", S::Integer("At most this many lines, the newest, 1-2000. Default 100.", 1, 2000)},
                                  {"level", S::Enum({"all", "warning", "error"}, "Minimum level. Default all.")},
                                  {"contains", S::String("Only lines containing this text (case-insensitive).")}});
        t.readOnly = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return EditorLog(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "map_file";
        t.title = "New, open or save a map";
        t.description =
            "Map files. action new starts an empty map; open loads a .sdc (a bare name such as \"AquaD\" is looked up in Packages\\MapsEd); save writes the "
            "current file; save_as names it. Maps are saved in Packages\\MapsEd as ASCII names without spaces; the wiki (map-packaging) names the file after "
            "the first four letters of the map name plus D (Aquarius -> AquaD), which save_as does from mapName unless path is given. new and open refuse "
            "to drop unsaved changes unless discardUnsaved is true; save_as refuses to replace another map's file unless overwrite. Saves keep a backup of "
            "the replaced file. After the first save of a new map, the wiki requires editor_restart before Map Settings can be edited. Files are not undoable.";
        t.inputSchema = S::Mutating(S::Input(
            {{"action", S::Enum({"new", "open", "save", "save_as"}, "What to do.")},
             {"path", S::String("open: the map to open. save_as: the file name (\"AquaD\" or \"AquaD.sdc\", in Packages\\MapsEd) or a full path in Packages\\MapsEd.")},
             {"mapName", S::String("save_as without path: the map's display name (as in Map Settings, CAPS); the file is named after its first four letters plus D.")},
             {"discardUnsaved", S::Boolean("new/open: drop the open map's unsaved changes. Default false.")},
             {"overwrite", S::Boolean("save_as: replace an existing file of another map. Default false.")}},
            {"action"}));
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return MapFile(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "map_build";
        t.title = "Build the map";
        t.description =
            "Rebuilds the map, then runs Map Check. what geometry (default) rebuilds BSP after brushes or blockout pieces change, as Build Geometry does; "
            "all runs the editor's full build (geometry, lighting, paths) without the Lighting Budget prompt, which can take minutes on a large map. "
            "Returns the build log's tail and the Map Check rows (errors first). Builds are not Undo steps. Follow with map_validate.";
        t.inputSchema = S::Mutating(S::Input({{"what", S::Enum({"geometry", "all"}, "geometry (default) or all.")},
                                              {"check", S::Boolean("Run Map Check afterwards. Default true.")}}));
        t.outputSchema = S::MutationOutput();
        t.idempotent = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return MapBuild(a, c); }); };
        registry.Add(std::move(t));
    }
}
}
