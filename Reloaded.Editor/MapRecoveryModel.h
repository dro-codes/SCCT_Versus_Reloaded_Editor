#pragma once
// The pure part of File > Recover Compiled Map's progress window and report:
// the stages and how far through the whole recovery each one is, which
// stages may still be cancelled, the report rows and summary text, and the
// rules deciding which recovered items are worth listing (thin brushes,
// surfaces whose original lighting did not carry over, equivalent surface
// mappings that may share one polygon). No Windows or engine calls, so the
// tests compile it alone.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace MapRecoveryModel
{
    enum class Stage : int
    {
        Preparing,
        Loading,
        Reading,
        Reconstructing,
        Merging,
        Materials,
        Assets,
        Importing,
        BuildingGeometry,
        Lighting,
        Paths,
        Saving,
        Verifying,
        Count
    };

    struct StageInfo
    {
        const char* name;
        // Share of a typical recovery's time (OffsD/ShipD timings): the
        // editor's own geometry and lighting builds dominate.
        double weight;
    };

    inline const StageInfo& Info(Stage stage)
    {
        static const StageInfo table[] = {
            {"Preparing", 2},
            {"Loading the compiled map", 5},
            {"Reading surfaces, actors and lighting", 5},
            {"Reconstructing brushes", 3},
            {"Merging fragmented geometry", 7},
            {"Restoring surface materials", 6},
            {"Preserving embedded assets", 2},
            {"Importing the source map", 4},
            {"Building geometry", 28},
            {"Restoring lighting", 24},
            {"Building paths", 3},
            {"Saving", 6},
            {"Reopening and verifying", 5},
        };
        static_assert(sizeof(table) / sizeof(table[0]) == static_cast<std::size_t>(Stage::Count),
                      "one entry per stage");
        const int index = (std::max)(0, (std::min)(static_cast<int>(stage), static_cast<int>(Stage::Count) - 1));
        return table[index];
    }

    // Whole-recovery percentage for a fraction of one stage, 0..100, never
    // going backwards within a stage.
    inline int Percent(Stage stage, double fraction)
    {
        if (!(fraction >= 0)) fraction = 0;
        if (fraction > 1) fraction = 1;
        double total = 0, before = 0;
        for (int index = 0; index < static_cast<int>(Stage::Count); ++index)
        {
            const double weight = Info(static_cast<Stage>(index)).weight;
            if (index < static_cast<int>(stage)) before += weight;
            total += weight;
        }
        if (stage >= Stage::Count) return 100;
        const double done = before + Info(stage).weight * fraction;
        return static_cast<int>(std::floor(done * 100.0 / total + 1e-9));
    }

    // The package recovery moves a map's embedded assets to, named after the
    // recovered map: <stem>_Assets, anything but ASCII letters, digits and
    // underscores made an underscore, and Recovery_ before a leading digit.
    inline std::string AssetPackageName(const std::string& stem)
    {
        std::string name = stem + "_Assets";
        for (auto& c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
        if (name.front() >= '0' && name.front() <= '9') name.insert(0, "Recovery_");
        return name;
    }

    // Saving writes the playable and source maps; from there on a cancel
    // would leave files the user did not ask for, so the stage runs to the end.
    inline bool CancelAllowed(Stage stage) { return stage < Stage::Saving; }

    enum class Target { None, Actor, Surface };

    struct ReportRow
    {
        std::string category;
        std::string item;
        std::string detail;
        Target target = Target::None;
        std::string actor;  // Target::Actor: the actor's object name
        int surface = -1;   // Target::Surface: BSP surface index of the saved map
    };

    struct Counts
    {
        bool merged = true;
        std::size_t cells = 0;          // convex cells read back from the compiled BSP
        std::size_t cellFaces = 0;
        std::size_t joinedBrushes = 0;  // after joining cells along whole shared faces
        std::size_t brushes = 0;        // final structural brushes
        std::size_t brushFaces = 0;
        std::size_t grown = 0;          // brushes grown over a neighbour
        std::size_t absorbed = 0;       // brushes found inside a grown one
        std::size_t splitBack = 0;      // grown brushes undone after a BSP build missed samples
        std::size_t sheetBrushes = 0;
        std::size_t polygons = 0;       // polygons written for the editor (all brushes)
        std::size_t unifiedSurfaces = 0;
        std::size_t actors = 0;
    };

    inline std::string Thousands(std::size_t value)
    {
        std::string digits = std::to_string(value), result;
        for (std::size_t index = 0; index < digits.size(); ++index)
        {
            if (index && (digits.size() - index) % 3 == 0) result += ',';
            result += digits[index];
        }
        return result;
    }

    inline std::string Fewer(std::size_t before, std::size_t after)
    {
        if (!before || after >= before) return "";
        const int percent = static_cast<int>(std::floor((before - after) * 100.0 / before + 0.5));
        return " (" + std::to_string(percent) + "% fewer)";
    }

    // Two or three lines for the report window and Recovery.txt.
    inline std::string Summary(const Counts& counts)
    {
        std::string text = "Brushes: " + Thousands(counts.cells) + " cells from the compiled BSP -> "
            + Thousands(counts.brushes) + " structural brushes" + Fewer(counts.cells, counts.brushes);
        if (counts.sheetBrushes) text += ", plus " + Thousands(counts.sheetBrushes) + " sheet brushes";
        text += ".\r\nBrush faces: " + Thousands(counts.cellFaces) + " -> " + Thousands(counts.brushFaces)
            + Fewer(counts.cellFaces, counts.brushFaces) + "; " + Thousands(counts.polygons)
            + " polygons written after restoring materials.";
        if (counts.merged)
        {
            text += "\r\nMerging: " + Thousands(counts.cells > counts.joinedBrushes ? counts.cells - counts.joinedBrushes : 0)
                + " cells joined along shared faces, " + Thousands(counts.grown) + " brushes grown over neighbours ("
                + Thousands(counts.absorbed) + " more absorbed), " + Thousands(counts.unifiedSurfaces)
                + " matching surfaces combined.";
            if (counts.splitBack)
                text += " " + Thousands(counts.splitBack)
                    + " grown brushes were split back because the editor's BSP build did not reproduce them exactly.";
        }
        else
            text += "\r\nMerging was off (File > Merge Recovered Geometry): only cells sharing whole faces were joined, "
                    "as recovery always has; no brushes were grown and no surfaces combined.";
        return text;
    }

    inline std::string TargetText(const ReportRow& row)
    {
        if (row.target == Target::Actor) return row.actor;
        if (row.target == Target::Surface) return "surface " + std::to_string(row.surface);
        return "";
    }

    // Tab-separated lines appended to Recovery.txt.
    inline std::string RowsText(const std::vector<ReportRow>& rows)
    {
        std::string text;
        for (const auto& row : rows)
            text += row.category + "\t" + row.item + "\t" + row.detail + "\n";
        return text;
    }

    // A brush thinner than one unit or smaller than a 4-unit cube is a sliver
    // left by the compiled BSP's splits. It still builds, but is hard to
    // select and edit, so the report lists it.
    inline bool IsThinBrush(double minimumWidth, double volume)
    {
        return minimumWidth < 1.0 || volume < 64.0;
    }

    // A rebuilt surface is listed when at least half of its lighting texels,
    // and at least 64 of them, had no original lighting to copy and kept the
    // freshly calculated bake.
    inline bool LightingNotPreserved(std::size_t matched, std::size_t unmatched)
    {
        return unmatched >= 64 && unmatched * 2 >= matched + unmatched;
    }

    // The texture mapping of one recovered surface.
    struct SurfaceMapping
    {
        std::string material;
        std::uint32_t flags = 0;
        bool structural = true;
        double normal[3]{};
        double origin[3]{};
        double u[3]{};
        double v[3]{};
    };

    // Two surfaces on one plane look identical when their material, flags,
    // texture axes and texture offsets agree: u = (P - origin) . U for every
    // point P, so differing origins only matter along U and V. Pieces of such
    // surfaces can then become one polygon. The tolerance is in texels.
    inline bool SameMapping(const SurfaceMapping& a, const SurfaceMapping& b, double tolerance = 0.01)
    {
        if (!a.structural || !b.structural || a.flags != b.flags) return false;
        if (a.material.size() != b.material.size()) return false;
        for (std::size_t index = 0; index < a.material.size(); ++index)
        {
            const char x = a.material[index], y = b.material[index];
            const char lx = (x >= 'A' && x <= 'Z') ? static_cast<char>(x - 'A' + 'a') : x;
            const char ly = (y >= 'A' && y <= 'Z') ? static_cast<char>(y - 'A' + 'a') : y;
            if (lx != ly) return false;
        }
        auto dot = [](const double* p, const double* q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
        for (int axis = 0; axis < 3; ++axis)
            if (std::fabs(a.normal[axis] - b.normal[axis]) > 1e-6 || a.u[axis] != b.u[axis] || a.v[axis] != b.v[axis])
                return false;
        const double delta[3] = {a.origin[0] - b.origin[0], a.origin[1] - b.origin[1], a.origin[2] - b.origin[2]};
        return std::fabs(dot(delta, a.u)) <= tolerance && std::fabs(dot(delta, a.v)) <= tolerance;
    }

    // Representative index for each surface: the first earlier surface with
    // the same mapping, or itself. Surfaces are grouped by material and flags
    // first, so the comparison stays quadratic only within such a group.
    inline std::vector<int> UnifyMappings(const std::vector<SurfaceMapping>& surfaces)
    {
        std::vector<int> result(surfaces.size());
        std::map<std::pair<std::string, std::uint32_t>, std::vector<int>> groups;
        for (std::size_t index = 0; index < surfaces.size(); ++index)
        {
            result[index] = static_cast<int>(index);
            if (!surfaces[index].structural) continue;
            std::string key = surfaces[index].material;
            for (auto& character : key)
                if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
            auto& group = groups[{key, surfaces[index].flags}];
            for (int earlier : group)
                if (SameMapping(surfaces[static_cast<std::size_t>(earlier)], surfaces[index]))
                {
                    result[index] = earlier;
                    break;
                }
            if (result[index] == static_cast<int>(index)) group.push_back(static_cast<int>(index));
        }
        return result;
    }
}
