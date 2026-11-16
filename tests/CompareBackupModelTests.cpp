#include "../Reloaded.Editor/CompareBackupModel.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
using namespace Workflow;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}
template<class F> void Rejects(F operation, const char* message, std::source_location where = std::source_location::current())
{
    bool rejected = false;
    try { operation(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, message, where);
}
Json Vec(const char* x, const char* y, const char* z) { return {{"X", x}, {"Y", y}, {"Z", z}}; }
Json Entry(const char* key, const char* type, Json values, Json more = Json::object())
{
    Json e = {{"key", key}, {"actor", {{"path", std::string("MyLevel.") + key}, {"class", type}}}, {"values", std::move(values)}, {"tag", type}};
    for (auto it = more.begin(); it != more.end(); ++it) e[it.key()] = it.value();
    return e;
}
const Json* RowFor(const Json& rows, const std::string& change, const std::string& target, const std::string& property = {})
{
    for (const auto& r : rows)
        if (r.at("change") == change && r.at("target") == target && (property.empty() || r.at("property") == property)) return &r;
    return nullptr;
}
size_t IndexOf(const Json& rows, const Json* row) { for (size_t i = 0; i < rows.size(); ++i) if (&rows[i] == row) return i; return rows.size(); }

int main()
{
    try
    {
        // Keys and the copy's package name.
        Check(Compare::Key("CompareCopy1.StaticMeshActor12", "CompareCopy1") == "StaticMeshActor12", "the package comes off the path");
        Check(Compare::Key("comparecopy1.Emitter3.SpriteEmitter0", "CompareCopy1") == "Emitter3.SpriteEmitter0", "without case, components keep their owner");
        Check(Compare::Key("Other.Thing", "CompareCopy1") == "Other.Thing", "another package's path stays whole");
        Check(Compare::RenameText("StaticMeshActor'CompareCopy1.Lamp'", "CompareCopy1", "MyLevel") == "StaticMeshActor'MyLevel.Lamp'", "a reference is renamed");
        Check(Compare::RenameText("CompareCopy1.Lamp", "CompareCopy1", "MyLevel") == "MyLevel.Lamp", "a bare path is renamed");
        Check(Compare::RenameText("CompareCopy12.Lamp", "CompareCopy1", "MyLevel") == "CompareCopy12.Lamp", "a longer name is not");
        Check(Compare::RenameText("Texture'Pkg.CompareCopy1.Lamp'", "CompareCopy1", "MyLevel") == "Texture'Pkg.CompareCopy1.Lamp'", "only a leading package");
        Check(Compare::RenameText("Note about CompareCopy1.Lamp", "CompareCopy1", "MyLevel") == "Note about CompareCopy1.Lamp", "text inside a string stays");
        Json renamed = Compare::Rename({{"actor", {{"path", "CompareCopy1.Lamp"}}}, {"list", {"Light'CompareCopy1.A'", "None"}}}, "CompareCopy1", "MyLevel");
        Check(renamed["actor"]["path"] == "MyLevel.Lamp" && renamed["list"][0] == "Light'MyLevel.A'" && renamed["list"][1] == "None", "every string of an entry");
        Check(Compare::Referenced("Light'MyLevel.A'") == "MyLevel.A" && Compare::Referenced("None").empty() && Compare::Referenced("'x'").empty(), "references");

        // The comparison.
        const Json backup = Json::array({
            Entry("Lamp", "Engine.Light", {{"Location", Vec("0", "0", "0")}, {"LightBrightness", "64.0"}, {"Tag", "Lamp"}}),
            Entry("Crate", "Engine.StaticMeshActor", {{"Location", Vec("10", "0", "0")}, {"Rotation", {{"Pitch", "0"}, {"Yaw", "0"}, {"Roll", "0"}}}},
                  {{"appearance", {{"StaticMesh", "StaticMesh'Pkg.Crate'"}, {"DrawScale", "1.0"}}}}),
            Entry("Gone", "Engine.StaticMeshActor", {{"Location", Vec("5", "5", "5")}, {"Event", "None"}}, {{"creatable", true}, {"appearance", {{"StaticMesh", "StaticMesh'Pkg.Box'"}}}}),
            Entry("Pointer", "SBase.SMagicEvent", {{"Location", Vec("1", "1", "1")}, {"Target", "StaticMeshActor'MyLevel.Gone'"}, {"Other", "Light'MyLevel.Lamp'"}}),
            Entry("Wall", "Engine.Brush", {{"Location", Vec("0", "0", "0")}, {"CsgOper", "CSG_Add"}}, {{"brush", true}, {"creatable", false}}),
            Entry("Smoke", "Engine.Emitter", {{"Location", Vec("2", "2", "2")}, {"Emitters", {"SpriteEmitter'MyLevel.Smoke.SpriteEmitter0'"}}}, {{"creatable", true}}),
            Entry("Smoke.SpriteEmitter0", "Engine.SpriteEmitter", {{"MaxParticles", "10"}}, {{"owner", "Smoke"}, {"creatable", true}}),
            Entry("Start", "Engine.PlayerStart", {{"Location", Vec("3", "3", "3")}}, {{"creatable", true}, {"team", "1"}}),
        });
        const Json current = Json::array({
            Entry("Lamp", "Engine.Light", {{"Location", Vec("0", "0", "0")}, {"LightBrightness", "80.0"}, {"Tag", "Lamp"}}),
            Entry("Crate", "Engine.StaticMeshActor", {{"Location", Vec("90", "0", "0")}, {"Rotation", {{"Pitch", "0"}, {"Yaw", "16384"}, {"Roll", "0"}}}},
                  {{"appearance", {{"StaticMesh", "StaticMesh'Pkg.Crate'"}, {"DrawScale", "2.0"}}}}),
            Entry("Pointer", "SBase.SMagicEvent", {{"Location", Vec("1", "1", "1")}, {"Target", "None"}, {"Other", "Light'MyLevel.Lamp'"}}),
            Entry("Wall", "Engine.Brush", {{"Location", Vec("0", "0", "64")}, {"CsgOper", "CSG_Add"}}, {{"brush", true}}),
            Entry("NewOne", "Engine.Light", {{"Location", Vec("7", "7", "7")}}),
        });
        auto rows = Compare::Diff(current, backup);
        Check(rows.size() == 11, "deleted 4 (Gone, Smoke, its particles, Start), added 1, moved 3, changed 3");
        Check(rows[0].at("change") == "Deleted", "deleted rows first");
        auto gone = RowFor(rows, "Deleted", "Gone");
        Check(gone && gone->at("action") == "create" && gone->at("select").is_null() && gone->at("before").get<std::string>().find("Engine.StaticMeshActor at (X=5") == 0,
              "a deleted actor can be added again; nothing to select; the backup's class and place");
        auto part = RowFor(rows, "Deleted", "Smoke.SpriteEmitter0");
        Check(part && part->at("action") == "" && part->at("note").get<std::string>().find("comes back with Smoke") == 0, "a particle component comes back with its emitter");
        auto added = RowFor(rows, "Added", "NewOne");
        Check(added && added->at("action") == "delete" && added->at("select")["path"] == "MyLevel.NewOne", "an added actor is deleted; a click selects it");
        auto moved = RowFor(rows, "Moved", "Crate", "Location");
        Check(moved && moved->at("before") == "(X=10,Y=0,Z=0)" && moved->at("after") == "(X=90,Y=0,Z=0)" && moved->at("action") == "update", "a move: old and new");
        Check(RowFor(rows, "Moved", "Crate", "Rotation") != nullptr, "a turn is a move");
        Check(RowFor(rows, "Changed", "Crate", "DrawScale") != nullptr && !RowFor(rows, "Changed", "Crate", "StaticMesh"), "its look is compared, unchanged parts are not listed");
        auto bright = RowFor(rows, "Changed", "Lamp", "LightBrightness");
        Check(bright && bright->at("before") == "64.0" && bright->at("after") == "80.0", "a property: old and new");
        Check(RowFor(rows, "Moved", "Wall", "Location") != nullptr, "a brush is compared by its properties");
        Check(RowFor(rows, "Changed", "Pointer", "Target") != nullptr && !RowFor(rows, "Changed", "Pointer", "Other"), "a reference");
        Check(Compare::Tally(rows) == "3 deleted, 1 added, 2 moved, 3 changed", "the tally counts actors, particle components with their emitters");
        Check(Compare::Tally(Json::array()) == "No differences", "an empty tally");

        // A property only one side could read (one class, so both have it) is not a change.
        Check(Compare::Diff(Json::array({Entry("P", "Engine.SpriteEmitter", {{"MaxParticles", "1"}, {"SizeScale", "()"}})}),
                            Json::array({Entry("P", "Engine.SpriteEmitter", {{"MaxParticles", "1"}})})).empty(), "a property one side could not read");

        // A deleted brush cannot come back.
        Json brushGone = Json::array({Entry("Wall", "Engine.Brush", {{"Location", Vec("0", "0", "0")}}, {{"brush", true}, {"creatable", true}})});
        auto brushRows = Compare::Diff(Json::array(), brushGone);
        Check(brushRows.size() == 1 && brushRows[0].at("action") == "" && brushRows[0].at("note").get<std::string>().find("shape") != std::string::npos, "brush shapes are not kept");

        // An actor brought back once is named Gone_2 (the deleted one keeps its name for Undo); it is the same actor.
        Json again = current;
        again.push_back(Entry("Gone_2", "Engine.StaticMeshActor", {{"Location", Vec("5", "5", "5")}, {"Event", "None"}}, {{"appearance", {{"StaticMesh", "StaticMesh'Pkg.Box'"}}}}));
        auto againRows = Compare::Diff(again, backup);
        Check(!RowFor(againRows, "Deleted", "Gone") && !RowFor(againRows, "Added", "Gone_2"), "Gone_2 stands for Gone");
        // An emitter brought back once: Smoke_2 with a new particle component; its list names that one.
        Json emitterAgain = current;
        emitterAgain.push_back(Entry("Smoke_2", "Engine.Emitter", {{"Location", Vec("2", "2", "2")}, {"Emitters", {"SpriteEmitter'MyLevel.Smoke_2.SpriteEmitter4'"}}}));
        emitterAgain.push_back(Entry("Smoke_2.SpriteEmitter4", "Engine.SpriteEmitter", {{"MaxParticles", "10"}}, {{"owner", "Smoke_2"}}));
        auto emitterRows = Compare::Diff(emitterAgain, backup);
        Check(!RowFor(emitterRows, "Deleted", "Smoke") && !RowFor(emitterRows, "Deleted", "Smoke.SpriteEmitter0") && !RowFor(emitterRows, "Added", "Smoke_2.SpriteEmitter4")
                  && !RowFor(emitterRows, "Changed", "Smoke_2", "Emitters"),
              "its components match by class under the new owner, and its list by what it names");
        // A stock emitter's component belongs to the map's package: listed, it selects the emitter, and is not set back alone.
        Json sharedNow = Json::array({Entry("Fire", "Engine.Emitter", {{"Location", Vec("0", "0", "0")}}),
                                      Entry("SpriteEmitter9", "Engine.SpriteEmitter", {{"MaxParticles", "20"}}, {{"owner", "Fire"}, {"shared", true}})});
        Json sharedOld = Json::array({Entry("Fire", "Engine.Emitter", {{"Location", Vec("0", "0", "0")}}, {{"creatable", true}}),
                                      Entry("SpriteEmitter9", "Engine.SpriteEmitter", {{"MaxParticles", "10"}}, {{"owner", "Fire"}, {"shared", true}, {"creatable", true}})});
        auto sharedRows = Compare::Diff(sharedNow, sharedOld);
        Check(sharedRows.size() == 1 && sharedRows[0].at("action") == "" && sharedRows[0].at("select")["path"] == "MyLevel.Fire", "a shared component");
        Check(Compare::Restored("Gone", "Gone_12") == "Gone" && Compare::Restored("Gone", "Gone_x").empty() && Compare::Restored("Gone", "Gonex_2").empty(), "the restored name rule");

        // Bringing back.
        Json comparison = {{"rows", rows}, {"current", current}, {"backup", backup}, {"level", "MyLevel"}};
        Rejects([&] { Compare::Changes(comparison, {IndexOf(rows, part)}, "map", "d"); }, "a row that cannot come back alone is nothing to do");
        Rejects([&] { Compare::Changes(comparison, {}, "map", "d"); }, "nothing ticked");
        Rejects([&] { Compare::Changes(comparison, {99}, "map", "d"); }, "a row that is not there");

        auto doc = Compare::Changes(comparison, {IndexOf(rows, gone)}, "c:\\maps\\x.sdc", "Bring back");
        Check(doc["format"] == "scct.map-changes" && doc["version"] == Authoring::kLatestVersion && doc["map"] == "c:\\maps\\x.sdc" && !doc.contains("allowDeletes"), "a change file");
        Check(doc["operations"].size() == 1, "one create");
        const auto& create = doc["operations"][0];
        Check(create["op"] == "create" && create["id"] == "Gone" && create["class"] == "Engine.StaticMeshActor" && create["geometry"] == "point", "the old name, class and geometry");
        Check(create["properties"]["Location"] == Vec("5", "5", "5") && create["properties"]["StaticMesh"] == "StaticMesh'Pkg.Box'" && create["properties"]["Event"] == "None",
              "the backup's values and look");
        Authoring::Validate(doc, "c:\\maps\\x.sdc");

        // Everything ticked: references to re-added actors use their creation ID, a player start keeps its team,
        // an emitter's particles come as components (its Emitters list left to them).
        std::vector<size_t> all;
        for (size_t i = 0; i < rows.size(); ++i) if (!rows[i].at("action").get<std::string>().empty()) all.push_back(i);
        auto full = Compare::Changes(comparison, all, "map", "Bring back");
        Authoring::Validate(full, "map");
        Check(full["allowDeletes"] == true, "deletes are allowed");
        std::map<std::string, Json> ops;
        for (const auto& op : full["operations"])
            ops[op["op"].get<std::string>() + ":" + (op.contains("id") ? op["id"].get<std::string>() : op["actor"]["path"].get<std::string>())] = op;
        Check(full["operations"][0]["op"] == "delete" && ops.count("delete:MyLevel.NewOne") && ops["delete:MyLevel.NewOne"]["before"] == current[4]["values"], "the added light goes, with its values");
        Check(ops.count("create:Gone") && ops.count("create:Smoke") && ops.count("create:Start") && !ops.count("create:Wall"), "creatable actors come back, the brush does not");
        Check(ops["create:Start"]["properties"]["TeamNumber"] == "1", "a start's team");
        Check(!ops["create:Smoke"]["properties"].contains("Emitters"), "the emitter's list is left to its components");
        Check(ops.count("component:Smoke_SpriteEmitter0") && ops["component:Smoke_SpriteEmitter0"]["owner"] == "Smoke" && ops["component:Smoke_SpriteEmitter0"]["properties"]["MaxParticles"] == "10",
              "its particles");
        auto& pointer = ops["update:MyLevel.Pointer"];
        Check(pointer["before"] == current[2]["values"] && pointer["properties"]["Target"] == Json{{"$ref", "Gone"}}, "a reference to a re-added actor goes by its ID");
        auto& crate = ops["update:MyLevel.Crate"];
        Check(crate["properties"].size() == 3 && crate["properties"]["Location"] == Vec("10", "0", "0") && crate["properties"]["DrawScale"] == "1.0", "one update per actor, with every ticked property");
        Check(ops["update:MyLevel.Wall"]["properties"]["Location"] == Vec("0", "0", "0"), "a brush moved back");

        // A reference to an actor that stays gone becomes None; to one this file deletes, None too.
        auto pointerRow = RowFor(rows, "Changed", "Pointer", "Target");
        auto alone = Compare::Changes(comparison, {IndexOf(rows, pointerRow)}, "map", "d");
        Check(alone["operations"][0]["properties"]["Target"] == "None", "a gone actor is not pointed at");
        // Gone came back before as Gone_2: the reference names Gone_2.
        Json comparison2 = {{"rows", againRows}, {"current", again}, {"backup", backup}, {"level", "MyLevel"}};
        auto pointerRow2 = RowFor(againRows, "Changed", "Pointer", "Target");
        auto relinked = Compare::Changes(comparison2, {IndexOf(againRows, pointerRow2)}, "map", "d");
        Check(relinked["operations"][0]["properties"]["Target"] == "StaticMeshActor'MyLevel.Gone_2'", "a reference follows the actor brought back before");

        // An ID the open map already uses as a name or Tag gets a suffix.
        std::set<std::string> taken = {Fold("Gone"), Fold("Gone_2")};
        Check(Compare::CreationId("Gone", taken) == "Gone_3" && Compare::CreationId("9 lives", {}) == "_9_lives", "creation IDs");
        Json tagged = current;
        tagged[0]["tag"] = "Gone";
        Json comparison3 = {{"rows", rows}, {"current", tagged}, {"backup", backup}, {"level", "MyLevel"}};
        Check(Compare::Changes(comparison3, {IndexOf(rows, gone)}, "map", "d")["operations"][0]["id"] == "Gone_2", "a Tag in use");

        std::cout << "CompareBackupModelTests: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "CompareBackupModelTests failed: " << e.what() << "\n";
        return 1;
    }
}
