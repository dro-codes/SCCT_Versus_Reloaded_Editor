#include "../Reloaded.Editor/PrefabModel.h"
#include <iostream>
#include <source_location>
#include <string>
using namespace Workflow;
using namespace Workflow::Prefab;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}
template <class F> void Reject(F f, std::source_location where = std::source_location::current())
{
    bool caught = false;
    try { f(); } catch (const std::exception&) { caught = true; }
    if (!caught) throw std::runtime_error("invalid input accepted at line " + std::to_string(where.line()));
}
bool Near(const Vector& a, const Vector& b, double tolerance = 0.01)
{
    for (int i = 0; i < 3; ++i) if (std::abs(a[i] - b[i]) > tolerance) return false;
    return true;
}

void Groups()
{
    Check(GroupName("Guard Post #2", "0123abcdEF998877") == "Prefab_Guard_Post_2_0123abcd", "readable stem, lower-case id");
    Check(GroupName("!!!", "deadbeef00") == "Prefab_Assembly_deadbeef", "a name with no letters");
    const auto longName = GroupName("A very long assembly name indeed, longer than twenty", "00000000");
    Check(longName.size() <= 7 + 20 + 9 && longName.rfind("Prefab_A_very_long_assembly", 0) == 0, "stem capped at 20");
    Reject([] { GroupName("Door", "xyz"); });
    Reject([] { GroupName("Door", "1234567g"); });
    Check(IsInstanceGroup("Prefab_Door_0123abcd") && IsInstanceGroup("prefab_door_0123ABCD"), "instance groups, any case");
    Check(!IsInstanceGroup("Prefab_Door") && !IsInstanceGroup("Doors_0123abcd") && !IsInstanceGroup("Prefab_Door_0123abc"), "other groups are not");
    Check(SplitGroups("None").empty() && SplitGroups(" A , b,a,None ") == std::vector<std::string>{ "A", "b" }, "split trims and drops repeats");
    Check(SplitGroups("\"Lobby,Prefab_Door_0123abcd\"").size() == 2, "a quoted Group field");
    Check(InstanceGroups("Lobby,Prefab_Door_0123abcd") == std::vector<std::string>{ "Prefab_Door_0123abcd" }, "instance groups of a field");
    Check(WithoutInstanceGroups("Lobby,Prefab_Door_0123abcd") == "Lobby", "the user's groups stay");
    Check(WithoutInstanceGroups("Prefab_Door_0123abcd") == "None", "nothing left is None");
    Check(WithGroup("None", "Prefab_Door_0123abcd") == "Prefab_Door_0123abcd", "join to an empty field");
    Check(WithGroup("Lobby", "Prefab_Door_0123abcd") == "Lobby,Prefab_Door_0123abcd", "join to a user group");
    Check(WithGroup("Lobby,prefab_door_0123ABCD", "Prefab_Door_0123abcd") == "Lobby,prefab_door_0123ABCD", "no repeat");
    Check(WithoutGroup("Lobby,Prefab_Door_0123abcd", "PREFAB_DOOR_0123ABCD") == "Lobby", "break link removes the group");
    Check(CanonicalName("RE_0123456789ab_RE_ba9876543210_Door1") == "Door1" && CanonicalName("Door1") == "Door1", "placement prefixes go");
}

Json Actor(const std::string& name, Vector position, Rotation rotation, const std::string& extra = "")
{
    const std::string text = "Begin Actor Class=StaticMeshActor Name=" + name + "\n" + extra + "    Location=(X=0,Y=0,Z=0)\nEnd Actor\n";
    return { { "name", name }, { "class", "Engine.StaticMeshActor" }, { "position", position }, { "rotation", rotation }, { "text", text } };
}

void Text()
{
    Json definition = { { "actors", Json::array({
        Actor("A", { 0, 0, 0 }, { 0, 0, 0 }, "    Group=\"Lobby,Prefab_Door_0123abcd\"\n"),
        Actor("B", { 0, 0, 0 }, { 0, 0, 0 }, "    Group=Prefab_Door_0123abcd\n"),
        Actor("C", { 0, 0, 0 }, { 0, 0, 0 }, "    Group=Lobby\n") }) } };
    auto stripped = WithoutInstanceGroupsInText(definition);
    Check(Property(stripped["actors"][0]["text"], "Group") == "\"Lobby\"", "the user's group is kept, quoted");
    Check(Property(stripped["actors"][1]["text"], "Group").empty(), "an instance-only group goes");
    Check(stripped["actors"][2]["text"] == definition["actors"][2]["text"], "a user group is untouched");
    Check(!HasBrushes(definition), "meshes need no build");
    definition["actors"].push_back({ { "name", "W" }, { "class", "Engine.BlockingVolume" }, { "text", "" } });
    Check(HasBrushes(definition), "a volume needs one");
}

void Poses()
{
    // Round trip: a member placed by a turned, raised frame gives that frame back.
    const Pose frame{ { 1000, -250, 64 }, { 0, 16384, 0 } };
    const Pose local{ { 128, 32, 0 }, { 0, 4096, 0 } };
    const auto world = Placed(frame, local);
    Check(Near(world.position, { 1000 - 32, -250 + 128, 64 }), "a quarter turn left");
    const auto back = FrameFromMember(local, world);
    Check(SamePose(back, frame), "frame from the member");
    Check(Near(back.position, frame.position, 0.05), "frame position");
    // A pitched and rolled member still finds the frame.
    const Pose tilted{ { 10, 20, 30 }, { 3000, 7000, -2000 } };
    const Pose frame2{ { -5000, 300, 900 }, { 0, -30000, 0 } };
    Check(SamePose(FrameFromMember(tilted, Placed(frame2, tilted)), frame2), "tilted member");
    // SamePose compares where things point, not how the rotator is written.
    Check(SamePose({ {}, { 0, 65536 + 100, 0 } }, { {}, { 0, 100, 0 } }), "a full turn is the same");
    Check(!SamePose({ {}, { 0, 0, 0 } }, { {}, { 0, 400, 0 } }), "two degrees is not");
    Check(!SamePose({ { 0, 0, 0 }, {} }, { { 0.8, 0, 0 }, {} }), "0.8 units is not");
}

Json Record(const Pose& frame, const Json& definition)
{
    Json placed = { { "id", "0123abcd99" }, { "assembly", "asm" }, { "members", Json::array() }, { "names", Json::object() },
                    { "tags", Json::object() }, { "position", frame.position }, { "rotation", frame.rotation } };
    return NewRecord(placed, "Prefab_Door_0123abcd", definition);
}

void Frames()
{
    Json definition = { { "actors", Json::array({ Actor("Frame", { 0, 0, 0 }, { 0, 0, 0 }), Actor("Door", { 64, 0, 0 }, { 0, 16384, 0 }) }) } };
    const Pose placedAt{ { 500, 500, 0 }, { 0, 8192, 0 } };
    auto record = Record(placedAt, definition);
    Check(Linked(record) && record["locals"].contains("door"), "a linked record knows its members' poses");
    const auto frameWorld = Placed(placedAt, { { 0, 0, 0 }, { 0, 0, 0 } });
    const auto doorWorld = Placed(placedAt, { { 64, 0, 0 }, { 0, 16384, 0 } });
    // Untouched: the recorded frame, exactly.
    auto frame = InstanceFrame(record, { { "RE_0123456789ab_Frame", frameWorld }, { "RE_0123456789ab_Door", doorWorld } });
    Check(frame.position == placedAt.position && frame.rotation == placedAt.rotation, "untouched instance keeps its frame");
    // One member edited (the door moved), the other where it was: still the recorded frame.
    Pose movedDoor = doorWorld; movedDoor.position[2] += 100;
    frame = InstanceFrame(record, { { "RE_0123456789ab_Door", movedDoor }, { "RE_0123456789ab_Frame", frameWorld } });
    Check(frame.position == placedAt.position, "an edited member does not move the instance");
    // The whole instance moved and turned: the frame follows it.
    const Pose movedTo{ { -800, 40, 256 }, { 0, -16384, 0 } };
    frame = InstanceFrame(record, { { "RE_aaaaaaaaaaaa_Door", Placed(movedTo, { { 64, 0, 0 }, { 0, 16384, 0 } }) },
                                    { "RE_aaaaaaaaaaaa_Frame", Placed(movedTo, { { 0, 0, 0 }, { 0, 0, 0 } }) } });
    Check(SamePose(frame, movedTo), "a moved instance is rebuilt where it stands");
    // Members it does not know are ignored; none known keeps the record.
    frame = InstanceFrame(record, { { "Lamp", { { 9, 9, 9 }, {} } } });
    Check(frame.position == placedAt.position, "unknown members keep the recorded frame");
    Json old = record; old.erase("locals");
    Check(InstanceFrame(old, { { "Door", { { 9, 9, 9 }, {} } } }).position == placedAt.position, "an old record keeps its frame");
}

void Records()
{
    Json definition = { { "actors", Json::array({ Actor("Door", { 0, 0, 0 }, { 0, 0, 0 }) }) } };
    auto a = Record({ { 0, 0, 0 }, {} }, definition);
    auto b = a; b["id"] = "fedcba98"; b["group"] = "Prefab_Door_fedcba98";
    Json plain = { { "id", "plain" }, { "assembly", "asm" }, { "members", Json::array() }, { "position", { 0, 0, 0 } }, { "rotation", { 0, 0, 0 } } };
    auto c = b; c["id"] = "other"; c["assembly"] = "another"; c["group"] = "Prefab_Crate_11112222";
    Json instances = Json::array({ a, plain, b, c });
    Check(LinkedOf(instances, "asm").size() == 2 && LinkedOf(instances, "another").size() == 1, "linked instances per assembly; unlinked copies do not count");
    Check(CountText(2) == "2 linked instances" && CountText(1) == "1 linked instance" && CountText(0).empty(), "count text");
    Check(ByGroup(instances, "PREFAB_DOOR_FEDCBA98") && ByGroup(instances, "PREFAB_DOOR_FEDCBA98")->at("id") == "fedcba98", "by group, any case");
    Check(!ByGroup(instances, "Prefab_Door_00000000"), "an unknown group");
    auto named = RecordsNamed(instances, { "Lobby,Prefab_Door_fedcba98", "Prefab_Door_fedcba98", "None", "Prefab_Door_0123abcd" });
    Check(named.size() == 2 && named[0]["id"] == "fedcba98" && named[1]["id"] == "0123abcd99", "each instance once, in selection order");
    Check(RecordsNamed(instances, { "Lobby" }).empty(), "an unlinked selection");
    // Rebuilt: the id, assembly and group stay, the copy's details are new.
    Json placed = { { "id", "new" }, { "assembly", "asm" }, { "members", Json::array({ { { "path", "MyLevel.RE_x_Door" } } }) }, { "names", Json::object() },
                    { "tags", Json::object() }, { "position", { 1, 2, 3 } }, { "rotation", { 0, 5, 0 } } };
    Json next = { { "actors", Json::array({ Actor("Door", { 0, 0, 0 }, { 0, 0, 0 }), Actor("Lamp", { 0, 0, 90 }, { 0, 0, 0 }) }) } };
    auto rebuilt = Rebuilt(b, placed, next);
    Check(rebuilt["id"] == "fedcba98" && rebuilt["group"] == "Prefab_Door_fedcba98" && rebuilt["members"].size() == 1 && rebuilt["position"][2] == 3, "rebuilt record");
    Check(rebuilt["locals"].contains("lamp"), "rebuilt record knows the new member");
    Replace(instances, rebuilt);
    Check(instances[2]["position"][2] == 3, "replaced in place");
    Reject([&] { Replace(instances, Json{ { "id", "missing" } }); });
    auto broken = WithoutRecord(instances, "fedcba98");
    Check(broken.size() == 3 && LinkedOf(broken, "asm").size() == 1, "break link drops the record");
}

int main()
{
    try
    {
        Groups(); Text(); Poses(); Frames(); Records();
        std::cout << "PrefabModelTests: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << "PrefabModelTests FAILED: " << e.what() << "\n"; return 1; }
}
