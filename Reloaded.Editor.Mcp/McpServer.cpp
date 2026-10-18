#include "McpServer.h"
#include "Tools/Common.h"
#include <chrono>
#include <cmath>
#include <istream>

namespace Mcp
{
namespace E = Protocol::ErrorCode;

std::string DefaultInstructions()
{
    return "RE+ MCP builds and edits Splinter Cell Chaos Theory Versus maps in the running RE+ editor (Reloaded_Editor.exe).\n"
           "1. Start with editor_status: it connects to the editor and shows the open map, unsaved changes and whether the user approves each change. "
           "editor_launch starts the editor when none is running (the user must have turned on RE+ Tools > MCP > Enable MCP server).\n"
           "2. Before designing, read scct://wiki/index and scct://rules. The wiki digests (scct://wiki/<page>) are the source of truth for every "
           "gameplay rule (spawns, game modes, CamNet, alarms, geometric events, map settings, packaging); validation cites them by page.\n"
           "3. Write the level as a scct.level-spec (scct://schema/level-spec, examples at scct://examples/*). Check it offline with level_spec_validate "
           "and plan_render, then level_build with mode preview, then mode apply. The intent tools (blockout, gameplay, security, mechanisms, events, "
           "environment, geometric_events, map_settings) make focused changes the same way.\n"
           "4. map_build, then map_validate (profile draft while iterating, release before sharing), and view_screenshot to compare with the source.\n"
           "5. map_file save_as names and saves the map in Packages\\MapsEd: the wiki names the file after the first four letters of the map name plus D "
           "(AquaD). A new map then needs editor_restart before its Map Settings can be edited.\n"
           "Every change is one Undo step (editor_undo), takes dryRun to preview and label to name it, and returns summary, created, changed, warnings, "
           "undo and next steps. map_query reads anything; actors_edit, map_changes_apply, workflow_request and editor_exec are escape hatches. "
           "Units are Unreal units (about 100 per metre, 180 a standing character), Z up, +X east, +Y north; yaw is degrees in specs and 65536 per "
           "turn in engine values.";
}

Server::Server(Registry& r, IBridge& b, ServerOptions o, Writer w) : registry(r), bridge(b), options(std::move(o)), write(std::move(w))
{
    if (options.instructions.empty()) options.instructions = DefaultInstructions();
    worker = std::thread([this] { Work(); });
}
Server::~Server() { Finish(); }

std::string Server::NegotiatedVersion() const
{
    std::lock_guard<std::mutex> guard(lock);
    return protocol;
}

void Server::Send(const Json& message)
{
    const auto line = Protocol::Line(message);
    std::lock_guard<std::mutex> guard(output);
    write(line);
}

void Server::Log(const std::string& level, const std::string& text)
{
    if (options.log) options.log("[" + level + "] " + text);
    bool send = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        send = initialized && Protocol::LogRank(level) >= logRank;
    }
    if (send) Send(Protocol::Notification("notifications/message", {{"level", level}, {"logger", "scct-reloaded"}, {"data", text}}));
}

void Server::Handle(const std::string& raw)
{
    auto line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
    if (line.empty()) return;
    Json message;
    try { message = Protocol::ParseLine(line); }
    catch (const std::exception& e)
    {
        Send(Protocol::Error(nullptr, E::Parse, std::string("Parse error: ") + e.what()));
        return;
    }
    // Batches (2025-03-26) are answered message by message.
    if (message.is_array())
    {
        if (message.empty()) Send(Protocol::Error(nullptr, E::InvalidRequest, "An empty batch."));
        for (const auto& item : message) Dispatch(item);
        return;
    }
    Dispatch(message);
}

void Server::Dispatch(const Json& message)
{
    const auto m = Protocol::Classify(message);
    switch (m.kind)
    {
    case Protocol::Message::Kind::Request: Request(m); return;
    case Protocol::Message::Kind::Notification: Notify(m); return;
    case Protocol::Message::Kind::Response: return; // this server sends no requests
    case Protocol::Message::Kind::Invalid: Send(Protocol::Error(m.id, E::InvalidRequest, m.problem)); return;
    }
}

const Resource* Server::FindResource(const std::string& uri) const
{
    for (const auto& r : registry.resources) if (!r.isTemplate && r.uri == uri) return &r;
    for (const auto& r : registry.resources)
    {
        std::map<std::string, std::string> parts;
        if (r.isTemplate && MatchTemplate(r.uri, uri, parts)) return &r;
    }
    return nullptr;
}

void Server::Request(const Protocol::Message& m)
{
    const auto& method = m.method;
    const auto& params = m.params;
    auto reply = [&](const Json& result) { Send(Protocol::Response(m.id, result)); };
    auto invalid = [&](const std::string& why) { Send(Protocol::Error(m.id, E::InvalidParams, why)); };
    try
    {
        if (method == "initialize")
        {
            const auto version = Protocol::Negotiate(params.value("protocolVersion", Json()));
            const auto name = params.contains("clientInfo") && params.at("clientInfo").is_object() ? params.at("clientInfo").value("name", std::string()) : std::string();
            {
                std::lock_guard<std::mutex> guard(lock);
                protocol = version;
                client = name;
            }
            reply(Protocol::InitializeResult(version, options.version, options.instructions));
            if (options.log) options.log("[info] initialize from " + (name.empty() ? std::string("a client") : name) + ", protocol " + version);
            return;
        }
        if (method == "ping") { reply(Json::object()); return; }
        const auto version = NegotiatedVersion();
        if (method == "tools/list")
        {
            Json tools = Json::array();
            for (const auto& t : registry.tools) tools.push_back(Protocol::ToolJson(t, version));
            reply({{"tools", tools}});
            return;
        }
        if (method == "resources/list" || method == "resources/templates/list")
        {
            const bool templates = method == "resources/templates/list";
            Json list = Json::array();
            for (const auto& r : registry.resources) if (r.isTemplate == templates) list.push_back(Protocol::ResourceJson(r, version));
            reply({{templates ? "resourceTemplates" : "resources", list}});
            return;
        }
        if (method == "prompts/list")
        {
            Json list = Json::array();
            for (const auto& p : registry.prompts) list.push_back(Protocol::PromptJson(p, version));
            reply({{"prompts", list}});
            return;
        }
        if (method == "logging/setLevel")
        {
            const auto level = params.value("level", std::string());
            const int rank = Protocol::LogRank(level);
            if (rank < 0) { invalid("level is one of debug, info, notice, warning, error, critical, alert, emergency."); return; }
            {
                std::lock_guard<std::mutex> guard(lock);
                logRank = rank;
            }
            reply(Json::object());
            return;
        }
        if (method == "resources/subscribe" || method == "resources/unsubscribe")
        {
            if (!params.contains("uri") || !params.at("uri").is_string()) { invalid("uri is required."); return; }
            const auto uri = params.at("uri").get<std::string>();
            if (!FindResource(uri)) { Send(Protocol::Error(m.id, E::ResourceNotFound, "Resource not found: " + uri, {{"uri", uri}})); return; }
            {
                std::lock_guard<std::mutex> guard(lock);
                if (method == "resources/subscribe") subscriptions.insert(uri);
                else subscriptions.erase(uri);
            }
            reply(Json::object());
            return;
        }
        if (method == "tools/call")
        {
            if (!params.contains("name") || !params.at("name").is_string()) { invalid("name is required."); return; }
            if (!registry.FindTool(params.at("name").get<std::string>())) { invalid("Unknown tool: " + params.at("name").get<std::string>()); return; }
            if (params.contains("arguments") && !params.at("arguments").is_object() && !params.at("arguments").is_null()) { invalid("arguments must be an object."); return; }
            Enqueue(m);
            return;
        }
        if (method == "resources/read")
        {
            if (!params.contains("uri") || !params.at("uri").is_string()) { invalid("uri is required."); return; }
            const auto uri = params.at("uri").get<std::string>();
            const auto* resource = FindResource(uri);
            if (!resource) { Send(Protocol::Error(m.id, E::ResourceNotFound, "Resource not found: " + uri, {{"uri", uri}})); return; }
            if (resource->live) { Enqueue(m); return; }
            ToolContext context{bridge, options.systemDir, options.workDir};
            Send(ReadResource(m.id, uri, context));
            return;
        }
        if (method == "prompts/get")
        {
            if (!params.contains("name") || !params.at("name").is_string()) { invalid("name is required."); return; }
            const auto* prompt = registry.FindPrompt(params.at("name").get<std::string>());
            if (!prompt) { invalid("Unknown prompt: " + params.at("name").get<std::string>()); return; }
            const auto arguments = params.value("arguments", Json::object());
            for (const auto& a : prompt->arguments)
                if (a.required && (!arguments.is_object() || !arguments.contains(a.name))) { invalid("The prompt needs the argument \"" + a.name + "\"."); return; }
            Enqueue(m);
            return;
        }
        Send(Protocol::Error(m.id, E::MethodNotFound, "Method not found: " + method));
    }
    catch (const std::exception& e) { Send(Protocol::Error(m.id, E::InvalidParams, e.what())); }
}

void Server::Notify(const Protocol::Message& m)
{
    if (m.method == "notifications/initialized")
    {
        std::lock_guard<std::mutex> guard(lock);
        initialized = true;
        return;
    }
    if (m.method == "notifications/cancelled")
    {
        if (!m.params.contains("requestId")) return;
        const auto key = m.params.at("requestId").dump();
        bool interrupt = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            const auto queued = std::find_if(queue.begin(), queue.end(), [&](const Job& job) { return job.key == key; });
            if (queued != queue.end()) queue.erase(queued);
            else if (busy && running == key) { cancelled.insert(key); interrupt = true; }
        }
        if (interrupt)
        {
            if (options.cancel) options.cancel();
            if (options.log) options.log("[info] request " + key + " cancelled: " + m.params.value("reason", std::string("no reason given")));
        }
        idle.notify_all();
    }
    // Others (roots/list_changed, progress from the client) need nothing.
}

void Server::Enqueue(const Protocol::Message& m)
{
    {
        std::lock_guard<std::mutex> guard(lock);
        queue.push_back({m.id, m.id.dump(), m.method, m.params});
    }
    wake.notify_one();
}

void Server::Work()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> guard(lock);
            wake.wait(guard, [&] { return stopping || !queue.empty(); });
            if (queue.empty()) return;
            job = std::move(queue.front());
            queue.pop_front();
            running = job.key;
            busy = true;
        }
        if (options.resume) options.resume();
        Json reply;
        try { reply = Execute(job); }
        catch (const std::exception& e) { reply = Protocol::Error(job.id, E::Internal, e.what()); }
        catch (...) { reply = Protocol::Error(job.id, E::Internal, "The request failed with an unknown error."); }
        bool drop = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            drop = cancelled.erase(job.key) > 0;
            running.clear();
            busy = false;
        }
        // A cancelled request gets no reply (the MCP cancellation rule).
        if (!drop) Send(reply);
        idle.notify_all();
    }
}

std::function<void(double, double, const std::string&)> Server::Progress(const Json& params)
{
    if (!params.contains("_meta") || !params.at("_meta").is_object() || !params.at("_meta").contains("progressToken")) return [](double, double, const std::string&) {};
    const auto token = params.at("_meta").at("progressToken");
    if (!token.is_string() && !token.is_number_integer() && !token.is_number_unsigned()) return [](double, double, const std::string&) {};
    auto last = std::make_shared<double>(-1);
    const auto version = NegotiatedVersion();
    return [this, token, last, version](double done, double total, const std::string& message)
    {
        if (!std::isfinite(done)) return;
        // Progress must rise with every message, whatever scale a tool uses.
        double value = done > *last ? done : *last + 0.001;
        *last = value;
        Send(Protocol::Notification("notifications/progress", Protocol::ProgressParams(token, value, total >= value ? total : 0, message, version)));
    };
}

Json Server::Execute(const Job& job)
{
    ToolContext context{bridge, options.systemDir, options.workDir};
    context.progress = Progress(job.params);
    if (job.method == "tools/call") return CallTool(job, context);
    if (job.method == "resources/read") return ReadResource(job.id, job.params.value("uri", std::string()), context);
    if (job.method == "prompts/get") return GetPrompt(job, context);
    return Protocol::Error(job.id, E::MethodNotFound, "Method not found: " + job.method);
}

Json Server::CallTool(const Job& job, ToolContext& context)
{
    const auto name = job.params.value("name", std::string());
    const auto* tool = registry.FindTool(name);
    if (!tool) return Protocol::Error(job.id, E::InvalidParams, "Unknown tool: " + name);
    Json arguments = job.params.contains("arguments") && job.params.at("arguments").is_object() ? job.params.at("arguments") : Json::object();
    ToolResult result;
    const auto problem = Protocol::CheckArguments(tool->inputSchema, arguments);
    if (!problem.empty()) result = Fail("Invalid arguments for " + name + ": " + problem + ". The tool's inputSchema lists what it takes.");
    else
    {
        const auto started = std::chrono::steady_clock::now();
        try { result = tool->run(arguments, context); }
        catch (const std::exception& e) { result = Tools::ErrorResult(e); }
        catch (...) { result = Fail(name + " failed with an unknown error."); }
        if (options.log)
            options.log("[info] " + name + (result.isError ? " failed" : " done") + " in " +
                        std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()) + " ms");
    }
    const bool dryRun = arguments.contains("dryRun") && arguments.at("dryRun").is_boolean() && arguments.at("dryRun").get<bool>();
    if (!tool->readOnly && !result.isError && !dryRun) MapChanged();
    return Protocol::Response(job.id, Protocol::ToolResultJson(result, NegotiatedVersion()));
}

Json Server::ReadResource(const Json& id, const std::string& uri, ToolContext& context)
{
    const auto* resource = FindResource(uri);
    if (!resource || !resource->read) return Protocol::Error(id, E::ResourceNotFound, "Resource not found: " + uri, {{"uri", uri}});
    Json read;
    try { read = resource->read(uri, context); }
    catch (const Bridge::Error& e)
    {
        const auto hint = Tools::Hint(e);
        return Protocol::Error(id, E::Internal, "Could not read " + uri + ": " + e.what() + (hint.empty() ? "" : " " + hint), {{"uri", uri}, {"code", e.code}});
    }
    catch (const std::exception& e) { return Protocol::Error(id, E::Internal, "Could not read " + uri + ": " + e.what(), {{"uri", uri}}); }
    if (read.is_null()) return Protocol::Error(id, E::ResourceNotFound, "Resource not found: " + uri, {{"uri", uri}});
    return Protocol::Response(id, Protocol::ReadContents(uri, read, resource->mimeType));
}

Json Server::GetPrompt(const Job& job, ToolContext& context)
{
    const auto* prompt = registry.FindPrompt(job.params.value("name", std::string()));
    if (!prompt) return Protocol::Error(job.id, E::InvalidParams, "Unknown prompt.");
    const auto arguments = job.params.contains("arguments") && job.params.at("arguments").is_object() ? job.params.at("arguments") : Json::object();
    try
    {
        Json result{{"messages", prompt->messages ? prompt->messages(arguments, context) : Json::array()}};
        if (!prompt->description.empty()) result["description"] = prompt->description;
        return Protocol::Response(job.id, result);
    }
    catch (const std::exception& e) { return Protocol::Error(job.id, E::Internal, std::string("Could not build the prompt: ") + e.what()); }
}

void Server::MapChanged()
{
    std::vector<std::string> uris;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (!initialized) return;
        uris.assign(subscriptions.begin(), subscriptions.end());
    }
    for (const auto& uri : uris)
        if (const auto* r = FindResource(uri); r && r->live) Send(Protocol::Notification("notifications/resources/updated", {{"uri", uri}}));
}

void Server::OnBridgeEvent(const Json& event)
{
    const auto kind = event.is_object() ? event.value("kind", std::string()) : std::string();
    if (kind == "map_changed" || kind == "map_opened" || kind == "map_saved" || kind == "undo") MapChanged();
    if (options.log && !kind.empty()) options.log("[info] editor event " + kind);
}

int Server::Run(std::istream& input)
{
    std::string line;
    while (std::getline(input, line)) Handle(line);
    Finish(5000, 3000);
    return 0;
}

void Server::Wait()
{
    std::unique_lock<std::mutex> guard(lock);
    idle.wait(guard, [&] { return queue.empty() && !busy; });
}

bool Server::Finish(unsigned waitMs, unsigned drainMs)
{
    // The input has ended: what was asked before it may still be answered.
    if (drainMs)
    {
        std::unique_lock<std::mutex> guard(lock);
        idle.wait_for(guard, std::chrono::milliseconds(drainMs), [&] { return finished || (queue.empty() && !busy); });
    }
    bool interrupt = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        if (finished) return true;
        finished = stopping = true;
        queue.clear();
        if (busy) { cancelled.insert(running); interrupt = true; }
    }
    if (interrupt && options.cancel) options.cancel();
    wake.notify_all();
    bool stopped = false;
    {
        std::unique_lock<std::mutex> guard(lock);
        stopped = idle.wait_for(guard, std::chrono::milliseconds(waitMs), [&] { return !busy; });
    }
    if (worker.joinable())
    {
        if (stopped) worker.join();
        else worker.detach();
    }
    return stopped;
}
}
