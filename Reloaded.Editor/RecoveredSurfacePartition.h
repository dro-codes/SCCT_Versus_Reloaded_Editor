#pragma once

#include "RecoveredBspGeometry.h"

#include <cstdint>

namespace RecoveredSurfacePartition
{
    using Vec3 = RecoveredBspGeometry::Vec3;

    struct Surface
    {
        std::vector<Vec3> vertices;
        Vec3 normal;
        int materialIndex = -1;
    };

    struct Piece
    {
        std::vector<Vec3> vertices;
        int materialIndex = -1;
    };

    struct Limits
    {
        std::size_t maxPieces = 200000;
        std::size_t maxVertices = 65536;
        std::size_t maxWork = 5000000;
        double epsilon = 0.000001;
        double planeTolerance = 0.02;
        double normalTolerance = 0.00001;
        // Optional authoring mode: adjust only material divisions that the
        // native importer or BSP builder cannot retain. Structural face
        // coverage is kept. Cut endpoints can move to existing boundary
        // vertices within the native 0.25-unit BSP distance band; narrower
        // paint fragments can be absorbed by a neighbouring material.
        // Exact mode is the default.
        bool matchEditorPrecision = false;
        // Partition only. The caller has already chosen the candidates that
        // lie on the face's plane (RecoveredSurfaceMatch measures that at the
        // face, with wider bands than planeTolerance and normalTolerance), so
        // every candidate not perpendicular to the face is projected onto it
        // and used.
        bool candidatesPreselected = false;
    };

    // Candidates describe visible polygons. Float32 vertex welding
    // can leave redundant/backtracking boundary points: those are repaired
    // only when every edge remains within the source float-precision envelope
    // of its convex hull boundary. Simple concave regions are triangulated
    // without filling their notches; self-crossing boundaries are rejected.
    // Non-coplanar and disjoint candidates are ignored. Earlier candidates
    // own overlapping source coverage.
    // Uncovered areas retain fallbackMaterialIndex. Source winding may be
    // opposite to the target brush face (as with subtractive brushes).
    // Output is planar, convex, and wound with face.normal. Both halves of
    // every split share exactly the same intersection points, with no
    // overlapping epsilon band. Output is cleared on failure.
    bool Partition(const RecoveredBspGeometry::Face& face,
                   const std::vector<Surface>& candidates,
                   int fallbackMaterialIndex, std::vector<Piece>& result,
                   std::string& error, const Limits& limits = {});

    // Join adjacent coplanar convex pieces only when they have the same
    // materialIndex (which must also identify their UV basis and flags).
    // Opposing shared edges are removed without moving any boundary vertex;
    // concave unions, holes and gaps remain separate. Subdivided shared edges
    // are matched explicitly. Output is cleared on failure.
    bool CoalesceCoplanarPieces(const std::vector<Piece>& pieces, const Vec3& normal,
                               std::vector<Piece>& result, std::string& error,
                               const Limits& limits = {});

    // Encode all subdivisions of one structural face together. Vertices in
    // the result are exactly representable by the editor's float32 storage.
    // Zero-area encoded material fragments may disappear. A fragment that
    // reverses or crosses after rounding may also disappear only if its
    // original width is within the measured coordinate-rounding displacement.
    // Local boundary backtracking is repaired within that same precision
    // envelope; genuine concave regions keep their triangulated outline.
    // An original face that collapses or reverses is rejected. Coverage is
    // checked with a bound calculated from actual coordinate rounding.
    bool PrepareForEditor(const RecoveredBspGeometry::Face& face,
                          const std::vector<Piece>& pieces, std::size_t maxVertices,
                          std::vector<Piece>& result, std::string& error,
                          const Limits& limits = {});

    // A float-collapsed bevel may be omitted only when the remaining brush
    // still has outward faces, positive volume and paired boundary edges.
    // collapsedFaces corresponds exactly to brush.faces; it is cleared on
    // failure. Matching keeps the existing reconstruction weld tolerance for
    // double arithmetic noise around coordinate zero. Collinear subdivisions
    // of shared edges are matched explicitly.
    bool ValidateEditorBrush(const RecoveredBspGeometry::Brush& brush,
                             std::vector<bool>& collapsedFaces,std::string& error,
                             const Limits& limits = {});

    // Resolve native-coincident vertices consistently across every face of
    // one brush, before material subdivision. A collapsed or low-area bevel is removed
    // only after the remaining faces prove closed, planar and positive-volume.
    // Retained supporting planes are unchanged. Expansion is bounded by native
    // point precision, or the 0.25-unit BSP split band for low-area bevels.
    // Failure leaves the brush intact.
    bool CanonicalizeBrushForEditor(RecoveredBspGeometry::Brush& brush,
                                    std::string& error,const Limits& limits = {});

    // Convex planar sheets with at most 16 float vertices remain one polygon;
    // planarity uses the stricter of epsilon and planeTolerance. Ordinary
    // non-solid sheets need not bound a volume or be exactly planar.
    // Other sheets use a stable triangulation projection while retaining the original
    // float32 3D vertices and a geometric normal for each triangle. Source
    // boundary repair is limited to its measured float-precision envelope.
    bool PrepareSheetForEditor(const Surface& sheet,std::vector<Surface>& result,
                               std::string& error,const Limits& limits = {});

    // Small polygons are retained; larger ones become triangles around an
    // interior point, preserving collinear boundary vertices without emitting
    // the zero-area triangles produced by a first-vertex fan.
    bool SplitForVertexLimit(const std::vector<Vec3>& polygon, const Vec3& normal,
                             std::size_t maxVertices,
                             std::vector<std::vector<Vec3>>& result,
                             std::string& error, const Limits& limits = {});

    // Optional broad-phase index. The absolute normalized plane distance is
    // independent of normal orientation. Conversion is range-checked and
    // reserves room for neighbouring bucket probes. Exact coplanarity must
    // still be checked by Partition; caller must choose a sufficiently broad
    // bucket search for source-plane precision.
    bool PlaneBucket(const Vec3& normal, const Vec3& point,
                     std::int64_t& bucket, double bucketWidth = 1.0);
}
