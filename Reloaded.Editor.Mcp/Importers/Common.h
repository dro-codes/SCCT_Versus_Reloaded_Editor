#pragma once
// What every importer shares: the report it fills, ids that are valid and
// unique in a scct.level-spec, CSV and text helpers, and the spec skeleton.
// Engine-free and Windows-free.
//
// The report (every importer fills it; Import() adds format and source):
//   {format, warnings:[sentence], counts:{name: n}, assumptions:[sentence],
//    ...importer extras (assets, overlay, sections)}
// Warnings say what was dropped or guessed badly; assumptions say what the
// importer decided for the agent to confirm (scale, heights, which side is north).
#include "../../Reloaded.Editor/WorkflowModel.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace Mcp::Import
{
    using Json = Workflow::Json;
    using Vector = Workflow::Vector;

    // --- The report ------------------------------------------------------------
    inline Json& Report(Json& report)
    {
        if (!report.is_object()) report = Json::object();
        for (const char* key : {"warnings", "assumptions"}) if (!report.contains(key) || !report.at(key).is_array()) report[key] = Json::array();
        if (!report.contains("counts") || !report.at("counts").is_object()) report["counts"] = Json::object();
        return report;
    }
    // Repeated warnings are kept once, with how many times they happened, so
    // a thousand bad rows do not bury the rest.
    inline void Warn(Json& report, const std::string& text)
    {
        auto& list = Report(report)["warnings"];
        if (list.size() >= 200) { if (list.back() != "More warnings were left out.") list.push_back("More warnings were left out."); return; }
        for (const auto& w : list) if (w == text) return;
        list.push_back(text);
    }
    inline void Assume(Json& report, const std::string& text)
    {
        auto& list = Report(report)["assumptions"];
        for (const auto& a : list) if (a == text) return;
        list.push_back(text);
    }
    inline void Count(Json& report, const std::string& key, long long n = 1)
    {
        auto& counts = Report(report)["counts"];
        counts[key] = counts.value(key, 0LL) + n;
    }

    // --- Text ------------------------------------------------------------------
    inline std::string Lower(std::string s)
    {
        for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    inline std::string Trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }
    inline bool StartsWith(const std::string& s, const std::string& prefix) { return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0; }
    inline bool EndsWith(const std::string& s, const std::string& suffix) { return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0; }
    inline std::string StripBom(const std::string& text) { return StartsWith(text, "\xEF\xBB\xBF") ? text.substr(3) : text; }
    inline std::vector<std::string> Lines(const std::string& text)
    {
        std::vector<std::string> lines;
        std::string line;
        for (char c : StripBom(text))
        {
            if (c == '\n') { if (!line.empty() && line.back() == '\r') line.pop_back(); lines.push_back(line); line.clear(); }
            else line += c;
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        return lines;
    }
    // A number in text, or nothing: the whole text must be the number.
    inline bool ParseNumber(const std::string& text, double& out)
    {
        const auto t = Trim(text);
        if (t.empty()) return false;
        try
        {
            size_t used = 0;
            const double n = std::stod(t, &used);
            if (used != t.size() || !std::isfinite(n)) return false;
            out = n;
            return true;
        }
        catch (const std::exception&) { return false; }
    }
    inline double NumberOr(const std::string& text, double fallback)
    {
        double n = 0;
        return ParseNumber(text, n) ? n : fallback;
    }
    // Numbers from text separated by spaces, commas or tabs ("1 2 3", "1,2").
    inline std::vector<double> Numbers(const std::string& text)
    {
        std::vector<double> out;
        std::string token;
        auto flush = [&] { double n = 0; if (!token.empty() && ParseNumber(token, n)) out.push_back(n); token.clear(); };
        for (char c : text)
        {
            if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ';') flush();
            else if ((c == '-' || c == '+') && !token.empty() && token.back() != 'e' && token.back() != 'E') { flush(); token += c; }
            else token += c;
        }
        flush();
        return out;
    }
    // A JSON number tidied for a spec: millionths dropped, whole numbers as integers.
    inline Json Num(double v)
    {
        if (!std::isfinite(v)) return 0;
        const double r = std::round(v);
        if (std::abs(v - r) < 1e-6 && std::abs(r) < 9e15) return static_cast<long long>(r);
        return std::round(v * 1000.0) / 1000.0;
    }
    inline Json Point(double x, double y) { return Json::array({Num(x), Num(y)}); }
    inline Json Point(double x, double y, double z) { return Json::array({Num(x), Num(y), Num(z)}); }
    // Yaw in degrees, 0-360, rounded to a tenth.
    inline double Degrees(double radians)
    {
        double d = radians * 180.0 / 3.14159265358979323846;
        d = std::fmod(d, 360.0);
        if (d < 0) d += 360;
        d = std::round(d * 10) / 10;
        return d >= 360 ? 0 : d;
    }
    // CSV with quoted cells ("" for a quote); the delimiter is sniffed from
    // the first line (comma, semicolon or tab) unless given.
    inline char SniffDelimiter(const std::string& text)
    {
        const auto end = text.find('\n');
        const auto first = text.substr(0, end);
        size_t best = 0;
        char chosen = ',';
        for (char d : {',', ';', '\t'})
        {
            const auto n = static_cast<size_t>(std::count(first.begin(), first.end(), d));
            if (n > best) { best = n; chosen = d; }
        }
        return chosen;
    }
    inline std::vector<std::vector<std::string>> ParseCsv(const std::string& source, char delimiter = 0)
    {
        const auto text = StripBom(source);
        if (!delimiter) delimiter = SniffDelimiter(text);
        std::vector<std::vector<std::string>> rows;
        std::vector<std::string> row;
        std::string cell;
        bool quoted = false, any = false;
        for (size_t i = 0; i < text.size(); ++i)
        {
            const char c = text[i];
            if (quoted)
            {
                if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; ++i; }
                else if (c == '"') quoted = false;
                else cell += c;
                continue;
            }
            if (c == '"' && Trim(cell).empty()) { cell.clear(); quoted = true; any = true; }
            else if (c == delimiter) { row.push_back(cell); cell.clear(); any = true; }
            else if (c == '\n' || c == '\r')
            {
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
                if (any || !cell.empty()) { row.push_back(cell); rows.push_back(row); }
                else rows.push_back({});
                row.clear(); cell.clear(); any = false;
            }
            else { cell += c; any = true; }
        }
        if (quoted) throw std::runtime_error("The CSV ends inside a quoted cell; a closing quote is missing.");
        if (any || !cell.empty()) { row.push_back(cell); rows.push_back(row); }
        return rows;
    }

    // --- Ids -------------------------------------------------------------------
    // Valid scct.level-spec ids ([A-Za-z_][A-Za-z0-9_]{0,62}), unique ignoring
    // case. A wanted name is cleaned up (spaces and punctuation become _) and
    // numbered when it is taken.
    class Ids
    {
    public:
        static std::string Clean(const std::string& wanted)
        {
            std::string out;
            for (unsigned char c : wanted)
            {
                if (std::isalnum(c) || c == '_') out += static_cast<char>(c);
                else if (!out.empty() && out.back() != '_') out += '_';
            }
            while (!out.empty() && out.back() == '_') out.pop_back();
            if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0]))) out = "_" + out;
            if (out.size() > 56) out.resize(56);
            return out;
        }
        void Reserve(const std::string& id) { used.insert(Lower(id)); }
        bool Taken(const std::string& id) const { return used.count(Lower(id)) != 0; }
        // wanted cleaned, or prefix + a number when wanted is empty; then
        // numbered _2, _3 ... until it is free.
        std::string Make(const std::string& wanted, const std::string& prefix)
        {
            auto base = Clean(wanted);
            if (base.empty())
            {
                for (int n = ++counters[Lower(prefix)];; n = ++counters[Lower(prefix)])
                {
                    const auto candidate = prefix + std::to_string(n);
                    if (!Taken(candidate)) { Reserve(candidate); return candidate; }
                }
            }
            if (!Taken(base)) { Reserve(base); return base; }
            for (int n = 2;; ++n)
            {
                const auto candidate = base + "_" + std::to_string(n);
                if (!Taken(candidate)) { Reserve(candidate); return candidate; }
            }
        }
        // The next of a numbered series: Room1, Room2, ...
        std::string Next(const std::string& prefix) { return Make(std::string(), prefix); }
    private:
        std::set<std::string> used;
        std::map<std::string, int> counters;
    };

    // --- The spec --------------------------------------------------------------
    // A map name in capitals: letters, digits and spaces.
    inline std::string MapName(const std::string& wanted, const std::string& fallback = "IMPORTED")
    {
        std::string out;
        for (unsigned char c : wanted)
        {
            if (std::isalnum(c)) out += static_cast<char>(std::toupper(c));
            else if ((c == ' ' || c == '_' || c == '-') && !out.empty() && out.back() != ' ') out += ' ';
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        if (out.size() > 40) out.resize(40);
        return out.empty() ? fallback : out;
    }
    // The skeleton every importer fills: format, version, map name, draft
    // profile and where it came from.
    inline Json NewSpec(const Json& options, const std::string& format, const std::string& fallbackName = "IMPORTED")
    {
        const auto name = MapName(options.is_object() ? options.value("name", options.value("mapName", std::string())) : std::string(), fallbackName);
        Json spec{{"format", "scct.level-spec"}, {"version", 1}, {"map", {{"name", name}}}, {"profile", "draft"}};
        Json source{{"format", format}};
        if (options.is_object() && options.contains("path") && options.at("path").is_string()) source["file"] = options.at("path");
        spec["provenance"] = {{"source", source}, {"notes", Json::array()}};
        return spec;
    }
    // Objects below a key path, created on first use.
    inline Json& Section(Json& spec, std::initializer_list<const char*> path)
    {
        Json* at = &spec;
        for (const char* key : path)
        {
            if (!at->contains(key) || !(*at)[key].is_object()) (*at)[key] = Json::object();
            at = &(*at)[key];
        }
        return *at;
    }
    inline Json& List(Json& spec, std::initializer_list<const char*> path)
    {
        std::vector<const char*> keys(path);
        Json* at = &spec;
        for (size_t i = 0; i + 1 < keys.size(); ++i)
        {
            if (!at->contains(keys[i]) || !(*at)[keys[i]].is_object()) (*at)[keys[i]] = Json::object();
            at = &(*at)[keys[i]];
        }
        auto& list = (*at)[keys.back()];
        if (!list.is_array()) list = Json::array();
        return list;
    }
    // Drops empty sections so the spec reads cleanly.
    inline void Prune(Json& value)
    {
        if (!value.is_object()) return;
        for (auto it = value.begin(); it != value.end();)
        {
            if (it.key() == "props" || it.key() == "properties" || it.key() == "document" || it.key() == "provenance" || it.key() == "existing") { ++it; continue; }
            Prune(it.value());
            if ((it.value().is_object() || it.value().is_array()) && it.value().empty()) it = value.erase(it);
            else ++it;
        }
    }
    // Option readers.
    inline double Option(const Json& options, const char* key, double fallback, double low = -1e9, double high = 1e9)
    {
        if (!options.is_object() || !options.contains(key) || options.at(key).is_null()) return fallback;
        const auto& v = options.at(key);
        if (!v.is_number()) throw std::runtime_error(std::string("Option ") + key + " must be a number.");
        const double n = v.get<double>();
        if (!std::isfinite(n) || n < low || n > high) throw std::runtime_error(std::string("Option ") + key + " must be between " + Json(low).dump() + " and " + Json(high).dump() + ".");
        return n;
    }
    inline bool Option(const Json& options, const char* key, bool fallback)
    {
        if (!options.is_object() || !options.contains(key) || options.at(key).is_null()) return fallback;
        if (!options.at(key).is_boolean()) throw std::runtime_error(std::string("Option ") + key + " must be true or false.");
        return options.at(key).get<bool>();
    }
    inline std::string Option(const Json& options, const char* key, const char* fallback)
    {
        if (!options.is_object() || !options.contains(key) || options.at(key).is_null()) return fallback;
        if (!options.at(key).is_string()) throw std::runtime_error(std::string("Option ") + key + " must be text.");
        return options.at(key).get<std::string>();
    }
    // The stem of a path ("C:\\maps\\Ship D.vmf" -> "Ship D").
    inline std::string Stem(const std::string& path)
    {
        const auto slash = path.find_last_of("/\\");
        auto name = slash == std::string::npos ? path : path.substr(slash + 1);
        const auto dot = name.find_last_of('.');
        return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    }
    inline std::string Extension(const std::string& path)
    {
        const auto slash = path.find_last_of("/\\");
        const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
        const auto dot = name.find_last_of('.');
        return dot == std::string::npos ? std::string() : Lower(name.substr(dot + 1));
    }
}
