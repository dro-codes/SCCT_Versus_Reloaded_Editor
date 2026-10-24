#pragma once
#include <windows.h>
#include "WorkflowModel.h"
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
    // True when the apply button closed it; ticked then holds the ticked rows, in order.
    bool Show(HWND owner,const Review& review,std::vector<size_t>* ticked=nullptr);
    // A list cell: UTF-8 to UTF-16, line breaks to spaces, long values shortened.
    std::wstring Cell(const std::string& text);
}
