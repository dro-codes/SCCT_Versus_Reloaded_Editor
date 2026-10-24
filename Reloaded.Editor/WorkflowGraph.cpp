#include "pch.h"
#undef min
#undef max
#include "WorkflowGraph.h"
#include "WindowPlacement.h"
#include "WorkflowGraphModel.h"
#include "WorkflowEditor.h"
#include "WorkflowTools.h"
#include "MagicEventWorkbench.h"
#include <commctrl.h>
#include <windowsx.h>
#include <cmath>
#include <limits>

namespace WorkflowGraph
{
using namespace Workflow;
namespace
{
enum Control { Mode=100,Relationship,Class,Unconnected,Follow,Search,Find,Expand,Fit,FitIsland,Refresh,Focus,Rename,Canvas=120,Status };
struct State
{
    HWND window{},canvas{},status{},tooltip{};
    Json actors=Json::array(),links=Json::array();Graph::Model graph;
    unsigned revision=0,generation=0;uintptr_t level=0;
    std::string root,selected,tip;int depth=1;
    double zoom=1,panX=30,panY=30;bool dragging=false;POINT last{};
};
HWND graphWindow=nullptr;
std::string Text(HWND w){int n=GetWindowTextLengthA(w);std::string t(n+1,'\0');GetWindowTextA(w,t.data(),n+1);t.resize(n);return t;}
bool Checked(State& s,int id){return SendMessage(GetDlgItem(s.window,id),BM_GETCHECK,0,0)==BST_CHECKED;}
HWND Add(State& s,const char* type,const char* label,int id,DWORD style=0)
{
    auto w=CreateWindowExA(0,type,label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,100,25,s.window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandle(nullptr),nullptr);
    SendMessage(w,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);return w;
}
POINT Screen(const State& s,double x,double y){return {static_cast<LONG>(std::lround(x*s.zoom+s.panX)),static_cast<LONG>(std::lround(y*s.zoom+s.panY))};}
RECT Box(const State& s,double x,double y,double width,double height){auto a=Screen(s,x,y),b=Screen(s,x+width,y+height);return {a.x,a.y,b.x,b.y};}
void Repaint(State& s){InvalidateRect(s.canvas,nullptr,FALSE);}
void Summary(State& s)
{
    std::string text=std::to_string(s.graph.nodes.size())+" nodes / "+std::to_string(s.graph.edges.size())+" links / "+std::to_string(s.graph.islands.size())+" islands. Blue: actor reference; purple: Event/Tag; orange: unresolved. Drag to pan, wheel to zoom, double-click to focus.";
    if(s.graph.nodes.empty())text="No actors match this view. Select an actor in the editor, choose Whole Level, or change the filters.";
    SetWindowTextA(s.status,text.c_str());
}
void FitGraph(State& s,bool island=false)
{
    if(s.graph.nodes.empty())return;
    double left=1e30,top=1e30,right=-1e30,bottom=-1e30;size_t group=SIZE_MAX;
    if(island)for(const auto& n:s.graph.nodes)if(n.id==s.selected)group=n.island;
    if(island && group==SIZE_MAX){SetWindowTextA(s.status,"Click a node first, then Fit Island.");return;}
    for(size_t i=0;i<s.graph.islands.size();++i)if(!island || i==group){const auto& b=s.graph.islands[i];left=std::min(left,b.x);top=std::min(top,b.y);right=std::max(right,b.x+b.width);bottom=std::max(bottom,b.y+b.height);}
    RECT r{};GetClientRect(s.canvas,&r);s.zoom=std::clamp(std::min((r.right-50)/(right-left),(r.bottom-50)/(bottom-top)),0.03,1.3);
    s.panX=(r.right-(right-left)*s.zoom)/2-left*s.zoom;s.panY=(r.bottom-(bottom-top)*s.zoom)/2-top*s.zoom;Repaint(s);
}
void Rebuild(State& s,bool reload,bool fit)
{
    const auto previousRoot=s.root;
    auto level=Editor::LevelIdentity();auto generation=Editor::MapGeneration();bool changed=level!=s.level || generation!=s.generation;
    if(reload || changed)
    {
        auto actors=Editor::Actors();auto links=Editor::Connections("");
        s.actors=std::move(actors);s.links=std::move(links);s.revision=Editor::ContentRevision();s.generation=generation;s.level=level;
        auto combo=GetDlgItem(s.window,Class);auto previous=Text(combo);SendMessage(combo,CB_RESETCONTENT,0,0);SendMessageA(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>("All actor classes"));
        std::set<std::string> classes;for(auto& a:s.actors)classes.insert(a.at("class"));int selected=0,index=1;
        for(auto& c:classes){SendMessageA(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(c.c_str()));if(c==previous)selected=index;++index;}SendMessage(combo,CB_SETCURSEL,selected,0);
        if(changed){s.selected.clear();s.root.clear();s.depth=1;}
    }
    const bool whole=SendMessage(GetDlgItem(s.window,Mode),CB_GETCURSEL,0,0)==1;
    if(!whole && (s.root.empty() || Checked(s,Follow))) {auto selected=Editor::Actors(true);s.root=selected.empty()?"":selected[0].at("path").get<std::string>();}
    if(!whole && (changed || s.selected.empty() || s.root!=previousRoot))s.selected=s.root;
    Graph::Options options;options.root=whole?"":(s.root.empty()?"<no selection>":s.root);options.depth=s.depth;
    options.relationship=static_cast<int>(SendMessage(GetDlgItem(s.window,Relationship),CB_GETCURSEL,0,0));options.showUnconnected=Checked(s,Unconnected);
    if(SendMessage(GetDlgItem(s.window,Class),CB_GETCURSEL,0,0)>0)options.actorClass=Text(GetDlgItem(s.window,Class));
    s.graph=Graph::Build(s.actors,s.links,options);
    EnableWindow(GetDlgItem(s.window,Expand),!whole);EnableWindow(GetDlgItem(s.window,Follow),!whole);EnableWindow(GetDlgItem(s.window,Unconnected),whole);
    auto label="Expand ("+std::to_string(s.depth)+(s.depth==1?" step)":" steps)");SetWindowTextA(GetDlgItem(s.window,Expand),label.c_str());
    Summary(s);if(fit || changed)FitGraph(s);Repaint(s);
}
void FocusActor(State& s)
{
    if(s.level!=Editor::LevelIdentity() || s.generation!=Editor::MapGeneration()){Rebuild(s,true,true);return;}
    for(const auto& n:s.graph.nodes)if(n.id==s.selected)
    {
        if(n.missing){SetWindowTextA(s.status,"This target is unresolved; there is no actor to select.");return;}
        bool found=false;for(const auto& actor:Editor::Actors())if(actor.at("path")==n.identity.at("path") && actor.at("class")==n.identity.at("class"))found=true;
        if(!found){Rebuild(s,true,false);SetWindowTextA(s.status,"That actor was deleted. The graph has been refreshed.");return;}
        Editor::Select(Json::array({n.identity}),true);return;
    }
}
int HitNode(State& s,POINT p)
{
    for(size_t i=0;i<s.graph.nodes.size();++i){auto& n=s.graph.nodes[i];auto r=Box(s,n.x,n.y,Graph::NodeWidth,Graph::NodeHeight);if(PtInRect(&r,p))return static_cast<int>(i);}return -1;
}
std::pair<POINT,POINT> Endpoints(const State& s,const Graph::Edge& e)
{
    auto& a=s.graph.nodes[e.from];auto& b=s.graph.nodes[e.to];double dx=b.x-a.x,dy=b.y-a.y;
    if(!dx && !dy)return {Screen(s,a.x+180,a.y),Screen(s,a.x+220,a.y+32)};
    double scale=1/std::max(std::abs(dx)/110,std::abs(dy)/(Graph::NodeHeight/2));
    // Reciprocal directions get distinct parallel strokes.
    double length=std::hypot(dx,dy),ox=-dy/length*5,oy=dx/length*5;
    return {Screen(s,a.x+110+dx*scale+ox,a.y+Graph::NodeHeight/2+dy*scale+oy),Screen(s,b.x+110-dx*scale+ox,b.y+Graph::NodeHeight/2-dy*scale+oy)};
}
double Distance(POINT p,POINT a,POINT b)
{
    double dx=b.x-a.x,dy=b.y-a.y,den=dx*dx+dy*dy;if(!den)return std::hypot(double(p.x-a.x),double(p.y-a.y));
    double t=std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/den,0.0,1.0);return std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy);
}
void Hover(State& s,POINT p)
{
    std::string tip;int node=HitNode(s,p);
    if(node>=0){auto& n=s.graph.nodes[node];tip=n.name+"\n"+n.type+"\nTag: "+n.tag+"\n"+(n.missing?"Unresolved target":n.id)+"\nIsland "+std::to_string(n.island+1);}
    else for(const auto& edge:s.graph.edges){auto [a,b]=Endpoints(s,edge);if(Distance(p,a,b)<=6){if(!tip.empty())tip+="\n";tip+=s.graph.nodes[edge.from].name+" -> "+s.graph.nodes[edge.to].name+" : "+edge.property+" / "+edge.kind;if(tip.size()>1800)break;}}
    if(tip==s.tip)return;s.tip=tip;TOOLINFOA info{sizeof(info)};info.hwnd=s.canvas;info.uId=reinterpret_cast<UINT_PTR>(s.canvas);info.lpszText=s.tip.data();SendMessageA(s.tooltip,TTM_UPDATETIPTEXTA,0,reinterpret_cast<LPARAM>(&info));
    if(tip.empty())Summary(s);else SetWindowTextA(s.status,tip.c_str());
}
void Fill(HDC dc,RECT r,COLORREF color){auto brush=CreateSolidBrush(color);FillRect(dc,&r,brush);DeleteObject(brush);}
void Line(HDC dc,POINT a,POINT b){MoveToEx(dc,a.x,a.y,nullptr);LineTo(dc,b.x,b.y);}
void Icon(HDC dc,RECT r,const std::string& kind)
{
    auto p=[&](int x,int y){return POINT{r.left+(r.right-r.left)*x/100,r.top+(r.bottom-r.top)*y/100};};
    auto line=[&](int x,int y,int x2,int y2){Line(dc,p(x,y),p(x2,y2));};
    auto circle=[&](int x,int y,int x2,int y2){auto a=p(x,y),b=p(x2,y2);Ellipse(dc,a.x,a.y,b.x,b.y);};
    auto pen=CreatePen(PS_SOLID,1,RGB(255,255,255));auto old=SelectObject(dc,pen);
    if(kind=="L"){circle(30,18,70,60);line(39,63,61,63);line(42,73,58,73);line(50,6,50,12);line(15,36,23,36);line(77,36,85,36);}
    else if(kind=="T"){POINT bolt[]={p(57,14),p(29,55),p(48,55),p(40,87),p(73,42),p(53,42)};Polygon(dc,bolt,6);}
    else if(kind=="S"){POINT speaker[]={p(19,39),p(36,39),p(54,22),p(54,78),p(36,61),p(19,61)};Polygon(dc,speaker,6);line(65,32,74,42);line(74,42,74,58);line(74,58,65,68);}
    else if(kind=="M"){line(17,50,83,50);line(17,50,34,33);line(17,50,34,67);line(83,50,66,33);line(83,50,66,67);}
    else if(kind=="B"){POINT cube[]={p(22,32),p(50,17),p(78,32),p(78,68),p(50,83),p(22,68)};Polygon(dc,cube,6);line(22,32,50,47);line(50,47,78,32);line(50,47,50,83);}
    else if(kind=="A"){circle(38,17,62,41);line(50,43,50,68);line(29,51,71,51);line(50,68,32,84);line(50,68,68,84);}
    else DrawTextA(dc,"!",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(dc,old);DeleteObject(pen);
}
void Draw(State& s,HDC target)
{
    RECT r{};GetClientRect(s.canvas,&r);if(r.right<=0 || r.bottom<=0)return;
    HDC dc=CreateCompatibleDC(target);auto bitmap=CreateCompatibleBitmap(target,r.right,r.bottom);auto oldBitmap=SelectObject(dc,bitmap);Fill(dc,r,RGB(242,245,249));SetBkMode(dc,TRANSPARENT);
    auto font=CreateFontA(-std::clamp(static_cast<int>(14*s.zoom),9,16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,"Segoe UI");auto oldFont=SelectObject(dc,font);
    auto border=CreatePen(PS_SOLID,1,RGB(199,209,222));auto oldPen=SelectObject(dc,border);auto oldBrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    for(size_t i=0;i<s.graph.islands.size();++i)
    {
        auto& b=s.graph.islands[i];auto box=Box(s,b.x,b.y,b.width,b.height);Rectangle(dc,box.left,box.top,box.right,box.bottom);
        if(s.zoom>.22){box.left+=10;box.top+=8;std::string label="Island "+std::to_string(i+1)+" / "+std::to_string(b.members)+" nodes";SetTextColor(dc,RGB(88,105,126));DrawTextA(dc,label.c_str(),-1,&box,DT_SINGLELINE|DT_NOPREFIX);}
    }
    for(const auto& e:s.graph.edges)
    {
        auto color=!e.resolved?RGB(202,111,19):Graph::IsTagRelationship(e.kind)?RGB(143,83,188):RGB(44,114,188);auto pen=CreatePen(e.resolved?PS_SOLID:PS_DOT,1,color);SelectObject(dc,pen);
        auto [a,b]=Endpoints(s,e);
        if(e.from==e.to){auto& n=s.graph.nodes[e.from];auto box=Box(s,n.x+170,n.y-25,65,65);Arc(dc,box.left,box.top,box.right,box.bottom,a.x,a.y,b.x,b.y);}
        else Line(dc,a,b);
        if(s.zoom>.15){double angle=atan2(double(b.y-a.y),double(b.x-a.x));double length=std::clamp(10*s.zoom,5.0,12.0);for(double side:{-0.45,0.45})Line(dc,b,{LONG(b.x-length*cos(angle+side)),LONG(b.y-length*sin(angle+side))});}
        SelectObject(dc,border);DeleteObject(pen);
    }
    for(const auto& n:s.graph.nodes)
    {
        auto box=Box(s,n.x,n.y,Graph::NodeWidth,Graph::NodeHeight);RECT clipped{};if(!IntersectRect(&clipped,&box,&r))continue;
        bool selected=n.id==s.selected;Fill(dc,box,n.missing?RGB(255,241,218):selected?RGB(219,235,255):RGB(255,255,255));
        auto pen=CreatePen(PS_SOLID,selected?2:1,n.missing?RGB(202,111,19):selected?RGB(34,102,180):RGB(174,188,205));SelectObject(dc,pen);RoundRect(dc,box.left,box.top,box.right,box.bottom,8,8);SelectObject(dc,border);DeleteObject(pen);
        if(s.zoom<.28)continue;
        auto icon=Box(s,n.x+9,n.y+29,27,27);std::string type=Fold(n.type),glyph="A";COLORREF color=RGB(83,120,163);
        if(n.missing){glyph="!";color=RGB(207,120,24);}else if(type.find("light")!=std::string::npos){glyph="L";color=RGB(185,143,12);}else if(type.find("trigger")!=std::string::npos){glyph="T";color=RGB(164,80,170);}else if(type.find("sound")!=std::string::npos){glyph="S";color=RGB(45,147,136);}else if(type.find("mover")!=std::string::npos){glyph="M";color=RGB(70,139,88);}else if(type.find("brush")!=std::string::npos){glyph="B";color=RGB(106,112,128);}
        auto brush=CreateSolidBrush(color);SelectObject(dc,brush);Ellipse(dc,icon.left,icon.top,icon.right,icon.bottom);SelectObject(dc,GetStockObject(NULL_BRUSH));DeleteObject(brush);SetTextColor(dc,RGB(255,255,255));Icon(dc,icon,glyph);
        if(s.zoom<.55)
        {
            // At overview scale, keep two readable lines instead of clipping three.
            RECT title{icon.right+3,box.top+2,box.right-2,box.top+12};
            SetTextColor(dc,RGB(29,46,68));DrawTextA(dc,n.name.c_str(),-1,&title,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
            RECT tagBox{title.left,box.top+12,title.right,box.bottom-1};
            auto tag=n.missing?std::string("Unresolved"):"Tag: "+n.tag;SetTextColor(dc,RGB(57,104,100));
            DrawTextA(dc,tag.c_str(),-1,&tagBox,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);continue;
        }
        auto title=Box(s,n.x+43,n.y+7,168,24);SetTextColor(dc,RGB(29,46,68));DrawTextA(dc,n.name.c_str(),-1,&title,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        auto subtitle=Box(s,n.x+43,n.y+30,168,22);auto shortType=n.type.substr(n.type.find_last_of('.')+1);SetTextColor(dc,RGB(96,111,131));DrawTextA(dc,shortType.c_str(),-1,&subtitle,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
        auto tagBox=Box(s,n.x+43,n.y+55,168,22);auto tag=n.missing?std::string("No matching actor"):"Tag: "+n.tag;SetTextColor(dc,RGB(57,104,100));DrawTextA(dc,tag.c_str(),-1,&tagBox,DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    }
    if(s.graph.nodes.empty()){SetTextColor(dc,RGB(85,102,123));RECT empty{35,35,r.right-30,r.bottom-30};DrawTextA(dc,"Select an actor or switch to Whole Level.\nUnconnected actors are hidden by default.",-1,&empty,DT_WORDBREAK);}
    SelectObject(dc,oldBrush);SelectObject(dc,oldPen);DeleteObject(border);SelectObject(dc,oldFont);DeleteObject(font);BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);SelectObject(dc,oldBitmap);DeleteObject(bitmap);DeleteDC(dc);
}
LRESULT CALLBACK CanvasProc(HWND w,UINT message,WPARAM wp,LPARAM lp)
{
    auto s=reinterpret_cast<State*>(GetWindowLongPtr(w,GWLP_USERDATA));if(message==WM_NCCREATE){s=static_cast<State*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);SetWindowLongPtr(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}if(!s)return DefWindowProcA(w,message,wp,lp);
    try
    {
        if(message==WM_PAINT){PAINTSTRUCT p{};auto dc=BeginPaint(w,&p);Draw(*s,dc);EndPaint(w,&p);return 0;}
        if(message==WM_PRINTCLIENT){Draw(*s,reinterpret_cast<HDC>(wp));return 0;}
        if(message==WM_ERASEBKGND)return 1;
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if(message==WM_CONTEXTMENU)
        {
            POINT screen=p;int hit=-1;
            if(p.x==-1 && p.y==-1){for(size_t i=0;i<s->graph.nodes.size();++i)if(s->graph.nodes[i].id==s->selected){hit=static_cast<int>(i);p=Screen(*s,s->graph.nodes[i].x+110,s->graph.nodes[i].y+42);break;}screen=p;ClientToScreen(w,&screen);}
            else {ScreenToClient(w,&p);hit=HitNode(*s,p);}
            if(hit<0)return 0;
            auto node=s->graph.nodes[hit];auto level=s->level;auto generation=s->generation;s->selected=node.id;Repaint(*s);
            HMENU menu=CreatePopupMenu();AppendMenuA(menu,MF_STRING|(node.missing?MF_GRAYED:0),1,"Select and Focus");AppendMenuA(menu,MF_STRING|(node.missing?MF_GRAYED:0),2,"Rename Tag...");
            if(!node.missing && Editor::Compatible(node.id,"SBase.SMagicEvent"))AppendMenuA(menu,MF_STRING,3,"Edit SMagicEvent...");
            Json objectiveSnapshot;
            if(!node.missing && WorkflowTools::AppendObjectiveMenu(menu,node.identity))objectiveSnapshot=Editor::InspectActor(node.identity);
            int command=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,screen.x,screen.y,w,nullptr);DestroyMenu(menu);
            if(command && (level!=Editor::LevelIdentity() || generation!=Editor::MapGeneration())){Rebuild(*s,true,true);SetWindowTextA(s->status,"The map changed. Choose a node in the refreshed graph.");return 0;}
            if(command==1)FocusActor(*s);
            if(command==2){s->selected=node.id;SendMessage(s->window,WM_COMMAND,Rename,0);}
            if(command==3)MagicEventWorkbench::Open(s->window,node.id);
            if((command>=WorkflowTools::kAddObjective && command<=WorkflowTools::kAddFlagObjective) ||
               (command>=WorkflowTools::kObjectivePlayersFirst && command<=WorkflowTools::kObjectivePlayersLast))
            {
                try{WorkflowTools::RunObjectiveCommand(command,objectiveSnapshot);}
                catch(const std::exception& e){MessageBoxA(w,e.what(),"Objectives",MB_OK|MB_ICONERROR);}
                Rebuild(*s,true,true);
            }
            return 0;
        }
        if(message==WM_LBUTTONDOWN){SetFocus(w);int hit=HitNode(*s,p);if(hit>=0)s->selected=s->graph.nodes[hit].id;s->dragging=true;s->last=p;SetCapture(w);Repaint(*s);return 0;}
        if(message==WM_LBUTTONUP || message==WM_CAPTURECHANGED){s->dragging=false;if(message==WM_LBUTTONUP)ReleaseCapture();return 0;}
        if(message==WM_MOUSEMOVE){if(s->dragging){s->panX+=p.x-s->last.x;s->panY+=p.y-s->last.y;s->last=p;Repaint(*s);}else Hover(*s,p);return 0;}
        if(message==WM_LBUTTONDBLCLK){s->dragging=false;ReleaseCapture();int hit=HitNode(*s,p);if(hit>=0){s->selected=s->graph.nodes[hit].id;FocusActor(*s);}return 0;}
        if(message==WM_MOUSEWHEEL){ScreenToClient(w,&p);double old=s->zoom;s->zoom=std::clamp(old*std::pow(1.2,GET_WHEEL_DELTA_WPARAM(wp)/120.0),.03,2.5);s->panX=p.x-(p.x-s->panX)*s->zoom/old;s->panY=p.y-(p.y-s->panY)*s->zoom/old;Repaint(*s);return 0;}
        if(message==WM_KEYDOWN){if(wp==VK_HOME)FitGraph(*s);else if(wp==VK_RETURN)FocusActor(*s);else{if(wp==VK_LEFT)s->panX+=40;if(wp==VK_RIGHT)s->panX-=40;if(wp==VK_UP)s->panY+=40;if(wp==VK_DOWN)s->panY-=40;Repaint(*s);}return 0;}
    }catch(const std::exception& e){SetWindowTextA(s->status,e.what());}
    return DefWindowProcA(w,message,wp,lp);
}
void Layout(State& s)
{
    RECT r{};GetClientRect(s.window,&r);
    const int ids[]={Mode,Relationship,Class,Refresh,Fit,FitIsland};const int widths[]={150,160,240,90,100,100};int x=12;
    for(int i=0;i<6;++i){MoveWindow(GetDlgItem(s.window,ids[i]),x,12,widths[i],i<3?300:27,TRUE);x+=widths[i]+8;}
    MoveWindow(GetDlgItem(s.window,Unconnected),12,49,175,25,TRUE);MoveWindow(GetDlgItem(s.window,Follow),195,49,145,25,TRUE);MoveWindow(GetDlgItem(s.window,Expand),345,49,145,27,TRUE);
    MoveWindow(GetDlgItem(s.window,Search),500,49,180,25,TRUE);MoveWindow(GetDlgItem(s.window,Find),688,49,80,27,TRUE);MoveWindow(GetDlgItem(s.window,Focus),776,49,150,27,TRUE);
    MoveWindow(s.canvas,12,88,std::max(1L,r.right-24),std::max(1L,r.bottom-150),TRUE);MoveWindow(s.status,12,r.bottom-55,r.right-24,50,TRUE);
}
LRESULT CALLBACK WindowProc(HWND w,UINT message,WPARAM wp,LPARAM lp)
{
    auto s=reinterpret_cast<State*>(GetWindowLongPtr(w,GWLP_USERDATA));if(message==WM_NCCREATE){s=static_cast<State*>(reinterpret_cast<CREATESTRUCT*>(lp)->lpCreateParams);s->window=w;SetWindowLongPtr(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}if(!s)return DefWindowProcA(w,message,wp,lp);
    try
    {
        if(message==WM_CREATE)
        {
            auto combo=[&](int id,std::initializer_list<const char*> choices){auto c=Add(*s,"COMBOBOX","",id,CBS_DROPDOWNLIST|WS_VSCROLL);for(auto text:choices)SendMessageA(c,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));SendMessage(c,CB_SETCURSEL,0,0);};
            combo(Mode,{"Selected Actor","Whole Level"});combo(Relationship,{"All relationships","Actor references","Event / Tag"});combo(Class,{"All actor classes"});
            Add(*s,"BUTTON","Refresh",Refresh);Add(*s,"BUTTON","Fit Graph",Fit);Add(*s,"BUTTON","Fit Island",FitIsland);Add(*s,"BUTTON","Show unconnected",Unconnected,BS_AUTOCHECKBOX);Add(*s,"BUTTON","Follow Selection",Follow,BS_AUTOCHECKBOX);SendMessage(GetDlgItem(w,Follow),BM_SETCHECK,BST_CHECKED,0);
            Add(*s,"BUTTON","Expand (1 step)",Expand);Add(*s,"EDIT","",Search,WS_BORDER|ES_AUTOHSCROLL);SendMessageA(GetDlgItem(w,Search),EM_SETLIMITTEXT,200,0);SendMessageW(GetDlgItem(w,Search),EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(L"Find actor, tag or class"));Add(*s,"BUTTON","Find Next",Find);Add(*s,"BUTTON","Select and Focus",Focus);
            s->status=Add(*s,"STATIC","",Status);s->canvas=CreateWindowExA(WS_EX_CLIENTEDGE,"ReloadedConnectionCanvas","",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,1,1,w,reinterpret_cast<HMENU>(Canvas),GetModuleHandle(nullptr),s);
            s->tooltip=CreateWindowExA(WS_EX_TOPMOST,TOOLTIPS_CLASSA,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,s->canvas,nullptr,GetModuleHandle(nullptr),nullptr);
            TOOLINFOA info{sizeof(info)};info.uFlags=TTF_SUBCLASS|TTF_IDISHWND;info.hwnd=s->canvas;info.uId=reinterpret_cast<UINT_PTR>(s->canvas);info.lpszText=const_cast<char*>("");SendMessageA(s->tooltip,TTM_ADDTOOLA,0,reinterpret_cast<LPARAM>(&info));SendMessage(s->tooltip,TTM_SETMAXTIPWIDTH,0,550);
            Layout(*s);Rebuild(*s,true,true);SetTimer(w,1,1000,nullptr);return 0;
        }
        if(message==WM_SIZE){if(s->canvas)Layout(*s);return 0;}
        if(message==WM_GETMINMAXINFO){reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={990,500};return 0;}
        if(message==WM_COMMAND)
        {
            int id=LOWORD(wp),notification=HIWORD(wp);
            if(id==Mode || id==Relationship || id==Class){if(notification==CBN_SELCHANGE){if(id==Mode)s->depth=1;Rebuild(*s,false,true);}return 0;}
            if(id==Unconnected || id==Follow){Rebuild(*s,false,true);return 0;}
            if(id==Refresh){Rebuild(*s,true,false);return 0;}
            if(id==Expand){s->depth=std::min(10,s->depth+1);Rebuild(*s,false,true);return 0;}
            if(id==Fit || id==FitIsland){FitGraph(*s,id==FitIsland);return 0;}
            if(id==Focus){FocusActor(*s);return 0;}
            if(id==Rename)
            {
                if(s->level!=Editor::LevelIdentity() || s->generation!=Editor::MapGeneration()){Rebuild(*s,true,true);return 0;}
                for(const auto& entry:s->graph.nodes)if(entry.id==s->selected && !entry.missing){auto node=entry;WorkflowTools::RenameActorTag(s->window,node.id,node.type);Rebuild(*s,true,false);break;}return 0;
            }
            if(id==Find)
            {
                auto query=Fold(Text(GetDlgItem(w,Search)));if(query.empty())return 0;size_t start=0;for(size_t i=0;i<s->graph.nodes.size();++i)if(s->graph.nodes[i].id==s->selected)start=i+1;
                for(size_t j=0;j<s->graph.nodes.size();++j){auto& n=s->graph.nodes[(start+j)%s->graph.nodes.size()];if(Fold(n.name+" "+n.id+" "+n.type+" "+n.tag).find(query)!=std::string::npos){s->selected=n.id;RECT r{};GetClientRect(s->canvas,&r);s->zoom=1;s->panX=r.right/2.0-n.x-110;s->panY=r.bottom/2.0-n.y-Graph::NodeHeight/2;Repaint(*s);SetWindowTextA(s->status,n.id.c_str());return 0;}}
                SetWindowTextA(s->status,"No match in the visible graph. Try Whole Level or change the filters.");return 0;
            }
        }
        if(message==WM_TIMER)
        {
            if(s->level!=Editor::LevelIdentity() || s->generation!=Editor::MapGeneration() || s->revision!=Editor::ContentRevision())Rebuild(*s,true,false);
            else if((Checked(*s,Follow) || s->root.empty()) && SendMessage(GetDlgItem(w,Mode),CB_GETCURSEL,0,0)==0){auto selected=Editor::Actors(true);auto root=selected.empty()?"":selected[0].at("path").get<std::string>();if(root!=s->root){s->depth=1;Rebuild(*s,false,true);}}
            return 0;
        }
        if(message==WM_CLOSE){DestroyWindow(w);return 0;}
    }catch(const std::exception& e){if(s->status)SetWindowTextA(s->status,e.what());if(message==WM_CREATE)return -1;}
    if(message==WM_NCDESTROY){WindowPlacement::Remember(w,"ConnectionGraph");KillTimer(w,1);if(s->tooltip)DestroyWindow(s->tooltip);SetWindowLongPtr(w,GWLP_USERDATA,0);graphWindow=nullptr;delete s;}
    return DefWindowProcA(w,message,wp,lp);
}
}
void Open(HWND owner)
{
    if(graphWindow && IsWindow(graphWindow)){ShowWindow(graphWindow,IsIconic(graphWindow)?SW_RESTORE:SW_SHOW);SetForegroundWindow(graphWindow);return;}
    WNDCLASSA canvas{};canvas.lpfnWndProc=CanvasProc;canvas.hInstance=GetModuleHandle(nullptr);canvas.lpszClassName="ReloadedConnectionCanvas";canvas.style=CS_DBLCLKS;canvas.hCursor=LoadCursor(nullptr,IDC_ARROW);RegisterClassA(&canvas);
    WNDCLASSA wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=canvas.hInstance;wc.lpszClassName="ReloadedConnectionGraph";wc.hCursor=canvas.hCursor;wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);RegisterClassA(&wc);
    auto state=new State;graphWindow=CreateWindowExA(WS_EX_CONTROLPARENT,wc.lpszClassName,"Gameplay Connection Graph",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1200,800,owner,nullptr,wc.hInstance,state);
    if(graphWindow){WindowPlacement::Restore(graphWindow,"ConnectionGraph");ShowWindow(graphWindow,SW_SHOW);}
}
}
