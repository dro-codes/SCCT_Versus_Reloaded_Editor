#pragma once
// map.recover's pure part (McpLifecycle.cpp runs it): the files a request
// names, what a recovery writes and so must not find there already, the
// imported packages no installed file provides, the progress messages worth
// sending, and the replies the agent reads (the Recovery Report's counts and
// rows). Paths are UTF-8 text with either slash and come back with
// backslashes; nothing here touches a file, so the tests compile it alone.
#include "McpBridgeModel.h"
#include "MapRecoveryModel.h"
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Mcp::Recover
{
    using Json = Workflow::Json;

    inline std::string Lower(std::string text)
    {
        for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return text;
    }
    inline bool Separator(char c) { return c == '\\' || c == '/'; }
    // A drive ("C:\"), a share ("\\server") or a leading slash.
    inline bool Absolute(const std::string& path)
    {
        return (path.size() >= 3 && path[1] == ':' && Separator(path[2])) || (!path.empty() && Separator(path[0]));
    }
    inline std::vector<std::string> Parts(const std::string& path)
    {
        std::vector<std::string> parts;
        std::string part;
        for (char c : path)
        {
            if (Separator(c)) { if (!part.empty()) parts.push_back(part); part.clear(); }
            else part += c;
        }
        if (!part.empty()) parts.push_back(part);
        return parts;
    }
    // Backslashes, no empty, "." or ".." parts: ".." takes back the part
    // before it, and never climbs above a drive or share.
    inline std::string Normal(const std::string& path)
    {
        std::string prefix;
        if (path.size() >= 2 && Separator(path[0]) && Separator(path[1])) prefix = "\\\\";
        else if (!path.empty() && Separator(path[0])) prefix = "\\";
        const auto parts = Parts(path);
        // A drive ("C:") or a share's server: what ".." stops at.
        const bool drive = !parts.empty() && parts[0].size() == 2 && parts[0][1] == ':';
        const size_t floor = drive || (prefix == "\\\\" && !parts.empty()) ? 1 : 0;
        std::vector<std::string> kept;
        for (const auto& part : parts)
        {
            if (part == ".") continue;
            if (part == "..")
            {
                if (kept.size() > floor && kept.back() != "..") kept.pop_back();
                else if (prefix.empty() && !drive) kept.push_back(part);
                continue;
            }
            kept.push_back(part);
        }
        std::string out = prefix;
        for (size_t i = 0; i < kept.size(); ++i) out += (i ? "\\" : "") + kept[i];
        if (kept.size() == 1 && kept[0].size() == 2 && kept[0][1] == ':') out += "\\";
        return out;
    }
    inline std::string Join(const std::string& folder, const std::string& name) { return Normal(folder + "\\" + name); }
    inline std::string Leaf(const std::string& path)
    {
        const auto parts = Parts(path);
        return parts.empty() ? std::string() : parts.back();
    }
    inline std::string Parent(const std::string& path)
    {
        const auto normal = Normal(path);
        const auto at = normal.find_last_of('\\');
        return at == std::string::npos ? std::string() : normal.substr(0, at);
    }
    // ".sdc" of "OffsD.sdc"; "" when the name has no dot after its first character.
    inline std::string Extension(const std::string& path)
    {
        const auto leaf = Leaf(path);
        const auto dot = leaf.find_last_of('.');
        return dot == std::string::npos || dot == 0 ? std::string() : leaf.substr(dot);
    }
    inline std::string Stem(const std::string& path)
    {
        const auto leaf = Leaf(path);
        return leaf.substr(0, leaf.size() - Extension(leaf).size());
    }
    // Compared without case, as Windows compares names.
    inline bool SamePath(const std::string& a, const std::string& b) { return Lower(Normal(a)) == Lower(Normal(b)); }
    inline bool DirectlyIn(const std::string& path, const std::string& folder) { return SamePath(Parent(path), folder); }

    inline std::string Packages(const std::string& root, const char* folder) { return Join(Join(root, "Packages"), folder); }

    // A bare stem or file name ("OffsD", "OffsD.sdc"): no folder or drive.
    inline bool Bare(const std::string& text) { return text.find_first_of("\\/:") == std::string::npos; }

    // A map file a request names: a bare stem or file name ("OffsD",
    // "OffsD.sdc") is in Packages\<folder>; a path starting with Packages is
    // relative to the game folder; anything else must be a full path. what
    // names the argument in the error.
    inline std::string MapFile(const std::string& text, const std::string& root, const char* folder, const std::string& what)
    {
        const auto example = std::string(" (a name such as \"OffsD\" for Packages\\") + folder + "\\OffsD.sdc, a path starting with Packages, or a full path).";
        if (text.empty() || text.size() > 400) throw Bridge::Error(Bridge::Code::Invalid, what + " names a map file" + example);
        for (unsigned char c : text)
            if (c < 0x20 || c == '"' || c == '*' || c == '?' || c == '<' || c == '>' || c == '|')
                throw Bridge::Error(Bridge::Code::Invalid, what + " has a character a file name cannot hold" + example);
        if (Bare(text))
        {
            if (text == "." || text == "..") throw Bridge::Error(Bridge::Code::Invalid, what + " names a map file" + example);
            return Join(Packages(root, folder), Extension(text).empty() ? text + ".sdc" : text);
        }
        if (Absolute(text)) return Normal(text);
        const auto parts = Parts(text);
        if (!parts.empty() && Lower(parts.front()) == "packages") return Join(root, text);
        throw Bridge::Error(Bridge::Code::Invalid, what + " \"" + text + "\" is neither a bare map name nor a full path" + example);
    }

    // Where the game keeps compiled (playable) maps and the editor its
    // editable source maps. Every save of a source map also writes a compiled
    // copy of it to Packages\Maps.
    inline bool CompiledMapPath(const std::string& path, const std::string& root)
    {
        return Lower(Extension(path)) == ".sdc" && DirectlyIn(path, Packages(root, "Maps"));
    }
    inline bool SourceMapPath(const std::string& path, const std::string& root) { return DirectlyIn(path, Packages(root, "MapsEd")); }

    // What a recovery into the map <stem> writes (MapRecovery.cpp
    // RecoverToSourceImpl): the source map, its compiled copy the save makes,
    // the recovery folder with Recovery.txt, and the package that receives
    // the assets the compiled map carried inside it.
    struct Outputs { std::string map, playable, folder, report, assets; };
    inline Outputs OutputsFor(const std::string& root, const std::string& stem)
    {
        Outputs out;
        out.map = Join(Packages(root, "MapsEd"), stem + ".sdc");
        out.playable = Join(Packages(root, "Maps"), stem + ".sdc");
        out.folder = Join(Join(Packages(root, "MapsEd"), "Recovery"), stem);
        out.report = Join(out.folder, "Recovery.txt");
        out.assets = Join(Packages(root, "StaticMeshes"), MapRecoveryModel::AssetPackageName(stem) + ".usx");
        return out;
    }
    // The ones that must not exist yet. Recovery itself refuses the map, its
    // compiled copy and the asset package; a recovery folder is the earlier
    // attempt a failed run keeps for inspection.
    inline std::vector<std::string> MustBeFree(const Outputs& out) { return {out.map, out.playable, out.folder, out.assets}; }
    // Whether saving the map <stem> would replace a compiled original: the
    // save also writes Packages\Maps\<stem>.sdc, and one there with no
    // Packages\MapsEd\<stem>.sdc beside it is a compiled-only map (one
    // downloaded, or a recovery's source) that no save could make again.
    inline bool ReplacesCompiledOriginal(const Outputs& out, const std::function<bool(const std::string&)>& exists)
    {
        return exists(out.playable) && !exists(out.map);
    }

    // A map file name the editor and map.save take: ASCII letters, digits and
    // underscores, at most 60 (McpLifecycle.cpp CheckSaveTarget).
    inline bool MapName(const std::string& stem)
    {
        return !stem.empty() && stem.size() <= 60 && stem.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::string::npos;
    }
    // A free destination stem, as File > Recover Compiled Map picks one:
    // <source>_Recovered, then _Recovered_2, _3 ... "" when none of the first
    // thousand is free. The source's part is cut so that every one of them
    // leaves the asset package a file name the editor can save.
    inline std::string FreeName(const std::string& sourceStem, const std::function<bool(const std::string&)>& taken)
    {
        std::string base;
        for (char c : sourceStem) base += MapName(std::string(1, c)) ? c : '_';
        if (base.empty()) base = "Map";
        while (base.size() > 1 && !MapRecoveryModel::AssetFileFits(base + "_Recovered_1000")) base.pop_back();
        for (int index = 1; index <= 1000; ++index)
        {
            const auto name = base + "_Recovered" + (index > 1 ? "_" + std::to_string(index) : std::string());
            if (!taken(name)) return name;
        }
        return {};
    }

    // Files the engine finds packages in by their name: scripts, textures
    // (and their compressed .utc), static meshes, sounds, animations, maps.
    inline bool PackageFile(const std::string& extension)
    {
        static const std::set<std::string> kinds{".u", ".utx", ".utc", ".usx", ".uax", ".ukx", ".sdc"};
        return kinds.count(Lower(extension)) != 0;
    }
    // The packages an import table names that nothing provides: not one the
    // game executable supplies (runtime), not the map itself, and not an
    // installed file (installed: their lowercase names). Loading the map
    // without one opens a dialog that waits for the user. Sorted, once each.
    inline std::vector<std::string> Missing(const std::vector<std::string>& imports, const std::set<std::string>& installed, const std::string& mapStem,
                                            const std::function<bool(const std::string&)>& runtime)
    {
        std::map<std::string, std::string> missing;
        for (const auto& name : imports)
        {
            const auto key = Lower(name);
            if (key.empty() || key == Lower(mapStem) || installed.count(key) || (runtime && runtime(name))) continue;
            missing.emplace(key, name);
        }
        std::vector<std::string> out;
        for (const auto& entry : missing) out.push_back(entry.second);
        return out;
    }

    // The request: map.recover {source, destination, merge, keepXboxOnly,
    // discard, legacy, maxRows}. mergeDefault is File > Merge Recovered
    // Geometry; legacy takes a source in Packages\MapsEd, as File > Convert
    // Legacy Recovered Map does (a map older RE+ builds saved in compiled
    // layout, or a compiled map copied there).
    struct Options
    {
        std::string source, destination;
        bool merge = true, keepXboxOnly = false, discard = false, legacy = false;
        int maxRows = 200;
    };
    inline Options ReadOptions(const Json& q, bool mergeDefault)
    {
        auto invalid = [](const std::string& message) { throw Bridge::Error(Bridge::Code::Invalid, "map.recover: " + message); };
        Options o;
        o.merge = mergeDefault;
        if (!q.is_object()) invalid("its arguments are an object.");
        for (const char* key : {"source", "destination"})
            if (!q.contains(key) || !q.at(key).is_string())
                invalid(std::string(key) + " is required text: source is the compiled map (\"OffsD\" for Packages\\Maps\\OffsD.sdc), destination (map_recover's name) the new map (\"OffsR\" for Packages\\MapsEd\\OffsR.sdc).");
        o.source = q.at("source").get<std::string>();
        o.destination = q.at("destination").get<std::string>();
        auto flag = [&](const char* key, bool& value)
        {
            if (!q.contains(key) || q.at(key).is_null()) return;
            if (!q.at(key).is_boolean()) invalid(std::string(key) + " is true or false.");
            value = q.at(key).get<bool>();
        };
        flag("merge", o.merge);
        flag("keepXboxOnly", o.keepXboxOnly);
        flag("discard", o.discard);
        flag("legacy", o.legacy);
        if (q.contains("maxRows") && !q.at("maxRows").is_null())
        {
            const auto& rows = q.at("maxRows");
            if (!rows.is_number_integer() || rows.get<long long>() < 0 || rows.get<long long>() > 2000) invalid("maxRows is a whole number from 0 to 2000.");
            o.maxRows = static_cast<int>(rows.get<long long>());
        }
        return o;
    }

    // The progress messages a recovery sends: one each time its stage or its
    // whole percentage changes, never more than about a hundred and a dozen.
    struct Throttle
    {
        int stage = -1, percent = -1;
        // The fields of a "recovering" progress message, or null when there is
        // nothing new to say.
        Json Next(MapRecoveryModel::Stage at, double fraction)
        {
            const int index = static_cast<int>(at), now = MapRecoveryModel::Percent(at, fraction);
            if (index == stage && now == percent) return Json();
            stage = index;
            percent = now;
            return {{"stage", MapRecoveryModel::Info(at).name}, {"stageIndex", index}, {"percent", now}};
        }
    };

    // Report text uses the window's CRLF; JSON wants plain line breaks.
    inline std::string Lines(std::string text)
    {
        for (size_t at = 0; (at = text.find("\r\n", at)) != std::string::npos;) text.erase(at, 1);
        return text;
    }
    inline Json CountsJson(const MapRecoveryModel::Counts& c)
    {
        return {{"merged", c.merged}, {"cells", c.cells}, {"cellFaces", c.cellFaces}, {"joinedBrushes", c.joinedBrushes}, {"brushes", c.brushes},
                {"brushFaces", c.brushFaces}, {"grown", c.grown}, {"absorbed", c.absorbed}, {"splitBack", c.splitBack}, {"sheetBrushes", c.sheetBrushes},
                {"polygons", c.polygons}, {"unifiedSurfaces", c.unifiedSurfaces}, {"actors", c.actors}};
    }
    // A report row; actor or surface when a click in the Recovery Report
    // would select one.
    inline Json RowJson(const MapRecoveryModel::ReportRow& row)
    {
        Json out{{"category", row.category}, {"item", row.item}, {"detail", row.detail}};
        if (row.target == MapRecoveryModel::Target::Actor && !row.actor.empty()) out["actor"] = row.actor;
        if (row.target == MapRecoveryModel::Target::Surface && row.surface >= 0) out["surface"] = row.surface;
        return out;
    }

    // What a finished recovery answers. The first maxRows rows come inline;
    // categories counts every row, and Recovery.txt (report) holds them all.
    struct Finished
    {
        std::string file, source, report, folder;
        bool merge = true, keepXboxOnly = false;
        double seconds = 0;
    };
    inline Json Result(const Finished& done, const MapRecoveryModel::Counts& counts, const std::vector<MapRecoveryModel::ReportRow>& rows, int maxRows)
    {
        Json categories = Json::object(), shown = Json::array();
        std::map<std::string, size_t> tally;
        for (const auto& row : rows) ++tally[row.category];
        for (const auto& [category, count] : tally) categories[category] = count;
        const size_t limit = maxRows < 0 ? 0 : static_cast<size_t>(maxRows);
        for (size_t i = 0; i < rows.size() && i < limit; ++i) shown.push_back(RowJson(rows[i]));
        return {{"ok", true}, {"file", done.file}, {"source", done.source}, {"merge", done.merge}, {"keepXboxOnly", done.keepXboxOnly},
                {"seconds", std::round(done.seconds * 10) / 10}, {"summary", Lines(MapRecoveryModel::Summary(counts))}, {"counts", CountsJson(counts)},
                {"categories", categories}, {"rows", shown}, {"rowsTotal", rows.size()}, {"report", done.report}, {"folder", done.folder}};
    }

    // What a dry run answers: the checks it made, and nothing changed.
    struct Checked
    {
        std::string source, destination;
        bool merge = true, keepXboxOnly = false;
        size_t imports = 0;
        unsigned long long sourceBytes = 0;
        std::vector<std::string> checks;
    };
    inline Json DryRun(const Checked& c)
    {
        return {{"dryRun", true}, {"source", c.source}, {"destination", c.destination}, {"merge", c.merge}, {"keepXboxOnly", c.keepXboxOnly},
                {"imports", c.imports}, {"missing", Json::array()}, {"sourceBytes", c.sourceBytes}, {"checks", c.checks},
                {"note", "Recovers " + Leaf(c.source) + " into " + Leaf(c.destination) + (c.merge ? " (merging geometry)" : " (no merging)") +
                             ". It starts with a new map, so the open map is replaced; nothing was changed by this dry run. Only the packages the map imports directly"
                             " were checked: one that a package of those needs and that is not installed can still stop the load at a dialog."}};
    }
}
