// Session tools: the editor connection, starting and restarting the editor,
// Undo, approval, the log, map files, recovering compiled maps and builds.
#include "Common.h"
#include "../BridgeClient.h"
#include "../RecoveryReportModel.h"
#include "../../Reloaded.Editor/Version.h"
#include <chrono>
#include <set>
#include <thread>

namespace Mcp
{
namespace
{
    using namespace Tools;
    namespace S = Tools::Schema;
    using Clock = std::chrono::steady_clock;

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
    std::set<unsigned long> Pids(const Json& editors)
    {
        std::set<unsigned long> pids;
        for (const auto& e : editors) if (e.is_object() && e.value("pid", 0ul)) pids.insert(e.value("pid", 0ul));
        return pids;
    }
    // Reloaded_Editor.exe logs to Reloaded_Editor.exe.log beside it; the lines
    // that say it could not start the editor, when this launch wrote the log.
    std::string LauncherFailure(const ToolContext& c, std::filesystem::file_time_type launchedAt)
    {
        const auto log = c.systemDir / "Reloaded_Editor.exe.log";
        std::error_code error;
        const auto written = std::filesystem::last_write_time(log, error);
        if (error || written + std::chrono::seconds(3) < launchedAt) return {};
        std::ifstream in(log, std::ios::binary);
        std::istringstream lines(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        std::vector<std::string> failures;
        for (std::string line; std::getline(lines, line);)
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.find("not found") != std::string::npos || line.rfind("Failed", 0) == 0) failures.push_back(line);
        }
        return Join(failures, " ");
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
    // map.open's path: a bare name ("AquaD": no folder, no extension) is
    // Packages\MapsEd\AquaD.sdc, which the editor finds only with its .sdc.
    std::string MapOpenPath(const std::string& path)
    {
        return !path.empty() && path.find_first_of("\\/:.") == std::string::npos ? path + ".sdc" : path;
    }

    // Starts the editor with -reMcp and waits until its pipe answers hello.
    // Reloaded_Editor.exe is only the launcher: it starts ChaosTheory_Editor.exe
    // with the DLL injected and exits, so the pipe belongs to a process it
    // started, not to the pid it was given. The editor is the first pipe that
    // was not there before the launch (or the launcher's own, should it ever
    // be the editor).
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
        const auto before = Pids(c.bridge.Editors());
        const auto launchedAt = std::filesystem::file_time_type::clock::now();
        const auto launcher = Process().launch(exe, "-reMcp", c.systemDir);
        const auto started = Clock::now();
        unsigned long pid = 0; // the editor's, once its pipe shows
        bool launcherGone = false;
        Json hello;
        std::string last;
        for (;;)
        {
            const double waited = Elapsed(started);
            c.progress(std::min<double>(waited, waitSeconds), waitSeconds,
                       pid ? "Waiting for the editor (process " + std::to_string(pid) + ") to answer on its MCP pipe" : std::string("Waiting for the new editor to open its MCP pipe"));
            const auto editors = c.bridge.Editors();
            if (!pid)
            {
                if (Listed(editors, launcher)) pid = launcher;
                else for (const auto found : Pids(editors)) if (!before.count(found)) { pid = found; break; }
            }
            if (pid && Listed(editors, pid))
            {
                c.bridge.Select(pid);
                try { hello = c.bridge.Hello(); break; }
                catch (const Bridge::Error& e)
                {
                    if (e.code == Bridge::Code::Cancelled) throw;
                    last = e.what();
                }
            }
            else if (pid && !Process().running(pid))
                throw std::runtime_error("The editor (process " + std::to_string(pid) + ") exited while starting. Ask the user to start Reloaded_Editor.exe and look for an error.");
            // The launcher leaving is normal; leaving a failure in its log is not.
            if (!pid && !launcherGone && !Process().running(launcher))
            {
                launcherGone = true;
                const auto failure = LauncherFailure(c, launchedAt);
                if (!failure.empty())
                    throw std::runtime_error("Reloaded_Editor.exe could not start the editor: " + failure + " ChaosTheory_Editor.exe and Reloaded.Editor.dll must be in " +
                                             PathText(c.systemDir) + "; ask the user to start Reloaded_Editor.exe there.");
            }
            if (waited >= waitSeconds)
            {
                std::string why;
                if (pid) why = "The editor started (process " + std::to_string(pid) + ") but its MCP pipe did not answer within " + std::to_string(waitSeconds) + " s.";
                else if (!launcherGone) why = "Reloaded_Editor.exe (process " + std::to_string(launcher) + ") had not started the editor after " + std::to_string(waitSeconds) +
                                             " s; it may be showing a message (ChaosTheory_Editor.exe or Reloaded.Editor.dll missing).";
                else why = "The editor was started, but no new MCP pipe opened within " + std::to_string(waitSeconds) + " s (if no editor window appeared, System\\Reloaded_Editor.exe.log says why).";
                throw Bridge::Error(Bridge::Code::Timeout, why + " A window may be waiting for the user (an update offer, crash recovery, a missing package): ask the user to answer it, then call editor_status." +
                                    (last.empty() ? "" : " Last error: " + last), "startup", Json{{"pid", pid}, {"launcher", launcher}});
            }
            Pause(500, "editor_launch");
        }
        // The new editor numbers its calls from 1: the old ones cannot be undone by number.
        {
            auto& memory = Memory();
            std::lock_guard<std::mutex> guard(memory.lock);
            memory.Reset();
            memory.Editor(pid);
        }
        Json out{{"pid", pid}, {"launcher", launcher}, {"hello", hello}, {"seconds", std::round(Elapsed(started) * 10) / 10}};
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
            NoteEditor(c, hello);
            const auto state = State(c);
            result["connected"] = true;
            result["bridge"] = {{"dll", hello.value("dll", std::string())}, {"pid", hello.value("pid", 0ul)}, {"protocol", hello.value("protocol", Json())},
                                {"approval", hello.value("approval", false)}, {"enabled", hello.value("enabled", true)}, {"limits", hello.value("limits", Json())},
                                {"ops", hello.contains("ops") ? hello.at("ops").size() : 0}, {"opsDoc", "scct://docs/ops"}};
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
            try { launched["state"] = Call(c, "map.open", {{"path", MapOpenPath(RequireString(a, "map"))}}, Label(a, "editor_launch"), false, 600000); }
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
            Pause(250, "editor_restart");
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
        if (redo && Has(a, "call")) return Fail("redo takes steps, not call: it redoes the newest undone Undo steps.");
        unsigned call = Has(a, "call") ? static_cast<unsigned>(Integer(a, "call", 0, 1, 0xffffffffLL)) : 0;
        // The editor answers first, so the calls of an editor that has gone
        // since are forgotten before one is picked.
        const auto before = State(c);
        NoteEditor(c, before);
        // No arguments: this server's newest call that still has Undo steps,
        // never a blind undo, which could take back something the user did.
        if (!call && !redo && !hasSteps)
        {
            {
                auto& memory = Memory();
                std::lock_guard<std::mutex> guard(memory.lock);
                if (!memory.calls.empty()) call = memory.calls.back().call;
            }
            if (!call)
                return Fail("This server has made no change it can still undo in this editor, so nothing was undone (with no arguments editor_undo only takes back this "
                            "server's own calls). Pass call (undo.call in a tool result) for a given MCP call, or steps to undo the newest Undo steps whoever made them.",
                            {{"next", Json::array({Next("map_query", "See what the map holds now.", {{"what", "summary"}})})}});
        }
        std::string op = "undo.steps";
        Json args{{"count", steps}};
        std::string what = "undo " + Count(static_cast<size_t>(steps), "step");
        if (call && !redo) { op = "undo.call"; args = {{"call", call}}; what = "undo MCP call " + std::to_string(call); }
        else if (redo) { op = "redo.steps"; what = "redo " + Count(static_cast<size_t>(steps), "step"); }
        if (DryRun(a))
        {
            Outcome o;
            o.summary = "Dry run: would " + what + ". canUndo " + (before.value("canUndo", false) ? "true" : "false") + ", canRedo " + (before.value("canRedo", false) ? "true" : "false") + ".";
            o.extra = {{"op", op}, {"args", args}, {"state", before}};
            return Done(c, o, false);
        }
        Json result;
        try { result = Call(c, op, args, label); }
        catch (const Bridge::Error& e)
        {
            if (op != "undo.call") throw;
            // Calls the editor has no record of (another editor, or one it
            // dropped), has undone already or no longer has steps of: there
            // is nothing to take back by number.
            const bool gone = e.reason == "gone" || e.reason == "already_undone" || e.reason == "unknown_call" || (e.code == Bridge::Code::Invalid && e.reason.empty());
            // Calls it can no longer undo whole: partly undone, mixed with
            // other changes, or partly out of a full buffer.
            const bool broken = e.reason == "partly_undone" || e.reason == "interleaved" || e.reason == "trimmed";
            if (!gone && !broken) throw; // not_newest: undoable once the newer steps are
            bool known = false;
            {
                auto& memory = Memory();
                std::lock_guard<std::mutex> guard(memory.lock);
                for (const auto* list : {&memory.calls, &memory.undone})
                    known = known || std::any_of(list->begin(), list->end(), [&](const Session::Made& m) { return m.call == call; });
                memory.Forget(call);
            }
            if (!known) throw;
            const auto name = "MCP call " + std::to_string(call);
            throw Bridge::Error(e.code, std::string(e.what()) + " editor_undo has forgotten " + name +
                                        (broken ? ", which can no longer be undone as a whole: to take back its edits, look at Undo History and undo by steps (they may take the user's own changes with them)."
                                                : "; called with no arguments it now takes this server's call before it."),
                                e.reason, e.details);
        }
        const auto after = State(c);
        // How many steps to redo to put it back: undo.call says how many it
        // undid ({call, done, steps}), the others {done}.
        auto whole = [&](const char* key) { return result.is_object() && result.contains(key) && result.at(key).is_number_integer() ? result.at(key).get<long long>() : 0LL; };
        long long undid = result.is_object() && result.contains("done") ? whole("done") : steps;
        {
            auto& memory = Memory();
            std::lock_guard<std::mutex> guard(memory.lock);
            if (op == "undo.call")
            {
                Session::Made made{call, 1, "MCP call"};
                for (const auto& m : memory.calls) if (m.call == call) made = m;
                if (whole("done") > 0) made.steps = static_cast<int>(std::min(whole("done"), 100000LL));
                else if (whole("steps") > 0) made.steps = static_cast<int>(std::min(whole("steps"), 100000LL));
                undid = made.steps;
                memory.Forget(call);
                memory.undone.push_back(made);
            }
            else if (op == "redo.steps")
            {
                // Redone steps bring back the calls they undid, newest undone first.
                auto left = undid;
                while (!memory.undone.empty() && memory.undone.back().steps <= left)
                {
                    left -= memory.undone.back().steps;
                    memory.calls.push_back(memory.undone.back());
                    memory.undone.pop_back();
                }
            }
        }
        Outcome o;
        o.summary = "Did " + what + ". Revision " + std::to_string(before.value("revision", 0)) + " -> " + std::to_string(after.value("revision", 0)) + "; " + MapText(after) + ".";
        o.extra = {{"op", op}, {"result", result}, {"state", after}};
        if (!redo && undid > 0) o.next.push_back(Next("editor_undo", "Put it back with redo.", {{"redo", true}, {"steps", std::min(undid, 100LL)}}));
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
                    NoteEditor(c);
                    {
                        auto& memory = Memory();
                        std::lock_guard<std::mutex> guard(memory.lock);
                        memory.Push(call, undo.value("steps", 0), "approved change");
                    }
                    o.extra["undo"] = {{"call", call}, {"steps", undo.value("steps", 0)}, {"how", "editor_undo with call " + std::to_string(call) + " reverts this whole call while it is the newest change"}};
                }
                o.next.push_back(Next("map_validate", "Check the map against the wiki rules."));
                return Done(c, o, false);
            }
            if (waited >= timeout)
                return Ok({{"ticket", ticket}, {"pending", true}, {"status", status}},
                          "Still waiting for the user's approval (ticket " + ticket + "). Call approval_wait again, or ask the user to answer the review window in the editor.");
            if (left > 1000) Pause(1000, "approval_wait");
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
    // A map file name as map.save takes it (Packages\MapsEd, ASCII letters,
    // digits and underscores), with .sdc added when it has none.
    std::string CheckedMapName(std::string name)
    {
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
    std::string MapFileName(const Json& a)
    {
        if (Has(a, "path")) return CheckedMapName(RequireString(a, "path"));
        if (Has(a, "mapName")) return CheckedMapName(FileStem(RequireString(a, "mapName")));
        throw std::runtime_error("save_as needs path (\"AquaD\" or \"AquaD.sdc\", saved in Packages\\MapsEd) or mapName (the file is then named after its first four letters plus D, as the wiki's map-packaging page recommends).");
    }

    // ---------------------------------------------------------- map_recover
    // Recovery runs every editor build and saves twice; the slowest maps take
    // well over an hour on a slow PC.
    constexpr unsigned kRecoverMs = 3u * 3600u * 1000u;
    // The editable map File > Recover Compiled Map would make from source:
    // <stem>_Recovered (the stem made a map name, Recovery::DefaultBase),
    // numbered past what is in Packages when the server can see the game's
    // folder.
    std::string RecoveryName(const ToolContext& c, const std::string& source)
    {
        const auto stem = Recovery::Stem(source);
        std::error_code error;
        if (!std::filesystem::is_directory(GamePackages(c), error)) return Recovery::DefaultBase(stem) + "_Recovered";
        const auto name = Recovery::DefaultName(stem, [&](const std::string& relative) { std::error_code e; return std::filesystem::exists(InPackages(c, relative), e); });
        return name.empty() ? Recovery::DefaultBase(stem) + "_Recovered" : name;
    }
    // A plain MAP LOAD of a compiled map (Packages\Maps) loses alignment in
    // its cooked BSP, so the bridge refuses map.open of one (compiled_map);
    // asked here first for paths the server can tell are compiled: relative
    // to the game ("Packages\Maps\OffsD.sdc") or in this game's Packages\Maps.
    bool CompiledMapPath(const ToolContext& c, const std::string& path)
    {
        auto folded = [](std::string text)
        {
            for (auto& ch : text) if (ch == '/') ch = '\\';
            return Lower(text);
        };
        const auto file = folded(path);
        if (file.size() < 5 || file.compare(file.size() - 4, 4, ".sdc") != 0) return false;
        const auto maps = folded(PathText(GamePackages(c) / "Maps")) + "\\";
        const std::string folders[] = {"packages\\maps\\", ".\\packages\\maps\\", maps};
        for (const auto& folder : folders)
            if (file.size() > folder.size() && file.compare(0, folder.size(), folder) == 0 && file.find('\\', folder.size()) == std::string::npos) return true;
        return false;
    }
    Json RecoverArgs(const ToolContext& c, const std::string& source) { return {{"source", source}, {"name", RecoveryName(c, source)}}; }
    // The refusal in the tools' terms, whether the server or the bridge found
    // it: the bridge's own message speaks of map.open and destination; its
    // details give the path it resolved and the MapsEd source if there is one.
    ToolResult CompiledMapRefusal(const ToolContext& c, const std::string& path, const Json& details = Json())
    {
        const auto args = RecoverArgs(c, path);
        // A stock map's editable source has the same name in Packages\MapsEd:
        // the original file, which recovery can only approach.
        std::string twin;
        if (details.is_object() && details.contains("source") && details.at("source").is_string()) twin = details.at("source").get<std::string>();
        else
        {
            std::error_code error;
            const auto candidate = GamePackages(c) / "MapsEd" / (Recovery::Stem(path) + ".sdc");
            if (std::filesystem::is_regular_file(candidate, error)) twin = PathText(candidate);
        }
        Json next = Json::array();
        std::string text = path + " is a compiled map (Packages\\Maps): opened as a source map its geometry loses alignment, so it was not opened.";
        if (!twin.empty())
        {
            text += " Packages\\MapsEd has " + Recovery::Stem(path) + ".sdc, normally its original editable source: open that instead (exact, nothing rebuilt).";
            next.push_back(Next("map_file", "Open the original editable source of this map.", {{"action", "open"}, {"path", twin}}));
        }
        next.push_back(Next("map_recover", "Recover the compiled map as an editable source map.", args));
        return Fail(text + " To turn the compiled map itself into an editable map, use map_recover (source \"" + path + "\", name \"" + args.at("name").get<std::string>() +
                        "\"; dryRun: true checks it first).",
                    {{"code", Bridge::Code::Refused}, {"reason", "compiled_map"}, {"retryable", false}, {"next", next}});
    }

    // A refused or failed recovery: the bridge's code and reason, a message
    // that says what to do, and the calls that do it.
    ToolResult RecoverFailure(const Bridge::Error& e, const std::string& message, const Json& next = Json::array())
    {
        Json details{{"code", e.code}, {"retryable", Bridge::Retryable(e.code)}};
        if (!e.reason.empty()) details["reason"] = e.reason;
        if (!e.details.is_null()) details["details"] = e.details;
        if (!next.empty()) details["next"] = next;
        return Fail(message, details);
    }
    ToolResult RecoverError(const Bridge::Error& e, const ToolContext& c, const Json& args, const std::string& destination)
    {
        namespace C = Bridge::Code;
        const auto& source = args.at("source").get<std::string>();
        const auto stem = Recovery::Stem(destination);
        Json retry = Json{{"source", source}, {"name", stem}};
        if (args.contains("merge")) retry["merge"] = args.at("merge");
        if (args.value("keepXboxOnly", false)) retry["keepXboxOnly"] = true;
        if (args.value("legacy", false)) retry["legacy"] = true;
        auto with = [&](const char* key, const Json& value) { Json out = retry; out[key] = value; return out; };
        // A name for a retry: the editor's own suggestion when it made one
        // (destination_exists), else the next default name.
        auto another = [&]
        {
            if (e.details.is_object() && e.details.contains("suggestion") && e.details.at("suggestion").is_string() && !e.details.at("suggestion").get<std::string>().empty())
                return e.details.at("suggestion").get<std::string>();
            const auto other = RecoveryName(c, source);
            return Lower(other) == Lower(stem) ? stem + "_2" : other;
        };
        // A recovery that stopped before saving: the editor says whether it
        // started a new map and which files were in use and remain
        // (MapRecovery.cpp FinishCancelledRecovery).
        auto stopped = [&](const std::string& said, const std::string& before)
        {
            const auto low = Lower(said);
            const bool reset = low.find("could not start a new map") == std::string::npos, remain = low.find("remain") != std::string::npos;
            std::string text = before + (low.find("new empty map") != std::string::npos || !reset || remain
                                             ? said : std::string("The recovery was cancelled. The editor now has a new, empty map, and the files the recovery had written were removed."));
            Json next = Json::array();
            if (!reset)
            {
                text += " Restart the editor (editor_restart) before opening or recovering another map.";
                next.push_back(Next("editor_restart", "Restart the editor, as it asked."));
            }
            else next.push_back(Next("editor_status", "See which map the editor has now."));
            if (remain)
            {
                const auto other = another();
                text += " Those files keep the name " + stem + " taken: recover again under another name, such as \"" + other + "\".";
                next.push_back(Next("map_recover", "Recover under a name nothing uses yet.", with("name", other)));
            }
            return RecoverFailure(e, text + " A cancel is not taken once it is saving (Saving, Reopening and verifying): then it finishes, so check editor_status before starting again.", next);
        };
        if (e.reason == "missing_packages")
        {
            std::vector<std::string> missing;
            if (e.details.is_object())
            {
                const Json list = e.details.value("missing", Json::array());
                for (const auto& name : list) missing.push_back(name.is_string() ? name.get<std::string>() : name.dump());
            }
            return RecoverFailure(e, source + " uses " + (missing.empty() ? std::string("packages") : Count(missing.size(), "package") + " (" + Join(missing) + ")") +
                                         " that this game does not have, and loading it would stop the editor at a missing-package dialog, so nothing was changed. "
                                         "Ask the user to install them: each into its Packages folder (.utx Textures, .usx StaticMeshes, .uax Sounds, .ukx Animations; "
                                         ".u into System), from wherever the map came from, such as the SCCT-Maps collection. Then run map_recover again (dryRun: true checks first).",
                                  Json::array({Next("map_recover", "Check again once the packages are installed.", with("dryRun", true))}));
        }
        if (e.reason == "destination_exists")
        {
            // In the tool's terms (name, not the bridge's destination), with one suggestion.
            std::vector<std::string> existing;
            if (e.details.is_object())
            {
                const Json list = e.details.value("existing", Json::array());
                for (const auto& path : list) if (path.is_string()) existing.push_back(path.get<std::string>());
            }
            const auto other = another();
            return RecoverFailure(e, "The name " + stem + " is taken" + (existing.empty() ? std::string() : ": " + Join(existing) + (existing.size() > 1 ? " already exist" : " already exists")) +
                                         ". Recovery never replaces files (the map, its Packages\\Maps copy, StaticMeshes\\" + Recovery::AssetPackage(stem) + ".usx or MapsEd\\Recovery\\" +
                                         stem + "), so nothing was changed. Pass another name, such as \"" + other + "\".",
                                  Json::array({Next("map_recover", "Recover under a name nothing uses yet.", with("name", other))}));
        }
        if (e.reason == "unsaved_changes")
            return RecoverFailure(e, "Recovery replaces the open map (it starts with MAP NEW), and the open map has unsaved changes, so nothing was changed. Save it first "
                                     "(map_file action:\"save\", or save_as for a map with no file yet), or pass discard: true to lose them.",
                                  Json::array({Next("map_file", "Save the open map first.", {{"action", "save"}})}));
        // A file in Packages\MapsEd is normally an editable map already; a legacy
        // recovered map (compiled layout, older RE+) or a compiled copy put
        // there is recovered with legacy, as File > Convert Legacy Recovered Map does.
        if (e.reason == "source_map")
            return RecoverFailure(e, source + " is in Packages\\MapsEd, where maps are normally editable already: open it with map_file action:\"open\". "
                                     "Only if it is a legacy recovered map (saved in compiled layout by an older RE+) or a compiled map copied there, recover it with legacy: true.",
                                  Json::array({Next("map_file", "Open the source map.", {{"action", "open"}, {"path", source}}),
                                               Next("map_recover", "Recover it as a legacy recovered map or compiled copy.", with("legacy", true))}));
        if (e.code == C::Busy && e.reason != "approval_pending" && e.reason != "another_client")
        {
            std::string said = e.what();
            if (const auto hint = Guard::BusyHint(e.reason); said.find(hint) == std::string::npos) said += " " + hint;
            return RecoverFailure(e, said + " A recovery started from the File menu, or Rebuild All Maps, has to finish before map_recover can start: wait for it "
                                     "(editor_status shows the editor; leave its progress window open, since closing it cancels that recovery), then try again.",
                                  Json::array({Next("editor_status", "See whether the editor is free.")}));
        }
        if (e.code == C::Cancelled) return stopped(e.what(), {});
        if (e.code == C::Timeout && e.reason != "awaiting_approval")
            return RecoverFailure(e, std::string(e.what()) + " It stops at its next stage and leaves a new, empty map, unless it was already saving, which then finishes: "
                                     "editor_status, and editor_log with contains \"MapRecovery\", show which happened.",
                                  Json::array({Next("editor_status", "See which map the editor has now."),
                                               Next("editor_log", "The recovery's own log lines.", {{"contains", "MapRecovery"}})}));
        // An unknown dialog: the bridge answered it with its most cautious
        // button and stopped the call. details.cause is set only when the
        // recovery itself failed; from Saving on a recovery no longer stops,
        // so without a cause it finished and saved.
        Json cause = e.code == C::DialogBlocked && e.details.is_object() ? e.details.value("cause", Json()) : Json();
        if (!cause.is_object()) cause = Json();
        if (e.code == C::DialogBlocked && cause.is_null())
            return RecoverFailure(e, std::string(e.what()) + " The dialog came once the recovery was saving, when it no longer stops, so it finished: " + destination +
                                         ", its Packages\\Maps copy and its Recovery Report were saved, and the map is open. Do not run map_recover again; read the report, "
                                         "and ask the user what the dialog said.",
                                  Json::array({Next("map_query", "The finished recovery's report.", {{"what", "recovery"}, {"file", stem}}),
                                               Next("editor_status", "The map the editor has open.")}));
        if (e.code == C::DialogBlocked && cause.value("code", std::string()) == C::Cancelled)
            return stopped(cause.value("message", std::string()), std::string(e.what()) + " The recovery stopped at the dialog. ");
        if (e.code == C::EditorError || e.code == C::DialogBlocked)
        {
            const std::string failure = cause.is_null() ? e.what() : std::string(e.what()) + " " + cause.value("message", std::string());
            Json next = Json::array({Next("editor_log", "The recovery's own log lines, stage by stage.", {{"contains", "MapRecovery"}})});
            std::string advice;
            const auto what = Lower(failure);
            if (args.value("merge", true) && (what.find("solid/empty") != std::string::npos || what.find("bsp rebuild") != std::string::npos))
            {
                advice = " Merging may be what the BSP build could not reproduce: try again with merge: false (only cells sharing whole faces are joined).";
                next.push_back(Next("map_recover", "Recover without merging.", with("merge", false)));
            }
            // What the failed recovery left for inspection (details.left): those
            // names stay taken, so a retry needs another name.
            std::vector<std::string> left;
            if (e.details.is_object())
            {
                const Json list = e.details.value("left", Json::array());
                for (const auto& path : list) if (path.is_string()) left.push_back(path.get<std::string>());
            }
            std::string kept = " Nothing was saved as " + destination + ".";
            if (!left.empty())
            {
                const auto other = another();
                kept = " Nothing usable was saved as " + destination + "; it left " + Join(left) + " for inspection, so recover again under another name, such as \"" + other + "\".";
                for (auto& step : next) if (step.value("tool", std::string()) == "map_recover") step["args"]["name"] = other;
            }
            return RecoverFailure(e, "The recovery failed: " + failure + kept +
                                         " The editor may be left with an unsaved working map, which map_file action:\"new\" with discardUnsaved: true clears." + advice,
                                  next);
        }
        return ErrorResult(e);
    }
    // The preflight's own failures. Refusals read as RecoverError says; for
    // anything else the checks never finished, and nothing of the recovery
    // ran. A busy editor comes back within seconds (CallOptions::busyMs).
    ToolResult CheckError(const Bridge::Error& e, const ToolContext& c, const Json& args, const std::string& destination)
    {
        namespace C = Bridge::Code;
        for (const char* reason : {"missing_packages", "destination_exists", "unsaved_changes", "source_map"})
            if (e.reason == reason) return RecoverError(e, c, args, destination);
        std::string text = std::string("map_recover's checks found this before anything ran: ") + e.what();
        Json next = Json::array();
        if (e.code == C::Timeout || (e.code == C::Busy && e.reason != "approval_pending" && e.reason != "another_client"))
        {
            if (e.code == C::Busy)
                if (const auto hint = Guard::BusyHint(e.reason); text.find(hint) == std::string::npos) text += " " + hint;
            text += " The editor did not take the checks: a recovery started from the File menu, or Rebuild All Maps, holds it until it is done (leave its progress "
                    "window open, since closing it cancels that recovery). Wait for it, then try again.";
            next.push_back(Next("editor_status", "See whether the editor is free."));
        }
        else if (const auto hint = Hint(e); !hint.empty() && text.find(hint) == std::string::npos) text += " " + hint;
        return RecoverFailure(e, text + " Nothing was started or changed.", next);
    }

    // The tool's arguments as the bridge op takes them.
    Json RecoverRequest(const Json& a, const std::string& source, const std::string& destination)
    {
        Json args{{"source", source}, {"destination", destination}, {"keepXboxOnly", Bool(a, "keepXboxOnly")}, {"discard", Bool(a, "discard")},
                  {"maxRows", Integer(a, "maxRows", 200, 0, 2000)}};
        if (Bool(a, "legacy")) args["legacy"] = true;
        // Absent: the editor's File > Merge Recovered Geometry setting.
        if (Has(a, "merge")) args["merge"] = Bool(a, "merge");
        return args;
    }
    std::string OnOff(bool value) { return value ? "on" : "off"; }

    ToolResult MapRecover(const Json& a, ToolContext& c)
    {
        const auto source = RequireString(a, "source");
        const auto sourceStem = Recovery::Stem(source);
        if (sourceStem.empty() || sourceStem.find('.') != std::string::npos)
            return Fail("source is the compiled map: \"OffsD\" or \"OffsD.sdc\" (in Packages\\Maps), \"Packages\\Maps\\OffsD.sdc\", or a full path to a .sdc.");
        const auto destination = CheckedMapName(Has(a, "name") ? RequireString(a, "name") : RecoveryName(c, source));
        const auto label = Label(a, "map_recover " + sourceStem);
        const bool dry = DryRun(a), discard = Bool(a, "discard");
        auto args = RecoverRequest(a, source, destination);
        const auto state = State(c);
        const bool dirty = state.value("dirty", false) && !discard;
        Outcome o;
        Json preflight;
        // Always checked first, with a short timeout and a busy editor not
        // retried: a refusal comes back in seconds and a busy editor in about
        // ten (the bridge's pickup time) instead of waiting out the
        // recovery's hours. With unsaved changes the check runs as if
        // discarding, so a dry run reports everything else too.
        try
        {
            auto check = args;
            if (dirty) check["discard"] = true;
            check["dryRun"] = true;
            CallOptions options;
            options.label = label;
            options.dryRun = true;
            options.timeoutMs = 300000;
            options.busyMs = 5000;
            options.progress = [&c](const Json& m) { c.progress(0, 100, ProgressText("map.recover", m)); };
            preflight = c.bridge.Call("map.recover", check, options);
        }
        catch (const Bridge::Error& e) { return CheckError(e, c, args, destination); }
        const auto missing = preflight.value("missing", Json::array());
        if (missing.is_array() && !missing.empty())
            return CheckError(Bridge::Error(Bridge::Code::Refused, "Packages are missing.", "missing_packages", Json{{"missing", missing}}), c, args, destination);
        const bool merge = preflight.value("merge", args.value("merge", true));
        const bool keep = preflight.value("keepXboxOnly", args.at("keepXboxOnly").get<bool>());
        Json retry{{"source", source}, {"name", Recovery::Stem(destination)}};
        if (Has(a, "merge")) retry["merge"] = merge;
        if (keep) retry["keepXboxOnly"] = true;
        if (Bool(a, "legacy")) retry["legacy"] = true;
        if (dry)
        {
            const bool checked = preflight.contains("checks") || preflight.contains("imports");
            o.summary = "Dry run: map_recover would recover " + preflight.value("source", source) + " into " + preflight.value("destination", destination) + " (merging " +
                        OnOff(merge) + ", Xbox-only actors " + (keep ? "kept" : "left out") + ")" +
                        (checked ? "; " + Count(preflight.value("imports", size_t{0}), "imported package") + ", none missing" : "") + ". Nothing was changed.";
            if (!checked) o.warnings.push_back("The editor did not report its checks for this dry run; the recovery checks again when it starts.");
            if (dirty)
            {
                o.warnings.push_back("The open map has unsaved changes and recovery starts with MAP NEW: save it (map_file action:\"save\") or pass discard: true.");
                o.next.push_back(Next("map_file", "Save the open map first.", {{"action", "save"}}));
            }
            o.warnings.push_back("A recovery takes minutes, an hour or more for a big map on a slow PC, and holds the editor until it is done. Progress is reported, "
                                 "every 15 s at least. A client that stops waiting sooner cancels it, so raise the tool timeout before running it (Claude Code: "
                                 "MCP_TOOL_TIMEOUT, in ms; 10800000 for the server's 3 hours).");
            o.extra = {{"preflight", preflight}, {"destination", destination}, {"merge", merge}, {"keepXboxOnly", keep}};
            o.next.push_back(Next("map_recover", "Run the recovery.", retry));
            return Done(c, o, false);
        }
        if (dirty)
            return RecoverError(Bridge::Error(Bridge::Code::Refused, "The open map has unsaved changes.", "unsaved_changes"), c, args, destination);

        // The recovery itself: its stages as progress, percent of the whole.
        // A heartbeat (a stage that runs long without a checkpoint) repeats
        // the stage with the time so far; the server keeps the value rising.
        auto percent = std::make_shared<double>(0);
        auto stage = std::make_shared<std::string>("Recovering " + sourceStem + " into " + destination);
        CallOptions options;
        options.label = label;
        options.timeoutMs = kRecoverMs;
        options.progress = [&c, percent, stage, sourceStem](const Json& m)
        {
            if (m.contains("percent") && m.at("percent").is_number())
            {
                *percent = std::clamp(m.at("percent").get<double>(), 0.0, 100.0);
                *stage = "Recovering " + sourceStem + ": " + m.value("stage", std::string("working")) + " (" + std::to_string(static_cast<int>(*percent)) + "%)";
                c.progress(*percent, 100, *stage);
            }
            else if (m.value("heartbeat", false) && m.value("stage", std::string()) != "awaiting_approval") c.progress(*percent, 100, *stage + ", " + RunTime(m.value("seconds", 0u)) + " in");
            else c.progress(*percent, 100, ProgressText("map.recover", m));
        };
        c.progress(0, 100, *stage);
        Json result;
        try { result = c.bridge.Call("map.recover", args, options); }
        catch (const Bridge::Error& e) { return RecoverError(e, c, args, destination); }
        if (!result.is_object()) result = Json::object();

        const auto file = result.value("file", destination);
        auto summary = result.value("summary", std::string());
        summary.erase(std::remove(summary.begin(), summary.end(), '\r'), summary.end());
        const Json rows = result.value("rows", Json::array());
        const auto total = result.value("rowsTotal", rows.size());
        const Json counted = result.contains("categories") && result.at("categories").is_object() ? result.at("categories") : Recovery::Counts(rows);
        const auto tally = Recovery::Tally(counted);
        const auto counts = result.value("counts", Json::object());
        const auto newStem = Recovery::Stem(file);
        o.summary = "Recovered " + result.value("source", source) + " into " + file;
        if (result.contains("seconds")) o.summary += " in " + result.at("seconds").dump() + " s";
        o.summary += " (merging " + OnOff(result.value("merge", merge)) + ", Xbox-only actors " + (result.value("keepXboxOnly", keep) ? "kept" : "left out") + ")";
        if (counts.contains("brushes"))
            o.summary += ": " + Count(counts.value("brushes", size_t{0}), "structural brush", "structural brushes") + " from " + Count(counts.value("cells", size_t{0}), "BSP cell") + ", " +
                         Count(counts.value("actors", size_t{0}), "actor");
        o.summary += total ? ". The Recovery Report lists " + Count(total, "item") + ": " + Recovery::TallyLine(tally) + "." : ". The Recovery Report lists nothing: everything came back cleanly.";
        if (rows.size() < total)
            o.summary += (rows.empty() ? std::string(" ") : " The first " + std::to_string(rows.size()) + " are below; ") + "map_query what:\"recovery\" pages through all of them.";
        o.created.push_back(file);
        // The playable copy (Packages\Maps) and the asset package the editor wrote beside it.
        if (result.contains("playable") && result.at("playable").is_string()) o.created.push_back(result.at("playable").get<std::string>());
        if (result.contains("assetPackage") && result.at("assetPackage").is_string()) o.created.push_back(result.at("assetPackage").get<std::string>());
        o.changed.push_back("the open map: now " + file);
        if (result.contains("assetPackage") || counted.contains("Asset moved"))
            o.warnings.push_back("Embedded assets moved into Packages\\StaticMeshes\\" + Recovery::AssetPackage(newStem) + ".usx: ship it with the map.");
        o.warnings.push_back("The recovered map keeps the compiled map's baked lighting. Ordinary builds (map_build, Build All) keep it too, so new or moved lights do not show until a "
                             "re-bake; Build > Recalculate Lighting re-bakes the whole map and changes the recovered look. For listed surfaces and meshes, the user selects them "
                             "and runs Build > Recalculate Selected Lighting.");
        o.extra = {{"file", file}, {"source", result.value("source", source)}, {"merge", result.value("merge", merge)}, {"keepXboxOnly", result.value("keepXboxOnly", keep)},
                   {"seconds", result.value("seconds", Json())}, {"recoverySummary", summary}, {"counts", counts}, {"categories", tally}, {"rows", rows},
                   {"rowsTotal", total}, {"report", result.value("report", PathText(InPackages(c, Recovery::ReportFile(newStem))))},
                   {"folder", result.value("folder", Json())},
                   {"limits", Json::array({"Brushes are rebuilt from the compiled BSP's convex cells (RecoveredVolume<n>): the original brushes, their grouping and history are gone.",
                                                "Xbox-only actors are left out unless keepXboxOnly; references to them are cleared (Reference cleared rows).",
                                                "Lighting is the compiled bake carried over; rows say where it could not be."})}};
        if (total) o.next.push_back(Next("map_query", "Every row of the Recovery Report, by category.", {{"what", "recovery"}}));
        Json frame = Json::array();
        for (const auto& row : rows)
            if (row.contains("actor") && frame.size() < 8) frame.push_back(row.at("actor"));
        if (!frame.empty()) o.next.push_back(Next("view_screenshot", "Look at the actors the report names.", {{"frame", frame}}));
        o.next.push_back(Next("map_validate", "Check the recovered map against the wiki rules.", {{"profile", "draft"}}));
        o.next.push_back(Next("map_query", "See what the recovered map holds (spawns, missions, cameras).", {{"what", "summary"}}));
        return Done(c, o, false);
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
            const auto given = RequireString(a, "path");
            const auto path = MapOpenPath(given);
            if (CompiledMapPath(c, path)) return CompiledMapRefusal(c, path);
            // A bare name only Packages\Maps has: the compiled map, which the
            // editor would not find in Packages\MapsEd.
            if (path != given)
            {
                std::error_code error;
                const auto compiled = GamePackages(c) / "Maps" / path;
                if (!std::filesystem::exists(GamePackages(c) / "MapsEd" / path, error) && std::filesystem::is_regular_file(compiled, error))
                    return CompiledMapRefusal(c, PathText(compiled));
            }
            if (const auto why = unsaved("open another map"); !why.empty()) return Fail(why, {{"state", state}});
            Json result;
            try { result = Call(c, "map.open", {{"path", path}, {"discard", discard}}, label, dry, 600000); }
            catch (const Bridge::Error& e)
            {
                if (e.reason == "compiled_map")
                {
                    const bool resolved = e.details.is_object() && e.details.contains("path") && e.details.at("path").is_string();
                    return CompiledMapRefusal(c, resolved ? e.details.at("path").get<std::string>() : path, e.details);
                }
                throw;
            }
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
            Json result;
            try { result = Call(c, "map.save", args, label, dry, 900000); }
            catch (const Bridge::Error& e)
            {
                // Saving under a compiled-only map's name would replace that
                // original in Packages\Maps (every save writes the playable copy).
                if (e.reason != "compiled_original") throw;
                const auto stem = Recovery::Stem(args.value("path", state.value("mapFile", std::string())));
                return Fail(std::string(e.what()) + " Save under another name, such as \"" + stem + "_Edit\".",
                            {{"code", e.code}, {"reason", e.reason}, {"retryable", false}, {"details", e.details},
                             {"next", Json::array({Next("map_file", "Save under a name no compiled map has.", {{"action", "save_as"}, {"path", stem + "_Edit.sdc"}})})}});
            }
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
            "Starts Reloaded_Editor.exe from the game's System folder with -reMcp and waits (default 180 s) until the new editor's MCP pipe answers; the result's "
            "pid is the editor's. Needs [Mcp] Enabled=1 in System\\Reloaded_Editor.ini, which only the user sets (RE+ Tools > MCP > Enable MCP server); refused "
            "otherwise unless force. Does nothing when an editor is already running (editor_status). map opens a .sdc once started. A startup window (update offer, "
            "crash recovery) makes the wait time out: ask the user to answer it. Windows only.";
        t.inputSchema = S::Mutating(S::Input({{"waitSeconds", S::Integer("Seconds to wait for the pipe. Default 180.", 10, 600)},
                                              {"map", S::String("A map to open once started (\"AquaD\" or a .sdc path).")},
                                              {"force", S::Boolean("Start even when the INI says the server is off or an editor runs.")}}));
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
        t.inputSchema = S::Mutating(S::Input({{"save", S::Boolean("Save unsaved changes first. Default true; false discards them.")},
                                              {"waitSeconds", S::Integer("Seconds to wait for the new editor. Default 180.", 10, 600)}}));
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
            "Undoes changes. Every MCP change is one Undo step labelled \"MCP <n>: <label>\" in Undo History. With no arguments it undoes this server's newest "
            "call that is still undoable (called again, the one before it), by call number, so it is refused rather than undo something newer the user did, "
            "and does nothing when this server has made no change (in this editor process); a call the editor can no longer undo whole is forgotten, so the "
            "next one goes on to the call before it. call undoes a given MCP call (undo.call in each mutating tool's result) while its steps are the newest; "
            "steps undoes that many Undo steps whatever made them; redo with steps redoes. Returns the editor state before and after.";
        t.inputSchema = S::Mutating(S::Input({{"call", S::Integer("An MCP call (undo.call in a tool result) to undo as a whole.", 1, 0xffffffffLL)},
                                              {"steps", S::Integer("Undo (with redo: redo) this many steps.", 1, 100)},
                                              {"redo", S::Boolean("Redo instead of undo.")}}));
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
        t.inputSchema = S::Input({{"ticket", S::String("The awaiting_approval error's ticket.")},
                                  {"timeoutSeconds", S::Integer("How long to wait this time. Default 50.", 1, 600)}},
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
        t.inputSchema = S::Input({{"since", S::Integer("Only lines after this sequence number (a result's last).", 0, 0x7fffffffffffffffLL)},
                                  {"newOnly", S::Boolean("Continue after the previous editor_log.")},
                                  {"max", S::Integer("At most this many lines, the newest. Default 100.", 1, 2000)},
                                  {"level", S::Enum({"all", "warning", "error"}, "Minimum level. Default all.")},
                                  {"contains", S::String("Only lines containing this text (any case).")}});
        t.readOnly = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return EditorLog(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "map_file";
        t.title = "New, open or save a map";
        t.description =
            "Map files. action new starts an empty map; open loads a .sdc (a bare name such as \"AquaD\" is looked up in Packages\\MapsEd); save writes the current "
            "file; save_as names it: Packages\\MapsEd, ASCII without spaces, by the wiki (map-packaging) the first four letters of the map name plus D (Aquarius -> "
            "AquaD), which save_as makes from mapName. new and open refuse to drop unsaved changes unless discardUnsaved; save_as refuses to replace another map's "
            "file unless overwrite. After the first save of a new map, editor_restart comes before Map Settings. A compiled map (Packages\\Maps) is refused: "
            "use map_recover. Files are not undoable.";
        t.inputSchema = S::Mutating(S::Input(
            {{"action", S::Enum({"new", "open", "save", "save_as"}, "What to do.")},
             {"path", S::String("open: the map. save_as: the file name (\"AquaD\") or a full path in Packages\\MapsEd.")},
             {"mapName", S::String("save_as without path: the map's display name (CAPS), which names the file.")},
             {"discardUnsaved", S::Boolean("new, open: drop the open map's unsaved changes.")},
             {"overwrite", S::Boolean("save_as: replace another map's file.")}},
            {"action"}));
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return MapFile(a, c); }); };
        registry.Add(std::move(t));
    }
    {
        Tool t;
        t.name = "map_recover";
        t.title = "Recover a compiled map";
        t.description =
            "File > Recover Compiled Map: turns a compiled Versus .sdc (source: \"OffsD\" is Packages\\Maps\\OffsD.sdc, or a path) into an editable map in "
            "Packages\\MapsEd (name, default <source>_Recovered), replacing the open map (discard drops unsaved changes). Missing packages, a taken name or a "
            "running recovery are refused first; dryRun only checks. Takes minutes to an hour, with progress; cancelling works until it saves. Returns counts, "
            "the Recovery Report's categories (meaning, what to do) and rows (maxRows; map_query what:\"recovery\" has all). Not undoable.";
        t.inputSchema = S::Mutating(S::Input({{"source", S::String("The compiled map: a name in Packages\\Maps, or a path.")},
                                              {"name", S::String("The new file in Packages\\MapsEd. Default <source>_Recovered.")},
                                              {"merge", S::Boolean("Merge BSP fragments into fewer brushes. Default: File > Merge Recovered Geometry.")},
                                              {"keepXboxOnly", S::Boolean("Keep Xbox-only actors.")},
                                              {"legacy", S::Boolean("The source is a legacy recovered map in Packages\\MapsEd.")},
                                              {"discard", S::Boolean("Drop unsaved changes.")},
                                              {"maxRows", S::Integer("Rows to return. Default 200.", 0, 2000)}},
                                             {"source"}));
        t.outputSchema = S::MutationOutput();
        t.destructive = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return MapRecover(a, c); }); };
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
        t.inputSchema = S::Mutating(S::Input({{"what", S::Enum({"geometry", "all"}, "Default geometry.")},
                                              {"check", S::Boolean("Run Map Check afterwards. Default true.")}}));
        t.outputSchema = S::MutationOutput();
        t.idempotent = true;
        t.run = [](const Json& a, ToolContext& c) { return Guarded([&] { return MapBuild(a, c); }); };
        registry.Add(std::move(t));
    }
}
}
