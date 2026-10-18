#pragma once
// The files under Reloaded.Editor.Mcp/Resources (wiki digests, schemas,
// examples, docs, prompt templates), compiled into the exe so it needs no
// files beside it. tools/mcp/embed_resources.py writes Embedded.gen.h, which
// defines the arrays and EmbeddedFiles() inline; Resources.cpp serves them as
// scct://<path without extension>.
#include <cstddef>
#include <vector>

namespace Mcp
{
    // path is relative to Resources with forward slashes ("wiki/game-modes.md").
    struct EmbeddedFile { const char* path; const char* mime; const unsigned char* data; size_t size; };
    const std::vector<EmbeddedFile>& EmbeddedFiles();
}

#include "Embedded.gen.h"
