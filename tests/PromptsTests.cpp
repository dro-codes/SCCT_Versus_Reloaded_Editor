// The embedded MCP resources and prompts (Reloaded.Editor.Mcp/Resources,
// Embedded.gen.h, Prompts.cpp):
// - every wiki digest, schema and document is present;
// - every prompt template parses and fills;
// - the tool names the pages cite exist.
//
// Embedded.gen.h is generated, not committed: tools/test_mcp.cmd and
// tools/test_mcp.sh write it first (tools/mcp/embed_resources.ps1 or .py).
#include "../Reloaded.Editor.Mcp/Prompts.cpp"
#include <iostream>
#include <regex>
#include <source_location>
using namespace Mcp;
using Workflow::Json;
void Check(bool value, const std::string& message, std::source_location where = std::source_location::current())
{
    if (!value) throw std::runtime_error(message + " (line " + std::to_string(where.line()) + ")");
}
template<class F> void Reject(F f, std::source_location where = std::source_location::current())
{
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    if (!caught) throw std::runtime_error("invalid input accepted at line " + std::to_string(where.line()));
}
// Prompts never reach the editor.
struct NoBridge : IBridge
{
    Json Call(const std::string& op, const Json&, const CallOptions&) override { throw std::runtime_error("prompt called the bridge: " + op); }
    Json Hello() override { throw std::runtime_error("prompt called hello"); }
    bool Connected() override { return false; }
    Json State() override { return Json::object(); }
};
std::string Text(const EmbeddedFile& file) { return std::string(reinterpret_cast<const char*>(file.data), file.size); }
const EmbeddedFile* File(const std::string& path)
{
    for (const auto& file : EmbeddedFiles()) if (path == file.path) return &file;
    return nullptr;
}
const std::vector<std::string> kSlugs = {"home", "spawn-points", "game-modes", "map-settings", "level-properties", "vision-mode-properties", "geometric-events",
    "events", "alarms", "movers", "elevators", "lighting", "material-classes", "static-meshes", "sound-actors", "sound-browser", "gameplay-objects",
    "buttons-and-hacking-panels", "level-ingredients", "zones-and-volumes", "softbody-actors", "animated-meshes", "fish", "map-tutorials", "map-packaging"};
const std::set<std::string> kTools = {"editor_status", "editor_launch", "editor_restart", "editor_undo", "approval_wait", "editor_log", "map_file", "map_build",
    "map_query", "view_screenshot", "actors_edit", "map_changes_apply", "workflow_request", "editor_exec"};
const std::set<std::string> kPrompts = {};
// snake_case words in backticks that are not tools: tool actions, error codes and outside names.
const std::set<std::string> kOtherWords = {"rename_tag", "copy_properties", "save_as", "awaiting_approval", "not_connected", "protocol_mismatch", "dialog_blocked",
    "editor_error", "another_client", "io_scene_ase", "approval_pending", "outside_write_roots"};

// Every `snake_case` word in backticks names a tool, a prompt or a known code.
void CheckToolNames(const std::string& path, const std::string& text)
{
    static const std::regex word("`([a-z]+(?:_[a-z]+)+)`");
    for (std::sregex_iterator it(text.begin(), text.end(), word), end; it != end; ++it)
    {
        const auto name = (*it)[1].str();
        Check(kTools.count(name) || kPrompts.count(name) || kOtherWords.count(name), path + " names `" + name + "`, which is not an MCP tool");
    }
}

void EmbeddedResources()
{
    for (const auto& file : EmbeddedFiles())
    {
        const std::string path = file.path, mime = file.mime;
        Check(path.find('\\') == std::string::npos && path.find("..") == std::string::npos, path + ": paths are relative with forward slashes");
        Check(file.data[file.size] == 0, path + ": the data ends in a zero byte");
        const auto dot = path.rfind('.');
        const auto extension = dot == std::string::npos ? std::string() : path.substr(dot);
        if (extension == ".md") Check(mime == "text/markdown", path + " is text/markdown");
        if (extension == ".json")
        {
            Check(mime == "application/json", path + " is application/json");
            Check(!Json::parse(Text(file), nullptr, false).is_discarded(), path + " parses");
        }
    }
    // One digest per wiki page, each citing its source.
    for (const auto& slug : kSlugs)
    {
        const auto* file = File("wiki/" + slug + ".md");
        Check(file, "wiki/" + slug + ".md is embedded");
        const auto text = Text(*file);
        Check(text.rfind("# ", 0) == 0, slug + " starts with its title");
        Check(text.find("\nSource: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/") != std::string::npos && text.find("\nSource: ") < 400, slug + " has its Source line under the title");
        Check(text.find("](https://github.com/user-attachments") == std::string::npos && text.find("<img") == std::string::npos, slug + " copies no wiki images");
        CheckToolNames("wiki/" + slug + ".md", text);
    }
    Check(File("wiki/index.md") && Text(*File("wiki/index.md")).find("Minimum viable Versus map checklist") != std::string::npos, "the index has the checklist");
    CheckToolNames("wiki/index.md", Text(*File("wiki/index.md")));
    for (const char* name : {"map-changes", "map-authoring", "stages", "smagic-event", "build-plan"})
    {
        const auto* file = File(std::string("schemas/") + name + ".schema.json");
        Check(file, std::string(name) + " schema is embedded");
        const auto schema = Json::parse(Text(*file));
        Check(schema.value("$id", std::string()) == std::string("scct://schema/") + name, std::string(name) + " schema has its scct:// $id");
        Check(schema.value("$schema", std::string()) == "https://json-schema.org/draft/2020-12/schema", std::string(name) + " schema is draft 2020-12");
    }
    for (const char* name : {"map-authoring", "map-design", "tools"})
    {
        Check(File(std::string("docs/") + name + ".md"), std::string("docs/") + name + ".md is embedded");
        CheckToolNames(std::string("docs/") + name + ".md", Text(*File(std::string("docs/") + name + ".md")));
    }
    // Paths are in byte order, so the generated header is deterministic.
    for (size_t i = 1; i < EmbeddedFiles().size(); ++i) Check(std::string(EmbeddedFiles()[i - 1].path) < EmbeddedFiles()[i].path, "embedded files are sorted");
}

void TemplateSyntax()
{
    using namespace PromptTemplates;
    const auto t = Parse("prompts/x.md",
        "---\r\nname: sample\r\ntitle: \"Sample: quoted\"\r\ndescription: A test.\r\narguments:\r\n  - name: need\r\n    description: Needed.\r\n    required: true\r\n"
        "  - name: maybe\r\n    description: Optional.\r\n  - name: mode\r\n    description: With a default.\r\n    default: story\r\nwiki: [index, game-modes]\r\n---\r\n"
        "A {{need}} in {{mode}}.{{#maybe}} Maybe {{maybe}}{{#maybe}}!{{/maybe}}.{{/maybe}}{{^maybe}} No maybe.{{/maybe}}\r\n");
    Check(t.name == "sample" && t.title == "Sample: quoted" && t.arguments.size() == 3 && t.wiki.size() == 2, "front matter parses");
    Check(t.arguments[0].required && !t.arguments[1].required && t.arguments[2].hasDefault && t.arguments[2].fallback == "story", "argument fields parse");
    Check(Render(t.body, Values(t, {{"need", "door"}}), t.file) == "A door in story. No maybe.\n", "defaults and inverted sections");
    Check(Render(t.body, Values(t, {{"need", "door"}, {"maybe", "{{need}}"}, {"mode", "deathmatch"}}), t.file) == "A door in deathmatch. Maybe {{need}}!.\n",
          "values are inserted once, sections nest");
    Check(Render(t.body, Values(t, {{"need", 3}}), t.file).rfind("A 3 in", 0) == 0, "non-string values are written as JSON");
    Reject([&] { Values(t, Json::object()); });                          // required argument missing
    Reject([&] { Values(t, {{"need", "  "}}); });                        // blank counts as missing
    Reject([&] { Values(t, {{"need", "x"}, {"typo", "y"}}); });          // unknown argument
    Reject([&] { Values(t, Json::array()); });
    const auto text = Text(t, {{"need", "door"}});
    Check(text.find("<!-- scct://wiki/game-modes -->") != std::string::npos, "listed pages embedded");
    auto parse = [](const std::string& source) { return Parse("prompts/bad.md", source); };
    Reject([&] { parse("name: x\n"); });                                                                       // no front matter
    Reject([&] { parse("---\nname: x\ntitle: T\ndescription: D\n"); });                                        // unclosed
    Reject([&] { parse("---\nname: x\ntitle: T\ndescription: D\ncolour: red\n---\n"); });                      // unknown key
    Reject([&] { parse("---\nname: 1x\ntitle: T\ndescription: D\n---\n"); });                                  // bad name
    Reject([&] { parse("---\nname: x\ntitle: T\n---\n"); });                                                   // no description
    Reject([&] { parse("---\nname: x\ntitle: T\ndescription: D\nwiki: [no-such-page]\n---\n"); });           // unknown slug
    Reject([&] { parse("---\nname: x\ntitle: T\ndescription: D\ndocs: [nothing]\n---\n"); });                // unknown document
    Reject([&] { parse("---\nname: x\ntitle: T\ndescription: D\nwiki: index\n---\n"); });                    // not a list
    Reject([&] { parse("---\nname: x\ntitle: T\ndescription: D\narguments:\n  - name: a\n    description: A\n  - name: a\n    description: B\n---\n"); });
    Reject([&] { parse("---\nname: x\ntitle: T\ndescription: D\narguments:\n  - name: a\n---\n"); });        // argument without description
    Reject([&] { parse("---\nname: x\ntitle: T\ndescription: D\narguments:\n  - name: a\n    description: A\n    required: true\n    default: z\n---\n"); });
    const auto bad = parse("---\nname: x\ntitle: T\ndescription: D\narguments:\n  - name: a\n    description: A\n---\n{{b}} {{#a}}open");
    Reject([&] { Render("{{b}}", Values(bad, Json::object()), "x"); });   // unknown placeholder
    Reject([&] { Render("{{#a}}open", Values(bad, Json::object()), "x"); }); // unclosed section
    Reject([&] { Render("{{/a}}", Values(bad, Json::object()), "x"); });  // stray close
    Reject([&] { Render("{{a", Values(bad, Json::object()), "x"); });     // unclosed tag
}

void RegisteredPrompts()
{
    Registry registry;
    RegisterPrompts(registry);
    std::set<std::string> names;
    for (const auto& p : registry.prompts) names.insert(p.name);
    Check(names == kPrompts, "no prompts are registered");
    NoBridge bridge;
    ToolContext context{bridge, {}, {}};
    for (const auto& t : PromptTemplates::Templates())
    {
        const auto* p = registry.FindPrompt(t.name);
        Check(p && !p->title.empty() && !p->description.empty(), t.name + " is registered with a title and description");
        // Required arguments only, then every argument.
        Json minimal = Json::object(), full = Json::object();
        for (const auto& argument : p->arguments)
        {
            if (argument.required) minimal[argument.name] = "SAMPLE_" + argument.name;
            full[argument.name] = "SAMPLE_" + argument.name;
        }
        for (const auto& arguments : {minimal, full})
        {
            const auto messages = p->messages(arguments, context);
            Check(messages.is_array() && messages.size() == 1 && messages[0].value("role", std::string()) == "user", t.name + " is one user message");
            const auto& content = messages[0].at("content");
            Check(content.value("type", std::string()) == "text", t.name + " is text");
            const auto text = content.value("text", std::string());
            Check(text.find("{{") == std::string::npos && text.find("}}") == std::string::npos, t.name + " fills every placeholder");
            for (auto it = arguments.begin(); it != arguments.end(); ++it)
                Check(text.find(it.value().get<std::string>()) != std::string::npos, t.name + " uses its argument " + it.key());
            for (const auto& slug : t.wiki)
                Check(text.find("<!-- scct://wiki/" + slug + " -->\n\n# ") != std::string::npos, t.name + " embeds scct://wiki/" + slug);
            for (const auto& doc : t.docs) Check(text.find("<!-- scct://docs/" + doc + " -->") != std::string::npos, t.name + " embeds scct://docs/" + doc);
            Check(text.size() < 200000, t.name + " stays a reasonable size");
        }
        Check(PromptTemplates::Render(t.body, PromptTemplates::Values(t, full), t.file).find("## Recipe") != std::string::npos, t.name + " has a recipe");
        CheckToolNames(t.file, t.body);
        for (const auto& argument : p->arguments)
            if (argument.required)
                Reject([&] { Json missing = full; missing.erase(argument.name); p->messages(missing, context); });
        Reject([&] { Json extra = minimal; extra["no_such_argument"] = "x"; p->messages(extra, context); });
    }
}

int main()
{
    try
    {
        EmbeddedResources();
        TemplateSyntax();
        RegisteredPrompts();
        std::cout << "PromptsTests passed (" << EmbeddedFiles().size() << " embedded files, " << PromptTemplates::Templates().size() << " prompts)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "PromptsTests failed: " << error.what() << "\n";
        return 1;
    }
}
