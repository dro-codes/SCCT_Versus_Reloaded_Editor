// Standalone: cl /nologo /std:c++20 /W4 /WX /EHsc tests\MapRecoveryModelTests.cpp
#include "../Reloaded.Editor/MapRecoveryModel.h"
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
using namespace MapRecoveryModel;
int checks = 0;
void Check(bool value, const char* message, std::source_location where = std::source_location::current())
{
    ++checks;
    if (!value) throw std::runtime_error(std::string(message) + " (line " + std::to_string(where.line()) + ")");
}
bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void Stages()
{
    Check(Percent(Stage::Preparing, 0) == 0, "recovery starts at 0%");
    Check(Percent(Stage::Verifying, 1) == 100, "the last stage ends at 100%");
    Check(Percent(Stage::Count, 0) == 100, "past the last stage is 100%");
    int previous = -1;
    for (int stage = 0; stage < static_cast<int>(Stage::Count); ++stage)
        for (double fraction : {0.0, 0.3, 0.7, 1.0})
        {
            const int percent = Percent(static_cast<Stage>(stage), fraction);
            Check(percent >= previous && percent >= 0 && percent <= 100, "percentages never go backwards");
            previous = percent;
        }
    Check(Percent(Stage::Loading, 1) == Percent(Stage::Reading, 0), "one stage ends where the next starts");
    Check(Percent(Stage::Merging, -5) == Percent(Stage::Merging, 0), "negative fractions clamp");
    Check(Percent(Stage::Merging, 9) == Percent(Stage::Merging, 1), "fractions above one clamp");
    Check(Percent(Stage::Merging, std::nan("")) == Percent(Stage::Merging, 0), "NaN counts as not started");
    Check(Percent(Stage::Lighting, 0) > 50, "the editor's builds carry most of the bar");
    Check(std::string(Info(Stage::Merging).name) == "Merging fragmented geometry", "stage names");
    Check(CancelAllowed(Stage::Lighting) && CancelAllowed(Stage::Paths), "cancel allowed before saving");
    Check(!CancelAllowed(Stage::Saving) && !CancelAllowed(Stage::Verifying), "no cancel once files are written");
}

void Summaries()
{
    Counts counts;
    counts.cells = 1803; counts.cellFaces = 10496; counts.joinedBrushes = 1205; counts.brushes = 797;
    counts.brushFaces = 4703; counts.grown = 311; counts.absorbed = 97; counts.sheetBrushes = 61;
    counts.polygons = 7123; counts.unifiedSurfaces = 12;
    const auto text = Summary(counts);
    Check(Has(text, "1,803 cells from the compiled BSP -> 797 structural brushes (56% fewer)"), "brush counts and saving");
    Check(Has(text, "plus 61 sheet brushes"), "sheet brushes");
    Check(Has(text, "10,496 -> 4,703 (55% fewer)"), "face counts");
    Check(Has(text, "7,123 polygons"), "polygon count");
    Check(Has(text, "598 cells joined") && Has(text, "311 brushes grown") && Has(text, "97 more absorbed")
          && Has(text, "12 matching surfaces"), "merge details");
    Check(!Has(text, "split back"), "no split-back note when nothing was split");
    counts.splitBack = 3;
    Check(Has(Summary(counts), "3 grown brushes were split back"), "split-back note");
    counts.merged = false;
    counts.brushes = counts.joinedBrushes;
    const auto off = Summary(counts);
    Check(Has(off, "Merging was off") && Has(off, "only cells sharing whole faces") && !Has(off, "brushes grown"),
          "merging off: only face joining described");
    Check(Thousands(0) == "0" && Thousands(999) == "999" && Thousands(1000) == "1,000" && Thousands(1234567) == "1,234,567",
          "thousands separators");
    Check(Fewer(0, 0).empty() && Fewer(10, 12).empty() && Fewer(200, 100) == " (50% fewer)", "percent fewer");

    ReportRow actor{"Lighting recalculated", "StaticMeshActor12", "vertex count changed", Target::Actor, "StaticMeshActor12"};
    ReportRow surface{"Lighting not preserved", "Surface 40", "80% of texels", Target::Surface, "", 40};
    Check(TargetText(actor) == "StaticMeshActor12" && TargetText(surface) == "surface 40"
          && TargetText(ReportRow{}).empty(), "row targets");
    Check(RowsText({actor, surface}) == "Lighting recalculated\tStaticMeshActor12\tvertex count changed\n"
          "Lighting not preserved\tSurface 40\t80% of texels\n", "report rows as text");
}

void Rules()
{
    Check(IsThinBrush(0.5, 1e6) && IsThinBrush(10, 32) && !IsThinBrush(1, 64) && !IsThinBrush(144, 1.9e7),
          "thin brush rule");
    Check(!LightingNotPreserved(1000, 63) && !LightingNotPreserved(100, 64) && LightingNotPreserved(64, 64)
          && LightingNotPreserved(0, 500) && !LightingNotPreserved(0, 10), "lighting rule");

    SurfaceMapping floor;
    floor.material = "Ship_TXT.Floor.metal";
    floor.normal[2] = 1;
    floor.u[0] = 1;
    floor.v[1] = -1;
    floor.origin[0] = 64; floor.origin[1] = 32;
    SurfaceMapping same = floor;
    same.material = "ship_txt.floor.METAL";
    same.origin[2] = 500; // moving the origin along the normal changes nothing
    Check(SameMapping(floor, same), "same mapping despite case and an origin moved along the normal");
    SurfaceMapping shifted = floor;
    shifted.origin[0] = 65;
    Check(!SameMapping(floor, shifted), "an origin moved along U shifts the texture");
    SurfaceMapping nearly = floor;
    nearly.origin[1] = 32.004;
    Check(SameMapping(floor, nearly), "a sub-texel origin difference is the same mapping");
    SurfaceMapping flagged = floor;
    flagged.flags = 0x40;
    Check(!SameMapping(floor, flagged), "different flags");
    SurfaceMapping scaled = floor;
    scaled.u[0] = 2;
    Check(!SameMapping(floor, scaled), "different texture scale");
    SurfaceMapping other = floor;
    other.material = "Ship_TXT.Floor.wood";
    Check(!SameMapping(floor, other), "different material");
    SurfaceMapping sheet = floor;
    sheet.structural = false;
    Check(!SameMapping(floor, sheet), "sheets are never combined");
    SurfaceMapping turned = floor;
    turned.normal[2] = -1;
    Check(!SameMapping(floor, turned), "opposite facing");

    const auto unified = UnifyMappings({floor, other, same, shifted, nearly, sheet, sheet});
    Check(unified == std::vector<int>({0, 1, 0, 3, 0, 5, 6}), "representatives are the first matching surface");
    Check(UnifyMappings({}).empty(), "no surfaces");
}

void AssetPackages()
{
    Check(AssetPackageName("OffsD_Recovered") == "OffsD_Recovered_Assets", "the asset package is named after the recovered map");
    Check(AssetPackageName("Off-s D") == "Off_s_D_Assets", "other characters become underscores");
    Check(AssetPackageName("2Fort") == "Recovery_2Fort_Assets", "a leading digit gets a prefix");
}

int main()
{
    try
    {
        Stages();
        Summaries();
        Rules();
        AssetPackages();
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "Map recovery model tests passed (" << checks << " checks)\n";
    return 0;
}
