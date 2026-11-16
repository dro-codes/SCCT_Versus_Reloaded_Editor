#pragma once
// What Reloaded.Editor.Mcp.exe publishes over MCP (tools, resources, prompts)
// and the editor connection the tools use. Portable C++: everything here
// builds and is tested without Windows; only BridgeClient.cpp (the pipe) and
// Media.cpp (Media Foundation, WIC) need it.
#include "../Reloaded.Editor/McpBridgeModel.h"
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Mcp
{
    using Json = Workflow::Json;

    // One bridge call's options. expect makes the editor refuse the call as
    // stale when the map moved on since the caller read it.
    struct CallOptions
    {
        std::string label;
        bool dryRun = false;
        Bridge::Expect expect;
        unsigned timeoutMs = 120000;
        std::function<void(const Json& progress)> progress;
    };
    // The editor, through the pipe (BridgeClient) or a recording (FakeBridge).
    // Call returns the op's result and throws Bridge::Error carrying the
    // bridge's code (busy is retried inside Call until timeoutMs).
    class IBridge
    {
    public:
        virtual ~IBridge() = default;
        virtual Json Call(const std::string& op, const Json& args, const CallOptions& options = {}) = 0;
        // The editor's hello reply; connects first if needed. Throws
        // Bridge::Error(not_connected) when no editor is listening.
        virtual Json Hello() = 0;
        virtual bool Connected() = 0;
        // The state block of the last reply: level, generation, revision,
        // mapKey, dirty, canUndo, canRedo, ...
        virtual Json State() = 0;
        // Editors with a bridge pipe: [{pid}]; selects one for later calls.
        virtual Json Editors() { return Json::array(); }
        virtual void Select(unsigned long pid) { (void)pid; }
        virtual void Disconnect() {}
        // A blob the editor wrote (authoring.export), read back in chunks.
        std::string ReadBlob(const std::string& blob)
        {
            std::string data;
            for (size_t offset = 0;;)
            {
                auto chunk = Call("blob.read", {{"blob", blob}, {"offset", offset}, {"size", Bridge::kBlobChunk}});
                const auto bytes = Bridge::Unbase64(chunk.value("data", std::string()));
                data += bytes;
                offset += bytes.size();
                if (chunk.value("eof", true) || bytes.empty()) break;
                if (data.size() > Bridge::kMaxDocument * 2) throw std::runtime_error("Blob is larger than the bridge allows.");
            }
            try { Call("blob.free", {{"blob", blob}}); } catch (const std::exception&) {}
            return data;
        }
    };

    // A tool result: structuredContent plus content blocks (text, image,
    // embedded resource). isError marks a failure the agent should read.
    struct Content
    {
        std::string type = "text"; // text | image | resource
        std::string text;          // text
        std::string data;          // image: base64
        std::string mimeType;      // image/png
        Json resource;             // resource: {uri, mimeType, text}
    };
    struct ToolResult
    {
        Json structured = Json::object();
        std::vector<Content> content;
        bool isError = false;
    };
    inline Content TextContent(const std::string& text) { Content c; c.text = text; return c; }
    inline Content ImageContent(const std::string& base64, const std::string& mime = "image/png") { Content c; c.type = "image"; c.data = base64; c.mimeType = mime; return c; }
    // A result whose text block is the summary (or the JSON) of structured.
    inline ToolResult Ok(const Json& structured, const std::string& summary = {})
    {
        ToolResult r;
        r.structured = structured.is_object() ? structured : Json{{"result", structured}};
        r.content.push_back(TextContent(summary.empty() ? r.structured.dump(2) : summary + "\n\n" + r.structured.dump(2)));
        return r;
    }
    inline ToolResult Fail(const std::string& message, const Json& details = Json::object())
    {
        ToolResult r;
        r.isError = true;
        r.structured = details.is_object() ? details : Json{{"details", details}};
        r.structured["error"] = message;
        r.content.push_back(TextContent(message + (details.empty() ? std::string() : "\n\n" + details.dump(2))));
        return r;
    }

    // What a running tool can reach.
    struct ToolContext
    {
        IBridge& bridge;
        std::filesystem::path systemDir; // the game's System folder (where the exe is)
        std::filesystem::path workDir;   // %ProgramData%\REPlusMcp or [Mcp] WorkDir
        std::function<void(double done, double total, const std::string& message)> progress = [](double, double, const std::string&) {};
    };
    struct Tool
    {
        std::string name, title, description;
        Json inputSchema = Json{{"type", "object"}, {"properties", Json::object()}};
        Json outputSchema; // null: none published
        bool readOnly = false, destructive = false, idempotent = false, openWorld = false;
        std::function<ToolResult(const Json& arguments, ToolContext& context)> run;
    };
    // A resource is static (uri) or a template (uri with {name} parts, matched
    // by MatchTemplate). read returns {text} or {blob (base64)} and mimeType.
    struct Resource
    {
        std::string uri, name, title, description, mimeType = "text/markdown";
        bool isTemplate = false, live = false;
        std::function<Json(const std::string& uri, ToolContext& context)> read;
    };
    struct PromptArgument { std::string name, description; bool required = false; };
    struct Prompt
    {
        std::string name, title, description;
        std::vector<PromptArgument> arguments;
        // MCP prompt messages: [{role, content:{type:"text", text}}].
        std::function<Json(const Json& arguments, ToolContext& context)> messages;
    };
    class Registry
    {
    public:
        void Add(Tool tool) { tools.push_back(std::move(tool)); }
        void Add(Resource resource) { resources.push_back(std::move(resource)); }
        void Add(Prompt prompt) { prompts.push_back(std::move(prompt)); }
        const Tool* FindTool(const std::string& name) const
        {
            for (const auto& t : tools) if (t.name == name) return &t;
            return nullptr;
        }
        const Prompt* FindPrompt(const std::string& name) const
        {
            for (const auto& p : prompts) if (p.name == name) return &p;
            return nullptr;
        }
        std::vector<Tool> tools;
        std::vector<Resource> resources;
        std::vector<Prompt> prompts;
    };
    // {name} parts of a template uri matched against a uri; false if it does not fit.
    inline bool MatchTemplate(const std::string& pattern, const std::string& uri, std::map<std::string, std::string>& parts)
    {
        size_t p = 0, u = 0;
        while (p < pattern.size())
        {
            if (pattern[p] == '{')
            {
                const auto close = pattern.find('}', p);
                if (close == std::string::npos) return false;
                const auto name = pattern.substr(p + 1, close - p - 1);
                const auto next = close + 1 < pattern.size() ? pattern[close + 1] : '\0';
                auto end = next ? uri.find(next, u) : uri.size();
                if (end == std::string::npos || end == u) return false;
                parts[name] = uri.substr(u, end - u);
                u = end;
                p = close + 1;
            }
            else
            {
                if (u >= uri.size() || uri[u] != pattern[p]) return false;
                ++u; ++p;
            }
        }
        return u == uri.size();
    }

    // Each Tools/*.cpp contributes its part; Register calls them all.
    void RegisterSessionTools(Registry&);  // editor_status, editor_launch, editor_restart, editor_undo, approval_wait, editor_log, map_file, map_build
    void RegisterQueryTools(Registry&);    // map_query, view_screenshot, plan_render
    void RegisterValidateTools(Registry&); // map_validate
    void RegisterSpecTools(Registry&);     // level_spec_validate, level_build
    void RegisterIntentTools(Registry&);   // blockout, gameplay, security, mechanisms, events, environment, geometric_events, map_settings
    void RegisterEscapeTools(Registry&);   // actors_edit, map_changes_apply, workflow_request, editor_exec
    void RegisterImportTools(Registry&);   // import_to_spec, assets_import
    void RegisterMediaTools(Registry&);    // media_extract_frames, media_read_image, image_convert
    void RegisterReleaseTools(Registry&);  // release
    void RegisterResources(Registry&);     // scct:// resources
    void RegisterPrompts(Registry&);
    inline void Register(Registry& registry)
    {
        RegisterSessionTools(registry);
        RegisterQueryTools(registry);
        RegisterValidateTools(registry);
        RegisterSpecTools(registry);
        RegisterIntentTools(registry);
        RegisterEscapeTools(registry);
        RegisterImportTools(registry);
        RegisterMediaTools(registry);
        RegisterReleaseTools(registry);
        RegisterResources(registry);
        RegisterPrompts(registry);
    }
}
