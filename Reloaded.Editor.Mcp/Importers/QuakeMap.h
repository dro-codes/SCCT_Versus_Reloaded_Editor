#pragma once
// Quake-family .map files (Quake, Half-Life Valve 220, Quake 3 brushDef,
// Doom 3 brushDef3) to scct.level-spec: convex brushes inverted for SCCT's
// solid world, plus player starts and lights (BrushImportModel.h).
#include "../BrushImportModel.h"

namespace Mcp::Import
{
    inline Json ImportQuakeMap(const std::string& text, const Json& options, Json& report) { return Brushes::ImportQuake(text, options, report); }
}
