#pragma once
// File > Recover Compiled Map as the MCP tools present it (map_recover,
// map_query what:"recovery"): what each Recovery Report category means and
// what to do about it, the name the editor gives a recovered map, and
// Recovery.txt read back. The editor writes Recovery.txt in
// Packages\MapsEd\Recovery\<map>\ (MapRecovery.cpp, MapRecoveryModel.h): a
// block of "Name: value" lines and notes, a blank line, the counts summary
// (MapRecoveryModel::Summary, lines joined with CRLF), then "Nothing to
// report" or "Items that did not come back cleanly (N):" and one
// "category\titem\tdetail" line per item (MapRecoveryModel::RowsText).
// Portable: files are the caller's. Tests: tests/McpRecoverToolsTests.cpp.
#include "../Reloaded.Editor/WorkflowModel.h"
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace Mcp::Recovery
{
    using Json = Workflow::Json;

    // The report's categories (outcome.rows in MapRecovery.cpp). actor: the
    // item is the recovered actor's object name, which map_query and
    // view_screenshot's frame take.
    struct Category { const char* name; const char* meaning; const char* action; bool actor; };
    inline const std::vector<Category>& Categories()
    {
        static const std::vector<Category> table = {
            {"Actor left out", "An Xbox-only actor of the compiled map (PLF_XBOX_ONLY) was not recreated: the recovered map is for PC.",
             "Usually nothing. To keep them as common content, recover again under a new name with keepXboxOnly: true.", false},
            {"Xbox-only actor kept", "An Xbox-only actor of the compiled map, kept as common content (keepXboxOnly), so the PC game loads it too.",
             "Look at it (view_screenshot frame:[name]): the PC version never showed it, so check it fits and plays well.", true},
            {"Reference cleared", "A property pointed at an actor that was left out or had been deleted, so it is now None.",
             "Check the actor (map_query what:\"actor\", view_screenshot frame:[actor]) and point the property at a suitable actor again if gameplay needs it (actors_edit).", true},
            {"Asset moved", "A mesh or material embedded in the compiled map now lives in the map's own StaticMeshes\\<map>_Assets.usx.",
             "Nothing to fix: ship that package with the map (release action:\"check\" lists it).", false},
            {"Thin brush", "A structural brush under 1 unit across or smaller than a 4-unit cube: a sliver of the compiled BSP's splits. It builds, but is hard to select.",
             "Leave it unless you edit that area; view_screenshot frame:[name] shows it.", true},
            {"Mesh lighting recalculated", "A static mesh whose vertex layout changed, so its original baked colours could not be kept; it has freshly built lighting.",
             "Look at it (view_screenshot frame:[name]); it may be lit differently from the original.", true},
            {"BSP lighting not preserved", "A rebuilt BSP surface (item: its index in the saved map) where most lighting texels found no original lighting and kept the new bake.",
             "Compare it in the editor. To re-bake only such surfaces, the user selects them and runs Build > Recalculate Selected Lighting; Build > Recalculate Lighting would replace the whole preserved bake.", false},
        };
        return table;
    }
    inline std::string Folded(std::string text)
    {
        for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return text;
    }
    inline const Category* FindCategory(const std::string& name)
    {
        const auto folded = Folded(name);
        for (const auto& category : Categories()) if (Folded(category.name) == folded) return &category;
        return nullptr;
    }

    // ---------------------------------------------------------------- names
    // A map's file stem from a name or a path ("OffsD", "OffsD.sdc", "C:\...\OffsD.sdc").
    inline std::string Stem(std::string text)
    {
        const auto slash = text.find_last_of("\\/");
        if (slash != std::string::npos) text = text.substr(slash + 1);
        if (text.size() > 4 && Folded(text.substr(text.size() - 4)) == ".sdc") text.resize(text.size() - 4);
        return text;
    }
    // The package recovery moves embedded assets into, StaticMeshes\<this>.usx
    // (MapRecovery.cpp RecoveryAssetName).
    inline std::string AssetPackage(const std::string& destinationStem)
    {
        std::string name = destinationStem + "_Assets";
        for (auto& ch : name) if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') ch = '_';
        if (std::isdigit(static_cast<unsigned char>(name.front()))) name.insert(0, "Recovery_");
        return name;
    }
    // What a recovery into destinationStem writes, relative to Packages: the
    // editable map, its playable copy, the asset package and the scratch
    // folder (Recovery.txt and the intermediate files).
    inline std::vector<std::string> Outputs(const std::string& destinationStem)
    {
        return {"MapsEd\\" + destinationStem + ".sdc", "Maps\\" + destinationStem + ".sdc", "StaticMeshes\\" + AssetPackage(destinationStem) + ".usx",
                "MapsEd\\Recovery\\" + destinationStem};
    }
    inline std::string ReportFile(const std::string& destinationStem) { return "MapsEd\\Recovery\\" + destinationStem + "\\Recovery.txt"; }
    // The source's stem as a map name takes it: letters, digits and
    // underscores (anything else becomes '_', as the bridge's Recover::FreeName
    // does), "Map_" before a leading digit, and short enough that any
    // DefaultName stays within 40 characters, well inside the editor's limits
    // on the map name and its StaticMeshes asset package's path.
    inline std::string DefaultBase(const std::string& sourceStem)
    {
        std::string base;
        for (unsigned char ch : sourceStem) base += ch < 0x80 && (std::isalnum(ch) || ch == '_') ? static_cast<char>(ch) : '_';
        if (base.empty()) base = "Map";
        if (std::isdigit(static_cast<unsigned char>(base.front()))) base.insert(0, "Map_");
        return base.substr(0, 40 - std::string("_Recovered_10000").size());
    }
    // The name File > Recover Compiled Map gives its output: <source>_Recovered,
    // then _Recovered_2, _3, ... until none of its outputs exists
    // (MapRecovery.cpp UniqueRecoveryPath), from DefaultBase. exists is asked
    // about each Outputs() path; empty when every name up to _10000 is taken.
    inline std::string DefaultName(const std::string& sourceStem, const std::function<bool(const std::string& relative)>& exists)
    {
        const auto base = DefaultBase(sourceStem);
        for (unsigned index = 0; index < 10000; ++index)
        {
            const auto name = base + "_Recovered" + (index ? "_" + std::to_string(index + 1) : std::string());
            bool taken = false;
            if (exists)
                for (const auto& output : Outputs(name)) taken = taken || exists(output);
            if (!taken) return name;
        }
        return {};
    }

    // ----------------------------------------------------------------- rows
    // A row as the tools return it: {category, item, detail}, plus the actor
    // to frame (the categories that name one) or the BSP surface index.
    inline Json Row(const std::string& category, const std::string& item, const std::string& detail)
    {
        Json row{{"category", category}, {"item", item}, {"detail", detail}};
        const auto* known = FindCategory(category);
        if (known && known->actor && !item.empty()) row["actor"] = item;
        if (Folded(category) == "bsp lighting not preserved" && Folded(item).rfind("surface ", 0) == 0)
        {
            const auto digits = item.substr(8);
            if (!digits.empty() && digits.size() < 10 && digits.find_first_not_of("0123456789") == std::string::npos) row["surface"] = std::stoi(digits);
        }
        return row;
    }
    // [{category, count, meaning, action}] from {category: count}, in the
    // report's order, unknown categories last.
    inline Json Tally(const Json& counts)
    {
        Json out = Json::array();
        if (!counts.is_object()) return out;
        auto add = [&](const std::string& name, const Json& count, const Category* known)
        {
            Json entry{{"category", name}, {"count", count}};
            if (known) { entry["meaning"] = known->meaning; entry["action"] = known->action; }
            out.push_back(entry);
        };
        for (const auto& category : Categories())
            for (auto it = counts.begin(); it != counts.end(); ++it)
                if (Folded(it.key()) == Folded(category.name)) add(it.key(), it.value(), &category);
        for (auto it = counts.begin(); it != counts.end(); ++it)
            if (!FindCategory(it.key())) add(it.key(), it.value(), nullptr);
        return out;
    }
    inline Json Counts(const Json& rows)
    {
        Json counts = Json::object();
        for (const auto& row : rows)
        {
            const auto category = row.value("category", std::string());
            counts[category] = counts.value(category, 0) + 1;
        }
        return counts;
    }
    // "3 Thin brush, 1 Asset moved" for a summary line.
    inline std::string TallyLine(const Json& tally)
    {
        std::string out;
        for (const auto& entry : tally) out += (out.empty() ? "" : ", ") + entry.at("count").dump() + " " + entry.value("category", std::string());
        return out;
    }
    // The tally with meanings, and the rows, as text for the agent.
    inline std::string TallyText(const Json& tally)
    {
        std::string out;
        for (const auto& entry : tally)
        {
            out += "- " + entry.value("category", std::string()) + " (" + entry.at("count").dump() + ")";
            if (entry.contains("meaning")) out += ": " + entry.value("meaning", std::string()) + " To do: " + entry.value("action", std::string());
            out += "\n";
        }
        return out;
    }
    inline std::string RowsText(const Json& rows)
    {
        std::string out;
        for (const auto& row : rows)
        {
            out += "- [" + row.value("category", std::string()) + "] " + row.value("item", std::string());
            const auto detail = row.value("detail", std::string());
            if (!detail.empty()) out += ": " + detail;
            out += "\n";
        }
        return out;
    }

    // ----------------------------------------------------------- the report
    // Recovery.txt's paths are in the ANSI code page the editor wrote them in
    // (path::string()), which for a folder with an accented letter is not
    // UTF-8. Read back through filesystem::path, which takes a narrow string
    // in that code page on Windows; any byte still not UTF-8 becomes U+FFFD.
    inline size_t Utf8Length(const std::string& text, size_t at)
    {
        const auto lead = static_cast<unsigned char>(text[at]);
        const size_t length = lead < 0x80 ? 1 : lead >= 0xC2 && lead < 0xE0 ? 2 : lead >= 0xE0 && lead < 0xF0 ? 3 : lead >= 0xF0 && lead < 0xF5 ? 4 : 0;
        if (!length || at + length > text.size()) return 0;
        for (size_t i = 1; i < length; ++i)
            if ((static_cast<unsigned char>(text[at + i]) & 0xC0) != 0x80) return 0;
        return length;
    }
    inline bool ValidUtf8(const std::string& text)
    {
        for (size_t at = 0; at < text.size();)
        {
            const auto length = Utf8Length(text, at);
            if (!length) return false;
            at += length;
        }
        return true;
    }
    inline std::string Utf8Text(const std::string& text)
    {
        if (ValidUtf8(text)) return text;
        std::string from = text;
        try
        {
            const auto converted = std::filesystem::path(text).u8string();
            from.assign(converted.begin(), converted.end());
        }
        catch (const std::exception&) {}
        std::string out;
        for (size_t at = 0; at < from.size();)
        {
            const auto length = Utf8Length(from, at);
            out += length ? from.substr(at, length) : std::string("\xEF\xBF\xBD");
            at += length ? length : 1;
        }
        return out;
    }
    struct Report
    {
        std::string summary;            // the counts block, lines joined by \n
        Json facts = Json::object();    // "Name: value" lines (numbers as numbers)
        std::vector<std::string> notes; // the other lines of the first block
        std::string assetPackage;       // the asset dependency line's path, if any
        Json rows = Json::array();
        long long declared = -1;        // "(N)" of the items line; -1 when absent
        bool clean = false;             // "Nothing to report"
    };
    inline Report Parse(const std::string& text)
    {
        Report report;
        std::string normal;
        normal.reserve(text.size());
        for (char ch : text) if (ch != '\r') normal += ch;
        if (normal.size() >= 3 && normal.compare(0, 3, "\xEF\xBB\xBF") == 0) normal.erase(0, 3);
        std::istringstream lines(normal);
        bool header = true, inSummary = false;
        std::vector<std::string> summary;
        for (std::string line; std::getline(lines, line);)
        {
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\n')) line.pop_back();
            // Rows can follow anything; every tab-separated line is one.
            if (const auto tab = line.find('\t'); tab != std::string::npos)
            {
                const auto second = line.find('\t', tab + 1);
                report.rows.push_back(Row(line.substr(0, tab), second == std::string::npos ? line.substr(tab + 1) : line.substr(tab + 1, second - tab - 1),
                                          second == std::string::npos ? std::string() : line.substr(second + 1)));
                inSummary = false;
                continue;
            }
            if (line.empty()) { header = false; inSummary = false; continue; }
            if (line.rfind("Brushes: ", 0) == 0) { header = false; inSummary = true; summary.push_back(line); continue; }
            if (inSummary) { summary.push_back(line); continue; }
            if (line.rfind("Nothing to report", 0) == 0) { report.clean = true; continue; }
            if (line.rfind("Items that did not come back cleanly", 0) == 0)
            {
                const auto open = line.find('('), close = line.find(')');
                if (open != std::string::npos && close != std::string::npos && close > open + 1)
                {
                    const auto digits = line.substr(open + 1, close - open - 1);
                    if (digits.size() < 12 && digits.find_first_not_of("0123456789") == std::string::npos) report.declared = std::stoll(digits);
                }
                continue;
            }
            if (!header) { report.notes.push_back(line); continue; }
            const auto colon = line.find(": ");
            if (line.rfind("Include this asset dependency", 0) == 0 && colon != std::string::npos) { report.assetPackage = line.substr(colon + 2); continue; }
            if (colon == std::string::npos || colon > 70) { report.notes.push_back(line); continue; }
            const auto value = line.substr(colon + 2);
            if (!value.empty() && value.size() < 16 && value.find_first_not_of("0123456789") == std::string::npos) report.facts[line.substr(0, colon)] = std::stoll(value);
            else report.facts[line.substr(0, colon)] = value;
        }
        for (const auto& line : summary) report.summary += (report.summary.empty() ? "" : "\n") + line;
        return report;
    }
}
