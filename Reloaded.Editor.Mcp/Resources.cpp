// scct:// resources: the embedded wiki digests, schemas and docs
// (Embedded.gen.h), and the live editor and map.
#include "Embedded.h"
#include "Tools/Common.h"
#include <tuple>

namespace Mcp
{
namespace
{
    using namespace Tools;

    // "wiki/game-modes.md" -> "scct://wiki/game-modes"; the schemas folder is
    // served as scct://schema/..., and "map-changes.schema.json" loses both
    // extensions.
    std::string UriOf(const std::string& path)
    {
        auto relative = path;
        if (relative.rfind("schemas/", 0) == 0) relative = "schema/" + relative.substr(8);
        const auto slash = relative.find_last_of('/');
        const auto dot = relative.find('.', slash == std::string::npos ? 0 : slash + 1);
        if (dot != std::string::npos) relative.erase(dot);
        return "scct://" + relative;
    }
    bool IsText(const std::string& mime)
    {
        return mime.rfind("text/", 0) == 0 || mime == "application/json" || mime == "application/schema+json" || mime == "application/xml" ||
               (mime.size() > 5 && mime.compare(mime.size() - 5, 5, "+json") == 0);
    }
    std::string Folder(const std::string& path) { const auto slash = path.find('/'); return slash == std::string::npos ? std::string() : path.substr(0, slash); }
    // The page's first heading, or the schema's title.
    std::string TitleOf(const EmbeddedFile& file)
    {
        const std::string text(reinterpret_cast<const char*>(file.data), std::min<size_t>(file.size, 4096));
        std::istringstream lines(text);
        std::string line;
        for (int n = 0; n < 40 && std::getline(lines, line); ++n)
            if (line.rfind("# ", 0) == 0)
            {
                line = line.substr(2);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                return line;
            }
        if (std::string(file.mime).find("json") != std::string::npos)
        {
            const auto json = Json::parse(std::string(reinterpret_cast<const char*>(file.data), file.size), nullptr, false);
            if (json.is_object() && json.contains("title") && json.at("title").is_string()) return json.at("title").get<std::string>();
        }
        return {};
    }
    std::string DescriptionOf(const std::string& folder, const std::string& name)
    {
        if (folder == "wiki")
            return name == "index" ? "Index of the SCCT Versus wiki digests: start here. Each page holds the class and property names and the rules the tools follow."
                                   : "SCCT Versus wiki digest: " + name + ". Class and property names verbatim from the wiki, which is the source of truth for gameplay rules.";
        if (folder == "schemas") return "JSON Schema for " + name + " documents.";
        if (folder == "docs") return "RE+ documentation: " + name + ".";
        return "Embedded file " + name + ".";
    }
    Json ReadEmbedded(const EmbeddedFile& file)
    {
        const std::string bytes(reinterpret_cast<const char*>(file.data), file.size);
        if (IsText(file.mime)) return {{"text", bytes}, {"mimeType", file.mime}};
        return {{"blob", Bridge::Base64(bytes)}, {"mimeType", file.mime}};
    }
    const EmbeddedFile* FindFile(const std::string& uri)
    {
        for (const auto& file : EmbeddedFiles()) if (UriOf(file.path) == uri) return &file;
        return nullptr;
    }
    Json Text(const Json& value) { return {{"text", value.dump(2, ' ', false, Json::error_handler_t::replace)}, {"mimeType", "application/json"}}; }
}

void RegisterResources(Registry& registry)
{
    for (const auto& file : EmbeddedFiles())
    {
        const std::string path = file.path, folder = Folder(path);
        // Prompt templates are served as prompts (prompts/get), not resources.
        if (folder == "prompts" || path.empty()) continue;
        Resource r;
        r.uri = UriOf(path);
        const auto slash = path.find_last_of('/');
        r.name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        r.name = r.name.substr(0, r.name.find('.'));
        r.title = TitleOf(file);
        r.description = DescriptionOf(folder, r.name);
        r.mimeType = file.mime;
        const auto* entry = &file;
        r.read = [entry](const std::string&, ToolContext&) { return ReadEmbedded(*entry); };
        registry.Add(std::move(r));
    }
    // Templates, for clients that complete names; each reads the same files.
    for (const auto& [folder, uri, mime, what] : std::vector<std::tuple<std::string, std::string, std::string, std::string>>{
             {"wiki", "scct://wiki/{page}", "text/markdown", "A wiki digest by slug (index, spawn-points, game-modes, alarms, geometric-events, map-settings, ...)."},
             {"schema", "scct://schema/{name}", "application/schema+json", "A JSON Schema: map-changes, map-authoring, stages, smagic-event, build-plan."},
             {"docs", "scct://docs/{name}", "text/markdown", "RE+ documentation, such as map-authoring and map-design."}})
    {
        Resource r;
        r.uri = uri;
        r.name = folder;
        r.title = folder == "wiki" ? "Wiki page" : folder == "schema" ? "Schema" : "Documentation";
        r.description = what;
        r.mimeType = mime;
        r.isTemplate = true;
        r.read = [](const std::string& u, ToolContext&) -> Json
        {
            const auto* file = FindFile(u);
            return file ? ReadEmbedded(*file) : Json();
        };
        registry.Add(std::move(r));
    }

    // Live: read from the editor each time; subscribers hear when the map changes.
    auto live = [&registry](std::string uri, std::string name, std::string title, std::string description, std::string mime, std::function<Json(const std::string&, ToolContext&)> read, bool isTemplate = false)
    {
        Resource r;
        r.uri = std::move(uri);
        r.name = std::move(name);
        r.title = std::move(title);
        r.description = std::move(description);
        r.mimeType = std::move(mime);
        r.live = true;
        r.isTemplate = isTemplate;
        r.read = std::move(read);
        registry.Add(std::move(r));
    };
    live("scct://editor/state", "editor-state", "Editor state",
         "The open map's identity and state: file, unsaved changes, level, generation, revision, Undo/Redo, geometry builds.", "application/json",
         [](const std::string&, ToolContext& c) { return Text(Call(c, "editor.state")); });
    live("scct://map/summary", "map-summary", "Map summary", "The open map at a glance: actor counts, spawns per team, missions, security, cameras (map_query summary).",
         "application/json", [](const std::string&, ToolContext& c) { return Text(MapSummary(c)); });
    live("scct://map/validation", "map-validation", "Map validation",
         "The open map checked by the editor's Map Check.", "application/json",
         [](const std::string&, ToolContext& c)
         {
             return Text(Call(c, "mapcheck.run", Json::object(), {}, false, 300000));
         });
    live("scct://map/screenshot/latest", "screenshot", "Viewport screenshot", "A PNG of the perspective viewport as it is now (1024 pixels on the longer side).", "image/png",
         [](const std::string&, ToolContext& c)
         {
             const auto shot = Call(c, "view.screenshot", {{"viewport", "perspective"}, {"maxEdge", 1024}}, {}, false, 120000);
             return Json{{"blob", shot.value("png", std::string())}, {"mimeType", "image/png"}};
         });
    live("scct://map/actor/{path}", "actor", "Actor", "One actor's editable values by object path, such as scct://map/actor/MyLevel.Light12.", "application/json",
         [](const std::string& uri, ToolContext& c) -> Json
         {
             std::map<std::string, std::string> parts;
             if (!MatchTemplate("scct://map/actor/{path}", uri, parts)) return Json();
             const auto identity = Resolve(c, Json::array({parts["path"]})).at(0);
             auto inspected = Call(c, "magic.inspect", {{"actor", identity}});
             inspected.erase("schema");
             return Text(inspected);
         },
         true);
}
}
