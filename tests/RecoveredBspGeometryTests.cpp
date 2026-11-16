// Standalone, no editor/game needed:
// cl /nologo /std:c++17 /W4 /WX /EHsc tests\RecoveredBspGeometryTests.cpp Reloaded.Editor\RecoveredBspGeometry.cpp
#include "../Reloaded.Editor/RecoveredBspGeometry.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace RecoveredBspGeometry;

namespace
{
    const Bounds workBounds{ {-100,-100,-100}, {100,100,100} };

    Vec3 Subtract(const Vec3& a, const Vec3& b)
    {
        return { a.x-b.x, a.y-b.y, a.z-b.z };
    }

    double Dot(const Vec3& a, const Vec3& b)
    {
        return a.x*b.x + a.y*b.y + a.z*b.z;
    }

    Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
    }

    bool Near(const Vec3& a, const Vec3& b)
    {
        const Vec3 difference = Subtract(a,b);
        return Dot(difference,difference) < 1e-12;
    }

    // Append a six-plane box. Inward-facing room walls make their front
    // space empty; an outward-facing solid box makes its back space solid.
    int AddBox(std::vector<Node>& nodes, const Bounds& box, bool empty)
    {
        const int first = static_cast<int>(nodes.size());
        const Vec3 normals[] = {{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
        const double distances[] = {-box.minimum.x,box.maximum.x,-box.minimum.y,
                                    box.maximum.y,-box.minimum.z,box.maximum.z};
        for (int index = 0; index < 6; ++index)
        {
            Node node;
            const double sign = empty ? -1.0 : 1.0;
            node.normal = {normals[index].x*sign,normals[index].y*sign,normals[index].z*sign};
            node.distance = distances[index]*sign;
            node.surfaceIndex = first + index;
            const int child = index == 5 ? -1 : first + index + 1;
            if (empty)
                node.front = child;
            else
                node.back = child;
            nodes.push_back(node);
        }
        return first;
    }

    double Volume(const Result& result)
    {
        double volume = 0;
        for (const Brush& brush : result.brushes)
            for (const Face& face : brush.faces)
                for (std::size_t index = 1; index + 1 < face.vertices.size(); ++index)
                    volume += Dot(face.vertices[0], Cross(face.vertices[index],face.vertices[index+1]))/6.0;
        return volume;
    }

    bool Contains(const Brush& brush, const Vec3& point)
    {
        for (const Face& face : brush.faces)
            if (Dot(face.normal, Subtract(point,face.vertices.front())) > 1e-7)
                return false;
        return true;
    }

    bool Contains(const Result& result, const Vec3& point)
    {
        return std::any_of(result.brushes.begin(),result.brushes.end(),
            [&](const Brush& brush) { return Contains(brush,point); });
    }

    bool BspOutside(const std::vector<Node>& nodes, const Vec3& point, bool outside)
    {
        int index = nodes.empty() ? -1 : 0;
        while (index != -1)
        {
            const Node& node = nodes[static_cast<std::size_t>(index)];
            if (Dot(node.normal,point) > node.distance)
            {
                outside = outside || node.isCsg;
                index = node.front;
            }
            else
            {
                outside = outside && !node.isCsg;
                index = node.back;
            }
        }
        return outside;
    }

    void CompareSpace(const std::vector<Node>& nodes, const Result& result,
                      bool rootOutside = false)
    {
        for (int x = -12; x <= 12; ++x)
            for (int y = -12; y <= 12; ++y)
                for (int z = -12; z <= 12; ++z)
                {
                    const Vec3 point{x+0.137,y+0.269,z+0.391};
                    const bool target = BspOutside(nodes,point,rootOutside) != rootOutside;
                    assert(Contains(result,point) == target);
                }
    }

    void VerifyClosed(const Result& result, bool subtractive)
    {
        for (const Brush& brush : result.brushes)
        {
            assert(brush.subtractive == subtractive);
            assert(brush.faces.size() >= 4);
            for (const Face& face : brush.faces)
            {
                assert(face.vertices.size() >= 3);
                assert(face.surfaceIndex >= 0);
                assert(std::fabs(Dot(face.normal,face.normal)-1) < 1e-10);
                Vec3 area{};
                for (std::size_t i = 1; i+1 < face.vertices.size(); ++i)
                {
                    const Vec3 piece = Cross(Subtract(face.vertices[i],face.vertices[0]),
                                             Subtract(face.vertices[i+1],face.vertices[0]));
                    area = {area.x+piece.x, area.y+piece.y, area.z+piece.z};
                }
                assert(Dot(area,face.normal) > 0);
                for (std::size_t index = 0; index < face.vertices.size(); ++index)
                {
                    const Vec3& a = face.vertices[index];
                    const Vec3& b = face.vertices[(index+1)%face.vertices.size()];
                    int reverseCount = 0;
                    for (const Face& other : brush.faces)
                        for (std::size_t j = 0; j < other.vertices.size(); ++j)
                            if (Near(a,other.vertices[(j+1)%other.vertices.size()])
                                && Near(b,other.vertices[j]))
                                ++reverseCount;
                    assert(reverseCount == 1);
                    assert(std::fabs(Dot(face.normal,Subtract(a,face.vertices[0]))) < 1e-6);
                }
            }
        }
    }

    Result Recover(const std::vector<Node>& nodes, bool rootOutside = false,
                   const Bounds& bounds = workBounds)
    {
        Result result;
        std::string error;
        if (!Reconstruct(nodes,rootOutside,bounds,result,error))
        {
            std::fprintf(stderr,"Unexpected reconstruction failure: %s\n",error.c_str());
            assert(false);
        }
        assert(error.empty());
        VerifyClosed(result,!rootOutside);
        return result;
    }

    void MustFail(const std::vector<Node>& nodes, const char* part,
                  const Limits& limits = {}, const Bounds& bounds = workBounds)
    {
        Result result;
        result.brushes.push_back({}); // A failure must not expose partial output.
        std::string error;
        assert(!Reconstruct(nodes,false,bounds,result,error,limits));
        assert(result.brushes.empty());
        assert(result.emptyLeafCount == 0 && result.solidLeafCount == 0);
        if (error.find(part) == std::string::npos)
        {
            std::fprintf(stderr,"Expected failure containing '%s', got '%s'\n",part,error.c_str());
            assert(false);
        }
    }

    void VerifyConvexMerging()
    {
        std::vector<Node> grid;
        AddBox(grid,{{-10,-10,-10},{10,10,10}},true);
        grid[5].front=6;
        grid.push_back({{1,0,0},0,7,8,false,6});
        grid.push_back({{0,1,0},0,-1,-1,false,7});
        grid.push_back({{0,1,0},0,-1,-1,false,8});
        Result cells=Recover(grid);
        assert(cells.brushes.size()==4);
        std::string error;
        assert(MergeAdjacentConvexBrushes(cells,error));
        assert(error.empty() && cells.brushes.size()==1 && cells.brushes[0].faces.size()==6);
        assert(std::fabs(Volume(cells)-8000)<1e-7);
        VerifyClosed(cells,true);
        CompareSpace(grid,cells);
        assert(MergeAdjacentConvexBrushes(cells,error) && cells.brushes.size()==1);

        auto box=[](const Bounds& bounds)
        {
            std::vector<Node> nodes;
            AddBox(nodes,bounds,true);
            return Recover(nodes).brushes.front();
        };
        Result concave;
        concave.brushes={box({{-10,-10,-10},{0,10,10}}),box({{0,-10,-10},{10,0,10}})};
        assert(MergeAdjacentConvexBrushes(concave,error));
        assert(concave.brushes.size()==2 && !Contains(concave,{5,5,0}));
        assert(Contains(concave,{-5,5,0}) && Contains(concave,{5,-5,0}));
        VerifyClosed(concave,true);

        Result separated;
        separated.brushes={box({{-10,-10,-10},{0,10,10}}),box({{0.0001,-10,-10},{10,10,10}})};
        assert(MergeAdjacentConvexBrushes(separated,error));
        assert(separated.brushes.size()==2 && !Contains(separated,{0.00005,0,0}));
        Result mixed;
        mixed.brushes={box({{-10,-10,-10},{0,10,10}}),box({{0,-10,-10},{10,10,10}})};
        mixed.brushes[1].subtractive=false;
        assert(MergeAdjacentConvexBrushes(mixed,error) && mixed.brushes.size()==2);

        std::vector<Node> pillar;
        AddBox(pillar,{{-10,-10,-10},{10,10,10}},true);
        const int inside=AddBox(pillar,{{-2,-2,-10},{2,2,10}},false);
        pillar[5].front=inside;
        Result aroundPillar=Recover(pillar);
        const double originalVolume=Volume(aroundPillar);
        const auto originalBrushes=aroundPillar.brushes.size();
        assert(MergeAdjacentConvexBrushes(aroundPillar,error));
        assert(aroundPillar.brushes.size()<=originalBrushes && !Contains(aroundPillar,{0,0,0}));
        assert(std::fabs(Volume(aroundPillar)-originalVolume)<1e-7);
        VerifyClosed(aroundPillar,true);
        CompareSpace(pillar,aroundPillar);

        Result invalid=concave;
        invalid.brushes[0].faces.pop_back();
        assert(!MergeAdjacentConvexBrushes(invalid,error));
        assert(invalid.brushes.size()==2 && invalid.brushes[0].faces.size()==5);
        invalid=concave;
        invalid.brushes[0].faces[0].vertices.clear();
        assert(!MergeAdjacentConvexBrushes(invalid,error));
        assert(invalid.brushes[0].faces[0].vertices.empty());
        Limits limited;
        limited.maxClippingWork=1;
        assert(!MergeAdjacentConvexBrushes(concave,error,limited));
        assert(concave.brushes.size()==2 && error.find("work limit")!=std::string::npos);
    }

    Brush Box(const Bounds& bounds)
    {
        std::vector<Node> nodes;
        AddBox(nodes,bounds,true);
        return Recover(nodes).brushes.front();
    }

    // The grown set must cover exactly the reference space, on a lattice
    // offset from every box plane used below.
    void SameSpace(const Result& reference,const Result& grown)
    {
        for (int x=-24;x<=24;++x)
            for (int y=-24;y<=24;++y)
                for (int z=-6;z<=6;++z)
                {
                    const Vec3 point{x*0.5+0.137,y*0.5+0.269,z*0.5+0.391};
                    assert(Contains(reference,point)==Contains(grown,point));
                }
    }

    void VerifyConvexGrowth()
    {
        std::string error;
        GrowStatistics statistics;

        // A crossing: the BSP leaves a centre and four arms. Joining along
        // whole faces gives one bar and two loose arms; growth spans the
        // arms through the bar, overlapping it, for two brushes.
        Result cross;
        cross.brushes={Box({{-2,-2,-2},{2,2,2}}),Box({{-10,-2,-2},{-2,2,2}}),Box({{2,-2,-2},{10,2,2}}),
                       Box({{-2,2,-2},{2,10,2}}),Box({{-2,-10,-2},{2,-2,2}})};
        const Result crossCells=cross;
        assert(MergeAdjacentConvexBrushes(cross,error) && cross.brushes.size()==3);
        assert(GrowConvexBrushes(crossCells,cross,true,statistics,error));
        assert(error.empty() && cross.brushes.size()==2 && statistics.grown==1 && !statistics.budgetReached);
        VerifyClosed(cross,true);
        SameSpace(crossCells,cross);
        for (const Brush& brush:cross.brushes) assert(std::fabs(Volume(brush)-20*4*4)<1e-7);
        // Running it again finds nothing more to do.
        assert(GrowConvexBrushes(crossCells,cross,true,statistics,error) && cross.brushes.size()==2 && statistics.grown==0);

        // Two cells fill a square together with a third: growing the first
        // pair covers the third, which is dropped.
        Result square;
        square.brushes={Box({{-10,-10,-2},{0,10,2}}),Box({{0,-10,-2},{10,0,2}}),Box({{0,0,-2},{10,10,2}})};
        const Result squareCells=square;
        assert(GrowConvexBrushes(squareCells,square,true,statistics,error));
        assert(square.brushes.size()==1 && statistics.grown==1 && statistics.absorbed==1);
        assert(std::fabs(Volume(square.brushes.front())-20*20*4)<1e-7);
        SameSpace(squareCells,square);
        assert(statistics.members.size()==1 && statistics.members[0]==std::vector<std::size_t>({0,1,2}));

        // Kept disjoint, the square still becomes one brush (an exact union),
        // but the crossing cannot: spanning the arms cuts into the bar.
        Result disjointSquare=squareCells;
        assert(GrowConvexBrushes(squareCells,disjointSquare,false,statistics,error) && disjointSquare.brushes.size()==1);
        SameSpace(squareCells,disjointSquare);
        Result disjointCross=crossCells;
        assert(MergeAdjacentConvexBrushes(disjointCross,error) && disjointCross.brushes.size()==3);
        assert(GrowConvexBrushes(crossCells,disjointCross,false,statistics,error));
        assert(disjointCross.brushes.size()==3 && statistics.grown==0);
        assert(statistics.members==std::vector<std::vector<std::size_t>>({{0},{1},{2}}));
        SameSpace(crossCells,disjointCross);

        // An L shape cannot grow: the region spanning both arms reaches the
        // solid corner.
        Result corner;
        corner.brushes={Box({{-10,-10,-2},{0,10,2}}),Box({{0,-10,-2},{10,0,2}})};
        const Result cornerCells=corner;
        assert(GrowConvexBrushes(cornerCells,corner,true,statistics,error) && corner.brushes.size()==2);
        assert(statistics.grown==0 && !Contains(corner,{5,5,0}));
        SameSpace(cornerCells,corner);

        // A step one hundredth of a unit high still stops growth: the test is
        // exact volume, not sampling.
        Result step;
        step.brushes={Box({{-10,-2,-2},{0,2,2}}),Box({{0,-2,-2},{10,2,1.99}}),Box({{-2,2,-2},{2,10,2}})};
        const Result stepCells=step;
        assert(GrowConvexBrushes(stepCells,step,true,statistics,error));
        SameSpace(stepCells,step);
        assert(!Contains(step,{5,0,1.995}));

        // A room around a pillar keeps its exact space.
        std::vector<Node> pillar;
        AddBox(pillar,{{-10,-10,-10},{10,10,10}},true);
        pillar[5].front=AddBox(pillar,{{-2,-2,-10},{2,2,10}},false);
        const Result pillarCells=Recover(pillar);
        Result aroundPillar=pillarCells;
        assert(MergeAdjacentConvexBrushes(aroundPillar,error));
        const auto joined=aroundPillar.brushes.size();
        assert(GrowConvexBrushes(pillarCells,aroundPillar,true,statistics,error));
        assert(aroundPillar.brushes.size()<=joined && !Contains(aroundPillar,{0,0,0}));
        VerifyClosed(aroundPillar,true);
        CompareSpace(pillar,aroundPillar);

        // Mixed CSG kinds are refused and leave the input alone.
        Result mixed=cornerCells;
        mixed.brushes[1].subtractive=false;
        assert(!GrowConvexBrushes(cornerCells,mixed,true,statistics,error) && !error.empty());
        assert(mixed.brushes.size()==2 && !mixed.brushes[1].subtractive);
        Result open=crossCells;
        open.brushes[0].faces.pop_back();
        open.brushes[0].faces.pop_back();
        open.brushes[0].faces.pop_back();
        assert(!GrowConvexBrushes(crossCells,open,true,statistics,error) && open.brushes.size()==5);

        // Running out of work keeps a valid (here unchanged) result.
        Result limitedCross=crossCells;
        Limits limited;
        limited.maxClippingWork=3;
        assert(GrowConvexBrushes(crossCells,limitedCross,true,statistics,error,limited));
        assert(statistics.budgetReached && error.empty() && limitedCross.brushes.size()==5);
        SameSpace(crossCells,limitedCross);

        assert(std::fabs(MinimumWidth(Box({{0,0,0},{3,8,0.5}}))-0.5)<1e-12);
        assert(std::fabs(Volume(Box({{0,0,0},{3,8,0.5}}))-12)<1e-9);
    }
}

void VerifyRebuildOrdering()
{
    auto box=[](double half,int tag)
    {
        std::vector<Node> nodes;
        AddBox(nodes,{{-half,-half,-half},{half,half,half}},true);
        Brush brush=Recover(nodes).brushes.front();
        brush.faces.front().surfaceIndex=tag;
        return brush;
    };
    Result result;
    result.brushes={box(1,11),box(4,44),box(2,21),box(2,22)};
    const auto original=result;
    const double volume=Volume(result);
    std::string error;
    assert(OrderForRebuild(result,error));
    const int expected[]={44,21,22,11};
    for (std::size_t i=0;i<result.brushes.size();++i)
    {
        const auto& brush=result.brushes[i];
        assert(brush.faces.front().surfaceIndex==expected[i]);
        const auto found=std::find_if(original.brushes.begin(),original.brushes.end(),[&](const Brush& b)
        {
            return b.faces.front().surfaceIndex==expected[i];
        });
        assert(found!=original.brushes.end());
        for (std::size_t face=0;face<brush.faces.size();++face)
            for (std::size_t vertex=0;vertex<brush.faces[face].vertices.size();++vertex)
            {
                const auto a=brush.faces[face].vertices[vertex];
                const auto b=found->faces[face].vertices[vertex];
                assert(a.x==b.x && a.y==b.y && a.z==b.z);
            }
    }
    assert(std::fabs(Volume(result)-volume)<1e-9);
    result.brushes[1].subtractive=false;
    assert(!OrderForRebuild(result,error));
    for (std::size_t i=0;i<result.brushes.size();++i)
        assert(result.brushes[i].faces.front().surfaceIndex==expected[i]);
    result.brushes.clear();
    assert(OrderForRebuild(result,error));
}

int main()
{
    VerifyRebuildOrdering();
    VerifyConvexMerging();
    VerifyConvexGrowth();
    const Bounds room{{-10,-10,-10},{10,10,10}};
    std::vector<Node> nodes;
    AddBox(nodes,room,true);
    Result recovered = Recover(nodes);
    assert(recovered.brushes.size() == 1);
    assert(std::fabs(Volume(recovered)-8000) < 1e-7);
    assert(Contains(recovered,{0,0,0}));
    assert(!Contains(recovered,{11,0,0}));
    assert(recovered.emptyLeafCount == 1 && recovered.solidLeafCount == 6);

    // Non-unit normals and nonzero offsets preserve the same half-spaces.
    for (Node& node : nodes)
    {
        node.normal = {node.normal.x*7,node.normal.y*7,node.normal.z*7};
        node.distance *= 7;
    }
    assert(std::fabs(Volume(Recover(nodes))-8000) < 1e-7);

    // A structural pillar: the output is the exact room-minus-pillar volume.
    nodes.clear();
    AddBox(nodes,room,true);
    nodes[5].front = AddBox(nodes,{{-2,-2,-10},{2,2,10}},false);
    recovered = Recover(nodes);
    assert(std::fabs(Volume(recovered)-7680) < 1e-7);
    assert(!Contains(recovered,{0,0,0}));
    assert(Contains(recovered,{5,0,0}));
    assert(Contains(recovered,{0,5,0}));
    CompareSpace(nodes,recovered);

    // A non-CSG partition inside empty space must preserve both child states.
    nodes.clear();
    AddBox(nodes,room,true);
    nodes[5].front = static_cast<int>(nodes.size());
    nodes.push_back({{1,0,0},0,-1,-1,false,99});
    recovered = Recover(nodes);
    assert(recovered.brushes.size() == 2);
    assert(std::fabs(Volume(recovered)-8000) < 1e-7);
    assert(Contains(recovered,{-5,0,0}) && Contains(recovered,{5,0,0}));

    // A diagonal cut exercises cap winding independently of box axes.
    nodes.back().normal = {1,2,3};
    nodes.back().distance = 4;
    recovered = Recover(nodes);
    assert(std::fabs(Volume(recovered)-8000) < 1e-7);
    assert(recovered.brushes.size() == 2);

    // A diagonal structural plane makes just its front half empty.
    nodes.back() = {{1,1,1},0,-1,-1,true,99};
    recovered = Recover(nodes);
    assert(std::fabs(Volume(recovered)-4000) < 1e-7);
    assert(Contains(recovered,{5,5,5}) && !Contains(recovered,{-5,-5,-5}));
    CompareSpace(nodes,recovered);

    // The root starts in solid space: non-CSG splitters cannot create rooms.
    recovered = Recover({{{1,0,0},0,-1,-1,false,0}});
    assert(recovered.brushes.empty());

    // Empty-world models use additive cells, with the same closed geometry.
    nodes.clear();
    AddBox(nodes,room,false);
    recovered = Recover(nodes,true);
    assert(recovered.brushes.size() == 1 && !recovered.brushes[0].subtractive);
    assert(std::fabs(Volume(recovered)-8000) < 1e-7);
    CompareSpace(nodes,recovered,true);
    assert(Recover({},true).brushes.empty());
    assert(Recover({}).brushes.empty());

    // Two independently bounded adjacent rooms partitioned by a non-CSG node.
    nodes = {{{1,0,0},0,-1,-1,false,100}};
    const int right = AddBox(nodes,{{0,-10,-10},{10,10,10}},true);
    const int left = AddBox(nodes,{{-10,-10,-10},{0,10,10}},true);
    nodes[0].front = right;
    nodes[0].back = left;
    recovered = Recover(nodes);
    assert(recovered.brushes.size() == 2);
    assert(std::fabs(Volume(recovered)-8000) < 1e-7);
    CompareSpace(nodes,recovered);

    // Oblique nested partitions stress cap intersections and preserve room
    // space even when the cooked tree contains many nonstructural splits.
    nodes.clear();
    AddBox(nodes,room,true);
    nodes[5].front = 6;
    nodes.push_back({{1,2,3},1,7,8,false,6});
    nodes.push_back({{-3,2,1},2,9,10,false,7});
    nodes.push_back({{2,-3,1},-1,11,12,false,8});
    nodes.push_back({{1,1,-2},3,-1,-1,false,9});
    nodes.push_back({{4,1,-1},-2,-1,-1,false,10});
    nodes.push_back({{-1,3,2},0,-1,-1,false,11});
    nodes.push_back({{2,-1,4},4,-1,-1,false,12});
    recovered = Recover(nodes);
    assert(std::fabs(Volume(recovered)-8000) < 1e-7);
    CompareSpace(nodes,recovered);

    // Redundant/copanar split planes do not add artificial slivers or holes.
    nodes.clear();
    AddBox(nodes,room,true);
    nodes[5].front = 6;
    nodes.push_back({{1,0,0},-10,-1,-1,true,6});
    assert(std::fabs(Volume(Recover(nodes))-8000) < 1e-7);

    // Deep trees use explicit work stacks, including branches clipped away.
    nodes.clear();
    AddBox(nodes,room,true);
    nodes[5].front = 6;
    constexpr int depth = 20000;
    for (int index = 0; index < depth; ++index)
        nodes.push_back({{1,0,0},-20,index+1 == depth ? -1 : index+7,-1,false,6});
    recovered = Recover(nodes);
    assert(recovered.brushes.size() == 1);
    assert(std::fabs(Volume(recovered)-8000) < 1e-7);

    MustFail({{{1,0,0},0,0,-1,true,0}},"cycle");
    MustFail({{{1,0,0},0,1,1,true,0},{{0,1,0},0,-1,-1,true,1}},"repeated");
    MustFail({{{1,0,0},0,5,-1,true,0}},"child index");
    MustFail({{{1,0,0},0,-2,-1,true,0}},"child index");
    MustFail({{{0,0,0},0,-1,-1,true,0}},"splitting plane");
    MustFail({{{1,0,0},std::numeric_limits<double>::infinity(),-1,-1,true,0}},"splitting plane");
    MustFail({{{std::numeric_limits<double>::quiet_NaN(),0,0},0,-1,-1,true,0}},"splitting plane");

    // There is no enclosing wall behind this plane: never create a capped room.
    MustFail({{{1,0,0},0,-1,-1,true,0}},"unbounded");
    nodes.clear();
    AddBox(nodes,room,true);
    MustFail(nodes,"extraction bounds",{},{{-5,-5,-5},{5,5,5}});

    // Asked to, a cell that cannot become a brush is left out and counted.
    {
        Limits tolerant;
        tolerant.skipUnusableCells = true;
        Result open;
        std::string error;
        assert(Reconstruct({{{1,0,0},0,-1,-1,true,0}},false,workBounds,open,error,tolerant));
        assert(open.brushes.empty() && open.unboundedCells == 1 && error.empty());
        assert(Reconstruct(nodes,false,{{-5,-5,-5},{5,5,5}},open,error,tolerant));
        assert(open.brushes.empty() && open.unboundedCells == 1);
        // The room itself still comes back, and a wider sliver limit drops it.
        assert(Reconstruct(nodes,false,workBounds,open,error,tolerant));
        assert(open.brushes.size() == 1 && open.unboundedCells == 0 && open.sliverCells == 0);
        tolerant.minimumCellWidth = MinimumWidth(open.brushes.front()) + 1;
        assert(Reconstruct(nodes,false,workBounds,open,error,tolerant));
        assert(open.brushes.empty() && open.sliverCells == 1);
        tolerant.minimumCellWidth = 0.01;
        assert(Reconstruct(nodes,false,workBounds,open,error,tolerant));
        assert(open.brushes.size() == 1 && open.sliverCells == 0);
    }
    MustFail(nodes,"nonzero",{},{{0,0,0},{0,1,1}});

    Limits limits;
    limits.maxBrushes = 0;
    MustFail(nodes,"brush or total-face",limits);
    limits = {};
    limits.maxClippingWork = 20;
    MustFail(nodes,"clipping-work",limits);
    limits = {};
    limits.maxNodes = 2;
    MustFail(nodes,"node limit",limits);
    limits = {};
    limits.maxPendingCells = 1;
    MustFail(nodes,"pending-cell",limits);
    limits = {};
    limits.epsilon = 0;
    MustFail(nodes,"positive tolerance",limits);

    std::puts("Recovered BSP geometry tests passed");
}
