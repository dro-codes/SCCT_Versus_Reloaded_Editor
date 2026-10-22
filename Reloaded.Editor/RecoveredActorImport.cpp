#include "RecoveredActorImport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace
{
    struct Actor
    {
        std::string className;
        std::string name;
        std::vector<std::string> lines;
    };

    std::string Lower(std::string_view text)
    {
        std::string result(text);
        for (char& c : result)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return result;
    }

    std::string_view Trim(std::string_view text)
    {
        const auto first = text.find_first_not_of(" \t\r");
        if (first == std::string_view::npos)
            return {};
        const auto last = text.find_last_not_of(" \t\r");
        return text.substr(first, last - first + 1);
    }

    std::string ClassLeaf(std::string_view text)
    {
        const auto dot = text.find_last_of('.');
        return Lower(dot == std::string_view::npos ? text : text.substr(dot + 1));
    }

    // Native FName exports can append the diagnostic suffix \xA7(number).
    // Do not change this sequence inside authored double-quoted strings.
    std::string NormalizeNames(std::string_view text)
    {
        std::string result;
        result.reserve(text.size());
        bool quoted = false;
        bool escaped = false;
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            std::size_t marker = 0;
            if (!quoted && c == 0xA7)
                marker = 1;
            else if (!quoted && c == 0xC2 && i + 1 < text.size()
                     && static_cast<unsigned char>(text[i + 1]) == 0xA7)
                marker = 2;
            if (marker && i + marker + 2 < text.size()
                && text[i + marker] == '(')
            {
                std::size_t end = i + marker + 1;
                while (end < text.size()
                       && std::isdigit(static_cast<unsigned char>(text[end])))
                    ++end;
                if (end > i + marker + 1 && end < text.size() && text[end] == ')')
                {
                    i = end;
                    continue;
                }
            }
            result += text[i];
            if (text[i] == '"' && !escaped)
                quoted = !quoted;
            if (quoted && text[i] == '\\' && !escaped)
                escaped = true;
            else
                escaped = false;
        }
        return result;
    }

    std::string Token(std::string_view& text)
    {
        text = Trim(text);
        const auto end = text.find_first_of(" \t\r");
        const std::string result(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
        return result;
    }

    bool Boundary(std::string_view line, std::string& verb, std::string& kind)
    {
        verb = Lower(Token(line));
        if (verb != "begin" && verb != "end")
            return false;
        kind = Lower(Token(line));
        return true;
    }

    std::string Attribute(std::string_view line, std::string_view wanted)
    {
        while (!(line = Trim(line)).empty())
        {
            const auto equals = line.find('=');
            const auto space = line.find_first_of(" \t");
            if (equals == std::string_view::npos)
                return {};
            if (space != std::string_view::npos && space < equals)
            {
                line.remove_prefix(space + 1);
                continue;
            }
            const std::string key = Lower(Trim(line.substr(0, equals)));
            line = Trim(line.substr(equals + 1));
            std::string value;
            if (!line.empty() && (line.front() == '"' || line.front() == '\''))
            {
                const char quote = line.front();
                line.remove_prefix(1);
                const auto end = line.find(quote);
                if (end == std::string_view::npos)
                    return {};
                value = line.substr(0, end);
                line.remove_prefix(end + 1);
            }
            else
                value = Token(line);
            if (key == wanted)
                return value;
        }
        return {};
    }

    void SetNameAttribute(std::string& line, std::string_view name)
    {
        std::string_view remaining(line);
        while (!(remaining = Trim(remaining)).empty())
        {
            const auto equals = remaining.find('=');
            const auto space = remaining.find_first_of(" \t");
            if (equals == std::string_view::npos) return;
            if (space != std::string_view::npos && space < equals)
            {
                remaining.remove_prefix(space + 1);
                continue;
            }
            const auto key = Lower(Trim(remaining.substr(0, equals)));
            remaining = Trim(remaining.substr(equals + 1));
            const bool quoted = !remaining.empty() && (remaining.front() == '"' || remaining.front() == '\'');
            const char quote = quoted ? remaining.front() : '\0';
            if (quoted) remaining.remove_prefix(1);
            const auto end = quoted ? remaining.find(quote) : remaining.find_first_of(" \t\r");
            const auto value = remaining.substr(0, end);
            if (key == "name")
            {
                const auto offset = static_cast<std::size_t>(value.data() - line.data());
                line.replace(offset, value.size(), name);
                return;
            }
            if (end == std::string_view::npos) return;
            remaining.remove_prefix(end + (quoted ? 1 : 0));
        }
    }

    bool Parse(std::string_view text, std::vector<Actor>& actors, std::string& error)
    {
        if (text.size() > 256u * 1024u * 1024u)
        {
            error = "The T3D snapshot exceeds the supported 256 MB size.";
            return false;
        }
        std::istringstream input(NormalizeNames(text));
        std::vector<std::string> blocks;
        std::set<std::string> actorNames;
        bool seenMap = false;
        bool endedMap = false;
        bool malformed = false;
        std::string line;
        while (std::getline(input, line))
        {
            if (!seenMap && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
                line.erase(0, 3);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const auto trimmed = Trim(line);
            if (trimmed.empty())
            {
                if (blocks.size() >= 2)
                    actors.back().lines.push_back(line);
                continue;
            }
            std::string verb, kind;
            if (Boundary(trimmed, verb, kind))
            {
                if (kind.empty() || endedMap)
                {
                    malformed = true;
                    break;
                }
                if (verb == "begin")
                {
                    if (blocks.empty())
                    {
                        if (seenMap || kind != "map")
                        {
                            malformed = true;
                            break;
                        }
                        seenMap = true;
                    }
                    else if (blocks.size() == 1)
                    {
                        if (kind != "actor" && kind != "surface" && kind != "ge")
                        {
                            malformed = true;
                            break;
                        }
                        Actor actor;
                        actor.className = kind == "actor"
                            ? Attribute(trimmed, "class") : "$" + kind;
                        actor.name = kind == "actor" ? Attribute(trimmed, "name") : kind;
                        if (actor.className.empty() || actor.name.empty()
                            || (kind == "actor" && !actorNames.insert(Lower(actor.name)).second))
                        {
                            malformed = true;
                            break;
                        }
                        actors.push_back(std::move(actor));
                    }
                    else if (kind == "actor" || kind == "map")
                    {
                        malformed = true;
                        break;
                    }
                    if (blocks.size() >= 128)
                    {
                        malformed = true;
                        break;
                    }
                    blocks.push_back(kind);
                    if (blocks.size() >= 2)
                        actors.back().lines.push_back(line);
                }
                else
                {
                    if (blocks.empty() || blocks.back() != kind)
                    {
                        malformed = true;
                        break;
                    }
                    if (blocks.size() >= 2)
                        actors.back().lines.push_back(line);
                    blocks.pop_back();
                    if (blocks.empty())
                        endedMap = true;
                }
            }
            else
            {
                if (blocks.size() < 2)
                {
                    malformed = true;
                    break;
                }
                actors.back().lines.push_back(line);
            }
        }
        if (malformed || !input.eof() || !seenMap || !endedMap || !blocks.empty())
        {
            error = "The actor snapshot has malformed map/actor/object boundaries, "
                    "duplicate actor names, or a missing actor class/name.";
            actors.clear();
            return false;
        }
        return true;
    }

    bool RuntimeProperty(std::string_view line, bool levelInfo)
    {
        const auto equals = line.find('=');
        if (equals == std::string_view::npos)
            return false;
        auto key = Trim(line.substr(0, equals));
        key = Trim(key.substr(0, key.find('(')));
        const std::string property = Lower(key);
        static const std::set<std::string> transient = {
            "level", "region", "staticmeshinstance", "staticmeshinstanceonxbox", "nextnavigationpoint",
            "navigationpointlist", "bmustinitnetchannels", "physicsvolume",
            "touching", "stateframe", "renderdata", "lightrenderdata",
            "lastrendertime", "lastrendertimeonscreen", "octreenodes", "leaves",
            "nettag", "netupdatetime", "latentaction", "name", "bselected"
        };
        if (transient.count(property))
            return true;
        return levelInfo && (property == "summary" || property == "timeseconds"
            || property == "m_dt");
    }

    Actor Sanitize(Actor actor, std::size_t& removed)
    {
        std::vector<std::string> lines;
        std::size_t depth = 0;
        for (const auto& line : actor.lines)
        {
            std::string verb, kind;
            if (Boundary(Trim(line), verb, kind))
            {
                if (verb == "begin")
                    ++depth;
                else
                    --depth;
                lines.push_back(line);
            }
            else if (depth == 1 && RuntimeProperty(Trim(line),
                                                   ClassLeaf(actor.className) == "levelinfo"))
                ++removed;
            else
                lines.push_back(line);
        }
        actor.lines = std::move(lines);
        return actor;
    }

    std::string Text(const Actor& actor)
    {
        std::string result;
        for (const auto& line : actor.lines)
            result += line + '\n';
        return result;
    }

    std::string_view ObjectReferencePath(std::string_view path)
    {
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            return path.substr(1, path.size() - 2);
        return path;
    }

    std::string ImportReferencePath(std::string_view path)
    {
        path = ObjectReferencePath(path);
        const bool whitespace = std::any_of(path.begin(), path.end(), [](char value) {
            return std::isspace(static_cast<unsigned char>(value)) != 0;
        });
        return whitespace ? "\"" + std::string(path) + "\"" : std::string(path);
    }

    void Definitions(const Actor& actor, std::set<std::string>& names, bool nativeParticleRoots = true)
    {
        if (!actor.className.empty() && actor.className.front() == '$')
            return;
        names.insert(Lower(actor.name));
        std::vector<std::string> objectNames;
        for (const auto& line : actor.lines)
        {
            std::string verb, kind;
            if (!Boundary(Trim(line), verb, kind))
                continue;
            if (verb == "begin")
            {
                const std::string name = Attribute(line, "name");
                objectNames.push_back(name);
                if (name.empty())
                    continue;
                // The native map importer creates brush models and inline
                // particle objects in the map package, regardless of nesting.
                const auto type = ClassLeaf(Attribute(line, "class"));
                if (kind == "brush" || (nativeParticleRoots && kind == "object"
                    && (type == "spriteemitter" || type == "meshemitter")))
                    names.insert(Lower(name));
                std::string path;
                for (const auto& part : objectNames)
                    if (!part.empty())
                        path += (path.empty() ? "" : ".") + part;
                names.insert(Lower(path));
            }
            else
                objectNames.pop_back();
        }
    }

    void CheckReference(std::string_view path, const std::set<std::string>& definitions,
                        std::set<std::string>& missing)
    {
        const std::string lower = Lower(path);
        constexpr std::string_view prefix = "mylevel.";
        if (lower.compare(0, prefix.size(), prefix) == 0
            && !definitions.count(lower.substr(prefix.size())))
            missing.insert(std::string(path));
    }

    using ReferenceClasses = std::map<std::string, std::set<std::string>>;

    void References(const Actor& actor, const std::set<std::string>& definitions,
                    std::set<std::string>& missing, ReferenceClasses* classes = nullptr)
    {
        for (const auto& line : actor.lines)
        {
            bool inString = false;
            bool escaped = false;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"' && !escaped)
                    inString = !inString;
                if (!inString && line[i] == '\'')
                {
                    const auto end = line.find('\'', i + 1);
                    if (end == std::string::npos)
                        break;
                    const auto path = ObjectReferencePath(std::string_view(line).substr(i + 1, end - i - 1));
                    CheckReference(path, definitions, missing);
                    if (classes)
                    {
                        auto beginClass = i;
                        while (beginClass > 0 &&
                            (std::isalnum(static_cast<unsigned char>(line[beginClass - 1]))
                             || line[beginClass - 1] == '_' || line[beginClass - 1] == '.'))
                            --beginClass;
                        (*classes)[Lower(path)].insert(line.substr(beginClass, i - beginClass));
                    }
                    i = end;
                }
                escaped = inString && line[i] == '\\' && !escaped;
            }
            std::string verb, kind;
            if (Boundary(Trim(line), verb, kind) && verb == "begin" && kind == "polygon")
            {
                const auto texture = Attribute(line, "texture");
                CheckReference(texture, definitions, missing);
                if (classes && Lower(texture).compare(0, 8, "mylevel.") == 0)
                    (*classes)[Lower(texture)].insert("Material");
            }
        }
    }

    bool ValidateReferences(const std::vector<Actor>& actors,
                            std::vector<std::string>& unsupported, std::string& error)
    {
        std::set<std::string> definitions, missing;
        for (const auto& actor : actors)
            Definitions(actor, definitions);
        for (const auto& actor : actors)
            References(actor, definitions, missing);
        unsupported.assign(missing.begin(), missing.end());
        if (missing.empty())
            return true;
        error = "The actor snapshot references map-local objects/assets that were not "
                "exported. Recovery stopped to avoid losing them: ";
        std::size_t count = 0;
        for (const auto& reference : missing)
        {
            if (count++)
                error += ", ";
            error += reference;
            if (count == 8)
                break;
        }
        if (missing.size() > count)
            error += " (and " + std::to_string(missing.size() - count) + " more)";
        return false;
    }

    void RewritePath(Actor& actor, std::string_view original, std::string_view replacement,
                     bool descendants = false, std::string_view classFilter = {})
    {
        const std::string oldPath = Lower(original);
        for (auto& line : actor.lines)
        {
            bool inString = false;
            bool escaped = false;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"' && !escaped)
                    inString = !inString;
                if (!inString && line[i] == '\'')
                {
                    auto end = line.find('\'', i + 1);
                    if (end == std::string::npos)
                        break;
                    const auto reference = ObjectReferencePath(std::string_view(line).substr(i + 1, end - i - 1));
                    auto beginClass = i;
                    while (beginClass > 0 && (std::isalnum(static_cast<unsigned char>(line[beginClass - 1]))
                        || line[beginClass - 1] == '_' || line[beginClass - 1] == '.')) --beginClass;
                    const bool matchesClass = classFilter.empty()
                        || ClassLeaf(std::string_view(line).substr(beginClass, i - beginClass)) == classFilter;
                    const auto lowerReference = Lower(reference);
                    if (matchesClass && (lowerReference == oldPath || (descendants && lowerReference.size() > oldPath.size()
                        && lowerReference.compare(0, oldPath.size(), oldPath) == 0
                        && lowerReference[oldPath.size()] == '.')))
                    {
                        const std::string path = ImportReferencePath(std::string(replacement)
                            + std::string(reference.substr(oldPath.size())));
                        line.replace(i + 1, end - i - 1, path);
                        end = i + 1 + path.size();
                    }
                    i = end;
                }
                escaped = inString && line[i] == '\\' && !escaped;
            }
            std::string verb, kind;
            if (Boundary(Trim(line), verb, kind) && verb == "begin" && kind == "polygon")
            {
                const auto texture = Attribute(line, "texture");
                if (Lower(texture) == oldPath)
                {
                    const auto at = line.find(texture);
                    if (at != std::string::npos)
                        line.replace(at, texture.size(), replacement);
                }
            }
        }
    }

    void RewriteLevelInfo(Actor& actor, std::string_view original, std::string_view replacement)
    {
        RewritePath(actor, "MyLevel." + std::string(original), "MyLevel." + std::string(replacement));
    }

    void ClearKnownActorReferences(Actor& actor, const std::set<std::string>& skipped,
                                      std::size_t& cleared)
    {
        for (auto& line : actor.lines)
        {
            bool inString = false;
            bool escaped = false;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"' && !escaped) inString = !inString;
                if (!inString && line[i] == '\'')
                {
                    const auto end = line.find('\'', i + 1);
                    if (end == std::string::npos) break;
                    if (skipped.count(Lower(ObjectReferencePath(std::string_view(line).substr(i + 1, end - i - 1)))))
                    {
                        auto begin = i;
                        while (begin > 0 && (std::isalnum(static_cast<unsigned char>(line[begin - 1]))
                            || line[begin - 1] == '_' || line[begin - 1] == '.')) --begin;
                        // Unreal object references have a class immediately
                        // before the quote. Never rewrite bare quoted text.
                        if (begin == i || (!std::isalpha(static_cast<unsigned char>(line[begin]))
                            && line[begin] != '_'))
                        {
                            i = end;
                            continue;
                        }
                        line.replace(begin, end - begin + 1, "None");
                        ++cleared;
                        i = begin + 3;
                    }
                    else i = end;
                }
                escaped = inString && line[i] == '\\' && !escaped;
            }
        }
    }

    void QuoteImportReferences(Actor& actor)
    {
        for (auto& line : actor.lines)
        {
            bool inString = false, escaped = false;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"' && !escaped) inString = !inString;
                if (!inString && line[i] == '\'')
                {
                    const auto end = line.find('\'', i + 1);
                    if (end == std::string::npos) break;
                    // Only typed object references use this native syntax.
                    if (i > 0 && (std::isalnum(static_cast<unsigned char>(line[i - 1])) || line[i - 1] == '_'))
                    {
                        const auto path = ImportReferencePath(std::string_view(line).substr(i + 1, end - i - 1));
                        line.replace(i + 1, end - i - 1, path);
                        i += path.size() + 1;
                    }
                    else i = end;
                }
                escaped = inString && line[i] == '\\' && !escaped;
            }
        }
    }

    std::string ReserveGeneratedName(std::string_view original, std::set<std::string>& used);

    bool NormalizeInlineEmitterReferences(std::vector<Actor>& actors, std::string& error)
    {
        struct Component
        {
            std::size_t actor, line;
            std::string name, path, type;
        };
        std::vector<Component> components;
        std::set<std::string> definitions, missing;
        std::set<std::string> used = {"levelinfo0"}, componentPaths;
        std::map<std::string, std::map<std::string, std::set<std::string>>> aliases;
        ReferenceClasses classes;
        for (std::size_t actorIndex = 0; actorIndex < actors.size(); ++actorIndex)
        {
            const auto& actor = actors[actorIndex];
            Definitions(actor, definitions, false);
            if (!actor.className.empty() && actor.className.front() != '$') used.insert(Lower(actor.name));
            std::vector<std::string> parents;
            for (std::size_t lineIndex = 0; lineIndex < actor.lines.size(); ++lineIndex)
            {
                const auto& line = actor.lines[lineIndex];
                std::string verb, kind;
                if (!Boundary(Trim(line), verb, kind)) continue;
                if (verb == "end")
                {
                    parents.pop_back();
                    continue;
                }
                const auto name = Attribute(line, "name");
                parents.push_back(name);
                const auto type = ClassLeaf(Attribute(line, "class"));
                if (kind != "object" || name.empty()
                    || (type != "spriteemitter" && type != "meshemitter"))
                {
                    if (!name.empty() && (kind == "object" || kind == "brush")) used.insert(Lower(name));
                    continue;
                }
                std::string path = "MyLevel";
                for (const auto& parent : parents)
                    if (!parent.empty()) path += "." + parent;
                if (!componentPaths.insert(Lower(path)).second)
                {
                    error = "The actor snapshot contains duplicate inline particle objects: " + path;
                    return false;
                }
                aliases[Lower("MyLevel." + name)][type].insert(path);
                components.push_back({actorIndex, lineIndex, name, path, type});
            }
        }
        for (const auto& actor : actors) References(actor, definitions, missing, &classes);
        // Cooked snapshots may use either package-root or actor-qualified
        // paths for the same inline object. Resolve root aliases by class first,
        // so a same-named authored actor reference is never rewritten with it.
        for (const auto& reference : classes)
        {
            const auto alias = aliases.find(reference.first);
            if (alias == aliases.end()) continue;
            for (const auto& referenceClass : reference.second)
            {
                const auto type = ClassLeaf(referenceClass);
                const auto owners = alias->second.find(type);
                if (owners == alias->second.end())
                {
                    if (type == "spriteemitter" || type == "meshemitter")
                    {
                        error = "The cooked particle reference class does not match its inline definition: " + reference.first;
                        return false;
                    }
                    continue;
                }
                if (owners->second.size() != 1)
                {
                    error = "The cooked particle reference has more than one exported owner: " + reference.first;
                    return false;
                }
                for (auto& actor : actors)
                    RewritePath(actor, reference.first, *owners->second.begin(), false, type);
            }
        }
        for (const auto& component : components)
        {
            const auto name = ReserveGeneratedName(component.name, used);
            auto& lines = actors[component.actor].lines;
            if (name != component.name)
            {
                SetNameAttribute(lines[component.line], name);
                std::size_t depth = 1;
                for (std::size_t i = component.line + 1; i < lines.size() && depth; ++i)
                {
                    std::string verb, kind;
                    if (Boundary(Trim(lines[i]), verb, kind)) depth += verb == "begin" ? 1 : -1;
                    else if (depth == 1)
                    {
                        const auto equals = lines[i].find('=');
                        if (equals != std::string::npos && Lower(Trim(std::string_view(lines[i]).substr(0, equals))) == "name")
                            lines[i].replace(equals + 1, std::string::npos, "\"" + name + "\"");
                    }
                }
            }
            for (auto& actor : actors)
                RewritePath(actor, component.path, "MyLevel." + name, false, component.type);
        }
        return true;
    }

    std::string ReserveGeneratedName(std::string_view original, std::set<std::string>& used)
    {
        std::string candidate(original);
        for (std::size_t suffix = 1; !used.insert(Lower(candidate)).second; ++suffix)
            candidate = std::string(original) + "_Recovery" + std::to_string(suffix);
        return candidate;
    }

    void RenameGeneratedBrush(Actor& actor, std::set<std::string>& used)
    {
        const auto oldActorName = actor.name;
        actor.name = ReserveGeneratedName(actor.name, used);
        if (actor.name != oldActorName)
        {
            SetNameAttribute(actor.lines.front(), actor.name);
            RewritePath(actor, "MyLevel." + oldActorName, "MyLevel." + actor.name, true);
        }
        for (auto& line : actor.lines)
        {
            std::string verb, kind;
            if (!Boundary(Trim(line), verb, kind) || verb != "begin" || kind != "brush") continue;
            const auto oldModelName = Attribute(line, "name");
            if (oldModelName.empty()) continue;
            const auto modelName = ReserveGeneratedName(oldModelName, used);
            if (modelName == oldModelName) continue;
            SetNameAttribute(line, modelName);
            RewritePath(actor, "MyLevel." + oldModelName, "MyLevel." + modelName, true);
            RewritePath(actor, "MyLevel." + actor.name + "." + oldModelName,
                        "MyLevel." + actor.name + "." + modelName, true);
        }
    }

    bool KnownAssetClass(std::string_view className)
    {
        static const std::set<std::string> assets = {
            "staticmesh", "mesh", "lodmesh", "skeletalmesh", "meshanimation",
            "texture", "bitmapmaterial", "material", "shader", "finalblend", "combiner",
            "cubemap", "texmodifier", "texcoordsource", "texenvmap", "texoscillator",
            "texpanner", "texrotator", "texscaler", "texmatrix", "texpantriggered",
            "constantcolor", "constantmaterial", "vertexcolor", "fadingcolor",
            "materialswitch", "materialsequence", "materialfade", "particlematerial",
            "fluidtexture", "firetexture", "wettexture", "wavetexture", "icetexture",
            "sound", "music", "font",
            // AntiPortalActor and mesh actors can share map-local occlusion
            // volumes. Native T3D exports their reference, not their geometry;
            // preserve the complete native object in the asset dependency.
            "convexvolume",
            // ZoneInfo.ZoneEffect instances are authored audio environment
            // settings, stored as package-root objects in the compiled map.
            // One class per I3DL2 reverb preset, so the whole standard set is
            // here rather than the presets that happened to turn up first: a
            // map reverberating like a city or a room is as ordinary as one
            // that sounds like a hangar.
            "i3dl2listener", "effect_generic", "effect_paddedcell", "effect_room",
            "effect_bathroom", "effect_livingroom", "effect_stoneroom", "effect_auditorium",
            "effect_concerthall", "effect_cave", "effect_arena", "effect_hangar",
            "effect_carpetedhallway", "effect_hallway", "effect_stonecorridor", "effect_alley",
            "effect_forest", "effect_city", "effect_mountains", "effect_quarry",
            "effect_plain", "effect_parkinglot", "effect_sewerpipe", "effect_underwater",
            "effect_drugged", "effect_dizzy", "effect_psychotic"
        };
        return assets.count(ClassLeaf(className)) != 0;
    }

    bool ExternalizeAssets(std::vector<Actor>& actors, std::string_view package,
                           std::vector<RecoveredActorImport::ExternalizedAsset>& externalized,
                           std::string& error)
    {
        if (package.empty()) return true;
        if (!std::isalpha(static_cast<unsigned char>(package.front())) && package.front() != '_')
        {
            error = "The recovery asset dependency has an invalid package name.";
            return false;
        }
        for (char c : package)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            {
                error = "The recovery asset dependency has an invalid package name.";
                return false;
            }
        std::set<std::string> definitions, missing;
        ReferenceClasses classes;
        for (const auto& actor : actors) Definitions(actor, definitions);
        for (const auto& actor : actors) References(actor, definitions, missing, &classes);
        for (const auto& path : missing)
        {
            const auto found = classes.find(Lower(path));
            if (found == classes.end() || found->second.empty()
                || !std::all_of(found->second.begin(), found->second.end(), KnownAssetClass))
                continue;
            RecoveredActorImport::ExternalizedAsset asset;
            asset.className = *found->second.begin();
            asset.originalPath = path;
            asset.externalPath = std::string(package) + path.substr(7);
            for (auto& actor : actors)
                RewritePath(actor, path, asset.externalPath);
            externalized.push_back(std::move(asset));
        }
        return true;
    }

    std::map<std::string, std::string> RootProperties(const Actor& actor)
    {
        std::map<std::string, std::string> properties;
        std::size_t depth = 0;
        for (const auto& line : actor.lines)
        {
            std::string verb, kind;
            if (Boundary(Trim(line), verb, kind))
            {
                if (verb == "begin") ++depth;
                else --depth;
                continue;
            }
            const auto equals = line.find('=');
            if (depth == 1 && equals != std::string::npos)
                properties[Lower(Trim(std::string_view(line).substr(0, equals)))] =
                    Trim(std::string_view(line).substr(equals + 1));
        }
        return properties;
    }

    struct ComparisonToken
    {
        std::string text;
        double number = 0;
        bool numeric = false;
    };

    std::vector<Actor> InlineParticleObjects(const Actor& actor)
    {
        std::vector<Actor> result;
        for (std::size_t i = 0; i < actor.lines.size(); ++i)
        {
            std::string verb, kind;
            if (!Boundary(Trim(actor.lines[i]), verb, kind) || verb != "begin" || kind != "object") continue;
            const auto type = Attribute(actor.lines[i], "class");
            if (ClassLeaf(type) != "spriteemitter" && ClassLeaf(type) != "meshemitter") continue;
            Actor particle;
            particle.name = Attribute(actor.lines[i], "name");
            particle.className = type;
            std::size_t depth = 0;
            for (std::size_t at = i; at < actor.lines.size(); ++at)
            {
                particle.lines.push_back(actor.lines[at]);
                if (Boundary(Trim(actor.lines[at]), verb, kind))
                {
                    if (verb == "begin") ++depth;
                    else if (--depth == 0) break;
                }
            }
            result.push_back(std::move(particle));
        }
        return result;
    }

    std::vector<ComparisonToken> ComparisonTokens(std::string_view text)
    {
        std::vector<ComparisonToken> tokens;
        for (std::size_t i = 0; i < text.size();)
        {
            const auto byte = static_cast<unsigned char>(text[i]);
            if (std::isspace(byte)) { ++i; continue; }
            const auto begin = i;
            if (text[i] == '"')
            {
                bool escaped = false;
                ++i;
                while (i < text.size())
                {
                    const char current = text[i++];
                    if (current == '"' && !escaped) break;
                    escaped = current == '\\' && !escaped;
                }
                tokens.push_back({std::string(text.substr(begin, i - begin)), 0, false});
            }
            else if (text[i] == '\'')
            {
                const auto end = text.find('\'', i + 1);
                if (end == std::string_view::npos)
                    tokens.push_back({std::string(1, text[i++]), 0, false});
                else
                {
                    const auto path = ObjectReferencePath(text.substr(i + 1, end - i - 1));
                    tokens.push_back({"'" + Lower(path) + "'", 0, false});
                    i = end + 1;
                }
            }
            else if (std::isdigit(byte) || ((text[i] == '+' || text[i] == '-')
                     && i + 1 < text.size()
                     && (std::isdigit(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '.')))
            {
                const std::string tail(text.substr(i));
                char* end = nullptr;
                const double number = std::strtod(tail.c_str(), &end);
                if (end != tail.c_str() && std::isfinite(number))
                {
                    i += static_cast<std::size_t>(end - tail.c_str());
                    tokens.push_back({{}, number, true});
                }
                else
                    tokens.push_back({std::string(1, text[i++]), 0, false});
            }
            else
            {
                if (std::isalnum(byte) || text[i] == '_')
                    while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i]))
                                               || text[i] == '_')) ++i;
                else
                    ++i;
                tokens.push_back({Lower(text.substr(begin, i - begin)), 0, false});
            }
        }
        return tokens;
    }

    std::vector<ComparisonToken> NormalizeNullStructFields(const std::vector<ComparisonToken>& tokens)
    {
        std::vector<ComparisonToken> result;
        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            result.push_back(tokens[i]);
            if (tokens[i].text != "(") continue;
            std::size_t end = i + 1, depth = 1;
            for (; end < tokens.size() && depth; ++end)
            {
                if (tokens[end].text == "(") ++depth;
                else if (tokens[end].text == ")") --depth;
            }
            if (depth) continue;
            const auto inner = NormalizeNullStructFields({tokens.begin() + i + 1, tokens.begin() + end - 1});
            std::size_t begin = 0;
            depth = 0;
            bool first = true;
            for (std::size_t at = 0; at <= inner.size(); ++at)
            {
                if (at < inner.size())
                {
                    if (inner[at].text == "(") ++depth;
                    else if (inner[at].text == ")") --depth;
                }
                if (at != inner.size() && (depth || inner[at].text != ",")) continue;
                // Native struct export omits default-null object fields. Only
                // discard a complete Field=None assignment, never another
                // value, an unnamed array item, or the authored string "None".
                const bool nullField = at - begin == 3 && !inner[begin].numeric
                    && !inner[begin].text.empty()
                    && (std::isalpha(static_cast<unsigned char>(inner[begin].text.front()))
                        || inner[begin].text.front() == '_')
                    && inner[begin + 1].text == "=" && inner[begin + 2].text == "none";
                if (at != begin && !nullField)
                {
                    if (!first) result.push_back({",", 0, false});
                    result.insert(result.end(), inner.begin() + begin, inner.begin() + at);
                    first = false;
                }
                begin = at + 1;
            }
            result.push_back(tokens[end - 1]);
            i = end - 1;
        }
        return result;
    }

    bool SameProperty(std::string_view expected, std::string_view actual)
    {
        const auto left = NormalizeNullStructFields(ComparisonTokens(expected));
        const auto right = NormalizeNullStructFields(ComparisonTokens(actual));
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            if (left[i].numeric != right[i].numeric) return false;
            if (left[i].numeric)
            {
                if (std::abs(left[i].number - right[i].number) > 0.001) return false;
            }
            else if (left[i].text != right[i].text)
                return false;
        }
        return true;
    }

    // Native export writes a transform struct when a component differs from the
    // Actor default by less than its six-decimal precision (USDOJO
    // StaticMeshActor470 DrawScale3D=(Z=1.000000)). Text import stores the
    // printed default, so the re-export omits the property. Accept that only
    // when every recorded component equals the Actor default.
    bool DefaultTransform(const std::string& property, std::string_view value)
    {
        const double fallback = property == "drawscale3d" ? 1
            : property == "location" || property == "prepivot" || property == "rotation" ? 0 : -1;
        if (fallback < 0) return false;
        const auto tokens = ComparisonTokens(value);
        if (tokens.size() < 5 || tokens.front().text != "(" || tokens.back().text != ")") return false;
        for (std::size_t i = 1; i + 1 < tokens.size(); i += 4)
        {
            if (i + 3 >= tokens.size() || tokens[i].numeric || tokens[i + 1].text != "="
                || !tokens[i + 2].numeric || std::abs(tokens[i + 2].number - fallback) > 0.001
                || (tokens[i + 3].text != "," && i + 3 != tokens.size() - 1)) return false;
        }
        return true;
    }

    std::vector<std::string> GameplayEdges(const std::vector<Actor>& records)
    {
        std::vector<std::string> result;
        for (const auto& record : records)
            if (record.className == "$ge")
                for (const auto& line : record.lines)
                {
                    auto text = Trim(line);
                    if (Lower(Token(text)) == "ge" && Lower(Token(text)) == "add")
                        result.push_back(line);
                }
        return result;
    }
}

bool RecoveredActorImport::Prepare(std::string_view exportedMap, PreparedMap& prepared,
                                   std::string& error, std::string_view externalAssetPackage,
                                   const std::vector<std::string>& confirmedDeletedActorPaths,
                                   const std::vector<std::string>& confirmedPcActorPaths,
                                   bool keepXboxOnlyActors)
{
    prepared = {};
    error.clear();
    std::vector<Actor> parsed, kept;
    if (!Parse(exportedMap, parsed, error))
        return false;
    if (parsed.empty() || ClassLeaf(parsed.front().className) != "levelinfo")
    {
        error = "The actor snapshot must start with its LevelInfo actor.";
        return false;
    }
    std::set<std::string> skippedXboxActors, deletedActors;
    std::set<std::string> pcActors;
    for (const auto& path : confirmedPcActorPaths)
    {
        const auto key = Lower(path);
        if (key.size() <= 8 || key.compare(0, 8, "mylevel.") != 0)
        {
            error = "Confirmed PC actor paths must name objects in MyLevel: " + path;
            return false;
        }
        pcActors.insert(key);
    }
    for (const auto& path : confirmedDeletedActorPaths)
    {
        const auto key = Lower(path);
        if (key.size() <= 8 || key.compare(0, 8, "mylevel.") != 0)
        {
            error = "Confirmed deleted actor paths must name objects in MyLevel: " + path;
            return false;
        }
        deletedActors.insert(key);
        if (pcActors.count(key))
        {
            error = "An actor cannot be both confirmed PC content and deleted: " + path;
            return false;
        }
    }
    // Removes only the root platform property, leaving nested components
    // intact: the native default is common content, included in PC saves.
    auto makeCommon = [](Actor& common)
    {
        std::size_t depth = 0;
        for (auto& line : common.lines)
        {
            std::string verb, kind;
            if (Boundary(Trim(line), verb, kind))
            {
                if (verb == "begin") ++depth; else --depth;
                continue;
            }
            const auto equals = line.find('=');
            if (depth == 1 && equals != std::string::npos
                && Lower(Trim(std::string_view(line).substr(0, equals))) == "platform")
                line.clear();
        }
    };
    for (auto& actor : parsed)
    {
        if (!actor.className.empty() && actor.className.front() != '$'
            && deletedActors.count(Lower("MyLevel." + actor.name)))
        {
            error = "An actor marked deleted is present in the exported actor snapshot: " + actor.name;
            return false;
        }
        const auto className = ClassLeaf(actor.className);
        if (className == "brush")
        {
            ++prepared.removedBrushCount;
            continue;
        }
        if (className == "levelinfo" && !kept.empty())
        {
            error = "The actor snapshot contains more than one LevelInfo actor.";
            return false;
        }
        const auto properties = RootProperties(actor);
        const auto platform = properties.find("platform");
        if (platform != properties.end() && Lower(Trim(platform->second)) == "plf_xbox_only"
            && pcActors.count(Lower("MyLevel." + actor.name)))
        {
            // This object is actually in the compiled PC level.
            makeCommon(actor);
            ++prepared.correctedPcActorPlatformCount;
        }
        else if (platform != properties.end() && Lower(Trim(platform->second)) == "plf_xbox_only")
        {
            if (className == "levelinfo")
            {
                error = "The compiled map's LevelInfo is Xbox-only and cannot define a PC source map.";
                return false;
            }
            if (keepXboxOnlyActors)
            {
                // Asked for: the Xbox build's own content becomes common.
                makeCommon(actor);
                ++prepared.keptXboxActorCount;
                prepared.keptXboxActorNames.push_back(actor.name);
                kept.push_back(Sanitize(std::move(actor), prepared.removedRuntimePropertyCount));
                continue;
            }
            skippedXboxActors.insert(Lower("MyLevel." + actor.name));
            ++prepared.skippedXboxActorCount;
            prepared.skippedXboxActorNames.push_back(actor.name);
            continue;
        }
        kept.push_back(Sanitize(std::move(actor), prepared.removedRuntimePropertyCount));
    }
    for (auto& actor : kept)
    {
        const auto clearedBefore = prepared.clearedXboxActorReferenceCount + prepared.clearedDeletedActorReferenceCount;
        ClearKnownActorReferences(actor, skippedXboxActors, prepared.clearedXboxActorReferenceCount);
        ClearKnownActorReferences(actor, deletedActors, prepared.clearedDeletedActorReferenceCount);
        if (prepared.clearedXboxActorReferenceCount + prepared.clearedDeletedActorReferenceCount != clearedBefore)
            prepared.clearedReferenceActorNames.push_back(actor.name);
        if (ClassLeaf(actor.className) == "esbstripdooractor"
            || ClassLeaf(actor.className) == "esbpatchactor")
        {
            std::size_t depth = 0;
            for (auto& line : actor.lines)
            {
                std::string verb, kind;
                if (Boundary(Trim(line), verb, kind))
                {
                    if (verb == "begin") ++depth; else --depth;
                    continue;
                }
                const auto equals = line.find('=');
                if (depth != 1 || equals == std::string::npos
                    || Lower(Trim(std::string_view(line).substr(0, equals))) != "softbody") continue;
                const auto value = Trim(std::string_view(line).substr(equals + 1));
                const auto quote = value.find('\'');
                if (quote == std::string_view::npos || value.back() != '\''
                    || ClassLeaf(value.substr(0, quote)) !=
                        (ClassLeaf(actor.className) == "esbpatchactor" ? "esbpatch" : "esbstripdoor")) continue;
                const auto path = ObjectReferencePath(value.substr(quote + 1, value.size() - quote - 2));
                if (Lower(path).compare(0, 16, "mylevel.mylevel.") != 0) continue;
                // Native PostEditChange regenerates a null simulation even
                // when all previous-value caches match. Keep every authored
                // setting; the native caller verifies springs and anchors.
                line = "    SoftBody=None";
                prepared.regeneratedSoftBodies.push_back(actor.name);
            }
        }
    }
    if (!NormalizeInlineEmitterReferences(kept, error)
        || !ExternalizeAssets(kept, externalAssetPackage, prepared.externalizedAssets, error))
        return false;
    for (auto& actor : kept) QuoteImportReferences(actor);
    prepared.levelInfoName = kept.front().name;
    prepared.levelInfoActor = Text(kept.front());
    prepared.actorsT3d = "Begin Map\n";
    prepared.mapSectionsT3d = "Begin Map\n";
    for (std::size_t i = 1; i < kept.size(); ++i)
    {
        if (kept[i].className.front() == '$')
            prepared.mapSectionsT3d += Text(kept[i]);
        else
        {
            prepared.actorsT3d += Text(kept[i]);
            ++prepared.actorCount;
        }
    }
    prepared.actorsT3d += "End Map\n";
    prepared.mapSectionsT3d += "End Map\n";
    return ValidateReferences(kept, prepared.unsupportedReferences, error);
}

bool RecoveredActorImport::ComposeSourceMap(const PreparedMap& prepared,
                                            std::string_view freshMap,
                                            std::string_view geometryMap,
                                            std::string& completeMap, std::string& error)
{
    completeMap.clear();
    error.clear();
    std::vector<Actor> fresh, recovered, geometry, levelInfo, sections;
    if (!Parse(freshMap, fresh, error) || !Parse(prepared.actorsT3d, recovered, error)
        || !Parse(geometryMap, geometry, error)
        || !Parse(prepared.mapSectionsT3d, sections, error)
        || !Parse("Begin Map\n" + prepared.levelInfoActor + "End Map\n", levelInfo, error))
        return false;
    fresh.erase(std::remove_if(fresh.begin(), fresh.end(), [](const Actor& actor) {
        return !actor.className.empty() && actor.className.front() == '$';
    }), fresh.end());
    if (fresh.size() != 2 || ClassLeaf(fresh[0].className) != "levelinfo"
        || ClassLeaf(fresh[1].className) != "brush" || levelInfo.size() != 1
        || ClassLeaf(levelInfo[0].className) != "levelinfo")
    {
        error = "The fresh normal map must contain exactly LevelInfo and its builder brush.";
        return false;
    }
    bool builderModel = false;
    for (const auto& line : fresh[1].lines)
    {
        std::string verb, kind;
        if (Boundary(Trim(line), verb, kind) && verb == "begin" && kind == "brush")
            builderModel = true;
    }
    if (!builderModel)
    {
        error = "The fresh builder brush has no exported source model.";
        return false;
    }
    for (const auto& actor : geometry)
        if (ClassLeaf(actor.className) != "brush")
        {
            error = "Reconstructed geometry must contain only Brush actors.";
            return false;
        }
    std::vector<Actor> combined;
    Actor settings = levelInfo.front();
    // The native source saver expects MyLevel.LevelInfo0. MAP NEW can emit an
    // incremented transient name, which must not become the saved identity.
    settings.name = "LevelInfo0";
    settings.className = fresh[0].className;
    settings.lines.front() = "Begin Actor Class=" + settings.className + " Name=" + settings.name;
    std::set<std::string> usedNames;
    for (const auto& actor : recovered) Definitions(actor, usedNames);
    if (usedNames.count(Lower(settings.name)))
    {
        error = "An authored map object conflicts with the required LevelInfo0 identity.";
        return false;
    }
    Definitions(settings, usedNames);
    combined.push_back(std::move(settings));
    std::size_t removed = 0;
    RewriteLevelInfo(fresh[1], fresh[0].name, combined.front().name);
    // Actor and brush-model names share MyLevel's object namespace. Preserve
    // authored identities and rename only the newly generated source objects.
    RenameGeneratedBrush(fresh[1], usedNames);
    combined.push_back(Sanitize(std::move(fresh[1]), removed));
    for (auto& actor : recovered)
        combined.push_back(std::move(actor));
    for (auto& actor : geometry)
    {
        RenameGeneratedBrush(actor, usedNames);
        combined.push_back(std::move(actor));
    }
    for (auto& section : sections)
        combined.push_back(std::move(section));
    std::set<std::string> names;
    for (auto& actor : combined)
    {
        if (actor.className.front() != '$' && !names.insert(Lower(actor.name)).second)
        {
            error = "The new map has an actor-name collision: " + actor.name;
            return false;
        }
        RewriteLevelInfo(actor, prepared.levelInfoName, combined.front().name);
    }
    std::vector<std::string> missing;
    if (!ValidateReferences(combined, missing, error))
        return false;
    completeMap = "Begin Map\n";
    for (const auto& actor : combined)
        completeMap += Text(actor);
    completeMap += "End Map\n";
    return true;
}

bool RecoveredActorImport::VerifySourceMap(const PreparedMap& prepared,
                                           std::string_view actualMap, std::string& error)
{
    error.clear();
    std::vector<Actor> actual, expected, levelInfo, sections;
    if (!Parse(actualMap, actual, error) || !Parse(prepared.actorsT3d, expected, error)
        || !Parse(prepared.mapSectionsT3d, sections, error)
        || !Parse("Begin Map\n" + prepared.levelInfoActor + "End Map\n", levelInfo, error))
        return false;
    std::map<std::string, const Actor*> byName;
    const Actor* actualLevel = nullptr;
    for (const auto& actor : actual)
    {
        if (actor.className.front() == '$') continue;
        byName.emplace(Lower(actor.name), &actor);
        if (ClassLeaf(actor.className) == "levelinfo")
        {
            if (actualLevel)
            {
                error = "The imported source contains multiple LevelInfo actors.";
                return false;
            }
            actualLevel = &actor;
        }
    }
    if (!actualLevel || levelInfo.size() != 1)
    {
        error = "The imported source did not retain a valid LevelInfo actor.";
        return false;
    }
    static const std::set<std::string> authoredActorProperties = {
        "location", "rotation", "drawscale", "drawscale3d", "prepivot", "event", "tag", "m_platform", "platform"
    };
    for (auto& actor : expected)
    {
        const auto found = byName.find(Lower(actor.name));
        if (found == byName.end()
            || ClassLeaf(found->second->className) != ClassLeaf(actor.className))
        {
            error = "The imported source is missing or changed actor " + actor.name
                + " (" + actor.className + ").";
            return false;
        }
        RewriteLevelInfo(actor, prepared.levelInfoName, actualLevel->name);
        const auto originalProperties = RootProperties(actor);
        const auto importedProperties = RootProperties(*found->second);
        const bool regenerated = std::any_of(prepared.regeneratedSoftBodies.begin(),
            prepared.regeneratedSoftBodies.end(), [&](const auto& name) { return Lower(name) == Lower(actor.name); });
        if (regenerated)
        {
            const auto body = importedProperties.find("softbody");
            if (body == importedProperties.end()
                || Lower(body->second).find(ClassLeaf(actor.className) == "esbpatchactor"
                    ? "esbpatch'mylevel.mylevel." : "esbstripdoor'mylevel.mylevel.") != 0
                || body->second.back() != '\'')
            {
                error = "The imported source did not regenerate the procedural soft-body simulation on actor " + actor.name + ".";
                return false;
            }
        }
        for (const auto& property : originalProperties)
        {
            if (regenerated && (property.first == "softbody" || property.first == "name")) continue;
            // Confirm authored asset/actor bindings as well as actor presence.
            // Runtime-only instance/region bindings were removed by Prepare.
            const bool reference = property.second.find('\'') != std::string::npos
                && (property.second.empty() || property.second.front() != '"');
            const bool emptyReference = Lower(Trim(property.second)) == "none";
            const auto tokens = ComparisonTokens(property.second);
            const bool hasEmptyReference = std::any_of(tokens.begin(), tokens.end(), [](const auto& token) {
                return !token.numeric && token.text == "none";
            });
            if (!regenerated && !reference && !hasEmptyReference && !authoredActorProperties.count(property.first)) continue;
            const auto imported = importedProperties.find(property.first);
            // Native export omits default null objects, including an entire
            // struct/array element whose only assignments are null objects
            // (HELI02 SAlarm.MoversToLock). Keep checking every non-null field.
            if (imported == importedProperties.end()
                && (emptyReference || (hasEmptyReference && SameProperty(property.second,"()"))
                    || DefaultTransform(property.first, property.second))) continue;
            if (imported == importedProperties.end() || !SameProperty(property.second, imported->second))
            {
                error = "The imported source changed the " + property.first
                    + " property on actor " + actor.name + ".";
                return false;
            }
        }
        const auto originalParticles = InlineParticleObjects(actor);
        const auto importedParticles = InlineParticleObjects(*found->second);
        for (const auto& particle : originalParticles)
        {
            const auto match = std::find_if(importedParticles.begin(), importedParticles.end(), [&](const auto& imported) {
                return Lower(particle.name) == Lower(imported.name)
                    && ClassLeaf(particle.className) == ClassLeaf(imported.className);
            });
            if (match == importedParticles.end())
            {
                error = "The imported source is missing inline particle " + particle.name + " on actor " + actor.name + ".";
                return false;
            }
            const auto originalParticleProperties = RootProperties(particle);
            const auto importedParticleProperties = RootProperties(*match);
            for (const auto& property : originalParticleProperties)
            {
                if (property.first == "name") continue;
                const auto imported = importedParticleProperties.find(property.first);
                const auto value = ComparisonTokens(property.second);
                const bool emptyReference = value.size() == 1 && value.front().text == "none";
                // Native ParticleEmitter defaults SizeScaleRepeats to zero;
                // export omits that field after text import rounds cooked tiny
                // floating-point residues printed as 0.000000 down to zero.
                const bool zeroScaleRepeat = property.first == "sizescalerepeats"
                    && value.size() == 1 && value.front().numeric && value.front().number == 0;
                if (imported == importedParticleProperties.end() && (emptyReference || zeroScaleRepeat)) continue;
                if (imported == importedParticleProperties.end() || !SameProperty(property.second, imported->second))
                {
                    error = "The imported source changed the " + property.first + " property on inline particle "
                        + particle.name + " of actor " + actor.name + ".";
                    return false;
                }
            }
        }
    }
    static const std::set<std::string> authoredLevelProperties = {
        "title", "author", "levelentertext", "idealplayercount", "recommendednumplayers", "mapinfo",
        "defaultgametype", "song", "songsection", "cdtrack", "playerdoppler"
    };
    const auto originalSettings = RootProperties(levelInfo.front());
    const auto importedSettings = RootProperties(*actualLevel);
    for (const auto& property : originalSettings)
    {
        if (!authoredLevelProperties.count(property.first)) continue;
        const auto imported = importedSettings.find(property.first);
        if (imported == importedSettings.end() || !SameProperty(property.second, imported->second))
        {
            error = "The imported source did not retain the LevelInfo " + property.first + " setting.";
            return false;
        }
    }
    const auto originalEdges = GameplayEdges(sections);
    const auto importedEdges = GameplayEdges(actual);
    if (originalEdges.size() != importedEdges.size())
    {
        error = "The imported source retained " + std::to_string(importedEdges.size()) + " of "
            + std::to_string(originalEdges.size()) + " gameplay ledge/pipe definitions.";
        return false;
    }
    std::vector<bool> matched(importedEdges.size(), false);
    for (const auto& edge : originalEdges)
    {
        std::size_t i = 0;
        for (; i < importedEdges.size(); ++i)
            if (!matched[i] && SameProperty(edge, importedEdges[i]))
            {
                matched[i] = true;
                break;
            }
        if (i == importedEdges.size())
        {
            error = "The imported source changed a gameplay ledge/pipe definition.";
            return false;
        }
    }
    return true;
}
