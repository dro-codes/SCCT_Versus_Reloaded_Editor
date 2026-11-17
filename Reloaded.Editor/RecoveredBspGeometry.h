#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Engine-independent reconstruction of structural BSP space. Coordinates and
// clipping use doubles; conversion to the editor's FPoly format belongs to the
// caller. No original construction-brush grouping can be inferred here.
namespace RecoveredBspGeometry
{
    struct Vec3
    {
        double x = 0;
        double y = 0;
        double z = 0;
    };

    struct Bounds
    {
        Vec3 minimum;
        Vec3 maximum;
    };

    struct Node
    {
        // Plane equation: dot(normal, point) = distance. Front is the
        // positive half-space. Non-unit normals are normalized on entry.
        Vec3 normal;
        double distance = 0;
        int front = -1;
        int back = -1;
        bool isCsg = true;
        int surfaceIndex = -1;
    };

    struct Face
    {
        // Counter-clockwise when viewed from outside the closed brush.
        std::vector<Vec3> vertices;
        Vec3 normal;
        // The ancestral splitting surface, not a guarantee that its material
        // covers this entire polygon. Coplanar material partitioning is the
        // caller's responsibility. -1 means no known surface.
        int surfaceIndex = -1;
    };

    struct Brush
    {
        std::vector<Face> faces;
        bool subtractive = true;
    };

    struct Limits
    {
        std::size_t maxNodes = 1000000;
        std::size_t maxBrushes = 20000;
        std::size_t maxFacesPerBrush = 256;
        std::size_t maxTotalFaces = 1000000;
        std::size_t maxPendingCells = 4096;
        std::size_t maxClippingWork = 50000000;
        // Absolute coordinate tolerance. Geometry below this scale cannot be
        // distinguished reliably and is treated as coincident/zero volume.
        double epsilon = 0.000001;
        // Reconstruct only. A cell that cannot become a brush is left out and
        // counted in Result instead of failing the whole reconstruction: one
        // open to the extraction bounds, one a split leaves without a closed
        // cap, and one that is not a closed volume at the tolerance.
        bool skipUnusableCells = false;
        // Reconstruct only. Cells narrower than this are left out and counted
        // (0 keeps every cell): the editor's BSP builder cannot tell the two
        // sides of such a sliver apart.
        double minimumCellWidth = 0;
    };

    struct Result
    {
        std::vector<Brush> brushes;
        std::size_t emptyLeafCount = 0;
        std::size_t solidLeafCount = 0;
        // Cells left out (Limits::skipUnusableCells, minimumCellWidth).
        std::size_t unboundedCells = 0;
        std::size_t degenerateCells = 0;
        std::size_t sliverCells = 0;
    };

    // Node zero is the root; -1 denotes a leaf. Nodes reachable through front
    // and back must form a tree. Unreachable nodes may represent coplanar
    // surface-chain records, and are validated but not traversed.
    //
    // extractionBounds must STRICTLY enclose the geometry being recovered.
    // They are only a clipping workspace: output retaining one of these
    // artificial faces is rejected, never exported as an invented wall.
    //
    // rootOutside == false: emit empty cells as subtractive brushes.
    // rootOutside == true: emit solid cells as additive brushes.
    // For each splitter: frontOutside = outside || isCsg;
    //                    backOutside  = outside && !isCsg.
    //
    // Output is committed only on success. Any failure clears result.
    // With Limits::skipUnusableCells the faults that concern one cell (open
    // to the bounds, no closed cap, not a closed volume) no longer fail: the
    // cell is left out and counted in result.
    bool Reconstruct(const std::vector<Node>& nodes, bool rootOutside,
                     const Bounds& extractionBounds, Result& result,
                     std::string& error, const Limits& limits = {});

    // Remove internal cell boundaries only when adjacent brushes share a
    // complete face and their union is convex. Exterior planes and occupied
    // space are preserved at the existing reconstruction tolerance. Material
    // regions on merged faces must still be restored from cooked surfaces.
    // Commits atomically; on failure the input result is unchanged.
    bool MergeAdjacentConvexBrushes(Result& result,std::string& error,
                                    const Limits& limits = {});

    struct GrowStatistics
    {
        std::size_t grown = 0;     // pairs replaced by one larger brush
        std::size_t absorbed = 0;  // further brushes lying inside a grown one
        bool budgetReached = false;
        // For each output brush, the input brushes (by index) it replaces.
        std::vector<std::vector<std::size_t>> members;
    };

    // Replace two neighbouring brushes (or two brushes either side of a common
    // neighbour) with the region bounded by the planes supporting both, when
    // that region lies entirely inside reference (the disjoint cells the
    // brushes came from), and drop any other brush it then contains. This
    // finds convex unions of several brushes that pairwise face joining
    // cannot reach. No plane is invented, so the result still meets the
    // original walls exactly.
    // allowOverlap == false keeps the brushes disjoint: a region that would
    // cut into a brush it does not contain is refused. true lets brushes of
    // one CSG kind overlap (the arms of a crossing become two crossing bars),
    // which describes the same space but gives the editor's BSP builder
    // coplanar overlapping faces to resolve.
    // Reaching the work budget keeps the merges made so far
    // (statistics.budgetReached); invalid input fails and leaves result intact.
    bool GrowConvexBrushes(const Result& reference, Result& result, bool allowOverlap,
                           GrowStatistics& statistics, std::string& error,
                           const Limits& limits = {});

    // Enclosed volume of a closed brush, and its narrowest width: the least,
    // over its faces, of the greatest depth any vertex lies behind that face.
    double Volume(const Brush& brush);
    double MinimumWidth(const Brush& brush);

    // Build larger volumes before smaller details. The native BSP builder
    // can otherwise lose narrow features between adjacent recovered cells.
    // Only homogeneous additive or subtractive sets can be reordered without
    // changing CSG meaning. Coordinates, faces and equal-volume order remain
    // unchanged; failure leaves the input intact.
    bool OrderForRebuild(Result& result,std::string& error);
}
