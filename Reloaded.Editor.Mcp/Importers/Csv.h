#pragma once
// A CSV table of actors to scct.level-spec: one map-changes create operation
// per row, in extra.operations. Engine-free.
//
// The header names the columns (any case): class (required), x, y, z (Unreal
// units; Location is required), yaw, pitch, roll (degrees), id, tag, event.
// Every other column is a property, written as given: a number or name stays
// text, a cell that starts with { or [ is JSON (structs and arrays), and an
// empty cell leaves the class default. Strings must carry their own quotes
// ("\"Door opens\"") as map-changes expects. Options: scale (1) multiplies
// x, y and z; offset [x, y, z] is added after.
#include "Common.h"

namespace Mcp::Import
{
    // Whether a CSV's first row is an actor table's header (it has a class column).
    inline bool IsActorTable(const std::string& text)
    {
        const auto rows = ParseCsv(text.substr(0, std::min<size_t>(text.size(), 4096)));
        for (const auto& row : rows)
        {
            bool any = false;
            for (const auto& cell : row) any = any || !Trim(cell).empty();
            if (!any) continue;
            for (const auto& cell : row) if (Lower(Trim(cell)) == "class") return true;
            return false;
        }
        return false;
    }
    inline Json ImportCsv(const std::string& text, const Json& options, Json& report)
    {
        Report(report);
        const double scale = Option(options, "scale", 1.0, 1e-6, 1e6);
        Vector offset{0, 0, 0};
        if (options.is_object() && options.contains("offset"))
        {
            const auto& v = options.at("offset");
            if (!v.is_array() || v.size() != 3 || !v[0].is_number() || !v[1].is_number() || !v[2].is_number()) throw std::runtime_error("Option offset is [x, y, z].");
            offset = {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
        }
        const auto rows = ParseCsv(text);
        size_t first = 0;
        while (first < rows.size() && std::all_of(rows[first].begin(), rows[first].end(), [](const std::string& c) { return Trim(c).empty(); })) ++first;
        if (first >= rows.size()) throw std::runtime_error("The CSV is empty.");
        std::vector<std::string> header;
        for (const auto& cell : rows[first]) header.push_back(Trim(cell));
        std::map<std::string, size_t> column;
        for (size_t i = 0; i < header.size(); ++i) if (!header[i].empty()) column.emplace(Lower(header[i]), i);
        if (!column.count("class")) throw std::runtime_error("The CSV needs a class column (and x, y, z): one row per actor.");
        static const std::set<std::string> known{"class", "x", "y", "z", "yaw", "pitch", "roll", "id", "tag", "event"};
        Json spec = NewSpec(options, "csv");
        auto& operations = List(spec, {"extra", "operations"});
        Ids ids;
        long long actors = 0, skipped = 0;
        for (size_t r = first + 1; r < rows.size(); ++r)
        {
            const auto& row = rows[r];
            auto cell = [&](const std::string& name) -> std::string
            {
                const auto found = column.find(name);
                return found == column.end() || found->second >= row.size() ? std::string() : Trim(row[found->second]);
            };
            if (std::all_of(row.begin(), row.end(), [](const std::string& c) { return Trim(c).empty(); })) continue;
            const auto line = " (row " + std::to_string(r + 1) + ")";
            const auto cls = cell("class");
            double x = 0, y = 0, z = 0;
            if (cls.empty()) { Warn(report, "Rows without a class were left out" + line + "."); ++skipped; continue; }
            if (!ParseNumber(cell("x"), x) || !ParseNumber(cell("y"), y)) { Warn(report, "Rows without numeric x and y were left out" + line + "."); ++skipped; continue; }
            if (!cell("z").empty() && !ParseNumber(cell("z"), z)) { Warn(report, "A row's z is not a number" + line + "; it was left out."); ++skipped; continue; }
            auto text3 = [](double v) { return Num(v).dump(); };
            auto units = [](double degrees) { long long u = std::llround(degrees * 65536.0 / 360.0) % 65536; if (u < 0) u += 65536; return std::to_string(u); };
            Json properties{{"Location", {{"X", text3(x * scale + offset[0])}, {"Y", text3(y * scale + offset[1])}, {"Z", text3(z * scale + offset[2])}}}};
            double pitch = 0, yaw = 0, roll = 0;
            const bool hasYaw = ParseNumber(cell("yaw"), yaw), hasPitch = ParseNumber(cell("pitch"), pitch), hasRoll = ParseNumber(cell("roll"), roll);
            if (hasYaw || hasPitch || hasRoll) properties["Rotation"] = {{"Pitch", units(pitch)}, {"Yaw", units(yaw)}, {"Roll", units(roll)}};
            const auto tag = cell("tag"), event = cell("event");
            if (!tag.empty()) properties["Tag"] = tag;
            if (!event.empty()) properties["Event"] = event;
            for (size_t i = 0; i < header.size() && i < row.size(); ++i)
            {
                if (header[i].empty() || known.count(Lower(header[i]))) continue;
                const auto value = Trim(row[i]);
                if (value.empty()) continue;
                if (value[0] == '{' || value[0] == '[')
                {
                    try { properties[header[i]] = Json::parse(value); continue; }
                    catch (const std::exception&) { Warn(report, "Column " + header[i] + " holds text that starts like JSON but is not; it was written as text" + line + "."); }
                }
                properties[header[i]] = value;
            }
            const auto shortName = cls.substr(cls.find_last_of('.') == std::string::npos ? 0 : cls.find_last_of('.') + 1);
            std::string wanted = cell("id");
            if (wanted.empty()) wanted = tag;
            const auto id = wanted.empty() ? ids.Next(Ids::Clean(shortName).empty() ? std::string("Actor") : Ids::Clean(shortName)) : ids.Make(wanted, "Actor");
            if (!wanted.empty() && id != Ids::Clean(wanted)) Warn(report, "Id \"" + wanted + "\" became " + id + " (ids are letters, digits and _, unique ignoring case)" + line + ".");
            operations.push_back({{"op", "create"}, {"id", id}, {"class", cls}, {"properties", properties}});
            ++actors;
        }
        Count(report, "actors", actors);
        if (skipped) Count(report, "skippedRows", skipped);
        if (scale != 1 || offset != Vector{0, 0, 0}) Assume(report, "Positions are scaled by " + Num(scale).dump() + " and offset by [" + Num(offset[0]).dump() + ", " + Num(offset[1]).dump() + ", " + Num(offset[2]).dump() + "].");
        Assume(report, "Each row is created as written (extra.operations); property values are not checked until the spec is previewed against the live classes.");
        spec["provenance"]["notes"] = report.at("assumptions");
        Prune(spec);
        return spec;
    }
}
