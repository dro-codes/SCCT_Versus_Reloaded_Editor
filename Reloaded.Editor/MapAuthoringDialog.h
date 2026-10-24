#pragma once
#include <windows.h>
#include "WorkflowModel.h"
#include <filesystem>
#include <string>
#include <vector>
namespace MapAuthoringDialog
{
    constexpr UINT Export=40934,Import=40935;
    void Open(HWND owner,bool exporting);

    // The change list the Map JSON import previews with, for other tools to show their rows in: a heading, five
    // columns, a status line, the apply button and Cancel, modal over the owner. A row click selects the row's
    // actor in the map (select is its identity, or null). With checkboxes, rows marked checkable can be ticked
    // (a click on the box, Space, or Tick All) and the apply button needs at least one.
    struct ReviewRow { std::wstring cells[5]; Workflow::Json select; bool checkable=false; };
    struct Review
    {
        std::wstring title,heading,hint,apply,cancel=L"Cancel";
        const wchar_t* columns[5]={L"Change",L"Actor",L"Property",L"Before",L"After"};
        int widths[5]={60,180,130,250,250};
        bool checkboxes=false;
        const char* placement=nullptr; // WindowPlacement key, or none
        std::vector<ReviewRow> rows;
    };
    // True when the apply button closed it; ticked then holds the ticked rows, in order. Only one change list is
    // open at a time: a second while one is open throws.
    bool Show(HWND owner,const Review& review,std::vector<size_t>* ticked=nullptr);
    // A list cell: UTF-8 to UTF-16, line breaks to spaces, long values shortened.
    std::wstring Cell(const std::string& text);
    // The change preview Import Map from JSON shows (preview: PreviewMapAuthoring's
    // {rows, tally, note}; document: {map, description} at least), for the MCP
    // bridge's approval. A nested modal loop over owner (Show); true when the user
    // chose Approve.
    bool Approve(HWND owner,const Workflow::Json& preview,const Workflow::Json& document);
    // Closes an open change list as Cancel (its Show returns false); nothing when none is open.
    void CancelReview();
    bool Reviewing();
    // Streams a document to path as indented JSON, through a temporary file
    // beside it, refusing more than 256 MiB. Throws when it cannot be written.
    void WriteExport(const std::filesystem::path& path,const Workflow::Json& document);
}
