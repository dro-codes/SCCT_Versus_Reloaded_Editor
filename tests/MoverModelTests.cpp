#include "../Reloaded.Editor/MoverModel.h"
#include <iostream>
#include <source_location>
#include <string>
using namespace Workflow;
using namespace Workflow::Movers;
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
bool Near(double a, double b, double within = 1e-6) { return std::abs(a - b) <= within; }
bool Near(const Vector& a, const Vector& b, double within = 1e-6) { return Near(a[0], b[0], within) && Near(a[1], b[1], within) && Near(a[2], b[2], within); }

// A door like ShipD's Mover39247: base (1674,-919,-1), key 1 slides 166 along -X.
Path Door()
{
    Path p;
    p.base = { 1674, -919, -1 };
    p.keys = { Key{}, Key{ { -166, 0, 0 }, {} } };
    return p;
}

void Places()
{
    auto door = Door();
    Check(Near(At(door, 0).position, { 1674, -919, -1 }), "key 0 is the base");
    Check(Near(At(door, 1).position, { 1508, -919, -1 }), "key 1 is base + KeyPos[1]");
    door.baseRotation = { 0, 16384, 0 };
    door.keys[1].rotation = { 0, 8192, 0 };
    Check(At(door, 1).rotation == Rotation{ 0, 24576, 0 }, "rotations add up");
    Reject([&] { At(door, 2); });
    Reject([&] { At(door, -1); });
    const auto key = KeyFor(door, { { 1700, -919, 100 }, { 0, 16384, 100 } });
    Check(Near(key.position, { 26, 0, 101 }) && key.rotation == Rotation{ 0, 0, 100 }, "a place as a key of the path");
    Check(OnKey(door, At(door, 0)), "sits on key 0");
    Check(OnKey(door, { { 1674.3, -919, -1 }, { 0, 16384, 0 } }), "within half a unit still sits on its key");
    Check(!OnKey(door, { { 1675, -919, -1 }, { 0, 16384, 0 } }), "a unit off is off its key");
    Check(!OnKey(door, { { 1674, -919, -1 }, { 0, 16390, 0 } }), "turned is off its key");
}

void Adding()
{
    auto door = Door();
    const Pose place{ { 1674, -919, 199 }, {} };
    auto added = AddKey(door, place);
    Check(added.keys.size() == 3 && added.current == 2, "a new last key, and the mover sits on it");
    Check(Near(added.keys[2].position, { 0, 0, 200 }), "the new key is the place from the base");
    Check(Near(At(added, added.current).position, place.position), "the mover does not move");
    Check(added.keys[0] == door.keys[0] && added.keys[1] == door.keys[1], "earlier keys stay");
    // Full: 8 keys.
    Path full = door;
    while (static_cast<int>(full.keys.size()) < full.capacity) full = AddKey(full, place);
    Check(full.keys.size() == 8, "eight keys fit");
    Reject([&] { AddKey(full, place); });
    // A path that is not readable is refused.
    Path broken = door; broken.current = 5;
    Reject([&] { AddKey(broken, place); });
    Path none = door; none.keys.clear(); none.current = 0;
    Reject([&] { AddKey(none, place); });
}

void Updating()
{
    auto door = Door();
    auto updated = UpdateKey(door, 1, { { 1674, -1000, -1 }, { 0, 100, 0 } });
    Check(Near(updated.keys[1].position, { 0, -81, 0 }) && updated.keys[1].rotation == Rotation{ 0, 100, 0 }, "key 1 takes the place");
    Check(updated.current == 1, "the mover sits on the updated key");
    Check(updated.keys[0] == door.keys[0], "key 0 stays");
    Reject([&] { UpdateKey(door, 2, {}); });
}

Path ThreeKeys()
{
    Path p = Door();
    p.keys.push_back(Key{ { -166, 0, 100 }, { 0, 0, 0 } });
    p.keys.push_back(Key{ { -166, 200, 100 }, { 0, 4096, 0 } });
    return p; // 4 keys
}

void Deleting()
{
    auto p = ThreeKeys();
    p.current = 3;
    auto d = DeleteKey(p, 1);
    Check(d.keys.size() == 3 && d.keys[1] == p.keys[2] && d.keys[2] == p.keys[3], "later keys move up");
    Check(d.current == 2, "the mover stays on the key it sat on");
    p.current = 1;
    d = DeleteKey(p, 1);
    Check(d.current == 1 && d.keys[1] == p.keys[2], "deleting the key it sits on: the next key takes its place");
    p.current = 3;
    d = DeleteKey(p, 3);
    Check(d.current == 2, "deleting the last key it sits on: back to the new last key");
    p.current = 0;
    d = DeleteKey(p, 0);
    Check(d.current == 0 && d.keys[0] == p.keys[1] && d.keys.size() == 3, "key 0 can go: key 1 becomes the first");
    Path one = Door(); one.keys.resize(1);
    Reject([&] { DeleteKey(one, 0); });
    Reject([&] { DeleteKey(p, 4); });
}

void Reordering()
{
    auto p = ThreeKeys();
    p.current = 1;
    auto m = MoveKey(p, 1, +1);
    Check(m.keys[2] == p.keys[1] && m.keys[1] == p.keys[2], "down swaps with the next");
    Check(m.current == 2, "the mover keeps sitting on the key it was on");
    m = MoveKey(p, 2, -1);
    Check(m.keys[1] == p.keys[2] && m.current == 2, "the neighbour moved: the mover goes with the key it sat on");
    Reject([&] { MoveKey(p, 0, -1); });
    Reject([&] { MoveKey(p, 3, +1); });
    Reject([&] { MoveKey(p, 1, 2); });
    auto g = GoTo(p, 3);
    Check(g.current == 3 && g.keys == p.keys, "Go to Key only changes the key it sits on");
    Reject([&] { GoTo(p, 4); });
}

void Moves()
{
    const auto rest = Door();
    // Nothing moved.
    Check(Compare(rest, rest, At(rest, 0)) == Moved::No, "at rest");
    // Moved off its key, the path untouched.
    const Pose off{ { 1700, -919, -1 }, {} };
    Check(Compare(rest, rest, off) == Moved::OffKey, "moved off the key");
    // The stock editor's move at key 0: the base goes with the mover.
    Path dragged0 = rest; dragged0.base = { 1700, -900, -1 };
    Check(Compare(rest, dragged0, At(dragged0, 0)) == Moved::Dragged, "a move at key 0 carries the base");
    // At key 1: KeyPos[1] rewritten to the new place.
    Path at1 = rest; at1.current = 1;
    Path dragged1 = at1; dragged1.keys[1].position = { -100, 50, 0 };
    Check(Compare(at1, dragged1, At(dragged1, 1)) == Moved::Dragged, "a move at key 1 rewrites key 1");
    // Two keys different, or another key changed: an edit, not a move.
    Path edited = at1; edited.keys[0].position = { 5, 0, 0 };
    Check(Compare(at1, edited, At(edited, 1)) == Moved::Changed, "another key edited");
    Path more = rest; more.keys.push_back(Key{});
    Check(Compare(rest, more, At(more, 0)) == Moved::Changed, "a key added elsewhere");
    Path other = rest; other.current = 1;
    Check(Compare(rest, other, At(other, 1)) == Moved::Changed, "a different key chosen");

    // Add Key Here after a stock move at key 1: key 1 stays where it was.
    const Pose place = At(dragged1, 1);
    const auto& reference = MeasuredAgainst(&at1, dragged1, place);
    Check(&reference == &at1, "measured against the path at rest");
    auto added = AddKey(reference, place);
    Check(added.keys[1] == at1.keys[1] && Near(added.keys[2].position, { -100, 50, 0 }) && added.current == 2, "the move becomes a new key");
    // After a move at key 0 the base goes back, so the path stays put.
    const Pose place0 = At(dragged0, 0);
    auto added0 = AddKey(MeasuredAgainst(&rest, dragged0, place0), place0);
    Check(Near(added0.base, rest.base) && Near(added0.keys[2].position, { 26, 19, 0 }), "the base stays; the new key is the move");
    Check(Near(At(added0, added0.current).position, place0.position), "and the mover stays where it was put");
    // Moved off its key without the path rewritten.
    auto addedOff = AddKey(MeasuredAgainst(&rest, rest, off), off);
    Check(Near(addedOff.keys[2].position, { 26, 0, 0 }), "off its key: the key is the place");
    // An edit elsewhere: the live path is kept.
    Check(&MeasuredAgainst(&at1, edited, At(edited, 1)) == &edited, "an edit is kept");
    Check(&MeasuredAgainst(nullptr, edited, At(edited, 1)) == &edited, "nothing at rest: the live path");
}

void Preview()
{
    auto p = ThreeKeys(); // 4 keys: 3 legs
    Timing t;
    t.moveTime = 2;
    t.stayOpenTime = 10;
    t.glide = false;
    Check(Near(PreviewLength(p, t), 2 * 3 * 2 + kPreviewPause), "out, a pause capped at 3 s, and back");
    Check(Near(PreviewAt(p, t, 0).position, At(p, 0).position), "starts at key 0");
    Check(Near(PreviewAt(p, t, 1).position, { 1674 - 83, -919, -1 }), "halfway along the first leg");
    Check(Near(PreviewAt(p, t, 2).position, At(p, 1).position), "reaches key 1 after MoveTime");
    Check(Near(PreviewAt(p, t, 6).position, At(p, 3).position), "the last key after three legs");
    Check(Near(PreviewAt(p, t, 7.5).position, At(p, 3).position), "pauses there");
    Check(Near(PreviewAt(p, t, 9 + 1).position, Vector{ 1674 - 166, -919 + 100, -1 + 100 }), "comes back: halfway from key 3 to key 2");
    Check(Near(PreviewAt(p, t, 100).position, At(p, 0).position), "ends at key 0");
    Check(PreviewAt(p, t, 4 + 1).rotation == Rotation{ 0, 2048, 0 }, "turns as it goes");
    t.glide = true;
    Check(Near(PreviewAt(p, t, 1).position, { 1674 - 83, -919, -1 }), "eased: the middle is still the middle");
    Check(PreviewAt(p, t, 0.5).position[0] > 1674 - 83 * 0.5, "eased: slower at the start");
    Check(Near(Ease(0.25, true), 0.15625) && Near(Ease(0.25, false), 0.25), "3a^2 - 2a^3");
    t.stayOpenTime = 1;
    Check(Near(PreviewLength(p, t), 13), "a short pause is kept");
    Path one = Door(); one.keys.resize(1);
    Check(PreviewLength(one, t) == 0, "one key: nothing to play");
    Check(Near(Arrival(t, 3), 6), "arrival: legs of MoveTime");
    // The short way round.
    Check(TurnBetween(0, 61440, 0.5, false) == 30720, "the long way");
    Check(TurnBetween(0, 61440, 0.5, true) == -2048, "the short way");
    Check(TurnBetween(65536, 0, 1, true) == 65536, "a whole turn the short way is none");
}

void Ghosts()
{
    const std::vector<Edge> edges = { { { 110, 0, 0 }, { 110, 10, 0 } } };
    const Pose place{ { 100, 0, 0 }, {} };
    auto moved = Ghost(edges, place, { { 100, 0, 50 }, {} });
    Check(Near(moved[0].from, { 110, 0, 50 }) && Near(moved[0].to, { 110, 10, 50 }), "carried along");
    auto turned = Ghost(edges, place, { { 100, 0, 0 }, { 0, 16384, 0 } });
    Check(Near(turned[0].from, { 100, 10, 0 }, 1e-3) && Near(turned[0].to, { 90, 10, 0 }, 1e-3), "turned a quarter about the pivot");
    // From a turned mover back to no turn.
    auto back = Ghost(turned, { { 100, 0, 0 }, { 0, 16384, 0 } }, place);
    Check(Near(back[0].from, edges[0].from, 1e-3) && Near(back[0].to, edges[0].to, 1e-3), "and back");
    std::vector<Edge> many(kGhostEdges + 1, Edge{ { 0, 0, 0 }, { 10, 20, 30 } });
    auto box = Simplify(many);
    Check(box.size() == 12, "past the cap a box stands in");
    Check(Simplify(edges).size() == 1, "under the cap the outline is kept");
}

void Names()
{
    Check(StateLabel("TriggerOpenTimed").find("StayOpenTime") != std::string::npos, "timed trigger");
    Check(StateLabel("triggertoggle").find("each trigger") != std::string::npos, "any case");
    Check(StateLabel("SLiftGo") == "SLiftGo", "unknown states keep their name");
    Check(StateLabel("None").find("default") != std::string::npos, "none set");
    Check(InternalState("s_WaitAndRetry") && InternalState("OpenTimedMover") && !InternalState("TriggerControl"), "helper states");
    Check(ChoiceLabel("MoverEncroachType", "ME_CrushWhenEncroach") == "Crushes what blocks it (ME_CrushWhenEncroach)", "encroach");
    Check(ChoiceLabel("MoverGlideType", "MV_GlideByTime").find("Eases") == 0, "glide");
    Check(ChoiceLabel("BumpType", "BT_Other") == "BT_Other", "unknown choice");
    Check(Near(Seconds("2.5", "MoveTime"), 2.5), "seconds");
    Reject([] { Seconds("-1", "MoveTime"); });
    Reject([] { Seconds("abc", "MoveTime"); });
    Reject([] { Seconds("601", "MoveTime"); });
    Reject([] { Seconds("2s", "MoveTime"); });
}

void Arrays()
{
    // The values as the authoring JSON reads them from ShipD's door.
    Json values = { { "NumKeys", "2" }, { "KeyNum", "1" }, { "KeyPos", Json::array() }, { "KeyRot", Json::array() } };
    for (int i = 0; i < 8; ++i)
    {
        values["KeyPos"].push_back({ { "X", i == 1 ? "-166.000000" : "0.000000" }, { "Y", "0.000000" }, { "Z", "0.000000" } });
        values["KeyRot"].push_back({ { "Pitch", "0" }, { "Yaw", i == 1 ? "16384" : "0" }, { "Roll", "0" } });
    }
    auto p = FromValues(values, { 1674, -919, -1 }, {});
    Check(p.keys.size() == 2 && p.capacity == 8 && p.current == 1, "two keys of eight, on key 1");
    Check(Near(p.keys[1].position, { -166, 0, 0 }) && p.keys[1].rotation == Rotation{ 0, 16384, 0 }, "key 1");
    auto positions = PositionsJson(p), rotations = RotationsJson(p);
    Check(positions.size() == 8 && positions[1]["X"] == "-166.000000" && positions[5]["Z"] == "0.000000", "all slots written");
    Check(rotations[1]["Yaw"] == "16384" && rotations[7]["Roll"] == "0", "rotations written");
    // Deleting a key clears the slot it leaves.
    auto three = AddKey(p, { { 0, 0, 0 }, {} });
    auto back = DeleteKey(three, 2);
    Check(PositionsJson(back)[2]["X"] == "0.000000", "a freed slot is zero");
    values["NumKeys"] = "0";
    Reject([&] { FromValues(values, {}, {}); });
    values["NumKeys"] = "9";
    Reject([&] { FromValues(values, {}, {}); });
    values["NumKeys"] = "2";
    values["KeyNum"] = "7";
    Check(FromValues(values, {}, {}).current == 1, "a KeyNum past the keys reads as the last key");
    values["KeyRot"].erase(0);
    Reject([&] { FromValues(values, {}, {}); });
    // Between the window and the editor.
    auto round = PathFrom(PathJson(three));
    Check(round == three, "the path survives JSON");
    auto bad = PathJson(three);
    bad["current"] = 3;
    Reject([&] { PathFrom(bad); });
    bad = PathJson(three);
    bad["keys"] = Json::array();
    Reject([&] { PathFrom(bad); });
}

int main()
{
    try
    {
        Places();
        Adding();
        Updating();
        Deleting();
        Reordering();
        Moves();
        Preview();
        Ghosts();
        Names();
        Arrays();
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "MoverModelTests: " << checks << " checks passed\n";
    return 0;
}
