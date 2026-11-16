#pragma once
#include "WorkflowModel.h"
#include <string>
#include <vector>

namespace SoundBrowser {
    void Initialize();
    void* PeekSelectedSound();
    // Build UAS without its dialogs (the MCP bridge's uas.build): the map's
    // Packages\Sounds\<mapName>.uas from the Stream sounds of packages, OGG at
    // quality (0-10, 5 is the shipped default), as the Sound Browser button
    // builds it. A package not in memory is loaded from Packages\Sounds first;
    // one with no file there is skipped. Returns {file, size, packages,
    // loaded, skipped, encoded, sounds:[{package, name, status}], log}.
    // Throws, naming the missing sounds, when the build left any out.
    Workflow::Json BuildUas(const std::string& mapName, const std::vector<std::string>& packages, float quality);
}
