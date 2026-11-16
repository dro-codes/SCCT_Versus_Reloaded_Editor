#include "../Reloaded.Editor/MapCheckModel.h"
#include <iostream>
#include <source_location>
#include <stdexcept>

using namespace MapCheck;
using Json = nlohmann::json;

void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class F> void Reject(F f, std::source_location where = std::source_location::current())
{
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    if (!caught) throw std::runtime_error("invalid input accepted at line " + std::to_string(where.line()));
}

Problem Make(Severity severity, const char* group, const char* check, const std::string& message)
{
    Problem p;
    p.severity = severity; p.group = group; p.check = check; p.message = message;
    return p;
}

int main()
{
    try
    {
        // Rows keep the report's order, with severities spelled for the bridge.
        Report report;
        report.bspBuilt = true;
        report.actorsChecked = 12;
        report.zones = 3;
        auto error = Make(Severity::Error, "Actors", "Actor in the void", "PlayerStart3 is in solid space.");
        error.actors = {"MyLevel.PlayerStart3"};
        auto leak = Make(Severity::Warning, "Zones", "Zone leaks", "Zone 2 leaks to the outside.");
        leak.surfaces = {4, 17};
        leak.path = {{0.04, 1.06, 2.0}, {100.0, -50.25, 2.0}};
        leak.pathThroughPortal = true;
        auto note = Make(Severity::Note, "Editor checks", "Stock entry", "BSP Brush has zero polygons.");
        report.problems = {error, leak, note};

        const Json json = ToJson(report);
        Check(json.at("rows").size() == 3, "one row per problem");
        const auto& first = json.at("rows").at(0);
        Check(first.at("severity") == "error" && first.at("group") == "Actors" && first.at("check") == "Actor in the void", "error row fields");
        Check(first.at("message") == "PlayerStart3 is in solid space.", "error message");
        Check(first.at("actors") == Json::array({"MyLevel.PlayerStart3"}) && first.at("surfaces") == Json::array(), "error selects its actor");
        Check(!first.contains("path") && !first.contains("pathThroughPortal"), "no leak path on an actor row");
        const auto& second = json.at("rows").at(1);
        Check(second.at("severity") == "warning" && second.at("surfaces") == Json::array({4, 17}) && second.at("actors").empty(), "leak row selects surfaces");
        Check(second.at("path").size() == 2 && second.at("pathThroughPortal") == true, "leak path kept");
        Check(second.at("path").at(0) == Json::array({0.0, 1.1, 2.0}) && second.at("path").at(1) == Json::array({100.0, -50.3, 2.0}), "path rounded to a tenth");
        Check(json.at("rows").at(2).at("severity") == "info", "notes are info");

        const auto& summary = json.at("summary");
        Check(summary.at("errors") == 1 && summary.at("warnings") == 1 && summary.at("info") == 1, "counts by severity");
        Check(summary.at("actorsChecked") == 12 && summary.at("zones") == 3 && summary.at("bspBuilt") == true, "summary totals");
        Check(summary.at("text") == "1 error(s), 1 warning(s), 1 note(s); 12 actors and 3 zone(s) checked.", "summary text");

        // A clean report.
        Report clean;
        clean.bspBuilt = true;
        clean.actorsChecked = 4;
        clean.zones = 1;
        const Json none = ToJson(clean);
        Check(none.at("rows").empty() && none.at("summary").at("errors") == 0, "clean report has no rows");
        Check(none.at("summary").at("text") == "No problems found; 4 actors and 1 zone(s) checked.", "clean summary text");

        // An unbuilt map: Analyse says so, and the JSON carries it.
        const Json unbuilt = ToJson(Analyse(Scene{}, Settings{}));
        Check(unbuilt.at("summary").at("bspBuilt") == false, "unbuilt BSP flagged");
        bool told = false;
        for (const auto& row : unbuilt.at("rows")) if (row.at("check") == "BSP not built" && row.at("severity") == "warning") told = true;
        Check(told, "unbuilt BSP row");

        // Engine text is ANSI: a stray high byte is read as Latin-1, so the
        // reply still serialises; UTF-8 passes through unchanged.
        Check(Utf8("Caf\xE9") == "Caf\xC3\xA9", "Latin-1 byte converted");
        Check(Utf8("Caf\xC3\xA9") == "Caf\xC3\xA9", "UTF-8 kept");
        Check(Utf8("\xE2\x82\xAC 5") == "\xE2\x82\xAC 5", "three-byte UTF-8 kept");
        Check(Utf8("\xC3") == "\xC3\x83", "truncated sequence read as Latin-1");
        Check(Utf8("\xC0\x80") == "\xC3\x80\xC2\x80", "overlong form read as Latin-1");
        Check(Utf8("plain") == "plain" && Utf8("").empty(), "ASCII kept");
        Report ansi;
        auto named = Make(Severity::Warning, "Actors", "Actor far outside", "St\xE9phane's light is far outside the map.");
        named.actors = {"MyLevel.L\xE9ger"};
        ansi.problems = {named};
        const Json converted = ToJson(ansi);
        Check(converted.at("rows").at(0).at("actors").at(0) == "MyLevel.L\xC3\xA9ger", "actor path converted");
        std::string dumped;
        try { dumped = converted.dump(); } catch (const std::exception&) {}
        Check(!dumped.empty(), "converted report serialises");
        Reject([] { Json raw = std::string("\xE9"); (void)raw.dump(); }); // what the conversion prevents

        std::cout << "MapCheckJsonModelTests passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "MapCheckJsonModelTests failed: " << e.what() << "\n";
        return 1;
    }
}
