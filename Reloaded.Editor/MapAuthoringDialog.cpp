#include "pch.h"
#include "MapAuthoringDialog.h"
#include "WorkflowEditor.h"
#include "MapAuthoringModel.h"
#include "WindowPlacement.h"
#include <algorithm>
#include <commdlg.h>
#include <commctrl.h>
#include <vector>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace MapAuthoringDialog
{
namespace
{
    constexpr int kHeading=10,kList=11,kStatus=12,kTickAll=13;
    constexpr std::streamoff kMaximumExport=256LL*1024*1024;
    // Streams the export to disk. Pretty-printing a large map's export into one string needs a contiguous block
    // the 32-bit editor rarely has once the map is loaded (ShipD failed with "bad allocation").
    void WriteExport(const std::filesystem::path& path,const Workflow::Json& document)
    {
        auto temp=path;temp+=".tmp";std::error_code error;
        try
        {
            std::ofstream out(temp,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("Cannot create "+temp.string());
            out<<std::setw(2)<<document;out.flush();
            if(!out)throw std::runtime_error("Could not write "+temp.string());
            if(out.tellp()>kMaximumExport)throw std::runtime_error("The export exceeds 256 MiB.");
        }
        catch(...){std::filesystem::remove(temp,error);throw;}
        std::filesystem::rename(temp,path,error);
        if(error){std::filesystem::remove(temp,error);throw std::runtime_error("Could not replace "+path.string());}
    }
    struct Preview { const Review* review=nullptr;std::vector<bool> ticked;bool accepted=false; };
    std::wstring Wide(const std::string& text)
    {
        auto count=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);
        std::wstring result(count,L'\0');MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),count);return result;
    }
    void Status(HWND window,const std::wstring& text){SetWindowTextW(GetDlgItem(window,kStatus),text.c_str());}
    size_t Ticked(const Preview& state){return static_cast<size_t>(std::count(state.ticked.begin(),state.ticked.end(),true));}
    void ShowTicked(HWND window,const Preview& state)
    {
        if(!state.review->checkboxes)return;
        const auto n=Ticked(state);EnableWindow(GetDlgItem(window,IDOK),n>0);
        Status(window,std::to_wstring(n)+L" row(s) ticked.");
    }
    void Toggle(HWND window,Preview& state,int index)
    {
        if(!state.review->checkboxes || index<0 || index>=static_cast<int>(state.ticked.size()) || !state.review->rows[index].checkable)return;
        state.ticked[index]=!state.ticked[index];ListView_RedrawItems(GetDlgItem(window,kList),index,index);ShowTicked(window,state);
    }
    void Choose(HWND window,Preview& state,int index)
    {
        if(index<0 || index>=static_cast<int>(state.review->rows.size()))return;
        const auto& line=state.review->rows[index];
        if(line.select.is_null()){Status(window,line.cells[1]+L" is not in the open map, so there is nothing to select.");return;}
        try{Workflow::Editor::Select(Workflow::Json::array({line.select}),true);Status(window,L"Selected "+line.cells[1]+L" in the map.");}
        catch(const std::exception& e){Status(window,L"Could not select "+line.cells[1]+L": "+Wide(e.what()));}
    }
    LRESULT CALLBACK PreviewProc(HWND window,UINT message,WPARAM w,LPARAM l)
    {
        auto state=reinterpret_cast<Preview*>(GetWindowLongPtr(window,GWLP_USERDATA));
        if(message==WM_CREATE)
        {
            state=static_cast<Preview*>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams);SetWindowLongPtr(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
            const auto& review=*state->review;
            // Created empty: an edit control refuses creation text beyond its default 30,000-character limit. Raise it, then set the text.
            auto edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,0,0,0,0,window,reinterpret_cast<HMENU>(kHeading),nullptr,nullptr);
            SendMessage(edit,EM_SETLIMITTEXT,0,0);SetWindowTextW(edit,review.heading.c_str());
            // A virtual list: a large change file has tens of thousands of rows.
            auto list=CreateWindowExW(WS_EX_CLIENTEDGE,WC_LISTVIEWW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|LVS_OWNERDATA,0,0,0,0,window,reinterpret_cast<HMENU>(kList),nullptr,nullptr);
            ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_LABELTIP|(review.checkboxes?LVS_EX_CHECKBOXES:0));
            // A virtual list keeps no item state: the tick boxes come from the rows, asked for row by row.
            if(review.checkboxes)ListView_SetCallbackMask(list,LVIS_STATEIMAGEMASK);
            for(int i=0;i<5;++i){LVCOLUMNW column{};column.mask=LVCF_TEXT|LVCF_WIDTH;column.pszText=const_cast<wchar_t*>(review.columns[i]);column.cx=review.widths[i];ListView_InsertColumn(list,i,&column);}
            ListView_SetItemCountEx(list,static_cast<int>(review.rows.size()),0);
            CreateWindowW(L"STATIC",review.hint.c_str(),WS_CHILD|WS_VISIBLE|SS_LEFTNOWORDWRAP,0,0,0,0,window,reinterpret_cast<HMENU>(kStatus),nullptr,nullptr);
            CreateWindowW(L"BUTTON",review.apply.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,0,0,0,0,window,reinterpret_cast<HMENU>(IDOK),nullptr,nullptr);
            CreateWindowW(L"BUTTON",review.cancel.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);
            if(review.checkboxes)CreateWindowW(L"BUTTON",L"Tick &All",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,0,0,window,reinterpret_cast<HMENU>(kTickAll),nullptr,nullptr);
            for(int id:{kHeading,kList,kStatus,static_cast<int>(IDOK),static_cast<int>(IDCANCEL),kTickAll})if(auto child=GetDlgItem(window,id))SendMessage(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
            if(review.checkboxes)EnableWindow(GetDlgItem(window,IDOK),FALSE);
            return 0;
        }
        if(message==WM_SIZE)
        {
            int width=LOWORD(l),height=HIWORD(l);const bool ticks=state && state->review->checkboxes;
            const int apply=ticks?170:130,left=ticks?100:0;
            MoveWindow(GetDlgItem(window,kHeading),12,12,width-24,96,TRUE);
            MoveWindow(GetDlgItem(window,kList),12,116,width-24,height-176,TRUE);
            if(ticks)MoveWindow(GetDlgItem(window,kTickAll),12,height-44,88,30,TRUE);
            MoveWindow(GetDlgItem(window,kStatus),12+left,height-37,width-160-apply-left,18,TRUE);
            MoveWindow(GetDlgItem(window,IDOK),width-130-apply,height-44,apply,30,TRUE);
            MoveWindow(GetDlgItem(window,IDCANCEL),width-120,height-44,108,30,TRUE);return 0;
        }
        if(message==WM_NOTIFY && state)
        {
            auto header=reinterpret_cast<NMHDR*>(l);const auto& rows=state->review->rows;
            if(header->idFrom==kList && header->code==LVN_GETDISPINFOW)
            {
                auto& item=reinterpret_cast<NMLVDISPINFOW*>(l)->item;
                if(item.iItem>=0 && item.iItem<static_cast<int>(rows.size()))
                {
                    if((item.mask&LVIF_TEXT) && item.iSubItem>=0 && item.iSubItem<5)item.pszText=const_cast<wchar_t*>(rows[item.iItem].cells[item.iSubItem].c_str());
                    // State image 0 is no box (a row that cannot be ticked), 1 unticked, 2 ticked.
                    if((item.mask&LVIF_STATE) && state->review->checkboxes)
                    {item.state=INDEXTOSTATEIMAGEMASK(rows[item.iItem].checkable?(state->ticked[item.iItem]?2:1):0);item.stateMask|=LVIS_STATEIMAGEMASK;}
                }
                return 0;
            }
            if(header->idFrom==kList && header->code==NM_CLICK)
            {
                LVHITTESTINFO hit{};hit.pt=reinterpret_cast<NMITEMACTIVATE*>(l)->ptAction;
                if(ListView_SubItemHitTest(header->hwndFrom,&hit)>=0 && (hit.flags&LVHT_ONITEMSTATEICON))Toggle(window,*state,hit.iItem);
                return 0;
            }
            if(header->idFrom==kList && header->code==LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(l)->wVKey==VK_SPACE)
            {Toggle(window,*state,ListView_GetNextItem(header->hwndFrom,-1,LVNI_SELECTED));return 0;}
            if(header->idFrom==kList && header->code==LVN_ITEMCHANGED)
            {
                auto change=reinterpret_cast<NMLISTVIEW*>(l);
                if((change->uChanged&LVIF_STATE) && (change->uNewState&LVIS_SELECTED) && !(change->uOldState&LVIS_SELECTED))Choose(window,*state,change->iItem);
                return 0;
            }
        }
        if(message==WM_GETMINMAXINFO){auto info=reinterpret_cast<MINMAXINFO*>(l);info->ptMinTrackSize={560,380};return 0;}
        if(message==WM_COMMAND && LOWORD(w)==kTickAll && state)
        {
            // Ticks every row that can be ticked; when they all are, unticks them.
            bool all=true;for(size_t i=0;i<state->ticked.size();++i)if(state->review->rows[i].checkable && !state->ticked[i])all=false;
            for(size_t i=0;i<state->ticked.size();++i)state->ticked[i]=state->review->rows[i].checkable && !all;
            InvalidateRect(GetDlgItem(window,kList),nullptr,FALSE);ShowTicked(window,*state);return 0;
        }
        if(message==WM_COMMAND && (LOWORD(w)==IDOK || LOWORD(w)==IDCANCEL))
        {
            if(LOWORD(w)==IDOK && state->review->checkboxes && !Ticked(*state)){Status(window,L"Tick the rows to bring back first.");return 0;}
            state->accepted=LOWORD(w)==IDOK;DestroyWindow(window);return 0;
        }
        if(message==WM_CLOSE){DestroyWindow(window);return 0;}
        if(message==WM_DESTROY && state && state->review->placement)WindowPlacement::Remember(window,state->review->placement);
        return DefWindowProc(window,message,w,l);
    }
    bool Confirm(HWND owner,const Workflow::Json& preview,const Workflow::Json& document)
    {
        auto map=document.at("map").get<std::string>();if(map.rfind("unsaved:",0)==0)map="Unsaved map (current editor session)";else if(map=="*")map="The map that is open";
        std::string text="Map: "+map+"\r\n";
        if(!document.value("description",std::string{}).empty())text+=document.at("description").get<std::string>()+"\r\n";
        text+="Changes: "+preview.value("tally",std::string{})+"\r\n"+preview.at("note").get<std::string>()+"\r\n";
        if(Workflow::Authoring::Deletes(document))text+="This file deletes actors (allowDeletes is on). Undo brings them back.\r\n";
        Review review;review.title=L"Preview Map JSON Import";review.heading=Wide(text);review.hint=L"Click a row to select its actor in the map.";review.apply=L"Apply changes";
        for(const auto& row:preview.value("rows",Workflow::Json::array()))
        {
            ReviewRow line;int i=0;
            for(const char* key:{"change","target","property","before","after"})line.cells[i++]=Cell(row.value(key,std::string{}));
            line.select=row.value("select",Workflow::Json());review.rows.push_back(std::move(line));
        }
        return Show(owner,review);
    }
}
std::wstring Cell(const std::string& text)
{
    // Long values (whole arrays) are shortened in the list; the change file holds them in full.
    auto wide=Wide(text);for(auto& c:wide)if(c==L'\r' || c==L'\n' || c==L'\t')c=L' ';
    if(wide.size()>300)wide=wide.substr(0,297)+L"...";
    return wide;
}
bool Show(HWND owner,const Review& review,std::vector<size_t>* ticked)
{
    Preview state{&review,std::vector<bool>(review.rows.size(),false)};
    WNDCLASSW cls{};cls.lpfnWndProc=PreviewProc;cls.hInstance=GetModuleHandle(nullptr);cls.lpszClassName=L"ReloadedMapAuthoringPreview";cls.hCursor=LoadCursor(nullptr,IDC_ARROW);cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassW(&cls);
    auto window=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,review.title.c_str(),WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,CW_USEDEFAULT,920,660,owner,nullptr,cls.hInstance,&state);
    if(!window)throw std::runtime_error("Could not open the change list.");
    if(review.placement)WindowPlacement::Restore(window,review.placement);
    EnableWindow(owner,FALSE);ShowWindow(window,SW_SHOW);MSG message{};
    while(IsWindow(window))
    {
        auto status=GetMessage(&message,nullptr,0,0);
        if(status<=0){if(status==0)PostQuitMessage(static_cast<int>(message.wParam));break;}
        if(!IsDialogMessage(window,&message)){TranslateMessage(&message);DispatchMessage(&message);}
    }
    if(IsWindow(window))DestroyWindow(window);EnableWindow(owner,TRUE);SetActiveWindow(owner);
    if(ticked){ticked->clear();for(size_t i=0;i<state.ticked.size();++i)if(state.ticked[i])ticked->push_back(i);}
    return state.accepted;
}
void Open(HWND owner,bool exporting)
{
    try
    {
        wchar_t path[32768]{};wcscpy_s(path,exporting?L"map.json":L"map-changes.json");
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=owner;dialog.lpstrFilter=L"Map JSON (*.json)\0*.json\0\0";
        dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"json";dialog.lpstrTitle=exporting?L"Export Map to JSON":L"Import Map from JSON";
        dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(exporting?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
        if(!(exporting?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog))){if(CommDlgExtendedError())throw std::runtime_error("File dialog failed.");return;}
        if(exporting)
        {
            WriteExport(std::filesystem::path(path),Workflow::Editor::ExportMapAuthoring());
            MessageBoxW(owner,L"Map exported to JSON. The file contains actor properties, available classes and assets, geometry context, and a template for importing changes.",L"Map JSON",MB_OK);return;
        }
        if(std::filesystem::file_size(path)>128*1024*1024)throw std::runtime_error("Change file exceeds 128 MiB.");
        std::ifstream input(std::filesystem::path(path),std::ios::binary);if(!input)throw std::runtime_error("Cannot read the change file.");
        auto document=Workflow::Json::parse(input,[](int depth,Workflow::Json::parse_event_t,Workflow::Json&){if(depth>64)throw std::runtime_error("Change file is nested too deeply.");return true;});
        Workflow::Authoring::Rebase(document,std::filesystem::path(path).parent_path()); // Texture files beside the change file.
        auto preview=Workflow::Editor::PreviewMapAuthoring(document);
        // A row click selects its actor, which is not a change to the map, so the check leaves the selection out.
        const auto level=Workflow::Editor::LevelIdentity();const auto generation=Workflow::Editor::MapGeneration();const auto revision=Workflow::Editor::ContentRevision();
        if(!Confirm(owner,preview,document))return;
        if(level!=Workflow::Editor::LevelIdentity() || generation!=Workflow::Editor::MapGeneration() || revision!=Workflow::Editor::ContentRevision())throw std::runtime_error("Map changed during preview. Import again to review the current changes.");
        Workflow::Editor::ApplyMapAuthoring(document);
        MessageBoxW(owner,L"Map JSON changes imported. Undo reverses the batch in one step. Save the map to keep the changes.",L"Map JSON",MB_OK);
    }
    catch(const std::exception& e){MessageBoxW(owner,Wide(e.what()).c_str(),L"Map JSON",MB_OK|MB_ICONERROR);}
}
}
