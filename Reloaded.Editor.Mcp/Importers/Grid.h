#pragma once
// ASCII and CSV grids (one character per cell) to scct.level-spec: the
// legend, storeys and options are GridPlanModel.h's.
#include "../GridPlanModel.h"

namespace Mcp::Import
{
    inline Json ImportGrid(const std::string& text, const Json& options, Json& report) { return Grid::ImportText(text, options, report); }
}
