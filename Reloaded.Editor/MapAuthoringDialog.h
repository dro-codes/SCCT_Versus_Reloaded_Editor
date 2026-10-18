#pragma once
#include <windows.h>
#include "WorkflowModel.h"
#include <filesystem>
namespace MapAuthoringDialog
{
    constexpr UINT Export=40934,Import=40935;
    void Open(HWND owner,bool exporting);
    // The change preview Import Map from JSON shows (preview: PreviewMapAuthoring's
    // {rows, tally, note}; document: {map, description} at least), for the MCP
    // bridge's approval. A nested modal loop over owner; true when the user chose
    // Apply. Only one review is open at a time: a second while one is open throws.
    bool Review(HWND owner,const Workflow::Json& preview,const Workflow::Json& document);
    // Closes an open review as Cancel (its Review returns false); nothing when none is open.
    void CancelReview();
    bool Reviewing();
    // Streams a document to path as indented JSON, through a temporary file
    // beside it, refusing more than 256 MiB. Throws when it cannot be written.
    void WriteExport(const std::filesystem::path& path,const Workflow::Json& document);
}
