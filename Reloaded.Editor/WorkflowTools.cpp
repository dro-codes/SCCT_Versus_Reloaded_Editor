#include "pch.h"
#undef min
#undef max
#include "WorkflowTools.h"
#include "WindowPlacement.h"
#include "CharacterSkinsWindow.h"
#include "ObjImportWindow.h"
#include "CharacterSkinPresetsWindow.h"
#include "EmitterLibraryWindow.h"
#include "EditorExtras.h"
#include "StoreyFilter.h"
#include "PropertySearch.h"
#include "UndoHistory.h"
#include "FavoritesWindow.h"
#include "CompareBackup.h"
#include "LightingBudget.h"
#include "MapCheck.h"
#include "RenderBudget.h"
#include "MapOutliner.h"
#include "MoverWorkbench.h"
#include "MinimapWindow.h"
#include "WorkflowEditor.h"
#include "EmitterLibraryModel.h"
#include "WorkflowGraph.h"
#include "MagicEventWorkbench.h"
#include "CameraNetworkPanel.h"
#include "MapAuthoringDialog.h"
#include "MapAuthoringModel.h"
#include "MapPackageDialog.h"
#include "MapOptimiseDialog.h"
#include "ObjExportWindow.h"
#include "MapUsagesWindow.h"
#include "MapRecovery.h"
#include "MemoryWriter.h"
#include "MapDesignModel.h"
#include "SecurityModel.h"
#include "StageModel.h"
#include "ObjectivePlayersModel.h"
#include "EmitterPreview.h"
#include "CharacterPreview.h"
#include "MeasureTool.h"
#include "CommandPalette.h"
#include "PlacementTools.h"
#include "ScatterTool.h"
#include "PropertyPaste.h"
#include "LightShadowMap.h"
#include "LightShadowModel.h"
#include "Reachability.h"
#include "ReachabilityModel.h"
#include "EntryThumbnailModel.h"
#include "ViewBookmarkModel.h"
#include "PrefabModel.h"
#include "UsageMarksModel.h"
#include "SaveBackupModel.h"
#include "CrashRecoveryModel.h"
#include "LevelSnapshot.h"
#include "logger.h"
#include <commdlg.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"gdiplus.lib")

namespace WorkflowTools
{
using namespace Workflow;
namespace
{
    constexpr int kList=100,kName=101,kStatus=102,kFollow=103,kScope=104;
    enum Button { Refresh=200,Primary,Secondary,Third,Fourth,Fifth,Sixth,Seventh,Eighth,Ninth,Tenth,Eleventh,Twelfth,
                  // Actor Assemblies' linked prefab buttons.
                  PlaceLinkedButton,SelectInstanceButton,SelectAllInstancesButton,UpdatePrefabButton,BreakLinkButton,LastButton=BreakLinkButton };
    constexpr int UpdateThumbnail=Twelfth; // List right-click menu only.
    enum class Kind { Assets,Connections,Views,Assemblies };
    // [WindowPlacement] keys, one per kind of window: Working Views opens
    // where a Working Views window was last closed.
    const char* PlacementKey(Kind kind)
    {
        return kind==Kind::Assets?"FindUsages":kind==Kind::Connections?"GameplayConnections":kind==Kind::Views?"WorkingViews":"ActorAssemblies";
    }
    Json ObjectiveOwner(const std::string& path = "")
    {
        auto actors=path.empty()?Editor::Actors(true):Editor::Actors();
        if(path.empty())return actors.size()==1?actors[0]:Json{};
        for(const auto& actor:actors)if(actor.at("path")==path)return actor;
        return {};
    }
    // Objective player counts (ObjectivePlayersModel.h), shared by the native menus
    // and Map Design: the missions (from ObjectiveMissions or ObjectiveActors) and the
    // map's rules (from ObjectivePlayerRules) as the model plays them out. The menus and
    // the Properties strip read ObjectiveMissions only: ObjectiveActors can select a
    // player start to read its team, and the Properties window follows the selection.
    ObjectivePlayers::Missions PlayerMissions(const Json& objectiveActors)
    {
        ObjectivePlayers::Missions missions;
        for(const auto& actor:objectiveActors)
        {
            if(actor.value("kind",std::string())!="Mission")continue;
            ObjectivePlayers::Mission mission;
            for(const auto& reference:actor.value("objectives",Json::array()))
                if(reference.is_string())mission.objectives.push_back(ObjectivePlayers::ReferencePath(reference.get<std::string>()));
            mission.minimum=actor.value("minimum",0);
            missions[ObjectivePlayers::Fold(actor.value("path",std::string()))]=mission;
        }
        return missions;
    }
    ObjectivePlayers::Rules PlayerRules(const Json& state)
    {
        ObjectivePlayers::Rules rules;
        for(const auto& rule:state.value("rules",Json::array()))
            rules[ObjectivePlayers::Fold(rule.value("path",std::string()))]={rule.value("min",0),rule.value("max",0)};
        return rules;
    }
    ObjectivePlayers::Targets PlayerTargets(const Json& state)
    {
        ObjectivePlayers::Targets targets;
        for(const auto& entry:state.value("targets",Json::array()))
        {
            std::array<int,ObjectivePlayers::TeamSizes> sizes{};
            const auto values=entry.value("sizes",Json::array());
            for(int k=0;k<ObjectivePlayers::TeamSizes && k<static_cast<int>(values.size());++k)sizes[k]=values[k].get<int>();
            targets[ObjectivePlayers::Fold(entry.value("path",std::string()))]=sizes;
        }
        return targets;
    }
    std::array<int,ObjectivePlayers::TeamSizes> MissionTargets(const Json& state,const std::string& path)
    {
        const auto targets=PlayerTargets(state);
        const auto found=targets.find(ObjectivePlayers::Fold(path));
        return found==targets.end()?std::array<int,ObjectivePlayers::TeamSizes>{}:found->second;
    }
    // The top mission is the game mode's own: no rule may leave it out.
    bool PlayerRuleAllowed(const Json& objectiveActors,const std::string& path)
    {
        if(!Editor::Compatible(path,"SBase.SMission"))return true;
        const auto roots=ObjectivePlayers::Roots(PlayerMissions(objectiveActors));
        return std::find(roots.begin(),roots.end(),ObjectivePlayers::Fold(path))==roots.end();
    }
    // The presets of the Players in Match submenu, by command.
    const std::pair<UINT,ObjectivePlayers::Rule> kPlayerPresets[]={
        {kObjectivePlayersFirst,{0,0}},{kObjectivePlayersFirst+1,{2,0}},{kObjectivePlayersFirst+2,{4,0}},{kObjectivePlayersFirst+3,{6,0}},
        {kObjectivePlayersFirst+4,{8,0}},{kObjectivePlayersFirst+5,{0,2}},{kObjectivePlayersFirst+6,{0,4}},{kObjectivePlayersFirst+7,{0,6}}};
    // Whether a mover's rule opens it inside its range.
    bool PlayerOpen(const Json& state,const std::string& path)
    {
        for(const auto& rule:state.value("rules",Json::array()))
            if(ObjectivePlayers::Fold(rule.value("path",std::string()))==ObjectivePlayers::Fold(path))return rule.value("open",false);
        return false;
    }
    void PlayersMenu(HMENU menu,const std::string& path)
    {
        const auto actors=Editor::ObjectiveMissions();
        if(!PlayerRuleAllowed(actors,path))return;
        const auto state=Editor::ObjectivePlayerRules();
        const auto rules=PlayerRules(state);
        const auto found=rules.find(ObjectivePlayers::Fold(path));
        const ObjectivePlayers::Rule current=found==rules.end()?ObjectivePlayers::Rule{}:found->second;
        const bool mover=Editor::Compatible(path,"Mover");
        auto sub=CreatePopupMenu();if(!sub)return;
        if(mover)
        {
            AppendMenuA(sub,MF_STRING|MF_GRAYED,0,"Outside the range: closed and locked");
            AppendMenuA(sub,MF_SEPARATOR,0,nullptr);
        }
        bool preset=false;
        for(const auto& [command,rule]:kPlayerPresets)
        {
            const bool checked=rule==current;preset|=checked;
            const auto label=ObjectivePlayers::Empty(rule)?std::string("&Every match"):
                             rule.minimum?"&"+std::to_string(rule.minimum)+"+ players":"Up to &"+std::to_string(rule.maximum)+" players";
            AppendMenuA(sub,MF_STRING|(checked?MF_CHECKED:0),command,label.c_str());
            if(command==kObjectivePlayersFirst || command==kObjectivePlayersFirst+4)AppendMenuA(sub,MF_SEPARATOR,0,nullptr);
        }
        const auto custom=preset?std::string("&Custom..."):"&Custom ("+ObjectivePlayers::Short(current)+")...";
        AppendMenuA(sub,MF_STRING|(preset?0:MF_CHECKED),kObjectivePlayersCustom,custom.c_str());
        if(mover)
        {
            AppendMenuA(sub,MF_SEPARATOR,0,nullptr);
            AppendMenuA(sub,MF_STRING|(PlayerOpen(state,path)?MF_CHECKED:0)|(ObjectivePlayers::Empty(current)?MF_GRAYED:0),kObjectivePlayersOpen,
                        "&Open at the start when in range");
        }
        const auto title="&Players in Match ("+(ObjectivePlayers::Empty(current)?std::string("every match"):ObjectivePlayers::Short(current))+")";
        AppendMenuA(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(sub),title.c_str());
    }
    bool ObjectiveMenu(HMENU menu,const Json& owner)
    {
        if(owner.is_null())return false;
        const auto path=owner.at("path").get<std::string>();
        if(Editor::Compatible(path,"SBase.SMission"))
        {
            AppendMenuA(menu,MF_STRING,kAddObjective,"Add SObjective");
            // Any mission, the top one included: how many objectives win it in each match size.
            std::string label="Objectives to &Win by Match Size...";
            try
            {
                const auto summary=ObjectivePlayers::TargetsText(MissionTargets(Editor::ObjectivePlayerRules(),path));
                if(!summary.empty())label="Objectives to &Win by Match Size ("+summary+")...";
            }
            catch(const std::exception&){}
            AppendMenuA(menu,MF_STRING,kObjectiveTargets,label.c_str());
        }
        else if(Editor::Compatible(path,"SBase.SObjective"))
        {
            AppendMenuA(menu,MF_STRING,kAddComputerObjective,"Add SComputerObjectiveTrigger");
            AppendMenuA(menu,MF_STRING,kAddBombObjective,"Add SBombTargetObjectiveTrigger");
            AppendMenuA(menu,MF_STRING,kAddFlagObjective,"Add SFlag && SFlagDropZone");
        }
        else if(Editor::Compatible(path,"Mover"))
        {
            // A mover takes a player count only: rooms that open in big games.
            try{PlayersMenu(menu,path);}catch(const std::exception&){return false;}
            return true;
        }
        else return false;
        try{PlayersMenu(menu,path);}catch(const std::exception&){ /* The objective items still work. */ }
        return true;
    }
    void AddObjective(UINT command,const Json& snapshot)
    {
        const char* types[]={"SBase.SObjective","SBase.SComputerObjectiveTrigger","SBase.SBombTargetObjectiveTrigger","SBase.SFlag"};
        Editor::AddObjectiveActor(snapshot,types[command-kAddObjective]);
    }
    // documentRevision lets panels cache their slice of the library instead of
    // copying the whole document on every repaint.
    Json document; uintptr_t level=0; std::string mapKey; bool loaded=false; unsigned mapEpoch=0,nativeMapGeneration=0,documentRevision=0;
    std::filesystem::path LibraryPath() { return Editor::Directory()/"library.json"; }
    Json EmptyMap() { return {{"views",Json::array()},{"instances",Json::array()}}; }
    std::filesystem::path ThumbnailFolder() { return Editor::Directory()/Thumbnail::Folder(); }
    // Named by id, so a rename keeps its picture. Throws for an id that is not one.
    std::filesystem::path ThumbnailPath(bool view,const std::string& id) { return ThumbnailFolder()/Thumbnail::FileName(view?Thumbnail::Kind::View:Thumbnail::Kind::Assembly,id); }
    // Pictures no saved entry owns: deleted elsewhere, views of a map that
    // was never saved, interrupted writes. Never allowed to stop a load.
    void SweepThumbnails()
    {
        try
        {
            std::error_code error; std::vector<std::string> files;
            for(std::filesystem::directory_iterator it(ThumbnailFolder(),error),end;!error && it!=end;it.increment(error)) files.push_back(it->path().filename().string());
            std::set<std::string> views,assemblies;
            for(const auto& a:document.at("assemblies")) if(a.is_object() && a.contains("id") && a.at("id").is_string()) assemblies.insert(a.at("id").get<std::string>());
            for(auto map=document.at("maps").begin();map!=document.at("maps").end();++map)
                if(map->is_object() && map->contains("views") && map->at("views").is_array())
                    for(const auto& v:map->at("views")) if(v.is_object() && v.contains("id") && v.at("id").is_string()) views.insert(v.at("id").get<std::string>());
            for(const auto& file:Thumbnail::Orphans(files,views,assemblies)) std::filesystem::remove(ThumbnailFolder()/file,error);
        }
        catch(const std::exception&) {}
    }
    void Save(Json next)
    {
        Json disk=next;
        disk["maps"].erase(""); // Untitled views stay in memory until first save.
        WriteDocument(LibraryPath(),disk); document=std::move(next); ++documentRevision;
    }
    bool Sync()
    {
        if(!loaded)
        {
            document=ReadDocument(LibraryPath(),{{"version",1},{"assemblies",Json::array()},{"maps",Json::object()}}); ++documentRevision;
            if(!document.at("assemblies").is_array() || !document.at("maps").is_object()) throw std::runtime_error("Invalid workflow library; restore a valid copy before editing it.");
            loaded=true; SweepThumbnails();
        }
        auto current=Editor::LevelIdentity(); auto key=Editor::MapKey();auto generation=Editor::MapGeneration(); bool changed=current!=level || key!=mapKey || generation!=nativeMapGeneration;
        if(!changed) return false;
        Json next=document;
        if(level && current==level && generation==nativeMapGeneration && key!=mapKey && !key.empty())
        {
            // Save As carries the entry to the new name. A kept backup saved
            // over the map it was taken from, or an autosave saved over a map,
            // puts back an older version of that map: the map keeps its own
            // entry (Map Design, Working Views, camera bookmarks, assemblies),
            // which the copy never had.
            const bool restored=(SaveBackups::RestoresMap(mapKey,key) || Sessions::IsAutosaveName(mapKey)) && next["maps"].contains(key);
            if(!restored)
            {
                next["maps"][key]=next["maps"].value(mapKey,EmptyMap());
                if(next["maps"][key].contains("design"))for(auto& piece:next["maps"][key]["design"]["pieces"])piece["map"]=key;
            }
        }
        else if(key.empty()) next["maps"][""]=EmptyMap();
        if(!next["maps"].contains(key)) next["maps"][key]=EmptyMap();
        if(current==level && !key.empty()) Save(next); else { document=std::move(next); ++documentRevision; }
        level=current; mapKey=key; nativeMapGeneration=generation; ++mapEpoch; return true;
    }
    HWND Control(HWND parent,const char* type,const std::string& title,DWORD style,int id,int x,int y,int w,int h)
    {
        HWND child=CreateWindowExA(type==std::string("EDIT")?WS_EX_CLIENTEDGE:0,type,title.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,x,y,w,h,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandle(nullptr),nullptr);
        SendMessage(child,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE); return child;
    }
    std::string Text(HWND control)
    {
        int count=GetWindowTextLengthA(control); std::string text(count+1,'\0'); GetWindowTextA(control,text.data(),count+1); text.resize(count); return text;
    }
    // Form controls are ANSI; Emitter Library documents are UTF-8.
    std::string Recode(const std::string& text,UINT from,UINT to)
    {
        if(text.empty()) return {};
        int count=MultiByteToWideChar(from,0,text.data(),static_cast<int>(text.size()),nullptr,0);
        std::wstring wide(count,L'\0'); MultiByteToWideChar(from,0,text.data(),static_cast<int>(text.size()),wide.data(),count);
        count=WideCharToMultiByte(to,0,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr);
        std::string out(count,'\0'); WideCharToMultiByte(to,0,wide.data(),static_cast<int>(wide.size()),out.data(),count,nullptr,nullptr); return out;
    }
    HWND brushVisibilityWindow=nullptr;
    void RefreshBrushVisibility(HWND window)
    {
        const auto rows=Editor::BrushVisibility();
        for(int i=0;i<static_cast<int>(rows.size());++i)
        {
            const int visible=rows[i].at("visible"),total=rows[i].at("total");
            const auto label=rows[i].at("name").get<std::string>()+" ("+std::to_string(visible)+"/"+std::to_string(total)+")";
            SetWindowTextA(GetDlgItem(window,100+i*3),label.c_str());
            SendDlgItemMessage(window,100+i*3,BM_SETCHECK,visible==0?BST_UNCHECKED:visible==total?BST_CHECKED:BST_INDETERMINATE,0);
            for(int j=0;j<3;++j)EnableWindow(GetDlgItem(window,100+i*3+j),total!=0);
        }
    }
    LRESULT CALLBACK BrushVisibilityProc(HWND window,UINT message,WPARAM w,LPARAM l)
    {
        try
        {
            if(message==WM_CREATE)
            {
                Control(window,"STATIC","Visible / total actors. Mixed check = partly hidden.",0,1,12,12,460,22);
                for(int i=0;i<7;++i)
                {
                    Control(window,"BUTTON","",BS_3STATE,100+i*3,12,40+i*34,260,28);
                    Control(window,"BUTTON","Only",BS_PUSHBUTTON,101+i*3,278,40+i*34,70,28);
                    Control(window,"BUTTON","Select",BS_PUSHBUTTON,102+i*3,354,40+i*34,70,28);
                }
                Control(window,"BUTTON","Show all",BS_PUSHBUTTON,200,12,288,110,28);
                Control(window,"BUTTON","Refresh",BS_PUSHBUTTON,201,132,288,110,28);
                // The selection's own visibility, through the editor's hide commands.
                Control(window,"BUTTON","Hide selected",BS_PUSHBUTTON,202,12,322,132,28);
                Control(window,"BUTTON","Isolate selected",BS_PUSHBUTTON,203,150,322,132,28);
                Control(window,"BUTTON","Unhide all",BS_PUSHBUTTON,204,288,322,136,28);
                Control(window,"STATIC","All viewports. Select also reveals that type.\r\nBuilder brush is unchanged. Built BSP remains visible.\r\nViewport show flags and hidden groups still apply.",0,2,12,364,460,60);
                RefreshBrushVisibility(window);return 0;
            }
            if(message==WM_ACTIVATE && LOWORD(w)!=WA_INACTIVE){RefreshBrushVisibility(window);return 0;}
            if(message==WM_COMMAND)
            {
                const int id=LOWORD(w);
                if(id>=100 && id<121)
                {
                    const int category=(id-100)/3,action=(id-100)%3;
                    const bool checked=SendDlgItemMessage(window,100+category*3,BM_GETCHECK,0,0)==BST_CHECKED;
                    Editor::SetBrushVisibility(category,action==1?"only":action==2?"select":checked?"hide":"show");
                }
                else if(id==200)Editor::SetBrushVisibility(0,"all");
                else if(id>=202 && id<=204)EditorExtras::HandleCommand(id==202?EditorExtras::kHideSelected:id==203?EditorExtras::kIsolateSelected:EditorExtras::kUnhideAll);
                RefreshBrushVisibility(window);return 0;
            }
            if(message==WM_DESTROY){brushVisibilityWindow=nullptr;return 0;}
        }
        catch(const std::exception& e)
        {
            if(message==WM_CREATE)return -1;
            if(message==WM_ACTIVATE)return 0;
            MessageBoxA(window,e.what(),"Brush Visibility",MB_OK|MB_ICONERROR);return 0;
        }
        return DefWindowProcA(window,message,w,l);
    }
    void OpenBrushVisibility()
    {
        if(IsWindow(brushVisibilityWindow)){ShowWindow(brushVisibilityWindow,SW_RESTORE);SetForegroundWindow(brushVisibilityWindow);return;}
        Editor::BrushVisibility();
        WNDCLASSA wc{};wc.lpfnWndProc=BrushVisibilityProc;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="ReloadedBrushVisibility";
        wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassA(&wc);
        RECT rect{0,0,448,432};AdjustWindowRectEx(&rect,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU, FALSE,WS_EX_TOOLWINDOW);
        brushVisibilityWindow=CreateWindowExA(WS_EX_TOOLWINDOW,wc.lpszClassName,"Brush Visibility",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
            CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,GetActiveWindow(),nullptr,wc.hInstance,nullptr);
        if(!brushVisibilityWindow)throw std::runtime_error("Could not open Brush Visibility.");
        ShowWindow(brushVisibilityWindow,SW_SHOW);
    }
    // An editable field with choices is a combo the user may also type into.
    struct InputField { std::string label,value; std::vector<std::string> choices; bool editable=false; };
    struct Form { std::vector<InputField> fields; std::vector<HWND> controls; bool done=false,accepted=false; };
    LRESULT CALLBACK FormProc(HWND window,UINT message,WPARAM w,LPARAM l)
    {
        auto form=reinterpret_cast<Form*>(GetWindowLongPtr(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) { form=static_cast<Form*>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams); SetWindowLongPtr(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(form)); }
        if(!form) return DefWindowProcA(window,message,w,l);
        if(message==WM_CREATE)
        {
            int y=16;
            for(size_t i=0;i<form->fields.size();++i)
            {
                auto& f=form->fields[i]; Control(window,"STATIC",f.label,0,0,12,y,200,22);
                HWND input=Control(window,f.choices.empty()?"EDIT":"COMBOBOX",f.choices.empty()?f.value:"",f.choices.empty()?ES_AUTOHSCROLL:(f.editable?CBS_DROPDOWN|CBS_AUTOHSCROLL:CBS_DROPDOWNLIST)|WS_VSCROLL,1000+static_cast<int>(i),215,y-3,340,f.choices.empty()?24:240);
                for(auto& choice:f.choices) SendMessageA(input,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(choice.c_str()));
                if(!f.choices.empty())
                {
                    auto selected=std::find(f.choices.begin(),f.choices.end(),f.value);
                    if(f.editable && selected==f.choices.end()) SetWindowTextA(input,f.value.c_str());
                    else SendMessage(input,CB_SETCURSEL,selected==f.choices.end()?0:selected-f.choices.begin(),0);
                }
                form->controls.push_back(input); y+=34;
            }
            Control(window,"BUTTON","OK",BS_DEFPUSHBUTTON,IDOK,365,y,90,28); Control(window,"BUTTON","Cancel",0,IDCANCEL,465,y,90,28);
            if(!form->controls.empty()) SetFocus(form->controls[0]); return 0;
        }
        if(message==WM_COMMAND && (LOWORD(w)==IDOK || LOWORD(w)==IDCANCEL))
        {
            form->accepted=LOWORD(w)==IDOK;
            if(form->accepted) for(size_t i=0;i<form->fields.size();++i) form->fields[i].value=Text(form->controls[i]);
            form->done=true; return 0;
        }
        if(message==WM_CLOSE) { form->done=true; return 0; }
        return DefWindowProcA(window,message,w,l);
    }
    bool Ask(HWND parent,const char* title,std::vector<InputField>& fields)
    {
        WNDCLASSA wc{}; wc.lpfnWndProc=FormProc; wc.hInstance=GetModuleHandle(nullptr); wc.lpszClassName="ReloadedWorkflowForm"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1); RegisterClassA(&wc);
        Form form{fields}; RECT owner{}; GetWindowRect(parent,&owner);
        HWND dialog=CreateWindowExA(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,wc.lpszClassName,title,WS_CAPTION|WS_SYSMENU|WS_POPUP,owner.left+40,owner.top+50,585,110+static_cast<int>(fields.size())*34,parent,nullptr,wc.hInstance,&form);
        if(!dialog) throw std::runtime_error("Could not create workflow dialog.");
        EnableWindow(parent,FALSE); ShowWindow(dialog,SW_SHOW);
        MSG msg{};
        while(!form.done && GetMessage(&msg,nullptr,0,0)>0) if(!IsDialogMessage(dialog,&msg)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        EnableWindow(parent,TRUE); DestroyWindow(dialog); SetActiveWindow(parent);
        if(form.accepted) fields=std::move(form.fields); return form.accepted;
    }
    bool GetName(HWND parent,const char* title,std::string& name)
    {
        std::vector<InputField> fields={{"Name",name,{}}}; if(!Ask(parent,title,fields)) return false;
        name=fields[0].value;
        if(name.empty() || name.size()>120 || name.find_first_of("\r\n")!=std::string::npos) throw std::runtime_error("Enter a name between 1 and 120 characters."); return true;
    }
    // After a change: lobby sizes the map's rules leave with nothing to play, and
    // objectives to win a match size cannot reach.
    void WarnPlayerCounts(HWND owner)
    {
        const auto actors=Editor::ObjectiveMissions();
        const auto state=Editor::ObjectivePlayerRules();
        std::map<std::string,std::string> names;
        for(const auto& actor:actors)
        {
            const auto missionPath=actor.value("path",std::string());
            names[ObjectivePlayers::Fold(missionPath)]="mission "+missionPath.substr(missionPath.find('.')+1);
        }
        const auto missions=PlayerMissions(actors);
        auto problems=ObjectivePlayers::Problems(missions,PlayerRules(state),names);
        for(const auto& warning:ObjectivePlayers::TargetWarnings(missions,PlayerRules(state),PlayerTargets(state),names))problems.push_back(warning);
        if(problems.empty())return;
        std::string text;for(const auto& problem:problems)text+=problem+"\n";
        MessageBoxA(owner,(text+"\nWiden a player range, or ask for fewer objectives to win.").c_str(),"Players in Match",MB_OK|MB_ICONWARNING);
    }
    // Sets one objective's, zone mission's or mover's range (open: a mover opens inside
    // it), then warns as WarnPlayerCounts does.
    void ApplyObjectivePlayers(const Json& identity,const ObjectivePlayers::Rule& rule,HWND owner,bool open=false)
    {
        Editor::SetObjectivePlayers(identity,rule.minimum,rule.maximum,open);
        WarnPlayerCounts(owner);
    }
    void ApplyMissionTargets(const Json& identity,const std::array<int,ObjectivePlayers::TeamSizes>& sizes,HWND owner)
    {
        Editor::SetMissionTargets(identity,Json(sizes));
        WarnPlayerCounts(owner);
    }
    // "1v1 (2 players)": the dialog's and the menu's names for a match size.
    std::string SizeName(int size)
    {
        return ObjectivePlayers::SizeLabel(size)+" ("+std::to_string(size*2-1)+"-"+std::to_string(size*2)+" players)";
    }
    // A Players in Match choice from a right-click menu: a preset or a range asked
    // for, or a mission's objectives to win by match size. (The count Play Level plays
    // with is on the top bar: PlayLevelPlayers.h.)
    void RunObjectivePlayers(UINT command,const Json& identity,HWND owner)
    {
        auto state=Editor::ObjectivePlayerRules();
        const auto path=identity.at("path").get<std::string>();
        if(command==kObjectiveTargets)
        {
            if(!Editor::Compatible(path,"SBase.SMission"))throw std::runtime_error("Select exactly one mission.");
            const auto current=MissionTargets(state,path);
            std::vector<InputField> fields;
            for(int size=1;size<=ObjectivePlayers::TeamSizes;++size)
                fields.push_back({"Objectives to win in "+SizeName(size),ObjectivePlayers::TargetText(current[size-1]),ObjectivePlayers::TargetChoices()});
            if(!Ask(owner,"Objectives to Win by Match Size (Map: the mission's own)",fields))return;
            std::array<int,ObjectivePlayers::TeamSizes> sizes{};
            for(int k=0;k<ObjectivePlayers::TeamSizes;++k)sizes[k]=ObjectivePlayers::TargetCount(fields[k].value);
            ApplyMissionTargets(identity,sizes,owner);
            return;
        }
        if(!Editor::Compatible(path,"SBase.SObjective") && !Editor::Compatible(path,"Mover"))throw std::runtime_error("Select exactly one objective, zone mission or mover.");
        if(!PlayerRuleAllowed(Editor::ObjectiveMissions(),path))throw std::runtime_error("The top mission is in every match: give its objectives or zones a player count instead.");
        auto rules=PlayerRules(state);
        ObjectivePlayers::Rule rule=rules.count(ObjectivePlayers::Fold(path))?rules.at(ObjectivePlayers::Fold(path)):ObjectivePlayers::Rule{};
        bool open=PlayerOpen(state,path);
        if(command==kObjectivePlayersOpen)
        {
            if(ObjectivePlayers::Empty(rule))throw std::runtime_error("Give the mover a player range first: it opens at the start only inside it.");
            ApplyObjectivePlayers(identity,rule,owner,!open);
            return;
        }
        bool chosen=false;
        for(const auto& [id,preset]:kPlayerPresets)if(id==command){rule=preset;chosen=true;}
        if(!chosen)
        {
            if(command!=kObjectivePlayersCustom)throw std::runtime_error("Invalid player count command.");
            std::vector<InputField> fields={{"Minimum players (both teams)",ObjectivePlayers::ChoiceText(rule.minimum),ObjectivePlayers::Choices()},
                                            {"Maximum players (both teams)",ObjectivePlayers::ChoiceText(rule.maximum),ObjectivePlayers::Choices()}};
            if(!Ask(owner,"Players in Match",fields))return;
            rule={ObjectivePlayers::ChoiceCount(fields[0].value),ObjectivePlayers::ChoiceCount(fields[1].value)};
        }
        ApplyObjectivePlayers(identity,rule,owner,open);
    }
    // The Players in Match strip of the editor's Properties windows (F4, double-click;
    // PropertySearch.h), under the filter box while the window edits one objective,
    // mission or mover: a row of Min and Max players (not on the top mission; a mover
    // also has "Opens at start"), and for a mission two rows of objectives to win, 1v1
    // to 4v4 and 5v5 to 8v8.
    enum StripControl { kStripBack=0x5300,kStripLabel,kStripMinLabel,kStripMin,kStripMaxLabel,kStripMax,kStripOpen,
                        kStripWinLabel,kStripWinSize=0x5308,kStripWin=0x5310,kStripEnd=0x5318 };
    constexpr int kStripRow=28,kStripWinRows=54;
    std::map<HWND,Json> playersStrips; // window -> what it shows: the subject, with "players" and "targets" flags
    void StripSelect(HWND window,int id,const std::string& text)
    {
        const auto combo=GetDlgItem(window,id);
        const LRESULT at=SendMessageA(combo,CB_FINDSTRINGEXACT,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageA(combo,CB_SETCURSEL,at==CB_ERR?0:at,0);
    }
    std::string StripText(HWND window,int id)
    {
        const auto combo=GetDlgItem(window,id);
        const LRESULT at=SendMessageA(combo,CB_GETCURSEL,0,0);
        if(at==CB_ERR)return {};
        char text[64]{};SendMessageA(combo,CB_GETLBTEXT,at,reinterpret_cast<LPARAM>(text));
        return text;
    }
    void StripFill(HWND window,const Json& subject)
    {
        const auto state=Editor::ObjectivePlayerRules();
        const auto path=subject.at("path").get<std::string>();
        const bool players=subject.value("players",true),targets=subject.value("targets",false);
        for(int id=kStripLabel;id<=kStripOpen;++id)if(auto control=GetDlgItem(window,id))ShowWindow(control,players?SW_SHOWNA:SW_HIDE);
        for(int id=kStripWinLabel;id<kStripEnd;++id)if(auto control=GetDlgItem(window,id))ShowWindow(control,targets?SW_SHOWNA:SW_HIDE);
        if(players)
        {
            const auto rules=PlayerRules(state);
            const auto found=rules.find(ObjectivePlayers::Fold(path));
            const auto rule=found==rules.end()?ObjectivePlayers::Rule{}:found->second;
            StripSelect(window,kStripMin,ObjectivePlayers::ChoiceText(rule.minimum));
            StripSelect(window,kStripMax,ObjectivePlayers::ChoiceText(rule.maximum));
            if(auto box=GetDlgItem(window,kStripOpen))
            {
                ShowWindow(box,subject.value("mover",false)?SW_SHOWNA:SW_HIDE);
                SendMessageA(box,BM_SETCHECK,PlayerOpen(state,path)?BST_CHECKED:BST_UNCHECKED,0);
                EnableWindow(box,!ObjectivePlayers::Empty(rule));
            }
        }
        if(targets)
        {
            const auto sizes=MissionTargets(state,path);
            for(int k=0;k<ObjectivePlayers::TeamSizes;++k)StripSelect(window,kStripWin+k,ObjectivePlayers::TargetText(sizes[k]));
        }
    }
    void StripRemove(HWND window)
    {
        for(int id=kStripBack;id<kStripEnd;++id)if(auto control=GetDlgItem(window,id))DestroyWindow(control);
        playersStrips.erase(window);
    }
    int StripHeight(const Json& subject)
    {
        return (subject.value("players",true)?kStripRow:0)+(subject.value("targets",false)?kStripWinRows:0);
    }
    int StripUpdate(HWND window,const std::vector<uintptr_t>& objects)
    {
        Json subject;
        try
        {
            if(objects.size()==1)subject=Editor::ObjectivePlayersSubject(objects[0]);
            if(!subject.is_null())
            {
                // A mission has objectives to win; any but the top one also has a player range.
                subject["targets"]=subject.value("mission",false);
                subject["players"]=PlayerRuleAllowed(Editor::ObjectiveMissions(),subject.at("path").get<std::string>());
            }
        }
        catch(const std::exception&){subject=Json();}
        if(subject.is_null()){StripRemove(window);return 0;}
        if(!GetDlgItem(window,kStripBack))
        {
            const auto instance=reinterpret_cast<HINSTANCE>(GetWindowLongPtrA(window,GWLP_HINSTANCE));
            const auto font=reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT));
            auto make=[&](const char* type,const char* text,DWORD style,int id)
            {
                auto control=CreateWindowExA(0,type,text,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|style,0,0,10,10,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
                SendMessageA(control,WM_SETFONT,font,FALSE);
                return control;
            };
            auto combo=[&](int id,const std::vector<std::string>& choices)
            {
                auto control=make("COMBOBOX","",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,id);
                for(const auto& choice:choices)SendMessageA(control,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(choice.c_str()));
            };
            make("STATIC","",0,kStripBack);
            make("STATIC"," Players in match:",SS_LEFT|SS_CENTERIMAGE,kStripLabel);
            make("STATIC","Min",SS_RIGHT|SS_CENTERIMAGE,kStripMinLabel);
            combo(kStripMin,ObjectivePlayers::Choices());
            make("STATIC","Max",SS_RIGHT|SS_CENTERIMAGE,kStripMaxLabel);
            combo(kStripMax,ObjectivePlayers::Choices());
            make("BUTTON","Opens at start",BS_AUTOCHECKBOX|WS_TABSTOP,kStripOpen);
            make("STATIC"," Objectives to win:",SS_LEFT|SS_CENTERIMAGE,kStripWinLabel);
            for(int k=0;k<ObjectivePlayers::TeamSizes;++k)
            {
                make("STATIC",ObjectivePlayers::SizeLabel(k+1).c_str(),SS_RIGHT|SS_CENTERIMAGE,kStripWinSize+k);
                combo(kStripWin+k,ObjectivePlayers::TargetChoices());
            }
            SetWindowPos(GetDlgItem(window,kStripBack),HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        }
        playersStrips[window]=subject;
        try{StripFill(window,subject);}catch(const std::exception&){}
        return StripHeight(subject);
    }
    void StripLayout(HWND window,int top,int width)
    {
        const auto found=playersStrips.find(window);
        if(found==playersStrips.end())return;
        const auto& subject=found->second;
        auto place=[&](int id,int x,int y,int w,int h){if(auto control=GetDlgItem(window,id))MoveWindow(control,x,y,w,h,TRUE);};
        place(kStripBack,0,top,width,StripHeight(subject));
        if(subject.value("players",true))
        {
            place(kStripLabel,0,top+3,100,21);
            place(kStripMinLabel,100,top+3,24,21);
            place(kStripMin,128,top+3,56,300);
            place(kStripMaxLabel,186,top+3,28,21);
            place(kStripMax,218,top+3,56,300);
            place(kStripOpen,290,top+3,110,21);
            top+=kStripRow;
        }
        if(subject.value("targets",false))
        {
            place(kStripWinLabel,0,top+3,100,21);
            for(int k=0;k<ObjectivePlayers::TeamSizes;++k)
            {
                const int x=100+(k%4)*86,y=top+3+(k/4)*26;
                place(kStripWinSize+k,x,y,28,21);
                place(kStripWin+k,x+32,y,50,300);
            }
        }
    }
    bool StripCommand(HWND window,WPARAM w,LPARAM)
    {
        const int id=LOWORD(w);
        const bool target=id>=kStripWin && id<kStripEnd;
        if(id!=kStripMin && id!=kStripMax && id!=kStripOpen && !target)return false;
        if(id==kStripOpen?HIWORD(w)!=BN_CLICKED:HIWORD(w)!=CBN_SELCHANGE)return true;
        const auto found=playersStrips.find(window);
        if(found==playersStrips.end())return true;
        const Json subject=found->second;
        try
        {
            if(target)
            {
                std::array<int,ObjectivePlayers::TeamSizes> sizes{};
                for(int k=0;k<ObjectivePlayers::TeamSizes;++k)sizes[k]=ObjectivePlayers::TargetCount(StripText(window,kStripWin+k));
                ApplyMissionTargets(subject,sizes,window);
            }
            else
            {
                // A minimum above the maximum moves the other end with it, not an error.
                ObjectivePlayers::Rule rule{ObjectivePlayers::ChoiceCount(StripText(window,kStripMin)),ObjectivePlayers::ChoiceCount(StripText(window,kStripMax))};
                if(id!=kStripOpen && rule.minimum && rule.maximum && rule.minimum>rule.maximum)(id==kStripMin?rule.maximum:rule.minimum)=id==kStripMin?rule.minimum:rule.maximum;
                const bool open=subject.value("mover",false) && SendMessageA(GetDlgItem(window,kStripOpen),BM_GETCHECK,0,0)==BST_CHECKED;
                ApplyObjectivePlayers(subject,rule,window,open);
            }
        }
        catch(const std::exception& e){MessageBoxA(window,e.what(),"Players in Match",MB_OK|MB_ICONERROR);}
        try{StripFill(window,subject);}catch(const std::exception&){}
        return true;
    }
    void StripClosed(HWND window){playersStrips.erase(window);}
    #include "MapDesignPanel.inl"
    struct TagPreviewText { std::string title,summary; Json* preview; bool ready=false; };
    void UpdateTagChecks(HWND window,TagPreviewText& data)
    {
        auto list=GetDlgItem(window,1000);size_t tags=0,events=0;
        (*data.preview)["excluded"]=Json::array();
        for(size_t i=0;i<data.preview->at("changes").size();++i)
            if(!ListView_GetCheckState(list,static_cast<int>(i)))(*data.preview)["excluded"].push_back(i);
            else if(data.preview->at("changes")[i].at("property")=="Tag")++tags;else ++events;
        auto summary=data.summary+std::to_string(tags)+" actor Tags and "+std::to_string(events)+" Event assignments checked.\r\n"
            "Unchecked assignments keep their old value. Partial renames split the group's connections; check which targets each Event should reach.";
        SetWindowTextA(GetDlgItem(window,1001),summary.c_str());EnableWindow(GetDlgItem(window,IDOK),tags>0);
    }
    INT_PTR CALLBACK TagPreviewProc(HWND window,UINT message,WPARAM w,LPARAM l)
    {
        if(message==WM_INITDIALOG)
        {
            auto data=reinterpret_cast<TagPreviewText*>(l);SetWindowLongPtr(window,DWLP_USER,l);SetWindowTextA(window,data->title.c_str());RECT r{};GetClientRect(window,&r);
            Control(window,"STATIC",data->summary,0,1001,12,12,r.right-24,95);
            auto list=Control(window,WC_LISTVIEWA,"",LVS_REPORT|LVS_SHOWSELALWAYS|WS_BORDER,1000,12,110,r.right-24,r.bottom-160);
            ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_CHECKBOXES|LVS_EX_DOUBLEBUFFER);
            const char* columns[]={"Entity","Assignment"};
            for(int i=0;i<2;++i){LVCOLUMNA col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.cx=i?230:r.right-280;col.pszText=const_cast<char*>(columns[i]);SendMessageA(list,LVM_INSERTCOLUMNA,i,reinterpret_cast<LPARAM>(&col));}
            int row=0;for(const auto& change:data->preview->at("changes"))
            {
                auto actor=change.at("actor").at("path").get<std::string>();auto property=change.at("property").get<std::string>();
                LVITEMA item{};item.mask=LVIF_TEXT;item.iItem=row;item.pszText=actor.data();SendMessageA(list,LVM_INSERTITEMA,0,reinterpret_cast<LPARAM>(&item));
                item.iSubItem=1;item.pszText=property.data();SendMessageA(list,LVM_SETITEMTEXTA,row,reinterpret_cast<LPARAM>(&item));ListView_SetCheckState(list,row,TRUE);++row;
            }
            Control(window,"BUTTON","Rename checked",BS_DEFPUSHBUTTON,IDOK,r.right-245,r.bottom-38,125,27);Control(window,"BUTTON","Cancel",0,IDCANCEL,r.right-110,r.bottom-38,95,27);
            data->ready=true;UpdateTagChecks(window,*data);return TRUE;
        }
        if(message==WM_NOTIFY){auto data=reinterpret_cast<TagPreviewText*>(GetWindowLongPtr(window,DWLP_USER));auto hdr=reinterpret_cast<NMHDR*>(l);if(data && data->ready && hdr->idFrom==1000 && hdr->code==LVN_ITEMCHANGED)UpdateTagChecks(window,*data);}
        if(message==WM_COMMAND && (LOWORD(w)==IDOK || LOWORD(w)==IDCANCEL)){EndDialog(window,LOWORD(w));return TRUE;}
        if(message==WM_CLOSE){EndDialog(window,IDCANCEL);return TRUE;}return FALSE;
    }
    bool ConfirmTagRename(HWND owner,TagPreviewText& data)
    {
        std::vector<WORD> bytes((sizeof(DLGTEMPLATE)+1)/2+3,0);auto dlg=reinterpret_cast<DLGTEMPLATE*>(bytes.data());
        dlg->style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER;dlg->cx=440;dlg->cy=285;
        auto answer=DialogBoxIndirectParamW(GetModuleHandle(nullptr),dlg,owner,TagPreviewProc,reinterpret_cast<LPARAM>(&data));
        if(answer==-1)throw std::runtime_error("Could not open tag rename preview.");return answer==IDOK;
    }
    // ---- Linked prefabs (PrefabModel.h) ----------------------------------------------------------
    // An instance is the actors filed in its Group, so actors added to that
    // group belong to it and a deleted actor is simply gone; the map's record
    // holds what the Group cannot (assembly, pose, the members' assembly poses).
    Json& MapInstances(Json& library) { return library["maps"][mapKey]["instances"]; }
    std::map<std::string,std::vector<Json>> PrefabMembers()
    {
        std::map<std::string,std::vector<Json>> result;
        for(const auto& actor:Editor::PrefabActors())
            for(const auto& group:Prefab::InstanceGroups(actor.at("groups").get<std::string>())) result[Fold(group)].push_back(actor);
        return result;
    }
    const std::vector<Json>& MembersOf(const std::map<std::string,std::vector<Json>>& members,const Json& record)
    {
        static const std::vector<Json> none;
        auto found=members.find(Fold(record.at("group").get<std::string>())); return found==members.end()?none:found->second;
    }
    Json Identities(const std::vector<Json>& actors)
    {
        Json result=Json::array(); for(const auto& a:actors) result.push_back({{"path",a.at("path")},{"class",a.at("class")}}); return result;
    }
    std::vector<Prefab::MemberPose> MemberPoses(const std::vector<Json>& actors)
    {
        std::vector<Prefab::MemberPose> result;
        for(const auto& a:actors) result.push_back({a.at("name").get<std::string>(),{a.at("position").get<Vector>(),a.at("rotation").get<Rotation>()}});
        return result;
    }
    const Json* AssemblyEntry(const std::string& id)
    {
        for(const auto& entry:document.at("assemblies")) if(entry.value("id",std::string{})==id) return &entry;
        return nullptr;
    }
    // The linked instances of each assembly that still have actors in the map.
    std::map<std::string,size_t> LinkedCounts()
    {
        std::map<std::string,size_t> counts; const auto members=PrefabMembers();
        const auto& instances=document.at("maps").at(mapKey).value("instances",Json::array());
        for(const auto& record:instances) if(Prefab::Linked(record) && !MembersOf(members,record).empty()) ++counts[Prefab::AssemblyOf(record)];
        return counts;
    }
    // The one linked instance the map selection belongs to.
    Json SelectedInstance()
    {
        Sync();
        const auto records=Prefab::RecordsNamed(document.at("maps").at(mapKey).value("instances",Json::array()),Editor::SelectedGroups());
        if(records.empty()) throw std::runtime_error("Select an actor of a linked prefab first: a copy placed with Actor Assemblies > Place Linked.");
        if(records.size()>1) throw std::runtime_error("The selection spans "+std::to_string(records.size())+" linked instances. Select actors of one instance.");
        return records[0];
    }
    bool SelectionIsLinked()
    {
        try { SelectedInstance(); return true; } catch(const std::exception&) { return false; }
    }
    void RefreshAssemblyWindows();
    Json PlaceLinked(const Json& entry,const Pose& frame,const std::map<std::string,std::string>& bindings)
    {
        const auto id=Id(); const auto group=Prefab::GroupName(entry.at("name").get<std::string>(),id);
        auto placed=Editor::PlaceAssembly(entry,frame,bindings,group); placed["id"]=id;
        auto record=Prefab::NewRecord(placed,group,entry);
        Json next=document; MapInstances(next).push_back(record);
        try { Save(next); }
        catch(...) { Editor::Exec("TRANSACTION UNDO"); throw; }
        return record;
    }
    std::string SelectInstance()
    {
        const auto record=SelectedInstance(); const auto all=PrefabMembers(); const auto& members=MembersOf(all,record);
        Editor::Select(Identities(members),false);
        const auto* entry=AssemblyEntry(Prefab::AssemblyOf(record));
        return "Selected the "+std::to_string(members.size())+" actors of this instance of '"+(entry?entry->at("name").get<std::string>():std::string("a deleted assembly"))+"' ("+record.at("group").get<std::string>()+").";
    }
    // Every linked instance of the assembly (the selection's when `assembly` is empty).
    std::string SelectAllInstances(std::string assembly)
    {
        Sync();
        if(assembly.empty()) assembly=Prefab::AssemblyOf(SelectedInstance());
        const auto members=PrefabMembers(); std::vector<Json> all; size_t count=0;
        for(const auto& record:Prefab::LinkedOf(document.at("maps").at(mapKey).value("instances",Json::array()),assembly))
        {
            const auto& own=MembersOf(members,record); if(own.empty()) continue;
            ++count; all.insert(all.end(),own.begin(),own.end());
        }
        if(all.empty()) throw std::runtime_error("No linked instance of this assembly is in the map. Place one with Place Linked.");
        Editor::Select(Identities(all),false);
        return "Selected "+Prefab::CountText(count)+" ("+std::to_string(all.size())+" actors).";
    }
    // Update Prefab from This Instance: the selection's instance becomes the
    // assembly's content and every other linked instance in this map is
    // rebuilt where it stands, in one Undo step. Returns what happened, or
    // nothing when the confirmation was cancelled.
    std::string UpdatePrefab(HWND owner,bool confirm)
    {
        const auto record=SelectedInstance();
        const auto* found=AssemblyEntry(Prefab::AssemblyOf(record));
        if(!found) throw std::runtime_error("This instance's assembly is no longer in the library. Break Link keeps its actors as they are.");
        const Json entry=*found; const auto name=entry.at("name").get<std::string>();
        const auto members=PrefabMembers(); const auto own=MembersOf(members,record);
        if(own.empty()) throw std::runtime_error("This instance has no actors left in the map.");
        const Pose frame=Prefab::InstanceFrame(record,MemberPoses(own));
        Json captured=Editor::CaptureAssembly(Identities(own),frame);
        const bool local=captured.contains("local"); captured.erase("local");
        const auto names=record.value("names",Json::object()).get<std::map<std::string,std::string>>();
        captured=Prefab::WithoutInstanceGroupsInText(CanonicalizeAssembly(captured,names));
        captured["pivot"]=entry.at("pivot");
        Json jobs=Json::array(); std::vector<Json> rebuilt; size_t missing=0;
        for(const auto& other:Prefab::LinkedOf(document.at("maps").at(mapKey).value("instances",Json::array()),entry.at("id")))
        {
            if(other.at("id")==record.at("id")) continue;
            const auto& theirs=MembersOf(members,other);
            if(theirs.empty()) { ++missing; continue; }
            const Pose at=Prefab::InstanceFrame(other,MemberPoses(theirs));
            jobs.push_back({{"members",Identities(theirs)},{"group",other.at("group")},{"position",at.position},{"rotation",at.rotation}});
            rebuilt.push_back(other);
        }
        if(confirm)
        {
            std::string text="Update the prefab '"+name+"' from this instance?\n\nIts "+std::to_string(own.size())+" actors become the assembly's content, and ";
            text+=jobs.empty()?std::string("there is no other linked instance in this map to rebuild."):Prefab::CountText(jobs.size())+" elsewhere in this map "+(jobs.size()==1?"is":"are")+" rebuilt where "+(jobs.size()==1?"it stands":"they stand")+", each keeping its own position and rotation. Changes made to those copies are replaced.";
            if(!captured.at("bindings").empty())
            {
                text+="\n\nOutside actors they point at (the same for every copy):\n";
                for(const auto& b:captured.at("bindings")) text+="  "+b.at("label").get<std::string>()+"\n";
            }
            if(missing) text+="\n\n"+std::to_string(missing)+(missing==1?" instance has":" instances have")+" no actors left in the map and "+(missing==1?"is":"are")+" left out.";
            if(local) text+="\n\nIt uses assets stored inside this map, so it places correctly only in this map.";
            text+="\n\nOne Undo step puts the map back; the library keeps the new content. Unlinked copies and other maps are not changed.";
            if(MessageBoxA(owner,text.c_str(),"Update Prefab from This Instance",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK) return {};
        }
        const auto results=jobs.empty()?Json::array():Editor::RebuildPrefabInstances(captured,jobs);
        Json next=document; auto& instances=MapInstances(next);
        UpdateEntry(next["assemblies"],entry.at("id"),captured);
        Json edited=record; edited["members"]=Identities(own); edited["names"]=Json::object();
        for(const auto& a:captured.at("actors")) edited["names"][a.at("sourcePath").get<std::string>()]=a.at("name");
        edited["position"]=frame.position; edited["rotation"]=frame.rotation; edited["locals"]=Prefab::Locals(captured);
        Prefab::Replace(instances,edited);
        for(size_t i=0;i<rebuilt.size();++i) Prefab::Replace(instances,Prefab::Rebuilt(rebuilt[i],results.at(i),captured));
        try { Save(next); }
        catch(...) { if(!jobs.empty()) Editor::Exec("TRANSACTION UNDO"); throw; }
        Editor::Select(Identities(own),false);
        std::string done="Updated '"+name+"' from this instance"+(jobs.empty()?std::string("."):" and rebuilt "+Prefab::CountText(jobs.size())+".");
        if(Prefab::HasBrushes(captured)) done+=" The prefab holds brushes: rebuild the geometry to see their new shape.";
        return done;
    }
    // Break Link: the instance's actors leave its Group (one Undo step) and the record goes.
    std::string BreakLink()
    {
        const auto record=SelectedInstance(); const auto all=PrefabMembers(); const auto& own=MembersOf(all,record);
        const auto group=record.at("group").get<std::string>();
        if(!own.empty()) Editor::DesignGroupMembers(Identities(own),group,"remove");
        Json next=document; MapInstances(next)=Prefab::WithoutRecord(MapInstances(next),record.at("id"));
        Save(next);
        return "Unlinked "+std::to_string(own.size())+" actors from their prefab; they stay in the map as ordinary actors.";
    }
    struct State
    {
        Kind kind; HWND window{},list{},status{}; Json rows=Json::array(); std::string selectedId,source,connectionActor; bool mesh=false;
        Json editing; Json members=Json::array(), instance; Pose frame{}; uintptr_t editLevel=0; unsigned epoch=0,revision=0; unsigned long builds=0;
        // Find Usages: the level and map generation its rows were listed from,
        // and whether a build has renumbered the BSP surfaces since Refresh; the
        // usages left out by hand that an edit has taken off the list since.
        uintptr_t usageLevel=0; unsigned usageGeneration=0; bool renumberedSurfaces=false; std::multiset<std::string> usagesAway;
        std::vector<HWND> buttons;
    };
    std::vector<State*> windows;
    void Status(State& s,const std::string& text) { SetWindowTextA(s.status,text.c_str()); }
    Json Entries(const State& s) { return s.kind==Kind::Assemblies ? document.at("assemblies") : document.at("maps").at(mapKey).at("views"); }
    int Selected(const State& s) { return ListView_GetNextItem(s.list,-1,LVNI_SELECTED); }
    Json Entry(State& s)
    {
        int row=Selected(s); if(row<0 || row>=static_cast<int>(s.rows.size())) throw std::runtime_error("Select an entry first.");
        auto entry=s.rows[row]; if(entry.contains("id")) s.selectedId=entry.at("id").get<std::string>(); return entry;
    }
    void Row(State& s,int index,const std::vector<std::string>& values,bool checked=false,int image=-1)
    {
        LVITEMA item{}; item.mask=LVIF_TEXT|(image>=0?LVIF_IMAGE:0); item.iItem=index; item.iImage=image; item.pszText=const_cast<char*>(values[0].c_str()); SendMessageA(s.list,LVM_INSERTITEMA,0,reinterpret_cast<LPARAM>(&item)); item.mask=LVIF_TEXT;
        for(size_t i=1;i<values.size();++i) { item.iSubItem=static_cast<int>(i); item.pszText=const_cast<char*>(values[i].c_str()); SendMessageA(s.list,LVM_SETITEMTEXTA,index,reinterpret_cast<LPARAM>(&item)); }
        if(s.kind==Kind::Assets) ListView_SetCheckState(s.list,index,checked);
    }
    // Refresh and the scope box pass fresh, and another map counts as fresh:
    // Find Usages marks its rows from the selection as it is now. Otherwise each
    // usage keeps its check box and the selection captured with it, so a click
    // in a viewport or an edit elsewhere re-checks nothing; BSP surfaces keep
    // theirs until a geometry build renumbers them (UsageMarksModel.h).
    void Populate(State& s,bool fresh=false)
    {
        Sync(); bool changed=s.epoch!=mapEpoch; s.epoch=mapEpoch;s.revision=Editor::ContentRevision();
        if(changed || (s.editLevel && s.editLevel!=level)) { s.editing=Json{}; s.members=Json::array(); s.editLevel=0; }
        std::vector<UsageMarks::Mark> marks; std::string focused; Json usages;
        // Find Usages' rows belong to the level they were listed from. Save As, or
        // the first save of an untitled map, starts a new epoch under another key
        // but keeps the level and its generation, so the marks carry across it.
        const bool another=s.kind==Kind::Assets && (level!=s.usageLevel || nativeMapGeneration!=s.usageGeneration);
        if(s.kind==Kind::Assets)
        {
            for(int i=0;i<static_cast<int>(s.rows.size());++i) marks.push_back({ListView_GetCheckState(s.list,i)!=0,s.rows[i].at("selected").get<bool>(),s.rows[i].value("excluded",false)});
            int row=Selected(s); if(row>=0 && row<static_cast<int>(s.rows.size())) focused=UsageMarks::Key(s.rows[row]);
            // Listed before the old rows go, so a failed listing leaves them with
            // their check boxes (another map's rows go all the same).
            if(another) { ListView_DeleteAllItems(s.list); s.rows=Json::array(); }
            usages=Editor::FindUsages(s.source);
        }
        ListView_DeleteAllItems(s.list);
        if(s.kind==Kind::Assets)
        {
            s.usageLevel=level; s.usageGeneration=nativeMapGeneration;
            // A geometry build numbers the BSP surfaces afresh: their old marks name other faces.
            const auto builds=Editor::GeometryBuilds(); const bool renumbered=!fresh && !another && builds!=s.builds; s.builds=builds;
            auto previous=std::move(s.rows); s.rows=std::move(usages); bool only=SendMessage(GetDlgItem(s.window,kScope),BM_GETCHECK,0,0)==BST_CHECKED;
            if(fresh || another) { marks.clear(); s.usagesAway.clear(); for(const auto& j:s.rows) marks.push_back(UsageMarks::Default(j,only)); }
            else marks=UsageMarks::Carry(previous,marks,s.rows,only,renumbered,&s.usagesAway);
            int i=0; bool surfaces=false; for(auto& j:s.rows)
            {
                // Replace Checked reads the scope from here, and the next listing
                // reads back what the user left out while the row is read-only.
                j["selected"]=marks[i].scoped; j["excluded"]=marks[i].excluded; surfaces=surfaces || j.at("surface").get<int>()>=0;
                auto actor=j.at("actor").is_object()?j.at("actor").value("path",std::string{}):"BSP";
                bool can=j.at("replaceable"); Row(s,i,{actor,j.at("property"),can?"Direct / replaceable":j.at("direct").get<bool>()?"Read-only assignment":"Indirect / shared asset"},marks[i].checked);
                if(!focused.empty() && UsageMarks::Key(j)==focused) { ListView_SetItemState(s.list,i,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED); ListView_EnsureVisible(s.list,i,FALSE); focused.clear(); }
                ++i;
            }
            // The note stays until Refresh or the scope box marks the rows again,
            // so a later edit does not take it away before the user has seen it.
            if(fresh || another) s.renumberedSurfaces=false; else if(renumbered && surfaces) s.renumberedSurfaces=true;
            Status(s,s.source+" — "+std::to_string(s.rows.size())+" usages. Uncheck results to exclude them; shared asset contents are read-only."+(s.renumberedSurfaces?" A geometry build or brush CSG operation renumbered the BSP surfaces: review their check boxes before replacing.":""));
        }
        else if(s.kind==Kind::Connections)
        {
            if(s.connectionActor.empty() || SendMessage(GetDlgItem(s.window,kFollow),BM_GETCHECK,0,0)==BST_CHECKED)
            {
                auto selected=Editor::Actors(true); s.connectionActor=selected.empty()?"":selected[0].at("path").get<std::string>();
            }
            s.rows=s.connectionActor.empty()?Json::array():Editor::Connections(s.connectionActor);
            int i=0; for(const auto& j:s.rows) { bool outgoing=Fold(j.at("from").get<std::string>())==Fold(s.connectionActor); Row(s,i++,{outgoing?"Outgoing":"Incoming",outgoing?j.at("to").get<std::string>():j.at("from").get<std::string>(),j.at("property").get<std::string>()+" / "+j.at("kind").get<std::string>()+(j.at("resolved").get<bool>()?"":" / UNRESOLVED")}); }
            Status(s,s.connectionActor.empty()?"Select an actor in the level, then Refresh.":s.connectionActor);
            EnableWindow(GetDlgItem(s.window,Secondary),!s.connectionActor.empty() && Editor::Compatible(s.connectionActor,"SBase.SObjective"));
        }
        else
        {
            s.rows=Entries(s); int i=0;
            // Image 0 is the placeholder for entries without a readable picture.
            auto images=ImageList_Create(Thumbnail::kCellWidth,Thumbnail::kCellHeight,ILC_COLOR32,static_cast<int>(s.rows.size())+1,4);
            auto add=[&](HBITMAP bitmap){int index=-1;if(images && bitmap)index=ImageList_Add(images,bitmap,nullptr);if(bitmap)DeleteObject(bitmap);return index;};
            const int placeholder=add(LevelSnapshot::ThumbnailPlaceholder(Thumbnail::kCellWidth,Thumbnail::kCellHeight));
            if(auto old=ListView_SetImageList(s.list,images,LVSIL_SMALL)) ImageList_Destroy(old);
            std::map<std::string,size_t> linked;
            if(s.kind==Kind::Assemblies) { try { linked=LinkedCounts(); } catch(const std::exception& e) { Logger::log(std::string("Linked prefabs: ")+e.what()); } }
            for(const auto& j:s.rows)
            {
                std::string info=s.kind==Kind::Views?std::to_string(j.at("cameras").size())+" viewports":std::to_string(j.at("actors").size())+" actors / "+std::to_string(j.at("bindings").size())+" external bindings";
                if(s.kind==Kind::Assemblies) { const auto count=linked.find(j.value("id",std::string{})); if(count!=linked.end()) info+=" / "+Prefab::CountText(count->second)+" in this map"; }
                if(s.kind==Kind::Views && Bookmarks::SlotOf(j)) info+=", key "+std::to_string(Bookmarks::SlotOf(j));
                int image=placeholder;
                try
                {
                    auto file=ThumbnailPath(s.kind==Kind::Views,j.at("id").get<std::string>()); std::error_code error;
                    if(std::filesystem::is_regular_file(file,error)) {int loaded=add(LevelSnapshot::LoadThumbnail(file,Thumbnail::kCellWidth,Thumbnail::kCellHeight)); if(loaded>=0) image=loaded;}
                }
                catch(const std::exception&) {}
                Row(s,i,{j.at("name"),info,j.value("modified",std::string{})},false,image);
                if(j.at("id")==s.selectedId) ListView_SetItemState(s.list,i,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
                ++i;
            }
            if(s.editing.is_object() && !s.editing.empty()) Status(s,"Editing "+s.editing.at("name").get<std::string>()+" — "+std::to_string(s.members.size())+" members. Add/Remove Selected changes membership; Save Changes updates the library.");
            else Status(s,s.kind==Kind::Views?"Restore a view, adjust the workspace, then Update Selected from Current. Right-click an entry to update its thumbnail.":"Place Linked keeps a copy linked: Update Prefab from Instance rebuilds every linked copy from the one you edited. Other placed copies stay independent. Right-click an entry to update its thumbnail.");
        }
    }
    void RefreshAssemblyWindows()
    {
        for(auto s:windows) if(s->kind==Kind::Assemblies)
        {
            try { Populate(*s); } catch(const std::exception& e) { Status(*s,e.what()); }
        }
    }
    void StoreEntry(State& s,Json value,bool update)
    {
        Json next=document;
        auto& entries=s.kind==Kind::Views?next["maps"][mapKey]["views"]:next["assemblies"];
        if(update) UpdateEntry(entries,s.selectedId,value);
        else { value["id"]=Id(); value["modified"]=Timestamp(); s.selectedId=value.at("id"); entries.push_back(value); }
        Save(next); Populate(s);
    }
    // The entry's picture from the perspective viewport, framed on the
    // given actors when there are any (the camera is put back exactly
    // afterwards). A failure never undoes the save it belongs to.
    bool CaptureEntryThumbnail(bool view,const std::string& id,const Json& frame,std::string& problem)
    {
        try
        {
            auto perspective=LevelSnapshot::FindPerspective(); Editor::CameraState moved;
            if(frame.is_array() && !frame.empty())
            {
                RECT r{}; GetClientRect(perspective.window,&r);
                try { moved=Editor::FrameCamera(perspective.camera,frame,r.bottom>0?static_cast<double>(r.right)/r.bottom:4.0/3); }
                catch(const std::exception& e) { moved={}; Logger::log(std::string("Thumbnail: captured unframed: ")+e.what()); } // Captured as it is instead.
            }
            try { LevelSnapshot::SaveViewportThumbnail(perspective,ThumbnailPath(view,id)); }
            catch(...) { Editor::RestoreCamera(moved); throw; }
            Editor::RestoreCamera(moved);
            return true;
        }
        catch(const std::exception& e) { problem=e.what(); return false; }
    }
    // The same for a list window's entry, which then shows it, or why not.
    bool CaptureThumbnail(State& s,const std::string& id,const Json& frame=Json{})
    {
        std::string problem;
        const bool captured=CaptureEntryThumbnail(s.kind==Kind::Views,id,frame,problem);
        Populate(s);
        if(!captured) Status(s,"Saved, but the thumbnail was not captured: "+problem);
        return captured;
    }
    void SaveAssembly(State& s,bool update,bool session)
    {
        Json original=update?(session?s.editing:Entry(s)):Json{};
        if(session && (s.editing.empty() || s.editLevel!=Editor::LevelIdentity())) throw std::runtime_error("Start Edit Contents in this map first.");
        Json members=session?s.members:Editor::SelectedIdentities();
        Pose frame=session?s.frame:Pose{};
        Json updatingInstance=session?s.instance:Json{};
        if(update && !session)
        {
            frame.position=original.at("pivot").get<Vector>();
            for(const auto& instance:document["maps"][mapKey]["instances"]) if(instance.at("assembly")==original.at("id"))
            {
                bool overlap=false;
                for(const auto& old:instance.at("members")) for(const auto& selected:members) if(old.at("path")==selected.at("path") && old.at("class")==selected.at("class"))overlap=true;
                if(!overlap)continue;
                if(!updatingInstance.empty())throw std::runtime_error("The selection spans multiple instances. Use Edit Contents to choose which instance to update.");
                updatingInstance=instance;frame={instance.at("position").get<Vector>(),instance.at("rotation").get<Rotation>()};
            }
        }
        if(!update)
        {
            // Start from actor bounds supplied by the native snapshot helper.
            frame=Editor::BuilderPose();
            auto snapshot=Editor::CaptureAssembly(members,Pose{}); Vector lo{},hi{}; bool first=true;
            for(const auto& a:snapshot.at("actors")) { Vector p=a.at("position").get<Vector>(); if(first) {lo=hi=p; first=false;} else for(int i=0;i<3;++i){lo[i]=std::min(lo[i],p[i]);hi[i]=std::max(hi[i],p[i]);} }
            for(int i=0;i<3;++i) frame.position[i]=(lo[i]+hi[i])/2;
            std::vector<InputField> pivot={{"Pivot","Selection centre",{"Selection centre","Builder brush","First selected actor"}}};
            if(!Ask(s.window,"Assembly Placement Pivot",pivot))return;
            if(pivot[0].value=="Builder brush")frame=Editor::BuilderPose();
            else if(pivot[0].value=="First selected actor")frame.position=snapshot.at("actors")[0].at("position").get<Vector>();
        }
        Json captured=Editor::CaptureAssembly(members,frame);
        if(captured.contains("local"))
        {
            // Assets stored in this map exist only here; say so now rather
            // than with a missing-package error when placing in another map.
            std::string text="This selection uses assets stored inside this map:\n";
            for(const auto& path:captured.at("local")) text+="  "+path.get<std::string>()+"\n";
            text+="\nThe assembly will place correctly only in this map. Save it anyway?";
            if(MessageBoxA(s.window,text.c_str(),"Map-local assets",MB_OKCANCEL|MB_ICONWARNING)!=IDOK) return;
            captured.erase("local");
        }
        auto names=session && s.instance.contains("names")?s.instance.at("names").get<std::map<std::string,std::string>>():std::map<std::string,std::string>{};
        if(update && !session)
        {
            for(const auto& instance:document["maps"][mapKey]["instances"]) if(instance.at("assembly")==original.at("id") && instance.contains("names"))
                for(auto i=instance.at("names").begin();i!=instance.at("names").end();++i) names[i.key()]=i.value();
        }
        captured=CanonicalizeAssembly(captured,names);
        std::string name=update?original.at("name").get<std::string>():"New assembly";
        if(!update && !GetName(s.window,"Save Selection as Assembly",name)) return;
        captured["name"]=name;
        if(update)
        {
            s.selectedId=original.at("id"); captured["pivot"]=original.at("pivot");
            std::set<std::string> before,after;
            for(const auto& a:original.at("actors")) before.insert(Fold(a.at("name").get<std::string>()));
            for(const auto& a:captured.at("actors")) after.insert(Fold(a.at("name").get<std::string>()));
            int added=0,removed=0,modified=0; for(const auto& n:after) added+=!before.count(n); for(const auto& n:before) removed+=!after.count(n);
            for(const auto& a:captured.at("actors")) for(const auto& b:original.at("actors"))
                if(Fold(a.at("name").get<std::string>())==Fold(b.at("name").get<std::string>()) && a.at("text")!=b.at("text")) ++modified;
            std::string summary="Update '"+name+"' in place?\n\n"+std::to_string(added)+" added, "+std::to_string(removed)+" removed.\nCapture current properties and transforms for "+std::to_string(captured.at("actors").size())+" members.\n\nExternal bindings:\n";
            summary+=std::to_string(modified)+" existing members have changed contents.\n";
            for(const auto& b:captured.at("bindings")) summary+="  "+b.at("label").get<std::string>()+" ("+b.at("type").get<std::string>()+")\n";
            if(captured.at("bindings").empty()) summary+="  None\n";
            for(const auto& old:original.at("bindings"))
                if(std::find(captured.at("bindings").begin(),captured.at("bindings").end(),old)==captured.at("bindings").end()) summary+="  Removed/changed: "+old.at("label").get<std::string>()+"\n";
            summary+="\nExisting placed copies will not be changed.";
            if(MessageBoxA(s.window,summary.c_str(),"Update Assembly",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK) return;
        }
        if(!updatingInstance.empty())
        {
            // Keep the editable instance's membership and canonical names current
            // so a second edit does not resurrect removed members.
            Json next=document; auto& list=next["maps"][mapKey]["instances"];
            for(auto& instance:list) if(instance.at("id")==updatingInstance.at("id"))
            {
                instance["members"]=Json::array();instance["names"]=Json::object();
                for(const auto& a:captured.at("actors"))
                {
                    auto source=a.at("sourcePath").get<std::string>();
                    for(const auto& m:members) if(m.at("path")==source) instance["members"].push_back(m);
                    instance["names"][source]=a.at("name");
                }
            }
            UpdateEntry(next["assemblies"],s.selectedId,captured);Save(next);Populate(s);
        }
        else StoreEntry(s,captured,update);
        if(session) { s.editing=Json{}; s.members=Json::array(); s.editLevel=0; }
        CaptureThumbnail(s,s.selectedId,members);
    }
    // Place Assembly, or with `linked` Place Linked: one copy that stays linked
    // to the entry as a prefab instance (PrefabModel.h).
    Json Place(State& s,const Json& entry,bool linked=false)
    {
        auto frame=Editor::BuilderPose(); std::vector<InputField> fields;
        const char* labels[]={"Position X","Position Y","Position Z","Pitch (degrees)","Yaw (degrees)","Roll (degrees)"};
        for(int i=0;i<6;++i) fields.push_back({labels[i],i<3?std::to_string(frame.position[i]):"0",{}});
        if(!linked){fields.push_back({"Copies","1",{}});fields.push_back({"Spacing X","128",{}});fields.push_back({"Spacing Y","0",{}});fields.push_back({"Spacing Z","0",{}});}
        fields.push_back({"Yaw snap (degrees, 0=off)","15",{}});
        if(!Ask(s.window,linked?"Place Linked Prefab":"Place Assembly",fields)) return {};
        for(int i=0;i<6;++i)
        {
            size_t used=0; double v=std::stod(fields[i].value,&used);
            if(used!=fields[i].value.size() || !std::isfinite(v) || abs(v)>10000000) throw std::runtime_error("Enter finite numeric position/rotation values.");
            if(i<3) frame.position[i]=v; else frame.rotation[i-3]=static_cast<int>(std::lround(std::fmod(v,360.0)*65536.0/360.0));
        }
        auto actors=Editor::Actors(); std::map<std::string,std::string> bindings;
        for(const auto& b:entry.at("bindings"))
        {
            std::vector<std::string> choices={"Unbound (None)"};
            for(const auto& a:actors) if(Editor::Compatible(a.at("path"),b.at("type")))
                if(b.at("kind")!="event" || Fold(a.at("tag").get<std::string>())!="none") choices.push_back(a.at("path"));
            std::vector<InputField> binding={{b.at("label"),"",choices}};
            if(!Ask(s.window,"Bind External Actor",binding)) return {};
            bindings[b.at("id")]=binding[0].value==choices[0]?"":binding[0].value;
        }
        double copies=linked?1:Design::Number(fields[6].value,1,128);if(copies!=std::floor(copies))throw std::runtime_error("Copy count must be a whole number.");
        double snap=Design::Number(fields.back().value,0,360);if(snap>0){double degrees=frame.rotation[1]*360.0/65536;frame.rotation[1]=static_cast<int>(std::round(degrees/snap)*snap*65536/360);}
        if(linked) return PlaceLinked(entry,frame,bindings);
        auto definition=entry;
        if(copies>1){if(!entry.at("bindings").empty())throw std::runtime_error("Repeated assemblies must contain their linked actors. Place a single bound copy otherwise.");definition=Design::Repeat(entry,static_cast<int>(copies),{Design::Number(fields[7].value),Design::Number(fields[8].value),Design::Number(fields[9].value)},0);}
        Json instance=Editor::PlaceAssembly(definition,frame,bindings); Json next=document;
        next["maps"][mapKey]["instances"].push_back(instance);
        try { Save(next); }
        catch(...) { Editor::Exec("TRANSACTION UNDO"); throw; }
        return instance;
    }
    void EditContents(State& s)
    {
        auto entry=Entry(s); std::vector<Json> instances;
        std::vector<std::string> choices={"Place a new editable copy"};
        auto live=Editor::Actors();
        for(const auto& i:document["maps"][mapKey]["instances"]) if(i.at("assembly")==entry.at("id"))
        {
            bool exists=false;
            for(const auto& m:i.at("members")) for(const auto& a:live) if(a.at("path")==m.at("path") && a.at("class")==m.at("class")) exists=true;
            if(exists) { instances.push_back(i); choices.push_back("Existing instance "+i.at("id").get<std::string>().substr(0,8)+" ("+std::to_string(i.at("members").size())+" members)"); }
        }
        std::vector<InputField> fields={{"Edit instance","",choices}}; if(!Ask(s.window,"Edit Assembly Contents",fields)) return;
        auto choice=std::find(choices.begin(),choices.end(),fields[0].value)-choices.begin();
        Json instance=choice==0?Place(s,entry):instances.at(choice-1); if(instance.empty()) return;
        s.editing=entry; s.instance=instance; s.members=instance.at("members"); s.frame={instance.at("position").get<Vector>(),instance.at("rotation").get<Rotation>()}; s.editLevel=level;
        Editor::Select(s.members,true); Populate(s);
    }
    void Activate(State& s)
    {
        Sync();
        if(s.epoch!=mapEpoch || (s.kind==Kind::Assets && s.revision!=Editor::ContentRevision()))
        {
            Populate(s); throw std::runtime_error("The map changed. Select an entry in the refreshed list.");
        }
        auto j=Entry(s);
        if(s.kind==Kind::Views) { auto message=Editor::RestoreView(j); Status(s,message); }
        else if(s.kind==Kind::Assets)
        {
            if(!j.at("actor").empty()) Editor::Select(Json::array({j.at("actor")}),true);
        }
        else if(s.kind==Kind::Connections)
        {
            if(!j.at("resolved").get<bool>()) throw std::runtime_error("This relationship has no matching target.");
            std::string target=Fold(j.at("from").get<std::string>())==Fold(s.connectionActor)?j.at("to"):j.at("from");
            for(const auto& a:Editor::Actors()) if(a.at("path")==target) { Editor::Select(Json::array({a}),true); break; }
        }
        else Place(s,j);
    }
    void Command(State& s,int command)
    {
        Sync(); if(s.epoch!=mapEpoch) { s.editing=Json{}; s.members=Json::array(); Populate(s); if(command!=Refresh) throw std::runtime_error("The map changed. Select an entry in the refreshed list."); }
        if(command==Refresh) { Populate(s,true); return; }
        if(s.kind==Kind::Assets)
        {
            if(command==Primary) { Activate(s); return; }
            if(command==Third) { MapUsagesWindow::Open(s.window,s.source); return; }
            if(command==Secondary)
            {
                if(s.revision!=Editor::ContentRevision()) { Populate(s); throw std::runtime_error("The map changed. Review the refreshed usages before replacing."); }
                auto target=Editor::CurrentAsset(s.mesh); if(target.empty()) throw std::runtime_error("Select a replacement in the asset browser first.");
                Json changes=Json::array(); bool only=SendMessage(GetDlgItem(s.window,kScope),BM_GETCHECK,0,0)==BST_CHECKED;
                for(int i=0;i<static_cast<int>(s.rows.size());++i) if(ListView_GetCheckState(s.list,i))
                {
                    auto j=s.rows[i]; if(!j.at("replaceable").get<bool>()) throw std::runtime_error("Uncheck read-only usages before replacing.");
                    if(only && !j.at("selected").get<bool>()) continue;
                    changes.push_back(j);
                }
                if(changes.empty()) throw std::runtime_error("Check at least one replaceable usage.");
                std::string preview="Replace "+std::to_string(changes.size())+" checked assignments?\n\nFrom: "+s.source+"\nTo: "+target+"\n\nThis is one undoable map operation.";
                if(MessageBoxA(s.window,preview.c_str(),"Preview Asset Replacement",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK) return;
                Editor::ReplaceUsages(changes,s.source,target); Populate(s);
            }
            return;
        }
        if(s.kind==Kind::Connections)
        {
            if(command==Third)WorkflowGraph::Open(s.window);
            else if(command==Secondary)
            {
                if(s.connectionActor.empty())return;
                auto owner=ObjectiveOwner(s.connectionActor);
                if(owner.is_null() || !Editor::Compatible(s.connectionActor,"SBase.SObjective"))return;
                auto snapshot=Editor::InspectActor(owner);
                auto menu=CreatePopupMenu();if(!menu)throw std::runtime_error("Cannot open objective menu.");
                ObjectiveMenu(menu,owner);POINT point{};GetCursorPos(&point);
                auto choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,s.window,nullptr);DestroyMenu(menu);
                if(choice){RunObjectiveCommand(choice,snapshot);Populate(s);}
            }
            else Activate(s);
            return;
        }
        if(s.kind==Kind::Views)
        {
            if(command==Primary) { Activate(s); return; }
            if(command==Secondary) { std::string name="New view"; if(GetName(s.window,"Save New Working View",name)) { auto view=Editor::CaptureView(); view["name"]=name; StoreEntry(s,view,false); CaptureThumbnail(s,s.selectedId); } return; }
            if(command==Third)
            {
                // A camera bookmark keeps its key and stays cameras only.
                const int slot=Bookmarks::SlotOf(Entry(s)); auto view=Editor::CaptureView();
                if(slot) { view=Bookmarks::CamerasOnly(view); view["slot"]=slot; }
                StoreEntry(s,view,true); if(CaptureThumbnail(s,s.selectedId)) Status(s,"Updated the selected view and its thumbnail from the current workspace. Its name and identity are unchanged."); return;
            }
        }
        if(command==UpdateThumbnail && (s.kind==Kind::Views || s.kind==Kind::Assemblies))
        {
            // A view is pictured as the viewport is now; an assembly is framed
            // on the current selection when there is one.
            Entry(s); CaptureThumbnail(s,s.selectedId,s.kind==Kind::Assemblies?Editor::SelectedIdentities():Json{}); return;
        }
        if(s.kind==Kind::Assemblies)
        {
            if(command==Primary) { Activate(s); return; }
            if(command==PlaceLinkedButton) { auto placed=Place(s,Entry(s),true); Populate(s); if(!placed.empty()) Status(s,"Placed a linked instance ("+placed.at("group").get<std::string>()+"). Edit any instance, then Update Prefab from Instance to change them all."); return; }
            if(command==SelectInstanceButton) { auto text=SelectInstance(); Status(s,text); return; }
            if(command==SelectAllInstancesButton)
            {
                // The listed entry's instances, or the map selection's when no entry is chosen.
                const int row=Selected(s); std::string assembly;
                if(row>=0 && row<static_cast<int>(s.rows.size())) assembly=s.rows[row].value("id",std::string{});
                auto text=SelectAllInstances(assembly); Status(s,text); return;
            }
            if(command==UpdatePrefabButton) { auto text=UpdatePrefab(s.window,true); if(!text.empty()) { Populate(s); Status(s,text); } return; }
            if(command==BreakLinkButton) { auto text=BreakLink(); Populate(s); Status(s,text); return; }
            if(command==Secondary) { SaveAssembly(s,false,false); return; }
            if(command==Third) { EditContents(s); return; }
            if(command==Sixth) { SaveAssembly(s,true,false); return; }
            if(command==Seventh || command==Eighth || command==Ninth || command==Tenth || command==Eleventh)
            {
                if(s.editing.empty() || s.editLevel!=level) throw std::runtime_error("Choose Edit Contents first.");
                if(command==Ninth) { Editor::Select(s.members,true); return; }
                if(command==Tenth) { SaveAssembly(s,true,true); return; }
                if(command==Eleventh) { s.editing=Json{}; s.members=Json::array(); Populate(s); return; }
                auto selected=Editor::SelectedIdentities();
                for(const auto& a:selected)
                {
                    auto found=std::find_if(s.members.begin(),s.members.end(),[&](const Json& m){return m.at("path")==a.at("path");});
                    if(command==Seventh && found==s.members.end()) s.members.push_back(a);
                    if(command==Eighth && found!=s.members.end()) s.members.erase(found);
                }
                Populate(s); return;
            }
        }
        if(command==Fourth || command==Fifth)
        {
            // Asked before copying the library: a camera bookmark stored from a
            // viewport while the box is open must not be written over.
            auto entry=Entry(s); std::string name;
            if(command==Fourth) { name=entry.at("name"); if(!GetName(s.window,"Rename Entry",name)) return; }
            else if(MessageBoxA(s.window,"Delete this saved entry? Placed actors are kept.","Delete Entry",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK) return;
            Json next=document;
            auto& entries=s.kind==Kind::Views?next["maps"][mapKey]["views"]:next["assemblies"];
            auto held=[&](const Json& j){return j.at("id")==s.selectedId;};
            if(std::none_of(entries.begin(),entries.end(),held)) { Populate(s); throw std::runtime_error("That entry is no longer listed. Select an entry in the refreshed list."); }
            if(command==Fourth) { for(auto& j:entries) if(held(j)) { j["name"]=name; j["modified"]=Timestamp(); } }
            else entries.erase(std::remove_if(entries.begin(),entries.end(),held),entries.end());
            Save(next);
            if(command==Fifth) { try { std::error_code error; std::filesystem::remove(ThumbnailPath(s.kind==Kind::Views,s.selectedId),error); } catch(const std::exception&) {} }
            Populate(s);
        }
    }
    void Layout(State& s)
    {
        RECT r{}; GetClientRect(s.window,&r); int width=r.right;
        int y=12,x=12;
        for(auto button:s.buttons) { if(x+205>width) {x=12;y+=33;} MoveWindow(button,x,y,200,27,TRUE); x+=207; }
        y+=37;
        if(s.kind==Kind::Connections) { MoveWindow(GetDlgItem(s.window,kFollow),12,y,250,24,TRUE); y+=30; }
        if(s.kind==Kind::Assets) { MoveWindow(GetDlgItem(s.window,kScope),12,y,380,24,TRUE); y+=30; }
        MoveWindow(s.list,12,y,std::max(50,width-24),std::max(30,static_cast<int>(r.bottom)-y-66),TRUE);
        MoveWindow(s.status,12,r.bottom-55,width-24,48,TRUE);
        int first=s.kind==Kind::Connections?100:(width-24)/3; ListView_SetColumnWidth(s.list,0,first); ListView_SetColumnWidth(s.list,1,(width-first-24)/2); ListView_SetColumnWidth(s.list,2,(width-first-24)/2-20);
    }
    LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM w,LPARAM l)
    {
        auto s=reinterpret_cast<State*>(GetWindowLongPtr(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) { s=static_cast<State*>(reinterpret_cast<CREATESTRUCT*>(l)->lpCreateParams); s->window=window; SetWindowLongPtr(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s)); }
        if(!s) return DefWindowProcA(window,message,w,l);
        try
        {
            if(message==WM_CREATE)
            {
                auto button=[&](int id,const char* name){s->buttons.push_back(Control(window,"BUTTON",name,0,id,0,0,200,27));};
                button(Refresh,"Refresh");
                if(s->kind==Kind::Assets) {button(Primary,"Select and Focus");button(Secondary,"Replace Checked...");button(Third,"Search All Maps...");Control(window,"BUTTON","Limit to selection captured at Refresh",BS_AUTOCHECKBOX,kScope,0,0,300,24);}
                if(s->kind==Kind::Connections) {button(Primary,"Select and Focus");button(Secondary,"Add Objective / Trigger...");button(Third,"Graph...");auto f=Control(window,"BUTTON","Follow Selection",BS_AUTOCHECKBOX,kFollow,0,0,200,24);SendMessage(f,BM_SETCHECK,BST_CHECKED,0);}
                if(s->kind==Kind::Views) {button(Primary,"Restore");button(Secondary,"Save New...");button(Third,"Update Selected from Current");button(Fourth,"Rename...");button(Fifth,"Delete...");}
                if(s->kind==Kind::Assemblies) {button(Primary,"Place...");button(Secondary,"Save Selection as New...");button(Third,"Edit Contents...");button(Sixth,"Update from Selection...");button(Fourth,"Rename...");button(Fifth,"Delete...");button(Seventh,"Add Selected");button(Eighth,"Remove Selected");button(Ninth,"Select Members");button(Tenth,"Save Changes...");button(Eleventh,"Cancel Editing");
                    button(PlaceLinkedButton,"Place Linked...");button(SelectInstanceButton,"Select Instance");button(SelectAllInstancesButton,"Select All Instances");button(UpdatePrefabButton,"Update Prefab from Instance...");button(BreakLinkButton,"Break Link");}
                s->list=Control(window,WC_LISTVIEWA,"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_BORDER,kList,0,0,500,300);
                ListView_SetExtendedListViewStyle(s->list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|(s->kind==Kind::Assets?LVS_EX_CHECKBOXES:0));
                const char* columns=s->kind==Kind::Connections?"Direction|Actor|Relationship":s->kind==Kind::Assets?"Actor|Property / surface|Usage":"Name|Contents|Updated (UTC epoch ms)";
                std::istringstream names(columns); std::string name; int i=0; while(std::getline(names,name,'|')) {LVCOLUMNA col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.cx=250;col.pszText=name.data();SendMessageA(s->list,LVM_INSERTCOLUMNA,i++,reinterpret_cast<LPARAM>(&col));}
                s->status=Control(window,"STATIC","",0,kStatus,0,0,600,50); Layout(*s); Populate(*s,true); SetTimer(window,1,1000,nullptr); return 0;
            }
            if(message==WM_SIZE) { if(s->list) Layout(*s); return 0; }
            if(message==WM_CONTEXTMENU && s->kind==Kind::Connections) {Command(*s,Secondary);return 0;}
            if(message==WM_CONTEXTMENU && (s->kind==Kind::Views || s->kind==Kind::Assemblies) && reinterpret_cast<HWND>(w)==s->list)
            {
                int row=Selected(*s); if(row<0) return 0;
                POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
                if(point.x==-1 && point.y==-1) {RECT r{};ListView_GetItemRect(s->list,row,&r,LVIR_LABEL);point={r.left,r.bottom};ClientToScreen(s->list,&point);}
                auto menu=CreatePopupMenu(); if(!menu) return 0;
                AppendMenuA(menu,MF_STRING,UpdateThumbnail,"Update &Thumbnail");
                if(s->kind==Kind::Assemblies) AppendMenuA(menu,MF_STRING,SelectAllInstancesButton,"Select All &Linked Instances");
                AppendMenuA(menu,MF_SEPARATOR,0,nullptr);
                AppendMenuA(menu,MF_STRING,Fourth,"&Rename...");
                AppendMenuA(menu,MF_STRING,Fifth,"&Delete...");
                auto choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr); DestroyMenu(menu);
                if(choice) Command(*s,static_cast<int>(choice));
                return 0;
            }
            if(message==WM_GETMINMAXINFO) {auto m=reinterpret_cast<MINMAXINFO*>(l);m->ptMinTrackSize={680,430};return 0;}
            if(message==WM_COMMAND)
            {
                if(LOWORD(w)>=Refresh && LOWORD(w)<=LastButton) {Command(*s,LOWORD(w));return 0;}
                if(LOWORD(w)==kScope || LOWORD(w)==kFollow) {Populate(*s,true);return 0;}
            }
            if(message==WM_NOTIFY)
            {
                auto hdr=reinterpret_cast<NMHDR*>(l);
                if(hdr->code==NM_DBLCLK && hdr->hwndFrom==s->list) {Activate(*s);return 0;}
                if(hdr->code==LVN_ITEMCHANGED && hdr->hwndFrom==s->list)
                {
                    int row=Selected(*s); if(row>=0 && row<static_cast<int>(s->rows.size()) && s->rows[row].contains("id")) s->selectedId=s->rows[row].at("id");
                }
            }
            if(message==WM_TIMER)
            {
                Sync(); if(s->epoch!=mapEpoch) { s->editing=Json{}; s->members=Json::array(); Populate(*s); }
                else if(s->revision!=Editor::ContentRevision() && (s->kind==Kind::Assets || s->kind==Kind::Connections)) Populate(*s);
                else if(s->kind==Kind::Connections && (s->connectionActor.empty() || SendMessage(GetDlgItem(window,kFollow),BM_GETCHECK,0,0)==BST_CHECKED))
                { auto actors=Editor::Actors(true); auto actor=actors.empty()?"":actors[0].at("path").get<std::string>(); if(actor!=s->connectionActor) Populate(*s); }
                return 0;
            }
            if(message==WM_CLOSE)
            {
                if(!s->editing.empty() && MessageBoxA(window,"Close without saving assembly membership changes? Scene edits remain in the map.","Close Assembly Editor",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK) return 0;
                DestroyWindow(window); return 0;
            }
        }
        catch(const std::exception& e)
        {
            if(message==WM_TIMER) { if(s->status) Status(*s,e.what()); }
            else MessageBoxA(window,e.what(),"RE+ Tools",MB_OK|MB_ICONERROR);
            return message==WM_CREATE?-1:0;
        }
        if(message==WM_NCDESTROY) {WindowPlacement::Remember(window,PlacementKey(s->kind));KillTimer(window,1);windows.erase(std::remove(windows.begin(),windows.end(),s),windows.end());SetWindowLongPtr(window,GWLP_USERDATA,0);delete s;}
        return DefWindowProcA(window,message,w,l);
    }
    HWND Open(Kind kind,HWND owner,std::string source={},bool mesh=false)
    {
        for(auto s:windows) if(s->kind==kind && (kind!=Kind::Assets || s->source==source)) {ShowWindow(s->window,IsIconic(s->window)?SW_RESTORE:SW_SHOW);SetForegroundWindow(s->window);return s->window;}
        // Another Find Usages window open (one per asset) has the place: this one
        // opens where Windows puts it, not exactly over it.
        const bool another=std::any_of(windows.begin(),windows.end(),[&](const State* o){return o->kind==kind;});
        Sync(); WNDCLASSA wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="ReloadedWorkflowTools";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassA(&wc);
        auto s=new State{kind}; s->source=std::move(source);s->mesh=mesh; windows.push_back(s);
        const char* title=kind==Kind::Assets?"Asset Find and Replace":kind==Kind::Connections?"Gameplay Connections":kind==Kind::Views?"Working Views":"Actor Assemblies";
        HWND window=CreateWindowExA(WS_EX_CONTROLPARENT,wc.lpszClassName,title,WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1000,650,owner,nullptr,wc.hInstance,s);
        if(window) {if(another)WindowPlacement::Track(window);else WindowPlacement::Restore(window,PlacementKey(kind));ShowWindow(window,SW_SHOW);} return window;
    }
    void __fastcall VertexClickHook(void* self,void*,void* cause,void* hit)
    {
        auto editor=*reinterpret_cast<unsigned char**>(0x1165dfa0);
        auto buttons=*reinterpret_cast<unsigned*>(static_cast<unsigned char*>(cause)+8);
        if(editor && *reinterpret_cast<int*>(editor+0x1ac)==0x19 && (buttons&2) && !(buttons&0x80))
        {
            auto menu=CreatePopupMenu();if(!menu)return;
            bool available=false;try{Editor::SelectedBrushVertices();available=true;}catch(const std::exception&){}
            AppendMenuA(menu,MF_STRING|MF_DISABLED,0,"Snap selected vertices to grid");
            AppendMenuA(menu,MF_SEPARATOR,0,nullptr);
            const char* labels[]={"X axis","Y axis","Z axis","All axes"};
            for(unsigned i=0;i<4;++i)AppendMenuA(menu,MF_STRING|(available?0:MF_GRAYED),kVertexSnapX+i,labels[i]);
            AppendMenuA(menu,MF_SEPARATOR,0,nullptr);
            AppendMenuA(menu,MF_STRING|(Editor::CanAddVertexPortal()?0:MF_GRAYED),kAddVertexPortal,"Add portal sheet");
            POINT point{};GetCursorPos(&point);
            auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,GetActiveWindow(),nullptr);
            DestroyMenu(menu);if(command)HandleCommand(command);return;
        }
        reinterpret_cast<void(__thiscall*)(void*,void*,void*)>(0x10ed5c20)(self,cause,hit);
    }
    using LoadMenuFn=HMENU(WINAPI*)(HINSTANCE,LPCSTR); LoadMenuFn previousLoadMenu=nullptr;
    bool builderMenuOnSurface=false; // the Builder Brush submenu was last built for a BSP surface popup
    bool stairMenuOnSurface=false; // Smooth Staircase was last offered on a BSP surface popup
    using TrackMenuFn=BOOL(WINAPI*)(HMENU,UINT,int,int,int,HWND,const RECT*);
    TrackMenuFn previousTrackMenu=nullptr;
    BOOL WINAPI TrackMenuHook(HMENU menu,UINT flags,int x,int y,int reserved,HWND window,const RECT* rect)
    {
        bool objective=false;for(UINT id=kAddObjective;id<=kAddFlagObjective;++id)
            if(GetMenuState(menu,id,MF_BYCOMMAND)!=UINT(-1))objective=true;
        for(UINT id=kObjectivePlayersFirst;id<=kObjectivePlayersLast;++id)
            if(GetMenuState(menu,id,MF_BYCOMMAND)!=UINT(-1))objective=true;
        if(!objective)return previousTrackMenu(menu,flags,x,y,reserved,window,rect);
        try
        {
            auto owner=ObjectiveOwner();if(owner.is_null())return FALSE;
            auto snapshot=Editor::InspectActor(owner);
            // Native actor popups post to their viewport owner, bypassing the
            // frame dispatcher. Consume our choices directly after the modal
            // loop, and retain native notification semantics for stock items.
            auto result=previousTrackMenu(menu,flags|TPM_RETURNCMD|TPM_NONOTIFY,x,y,reserved,window,rect);
            if(result>=kAddObjective && result<=kAddFlagObjective)
            {
                AddObjective(result,snapshot);return (flags&TPM_RETURNCMD)?0:TRUE;
            }
            if(result>=kObjectivePlayersFirst && result<=kObjectivePlayersLast)
            {
                RunObjectivePlayers(result,owner,window);return (flags&TPM_RETURNCMD)?0:TRUE;
            }
            if(flags&TPM_RETURNCMD)return result;
            if(result && !(flags&TPM_NONOTIFY))PostMessage(window,WM_COMMAND,MAKEWPARAM(result,0),0);
            return result!=0;
        }
        catch(const std::exception& e){MessageBoxA(window,e.what(),"Add Objective / Trigger",MB_OK|MB_ICONERROR);return FALSE;}
    }
    HMENU WINAPI LoadMenuHook(HINSTANCE instance,LPCSTR resource)
    {
        auto menu=previousLoadMenu(instance,resource);
        if(menu && reinterpret_cast<uintptr_t>(resource)<=65535)
        {
            const auto id=reinterpret_cast<uintptr_t>(resource);
            if(id==107 || id==108) if(auto context=GetSubMenu(menu,0))
            {
                bool available=false;
                try {Editor::BrushSnapBounds(id==108);available=true;}catch(const std::exception&){}
                if(available)
                {
                    auto snap=CreatePopupMenu();
                    if(snap)
                    {
                        const UINT first=id==108?kSurfaceSnapX:kBrushSnapX;
                        AppendMenuA(snap,MF_STRING,first,"&X axis");
                        AppendMenuA(snap,MF_STRING,first+1,"&Y axis");
                        AppendMenuA(snap,MF_STRING,first+2,"&Z axis");
                        AppendMenuA(snap,MF_SEPARATOR,0,nullptr);
                        AppendMenuA(snap,MF_STRING,first+3,"&All axes");
                        AppendMenuA(context,MF_POPUP,reinterpret_cast<UINT_PTR>(snap),"Snap brush &edge to grid");
                    }
                }
            }
            // Smooth Staircase goes at the top, under Properties, when a selected
            // brush (or on the surface menu a selected face's brush) is a
            // staircase, or already has a smoothing ramp to remove.
            if(id==107 || id==108) if(auto sub=GetSubMenu(menu,0))
            {
                Json state;
                try{state=Editor::StairSmoothState(id==108);}catch(const std::exception&){}
                const bool stairs=state.is_object() && state.value("stairs",false);
                const bool ramps=state.is_object() && state.value("ramps",0)>0;
                if(stairs || ramps)
                {
                    stairMenuOnSurface=id==108;
                    UINT at=2;
                    if(stairs)InsertMenuA(sub,at++,MF_BYPOSITION|MF_STRING,kSmoothStairs,ramps?"Re-&smooth Staircase":"&Smooth Staircase");
                    if(ramps)InsertMenuA(sub,at++,MF_BYPOSITION|MF_STRING,kRemoveStairSmoothing,"Remove Staircase Smoothing");
                    InsertMenuA(sub,at,MF_BYPOSITION|MF_SEPARATOR,0,nullptr);
                }
            }
            // Actor context resource 107 (verified native menu resource).
            if(reinterpret_cast<uintptr_t>(resource)==107) if(auto sub=GetSubMenu(menu,0))
            {
                AppendMenuA(sub,MF_STRING,kSaveAssembly,"Save Selection as &Assembly...");
                if(SelectionIsLinked())
                {
                    AppendMenuA(sub,MF_STRING,kPrefabSelectInstance,"Select Prefab &Instance");
                    AppendMenuA(sub,MF_STRING,kPrefabSelectAll,"Select All Instances of This Prefab");
                    AppendMenuA(sub,MF_STRING,kPrefabUpdate,"&Update Prefab from This Instance...");
                    AppendMenuA(sub,MF_STRING,kPrefabBreakLink,"Break Prefab Lin&k");
                }
                try {if(!Editor::SelectedEmitters().empty())AppendMenuA(sub,MF_STRING,kSaveToEmitterLibrary,"Save to &Emitter Library...");}
                catch(const std::exception&) { /* Only available with emitters selected in a source map. */ }
                AppendMenuA(sub,MF_STRING,MagicEventWorkbench::Command,"Edit SMagicEvent...");
                try
                {
                    if(!Editor::SelectedConveyorBelt().is_null())
                    {
                        AppendMenuA(sub,MF_STRING,kAddNextConveyorBelt,"Add &Next Conveyor Belt");
                        AppendMenuA(sub,MF_STRING,kAddNextConveyorBeltLeft,"Add Next Conveyor Belt, Turning &Left");
                        AppendMenuA(sub,MF_STRING,kAddNextConveyorBeltRight,"Add Next Conveyor Belt, Turning &Right");
                    }
                }
                catch(const std::exception&) { /* Only with one conveyor belt selected. */ }
                try {if(!Editor::SelectedMover().is_null())AppendMenuA(sub,MF_STRING,MoverWorkbench::kOpen,"M&over Workbench...");}
                catch(const std::exception&) { /* Only with one mover selected. */ }
                try {ObjectiveMenu(sub,ObjectiveOwner());}catch(const std::exception&){}
                try
                {
                    Editor::SelectedMeshBounds();
                    AppendMenuA(sub,MF_STRING,kFitBuilderBrush,"Position the builder brush around this");
                }
                catch(const std::exception&) { /* Only available for valid static-mesh selections. */ }
                try
                {
                    Editor::BrushSnapBounds(false);
                    AppendMenuA(sub,MF_STRING,kFitBuilderBrushToBrush,"Position the builder brush around this brush");
                }
                catch(const std::exception&) { /* Only available for valid editable-brush selections. */ }
                EditorExtras::AppendActorMenu(sub);
                PlacementTools::AppendActorMenu(sub);
            }
            // Brush/surface context resource: the native menu is the useful
            // target when the user right-clicks an existing BSP brush face.
            if(reinterpret_cast<uintptr_t>(resource)==108) if(auto sub=GetSubMenu(menu,0))
            {
                try
                {
                    Editor::BrushSnapBounds(false);
                    AppendMenuA(sub,MF_STRING,kFitBuilderBrushToBrush,"Position the builder brush around this brush");
                }
                catch(const std::exception&) { /* Only available for valid editable-brush selections. */ }
            }
            // Backdrop, actor and surface popups all set the click location
            // that their stock Paste > Here uses.
            if(id>=106 && id<=108) if(auto sub=GetSubMenu(menu,0)) if(auto builder=CreatePopupMenu())
            {
                builderMenuOnSurface=id==108;
                AppendMenuA(builder,MF_STRING,kPlaceBuilderBrush,"&Place Here");
                AppendMenuA(builder,MF_STRING,kRebuildPlaceBuilderBrush,"&Rebuild and Place Here");
                AppendMenuA(sub,MF_POPUP,reinterpret_cast<UINT_PTR>(builder),"&Builder Brush");
                AppendMenuA(sub,MF_STRING,kAddConveyorBeltHere,"Add Con&veyor Belt Here");
                MeasureTool::AddMenu(sub,id==108);
            }
        }
        return menu;
    }
}
bool AppendObjectiveMenu(HMENU menu,const Json& actor){return ObjectiveMenu(menu,actor);}
Json SaveEmitterSelection(HWND owner,bool confirm)
{
    auto members=Editor::SelectedEmitters();
    if(members.empty()) throw std::runtime_error("Select one or more emitters first.");
    // Capture first: a map-local asset or an outside link is refused before any typing.
    auto entry=Editor::CaptureEmitters(members);
    // A damaged library file refuses too, rather than after the form.
    for(const auto& problem:Editor::EmitterLibraryProblems()) if(problem==Json(Workflow::EmitterLibrary::DamagedFile())) throw std::runtime_error(problem.get<std::string>());
    std::vector<std::string> categories; for(const auto& category:Editor::EmitterCategories()) categories.push_back(Recode(category.get<std::string>(),CP_UTF8,CP_ACP));
    std::vector<InputField> fields={{"Name",Recode(entry.at("name").get<std::string>(),CP_UTF8,CP_ACP),{}},{"Category (choose or type)",Recode(entry.at("category").get<std::string>(),CP_UTF8,CP_ACP),categories,true},{"Description (optional)","",{}}};
    if(!Ask(owner,"Save to Emitter Library",fields)) return {};
    entry["name"]=Recode(fields[0].value,CP_ACP,CP_UTF8); entry["category"]=Recode(fields[1].value,CP_ACP,CP_UTF8); entry["description"]=Recode(fields[2].value,CP_ACP,CP_UTF8);
    auto saved=Editor::SaveEmitterEntry(entry);
    if(confirm)
    {
        const auto count=saved.at("actors").size();
        auto text="Saved '"+Recode(saved.at("name").get<std::string>(),CP_UTF8,CP_ACP)+"' to the Emitter Library under "+Recode(saved.at("category").get<std::string>(),CP_UTF8,CP_ACP)+" ("+std::to_string(count)+(count==1?" emitter).":" actors).");
        MessageBoxA(owner,text.c_str(),"Emitter Library",MB_OK|MB_ICONINFORMATION);
    }
    return saved;
}
Json AskEmitterDetails(HWND owner,const Json& entry,const char* title){return AskDetails(owner,entry,title,Editor::EmitterCategories());}
Json AskDetails(HWND owner,const Json& entry,const char* title,const Json& choices)
{
    std::vector<std::string> categories; for(const auto& category:choices) categories.push_back(Recode(category.get<std::string>(),CP_UTF8,CP_ACP));
    std::string description=Recode(entry.value("description",std::string{}),CP_UTF8,CP_ACP); std::replace(description.begin(),description.end(),'\n',' ');
    std::vector<InputField> fields={{"Name",Recode(entry.value("name",std::string{}),CP_UTF8,CP_ACP),{}},{"Category (choose or type)",Recode(entry.value("category",std::string{}),CP_UTF8,CP_ACP),categories,true},{"Description (optional)",description,{}}};
    if(!Ask(owner,title,fields)) return {};
    return {{"name",Recode(fields[0].value,CP_ACP,CP_UTF8)},{"category",Recode(fields[1].value,CP_ACP,CP_UTF8)},{"description",Recode(fields[2].value,CP_ACP,CP_UTF8)}};
}
void RunObjectiveCommand(UINT command,const Json& snapshot)
{
    if(command>=kObjectivePlayersFirst && command<=kObjectivePlayersLast)
    {
        RunObjectivePlayers(command,snapshot.at("actor"),GetActiveWindow());
        return;
    }
    if(command<kAddObjective || command>kAddFlagObjective)throw std::runtime_error("Invalid objective command.");
    AddObjective(command,snapshot);
}
void CameraBookmark(int slot,bool store,bool fromMenu)
{
    try
    {
        Sync();
        if(store)
        {
            Json next=document; auto& views=next["maps"][mapKey]["views"];
            views=Bookmarks::Assign(views,slot,Editor::CaptureView().at("cameras"),Id(),Timestamp());
            const auto id=views[static_cast<size_t>(Bookmarks::FindSlot(views,slot))].at("id").get<std::string>();
            Save(next);
            // Best effort, as for any view: the bookmark is kept without one.
            std::string problem;
            if(!CaptureEntryThumbnail(true,id,Json{},problem)) Logger::log("Camera bookmarks: "+Bookmarks::Label(slot)+" has no thumbnail: "+problem);
            for(auto s:windows) if(s->kind==Kind::Views)
            {
                try { Populate(*s); } catch(const std::exception& e) { Status(*s,e.what()); }
            }
            return;
        }
        const auto& views=document.at("maps").at(mapKey).at("views");
        const int at=Bookmarks::FindSlot(views,slot);
        if(at>=0) { Editor::RestoreView(Bookmarks::CamerasOnly(views[static_cast<size_t>(at)])); return; }
        if(!fromMenu) { Logger::log("Camera bookmarks: no "+Bookmarks::Label(slot)+" in this map yet."); MessageBeep(MB_ICONWARNING); return; }
        const auto text="No camera bookmark "+std::to_string(slot)+" in this map yet. Press Ctrl+"+std::to_string(slot)+" over a viewport to set it.";
        MessageBoxA(GetActiveWindow(),text.c_str(),"Camera Bookmarks",MB_OK|MB_ICONINFORMATION);
    }
    catch(const std::exception& e)
    {
        Logger::log(std::string("Camera bookmarks: ")+e.what());
        if(fromMenu) MessageBoxA(GetActiveWindow(),e.what(),"Camera Bookmarks",MB_OK|MB_ICONERROR);
        else MessageBeep(MB_ICONWARNING);
    }
}
bool HandleCommand(UINT command)
{
    if(EditorExtras::HandleCommand(command))return true;
    if(command==UndoHistory::kOpen || command==UndoHistory::kAddCheckpoint)
    {
        try{UndoHistory::HandleCommand(command);}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Undo History",MB_OK|MB_ICONERROR);}
        return true;
    }
    if(command==FavoritesWindow::kOpen)
    {
        try{FavoritesWindow::Open();}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Favorites",MB_OK|MB_ICONERROR);}
        return true;
    }
    if(CommandPalette::HandleCommand(command))return true;
    if(command==CompareBackup::kCompare)
    {
        try{CompareBackup::Open(Editor::MainWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Compare with Backup",MB_OK|MB_ICONERROR);}
        return true;
    }
    if(MeasureTool::HandleCommand(command))return true;
    if(PlacementTools::HandleCommand(command))return true;
    if(ScatterTool::HandleCommand(command))return true;
    if(PropertyPaste::HandleCommand(command))return true;
    if(LightShadowMap::HandleCommand(command))return true;
    if(Reachability::HandleCommand(command))return true;
    if(LightingBudget::HandleCommand(command))return true;
    if(MapCheck::HandleCommand(command))return true;
    if(RenderBudget::HandleCommand(command))return true;
    if(MapOutliner::HandleCommand(command))return true;
    if(MoverWorkbench::HandleCommand(command))return true;
    if(MinimapWindow::HandleCommand(command))return true;
    if(ObjExportWindow::HandleCommand(command))return true;
    if(MapRecovery::HandleCommand(command))return true;
    if(command==StoreyFilter::kOpen)
    {
        try{StoreyFilter::Open();}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Storeys",MB_OK|MB_ICONERROR);}
        return true;
    }
    if(command>=kPrefabSelectInstance && command<=kPrefabBreakLink)
    {
        const char* titles[]={"Select Prefab Instance","Select All Instances","Update Prefab from This Instance","Break Prefab Link"};
        const auto title=titles[command-kPrefabSelectInstance]; HWND owner=GetActiveWindow();
        try
        {
            std::string text;
            if(command==kPrefabSelectInstance) text=SelectInstance();
            else if(command==kPrefabSelectAll) text=SelectAllInstances({});
            else if(command==kPrefabUpdate) text=UpdatePrefab(owner,true);
            else text=BreakLink();
            if(!text.empty()) Logger::log("Linked prefabs: "+text);
            RefreshAssemblyWindows();
            // Only a brush note needs the user's eyes; the rest shows in the viewports.
            if(command==kPrefabUpdate && text.find("rebuild the geometry")!=std::string::npos) MessageBoxA(owner,text.c_str(),title,MB_OK|MB_ICONINFORMATION);
        }
        catch(const std::exception& e){MessageBoxA(owner,e.what(),title,MB_OK|MB_ICONINFORMATION);}
        return true;
    }
    if(command==kBrushVisibility)
    {
        try{OpenBrushVisibility();}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Brush Visibility",MB_OK|MB_ICONERROR);}
        return true;
    }
    if(command>=kBookmarkGoFirst && command<=kBookmarkLast)
    {
        const bool store=command>=kBookmarkSetFirst;
        CameraBookmark(Bookmarks::kFirst+static_cast<int>(command-(store?kBookmarkSetFirst:kBookmarkGoFirst)),store,true);
        return true;
    }
    if(command>=kAddObjective && command<=kAddFlagObjective)
    {
        try
        {
            auto owner=ObjectiveOwner();if(owner.is_null())throw std::runtime_error("Select exactly one mission or objective.");
            AddObjective(command,Editor::InspectActor(owner));
        }
        catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Add Objective / Trigger",MB_OK|MB_ICONERROR);}
        return true;
    }
    if(command>=kObjectivePlayersFirst && command<=kObjectivePlayersLast)
    {
        try
        {
            auto owner=ObjectiveOwner();if(owner.is_null())throw std::runtime_error("Select exactly one objective or zone mission.");
            RunObjectivePlayers(command,owner,GetActiveWindow());
        }
        catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Players in Match",MB_OK|MB_ICONERROR);}
        return true;
    }
    if(command==MapUsagesWindow::kFindMaterial || command==MapUsagesWindow::kFindMesh)
    {
        FindUsagesInAllMaps(GetActiveWindow(),command==MapUsagesWindow::kFindMesh);
        return true;
    }
    if(command==40948){try{OpenDesign(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Map Design",MB_OK|MB_ICONERROR);}return true;}
    if(command==kAddVertexPortal)
    {
        try{Editor::AddVertexPortal();}
        catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Add Portal",MB_OK|MB_ICONINFORMATION);}
        return true;
    }
    if(command>=kVertexSnapX && command<=kVertexSnapAll)
    {
        try{const auto axis=command-kVertexSnapX;Editor::SnapSelectedBrushVertices(axis==3?7u:1u<<axis);}
        catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Snap Selected Vertices",MB_OK|MB_ICONINFORMATION);}
        return true;
    }
    if(command>=kBrushSnapX && command<=kSurfaceSnapAll)
    {
        const bool surfaces=command>=kSurfaceSnapX;
        const auto choice=command-(surfaces?kSurfaceSnapX:kBrushSnapX);
        try {Editor::SnapBrushesToGrid(choice==3?7u:1u<<choice,surfaces);}
        catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Snap Brush Edge to Grid",MB_OK|MB_ICONINFORMATION);}
        return true;
    }
    if(command==kFitBuilderBrush)
    {
        try { Editor::FitBuilderBrushToMeshes(); }
        catch(const std::exception& e) { MessageBoxA(GetActiveWindow(),e.what(),"Position Builder Brush",MB_OK|MB_ICONINFORMATION); }
        return true;
    }
    if(command==kFitBuilderBrushToBrush)
    {
        try { Editor::FitBuilderBrushToBrushes(); }
        catch(const std::exception& e) { MessageBoxA(GetActiveWindow(),e.what(),"Position Builder Brush",MB_OK|MB_ICONINFORMATION); }
        return true;
    }
    if(command==kSmoothStairs)
    {
        try { Editor::SmoothStairs(stairMenuOnSurface); }
        catch(const std::exception& e) { MessageBoxA(GetActiveWindow(),e.what(),"Smooth Staircase",MB_OK|MB_ICONINFORMATION); }
        return true;
    }
    if(command==kRemoveStairSmoothing)
    {
        try { Editor::RemoveStairSmoothing(stairMenuOnSurface); }
        catch(const std::exception& e) { MessageBoxA(GetActiveWindow(),e.what(),"Remove Staircase Smoothing",MB_OK|MB_ICONINFORMATION); }
        return true;
    }
    if(command==kAddNextConveyorBelt || command==kAddNextConveyorBeltLeft || command==kAddNextConveyorBeltRight)
    {
        try
        {
            auto belt=Editor::SelectedConveyorBelt();
            if(belt.is_null())throw std::runtime_error("Select one conveyor belt first.");
            Json spec={{"after",belt.at("path")}};
            if(command!=kAddNextConveyorBelt)spec["turn"]=command==kAddNextConveyorBeltLeft?"left":"right";
            const auto added=Editor::AddConveyorBelt(spec);
            if(added.contains("warning"))MessageBoxA(GetActiveWindow(),added.at("warning").get<std::string>().c_str(),"Conveyor Belt",MB_OK|MB_ICONINFORMATION);
        }
        catch(const std::exception& e) { MessageBoxA(GetActiveWindow(),e.what(),"Conveyor Belt",MB_OK|MB_ICONINFORMATION); }
        return true;
    }
    if(command==kAddConveyorBelt || command==kAddConveyorBeltHere)
    {
        // From the viewport menu a click on a floor stands the belt on it; a backdrop
        // or actor click leaves a stale surface plane.
        try { Editor::AddConveyorBelt({{"surface",command==kAddConveyorBelt || builderMenuOnSurface}}); }
        catch(const std::exception& e) { MessageBoxA(GetActiveWindow(),e.what(),"Conveyor Belt",MB_OK|MB_ICONINFORMATION); }
        return true;
    }
    if(command==kPlaceBuilderBrush || command==kRebuildPlaceBuilderBrush)
    {
        try { Editor::PlaceBuilderBrushAtClick(command==kRebuildPlaceBuilderBrush,builderMenuOnSurface); }
        catch(const std::exception& e) { MessageBoxA(GetActiveWindow(),e.what(),"Builder Brush",MB_OK|MB_ICONINFORMATION); }
        return true;
    }
    if(command==MapRecovery::kRecalculateLightingCommandId){MapRecovery::RecalculateLighting(GetActiveWindow());return true;}
    if(command==MapRecovery::kRecalculateSelectedLightingCommandId){MapRecovery::RecalculateSelectedLighting(GetActiveWindow());return true;}
    if(command==MapRecovery::kMatchSelectedLightingCommandId){MapRecovery::RecalculateSelectedLighting(GetActiveWindow(),true);return true;}
    if(command==MapOptimiseDialog::Command){try{MapOptimiseDialog::Open(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Optimise Map Assets",MB_OK|MB_ICONERROR);}return true;}
    if(command==MapPackageDialog::Command){try{MapPackageDialog::Open(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Map Packaging",MB_OK|MB_ICONERROR);}return true;}
    if(command==MapAuthoringDialog::Export || command==MapAuthoringDialog::Import){MapAuthoringDialog::Open(GetActiveWindow(),command==MapAuthoringDialog::Export);return true;}
    if(command==MagicEventWorkbench::Command){try{MagicEventWorkbench::Open(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"SMagicEvent Workbench",MB_OK|MB_ICONERROR);}return true;}
    if(command==CameraNetworkPanel::Command){try{CameraNetworkPanel::Open(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"SCamNetwork Manager",MB_OK|MB_ICONERROR);}return true;}
    if(command==ObjImportWindow::Command){try{ObjImportWindow::Open(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Import OBJ",MB_OK|MB_ICONERROR);}return true;}
    if(command==CharacterSkinsWindow::Command){try{CharacterSkinsWindow::Open(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Character Skins",MB_OK|MB_ICONERROR);}return true;}
    if(command==CharacterSkinPresetsWindow::Command){try{CharacterSkinPresetsWindow::Open(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Character Skin Presets",MB_OK|MB_ICONERROR);}return true;}
    if(command==EmitterLibraryWindow::Command){try{EmitterLibraryWindow::Open(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Emitter Library",MB_OK|MB_ICONERROR);}return true;}
    if(command==kSaveToEmitterLibrary){try{SaveEmitterSelection(GetActiveWindow());}catch(const std::exception& e){MessageBoxA(GetActiveWindow(),e.what(),"Save to Emitter Library",MB_OK|MB_ICONERROR);}return true;}
    if(command<kConnections || command>kSaveAssembly) return false;
    try
    {
        Kind kind=command==kConnections?Kind::Connections:command==kViews?Kind::Views:Kind::Assemblies;
        auto window=Open(kind,GetActiveWindow());
        if(command==kSaveAssembly && window) SendMessage(window,WM_COMMAND,Secondary,0);
    }
    catch(const std::exception& e) {MessageBoxA(GetActiveWindow(),e.what(),"RE+ Tools",MB_OK|MB_ICONERROR);}
    return true;
}
Json PrefabRequest(const std::string& op,const Json& q)
{
    Sync();
    auto assembly=[&]()->Json {
        const auto key=q.at("assembly").get<std::string>();
        for(const auto& entry:document.at("assemblies")) if(entry.value("id",std::string{})==key || entry.value("name",std::string{})==key) return entry;
        throw std::runtime_error("No assembly called "+key);
    };
    Json result;
    if(op=="prefab.save")
    {
        // Save Selection as New with the selection centre as its pivot.
        const auto members=q.contains("members")?q.at("members"):Editor::SelectedIdentities();
        auto snapshot=Editor::CaptureAssembly(members,Pose{}); Vector lo{},hi{}; bool first=true;
        for(const auto& a:snapshot.at("actors")) { Vector p=a.at("position").get<Vector>(); if(first) {lo=hi=p; first=false;} else for(int i=0;i<3;++i){lo[i]=std::min(lo[i],p[i]);hi[i]=std::max(hi[i],p[i]);} }
        Pose frame; for(int i=0;i<3;++i) frame.position[i]=(lo[i]+hi[i])/2;
        auto captured=CanonicalizeAssembly(Editor::CaptureAssembly(members,frame),{}); captured.erase("local");
        captured["name"]=q.value("name",std::string("Prefab")); captured["id"]=Id(); captured["modified"]=Timestamp();
        Json next=document; next["assemblies"].push_back(captured); Save(next); result=captured;
    }
    else if(op=="prefab.place")
        result=PlaceLinked(assembly(),{q.at("position").get<Vector>(),q.value("rotation",Rotation{})},q.value("bindings",Json::object()).get<std::map<std::string,std::string>>());
    else if(op=="prefab.instances")
    {
        const auto members=PrefabMembers(); result=Json::array();
        for(const auto& record:document.at("maps").at(mapKey).value("instances",Json::array())) if(Prefab::Linked(record))
        {
            auto row=record; row.erase("locals"); row["live"]=Identities(MembersOf(members,record)); result.push_back(row);
        }
    }
    else if(op=="prefab.counts") { result=Json::object(); for(const auto& [id,count]:LinkedCounts()) result[id]=count; }
    else if(op=="prefab.update") result=UpdatePrefab(nullptr,false);
    else if(op=="prefab.break") result=BreakLink();
    else if(op=="prefab.selectinstance") result=SelectInstance();
    else if(op=="prefab.selectall") result=SelectAllInstances(q.value("assembly",std::string{}));
    else throw std::runtime_error("Unknown prefab request.");
    RefreshAssemblyWindows();
    return result;
}
Json MapMovementLimits()
{
    Sync();
    const auto maps=document.find("maps");
    if(maps==document.end() || !maps->is_object())return nullptr;
    const auto map=maps->find(mapKey);
    if(map==maps->end() || !map->is_object())return nullptr;
    const auto design=map->find("design");
    if(design==map->end() || !design->is_object() || !design->contains("movement"))return nullptr;
    return Design::Movement(*design);
}
void FindUsages(HWND owner,void*,bool mesh)
{
    try { Open(Kind::Assets,owner,Editor::CurrentAsset(mesh),mesh); }
    catch(const std::exception& e) {MessageBoxA(owner,e.what(),"Find Asset Usages",MB_OK|MB_ICONERROR);}
}
void FindUsagesInAllMaps(HWND owner,bool mesh)
{
    // The browser's current asset, if any; the window also takes a typed name.
    std::string asset;
    try { asset=Editor::CurrentAsset(mesh); } catch(const std::exception&) {}
    try { MapUsagesWindow::Open(owner,asset); }
    catch(const std::exception& e) {MessageBoxA(owner,e.what(),"Find Usages in All Maps",MB_OK|MB_ICONERROR);}
}
void RenameActorTag(HWND owner,std::string path,std::string type)
{
    try
    {
        Json identity={{"path",path},{"class",type}};std::string tag="NewTag";
        for(const auto& actor:Editor::Actors())if(actor.at("path")==path && actor.at("class")==type)tag=actor.at("tag");
        if(Fold(tag)=="none")tag="NewTag";
        if(!GetName(owner,"Rename Tag",tag))return;
        auto preview=Editor::PreviewTagRename(identity,tag);size_t targets=0,events=0;
        for(const auto& c:preview.at("changes"))if(c.at("property")=="Tag")++targets;else ++events;
        std::string text="Rename Tag '"+preview.at("old").get<std::string>()+"' to '"+tag+"'?\n\n";
        TagPreviewText details{targets>1?"Rename Shared Tag Group":"Preview Tag Rename",text,&preview};
        if(!ConfirmTagRename(owner,details))return;
        Editor::RenameTag(preview);
    }
    catch(const std::exception& e){MessageBoxA(owner,e.what(),"Rename Tag",MB_OK|MB_ICONERROR);}
}
void Initialize()
{
    Editor::Initialize();
    PropertySearch::Strip strip;strip.update=StripUpdate;strip.layout=StripLayout;strip.command=StripCommand;strip.closed=StripClosed;
    PropertySearch::SetStrip(strip);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
    previousLoadMenu=*reinterpret_cast<LoadMenuFn*>(0x11AF23F0);
    auto hook=&LoadMenuHook; MemoryWriter::WriteBytes(0x11AF23F0,&hook,sizeof(hook));
    previousTrackMenu=*reinterpret_cast<TrackMenuFn*>(0x11af23c0);
    auto track=&TrackMenuHook;MemoryWriter::WriteBytes(0x11af23c0,&track,sizeof(track));
    MemoryWriter::WriteJump(0x10e045e9,reinterpret_cast<void(*)()>(&VertexClickHook));
}
}

// Requests the bridge below does not take itself: its else-if chain is at the
// compiler's nesting limit (C1061), so newer features add theirs here.
static Workflow::Json MoreWorkflowRequests(const std::string& op,const Workflow::Json& q)
{
    using namespace Workflow;
    if(op=="objimport.preview") return Editor::ObjImportPreview(q);
    if(op=="objimport.import") return Editor::ImportObjMesh(q);
    if(op=="objectiveplayers.state") return Editor::ObjectivePlayerRules();
    if(op.rfind("prefab.",0)==0) return WorkflowTools::PrefabRequest(op,q);
    if(op=="conveyor.state") return Editor::ConveyorState();
    if(op=="conveyor.prepare") {Editor::PrepareConveyors();return Editor::ConveyorState();}
    if(op=="conveyor.add") return Editor::AddConveyorBelt(q);
    if(op=="conveyor.sync") return Editor::SyncConveyorBelts();
    if(op=="objectiveplayers.set") return Editor::SetObjectivePlayers(q.at("objective"),q.value("min",0),q.value("max",0),q.value("open",false));
    if(op=="objectiveplayers.playlevel") return Editor::SetObjectivePlayLevelPlayers(q.value("players",0));
    if(op=="objectiveplayers.targets") return Editor::SetMissionTargets(q.at("mission"),q.at("sizes"));
    if(op=="mover.selected") return Editor::SelectedMover();
    if(op=="mover.state") return Editor::MoverState(q.at("actor"));
    if(op=="mover.path") return Editor::SetMoverPath(q.at("actor"),q.at("path"),q.value("label",std::string("edit keys")));
    if(op=="mover.settings") return Editor::SetMoverSettings(q.at("actor"),q.at("changes"));
    if(op=="mover.outline") return Editor::MoverOutline(q.at("actor"));
    if(op=="mover.candidates") return Editor::MoverTriggerCandidates(q.at("actor"));
    if(op=="mover.wire") return Editor::WireMoverTrigger(q.at("actor"),q.at("trigger"));
    if(op=="mover.preview") {Editor::MoverPreviewPose(q.at("actor"),{q.at("position").get<Vector>(),q.value("rotation",Rotation{})});return true;}
    if(op=="mover.restore") {Editor::MoverRestorePose();return true;}
    throw std::runtime_error("Unknown workflow request.");
}

// JSON bridge used by the isolated native integration probe. Calls must be made
// on the editor UI thread, exactly like the corresponding dialog operations.
extern "C" __declspec(dllexport) int __cdecl ReloadedWorkflowRequest(const char* request,char* output,unsigned capacity)
{
    using namespace Workflow;
    try
    {
        Json q=Json::parse(request),result; std::string op=q.at("op");
        if(op=="actors") result=Editor::Actors(q.value("selected",false));
        else if(op=="map.file")result=Editor::MapFile();
        else if(op=="map.setfile"){Editor::SetMapFile(q.at("file").get<std::string>());result=Editor::MapFile();}
        else if(op=="design.scene")result=Editor::DesignScene();
        else if(op=="design.block")result=Editor::DesignBlockout(q.at("spec"),{q.at("position").get<Vector>(),q.at("rotation").get<Rotation>()},q.value("previous",Json{}));
        else if(op=="design.align"){Editor::DesignAlign(q.at("scene"),q.at("axis"),q.at("mode"),q.value("spacing",0.0));result=true;}
        else if(op=="design.layer"){Editor::DesignLayer(q.at("members"),q.at("hidden"),q.at("locked"),q.value("group",std::string()),q.value("groupAction",std::string("none")));result=true;}
        else if(op=="design.grid")result=Editor::DesignGrid();
        else if(op=="design.view")result=WorkflowTools::DesignView();
        else if(op=="design.polyflags")result=q.contains("flags")?Json(Editor::DesignSetPolyFlags(q.at("members"),q.at("flags"))):Editor::DesignPolyFlags(q.at("members"));
        else if(op=="design.repairflags")result=WorkflowTools::DesignRepairFlags();
        else if(op=="design.bspowners")result=Editor::BspSurfaceOwners();
        else if(op=="design.sendtolast")result=Editor::DesignSendToLast(q.at("members"));
        else if(op=="design.builds")result=Editor::GeometryBuilds();
        else if(op=="design.blockbatch")result=Editor::DesignBlockoutBatch(q.at("items"));
        else if(op=="design.flags"){Editor::DesignSetFlags(q.at("members"),q.value("hidden",-1),q.value("locked",-1));result=true;}
        else if(op=="design.group"){Editor::DesignGroupMembers(q.at("members"),q.at("group"),q.at("action"));result=true;}
        else if(op=="design.translate"){Editor::DesignTranslate(q.at("members"),q.at("delta").get<Vector>());result=true;}
        else if(op=="placement.selection")result=Editor::PlacementSelection();
        else if(op=="placement.drop")result=Editor::PlacementDrop(q.value("surface",0),q.value("align",false));
        else if(op=="design.lift")result=Editor::CreateLift(q.at("position").get<Vector>(),q.value("width",192.0),q.value("length",192.0),q.value("thickness",16.0),q.at("rise"),q.value("moveTime",2.0));
        else if(op=="security.actors")result=Editor::SecurityActors();
        else if(op=="light.list")result=Editor::Lights();
        else if(op=="objective.actors")result=Editor::ObjectiveActors();
        else if(op=="start.create")result=Editor::CreatePlayerStart(q.value("class",std::string("Engine.PlayerStart")),q.value("team",std::string("0")),{q.at("position").get<Vector>(),q.value("rotation",Rotation{})});
        else if(op=="alarm.locks")result=Editor::AddAlarmLocks(q.at("alarm"),q.at("targets"));
        else if(op=="light.set")result=Editor::SetActorProperties(q.at("actor"),q.value("properties",Json::object()));
        else if(op=="security.create")result=Editor::CreateSecurityActor(q.at("class"),{q.at("position").get<Vector>(),q.value("rotation",Rotation{})},q.value("properties",Json::object()));
        else if(op=="security.sensor")result=Editor::CreateMotionSensor(q.at("low").get<Vector>(),q.at("high").get<Vector>(),q.value("properties",Json::object()));
        else if(op=="security.link"){Editor::LinkDetectorToAlarm(q.at("detector"),q.at("alarm"));result=true;}
        else if(op=="security.outputs"){Editor::AddAlarmOutputs(q.at("alarm"),q.at("targets"));result=true;}
        else if(op=="security.unlink"){Editor::UnwireDetector(q.at("detector"));result=true;}
        else if(op=="security.delete"){Editor::DeleteSecurityActor(q.at("actor"));result=true;}
        else if(op=="security.move")result=Editor::MoveSecurityActor(q.at("actor"),{q.at("position").get<Vector>(),q.value("rotation",Rotation{})},q.value("properties",Json::object()));
        else if(op=="stage.actors")result=Editor::StageActors();
        else if(op=="stage.read")result=Editor::StagePlan();
        else if(op=="stage.preview")result=Editor::PreviewStages(q.at("plan"));
        else if(op=="stage.apply")result=Editor::ApplyStages(q.at("plan"));
        else if(op=="design.build"){Editor::BuildGeometry();result=true;}
        else if(op=="design.tempstart")result=Editor::DesignTemporaryStart(q.at("class"),q.value("team",std::string()),{q.at("position").get<Vector>(),q.at("rotation").get<Rotation>()});
        else if(op=="design.removestart"){Editor::DesignRemoveTemporaryStart(q.at("start"));result=true;}
        else if(op=="design.spawns")result=Editor::DesignSpawns();
        else if(op=="design.clearances")result=Editor::DesignClearances();
        else if(op=="design.play"){Editor::DesignPlay(q.at("start"),{q.at("position").get<Vector>(),q.at("rotation").get<Rotation>()},q.value("launch",false));result=true;}
        else if(op=="optimise.report") result=Editor::OptimiseReport(q.value("base",std::string()));
        else if(op=="optimise.release") result=Editor::OptimiseRelease(q);
        else if(op=="package.preview") {MapPackageDialog::Preview(GetActiveWindow(),q.at("map").get<std::string>());result=true;}
        else if(op=="authoring.export") result=Editor::ExportMapAuthoring();
        else if(op=="authoring.preview"){auto document=q.at("document");Authoring::Rebase(document,Authoring::Utf8Path(q.value("baseDirectory",std::string{})));result=Editor::PreviewMapAuthoring(document);}
        else if(op=="authoring.apply"){auto document=q.at("document");Authoring::Rebase(document,Authoring::Utf8Path(q.value("baseDirectory",std::string{})));result=Editor::ApplyMapAuthoring(document);}
        else if(op=="magic.classes") result=Editor::EventClasses();
        else if(op=="camera.snapshot") result=Editor::CameraNetworks();
        else if(op=="camera.order") {Editor::OrderCameras(q.at("snapshot"),q.at("paths"),q.value("loop",false),q.value("detach",std::string{}));result=true;}
        else if(op=="camera.add") result=Editor::AddNetworkCamera(q.at("snapshot"),q.at("paths"),q.value("loop",false));
        else if(op=="magic.assets") result=Editor::EventAssets(q.at("type"),q.value("classes",false));
        else if(op=="magic.component") result=Editor::CreateEventComponent(q.at("snapshot"),q.at("class"));
        else if(op=="magic.key") {Editor::CaptureMoverKey(q.at("snapshot"),q.at("key"));result=true;}
        else if(op=="magic.inspect") result=Editor::InspectActor(q.at("actor"));
        else if(op=="magic.json.export") result=Editor::ExportEventJson(q.at("actor"));
        else if(op=="magic.json.import") {Editor::ImportEventJson(q.at("snapshot"),q.at("document"));result=true;}
        else if(op=="magic.edit") {Editor::EditActor(q.at("snapshot"),q.at("changes"));result=true;}
        else if(op=="magic.create") result=Editor::CreateEventActor(q.at("class"),q.value("event",Json{}),q.value("trigger",false),q.value("geometry",std::string("point")),q.value("group",0));
        else if(op=="magic.link") {Editor::LinkEventActor(q.at("event"),q.at("target"),q.value("trigger",false),q.value("group",0));result=true;}
        else if(op=="select") {Editor::Select(q.at("actors"),q.value("focus",false));result=true;}
        else if(op=="view.capture") result=Editor::CaptureView();
        else if(op=="brush.visibility") result=Editor::BrushVisibility();
        else if(op=="brush.visibility.set") {Editor::SetBrushVisibility(q.at("category"),q.at("action"));result=true;}
        else if(op=="view.restore") result=Editor::RestoreView(q.at("view"));
        else if(op=="usages") result=Editor::FindUsages(q.at("asset"));
        else if(op=="replace") {Editor::ReplaceUsages(q.at("usages"),q.at("source"),q.at("replacement"));result=true;}
        else if(op=="connections") result=Editor::Connections(q.at("actor"));
        else if(op=="objective.add") result=Editor::AddObjectiveActor(q.at("owner"),q.at("class"));
        else if(op=="surface.brushes") result=Editor::SelectedSurfaceBrushes();
        else if(op=="mesh.bounds") result=Editor::SelectedMeshBounds();
        else if(op=="builder.fit") {Editor::FitBuilderBrushToMeshes();result=true;}
        else if(op=="builder.fit.brush") {Editor::FitBuilderBrushToBrushes();result=true;}
        else if(op=="brush.snap.bounds") result=Editor::BrushSnapBounds(q.value("surfaces",false));
        else if(op=="vertex.selection") result=Editor::SelectedBrushVertices();
        else if(op=="vertex.portal") result=Editor::AddVertexPortal();
        else if(op=="stairs.state") result=Editor::StairSmoothState(q.value("surfaces",false));
        else if(op=="stairs.smooth") result=Editor::SmoothStairs(q.value("surfaces",false));
        else if(op=="stairs.remove") result=Editor::RemoveStairSmoothing(q.value("surfaces",false));
        else if(op=="vertex.snap") {Editor::SnapSelectedBrushVertices(q.at("axes").get<unsigned>());result=true;}
        else if(op=="brush.snap") {Editor::SnapBrushesToGrid(q.at("axes").get<unsigned>(),q.value("surfaces",false));result=true;}
        else if(op=="tag.preview") result=Editor::PreviewTagRename(q.at("actor"),q.at("tag"));
        else if(op=="tag.rename") {Editor::RenameTag(q.at("preview"));result=true;}
        else if(op=="assembly.capture") result=Editor::CaptureAssembly(q.at("members"),{q.at("position").get<Vector>(),q.at("rotation").get<Rotation>()});
        else if(op=="assembly.place") result=Editor::PlaceAssembly(q.at("definition"),{q.at("position").get<Vector>(),q.at("rotation").get<Rotation>()},q.at("bindings").get<std::map<std::string,std::string>>());
        else if(op=="emitterlib.list") result=Editor::EmitterLibrary();
        else if(op=="emitterlib.packs") result=Editor::EmitterPacks();
        else if(op=="emitterlib.cache") result=Editor::EmitterLibraryCacheState();
        else if(op=="emitterlib.problems") result=Editor::EmitterLibraryProblems();
        else if(op=="emitterlib.categories") result=Editor::EmitterCategories();
        else if(op=="emitterlib.selected") result=Editor::SelectedEmitters();
        else if(op=="emitterlib.capture") result=Editor::CaptureEmitters(q.contains("members")?q.at("members"):Editor::SelectedEmitters(),q.value("map",std::string{}));
        else if(op=="emitterlib.save")
        {
            // An entry (a draft or a listed entry) or a fresh capture; the
            // text fields given here override the entry's own.
            Json entry=q.contains("entry")?q.at("entry"):Editor::CaptureEmitters(q.contains("members")?q.at("members"):Editor::SelectedEmitters(),q.value("map",std::string{}));
            for(const char* key:{"name","category","description","preview"}) if(q.contains(key)) entry[key]=q.at(key);
            result=Editor::SaveEmitterEntry(entry);
        }
        else if(op=="emitterlib.place")
        {
            Json entry=q.contains("entry")?q.at("entry"):EmitterLibrary::Find(*Editor::EmitterLibraryState().entries,q.contains("id")?q.at("id").get<std::string>():q.at("name").get<std::string>());
            Pose pose{q.contains("position")?q.at("position").get<Vector>():Editor::BuilderPose().position,q.value("rotation",Rotation{})};
            result=Editor::PlaceEmitterEntry(entry,pose);
        }
        else if(op=="emitterlib.delete") {Editor::DeleteEmitterEntry(q.at("id"));result=true;}
        else if(op=="emitterlib.update")
        {
            Json changes=Json::object(); for(const char* key:{"name","category","description"}) if(q.contains(key)) changes[key]=q.at(key);
            result=Editor::UpdateEmitterEntry(q.at("id"),changes);
        }
        else if(op=="emitterlib.restore") {Editor::RestoreBuiltinEmitters();result=true;}
        else if(op=="preview.open"){std::string error;if(!EmitterPreview::OpenTestWindow(error))throw std::runtime_error(error);result=EmitterPreview::State();}
        else if(op=="preview.show"){EmitterPreview::Show(q.at("entry"));result=EmitterPreview::State();}
        else if(op=="preview.capture") result=EmitterPreview::Capture(q.value("path",std::string()));
        else if(op=="preview.clear"){EmitterPreview::Clear();result=EmitterPreview::State();}
        else if(op=="preview.close"){EmitterPreview::CloseTestWindow();result=EmitterPreview::State();}
        else if(op=="preview.state") result=EmitterPreview::State();
        else if(op=="preview.attach"){std::string error;if(!EmitterPreview::Attach(reinterpret_cast<HWND>(static_cast<uintptr_t>(q.at("hwnd").get<uint64_t>())),error))throw std::runtime_error(error);result=EmitterPreview::State();}
        else if(op=="preview.detach"){EmitterPreview::Detach();result=EmitterPreview::State();}
        else if(op=="preview.step"){EmitterPreview::Step(q.value("seconds",1.0));result=EmitterPreview::State();}
        else if(op=="preview.camera"){if(q.contains("camera"))EmitterPreview::SetCamera(q.at("camera"));result=EmitterPreview::Camera();}
        else if(op=="characters.open"){std::string error;if(!CharacterPreview::OpenTestWindow(error))throw std::runtime_error(error);result=CharacterPreview::State();}
        else if(op=="characters.show"){result=CharacterPreview::Show(q.value("slots",Json::object()),q.value("models",Json::object()));result["state"]=CharacterPreview::State();}
        else if(op=="characters.capture") result=CharacterPreview::Capture(q.value("path",std::string()));
        else if(op=="characters.clear"){CharacterPreview::Clear();result=CharacterPreview::State();}
        else if(op=="characters.close"){CharacterPreview::CloseTestWindow();result=CharacterPreview::State();}
        else if(op=="characters.state") result=CharacterPreview::State();
        else if(op=="skins.settings") result=Editor::CharacterSkinSettings();
        else if(op=="skins.apply") result=Editor::ApplyCharacterSkins(q.value("slots",Json::object()),q.value("models",Json::object()),q.value("goggles",Json::object()));
        else if(op=="skins.remove") {Editor::RemoveCharacterSkins();result=Editor::CharacterSkinSettings();}
        else if(op=="characters.camera"){if(q.contains("camera"))CharacterPreview::SetCamera(q.at("camera"));result=CharacterPreview::Camera();}
        else if(op=="map") result={{"key",Editor::MapKey()},{"level",Editor::LevelPath()}};
        else result=MoreWorkflowRequests(op,q);
        auto text=Json({{"ok",true},{"result",result}}).dump(); if(text.size()+1>capacity) return -static_cast<int>(text.size()+1);
        memcpy(output,text.c_str(),text.size()+1); return 1;
    }
    catch(const std::exception& e)
    {
        auto text=Json({{"ok",false},{"error",e.what()}}).dump(); if(text.size()+1<=capacity) memcpy(output,text.c_str(),text.size()+1); return 0;
    }
}
