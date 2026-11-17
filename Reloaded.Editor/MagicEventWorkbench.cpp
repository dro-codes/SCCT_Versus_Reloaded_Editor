#include "pch.h"
#undef min
#undef max
#include "MagicEventWorkbench.h"
#include "WindowPlacement.h"
#include "MagicEventModel.h"
#include "WorkflowEditor.h"
#include "WorkflowTools.h"
#include <commctrl.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <commdlg.h>
#include <fstream>
#include <algorithm>
#include <memory>
#include <set>
#pragma comment(lib,"uxtheme.lib")
#pragma comment(lib,"comdlg32.lib")

namespace MagicEventWorkbench
{
using namespace Workflow;
namespace
{
constexpr COLORREF Background=RGB(240,240,240),Panel=RGB(255,255,255),Ink=RGB(0,0,0),Muted=RGB(96,96,96),Accent=RGB(0,102,204);
enum Id { EventSearch=100,Events,New,Refresh,Undo,Redo,Play,ActorSearch,Category,Actors,Focus,Attach,CreateTarget,CreateTrigger,AttachTrigger,
    Groups,AddGroup,DuplicateGroup,RemoveGroup,GroupUp,GroupDown,Timeline,DuplicateAction,RemoveAction,ActionUp,ActionDown,ZoomIn,ZoomOut,Snap,
    InspectEvent,InspectActor,PropertySearch,Properties,Value,Choices,Commit,AddItem,DeleteItem,CaptureKey,UseSelected,Status,DetachTrigger,Fit,Preview,ExportJson,ImportJson,JsonHint };
struct Binding { std::string pointer,root;Json schema; };
struct State
{
    HWND window{},timeline{},tree{};HFONT font{};HBRUSH background{},panel{};
    Json actors=Json::array(),events=Json::array(),actorRows=Json::array(),eventSnapshot,inspector,component;
    std::vector<std::unique_ptr<Binding>> bindings;Binding* selected{};
    std::string eventPath,inspectedPath,propertyPointer;
    int group=0,action=-1,left=260,right=365,split=0,scroll=0;
    double scale=75,playhead=0,dragDelay=0;int dragAction=-1;bool loading=false,snap=true,preview=false;
    uintptr_t level=0;unsigned generation=0,revision=0,selectionRevision=0;UINT dpi=96;DWORD tick=0;
};
HWND workbench=nullptr;
int Px(const State& s,int n){return MulDiv(n,s.dpi,96);}
std::string Text(HWND w){int n=GetWindowTextLengthA(w);std::string text(n+1,'\0');GetWindowTextA(w,text.data(),n+1);text.resize(n);return text;}
HWND Item(State& s,int id){return GetDlgItem(s.window,id);}
void Set(State& s,int id,const std::string& text){SetWindowTextA(Item(s,id),text.c_str());}
void StatusText(State& s,const std::string& text){Set(s,Status,text);}
void Combo(HWND w,const std::vector<std::string>& entries,int selected=0)
{
    SendMessage(w,CB_RESETCONTENT,0,0);for(const auto& entry:entries)SendMessageA(w,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(entry.c_str()));SendMessage(w,CB_SETCURSEL,selected,0);
}
HWND Add(State& s,const char* type,const char* text,int id,DWORD style=0)
{
    auto w=CreateWindowExA(0,type,text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,10,10,s.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandle(nullptr),nullptr);
    SendMessage(w,WM_SETFONT,reinterpret_cast<WPARAM>(s.font),TRUE);SetWindowTheme(w,L"Explorer",nullptr);return w;
}
void Button(State& s,const char* text,int id){Add(s,"BUTTON",text,id,BS_PUSHBUTTON);}
void Bounds(State& s,int id,int x,int y,int w,int h){MoveWindow(Item(s,id),Px(s,x),Px(s,y),Px(s,std::max(1,w)),Px(s,h),TRUE);}
void Layout(State& s)
{
    RECT r{};GetClientRect(s.window,&r);int w=MulDiv(r.right,96,s.dpi),h=MulDiv(r.bottom,96,s.dpi);
    s.left=std::clamp(s.left,215,std::max(215,w-700));s.right=std::clamp(s.right,280,std::max(280,w-s.left-420));
    int x=s.left+12,c=w-s.left-s.right-24,rx=w-s.right+6;
    Bounds(s,EventSearch,12,12,190,28);Bounds(s,Events,210,12,std::max(200,w-775),320);
    Bounds(s,New,w-553,12,105,28);Bounds(s,Refresh,w-442,12,90,28);Bounds(s,Undo,w-346,12,70,28);Bounds(s,Redo,w-270,12,70,28);Bounds(s,Play,w-194,12,180,28);
    Bounds(s,ActorSearch,12,58,s.left-24,28);Bounds(s,Category,12,94,s.left-24,300);Bounds(s,Actors,12,132,s.left-24,h-350);
    Bounds(s,Focus,12,h-208,s.left-24,28);Bounds(s,Attach,12,h-174,s.left-24,28);Bounds(s,AttachTrigger,12,h-140,s.left-24,28);
    Bounds(s,CreateTarget,12,h-106,(s.left-30)/2,28);Bounds(s,CreateTrigger,18+(s.left-30)/2,h-106,(s.left-30)/2,28);Bounds(s,DetachTrigger,12,h-72,s.left-24,28);
    Bounds(s,Groups,x,58,c-110,350);Bounds(s,InspectEvent,x+c-102,58,102,28);
    int bw=std::max(45,(c-24)/5);int ids[]={AddGroup,DuplicateGroup,RemoveGroup,GroupUp,GroupDown};for(int i=0;i<5;++i)Bounds(s,ids[i],x+i*(bw+6),96,bw,27);
    Bounds(s,Timeline,x,134,c,h-280);
    Bounds(s,DuplicateAction,x,h-134,100,28);Bounds(s,RemoveAction,x+106,h-134,94,28);Bounds(s,ActionUp,x+206,h-134,65,28);Bounds(s,ActionDown,x+277,h-134,65,28);
    Bounds(s,ZoomOut,x,h-96,36,28);Bounds(s,ZoomIn,x+42,h-96,36,28);Bounds(s,Fit,x+84,h-96,55,28);Bounds(s,Snap,x+145,h-96,85,28);Bounds(s,Preview,x+236,h-96,110,28);
    Bounds(s,InspectActor,rx,58,s.right-18,28);Bounds(s,PropertySearch,rx,94,s.right-18,28);Bounds(s,Properties,rx,132,s.right-18,h-326);
    Bounds(s,Value,rx,h-184,s.right-102,28);Bounds(s,Choices,rx,h-184,s.right-102,300);Bounds(s,Commit,w-88,h-184,76,28);
    Bounds(s,AddItem,rx,h-148,(s.right-24)/2,28);Bounds(s,DeleteItem,rx+(s.right-24)/2+6,h-148,(s.right-24)/2,28);
    Bounds(s,CaptureKey,rx,h-112,s.right-18,28);Bounds(s,UseSelected,rx,h-76,s.right-18,28);Bounds(s,Status,12,h-34,w-24,26);
    // A separate file row keeps the event picker and toolbar usable at minimum width.
    Bounds(s,ExportJson,12,50,116,28);Bounds(s,ImportJson,134,50,116,28);Bounds(s,JsonHint,264,54,w-280,24);
    for(int id:{ActorSearch,Category,Actors,Groups,InspectEvent,AddGroup,DuplicateGroup,RemoveGroup,GroupUp,GroupDown,Timeline,InspectActor,PropertySearch,Properties})
    {
        RECT child{};GetWindowRect(Item(s,id),&child);MapWindowPoints(nullptr,s.window,reinterpret_cast<POINT*>(&child),2);
        int height=child.bottom-child.top;if(id==Actors || id==Timeline || id==Properties)height-=Px(s,38);
        MoveWindow(Item(s,id),child.left,child.top+Px(s,38),child.right-child.left,std::max(1,height),TRUE);
    }
}
Json& GroupValues(State& s){return s.eventSnapshot.at("values").at("Groups");}
Json IdentityFor(State& s,const std::string& path){for(const auto& a:s.actors)if(a.at("path")==path)return a;if(!s.component.is_null() && s.component.at("path")==path)return s.component;throw std::runtime_error("Actor is no longer in this map.");}
Json SelectedActor(State& s)
{
    int i=static_cast<int>(SendMessage(Item(s,Actors),LB_GETCURSEL,0,0));
    if(i<0 || i>=static_cast<int>(s.actorRows.size()))throw std::runtime_error("Select an actor in the left pane.");return s.actorRows[i];
}
std::string Escape(const std::string& value){std::string out;for(char c:value)out+=c=='~'?"~0":c=='/'?"~1":std::string(1,c);return out;}
void FillInspector(State& s);
void RefreshAll(State& s,bool inspect=true);
void LoadEvent(State& s,const std::string& path)
{
    s.eventPath=path;s.group=0;s.action=-1;s.scroll=0;s.inspectedPath=path;RefreshAll(s);
}
void FilterActors(State& s)
{
    std::string selectedPath;int selected=static_cast<int>(SendMessage(Item(s,Actors),LB_GETCURSEL,0,0));if(selected>=0 && selected<static_cast<int>(s.actorRows.size()))selectedPath=s.actorRows[selected].at("path");
    s.actorRows=Json::array();SendMessage(Item(s,Actors),LB_RESETCONTENT,0,0);
    auto query=Fold(Text(Item(s,ActorSearch)));auto category=Text(Item(s,Category));
    std::set<std::string> tags;
    if(!s.eventSnapshot.is_null() && s.eventSnapshot.contains("values") && s.eventSnapshot["values"].contains("Groups"))for(const auto& g:GroupValues(s))for(const auto& a:g.at("EventGroup"))tags.insert(Fold(a.at("Event").get<std::string>()));
    std::string eventTag=s.eventSnapshot.is_null()?"":Fold(s.eventSnapshot.at("values").value("Tag",std::string{}));
    for(const auto& actor:s.actors)
    {
        if(!actor.value("authorable",true))continue;
        auto path=actor.at("path").get<std::string>(),tag=actor.at("tag").get<std::string>(),type=actor.at("class").get<std::string>();
        if(Fold(path+" "+tag+" "+type).find(query)==std::string::npos)continue;
        if(category=="Selected in map" && !actor.at("selected").get<bool>())continue;
        bool incoming=!eventTag.empty() && eventTag!="none" && Fold(actor.at("event"))==eventTag;
        if(category=="In this event" && !incoming && !tags.count(Fold(tag)))continue;
        if(category!="All actors" && category!="In this event" && category!="Selected in map" && Magic::Category(type)!=category)continue;
        auto label=(incoming?"[IN] ":tags.count(Fold(tag))?"[OUT] ":"")+actor.at("name").get<std::string>()+"  |  "+tag;
        int index=static_cast<int>(SendMessageA(Item(s,Actors),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str())));s.actorRows.push_back(actor);if(path==selectedPath)SendMessage(Item(s,Actors),LB_SETCURSEL,index,0);
    }
}
// Only the selection changed: the actors take the new flags and nothing is read from the map again. The event and the
// inspector stay as they are; the actor list is filtered again only while it shows the selection.
void SyncSelection(State& s)
{
    s.selectionRevision=Editor::Revision();
    std::set<std::string> selected;for(const auto& path:Editor::SelectedActorPaths())selected.insert(Fold(path));
    for(auto& actor:s.actors)actor["selected"]=selected.count(Fold(actor.at("path").get<std::string>()))!=0;
    if(Text(Item(s,Category))=="Selected in map")FilterActors(s);
}
void RefreshAll(State& s,bool inspect)
{
    s.loading=true;
    struct Done{bool& b;~Done(){b=false;}} done{s.loading};
    auto level=Editor::LevelIdentity();auto generation=Editor::MapGeneration();
    if(s.level && (s.level!=level || s.generation!=generation)){s.eventPath.clear();s.inspectedPath.clear();s.action=-1;s.group=0;}
    s.level=level;s.generation=generation;s.revision=Editor::ContentRevision();s.selectionRevision=Editor::Revision();s.actors=Editor::Actors();s.events=Json::array();
    auto query=Fold(Text(Item(s,EventSearch)));std::vector<std::string> names;int selected=-1;bool exists=false;
    for(const auto& actor:s.actors)
    {
        auto path=actor.at("path").get<std::string>();if(path==s.eventPath)exists=true;
        if(!actor.value("magicEvent",false))continue;
        if(path!=s.eventPath && Fold(path+" "+actor.at("tag").get<std::string>()).find(query)==std::string::npos)continue;
        if(path==s.eventPath)selected=static_cast<int>(names.size());names.push_back(actor.at("name").get<std::string>()+"  |  "+actor.at("tag").get<std::string>());s.events.push_back(actor);
    }
    Combo(Item(s,Events),names,selected);
    if(!exists){s.eventPath.clear();s.eventSnapshot=Json{};s.inspector=Json{};s.inspectedPath.clear();}
    else s.eventSnapshot=Editor::InspectActor(IdentityFor(s,s.eventPath));
    EnableWindow(Item(s,ExportJson),!s.eventSnapshot.is_null());EnableWindow(Item(s,ImportJson),!s.eventSnapshot.is_null());
    names.clear();
    if(!s.eventSnapshot.is_null() && s.eventSnapshot.at("values").contains("Groups"))
    {
        int i=0;for(const auto& group:GroupValues(s)){names.push_back("Group "+std::to_string(++i)+"  /  "+std::to_string(group.at("EventGroup").size())+" actions");}
        s.group=std::clamp(s.group,0,std::max(0,static_cast<int>(names.size())-1));
        if(!names.empty() && s.action>=static_cast<int>(GroupValues(s)[s.group]["EventGroup"].size()))s.action=-1;
    }
    Combo(Item(s,Groups),names,s.group);FilterActors(s);
    StatusText(s,s.eventSnapshot.is_null()?"Create a new event or choose an existing SMagicEvent above.":"Editing "+s.eventSnapshot.at("values").value("Tag",s.eventPath)+". Changes save to the map; each completed edit supports native undo.");
    if(inspect)FillInspector(s);InvalidateRect(s.timeline,nullptr,FALSE);
}
HTREEITEM TreeRow(State& s,HTREEITEM parent,const std::string& label,Binding* binding=nullptr)
{
    TVINSERTSTRUCTA item{};item.hParent=parent;item.hInsertAfter=TVI_LAST;item.item.mask=TVIF_TEXT|TVIF_PARAM;item.item.pszText=const_cast<char*>(label.c_str());item.item.lParam=reinterpret_cast<LPARAM>(binding);
    return reinterpret_cast<HTREEITEM>(SendMessageA(s.tree,TVM_INSERTITEMA,0,reinterpret_cast<LPARAM>(&item)));
}
void PropertyRows(State& s,HTREEITEM parent,const std::string& name,const Json& schema,const Json& value,const std::string& pointer,const std::string& root,int depth=0)
{
    if(s.bindings.size()>12000)return;
    auto b=std::make_unique<Binding>();b->pointer=pointer;b->root=root;b->schema=schema;auto raw=b.get();s.bindings.push_back(std::move(b));
    auto label=name+(value.is_string()?"   "+value.get<std::string>():value.is_array()?"   ["+std::to_string(value.size())+"]":"");
    auto node=TreeRow(s,parent,label,raw);
    if(value.is_object())for(auto it=value.begin();it!=value.end();++it){if(root=="Groups" && it.key()=="SequenceIndex")continue;PropertyRows(s,node,it.key(),schema.at("fields").at(it.key()),it.value(),pointer+"/"+Escape(it.key()),root,depth+1);}
    if(value.is_array())for(size_t i=0;i<value.size();++i)PropertyRows(s,node,"["+std::to_string(i)+"]",schema.at("inner"),value[i],pointer+"/"+std::to_string(i),root,depth+1);
    if(depth<1)TreeView_Expand(s.tree,node,TVE_EXPAND);
    if(pointer==s.propertyPointer && !s.selected)TreeView_SelectItem(s.tree,node);
}
void FillInspector(State& s)
{
    s.selected=nullptr;TreeView_DeleteAllItems(s.tree);s.bindings.clear();Set(s,Value,"");ShowWindow(Item(s,Choices),SW_HIDE);ShowWindow(Item(s,Value),SW_SHOW);
    if(s.inspectedPath.empty())return;
    try{s.inspector=Editor::InspectActor(IdentityFor(s,s.inspectedPath));}catch(...){s.inspectedPath=s.eventPath;s.inspector=s.eventSnapshot;}
    if(s.inspector.is_null())return;
    auto& values=s.inspector.at("values");auto& schema=s.inspector.at("schema");auto query=Fold(Text(Item(s,PropertySearch)));
    auto title=s.inspectedPath.substr(s.inspectedPath.find_last_of('.')+1);Set(s,InspectActor,title+" properties");
    if(s.inspectedPath==s.eventPath && query.empty() && values.contains("Groups") && !values.at("Groups").empty())
    {
        auto group=TreeRow(s,TVI_ROOT,"GROUP "+std::to_string(s.group+1));auto& gs=schema.at("Groups").at("inner");auto& gv=values.at("Groups")[s.group];
        for(const auto* field:{"Repeat","Sequence"})if(gv.contains(field))PropertyRows(s,group,field,gs.at("fields").at(field),gv.at(field),"/Groups/"+std::to_string(s.group)+"/"+field,"Groups");
        TreeView_Expand(s.tree,group,TVE_EXPAND);
        if(s.action>=0 && s.action<static_cast<int>(gv.at("EventGroup").size()))
        {
            auto action=TreeRow(s,TVI_ROOT,"ACTION "+std::to_string(s.action+1));auto& av=gv.at("EventGroup")[s.action];auto& as=gs.at("fields").at("EventGroup").at("inner");
            PropertyRows(s,action,"Action settings",as,av,"/Groups/"+std::to_string(s.group)+"/EventGroup/"+std::to_string(s.action),"Groups");TreeView_Expand(s.tree,action,TVE_EXPAND);
        }
    }
    auto setup=TreeRow(s,TVI_ROOT,"ACTOR SETUP");std::set<std::string> shown;
    if(query.empty())
    {
        for(const auto* key:{"Location","Rotation","Tag","Event","InitialState","StaticMesh","DrawScale","DrawScale3D"})if(values.contains(key)){PropertyRows(s,setup,key,schema.at(key),values.at(key),"/"+std::string(key),key);shown.insert(key);}
        auto className=s.inspector.at("actor").at("class").get<std::string>();auto family=Magic::Category(className);auto behaviour=TreeRow(s,TVI_ROOT,family=="Other"?"BEHAVIOUR":family+" / BEHAVIOUR");
        for(auto it=values.begin();it!=values.end();++it)
        {
            if(shown.count(it.key()) || it.key()=="Groups")continue;auto category=schema.at(it.key()).value("category",std::string{});auto folded=Fold(category);
            bool primary=category==className.substr(className.find_last_of('.')+1) || (family=="Movers" && (folded.find("mover")!=std::string::npos || it.key()=="KeyPos" || it.key()=="KeyRot" || it.key()=="NumKeys")) || (family=="Sounds" && folded=="sound") || (family=="Emitters" && (folded=="emitter" || folded=="global")) || (family=="Triggers" && (folded=="trigger" || folded=="collision")) || (family=="Volumes" && (folded.find("damage")!=std::string::npos || folded.find("volume")!=std::string::npos));
            if(primary){PropertyRows(s,behaviour,it.key(),schema.at(it.key()),it.value(),"/"+Escape(it.key()),it.key());shown.insert(it.key());}
        }
        TreeView_Expand(s.tree,setup,TVE_EXPAND);TreeView_Expand(s.tree,behaviour,TVE_EXPAND);
    }
    auto advanced=TreeRow(s,TVI_ROOT,query.empty()?"ADVANCED / all other editable properties":"SEARCH RESULTS");
    std::map<std::string,HTREEITEM> categories;
    for(auto it=values.begin();it!=values.end();++it)
    {
        auto& descriptor=schema.at(it.key());auto category=descriptor.value("category",std::string("Advanced"));
        if(shown.count(it.key()))continue;
        if(Fold(it.key()+" "+category).find(query)==std::string::npos)continue;
        if(!categories.count(category))categories[category]=TreeRow(s,advanced,category.empty()?"General":category);
        PropertyRows(s,categories[category],it.key(),descriptor,it.value(),"/"+Escape(it.key()),it.key());
    }
    for(const auto& c:categories)TreeView_Expand(s.tree,c.second,TVE_EXPAND);
    if(!query.empty())TreeView_Expand(s.tree,advanced,TVE_EXPAND);
    for(auto it=s.inspector.at("unavailable").begin();it!=s.inspector.at("unavailable").end();++it)TreeRow(s,TVI_ROOT,it.key()+" (unavailable: "+it.value().get<std::string>()+")");
    if(s.selected)TreeView_EnsureVisible(s.tree,TreeView_GetSelection(s.tree));else TreeView_SelectSetFirstVisible(s.tree,TreeView_GetRoot(s.tree));
}
void SelectProperty(State& s,Binding* b)
{
    s.selected=b;if(!b)return;s.propertyPointer=b->pointer;auto value=s.inspector.at("values").at(Json::json_pointer(b->pointer));
    bool scalar=value.is_string();auto choices=b->schema.value("choices",Json::array());bool dropdown=scalar && !choices.empty();
    ShowWindow(Item(s,Value),dropdown?SW_HIDE:SW_SHOW);ShowWindow(Item(s,Choices),dropdown?SW_SHOW:SW_HIDE);EnableWindow(Item(s,Value),scalar);
    if(dropdown){std::vector<std::string> names=choices.get<std::vector<std::string>>();int i=0;for(size_t j=0;j<names.size();++j)if(Fold(names[j])==Fold(value.get<std::string>()))i=static_cast<int>(j);Combo(Item(s,Choices),names,i);}else Set(s,Value,scalar?value.get<std::string>():"Expand to edit individual fields");
    std::string help="Enter or Apply commits one undoable edit.";
    if(b->pointer.ends_with("/Repeat"))help="-1 = disabled; 0 = unlimited; positive = number of cycles (not a timed loop).";
    if(b->pointer.ends_with("/Sequence"))help="True: advance one action per matching activation. False: schedule all applicable actions together.";
    if(b->pointer.ends_with("/Delay"))help="Seconds after the activating trigger/untrigger, not after the previous action.";
    if(b->pointer.ends_with("/Type"))help="Trigger / Untrigger, or alternate starting with the named first operation. Delayed actions operate on queued copies.";
    if(b->pointer.ends_with("/ValidOn"))help="Accept Trigger, Untrigger, or both (either combined value accepts both).";
    if(b->pointer.ends_with("/Event"))
    {
        help="Targets: ";int count=0;for(const auto& actor:s.actors)if(Fold(actor.at("tag").get<std::string>())==Fold(value.get<std::string>()) && Fold(value.get<std::string>())!="none"){if(count++)help+=", ";help+=actor.at("name").get<std::string>();}if(!count)help+="unresolved / None";
    }
    StatusText(s,b->pointer+"  |  "+help);
}
void CommitValue(State& s)
{
    if(!s.selected)return;auto binding=*s.selected;auto next=s.inspector.at("values");auto& value=next.at(Json::json_pointer(binding.pointer));if(!value.is_string())return;
    auto text=Text(Item(s,IsWindowVisible(Item(s,Choices))?Choices:Value));if(value==text)return;
    Magic::Validate(binding.schema,text);value=text;
    if(binding.pointer=="/Tag")
    {
        auto preview=Editor::PreviewTagRename(s.inspector.at("actor"),text);std::string description="Rename Tag and "+std::to_string(preview.at("changes").size())+" assignments?\n\n";
        for(const auto& change:preview.at("changes"))description+=change.at("actor").at("path").get<std::string>()+" : "+change.at("property").get<std::string>()+"\n";
        if(MessageBoxA(s.window,description.c_str(),"Rename Tag and dependent connections",MB_OKCANCEL|MB_ICONQUESTION)==IDOK)Editor::RenameTag(preview);RefreshAll(s);return;
    }
    Editor::EditActor(s.inspector,{{binding.root,next.at(binding.root)}});RefreshAll(s);StatusText(s,"Saved to map. Undo reverses this edit.");
}
void ChangeGroups(State& s,const Json& groups)
{
    if(s.eventSnapshot.is_null())throw std::runtime_error("Create or open an event first.");Editor::EditActor(s.eventSnapshot,{{"Groups",groups}});s.inspectedPath=s.eventPath;RefreshAll(s);
}
COLORREF TrackColour(const std::string& category)
{
    if(category=="Movers")return RGB(0,102,204);if(category=="Emitters")return RGB(130,65,160);if(category=="Sounds")return RGB(0,116,84);if(category=="Volumes")return RGB(168,70,32);return RGB(76,85,150);
}
std::string ActionLabel(const std::string& type)
{
    if(type=="EVT_Trigger")return "Trigger";if(type=="EVT_Untrigger")return "Untrigger";if(type=="EVT_TriggerUntrigger")return "Alternate T/U";if(type=="EVT_UntriggerTrigger")return "Alternate U/T";return type;
}
void DrawTimeline(State& s,HDC target)
{
    RECT r{};GetClientRect(s.timeline,&r);if(r.right<=0 || r.bottom<=0)return;
    auto dc=CreateCompatibleDC(target);auto bitmap=CreateCompatibleBitmap(target,r.right,r.bottom);auto previous=SelectObject(dc,bitmap);FillRect(dc,&r,s.panel);auto font=SelectObject(dc,s.font);SetBkMode(dc,TRANSPARENT);
    auto label=[&](const std::string& text,int x,int y,int width,COLORREF colour){RECT box{x,y,x+width,y+Px(s,25)};SetTextColor(dc,colour);DrawTextA(dc,text.c_str(),-1,&box,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);};
    double scale=s.scale*s.dpi/96.0;int origin=Px(s,155),row=Px(s,50),top=Px(s,65);
    bool sequence=!s.eventSnapshot.is_null() && s.eventSnapshot.at("values").contains("Groups") && !GroupValues(s).empty() && Fold(GroupValues(s)[s.group].at("Sequence"))=="true";
    label(sequence?"ONE ROW PER ACTIVATION / delay in seconds":"ACTIONS AFTER ACTIVATION / seconds",Px(s,12),Px(s,9),r.right-Px(s,24),Muted);
    double step=std::pow(10.0,std::floor(std::log10(65.0/scale)));if(step*scale<32)step*=5;else if(step*scale<60)step*=2;
    auto pen=CreatePen(PS_SOLID,1,RGB(212,212,212));auto old=SelectObject(dc,pen);
    for(double t=0;t<=86400 && origin+t*scale<r.right;t+=step){int x=origin+static_cast<int>(t*scale);MoveToEx(dc,x,Px(s,43),nullptr);LineTo(dc,x,r.bottom);label(Magic::Seconds(t),x+3,Px(s,32),Px(s,70),Muted);}
    SelectObject(dc,old);DeleteObject(pen);
    if(s.eventSnapshot.is_null() || !s.eventSnapshot.at("values").contains("Groups") || GroupValues(s).empty())label("Create an event, then add a group and choose actors.",Px(s,15),top,r.right-Px(s,30),Ink);
    else
    {
        auto& actions=GroupValues(s)[s.group].at("EventGroup");
        for(int i=0;i<static_cast<int>(actions.size());++i)
        {
            int y=top+(i-s.scroll)*row;if(y<top || y>r.bottom)continue;
            auto tag=actions[i].at("Event").get<std::string>();std::string category="Other";int count=0;
            for(const auto& actor:s.actors)if(Fold(actor.at("tag").get<std::string>())==Fold(tag) && Fold(tag)!="none"){++count;category=Magic::Category(actor.at("class"));}
            auto colour=count?TrackColour(category):RGB(156,83,0);
            label(std::to_string(i+1)+"  "+tag,Px(s,9),y,Px(s,140),i==s.action?Ink:Muted);
            double delay=0;bool valid=true;try{delay=i==s.dragAction?s.dragDelay:Magic::Number(actions[i].at("Delay"));valid=delay>=0 && delay<=86400;}catch(...){valid=false;}
            int x=origin+static_cast<int>(std::clamp(delay*scale,0.0,static_cast<double>(r.right)+20));
            RECT card{x,y-2,std::min<int>(r.right-Px(s,5),x+Px(s,155)),y+Px(s,42)};
            if(card.right>card.left){auto brush=CreateSolidBrush(i==s.action?RGB(219,235,255):RGB(248,248,248));FillRect(dc,&card,brush);DeleteObject(brush);RECT stripe=card;stripe.right=stripe.left+3;brush=CreateSolidBrush(colour);FillRect(dc,&stripe,brush);DeleteObject(brush);label((valid?Magic::Seconds(delay)+"s":"Check delay")+"  "+ActionLabel(actions[i].at("Type")),x+8,y,card.right-x-10,colour);auto on=actions[i].at("ValidOn").get<std::string>();label("On "+(on=="EVT_Trigger"?std::string("trigger"):on=="EVT_Untrigger"?std::string("untrigger"):std::string("either"))+" / "+std::to_string(count)+" targets",x+8,y+Px(s,19),card.right-x-10,Muted);}
        }
        if(actions.empty())label("Choose an actor on the left, then Add action.",Px(s,12),top,r.right-Px(s,24),Muted);
    }
    int cursor=origin+static_cast<int>(s.playhead*scale);pen=CreatePen(PS_SOLID,2,Accent);old=SelectObject(dc,pen);MoveToEx(dc,cursor,Px(s,43),nullptr);LineTo(dc,cursor,r.bottom);SelectObject(dc,old);DeleteObject(pen);
    SelectObject(dc,font);BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);
}
LRESULT CALLBACK TimelineProc(HWND w,UINT message,WPARAM wp,LPARAM lp)
{
    auto s=reinterpret_cast<State*>(GetWindowLongPtr(w,GWLP_USERDATA));if(message==WM_NCCREATE){s=static_cast<State*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);SetWindowLongPtr(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}if(!s)return DefWindowProcA(w,message,wp,lp);
    try
    {
        if(message==WM_PAINT){PAINTSTRUCT p{};auto dc=BeginPaint(w,&p);try{DrawTimeline(*s,dc);}catch(...){EndPaint(w,&p);throw;}EndPaint(w,&p);return 0;}
        if(message==WM_PRINTCLIENT){DrawTimeline(*s,reinterpret_cast<HDC>(wp));return 0;}
        if(message==WM_ERASEBKGND)return 1;
        if(message==WM_LBUTTONDOWN)
        {
            SetFocus(w);int y=GET_Y_LPARAM(lp);double time=std::max(0.0,(GET_X_LPARAM(lp)-Px(*s,155))/(s->scale*s->dpi/96.0));
            if(y<Px(*s,65)){s->playhead=time;InvalidateRect(w,nullptr,FALSE);return 0;}
            if(s->eventSnapshot.is_null() || GroupValues(*s).empty())return 0;
            int index=(y-Px(*s,65))/Px(*s,50)+s->scroll;if(index>=static_cast<int>(GroupValues(*s)[s->group]["EventGroup"].size()))return 0;
            s->action=index;s->inspectedPath=s->eventPath;FillInspector(*s);s->dragDelay=Magic::Number(GroupValues(*s)[s->group]["EventGroup"][index]["Delay"]);
            if(GET_X_LPARAM(lp)>=Px(*s,155)){s->dragAction=index;SetCapture(w);}InvalidateRect(w,nullptr,FALSE);return 0;
        }
        if(message==WM_MOUSEMOVE && s->dragAction>=0)
        {
            double time=std::clamp((GET_X_LPARAM(lp)-Px(*s,155))/(s->scale*s->dpi/96.0),0.0,86400.0);s->dragDelay=s->snap?std::round(time*10)/10:time;InvalidateRect(w,nullptr,FALSE);return 0;
        }
        if(message==WM_LBUTTONUP && s->dragAction>=0)
        {
            int index=s->dragAction;s->dragAction=-1;ReleaseCapture();auto groups=GroupValues(*s);auto before=Magic::Number(groups[s->group]["EventGroup"][index]["Delay"]);
            if(std::abs(before-s->dragDelay)>0.00001){groups[s->group]["EventGroup"][index]["Delay"]=Magic::Seconds(s->dragDelay);ChangeGroups(*s,groups);}return 0;
        }
        if(message==WM_CAPTURECHANGED){s->dragAction=-1;InvalidateRect(w,nullptr,FALSE);return 0;}
        if(message==WM_MOUSEWHEEL){if(GetKeyState(VK_CONTROL)&0x8000)s->scale=std::clamp(s->scale*(GET_WHEEL_DELTA_WPARAM(wp)>0?1.2:1/1.2),0.002,600.0);else s->scroll=std::max(0,s->scroll+(GET_WHEEL_DELTA_WPARAM(wp)>0?-2:2));InvalidateRect(w,nullptr,FALSE);return 0;}
        if(message==WM_KEYDOWN){if(wp==VK_DELETE)SendMessage(s->window,WM_COMMAND,RemoveAction,0);if(wp==VK_ESCAPE){s->dragAction=-1;ReleaseCapture();}if(wp==VK_UP || wp==VK_DOWN){if(!s->eventSnapshot.is_null() && !GroupValues(*s).empty()){s->action=std::clamp(s->action+(wp==VK_UP?-1:1),0,std::max(0,static_cast<int>(GroupValues(*s)[s->group]["EventGroup"].size())-1));s->inspectedPath=s->eventPath;FillInspector(*s);InvalidateRect(w,nullptr,FALSE);}}return 0;}
        if(message==WM_GETDLGCODE)return DLGC_WANTARROWS;
    }
    catch(const std::exception& e){s->dragAction=-1;ReleaseCapture();StatusText(*s,e.what());}
    return DefWindowProcA(w,message,wp,lp);
}

// Searchable class picker shares the same native controls and colour palette.
struct Picker{State ui;Json classes,rows;bool trigger=false,done=false;std::string chosen,geometry="point";};
void FilterClasses(Picker& p)
{
    auto query=Fold(Text(Item(p.ui,ActorSearch)));auto category=Text(Item(p.ui,Category));p.rows=Json::array();SendMessage(Item(p.ui,Actors),LB_RESETCONTENT,0,0);
    for(const auto& c:p.classes){auto type=c.at("class").get<std::string>();if(Fold(type).find(query)==std::string::npos || (category!="All classes" && c.at("category")!=category))continue;p.rows.push_back(c);SendMessageA(Item(p.ui,Actors),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(type.c_str()));}SendMessage(Item(p.ui,Actors),LB_SETCURSEL,0,0);
}
LRESULT CALLBACK PickerProc(HWND w,UINT message,WPARAM wp,LPARAM lp)
{
    auto p=reinterpret_cast<Picker*>(GetWindowLongPtr(w,GWLP_USERDATA));if(message==WM_NCCREATE){p=static_cast<Picker*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);SetWindowLongPtr(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));p->ui.window=w;}if(!p)return DefWindowProcA(w,message,wp,lp);
    if(message==WM_CREATE)
    {
        auto& s=p->ui;Add(s,"EDIT","",ActorSearch,ES_AUTOHSCROLL);SendMessageA(Item(s,ActorSearch),EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Search actor classes"));
        Add(s,"COMBOBOX","",Category,CBS_DROPDOWNLIST|WS_VSCROLL);Combo(Item(s,Category),{"All classes","Triggers","Movers","Emitters","Sounds","Volumes","Events","Other"},p->trigger?1:0);
        Add(s,"LISTBOX","",Actors,LBS_NOTIFY|WS_VSCROLL);Add(s,"COMBOBOX","",Choices,CBS_DROPDOWNLIST);Combo(Item(s,Choices),{"Point actor","Current builder brush","Box volume (256 units)"});
        Add(s,"BUTTON","Create and connect",Commit,BS_DEFPUSHBUTTON);Add(s,"BUTTON","Cancel",IDCANCEL);Add(s,"STATIC","Creates a map actor at the builder position.",Status);
        Bounds(s,ActorSearch,14,14,310,28);Bounds(s,Category,334,14,190,300);Bounds(s,Actors,14,56,510,310);Bounds(s,Choices,14,380,300,200);Bounds(s,Status,14,420,510,35);Bounds(s,Commit,245,465,175,30);Bounds(s,IDCANCEL,430,465,94,30);FilterClasses(*p);return 0;
    }
    if(message==WM_COMMAND)
    {
        int id=LOWORD(wp),notification=HIWORD(wp);
        if((id==ActorSearch && notification==EN_CHANGE) || (id==Category && notification==CBN_SELCHANGE)){FilterClasses(*p);return 0;}
        if(id==Commit || (id==Actors && notification==LBN_DBLCLK)){int i=static_cast<int>(SendMessage(Item(p->ui,Actors),LB_GETCURSEL,0,0));if(i>=0 && i<static_cast<int>(p->rows.size())){p->chosen=p->rows[i].at("class");int g=static_cast<int>(SendMessage(Item(p->ui,Choices),CB_GETCURSEL,0,0));p->geometry=g==1?"builder":g==2?"box":"point";p->done=true;}return 0;}
        if(id==IDCANCEL){p->done=true;return 0;}
    }
    if(message==WM_CLOSE){p->done=true;return 0;}
    if(message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX){SetTextColor(reinterpret_cast<HDC>(wp),Ink);SetBkColor(reinterpret_cast<HDC>(wp),message==WM_CTLCOLORSTATIC?Background:Panel);return reinterpret_cast<LRESULT>(message==WM_CTLCOLORSTATIC?p->ui.background:p->ui.panel);}
    if(message==WM_ERASEBKGND){RECT r{};GetClientRect(w,&r);FillRect(reinterpret_cast<HDC>(wp),&r,p->ui.background);return 1;}
    return DefWindowProcA(w,message,wp,lp);
}
void CreateActor(State& s,bool trigger)
{
    if(s.eventSnapshot.is_null())throw std::runtime_error("Create or open an event first.");
    Picker p;p.ui.font=s.font;p.ui.background=s.background;p.ui.panel=s.panel;p.ui.dpi=s.dpi;p.trigger=trigger;p.classes=Editor::EventClasses();
    WNDCLASSA wc{};wc.lpfnWndProc=PickerProc;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="ReloadedMagicClassPicker";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassA(&wc);
    HWND dialog=CreateWindowExA(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,wc.lpszClassName,trigger?"Create event trigger":"Create event actor",WS_CAPTION|WS_SYSMENU|WS_POPUP,CW_USEDEFAULT,CW_USEDEFAULT,Px(s,556),Px(s,548),s.window,nullptr,wc.hInstance,&p);
    if(!dialog)throw std::runtime_error("Could not open actor picker.");EnableWindow(s.window,FALSE);ShowWindow(dialog,SW_SHOW);MSG msg{};while(!p.done && GetMessage(&msg,nullptr,0,0)>0)if(!IsDialogMessage(dialog,&msg)){TranslateMessage(&msg);DispatchMessage(&msg);}EnableWindow(s.window,TRUE);DestroyWindow(dialog);SetActiveWindow(s.window);
    if(p.chosen.empty())return;auto actor=Editor::CreateEventActor(p.chosen,s.eventSnapshot,trigger,p.geometry,s.group);s.inspectedPath=actor.at("path");RefreshAll(s);
}
std::string ChooseReference(State& s,const Json& entries,const char* title)
{
    Picker p;p.ui.font=s.font;p.ui.background=s.background;p.ui.panel=s.panel;p.ui.dpi=s.dpi;p.classes=Json::array();
    for(const auto& entry:entries)p.classes.push_back({{"class",entry.at("path")},{"category","Other"}});
    WNDCLASSA wc{};wc.lpfnWndProc=PickerProc;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="ReloadedMagicClassPicker";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassA(&wc);
    auto dialog=CreateWindowExA(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,wc.lpszClassName,title,WS_CAPTION|WS_SYSMENU|WS_POPUP,CW_USEDEFAULT,CW_USEDEFAULT,Px(s,556),Px(s,548),s.window,nullptr,wc.hInstance,&p);
    if(!dialog)throw std::runtime_error("Could not open reference picker.");ShowWindow(Item(p.ui,Choices),SW_HIDE);ShowWindow(Item(p.ui,Category),SW_HIDE);Set(p.ui,Commit,"Use selected");Set(p.ui,Status,"Search currently loaded objects. Load asset packages in their browser first.");
    EnableWindow(s.window,FALSE);ShowWindow(dialog,SW_SHOW);MSG msg{};while(!p.done && GetMessage(&msg,nullptr,0,0)>0)if(!IsDialogMessage(dialog,&msg)){TranslateMessage(&msg);DispatchMessage(&msg);}EnableWindow(s.window,TRUE);DestroyWindow(dialog);SetActiveWindow(s.window);return p.chosen;
}
void ReferenceMenu(State& s)
{
    HMENU menu=CreatePopupMenu();AppendMenuA(menu,MF_STRING,1,"Choose loaded asset / actor...");AppendMenuA(menu,MF_STRING,2,"Use selected map actor / Tag");AppendMenuA(menu,MF_STRING,3,"Edit referenced emitter component");AppendMenuA(menu,MF_STRING,4,"Add particle emitter component...");
    RECT r{};GetWindowRect(Item(s,UseSelected),&r);int option=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,r.left,r.bottom,s.window,nullptr);DestroyMenu(menu);
    if(!option)return;
    if(option==4)
    {
        auto type=ChooseReference(s,Editor::EventAssets("Engine.ParticleEmitter",true),"Particle emitter component class");if(type.empty())return;
        s.component=Editor::CreateEventComponent(s.inspector,type);s.inspectedPath=s.component.at("path");RefreshAll(s);return;
    }
    if(!s.selected)throw std::runtime_error("Select a reference property first.");auto binding=*s.selected;auto kind=binding.schema.at("kind").get<std::string>();
    if(option==3)
    {
        auto text=s.inspector.at("values").at(Json::json_pointer(binding.pointer)).get<std::string>();auto begin=text.find('\''),end=text.rfind('\'');if(begin==std::string::npos || begin==end)throw std::runtime_error("Select a populated component reference.");
        s.component={{"path",text.substr(begin+1,end-begin-1)},{"class",text.substr(0,begin)}};Editor::InspectActor(s.component);s.inspectedPath=s.component.at("path");FillInspector(s);return;
    }
    std::string value;
    if(option==2)
    {
        auto actors=Editor::SelectedIdentities();if(actors.size()!=1)throw std::runtime_error("Select exactly one actor in the level.");
        if(kind=="NameProperty")value=actors[0].at("tag");else if(kind=="ObjectProperty" && Editor::Compatible(actors[0].at("path"),binding.schema.at("type")))value=actors[0].at("class").get<std::string>()+"'"+actors[0].at("path").get<std::string>()+"'";else throw std::runtime_error("Select a compatible actor-reference or event-name property.");
    }
    else
    {
        if(kind!="ObjectProperty" && kind!="ClassProperty")throw std::runtime_error("Select an asset, actor or class-reference property.");
        auto objects=Editor::EventAssets(binding.schema.at("type"),kind=="ClassProperty");auto path=ChooseReference(s,objects,"Choose reference");if(path.empty())return;
        for(const auto& o:objects)if(o.at("path")==path)value=o.at("class").get<std::string>()+"'"+path+"'";
    }
    Set(s,Value,value);CommitValue(s);
}
void CommandAction(State& s,int id)
{
    if(id==ExportJson || id==ImportJson)
    {
        if(s.eventSnapshot.is_null())throw std::runtime_error("Create or open an event first.");
        const auto snapshot=Editor::InspectActor(s.eventSnapshot.at("actor"));
        wchar_t path[32768]=L"SMagicEvent.json";
        OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=s.window;
        dialog.lpstrFilter=L"SMagicEvent JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";dialog.lpstrFile=path;dialog.nMaxFile=static_cast<DWORD>(std::size(path));
        dialog.lpstrDefExt=L"json";dialog.lpstrTitle=id==ExportJson?L"Export SMagicEvent as JSON":L"Import JSON into the open SMagicEvent";
        dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(id==ExportJson?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
        const bool chosen=(id==ExportJson?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog))!=FALSE;
        if(!chosen){if(CommDlgExtendedError())throw std::runtime_error("The JSON file dialog could not be opened.");return;}
        if(id==ExportJson)
        {
            WriteDocument(std::filesystem::path(path),Editor::ExportEventJson(snapshot.at("actor")));
            StatusText(s,"JSON exported. Share the file to get help editing the event.");
        }
        else
        {
            const std::filesystem::path file(path);
            if(std::filesystem::file_size(file)>8*1024*1024)throw std::runtime_error("Event JSON exceeds 8 MiB.");
            std::ifstream input(file,std::ios::binary);if(!input)throw std::runtime_error("Could not read the event JSON file.");
            std::string text((std::istreambuf_iterator<char>(input)),{});
            auto document=Json::parse(text,[](int depth,Json::parse_event_t,Json&){if(depth>64)throw std::runtime_error("Event JSON is nested too deeply.");return true;});
            Editor::ImportEventJson(snapshot,document);s.eventPath=snapshot.at("actor").at("path");s.inspectedPath=s.eventPath;s.group=0;s.action=-1;RefreshAll(s);
            StatusText(s,"JSON imported into the open event. Undo restores the previous settings; save the map to keep changes.");
        }
        return;
    }
    if(id==New){auto actor=Editor::CreateEventActor("SBase.SMagicEvent");SendMessage(Item(s,Category),CB_SETCURSEL,1,0);LoadEvent(s,actor.at("path"));return;}
    if(id==Refresh){RefreshAll(s);StatusText(s,"Refreshed from the current map.");return;}
    if(id==Undo || id==Redo){Editor::Exec(id==Undo?"TRANSACTION UNDO":"TRANSACTION REDO");RefreshAll(s);return;}
    if(id==Play){Editor::PlayLevel();return;}
    if(id==Commit){CommitValue(s);return;}
    if(id==InspectEvent){s.inspectedPath=s.eventPath;FillInspector(s);return;}
    if(id==InspectActor){if(!s.inspectedPath.empty())Editor::Select(Json::array({IdentityFor(s,s.inspectedPath)}),true);return;}
    if(id==Focus){Editor::Select(Json::array({SelectedActor(s)}),true);return;}
    if(id==CreateTarget || id==CreateTrigger){CreateActor(s,id==CreateTrigger);return;}
    if(id==Attach || id==AttachTrigger)
    {
        auto target=SelectedActor(s);auto snapshot=Editor::InspectActor(target);
        if(id==AttachTrigger && Fold(target.at("event"))!="none" && (!s.eventSnapshot.is_null() && target.at("event")!=s.eventSnapshot.at("values").at("Tag")))
        {auto text="Replace "+target.at("name").get<std::string>()+"'s current Event '"+target.at("event").get<std::string>()+"'? This disconnects its existing targets.";if(MessageBoxA(s.window,text.c_str(),"Rewire trigger",MB_OKCANCEL|MB_ICONQUESTION)!=IDOK)return;}
        Editor::LinkEventActor(s.eventSnapshot,snapshot,id==AttachTrigger,s.group);s.inspectedPath=id==AttachTrigger?target.at("path").get<std::string>():s.eventPath;RefreshAll(s);return;
    }
    if(id==DetachTrigger)
    {
        auto target=Editor::InspectActor(SelectedActor(s));if(s.eventSnapshot.is_null() || target.at("values").at("Event")!=s.eventSnapshot.at("values").at("Tag"))throw std::runtime_error("Select a trigger connected to this event.");
        Editor::EditActor(target,{{"Event","None"}});RefreshAll(s);return;
    }
    if(id==ZoomIn || id==ZoomOut){s.scale=std::clamp(s.scale*(id==ZoomIn?1.3:1/1.3),0.002,600.0);InvalidateRect(s.timeline,nullptr,FALSE);return;}
    if(id==Fit){double largest=1;if(!s.eventSnapshot.is_null() && !GroupValues(s).empty())for(const auto& action:GroupValues(s)[s.group]["EventGroup"])largest=std::max(largest,Magic::Number(action.at("Delay")));RECT r{};GetClientRect(s.timeline,&r);s.scale=std::clamp((r.right-Px(s,310))*96.0/s.dpi/largest,0.002,600.0);s.scroll=0;InvalidateRect(s.timeline,nullptr,FALSE);return;}
    if(id==Snap){s.snap=!s.snap;Set(s,Snap,s.snap?"Snap: 0.1s":"Snap: off");return;}
    if(id==Preview){s.preview=!s.preview;s.tick=GetTickCount();Set(s,Preview,s.preview?"Pause preview":"Timing preview");StatusText(s,"Illustrative authored-delay preview. Use Play Level to test actual group sequencing and gameplay.");return;}
    if(id==AddItem || id==DeleteItem)
    {
        if(!s.selected)return;auto binding=*s.selected;auto values=s.inspector.at("values");auto pointer=Json::json_pointer(binding.pointer);auto& value=values.at(pointer);
        if(id==AddItem){if(!value.is_array() || binding.schema.at("kind")!="ArrayProperty")throw std::runtime_error("Select a dynamic array to add an item.");value.push_back(Magic::Default(binding.schema.at("inner")));}
        else{auto parent=pointer.parent_pointer();auto& array=values.at(parent);if(!array.is_array())throw std::runtime_error("Select an array item to remove.");array.erase(static_cast<size_t>(std::stoul(pointer.back())));s.propertyPointer=parent.to_string();}
        Editor::EditActor(s.inspector,{{binding.root,values.at(binding.root)}});RefreshAll(s);return;
    }
    if(id==UseSelected)
    {
        ReferenceMenu(s);return;
    }
    if(id==CaptureKey)
    {
        if(!s.selected || s.selected->pointer.rfind("/KeyPos/",0)!=0)throw std::runtime_error("Select KeyPos[1] or a later mover key. The builder brush supplies the target pose.");
        auto key=std::stoi(s.selected->pointer.substr(8));Editor::CaptureMoverKey(s.inspector,key);RefreshAll(s);StatusText(s,"Captured builder brush pose relative to mover key 0.");return;
    }
    if(s.eventSnapshot.is_null())throw std::runtime_error("Create or open an event first.");
    auto groups=GroupValues(s);auto& schema=s.eventSnapshot.at("schema").at("Groups").at("inner");
    if(id==AddGroup){groups.push_back(Magic::Default(schema));s.group=static_cast<int>(groups.size())-1;s.action=-1;}
    else if(groups.empty())return;
    else if(id==DuplicateGroup){groups.insert(groups.begin()+s.group+1,groups[s.group]);++s.group;s.action=-1;}
    else if(id==RemoveGroup){groups.erase(groups.begin()+s.group);s.action=-1;}
    else if(id==GroupUp || id==GroupDown){int direction=id==GroupUp?-1:1;Magic::Move(groups,s.group,direction);s.group=std::clamp(s.group+direction,0,static_cast<int>(groups.size())-1);}
    else
    {
        auto& actions=groups[s.group]["EventGroup"];if(s.action<0 || s.action>=static_cast<int>(actions.size()))throw std::runtime_error("Select an action on the timeline.");
        if(id==DuplicateAction){actions.insert(actions.begin()+s.action+1,actions[s.action]);++s.action;}
        else if(id==RemoveAction){actions.erase(actions.begin()+s.action);s.action=-1;}
        else if(id==ActionUp || id==ActionDown){int direction=id==ActionUp?-1:1;Magic::Move(actions,s.action,direction);s.action=std::clamp(s.action+direction,0,static_cast<int>(actions.size())-1);}
        else return;
    }
    ChangeGroups(s,groups);
}
LRESULT CALLBACK ValueProc(HWND w,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
{
    if(message==WM_KEYDOWN && wp==VK_RETURN){SendMessage(reinterpret_cast<HWND>(data),WM_COMMAND,Commit,0);return 0;}
    if(message==WM_GETDLGCODE && wp==VK_RETURN)return DLGC_WANTALLKEYS;
    return DefSubclassProc(w,message,wp,lp);
}
LRESULT CALLBACK NavigationProc(HWND w,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
{
    auto owner=reinterpret_cast<HWND>(data);
    if(message==WM_KEYDOWN)
    {
        if(wp==VK_TAB){auto next=GetNextDlgTabItem(owner,w,(GetKeyState(VK_SHIFT)&0x8000)!=0);if(next)SetFocus(next);return 0;}
        if(wp==VK_F5){SendMessage(owner,WM_COMMAND,Refresh,0);return 0;}
        if((GetKeyState(VK_CONTROL)&0x8000) && GetDlgCtrlID(w)!=Value && (wp=='Z' || wp=='Y')){SendMessage(owner,WM_COMMAND,wp=='Z'?Undo:Redo,0);return 0;}
    }
    return DefSubclassProc(w,message,wp,lp);
}
LRESULT CALLBACK SearchProc(HWND w,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
{
    auto result=DefSubclassProc(w,message,wp,lp);
    // A version 6 edit (Modern appearance) draws its own cue banner: drawing ours too doubled it.
    wchar_t cue[8]{};
    if(message==WM_PAINT && GetWindowTextLengthA(w)==0 && GetFocus()!=w && !SendMessageW(w,EM_GETCUEBANNER,reinterpret_cast<WPARAM>(cue),std::size(cue)))
    {
        auto dc=GetDC(w);RECT r{};GetClientRect(w,&r);r.left+=6;r.top+=2;SetBkMode(dc,TRANSPARENT);SetTextColor(dc,Muted);auto font=SelectObject(dc,reinterpret_cast<HFONT>(SendMessage(w,WM_GETFONT,0,0)));DrawTextA(dc,reinterpret_cast<const char*>(data),-1,&r,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);SelectObject(dc,font);ReleaseDC(w,dc);
    }
    return result;
}
LRESULT CALLBACK WindowProc(HWND w,UINT message,WPARAM wp,LPARAM lp)
{
    auto s=reinterpret_cast<State*>(GetWindowLongPtr(w,GWLP_USERDATA));if(message==WM_NCCREATE){s=static_cast<State*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);s->window=w;SetWindowLongPtr(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}if(!s)return DefWindowProcA(w,message,wp,lp);
    try
    {
        if(message==WM_CREATE)
        {
            s->dpi=GetDpiForWindow(w);s->font=CreateFontA(-Px(*s,14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");s->background=CreateSolidBrush(Background);s->panel=CreateSolidBrush(Panel);
            Add(*s,"EDIT","",EventSearch,ES_AUTOHSCROLL);Add(*s,"COMBOBOX","",Events,CBS_DROPDOWNLIST|WS_VSCROLL);Button(*s,"New Event",New);Button(*s,"Refresh",Refresh);Button(*s,"Undo",Undo);Button(*s,"Redo",Redo);Button(*s,"Play Level",Play);
            Button(*s,"Export JSON...",ExportJson);Button(*s,"Import JSON...",ImportJson);Add(*s,"STATIC","Export an event for editing; import into the open event. Linked actors must already exist.",JsonHint);
            Add(*s,"EDIT","",ActorSearch,ES_AUTOHSCROLL);Add(*s,"COMBOBOX","",Category,CBS_DROPDOWNLIST|WS_VSCROLL);Combo(Item(*s,Category),{"In this event","All actors","Selected in map","Triggers","Movers","Emitters","Sounds","Volumes","Other"});
            Add(*s,"LISTBOX","",Actors,LBS_NOTIFY|WS_VSCROLL|WS_HSCROLL);Button(*s,"Select and focus",Focus);Button(*s,"Add action using actor",Attach);Button(*s,"Attach as trigger",AttachTrigger);Button(*s,"New actor",CreateTarget);Button(*s,"New trigger",CreateTrigger);Button(*s,"Detach trigger (keep actor)",DetachTrigger);
            Add(*s,"COMBOBOX","",Groups,CBS_DROPDOWNLIST|WS_VSCROLL);Button(*s,"Event settings",InspectEvent);Button(*s,"+ Group",AddGroup);Button(*s,"Duplicate",DuplicateGroup);Button(*s,"Remove",RemoveGroup);Button(*s,"Earlier",GroupUp);Button(*s,"Later",GroupDown);
            WNDCLASSA wc{};wc.lpfnWndProc=TimelineProc;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="ReloadedMagicTimeline";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassA(&wc);
            s->timeline=CreateWindowExA(0,wc.lpszClassName,"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,10,10,w,reinterpret_cast<HMENU>(Timeline),wc.hInstance,s);
            Button(*s,"Duplicate",DuplicateAction);Button(*s,"Remove",RemoveAction);Button(*s,"Earlier",ActionUp);Button(*s,"Later",ActionDown);Button(*s,"+",ZoomIn);Button(*s,"-",ZoomOut);Button(*s,"Fit",Fit);Button(*s,s->snap?"Snap: 0.1s":"Snap: off",Snap);Button(*s,"Timing preview",Preview);
            Button(*s,"Actor properties",InspectActor);Add(*s,"EDIT","",PropertySearch,ES_AUTOHSCROLL);s->tree=Add(*s,WC_TREEVIEWA,"",Properties,TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT|TVS_SHOWSELALWAYS);TreeView_SetBkColor(s->tree,Panel);TreeView_SetTextColor(s->tree,Ink);TreeView_SetLineColor(s->tree,Muted);TreeView_SetItemHeight(s->tree,Px(*s,25));
            Add(*s,"EDIT","",Value,ES_AUTOHSCROLL);SetWindowSubclass(Item(*s,Value),ValueProc,1,reinterpret_cast<DWORD_PTR>(w));Add(*s,"COMBOBOX","",Choices,CBS_DROPDOWNLIST|WS_VSCROLL);Button(*s,"Apply",Commit);Button(*s,"+ Array item",AddItem);Button(*s,"Remove item",DeleteItem);Button(*s,"Capture mover key from position",CaptureKey);Button(*s,"Use selected actor / Tag",UseSelected);Add(*s,"STATIC","Create an event or open an existing SMagicEvent.",Status);
            SendMessageW(Item(*s,EventSearch),EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Search events"));SendMessageW(Item(*s,ActorSearch),EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Search name, Tag or class"));SendMessageW(Item(*s,PropertySearch),EM_SETCUEBANNER,0,reinterpret_cast<LPARAM>(L"Search properties / categories"));
            for(HWND child=GetWindow(w,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SetWindowSubclass(child,NavigationProc,1,reinterpret_cast<DWORD_PTR>(w));
            SetWindowSubclass(Item(*s,EventSearch),SearchProc,1,reinterpret_cast<DWORD_PTR>("Search events"));SetWindowSubclass(Item(*s,ActorSearch),SearchProc,1,reinterpret_cast<DWORD_PTR>("Search name, Tag or class"));SetWindowSubclass(Item(*s,PropertySearch),SearchProc,1,reinterpret_cast<DWORD_PTR>("Search properties / categories"));
            Set(*s,UseSelected,"Assets / actors / components...");Set(*s,CaptureKey,"Capture key from builder pose");Layout(*s);RefreshAll(*s);SetTimer(w,1,500,nullptr);return 0;
        }
        if(message==WM_SIZE){Layout(*s);return 0;}
        if(message==WM_GETMINMAXINFO){auto m=reinterpret_cast<MINMAXINFO*>(lp);m->ptMinTrackSize={Px(*s,1080),Px(*s,650)};return 0;}
        if(message==WM_DPICHANGED){s->dpi=HIWORD(wp);auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(w,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER);auto old=s->font;s->font=CreateFontA(-Px(*s,14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");for(HWND child=GetWindow(w,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessage(child,WM_SETFONT,reinterpret_cast<WPARAM>(s->font),TRUE);DeleteObject(old);TreeView_SetItemHeight(s->tree,Px(*s,25));Layout(*s);return 0;}
        if(message==WM_LBUTTONDOWN){RECT r{};GetClientRect(w,&r);int x=GET_X_LPARAM(lp);if(std::abs(x-Px(*s,s->left))<Px(*s,8))s->split=1;else if(std::abs(x-(r.right-Px(*s,s->right)))<Px(*s,8))s->split=2;if(s->split)SetCapture(w);return 0;}
        if(message==WM_MOUSEMOVE && s->split){RECT r{};GetClientRect(w,&r);int x=MulDiv(GET_X_LPARAM(lp),96,s->dpi);if(s->split==1)s->left=x;else s->right=MulDiv(r.right,96,s->dpi)-x;Layout(*s);return 0;}
        if(message==WM_LBUTTONUP || message==WM_CAPTURECHANGED){s->split=0;if(GetCapture()==w)ReleaseCapture();return 0;}
        if(message==WM_COMMAND && !s->loading)
        {
            int id=LOWORD(wp),notification=HIWORD(wp);
            if((id==ActorSearch && notification==EN_CHANGE) || (id==Category && notification==CBN_SELCHANGE)){FilterActors(*s);return 0;}
            if(id==EventSearch && notification==EN_CHANGE){RefreshAll(*s,false);return 0;}
            if(id==PropertySearch && notification==EN_CHANGE){FillInspector(*s);return 0;}
            if(id==Choices && notification==CBN_SELCHANGE){CommitValue(*s);return 0;}
            if(id==Events && notification==CBN_SELCHANGE){int i=static_cast<int>(SendMessage(Item(*s,Events),CB_GETCURSEL,0,0));if(i>=0 && i<static_cast<int>(s->events.size()))LoadEvent(*s,s->events[i].at("path"));return 0;}
            if(id==Groups && notification==CBN_SELCHANGE){s->group=static_cast<int>(SendMessage(Item(*s,Groups),CB_GETCURSEL,0,0));s->action=-1;s->scroll=0;s->inspectedPath=s->eventPath;FillInspector(*s);InvalidateRect(s->timeline,nullptr,FALSE);return 0;}
            if(id==Actors && (notification==LBN_SELCHANGE || notification==LBN_DBLCLK)){s->inspectedPath=SelectedActor(*s).at("path");FillInspector(*s);if(notification==LBN_DBLCLK)CommandAction(*s,Focus);return 0;}
            if(notification==BN_CLICKED && id!=Value)CommandAction(*s,id);return 0;
        }
        if(message==WM_NOTIFY){auto header=reinterpret_cast<NMHDR*>(lp);if(header->idFrom==Properties && header->code==TVN_SELCHANGEDA){auto change=reinterpret_cast<NMTREEVIEWA*>(lp);SelectProperty(*s,reinterpret_cast<Binding*>(change->itemNew.lParam));}return 0;}
        if(message==WM_TIMER)
        {
            if(s->preview){auto now=GetTickCount();s->playhead=std::fmod(s->playhead+(now-s->tick)/1000.0,86400.0);s->tick=now;InvalidateRect(s->timeline,nullptr,FALSE);}
            if(s->dragAction<0 && !s->split && GetFocus()!=Item(*s,Value) && GetFocus()!=Item(*s,Choices))
            {
                if(s->level!=Editor::LevelIdentity() || s->generation!=Editor::MapGeneration() || s->revision!=Editor::ContentRevision())RefreshAll(*s);
                else if(s->selectionRevision!=Editor::Revision())SyncSelection(*s);
            }
            return 0;
        }
        if(message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT || message==WM_CTLCOLORLISTBOX){SetTextColor(reinterpret_cast<HDC>(wp),Ink);SetBkColor(reinterpret_cast<HDC>(wp),message==WM_CTLCOLORSTATIC?Background:Panel);return reinterpret_cast<LRESULT>(message==WM_CTLCOLORSTATIC?s->background:s->panel);}
        if(message==WM_ERASEBKGND){RECT r{};GetClientRect(w,&r);FillRect(reinterpret_cast<HDC>(wp),&r,s->background);return 1;}
        if(message==WM_CLOSE){try{WriteDocument(Editor::Directory()/"magic-workbench.json",{{"version",1},{"left",s->left},{"right",s->right},{"scale",s->scale},{"snap",s->snap}});}catch(...){}DestroyWindow(w);return 0;}
        if(message==WM_NCDESTROY){WindowPlacement::Remember(w,"MagicEventWorkbench");KillTimer(w,1);workbench=nullptr;DeleteObject(s->font);DeleteObject(s->background);DeleteObject(s->panel);SetWindowLongPtr(w,GWLP_USERDATA,0);delete s;return DefWindowProcA(w,message,wp,lp);}
    }
    catch(const std::exception& e){StatusText(*s,e.what());}
    return DefWindowProcA(w,message,wp,lp);
}
}
void Open(HWND owner,const std::string& actorPath)
{
    if(workbench){auto s=reinterpret_cast<State*>(GetWindowLongPtr(workbench,GWLP_USERDATA));if(!actorPath.empty())LoadEvent(*s,actorPath);ShowWindow(workbench,IsIconic(workbench)?SW_RESTORE:SW_SHOW);SetForegroundWindow(workbench);return;}
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_TREEVIEW_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    WNDCLASSA wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="ReloadedMagicWorkbench";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassA(&wc);
    auto s=new State;s->eventPath=actorPath;s->inspectedPath=actorPath;
    try{auto prefs=ReadDocument(Editor::Directory()/"magic-workbench.json",Json::object());s->left=std::clamp(prefs.value("left",260),215,600);s->right=std::clamp(prefs.value("right",365),280,700);s->scale=std::clamp(prefs.value("scale",75.0),0.002,600.0);s->snap=prefs.value("snap",true);}catch(...){}
    if(actorPath.empty())for(const auto& actor:Editor::SelectedIdentities())if(Editor::Compatible(actor.at("path"),"SBase.SMagicEvent")){s->eventPath=actor.at("path");s->inspectedPath=s->eventPath;break;}
    workbench=CreateWindowExA(WS_EX_CONTROLPARENT,wc.lpszClassName,"SMagicEvent Workbench",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1380,820,owner,nullptr,wc.hInstance,s);
    if(workbench){WindowPlacement::Restore(workbench,"MagicEventWorkbench");ShowWindow(workbench,SW_SHOW);}
}
}
