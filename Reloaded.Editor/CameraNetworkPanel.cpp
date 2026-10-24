#include "pch.h"
#undef min
#undef max
#include "CameraNetworkPanel.h"
#include "WindowPlacement.h"
#include "CameraNetworkModel.h"
#include "WorkflowEditor.h"
#include <commctrl.h>
#include <algorithm>

namespace CameraNetworkPanel
{
using namespace Workflow;
namespace
{
enum Id { Networks=100,List,NewNetwork,AddCamera,Refresh,Undo,Redo,Name,SaveName,Up,Down,First,Loop,ApplyOrder,
    Previous,Next,Preview,ReturnView,Focus,Capture,Detach,LinkSelected,Properties,Status,NameLabel,Help };
struct State
{
    HWND window{};HFONT font{};UINT dpi=96;Json snapshot=Json::array(),cameras=Json::array(),savedView;
    std::vector<Cameras::Network> networks;int index=-1;std::string path;
    uintptr_t level=0;unsigned generation=0,revision=0;bool loading=false;
};
HWND panel{};
HWND Item(State& s,int id){return GetDlgItem(s.window,id);}
int Px(State& s,int n){return MulDiv(n,s.dpi,96);}
std::wstring Wide(const std::string& text)
{
    int n=MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0);std::wstring result(n,L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),n);return result;
}
void Set(State& s,int id,const std::string& value){SetWindowTextW(Item(s,id),Wide(value).c_str());}
std::string Text(HWND window)
{
    int n=GetWindowTextLengthW(window);std::wstring wide(n+1,L'\0');GetWindowTextW(window,wide.data(),n+1);
    int count=WideCharToMultiByte(CP_UTF8,0,wide.data(),n,nullptr,0,nullptr,nullptr);std::string result(count,'\0');
    WideCharToMultiByte(CP_UTF8,0,wide.data(),n,result.data(),count,nullptr,nullptr);return result;
}
void Bounds(State& s,int id,int x,int y,int w,int h){MoveWindow(Item(s,id),Px(s,x),Px(s,y),Px(s,w),Px(s,h),TRUE);}
void Layout(State& s)
{
    RECT r{};GetClientRect(s.window,&r);int w=MulDiv(r.right,96,s.dpi),h=MulDiv(r.bottom,96,s.dpi),right=w-286;
    Bounds(s,Networks,12,12,w-320,260);Bounds(s,Refresh,w-296,12,90,28);Bounds(s,Undo,w-200,12,88,28);Bounds(s,Redo,w-106,12,94,28);
    Bounds(s,List,12,52,w-316,h-170);
    Bounds(s,NameLabel,12,h-106,100,20);Bounds(s,Name,12,h-82,w-424,28);Bounds(s,SaveName,w-406,h-82,102,28);
    int y=52;auto row=[&](int id){Bounds(s,id,right,y,274,28);y+=34;};
    row(NewNetwork);row(AddCamera);row(LinkSelected);
    Bounds(s,Up,right,y,133,28);Bounds(s,Down,right+141,y,133,28);y+=34;row(First);row(Loop);row(ApplyOrder);row(Detach);
    Bounds(s,Previous,right,y,133,28);Bounds(s,Next,right+141,y,133,28);y+=34;row(Preview);row(ReturnView);row(Focus);row(Capture);row(Properties);
    Bounds(s,Help,12,h-44,w-24,18);Bounds(s,Status,12,h-24,w-24,20);
}
Json Paths(const Json& cameras){Json result=Json::array();for(const auto& c:cameras)result.push_back(Cameras::Path(c));return result;}
bool Looping(State& s){return SendMessage(Item(s,Loop),BM_GETCHECK,0,0)==BST_CHECKED;}
Json& Current(State& s)
{
    if(s.index<0 || s.index>=static_cast<int>(s.cameras.size()))throw std::runtime_error("Choose a camera first.");return s.cameras[s.index];
}
void SelectRow(State& s,int index)
{
    s.index=index;s.path=index>=0?Cameras::Path(s.cameras[index]):"";
    SendMessage(Item(s,List),LB_SETCURSEL,index,0);Set(s,Name,index>=0?Cameras::Name(s.cameras[index]):"");
    bool selected=index>=0;
    for(int id:{SaveName,Up,Down,First,ApplyOrder,Detach,Previous,Next,Preview,Focus,Capture,Properties,AddCamera,Loop})EnableWindow(Item(s,id),selected);
    EnableWindow(Item(s,Up),selected && index>0);EnableWindow(Item(s,First),selected && index>0);
    EnableWindow(Item(s,Down),selected && index+1<static_cast<int>(s.cameras.size()));
    EnableWindow(Item(s,ReturnView),!s.savedView.is_null());
}
void LoadNetwork(State& s,int index)
{
    s.cameras=index>=0?s.networks[index].cameras:Json::array();SendMessage(Item(s,List),LB_RESETCONTENT,0,0);
    int selected=-1;
    for(size_t i=0;i<s.cameras.size();++i)
    {
        const auto& c=s.cameras[i];auto path=Cameras::Path(c);auto label=std::to_string(i+1)+". "+Cameras::Name(c)+"   ["+path.substr(path.find_last_of('.')+1)+"]";
        SendMessageW(Item(s,List),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(Wide(label).c_str()));
        if(Cameras::Path(c)==s.path)selected=static_cast<int>(i);
    }
    SendMessage(Item(s,Loop),BM_SETCHECK,index>=0 && s.networks[index].loop?BST_CHECKED:BST_UNCHECKED,0);
    SelectRow(s,selected>=0?selected:s.cameras.empty()?-1:0);
    Set(s,Status,index<0?"No cameras in this map. Create a network to begin.":s.networks[index].warning.empty()?"Changes save with the map and support native undo.":s.networks[index].warning);
}
void RefreshAll(State& s)
{
    s.loading=true;struct Done{bool& b;~Done(){b=false;}} done{s.loading};
    auto level=Editor::LevelIdentity();auto generation=Editor::MapGeneration();
    if(level!=s.level || generation!=s.generation){s.savedView=Json{};s.path.clear();}
    s.level=level;s.generation=generation;s.snapshot=Editor::CameraNetworks();s.networks=Cameras::Networks(s.snapshot);s.revision=Editor::ContentRevision();
    SendMessage(Item(s,Networks),CB_RESETCONTENT,0,0);int selected=-1;
    for(size_t i=0;i<s.networks.size();++i)
    {
        auto& n=s.networks[i];auto label="Network "+std::to_string(i+1)+" - "+Cameras::Name(n.cameras[0])+" ("+std::to_string(n.cameras.size())+")";
        if(!n.warning.empty())label+=" - check links";
        SendMessageW(Item(s,Networks),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(Wide(label).c_str()));
        for(const auto& c:n.cameras)if(Cameras::Path(c)==s.path)selected=static_cast<int>(i);
    }
    if(selected<0 && !s.networks.empty())selected=0;
    SendMessage(Item(s,Networks),CB_SETCURSEL,selected,0);LoadNetwork(s,selected);
}
Json ViewCamera()
{
    // The native editor uses 12/13/14 for orthographic XY/XZ/YZ views.
    auto view=Editor::CaptureView();for(const auto& camera:view.at("cameras"))
    {
        int mode=camera.at("mode");if(mode>=1 && mode<=6)return camera;
    }
    throw std::runtime_error("Open a perspective viewport first.");
}
void CheckMap(State& s)
{
    if(s.level!=Editor::LevelIdentity() || s.generation!=Editor::MapGeneration())throw std::runtime_error("The map changed. Refresh the camera panel.");
}
void LookThrough(State& s)
{
    CheckMap(s);auto camera=Editor::InspectActor(Current(s).at("actor"));auto view=ViewCamera();
    if(s.savedView.is_null())s.savedView={{"cameras",Json::array({view})},{"actors",Json::array()}};
    Vector position;Rotation rotation;const char* xyz[]={"X","Y","Z"};const char* pyr[]={"Pitch","Yaw","Roll"};
    for(int i=0;i<3;++i){position[i]=std::stod(camera.at("values").at("Location").at(xyz[i]).get<std::string>());rotation[i]=std::stoi(camera.at("values").at("Rotation").at(pyr[i]).get<std::string>());}
    view["position"]=position;view["rotation"]=rotation;
    Editor::RestoreView({{"cameras",Json::array({view})},{"actors",Json::array()}});
    EnableWindow(Item(s,ReturnView),TRUE);Set(s,Status,"Previewing "+Cameras::Name(camera)+". Previous / Next steps through this network.");
}
void Restore(State& s)
{
    if(s.savedView.is_null())return;
    if(s.level==Editor::LevelIdentity() && s.generation==Editor::MapGeneration())Editor::RestoreView(s.savedView);
    s.savedView=Json{};EnableWindow(Item(s,ReturnView),FALSE);
}
void Action(State& s,int id)
{
    if(id==Refresh){RefreshAll(s);return;}
    CheckMap(s);
    if(id==Undo || id==Redo){Editor::Exec(id==Undo?"TRANSACTION UNDO":"TRANSACTION REDO");RefreshAll(s);return;}
    if(id==ReturnView){Restore(s);return;}
    if(id==NewNetwork || id==AddCamera)
    {
        auto actor=Editor::AddNetworkCamera(s.snapshot,id==NewNetwork?Json::array():Paths(s.cameras),id==NewNetwork?false:Looping(s));
        s.path=actor.at("path");RefreshAll(s);Set(s,Status,"Camera added at the builder brush. Use 'Place at viewport' to match your view.");return;
    }
    if(id==LinkSelected)
    {
        Json paths=Json::array();for(const auto& a:Editor::SelectedIdentities())if(Editor::Compatible(a.at("path"),"SBase.SCamNetwork"))paths.push_back(a.at("path"));
        if(paths.size()<2)throw std::runtime_error("Select at least two cameras in the map. Include complete networks to merge them.");
        // Keep the displayed game order within each selected component.
        Json ordered=Json::array();for(const auto& n:s.networks)for(const auto& c:n.cameras)if(std::find(paths.begin(),paths.end(),Cameras::Path(c))!=paths.end())ordered.push_back(Cameras::Path(c));
        Editor::OrderCameras(s.snapshot,ordered,Looping(s));s.path=ordered[0];RefreshAll(s);return;
    }
    auto camera=Current(s);
    if(id==SaveName)
    {
        auto name=Text(Item(s,Name));if(name.empty() || name.find_first_not_of(" \t")==std::string::npos)throw std::runtime_error("Enter a camera display name.");
        Editor::EditActor(camera,{{"CamName",Json(name).dump()}});RefreshAll(s);return;
    }
    if(id==Up || id==Down || id==First || id==ApplyOrder || id==Detach)
    {
        auto order=s.cameras;int index=s.index;
        if(id==Up && index>0)std::swap(order[index],order[index-1]);
        if(id==Down && index+1<static_cast<int>(order.size()))std::swap(order[index],order[index+1]);
        if(id==First){order.erase(order.begin()+index);order.insert(order.begin(),camera);}
        Editor::OrderCameras(s.snapshot,Paths(order),Looping(s),id==Detach?s.path:"");RefreshAll(s);return;
    }
    if(id==Previous || id==Next)
    {
        int count=static_cast<int>(s.cameras.size());SelectRow(s,(s.index+(id==Next?1:count-1))%count);LookThrough(s);return;
    }
    if(id==Preview){LookThrough(s);return;}
    if(id==Focus || id==Properties)
    {
        Editor::Select(Json::array({camera.at("actor")}),id==Focus);if(id==Properties)Editor::Exec("ACTOR PROPERTIES");return;
    }
    if(id==Capture)
    {
        auto view=ViewCamera();Json position,rotation;const char* xyz[]={"X","Y","Z"};const char* pyr[]={"Pitch","Yaw","Roll"};
        for(int i=0;i<3;++i){position[xyz[i]]=std::to_string(view.at("position")[i].get<double>());rotation[pyr[i]]=std::to_string(view.at("rotation")[i].get<int>());}
        Editor::EditActor(camera,{{"Location",position},{"Rotation",rotation}});RefreshAll(s);return;
    }
}
LRESULT CALLBACK Keys(HWND w,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR owner)
{
    if(message==WM_KEYDOWN)
    {
        auto window=reinterpret_cast<HWND>(owner);
        if(wp==VK_TAB){SetFocus(GetNextDlgTabItem(window,w,(GetKeyState(VK_SHIFT)&0x8000)!=0));return 0;}
        if(wp==VK_F5){SendMessage(window,WM_COMMAND,Refresh,0);return 0;}
        if(wp==VK_RETURN && GetDlgCtrlID(w)==Name){SendMessage(window,WM_COMMAND,SaveName,0);return 0;}
        if(GetDlgCtrlID(w)!=Name && (GetKeyState(VK_CONTROL)&0x8000) && (wp=='Z' || wp=='Y')){SendMessage(window,WM_COMMAND,wp=='Z'?Undo:Redo,0);return 0;}
    }
    return DefSubclassProc(w,message,wp,lp);
}
void Font(State& s)
{
    auto old=s.font;s.font=CreateFontA(-Px(s,14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,"Segoe UI");
    for(auto child=GetWindow(s.window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT))SendMessage(child,WM_SETFONT,reinterpret_cast<WPARAM>(s.font),TRUE);
    if(old)DeleteObject(old);
}
LRESULT CALLBACK Proc(HWND w,UINT message,WPARAM wp,LPARAM lp)
{
    auto s=reinterpret_cast<State*>(GetWindowLongPtr(w,GWLP_USERDATA));
    if(message==WM_NCCREATE){s=static_cast<State*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);s->window=w;SetWindowLongPtr(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
    if(!s)return DefWindowProcA(w,message,wp,lp);
    try
    {
        if(message==WM_CREATE)
        {
            s->dpi=GetDpiForWindow(w);
            auto add=[&](const char* type,const char* text,int id,DWORD style=0)
            {
                auto child=CreateWindowExA(0,type,text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,10,10,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandle(nullptr),nullptr);
                SetWindowSubclass(child,Keys,1,reinterpret_cast<DWORD_PTR>(w));
            };
            auto button=[&](const char* text,int id){add("BUTTON",text,id,BS_PUSHBUTTON);};
            add("COMBOBOX","",Networks,CBS_DROPDOWNLIST|WS_VSCROLL);add("LISTBOX","",List,LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|WS_VSCROLL|WS_HSCROLL|WS_BORDER);
            SendMessage(Item(*s,List),LB_SETHORIZONTALEXTENT,Px(*s,1400),0);
            button("New network",NewNetwork);button("Add camera to network",AddCamera);button("Link selected cameras",LinkSelected);
            button("Refresh",Refresh);button("Undo",Undo);button("Redo",Redo);button("Move up",Up);button("Move down",Down);button("Make first camera",First);
            add("BUTTON","Loop last camera back to first",Loop,BS_AUTOCHECKBOX);button("Apply order / repair links",ApplyOrder);button("Detach camera (keep actor)",Detach);
            button("< Previous",Previous);button("Next >",Next);button("Look through camera",Preview);button("Return to original view",ReturnView);
            button("Select and focus in map",Focus);button("Place at viewport position + aim",Capture);button("More camera properties...",Properties);
            add("STATIC","Display name",NameLabel);add("EDIT","",Name,ES_AUTOHSCROLL|WS_BORDER);SendMessage(Item(*s,Name),EM_SETLIMITTEXT,512,0);button("Rename",SaveName);
            add("STATIC","Double-click a camera to preview. Rename edits CamName; actor Tags stay intact.",Help);add("STATIC","",Status);
            Font(*s);Layout(*s);RefreshAll(*s);SetTimer(w,1,750,nullptr);return 0;
        }
        if(message==WM_SIZE){Layout(*s);return 0;}
        if(message==WM_GETMINMAXINFO){reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={Px(*s,850),Px(*s,650)};return 0;}
        if(message==WM_DPICHANGED){s->dpi=HIWORD(wp);auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(w,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER);Font(*s);Layout(*s);return 0;}
        if(message==WM_COMMAND && !s->loading)
        {
            int id=LOWORD(wp),code=HIWORD(wp);
            if(id==Networks && code==CBN_SELCHANGE){LoadNetwork(*s,static_cast<int>(SendMessage(Item(*s,Networks),CB_GETCURSEL,0,0)));return 0;}
            if(id==List && (code==LBN_SELCHANGE || code==LBN_DBLCLK))
            {
                int index=static_cast<int>(SendMessage(Item(*s,List),LB_GETCURSEL,0,0));if(index>=0){SelectRow(*s,index);if(code==LBN_DBLCLK || !s->savedView.is_null())LookThrough(*s);}return 0;
            }
            if(code==BN_CLICKED && id!=Name && id!=Loop)Action(*s,id);return 0;
        }
        if(message==WM_TIMER)
        {
            if(s->level!=Editor::LevelIdentity() || s->generation!=Editor::MapGeneration())RefreshAll(*s);
            else if(s->revision!=Editor::ContentRevision())Set(*s,Status,"The map has changed. Refresh to show current camera settings.");return 0;
        }
        if(message==WM_CLOSE){try{Restore(*s);}catch(...){}DestroyWindow(w);return 0;}
        if(message==WM_NCDESTROY){WindowPlacement::Remember(w,"CameraNetworks");KillTimer(w,1);panel=nullptr;DeleteObject(s->font);SetWindowLongPtr(w,GWLP_USERDATA,0);delete s;return DefWindowProcA(w,message,wp,lp);}
    }
    catch(const std::exception& e){Set(*s,Status,e.what());}
    return DefWindowProcA(w,message,wp,lp);
}
}
void Open(HWND owner)
{
    if(panel){ShowWindow(panel,IsIconic(panel)?SW_RESTORE:SW_SHOW);SetForegroundWindow(panel);return;}
    Editor::LevelIdentity();INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES};InitCommonControlsEx(&controls);
    WNDCLASSA wc{};wc.lpfnWndProc=Proc;wc.hInstance=GetModuleHandle(nullptr);wc.lpszClassName="ReloadedCameraNetworks";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassA(&wc);
    auto state=new State;for(const auto& actor:Editor::SelectedIdentities())if(Editor::Compatible(actor.at("path"),"SBase.SCamNetwork")){state->path=actor.at("path");break;}
    // Keep an initial map identity so RefreshAll retains the selected actor.
    state->level=Editor::LevelIdentity();state->generation=Editor::MapGeneration();
    state->dpi=owner?GetDpiForWindow(owner):96;
    panel=CreateWindowExA(WS_EX_CONTROLPARENT,wc.lpszClassName,"SCamNetwork Manager",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,Px(*state,1000),Px(*state,730),owner,nullptr,wc.hInstance,state);
    if(panel){WindowPlacement::Restore(panel,"CameraNetworks");ShowWindow(panel,SW_SHOW);}
}
}
