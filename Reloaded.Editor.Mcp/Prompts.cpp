// MCP prompts: the templates in Resources/prompts (Embedded.gen.h).
//
// Each template is a markdown recipe with {{argument}} placeholders. It starts
// with a front-matter block:
//
//   ---
//   name: create_level_from_description
//   title: Create a level from a description
//   description: One line for the client's prompt list.
//   arguments:
//     - name: description
//       description: What the level should be.
//       required: true
//     - name: modes
//       description: Game modes the map must play.
//       default: story, diskhunt, deathmatch
//   wiki: [index, spawn-points, game-modes]
//   docs: [tools]
//   rules: true
//   ---
//
// Placeholders in the body:
// - {{name}} is the argument, or its default.
// - {{#name}}...{{/name}} is kept only when the argument has a value.
// - {{^name}}...{{/name}} is kept only when it has none.
//
// prompts/get returns one user message: the filled body, then the wiki
// digests and docs the template lists, then the rule catalogue
// (scct://rules). The agent starts with the pages it needs.
//
// Templates are checked when they are registered, and
// tests/PromptsTests.cpp checks every one fills. Portable.
#include "Embedded.h"
#include "McpTool.h"
#include <cctype>
#include <set>
#include <sstream>
#if !defined(RE_MCP_STANDALONE) && __has_include("LevelRulesModel.h")
#include "LevelRulesModel.h"
#define RE_MCP_PROMPT_RULES 1
#endif

namespace Mcp
{
namespace PromptTemplates
{
    struct Argument { std::string name, description, fallback; bool required = false, hasDefault = false; };
    struct Template
    {
        std::string file, name, title, description, body;
        std::vector<Argument> arguments;
        std::vector<std::string> wiki, docs;
        bool rules = true;
    };

    inline std::string Trim(const std::string& text)
    {
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string::npos) return {};
        return text.substr(first, text.find_last_not_of(" \t") - first + 1);
    }
    inline bool Identifier(const std::string& text, bool dashes)
    {
        if (text.empty() || std::isdigit(static_cast<unsigned char>(text[0]))) return false;
        for (unsigned char c : text) if (!std::isalnum(c) && c != '_' && !(dashes && c == '-')) return false;
        return true;
    }
    inline const EmbeddedFile* Find(const std::string& path)
    {
        for (const auto& file : EmbeddedFiles()) if (path == file.path) return &file;
        return nullptr;
    }
    inline std::string Contents(const EmbeddedFile& file) { return std::string(reinterpret_cast<const char*>(file.data), file.size); }

    // A front-matter value: plain text, a JSON-quoted string, or [a, b, c].
    inline std::string Scalar(const std::string& raw, const std::string& where)
    {
        const auto text = Trim(raw);
        if (text.empty() || text[0] != '"') return text;
        const auto parsed = Json::parse(text, nullptr, false);
        if (!parsed.is_string()) throw std::runtime_error(where + ": a quoted value must be one JSON string.");
        return parsed.get<std::string>();
    }
    inline std::vector<std::string> List(const std::string& raw, const std::string& where)
    {
        const auto text = Trim(raw);
        if (text.size() < 2 || text.front() != '[' || text.back() != ']') throw std::runtime_error(where + " must be a list such as [a, b].");
        std::vector<std::string> out;
        std::istringstream items(text.substr(1, text.size() - 2));
        for (std::string item; std::getline(items, item, ',');)
            if (!Trim(item).empty()) out.push_back(Scalar(item, where));
        return out;
    }
    inline bool Flag(const std::string& raw, const std::string& where)
    {
        const auto text = Trim(raw);
        if (text == "true") return true;
        if (text == "false") return false;
        throw std::runtime_error(where + " must be true or false.");
    }

    inline Template Parse(const std::string& file, std::string text)
    {
        std::string clean;
        for (char c : text) if (c != '\r') clean += c;
        text = std::move(clean);
        if (text.rfind("---\n", 0) != 0) throw std::runtime_error(file + ": a prompt template starts with a --- front-matter block.");
        const auto end = text.find("\n---\n", 3);
        if (end == std::string::npos) throw std::runtime_error(file + ": the front-matter block has no closing ---.");
        Template t;
        t.file = file;
        t.body = text.substr(end + 5);
        std::istringstream lines(text.substr(4, end - 4 + 1));
        bool inArguments = false;
        int number = 1;
        for (std::string line; std::getline(lines, line); ++number)
        {
            const auto where = file + " line " + std::to_string(number + 1);
            if (Trim(line).empty() || Trim(line)[0] == '#') continue;
            const size_t indent = line.find_first_not_of(' ');
            auto content = line.substr(indent);
            const bool item = content.rfind("- ", 0) == 0;
            if (item) content = content.substr(2);
            const auto colon = content.find(':');
            if (colon == std::string::npos) throw std::runtime_error(where + ": expected \"key: value\".");
            const auto key = Trim(content.substr(0, colon)), value = content.substr(colon + 1);
            if (indent == 0 && !item)
            {
                inArguments = key == "arguments";
                if (inArguments) { if (!Trim(value).empty()) throw std::runtime_error(where + ": list the arguments on the lines below \"arguments:\"."); continue; }
                if (key == "name") t.name = Scalar(value, where);
                else if (key == "title") t.title = Scalar(value, where);
                else if (key == "description") t.description = Scalar(value, where);
                else if (key == "wiki") t.wiki = List(value, where);
                else if (key == "docs") t.docs = List(value, where);
                else if (key == "rules") t.rules = Flag(value, where);
                else throw std::runtime_error(where + ": unknown front-matter key \"" + key + "\".");
                continue;
            }
            if (!inArguments) throw std::runtime_error(where + ": indented lines belong to the arguments list.");
            if (item) t.arguments.push_back({});
            if (t.arguments.empty()) throw std::runtime_error(where + ": each argument starts with \"- name:\".");
            auto& argument = t.arguments.back();
            if (key == "name") argument.name = Scalar(value, where);
            else if (key == "description") argument.description = Scalar(value, where);
            else if (key == "required") argument.required = Flag(value, where);
            else if (key == "default") { argument.fallback = Scalar(value, where); argument.hasDefault = true; }
            else throw std::runtime_error(where + ": unknown argument key \"" + key + "\".");
        }
        if (!Identifier(t.name, false)) throw std::runtime_error(file + ": the prompt needs a name of letters, digits and underscores.");
        if (t.title.empty() || t.description.empty()) throw std::runtime_error(file + ": the prompt needs a title and a description.");
        std::set<std::string> names;
        for (const auto& argument : t.arguments)
        {
            if (!Identifier(argument.name, false)) throw std::runtime_error(file + ": argument \"" + argument.name + "\" needs a name of letters, digits and underscores.");
            if (!names.insert(argument.name).second) throw std::runtime_error(file + ": argument \"" + argument.name + "\" is listed twice.");
            if (argument.description.empty()) throw std::runtime_error(file + ": argument \"" + argument.name + "\" needs a description.");
            if (argument.required && argument.hasDefault) throw std::runtime_error(file + ": required argument \"" + argument.name + "\" cannot have a default.");
        }
        for (const auto& slug : t.wiki)
            if (!Identifier(slug, true) || !Find("wiki/" + slug + ".md")) throw std::runtime_error(file + ": no wiki digest \"" + slug + "\" (Resources/wiki/" + slug + ".md).");
        for (const auto& doc : t.docs)
            if (!Identifier(doc, true) || !Find("docs/" + doc + ".md")) throw std::runtime_error(file + ": no document \"" + doc + "\" (Resources/docs/" + doc + ".md).");
        return t;
    }

    // Each argument's text: given, else its default, else empty. A required
    // argument must be given; unknown ones are refused, so a typo is not
    // silently dropped.
    inline std::map<std::string, std::string> Values(const Template& t, const Json& given)
    {
        if (!given.is_null() && !given.is_object()) throw std::runtime_error("Prompt arguments are an object of strings.");
        std::map<std::string, std::string> values;
        for (const auto& argument : t.arguments)
        {
            std::string value;
            if (given.is_object() && given.contains(argument.name) && !given.at(argument.name).is_null())
            {
                const auto& v = given.at(argument.name);
                value = v.is_string() ? v.get<std::string>() : v.dump();
            }
            if (Trim(value).empty()) value = argument.hasDefault ? argument.fallback : std::string();
            if (argument.required && Trim(value).empty()) throw std::runtime_error("The prompt " + t.name + " needs the argument \"" + argument.name + "\": " + argument.description);
            values[argument.name] = value;
        }
        if (given.is_object())
            for (auto it = given.begin(); it != given.end(); ++it)
                if (!values.count(it.key()))
                {
                    std::string known;
                    for (const auto& argument : t.arguments) known += (known.empty() ? "" : ", ") + argument.name;
                    throw std::runtime_error("The prompt " + t.name + " has no argument \"" + it.key() + "\" (it takes: " + (known.empty() ? std::string("none") : known) + ").");
                }
        return values;
    }

    // The body with its placeholders replaced, sections kept or dropped.
    // Inserted values are not scanned again.
    inline std::string Render(const std::string& body, const std::map<std::string, std::string>& values, const std::string& file)
    {
        std::string out;
        size_t at = 0;
        while (at < body.size())
        {
            const auto open = body.find("{{", at);
            if (open == std::string::npos) { out += body.substr(at); break; }
            out += body.substr(at, open - at);
            const auto close = body.find("}}", open + 2);
            if (close == std::string::npos) throw std::runtime_error(file + ": unclosed {{ in the template.");
            const auto tag = body.substr(open + 2, close - open - 2);
            const char kind = tag.empty() ? '\0' : tag[0];
            const auto name = kind == '#' || kind == '^' || kind == '/' ? tag.substr(1) : tag;
            const auto value = values.find(name);
            if (value == values.end()) throw std::runtime_error(file + ": {{" + tag + "}} names no argument of this prompt.");
            if (kind == '/') throw std::runtime_error(file + ": {{" + tag + "}} closes a section that was not opened.");
            if (kind != '#' && kind != '^')
            {
                out += value->second;
                at = close + 2;
                continue;
            }
            // The matching {{/name}}, past any nested section of the same name.
            const std::string opening = "{{#" + name + "}}", inverted = "{{^" + name + "}}", ending = "{{/" + name + "}}";
            size_t depth = 1, scan = close + 2, inner = scan;
            while (depth)
            {
                const auto next = body.find("{{", scan);
                if (next == std::string::npos) throw std::runtime_error(file + ": {{" + tag + "}} has no " + ending + ".");
                if (body.compare(next, opening.size(), opening) == 0 || body.compare(next, inverted.size(), inverted) == 0) ++depth;
                else if (body.compare(next, ending.size(), ending) == 0 && !--depth) { inner = next; break; }
                scan = next + 2;
            }
            const bool given = !Trim(value->second).empty();
            if (given == (kind == '#')) out += Render(body.substr(close + 2, inner - close - 2), values, file);
            at = inner + ending.size();
        }
        return out;
    }

    inline std::string RulesMarkdown()
    {
#ifdef RE_MCP_PROMPT_RULES
        return Rules::CatalogMarkdown();
#else
        if (const auto* file = Find("rules.md")) return Contents(*file);
        return "Read scct://rules for the rule catalogue (this build has none to embed).\n";
#endif
    }

    // The message text: the filled recipe, then the reference pages.
    inline std::string Text(const Template& t, const Json& arguments)
    {
        // Dropped sections can leave a line of indentation behind; trim each line's end.
        std::string text;
        std::istringstream lines(Render(t.body, Values(t, arguments), t.file));
        for (std::string line; std::getline(lines, line);)
        {
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
            text += line + "\n";
        }
        while (text.size() > 1 && text[text.size() - 2] == '\n') text.pop_back();
        if (!t.wiki.empty() || !t.docs.empty())
        {
            text += "\n---\n\n# Reference pages\n\nThe pages this recipe relies on, embedded so you have them now. They are also resources: ";
            std::string uris;
            for (const auto& slug : t.wiki) uris += (uris.empty() ? "" : ", ") + std::string("scct://wiki/") + slug;
            for (const auto& doc : t.docs) uris += (uris.empty() ? "" : ", ") + std::string("scct://docs/") + doc;
            text += uris + ". Every other wiki page is listed in scct://wiki/index.\n";
            auto embed = [&](const std::string& uri, const std::string& path)
            {
                auto page = Contents(*Find(path));
                while (!page.empty() && page.back() == '\n') page.pop_back();
                text += "\n---\n\n<!-- " + uri + " -->\n\n" + page + "\n";
            };
            for (const auto& slug : t.wiki) embed("scct://wiki/" + slug, "wiki/" + slug + ".md");
            for (const auto& doc : t.docs) embed("scct://docs/" + doc, "docs/" + doc + ".md");
        }
        if (t.rules)
        {
            auto rules = RulesMarkdown();
            while (!rules.empty() && rules.back() == '\n') rules.pop_back();
            text += "\n---\n\n<!-- scct://rules -->\n\n" + rules + "\n";
        }
        return text;
    }

    // Every template in Resources/prompts, parsed once, in file order.
    inline const std::vector<Template>& Templates()
    {
        static const std::vector<Template> templates = []
        {
            std::vector<Template> out;
            std::set<std::string> names;
            for (const auto& file : EmbeddedFiles())
            {
                const std::string path = file.path;
                if (path.rfind("prompts/", 0) != 0 || path.size() < 3 || path.compare(path.size() - 3, 3, ".md") != 0) continue;
                auto t = Parse(path, Contents(file));
                if (t.name != path.substr(8, path.size() - 11)) throw std::runtime_error(path + ": the prompt's name must match its file name.");
                if (!names.insert(t.name).second) throw std::runtime_error(path + ": a second prompt named " + t.name + ".");
                out.push_back(std::move(t));
            }
            return out;
        }();
        return templates;
    }
}

void RegisterPrompts(Registry& registry)
{
    for (const auto& t : PromptTemplates::Templates())
    {
        Prompt p;
        p.name = t.name;
        p.title = t.title;
        p.description = t.description;
        for (const auto& argument : t.arguments)
            p.arguments.push_back({argument.name, argument.description + (argument.hasDefault ? " Default: " + argument.fallback + "." : std::string()), argument.required});
        const auto* source = &t;
        p.messages = [source](const Json& arguments, ToolContext&) -> Json
        {
            return Json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", PromptTemplates::Text(*source, arguments)}}}}});
        };
        registry.Add(std::move(p));
    }
}
}
