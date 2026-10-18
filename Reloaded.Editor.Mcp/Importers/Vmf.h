#pragma once
// Hammer .vmf files to scct.level-spec: convex brushes inverted for SCCT's
// solid world, plus player starts and lights (BrushImportModel.h).
#include "../BrushImportModel.h"

namespace Mcp::Import
{
    inline Json ImportVmf(const std::string& text, const Json& options, Json& report) { return Brushes::ImportVmf(text, options, report); }
}
