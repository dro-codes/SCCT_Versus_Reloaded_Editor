// The conveyor belt's scripts, meshes and texture as the editor imports and compiles
// them into a map. The import, the compile and the belt in game are checked natively.
#include "../Reloaded.Editor/ConveyorModel.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
using namespace Conveyor;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}
bool Contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
size_t Count(const std::string& text, const std::string& part)
{
    size_t n = 0;
    for (auto at = text.find(part); at != std::string::npos; at = text.find(part, at + 1)) ++n;
    return n;
}
// Every face's (b - a) x (c - a) points away from the mesh's middle (all parts are convex boxes or a tube).
bool Outward(const Mesh& m, const Point& middle)
{
    for (const auto& t : m.triangles)
    {
        const auto n = Cross(Sub(t.points[1], t.points[0]), Sub(t.points[2], t.points[0]));
        Point centre{};
        for (const auto& p : t.points) for (int a = 0; a < 3; ++a) centre[a] += p[a] / 3;
        if (Dot(n, Sub(centre, middle)) <= 0) return false;
    }
    return true;
}

int main()
{
    try
    {
        const auto belt = BeltScript("MyMap"), roller = RollerScript("MyMap"), item = ItemScript();
        Check(Contains(belt, "class ConveyorBelt extends Actor\r\n\tplaceable;"), "belt class header");
        Check(Contains(roller, "class ConveyorRoller extends Actor;"), "roller class header");
        Check(Contains(item, "class ConveyorItem extends Actor;"), "item class header");
        for (const auto* script : {&belt, &roller, &item})
        {
            Check(script->find('\n') == script->find("\r\n") + 1, "CRLF line ends");
            Check(Count(*script, "{") == Count(*script, "}"), "balanced braces");
        }
        Check(Contains(belt, "StaticMesh=StaticMesh'MyMap.Conveyor.ConveyorFrame'"), "the frame from the map package");
        Check(Contains(roller, "StaticMesh=StaticMesh'MyMap.Conveyor.ConveyorRoller'"), "the roller from the map package");
        Check(Contains(belt, "Version=" + std::to_string(Version) + "\r\n"), "version default");
        Check(Contains(belt, "var(Conveyor) StaticMesh ItemMeshes[8];") && Contains(belt, "var vector ItemOffsets[8];"), "item slots");
        Check(Contains(belt, "var ConveyorItem Items[128];"), "item capacity");
        for (const char* name : {"Length", "Width", "Speed", "ItemSpacing", "FadeDistance", "ItemScale", "bCarryPlayers", "bShowItems", "bShowRollers"})
            Check(Contains(belt, std::string(" ") + name + ";"), "editable setting");
        // Runs on every machine: kept on clients and simulated there; spawned parts are local.
        Check(Contains(belt, "bStatic=True") && Contains(belt, "RemoteRole=ROLE_SimulatedProxy"), "belt is lit with the map and runs on clients");
        const auto motor = MotorScript();
        Check(Contains(motor, "class ConveyorMotor extends Info;") && Contains(motor, "Belt.Tick(DeltaTime)") && Contains(motor, "RemoteRole=ROLE_None"),
              "a local motor ticks the static belt");
        // An actor ticks after its owner and a static belt never ticks: nothing it spawns is owned.
        Check(Contains(belt, "Spawn(class'ConveyorMotor');") && Contains(belt, "Spawn(class'ConveyorRoller',,,") &&
              Contains(belt, "Spawn(class'ConveyorItem',,,") && !Contains(belt, ", self"), "spawned parts have no owner");
        Check(Contains(belt, "var(Conveyor) ConveyorBelt NextBelt;") && Contains(belt, "var ConveyorBelt Seg[" + std::to_string(MaxChain) + "];"), "belts chain");
        Check(MaxChain >= 64, "room for a long line: each turn adds two belts");
        // A loop of exactly MaxChain belts is still a loop: back at the first belt is
        // checked before the chain counts as full.
        const auto walk = belt.find("for (B = self; B != None; B = B.NextBelt)");
        Check(walk != std::string::npos && belt.find("bLoop = true;", walk) < belt.find("if (Segs >= " + std::to_string(MaxChain) + ")", walk),
              "a loop is found before the chain is full");
        Check(Contains(belt, "B != self && n < " + std::to_string(MaxChain) + ";"), "a loop of MaxChain belts has a leader");
        Check(Contains(belt, "if (!bLoop && FadeDistance > 0)"), "a closed loop never fades");
        Check(Contains(belt, "simulated function Join(int s, ConveyorBelt Before)") && Contains(belt, "Join(0, Seg[Segs - 1]);"),
              "items cross from a belt's end onto the next one's centre line, a loop's last onto its first");
        Check(!Contains(belt, "@"), "every placeholder filled");
        Check(!Contains(belt, "\nfunction ") && !Contains(belt, "\tfunction "), "every belt function is simulated");
        Check(Contains(roller, "RemoteRole=ROLE_None") && Contains(item, "RemoteRole=ROLE_None"), "local parts");
        Check(Contains(item, "bBlockActors=True") && Contains(item, "bBlockPlayers=False"), "items push curtains, not players");
        Check(Contains(belt, "Carry(C.Pawn, Push(C.Pawn, X, Y, DeltaTime))") && Contains(belt, "P.Move(Delta);") && !Contains(belt, "SetRotation(P"), "players are moved, never turned");

        const auto frame = Frame(), roll = Roller();
        Check(Outward(roll, {0, 0, 0}), "roller faces outward");
        const auto fb = Bounds(frame);
        Check(fb[0] == 0 && fb[3] == 100 && fb[1] == -50 && fb[4] == 50, "frame starts at its pivot, a unit wide and long");
        Check(fb[2] == -FrameDepth && fb[5] == 5, "frame reaches FrameDepth under the riding surface");
        Check(frame.hulls.size() == 3, "bed and two channels collide");
        const auto open = Frame(false, true);
        Check(open.name == "ConveyorFrameNoLeft" && Bounds(open)[1] == -50 && Bounds(open)[4] == 50 && open.hulls.size() == 3,
              "an open side keeps its width, flush with the riding surface");
        Check(std::string(FrameMeshName(true, false)) == "ConveyorFrameNoRight" && std::string(FrameMeshName(false, false)) == "ConveyorFrameOpen",
              "frame names by wall");
        Check(Contains(belt, "var(Conveyor) bool bLeftWall;") && Contains(belt, "bRightWall=True"), "wall options");
        const auto rb = Bounds(roll);
        Check(rb[1] == -50 && rb[4] == 50 && std::abs(rb[5] - RollerRadius) < 0.5, "roller spans a unit, its top at the radius");
        Check(roll.hulls.empty(), "rollers are only seen");

        const auto ase = Ase(frame);
        Check(Contains(ase, "*NODE_NAME \"ConveyorFrame\"") && Count(ase, "*NODE_NAME \"MCDCX_ConveyorFrame_") == 3, "frame and hull objects");
        Check(Contains(ase, "*BITMAP \"ConveyorParts.tga\""), "the texture by name");
        Check(Count(ase, "*MESH_FACE ") == frame.triangles.size() + 3 * 12, "every face written");

        const auto tga = TextureTga();
        Check(tga.size() == 18 + 4 * Swatches * SwatchSize * SwatchSize, "texture size");
        Check(tga[2] == 2 && tga[16] == 32 && tga[12] == Swatches * SwatchSize && tga[14] == SwatchSize, "32-bit TGA header");

        const auto offset = ItemOffset({-91, -73.5f, 0, 0.4f, 0, 59});
        Check(std::abs(offset[0] - 45.3f) < 0.01 && std::abs(offset[1] - 36.75f) < 0.01 && offset[2] == 0, "a corner pivot is centred");
        Check(ItemOffset({-40, -24, -25, 40, 24, 25})[2] == 25, "a centred mesh is lifted onto the belt");
        const auto scale = FrameScale(600, 120);
        Check(scale[0] == 6.0f && scale[1] == 1.2f && scale[2] == 1.0f, "frame scale");
        Check(FrameScale(0, -5)[0] == 0.01f, "never a flat frame");
        std::cout << "ConveyorModelTests: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
