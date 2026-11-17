// Standalone: cl /nologo /std:c++20 /W4 /WX /EHsc tests\RecoveredSurfaceMatchTests.cpp
#include "../Reloaded.Editor/RecoveredSurfaceMatch.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
using namespace RecoveredSurfaceMatch;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}

// A square of the given half size on the plane z = height, wound about +z.
std::vector<Vec3> Square(double half, double height, double x = 0, double y = 0)
{
    return {{x - half, y - half, height}, {x + half, y - half, height}, {x + half, y + half, height}, {x - half, y + half, height}};
}

void Unfolding()
{
    // z -448 -> -453 -> -450 -> -467: the corner at -453 doubles back.
    const auto folded = Unfold({{0, 0, -448}, {0, 0, -453}, {0, 0, -450}, {0, 0, -467}, {32, 0, -467}, {32, 0, -448}});
    Check(folded.size() == 4, "a spike out and back and the straight-on corner it leaves both go");
    for (const Vec3& point : folded) Check(point.z == -448 || point.z == -467, "only real corners stay");
    Check(Unfold(Square(8, 0)).size() == 4, "a clean polygon is unchanged");
    Check(Unfold({{0, 0, 0}, {4, 0, 0}, {8, 0, 0}, {8, 8, 0}, {0, 8, 0}}).size() == 4, "a straight-on corner goes");
    Check(Unfold({{0, 0, 0}, {0.0005, 0, 0}, {8, 0, 0}, {8, 8, 0}, {0, 8, 0}}).size() == 4, "a corner a hair from the last goes");
    Check(Unfold({{0, 0, 0}, {8, 0, 0}, {4, 0, 0}}).size() < 3, "a polygon that is only a spike has no corners left");
}

void Facing()
{
    // A floor polygon faces up into the room above; the ceiling of the room
    // below is on the same plane within the tolerance and faces down.
    Index index;
    Check(index.Add(0, Square(64, 0), {0, 0, 1}, {0, 0, 1}, 0), "floor");
    auto ceiling = Square(64, -0.01);
    std::reverse(ceiling.begin(), ceiling.end());
    Check(index.Add(1, ceiling, {0, 0, -1}, {0, 0, -1}, 0.01), "ceiling");
    // The room above is a subtractive brush: its bottom face looks down.
    const auto bottom = [] { auto face = Square(32, 0); std::reverse(face.begin(), face.end()); return face; }();
    auto found = index.Find(bottom, {0, 0, -1}, -1, kPlaneTolerance, kNormalTolerance, false);
    Check(found.size() == 1 && found[0] == 0, "a subtractive face takes the polygon facing into it, not the far side of a thin wall");
    found = index.Find(bottom, {0, 0, -1}, 1, kPlaneTolerance, kNormalTolerance, false);
    Check(found.size() == 1 && found[0] == 1, "an additive face takes the polygon facing out of it");
    Check(!index.Add(2, Square(1, 0), {0, 0, 1}, {0, 0, 0}, 0), "a plane with no normal is not indexed");
}

void Tiers()
{
    Index index;
    // Polygon points 0.2 off the face's plane, on a node plane 0.2 off too.
    Check(index.Add(7, Square(64, 0.2), {0, 0, 1}, {0, 0, 1}, 0.2), "band polygon");
    // A polygon whose own points stray 0.2 but whose node plane is the face's.
    Check(index.Add(8, Square(64, 0.2, 200), {0, 0, 1}, {0, 0, 1}, 0), "strayed polygon");
    auto face = Square(16, 0);
    std::reverse(face.begin(), face.end());
    Check(index.Find(face, {0, 0, -1}, -1, kPlaneTolerance, kNormalTolerance, false).empty(), "0.2 units off is not on the plane");
    Check(index.Find(face, {0, 0, -1}, -1, kCoplanarBand, kNormalTolerance, false).size() == 1, "it is within the coplanar band");
    auto far = Square(16, 0, 200);
    std::reverse(far.begin(), far.end());
    const auto strayed = index.Find(far, {0, 0, -1}, -1, kPlaneTolerance, kNormalTolerance, false);
    Check(strayed.size() == 1 && strayed[0] == 8, "the node plane decides, not the polygon's strayed points");

    // A split plane tilted by 1e-4 is a unit off 10,000 units out: at the
    // origin the two planes agree, at the face they do not.
    Index tilted;
    const double tilt = 1e-4;
    Check(tilted.Add(3, Square(16, 1.0, 10000), {0, 0, 1}, {-tilt, 0, 1}, 0), "tilted");
    auto distant = Square(16, 0, 10000);
    std::reverse(distant.begin(), distant.end());
    Check(tilted.Find(distant, {0, 0, -1}, -1, kCoplanarBand, kNormalTolerance, false).empty(), "a unit off at the face");
    Check(tilted.Find(distant, {0, 0, -1}, -1, kLooseTolerance + 0.01, kLooseNormalTolerance, true).size() == 1,
          "the thorough look takes polygons within a unit");
    auto lifted = Square(16, 1.0, 10000);
    std::reverse(lifted.begin(), lifted.end());
    Check(tilted.Find(lifted, {0, 0, -1}, -1, kPlaneTolerance, kNormalTolerance, false).size() == 1,
          "a plane that parts from the face's at the origin is still found at the face");
}

void Space()
{
    // One CSG plane: empty in front (z > 0) of a world that starts solid.
    Tree tree;
    tree.nodes.push_back({{0, 0, 2}, 0, -1, -1, true, 0});
    Check(tree.IsOutside({0, 0, 1}) && !tree.IsOutside({0, 0, -1}), "empty in front of a CSG node, solid behind");
    Check(tree.IsOutside({0, 0, 0}), "a point on the plane goes to the front");
    Check(tree.Between(Square(8, 0), {0, 0, 1}), "a face on the plane is a wall");
    Check(!tree.Between(Square(8, 16), {0, 0, 1}), "a face in open space is not");
    tree.nodes[0].isCsg = false;
    Check(!tree.IsOutside({0, 0, 1}), "a non-CSG node leaves the state as it was");
    Tree empty;
    empty.rootOutside = true;
    Check(empty.IsOutside({1, 2, 3}), "no nodes: the root state");

    Check(AcceptableMisses(0, 0) && AcceptableMisses(2, 200) && !AcceptableMisses(3, 200), "one sample in a hundred may differ");
    Check(std::fabs(Area(Square(8, 0), {0, 0, 1}) - 256) < 1e-9, "area");
}

int main()
{
    try
    {
        Unfolding();
        Facing();
        Tiers();
        Space();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "RecoveredSurfaceMatch: " << checks << " checks passed\n";
    return 0;
}
