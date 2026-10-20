// Included by NativeMapRecoveryProbe after its existing fixture helpers.
#include "../Reloaded.Editor/Include/nlohmann/json.hpp"
#include "../Reloaded.Editor/MapRecoveryModel.h"
#include "mcp/McpPipeClient.h"
#include <stdexcept>
#include <functional>
#include <map>
#include <set>
#include <filesystem>
#include <commctrl.h>
#include <commdlg.h>
#include <dlgs.h>
namespace WorkflowProbe {
UINT objectivePopupChoice=0;bool objectivePopupFound=false;int objectivePopupTicks=0;
void CALLBACK ChooseObjectivePopup(HWND,UINT,UINT_PTR,DWORD)
{
    if(++objectivePopupTicks>60){EndMenu();return;}
    if(objectivePopupFound)return;
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM)->BOOL {
        char cls[64]{};GetClassNameA(window,cls,sizeof(cls));if(strcmp(cls,"#32768"))return TRUE;
        auto menu=reinterpret_cast<HMENU>(SendMessage(window,0x01e1,0,0));
        for(int i=0;i<GetMenuItemCount(menu);++i)if(GetMenuItemID(menu,i)==objectivePopupChoice)
        {
            objectivePopupFound=true;
            GUITHREADINFO gui{sizeof(gui)};GetGUIThreadInfo(GetCurrentThreadId(),&gui);
            auto receiver=gui.hwndMenuOwner?gui.hwndMenuOwner:window;
            SendMessage(window,0x01e5,i,0); // MN_SELECTITEM: target this fixture's menu item.
            PostMessage(receiver,WM_KEYDOWN,VK_RETURN,0);return FALSE;
        }
        return TRUE;
    },0);
}
bool vertexPopupChecked=false;
bool vertexPortalEnabled=false;
void CALLBACK InspectVertexPopup(HWND,UINT,UINT_PTR,DWORD)
{
    EnumThreadWindows(GetCurrentThreadId(),[](HWND window,LPARAM)->BOOL {
        char cls[64]{};GetClassNameA(window,cls,sizeof(cls));
        if(!strcmp(cls,"#32768")){
            auto menu=reinterpret_cast<HMENU>(SendMessage(window,0x01e1,0,0)); // MN_GETHMENU
            auto state=GetMenuState(menu,40944,MF_BYCOMMAND);
            vertexPopupChecked=state!=UINT(-1) && !(state&(MF_DISABLED|MF_GRAYED));
            const auto portalState=GetMenuState(menu,40981,MF_BYCOMMAND);
            vertexPortalEnabled=portalState!=UINT(-1) && !(portalState&(MF_DISABLED|MF_GRAYED));
            EndMenu();return FALSE;
        }
        return TRUE;
    },0);
}
unsigned char* MagicActor(const nlohmann::json& identity)
{
    auto path=identity.at("path").get<std::string>();auto name=path.substr(path.find_last_of('.')+1);
    auto level=*reinterpret_cast<unsigned char**>(*reinterpret_cast<uintptr_t*>(kEditor)+0x130);auto data=*reinterpret_cast<unsigned char***>(level+0x2c);int count=*reinterpret_cast<int*>(level+0x30);
    for(int i=2;i<count;++i)if(data[i] && name==ObjectName(data[i]))return data[i];throw std::runtime_error("Magic fixture actor missing.");
}
void CALLBACK AcceptDesignPlay(HWND,UINT,UINT_PTR,DWORD)
{
    EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM)->BOOL {char title[128]{};GetWindowTextA(w,title,sizeof(title));if(!strcmp(title,"Play Map Options"))SendMessage(GetDlgItem(w,IDOK),BM_CLICK,0,0);return TRUE;},0);
}
BOOL WINAPI DesignKeepEditorWindow(HWND){return TRUE;}
nlohmann::json designLaunchStarts;
bool designLaunchSeen=false,designLaunchPose=false;
HINSTANCE WINAPI DesignCaptureLaunch(HWND,LPCSTR,LPCSTR,LPCSTR parameters,LPCSTR,INT)
{
    designLaunchSeen=parameters && strstr(parameters,"Editeur=true");designLaunchPose=true;
    for(auto& start:designLaunchStarts){auto p=reinterpret_cast<float*>(MagicActor(start)+0x80);designLaunchPose=designLaunchPose && p[0]==300 && p[1]==400 && p[2]==500;}
    return reinterpret_cast<HINSTANCE>(33);
}
nlohmann::json ActivateMagic(const nlohmann::json& identity,const nlohmann::json& source,bool activate)
{
    using A=uintptr_t;auto actor=MagicActor(identity);auto type=*reinterpret_cast<unsigned char**>(actor+0x24);unsigned char* function=nullptr;
    for(auto child=*reinterpret_cast<unsigned char**>(type+0x30);child;child=*reinterpret_cast<unsigned char**>(child+0x2c))if(!strcmp(ObjectName(child),"NativeTrigger"))function=child;
    if(!function)throw std::runtime_error("NativeTrigger fixture function missing.");
    // ProcessEvent suppresses script execution in editor mode. Exercise the
    // native thunk with a real FFrame expression stream in this disposable map.
    unsigned char code[8]={0x20,0,0,0,0,0x2a,static_cast<unsigned char>(activate?0x26:0x25),0x16};auto other=reinterpret_cast<A>(MagicActor(source));memcpy(code+1,&other,4);
    struct Frame{void* vtable;void* node;void* object;unsigned char* code;void* locals;void* previous;} frame{nullptr,function,actor,code,nullptr,nullptr};
    using Native=void(__thiscall*)(void*,void*,void*);reinterpret_cast<Native>(*reinterpret_cast<A*>(function+0x6c))(actor,&frame,nullptr);
    nlohmann::json result={{"queue",nlohmann::json::array()},{"groups",nlohmann::json::array()}};
    auto groups=*reinterpret_cast<unsigned char**>(actor+0x2f8);int count=*reinterpret_cast<int*>(actor+0x2fc);for(int i=0;i<count;++i)result["groups"].push_back({{"repeat",*reinterpret_cast<int*>(groups+i*24+12)},{"index",*reinterpret_cast<int*>(groups+i*24+16)}});
    auto queue=*reinterpret_cast<unsigned char**>(actor+0x304);count=*reinterpret_cast<int*>(actor+0x308);for(int i=0;i<count;++i)result["queue"].push_back({{"delay",*reinterpret_cast<float*>(queue+i*24+4)},{"due",*reinterpret_cast<float*>(queue+i*24+20)}});
    return result;
}
// Native reflection diagnostics for the game-specific event-group fixture.
nlohmann::json MagicSchema()
{
    using J=nlohmann::json;using A=uintptr_t;
    auto name=[](A object)->std::string {int index=*reinterpret_cast<int*>(object+0x20);auto names=*reinterpret_cast<A*>(0x1169cfbc);return reinterpret_cast<const char*>(*reinterpret_cast<A*>(names+index*4)+12);};
    A data=*reinterpret_cast<A*>(0x11697b70);int count=*reinterpret_cast<int*>(0x11697b74);J result=J::array();
    for(int i=0;i<count;++i)
    {
        A object=*reinterpret_cast<A*>(data+i*4);if(!object || name(object)!="SMagicEvent" || name(*reinterpret_cast<A*>(object+0x24))!="Class")continue;
        J cls={{"class",name(*reinterpret_cast<A*>(object+0x18))+"."+name(object)},{"properties",J::array()}};
        cls["functions"]=J::array();
        for(A child=*reinterpret_cast<A*>(object+0x30);child;child=*reinterpret_cast<A*>(child+0x2c))if(name(*reinterpret_cast<A*>(child+0x24))=="Function")
        {
            cls["functions"].push_back({{"name",name(child)},{"native",*reinterpret_cast<unsigned*>(child+0x6c)}});
        }
        for(A p=*reinterpret_cast<A*>(object+0x58);p;p=*reinterpret_cast<A*>(p+0x40))
        {
            J prop={{"name",name(p)},{"kind",name(*reinterpret_cast<A*>(p+0x24))},{"offset",*reinterpret_cast<int*>(p+0x3c)},{"stride",*reinterpret_cast<unsigned short*>(p+0x32)},{"dimension",*reinterpret_cast<unsigned short*>(p+0x30)}};
            if(name(p)=="Groups")
            {
                auto inner=*reinterpret_cast<A*>(p+0x64);prop["innerKind"]=name(*reinterpret_cast<A*>(inner+0x24));prop["innerStride"]=*reinterpret_cast<unsigned short*>(inner+0x32);
                auto structure=*reinterpret_cast<A*>(inner+0x64);prop["structure"]=name(structure);prop["fields"]=J::array();
                for(A f=*reinterpret_cast<A*>(structure+0x58);f;f=*reinterpret_cast<A*>(f+0x40))
                {
                    J field={{"name",name(f)},{"kind",name(*reinterpret_cast<A*>(f+0x24))},{"offset",*reinterpret_cast<int*>(f+0x3c)}};
                    if(name(f)=="EventGroup")
                    {
                        auto item=*reinterpret_cast<A*>(f+0x64);field["innerKind"]=name(*reinterpret_cast<A*>(item+0x24));field["innerStride"]=*reinterpret_cast<unsigned short*>(item+0x32);
                        if(field["innerKind"]=="StructProperty")
                        {
                            auto fields=*reinterpret_cast<A*>(item+0x64);field["structure"]=name(fields);field["fields"]=J::array();
                            // SuperEvent fields use UStruct.Children rather than the cached property chain.
                            for(A child=*reinterpret_cast<A*>(fields+0x30);child;child=*reinterpret_cast<A*>(child+0x2c))
                                field["fields"].push_back({{"name",name(child)},{"kind",name(*reinterpret_cast<A*>(child+0x24))},{"offset",*reinterpret_cast<int*>(child+0x3c)}});
                        }
                    }
                    prop["fields"].push_back(field);
                }
            }
            cls["properties"].push_back(prop);
        }
        result.push_back(cls);
    }
    return result;
}
std::string formName;
// Map Design combo choices for its route and workspace dialogs.
int routeTeamChoice=0,workspaceChoice=0;
bool cancelTagPreview=false;
std::string excludeTagEntity;
std::filesystem::path previewScreenshot;
void Screenshot(HWND window,const std::filesystem::path& path);
bool packagePreviewChecked=false;
std::wstring jsonDialogPath;
bool jsonDialogCancel=false;
bool authoringPreviewCancel=false,authoringPreviewSeen=false;
int jsonDialogTicks=0;
// Ticks are counted from the moment each file dialog appears, so an earlier
// form in the same command does not use up the count.
HWND jsonDialogWindow=nullptr;
void CALLBACK AnswerJsonDialog(HWND,UINT,UINT_PTR,DWORD)
{
    EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM)->BOOL {
        wchar_t title[256]{};GetWindowTextW(w,title,256);
        if(!wcscmp(title,L"Map JSON")){PostMessage(w,WM_COMMAND,GetDlgItem(w,IDOK)?IDOK:IDCANCEL,0);return TRUE;}
        if(!wcscmp(title,L"Preview Map JSON Import"))
        {
            authoringPreviewSeen=true;if(!previewScreenshot.empty())Screenshot(w,previewScreenshot);
            PostMessage(w,WM_COMMAND,authoringPreviewCancel?IDCANCEL:IDOK,0);return TRUE;
        }
        if(wcscmp(title,L"Choose Reference Image") && wcscmp(title,L"Export Map Design Workspace") && wcscmp(title,L"Import Map Design Workspace") && wcscmp(title,L"Export Map to JSON") && wcscmp(title,L"Import Map from JSON") && wcscmp(title,L"Export SMagicEvent as JSON") && wcscmp(title,L"Import JSON into the open SMagicEvent"))return TRUE;
        if(w!=jsonDialogWindow){jsonDialogWindow=w;jsonDialogTicks=0;}
        ++jsonDialogTicks;
        if(jsonDialogCancel || jsonDialogTicks>40){PostMessage(w,WM_COMMAND,IDCANCEL,0);return TRUE;}
        if(jsonDialogTicks!=2 || !IsWindowEnabled(w))return TRUE;
        SendMessageW(w,CDM_SETCONTROLTEXT,edt1,reinterpret_cast<LPARAM>(jsonDialogPath.c_str()));
        EnumChildWindows(w,[](HWND child,LPARAM)->BOOL {
            wchar_t cls[64]{};GetClassNameW(child,cls,64);
            if(!wcscmp(cls,L"Edit") && (GetDlgCtrlID(child)==1001 || GetDlgCtrlID(child)==edt1 || GetDlgCtrlID(GetParent(child))==cmb13 || GetDlgCtrlID(GetParent(GetParent(child)))==cmb13))SetWindowTextW(child,jsonDialogPath.c_str());
            return TRUE;
        },0);
        PostMessage(w,WM_COMMAND,IDOK,0);
        // Start counting again for the next dialog, even if Windows reuses
        // this handle; a dialog that stays open is simply answered again.
        jsonDialogWindow=nullptr;jsonDialogTicks=0;
        return TRUE;
    },0);
}
// File > Optimise Map Assets...: once its scan has filled the list, the rows,
// ticked rows and status line are read, the window is captured and closed.
int optimiseRows=-1,optimiseTicked=-1,optimiseTicks=0;std::string optimiseStatus;std::filesystem::path optimiseShot;
void CALLBACK InspectOptimiseDialog(HWND,UINT,UINT_PTR,DWORD)
{
    EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM)->BOOL {
        char title[256]{};GetWindowTextA(w,title,256);
        if(strcmp(title,"Optimise Map Assets"))return TRUE;
        auto list=GetDlgItem(w,100);
        const int rows=list?static_cast<int>(SendMessage(list,LVM_GETITEMCOUNT,0,0)):0;
        if(rows==0 && ++optimiseTicks<100)return FALSE; // Still scanning.
        optimiseRows=rows;optimiseTicked=0;
        for(int i=0;i<rows;++i)if(((SendMessage(list,LVM_GETITEMSTATE,i,LVIS_STATEIMAGEMASK)&LVIS_STATEIMAGEMASK)>>12)==2)++optimiseTicked;
        char status[1024]{};GetDlgItemTextA(w,102,status,sizeof(status));optimiseStatus=status;
        if(!optimiseShot.empty())Screenshot(w,optimiseShot);
        PostMessage(w,WM_COMMAND,IDCANCEL,0);
        return FALSE;
    },0);
}
HWND FindDialog(const char* title)
{
    struct Search { const char* title;HWND result; } search{title,nullptr};
    EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM p)->BOOL {
        auto s=reinterpret_cast<Search*>(p);char text[256]{};GetWindowTextA(w,text,256);
        if(!strcmp(text,s->title)){s->result=w;return FALSE;}return TRUE;
    },reinterpret_cast<LPARAM>(&search));return search.result;
}
void CALLBACK Answer(HWND,UINT,UINT_PTR,DWORD)
{
    EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM)->BOOL {
        char cls[100]{};GetClassNameA(w,cls,100);
        if(!strcmp(cls,"ReloadedWorkflowForm"))
        {
            char title[256]{};GetWindowTextA(w,title,256);
            if(strstr(title,"Save New") || strstr(title,"Save Selection") || strstr(title,"Rename"))SetWindowTextA(GetDlgItem(w,1000),formName.c_str());
            // Map Design route teams and workspace import/export use combo choices.
            if(strstr(title,"Plan Routes")){SetWindowTextA(GetDlgItem(w,1000),formName.c_str());SendMessage(GetDlgItem(w,1002),CB_SETCURSEL,routeTeamChoice,0);}
            if(strstr(title,"Map Design Workspace"))SendMessage(GetDlgItem(w,1000),CB_SETCURSEL,workspaceChoice,0);
            if(strstr(title,"Compare Route Timings"))
            {
                // The two most recent routes are this fixture's spy and merc routes.
                auto count=static_cast<int>(SendMessage(GetDlgItem(w,1000),CB_GETCOUNT,0,0));
                SendMessage(GetDlgItem(w,1000),CB_SETCURSEL,count-2,0);
                SendMessage(GetDlgItem(w,1001),CB_SETCURSEL,count-1,0);
            }
            if(strstr(title,"Edit Assembly Contents"))SendMessage(GetDlgItem(w,1000),CB_SETCURSEL,1,0);
            SendMessage(w,WM_COMMAND,IDOK,0);
        }
        if(!strcmp(cls,"#32770") && GetDlgItem(w,IDOK))
        {
            char title[256]{};GetWindowTextA(w,title,256);
            // File dialogs belong to AnswerJsonDialog. Pressing OK here as well
            // re-enters a dialog that is still handling the previous press.
            for(const char* owned:{"Choose Reference Image","Export Map Design Workspace","Import Map Design Workspace",
                                   "Export Map to JSON","Import Map from JSON","Export SMagicEvent as JSON"})
                if(!strcmp(title,owned))return TRUE;
            if(!strcmp(title,"Select saved playable map (save/build your edits first)"))
            {
                packagePreviewChecked=true;
                SendMessage(w,WM_COMMAND,IDCANCEL,0);
                return TRUE;
            }
            if(!strcmp(title,"Package Map for Sharing"))
            {
                if(IsWindowEnabled(GetDlgItem(w,103)))
                {
                    packagePreviewChecked=IsWindowEnabled(GetDlgItem(w,IDOK)) && ListView_GetItemCount(GetDlgItem(w,100))>0;
                    ListView_SetCheckState(GetDlgItem(w,100),0,FALSE);
                    packagePreviewChecked=packagePreviewChecked && ListView_GetCheckState(GetDlgItem(w,100),0);
                    if(!previewScreenshot.empty())Screenshot(w,previewScreenshot);
                    SendMessage(w,WM_COMMAND,IDCANCEL,0);
                }
                return TRUE;
            }
            if(strstr(title,"Tag") && !excludeTagEntity.empty())
            {
                auto list=GetDlgItem(w,1000);char controlClass[100]{};GetClassNameA(list,controlClass,100);
                if(!strcmp(controlClass,WC_LISTVIEWA))for(int row=0;row<ListView_GetItemCount(list);++row)
                {
                    char actor[2048]{};LVITEMA item{};item.iSubItem=0;item.pszText=actor;item.cchTextMax=sizeof(actor);SendMessageA(list,LVM_GETITEMTEXTA,row,reinterpret_cast<LPARAM>(&item));
                    if(excludeTagEntity==actor)ListView_SetCheckState(list,row,FALSE);
                }
                if(!previewScreenshot.empty())Screenshot(w,previewScreenshot);
            }
            SendMessage(w,WM_COMMAND,cancelTagPreview && strstr(title,"Tag")?IDCANCEL:IDOK,0);
        }
        return TRUE;
    },0);
}
void Click(HWND window,int command,const char* name="Native entry")
{
    formName=name;UINT_PTR timer=SetTimer(nullptr,0,50,Answer);SendMessage(window,WM_COMMAND,command,0);KillTimer(nullptr,timer);
}
void Screenshot(HWND window,const std::filesystem::path& path)
{
    RedrawWindow(window,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
    RECT r{};GetWindowRect(window,&r);int width=r.right-r.left,height=r.bottom-r.top;
    HDC screen=GetDC(window),dc=CreateCompatibleDC(screen);HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);auto old=SelectObject(dc,bitmap);
    PrintWindow(window,dc,2);SelectObject(dc,old);
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    std::vector<char> pixels(static_cast<size_t>(width)*height*4);GetDIBits(dc,bitmap,0,height,pixels.data(),&info,DIB_RGB_COLORS);
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+static_cast<DWORD>(pixels.size());
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&header),sizeof(header));out.write(reinterpret_cast<char*>(&info.bmiHeader),sizeof(info.bmiHeader));out.write(pixels.data(),pixels.size());
    DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(window,screen);
}
// MCP bridge section: the windows its bridge client closes, and the safety
// nets that close them when it could not, so a failed check cannot leave the
// editor waiting on a window nobody answers.
const wchar_t* const kMcpProbeBox=L"RE+ MCP probe: a modal dialog";
const wchar_t* const kMcpReviewClass=L"ReloadedMapAuthoringPreview";
const wchar_t* const kMcpReviewTitle=L"Approve a change from the MCP client";
int mcpReviewTicks=0;
void CALLBACK CloseMcpProbeBox(HWND,UINT,UINT_PTR,DWORD)
{
    if(HWND box=McpPipe::OwnWindow(L"#32770",kMcpProbeBox))PostMessageW(box,WM_COMMAND,IDOK,0);
}
void CALLBACK CancelStuckMcpReview(HWND,UINT,UINT_PTR,DWORD)
{
    HWND review=McpPipe::OwnWindow(kMcpReviewClass,kMcpReviewTitle);
    if(!review){mcpReviewTicks=0;return;}
    if(++mcpReviewTicks>90)PostMessageW(review,WM_COMMAND,MAKEWPARAM(IDCANCEL,BN_CLICKED),0);
}
// A PNG's size from its IHDR; false when the bytes are no PNG.
bool PngSize(const std::string& png,int& width,int& height)
{
    static const unsigned char signature[8]={0x89,'P','N','G','\r','\n',0x1a,'\n'};
    if(png.size()<24 || memcmp(png.data(),signature,8) || png.compare(12,4,"IHDR"))return false;
    auto word=[&](size_t at){uint32_t v=0;for(size_t i=0;i<4;++i)v=(v<<8)|static_cast<unsigned char>(png[at+i]);return v;};
    width=static_cast<int>(word(16));height=static_cast<int>(word(20));
    return width>0 && height>0;
}
// The title Undo History shows for the newest step Undo would take back
// (UTransBuffer at UEditorEngine+0x148: FTransaction records of 0x28 bytes at
// +0x28, Num +0x2c, UndoCount +0x34; the title FString at +0x14).
std::string NewestUndoTitle()
{
    auto editor=*reinterpret_cast<unsigned char**>(kEditor);
    auto trans=editor?*reinterpret_cast<unsigned char**>(editor+0x148):nullptr;
    if(!trans)return {};
    auto data=*reinterpret_cast<unsigned char**>(trans+0x28);
    const int count=*reinterpret_cast<int*>(trans+0x2c),undone=*reinterpret_cast<int*>(trans+0x34);
    if(!data || undone<0 || count-undone<1)return {};
    auto title=*reinterpret_cast<const char**>(data+(count-1-undone)*0x28+0x14);
    return title?title:"";
}
}
void RunWorkflowTests(HMODULE editorDll, const char* destination, bool restart=false, bool snapOnly=false, bool charactersOnly=false, bool stairsOnly=false, bool optimiseOnly=false, bool mcpOnly=false)
{
    using J=nlohmann::json;
    try
    {
        auto oldLibrary=directory/"ReloadedEditor"/"library.json";
        // A copied install's own libraries (hidden built-ins, a damaged file) must not change the counts.
        for(const auto* file:{"library","emitter_library"})
        {
            auto old=directory/"ReloadedEditor"/(std::string(file)+".json");
            if(!restart && std::filesystem::exists(old)) std::filesystem::rename(old,directory/"ReloadedEditor"/(std::string(file)+"-before-"+std::to_string(GetTickCount64())+".json"));
        }
        using Request=int(__cdecl*)(const char*,char*,unsigned);
        auto request=reinterpret_cast<Request>(GetProcAddress(editorDll,"ReloadedWorkflowRequest"));
        if(!request) request=reinterpret_cast<Request>(GetProcAddress(editorDll,"_ReloadedWorkflowRequest"));
        if(!request) throw std::runtime_error("Workflow API export missing.");
        auto call=[&](J q)->J {
            Record("workflow",q.at("op").get<std::string>().c_str());
            std::vector<char> buffer(8*1024*1024);auto text=q.dump();int ok=request(text.c_str(),buffer.data(),static_cast<unsigned>(buffer.size()));
            if(ok<0) {buffer.resize(-ok);ok=request(text.c_str(),buffer.data(),static_cast<unsigned>(buffer.size()));}
            J result=J::parse(buffer.data());if(!ok || !result.at("ok").get<bool>()) throw std::runtime_error(result.value("error","Request failed"));
            return result.at("result");
        };
        // A failed check is recorded and the run goes on, so one run reports
        // every regression; the section then fails at its end. Only an
        // exception (a request that errors, missing data) still stops it.
        int failures=0;std::string firstFailure;
        auto require=[&](bool ok,const char* text)
        {
            if(!ok){++failures;if(firstFailure.empty())firstFailure=text;Record("workflow_fail",text);return;}
            Record("workflow_check",text);
        };
        auto finish=[&](const char* pass)
        {
            if(failures>0)throw std::runtime_error(std::to_string(failures)+" check(s) failed; first: "+firstFailure);
            Record("PASS",pass);
        };
        // Character Skins 3D preview: a spy and a merc in a private level and viewport that
        // never reach the map. The captures are kept beside the report for a look by eye.
        auto characterPreview=[&]()
        {
            auto beforeActors=call({{"op","actors"}});auto undoBefore=call({{"op","characters.state"}})["undo"];
            auto opened=call({{"op","characters.open"}});
            require(opened["parentIsHost"]==true && opened["fillsHost"]==true && opened["renDev"]==true && opened["cameraInPreviewLevel"]==true && opened["rendMap"]==5,
                "character preview viewport fills its host and looks into its own level");
            auto stock=call({{"op","characters.show"}});Record("character_preview_stock",stock.dump().c_str());
            const auto& figures=stock["state"]["figures"];
            require(stock["state"]["faulted"]==false && figures.size()==2 && figures[0]["instance"]=="SkeletalMeshInstance" && figures[1]["instance"]=="SkeletalMeshInstance"
                && figures[0]["outer"]=="ReloadedCharacterPreview" && figures[0]["mesh"]=="SPerso.ATT_01" && figures[1]["mesh"]=="SPerso.DEF_01",
                "both stock characters stand in the preview package as skeletal meshes");
            require(!stock["poses"].value("Spy",std::string()).empty() && !stock["poses"].value("Merc",std::string()).empty(),"both characters hold a pose from their team's animations");
            auto stockFrame=call({{"op","characters.capture"},{"path",(directory/"character_preview_stock.bmp").string()}});Record("character_preview_stock_frame",stockFrame.dump().c_str());
            require(stockFrame["nonBlackPixels"].get<int>()>2000,"the stock characters draw in the preview");
            // The merc's body layer on the spy: a stock material, so always loaded, and a different look.
            auto dressed=call({{"op","characters.show"},{"slots",{{"SpyBody","SPersoTextures.alpha_DEF01.DEF01_heat_bodu"}}}});Record("character_preview_dressed",dressed.dump().c_str());
            auto dressedFrame=call({{"op","characters.capture"},{"path",(directory/"character_preview_dressed.bmp").string()}});
            require(dressed["warnings"].empty() && dressedFrame["nonBlackPixels"].get<int>()>2000 && dressedFrame["checksum"]!=stockFrame["checksum"],"a slot material changes the spy's look");
            // The spy's own stock model chosen in the box is no model: the slot is still worn.
            auto ownModel=call({{"op","characters.show"},{"slots",{{"SpyBody","SPersoTextures.alpha_DEF01.DEF01_heat_bodu"}}},{"models",{{"SpyModel","SPerso.ATT_01"}}}});
            auto ownModelFrame=call({{"op","characters.capture"}});
            require(ownModel["warnings"].empty() && ownModelFrame["checksum"]==dressedFrame["checksum"],"the team's own stock model keeps its slot materials");
            auto unused=call({{"op","characters.show"},{"slots",{{"SpyBody","SPersoTextures.alpha_DEF01.DEF01_heat_bodu"}}},{"models",{{"SpyModel","SPerso.DEF_01"}}}});
            require(unused["warnings"].size()==1,"a slot under another model is reported as not worn");
            auto model=call({{"op","characters.show"},{"models",{{"SpyModel","SPerso.DEF_01"}}}});Record("character_preview_model",model.dump().c_str());
            require(model["state"]["figures"][0]["mesh"]=="SPerso.DEF_01" && !model["poses"].value("Spy",std::string()).empty(),"a spy in the merc model holds a spy pose");
            call({{"op","characters.capture"},{"path",(directory/"character_preview_model.bmp").string()}});
            bool refused=false;try{call({{"op","characters.show"},{"slots",{{"MercHead","NoSuchPackage.Missing"}}}});}catch(const std::exception&){refused=true;}
            require(refused && call({{"op","characters.state"}})["figures"].size()==2,"a missing material is refused and the previous look stays");
            call({{"op","characters.show"}});
            call({{"op","characters.camera"},{"camera",{{"yaw",20000},{"pitch",-2000}}}});
            call({{"op","characters.capture"},{"path",(directory/"character_preview_turned.bmp").string()}});
            Exec("OBJ GARBAGE");auto collected=call({{"op","characters.state"}});
            require(collected["level"]==true && collected["viewport"]==true && collected["faulted"]==false && collected["figures"].size()==2,"character preview survives garbage collection");
            require(call({{"op","actors"}})==beforeActors && collected["undo"]==undoBefore,"character preview leaves map actors and Undo history untouched");
            auto closed=call({{"op","characters.close"}});
            require(closed["viewport"]==false && closed["figures"].empty() && closed["liveActorClasses"].value("SAnimatedMesh",0)==0,"closing the preview removes its characters and viewport");
            auto reopened=call({{"op","characters.open"}});
            require(reopened["viewport"]==true && reopened["viewportsOpened"].get<int>()==closed["viewportsOpened"].get<int>()+1,"reopening the character preview opens a new viewport");
            call({{"op","characters.close"}});
        };
        // Character Skins on the map: Apply compiles SCharacterOutfits into the map
        // package and places one actor of it, named like any other actor.
        auto characterSkinsActor=[&]()
        {
            const auto beforeActors=call({{"op","actors"}});
            auto skins=[&]()
            {
                J found=J::array();
                for(auto& a:call({{"op","actors"}}))if(a.at("class").get<std::string>().find("CharacterSkins")!=std::string::npos)found.push_back(a);
                Record("character_skins_actor",found.dump().c_str());
                return found;
            };
            auto applied=call({{"op","skins.apply"},{"slots",{{"SpyBody","SPersoTextures.alpha_DEF01.DEF01_heat_bodu"}}}});
            require(applied["placed"]==true && applied["compiled"]==true && applied["legacy"]==false && applied["extra"]==false,"Apply compiles the class and places one actor");
            require(applied["slots"]["SpyBody"]=="SPersoTextures.alpha_DEF01.DEF01_heat_bodu","Apply sets the slot");
            auto first=skins();
            require(first.size()==1 && first[0]["class"]=="MyLevel.SCharacterOutfits","the actor's class is SCharacterOutfits");
            require(first.size()==1 && first[0]["name"]=="SCharacterOutfits0","the actor is named SCharacterOutfits0");
            auto again=call({{"op","skins.apply"},{"slots",{{"MercBody","SPersoTextures.alpha_DEF01.DEF01_heat_bodu"}}}});
            auto second=skins();
            require(again["slots"]["MercBody"]=="SPersoTextures.alpha_DEF01.DEF01_heat_bodu" && again["slots"]["SpyBody"]=="" && second==first,"applying again edits the same renamed actor");
            auto removed=call({{"op","skins.remove"}});
            require(removed["placed"]==false && call({{"op","actors"}}).size()==beforeActors.size(),"Remove from Map deletes the actor");
            call({{"op","skins.apply"}});
            auto third=skins();
            const auto name=third.size()==1?third[0]["name"].get<std::string>():std::string();
            require(name.size()==18 && name.rfind("SCharacterOutfits",0)==0,"after a removal the new actor takes the lowest free number");
            call({{"op","skins.remove"}});
            require(call({{"op","actors"}}).size()==beforeActors.size(),"the map is back as it was");
        };
        if(charactersOnly){characterPreview();characterSkinsActor();finish("character skins 3D preview and map actor");return;}
        // Smooth Staircase: a spiral of a brush per step (Map Design) and a
        // straight flight in one brush (as the linear builder adds it) each get
        // one invisible BlockingVolume ramp with its own collision BSP, from the
        // actor menu's command; smoothing again replaces it, Remove deletes it,
        // and each is one Undo step (the frame's Undo and Redo, which refile
        // colliding actors in the collision hash).
        auto stairSmoothing=[&]()
        {
            // A map opened from the frame has a collision hash (ULevel+0x3a4c) and
            // the volume must be filed in it; the generated fixture has none.
            auto level=*reinterpret_cast<unsigned char**>(*reinterpret_cast<unsigned char**>(kEditor)+0x130);
            using SetCollision=void(__thiscall*)(void*,int,int);
            const bool hadHash=*reinterpret_cast<void**>(level+0x3a4c)!=nullptr;
            if(!hadHash)reinterpret_cast<SetCollision>(0x1111eb30)(level,1,0);
            require(*reinterpret_cast<void**>(level+0x3a4c)!=nullptr,"the level has a collision hash, as a map opened from the frame does");
            const auto beforeActors=call({{"op","actors"}});
            auto ramps=[&]()
            {
                J found=J::array();
                for(auto& a:call({{"op","actors"}}))if(a.at("class")=="Engine.BlockingVolume" && a.at("name").get<std::string>().rfind("StairRamp_",0)==0)found.push_back(a);
                return found;
            };
            auto rampPolygons=[&](const J& ramp)
            {
                auto native=WorkflowProbe::MagicActor(ramp);
                auto model=*reinterpret_cast<unsigned char**>(native+0x238);
                auto polys=model?*reinterpret_cast<unsigned char**>(model+0x50):nullptr;
                require(model && *reinterpret_cast<int*>(model+0x58)>0,"the ramp volume has its own collision BSP");
                return polys?*reinterpret_cast<int*>(polys+0x2c):0;
            };
            // Walks a ramp's own BSP from the root for a world point (nodes: plane
            // at +0, then UE2's iBack +0x30 before iFront +0x34, stride 0x5c; the
            // volume has no rotation or pivot): ending behind a plane is solid.
            auto solidAt=[&](const J& ramp,double x,double y,double z)
            {
                auto native=WorkflowProbe::MagicActor(ramp);const auto at=reinterpret_cast<float*>(native+0x80);
                auto model=*reinterpret_cast<unsigned char**>(native+0x238);
                auto nodes=*reinterpret_cast<unsigned char**>(model+0x54);const int count=*reinterpret_cast<int*>(model+0x58);
                x-=at[0];y-=at[1];z-=at[2];
                int node=0,guard=0;bool front=true;
                while(node>=0 && node<count && guard++<100000)
                {
                    auto n=nodes+node*0x5c;auto f=reinterpret_cast<float*>(n);
                    front=f[0]*x+f[1]*y+f[2]*z-f[3]>=0;node=*reinterpret_cast<int*>(n+(front?0x34:0x30));
                }
                return !front;
            };
            using Load=HMENU(WINAPI*)(HINSTANCE,LPCSTR);
            auto menuHas=[&](UINT command)
            {
                auto menu=(*reinterpret_cast<Load*>(0x11af23f0))(GetModuleHandle(nullptr),MAKEINTRESOURCEA(107));
                const bool has=menu && GetMenuState(GetSubMenu(menu,0),command,MF_BYCOMMAND)!=UINT(-1);
                if(menu)DestroyMenu(menu);
                return has;
            };
            auto names=[](const J& list){std::set<std::string> s;for(auto& a:list)s.insert(a.at("path").get<std::string>());return s;};
            constexpr UINT smooth=41385,remove=41386; // WorkflowTools::kSmoothStairs, kRemoveStairSmoothing
            const J spec={{"kind","Spiral"},{"construction","Carve"},{"width",384},{"length",384},{"height",272},{"thickness",16},{"steps",17},{"ceiling",false},{"portal",false},{"name","Stair smoothing spiral"}};
            auto spiral=call({{"op","design.block"},{"spec",spec},{"position",{6144,6144,0}},{"rotation",{0,0,0}}});
            const auto members=spiral.at("members");
            auto state=call({{"op","stairs.state"}});Record("stairs_spiral_state",state.dump().c_str());
            require(state["stairs"]==true && state["winding"]==true && state["steps"]==17 && state["ramps"]==0,"a Map Design spiral reads as a winding 17-step staircase");
            require(menuHas(smooth) && !menuHas(remove),"the actor menu offers Smooth Staircase for a selected staircase");
            SendMessage(frameWindow,WM_COMMAND,smooth,0);
            const auto first=ramps();
            require(first.size()==1 && call({{"op","actors"}}).size()==beforeActors.size()+members.size()+1,"Smooth Staircase adds one ramp volume");
            if(first.size()==1)require(rampPolygons(first[0])>17*2,"the spiral ramp is one helical solid");
            if(first.size()==1)
            {
                // Halfway between the 5th and 6th step edges at mid-radius the ramp top
                // is 88 up (treads rise 16; the first edge is 16 up at angle 0), and it
                // is 16 thick.
                const double angle=3.14159265358979323846*2*4.5/17,x=6144+120*std::cos(angle),y=6144+120*std::sin(angle);
                require(solidAt(first[0],x,y,80) && !solidAt(first[0],x,y,100) && !solidAt(first[0],x,y,60) && !solidAt(first[0],6144,6144,80),"the spiral ramp is solid under its top and open above, below and at the post");
            }
            require(names(call({{"op","actors"},{"selected",true}}))==names(members),"smoothing keeps the staircase selected");
            require(menuHas(smooth) && menuHas(remove),"a smoothed staircase offers Re-smooth and Remove");
            auto again=call({{"op","stairs.smooth"}});Record("stairs_resmooth",again.dump().c_str());
            const auto second=ramps();
            require(again["replaced"]==1 && second.size()==1 && names(second)!=names(first),"smoothing again replaces the ramp");
            SendMessage(frameWindow,WM_COMMAND,40019,0);require(names(ramps())==names(first),"Undo brings back the earlier ramp");
            SendMessage(frameWindow,WM_COMMAND,40019,0);require(ramps().empty(),"Undo removes the first ramp in one step");
            SendMessage(frameWindow,WM_COMMAND,40020,0);require(names(ramps())==names(first),"Redo smooths again");
            if(!ramps().empty())rampPolygons(ramps()[0]);
            require(call({{"op","stairs.remove"}})==1 && ramps().empty(),"Remove Staircase Smoothing deletes the ramp");
            SendMessage(frameWindow,WM_COMMAND,40019,0);require(ramps().size()==1,"Remove undoes in one step");
            SendMessage(frameWindow,WM_COMMAND,40019,0);require(ramps().empty(),"back to the bare spiral");
            SendMessage(frameWindow,WM_COMMAND,40019,0);require(call({{"op","actors"}}).size()==beforeActors.size(),"the spiral undoes");
            // A straight flight in one brush: eight 128 x 32 x 16 steps.
            std::ostringstream text;
            text<<"Begin Actor Class=Engine.Brush Name=Flight\nCsgOper=CSG_Add\nBegin Brush Name=FlightModel\nBegin PolyList\n";
            const int corners[6][4]={{1,3,7,5},{0,4,6,2},{2,6,7,3},{0,1,5,4},{4,5,7,6},{0,2,3,1}};
            for(int i=0;i<8;++i)
            {
                const double lo[3]={-64,32.0*i,0},hi[3]={64,32.0*(i+1),16.0*(i+1)};
                for(auto& face:corners)
                {
                    text<<"Begin Polygon\n";
                    for(int c:face)text<<"Vertex "<<(c&1?hi[0]:lo[0])<<","<<(c&2?hi[1]:lo[1])<<","<<(c&4?hi[2]:lo[2])<<"\n";
                    text<<"End Polygon\n";
                }
            }
            text<<"End PolyList\nEnd Brush\nBrush=Model'MyLevel.FlightModel'\nEnd Actor\n";
            const J definition={{"id","flight"},{"actors",J::array({{{"name","Flight"},{"class","Engine.Brush"},{"path","MyLevel.Flight"},{"text",text.str()},{"position",{0,0,0}},{"rotation",{0,0,0}},{"tag","None"},{"event","None"}}})},{"bindings",J::array()},{"dependencies",J::array()}};
            auto placed=call({{"op","assembly.place"},{"definition",definition},{"position",{-6144,6144,0}},{"rotation",{0,0,0}},{"bindings",J::object()}});
            call({{"op","select"},{"actors",placed.at("members")}});
            state=call({{"op","stairs.state"}});Record("stairs_flight_state",state.dump().c_str());
            require(state["stairs"]==true && state["winding"]==false && state["steps"]==8,"a one-brush straight flight reads as an 8-step staircase");
            call({{"op","stairs.smooth"}});
            const auto slab=ramps();
            require(slab.size()==1 && rampPolygons(slab[0])==6,"a straight flight gets one flat slab");
            if(slab.size()==1)
            {
                // The slab is centred on its own location and 16 thick: its middle is
                // solid, the space 40 above and below it and far away is not.
                const auto at=reinterpret_cast<float*>(WorkflowProbe::MagicActor(slab[0])+0x80);
                // It starts at the floor (z 0) a step in front of the first riser, so
                // the bottom step is smoothed too: its lowest corner is a rise (16)
                // below the floor.
                {
                    auto model=*reinterpret_cast<unsigned char**>(WorkflowProbe::MagicActor(slab[0])+0x238);
                    auto polys=*reinterpret_cast<unsigned char**>(model+0x50);
                    auto data=*reinterpret_cast<unsigned char**>(polys+0x28);const int count=*reinterpret_cast<int*>(polys+0x2c);
                    float lowest=1e9f;
                    for(int i=0;i<count;++i){auto poly=data+i*0x14c;const int n=*reinterpret_cast<unsigned short*>(poly+0x148);for(int k=0;k<n;++k){const float z=reinterpret_cast<float*>(poly+0x18)[k*3+2]+at[2];if(z<lowest)lowest=z;}}
                    Record("stairs_flight_lowest",std::to_string(lowest).c_str());
                    require(std::abs(lowest+16)<0.5f,"the flight ramp starts at the floor, so the bottom step is smoothed");
                }
                require(solidAt(slab[0],at[0],at[1],at[2]) && !solidAt(slab[0],at[0],at[1],at[2]+40) && !solidAt(slab[0],at[0],at[1],at[2]-40) && !solidAt(slab[0],at[0]+5000,at[1],at[2]),"the flight ramp is solid inside and open outside");
            }
            SendMessage(frameWindow,WM_COMMAND,40019,0);SendMessage(frameWindow,WM_COMMAND,40019,0);
            require(call({{"op","actors"}}).size()==beforeActors.size(),"the flight undoes");
            // Not a staircase: nothing offered.
            auto plain=std::find_if(beforeActors.begin(),beforeActors.end(),[](const J& a){return a.at("class")=="Engine.Brush" && a.value("authorable",false);});
            if(plain!=beforeActors.end())
            {
                call({{"op","select"},{"actors",J::array({*plain})}});
                require(call({{"op","stairs.state"}})["stairs"]==false && !menuHas(smooth),"a plain brush offers no smoothing");
            }
            call({{"op","select"},{"actors",J::array()}});
            if(!hadHash)reinterpret_cast<SetCollision>(0x1111eb30)(level,0,1);
        };
        if(stairsOnly){stairSmoothing();finish("Smooth Staircase");return;}
        // Optimise Map Assets: a mesh from a small pack (in a group, using a material
        // from a texture pack) is reported with its size, and release copies carry it
        // inside the map or in a package of their own; the working map keeps using the
        // packs, and each copy loads without them.
        auto optimiseAssets=[&]()
        {
            const std::string mesh="EST_STM.STM.GAR_aeration_E";
            auto fold=[](std::string s){for(auto& c:s)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return s;};
            auto same=[&](const J& value,const std::string& text){return value.is_string() && fold(value.get<std::string>())==fold(text);};
            const J definition={{"id","optimise"},{"actors",J::array({{{"name","OptimiseMesh"},{"class","Engine.StaticMeshActor"},{"path","MyLevel.OptimiseMesh"},
                {"text","Begin Actor Class=StaticMeshActor Name=OptimiseMesh\nStaticMesh=StaticMesh'"+mesh+"'\nEnd Actor\n"},{"position",{0,0,64}},{"rotation",{0,0,0}},{"tag","None"},{"event","None"}}})},
                {"bindings",J::array()},{"dependencies",J::array({mesh})}};
            call({{"op","assembly.place"},{"definition",definition},{"position",{0,0,64}},{"rotation",{0,0,0}},{"bindings",J::object()}});
            const auto actorCount=call({{"op","actors"}}).size();
            auto report=call({{"op","optimise.report"}});
            Record("optimise_report",report.dump().substr(0,4000).c_str());
            auto pack=[](const J& r,const std::string& name){for(auto& p:r.at("packs"))if(p.at("name")==name)return p;return J();};
            auto stm=pack(report,"EST_STM");
            bool listed=false;
            if(!stm.is_null())for(auto& a:stm.at("assets"))if(same(a.at("path"),mesh) && a.at("found")==true && a.at("size")==22577)listed=true;
            require(listed && stm.value("fileSize",0)==61088 && stm.value("usedSize",0)==22577,"the report lists the mesh with its size and its package's");
            require(!pack(report,"EST_TXT").is_null(),"the mesh's material's package is reported too");
            // The generated fixture was saved without the frame, which leaves it no file name.
            if(report.at("mapFile").get<std::string>().empty() && !stm.is_null())
                call({{"op","map.setfile"},{"file",(std::filesystem::path(stm.at("file").get<std::string>()).parent_path().parent_path()/"MapsEd"/"NativeFixture.sdc").string()}});
            const auto mapFile=call({{"op","map.file"}}).get<std::string>();
            {
                WorkflowProbe::optimiseShot=directory/"optimise_dialog.bmp";
                auto timer=SetTimer(nullptr,0,100,WorkflowProbe::InspectOptimiseDialog);
                SendMessage(frameWindow,WM_COMMAND,41390,0); // File > Optimise Map Assets...
                KillTimer(nullptr,timer);
                Record("optimise_dialog",(std::to_string(WorkflowProbe::optimiseRows)+" rows, "+std::to_string(WorkflowProbe::optimiseTicked)+" ticked: "+WorkflowProbe::optimiseStatus).c_str());
                require(WorkflowProbe::optimiseRows==static_cast<int>(report.at("packs").size()) && WorkflowProbe::optimiseTicked>=1 && WorkflowProbe::optimiseStatus.rfind("Ticked:",0)==0,
                        "the File menu opens the window with a row per package and the suggested ones ticked");
            }
            const auto mapName=report.at("map").get<std::string>();
            // Release copies keep the map's name (the game needs it), each in a
            // folder of its own laid out like the game's.
            const auto game=std::filesystem::path(mapFile).parent_path().parent_path().parent_path();
            const auto folderA=game/"Releases"/"NativeFixtureA",folderB=game/"Releases"/"NativeFixtureB";
            auto inside=call({{"op","optimise.release"},{"packs",{"EST_STM","EST_TXT"}},{"destination","map"},{"folder",folderA.string()},{"overwrite",true}});
            Record("optimise_inside",inside.dump().substr(0,4000).c_str());
            bool movedMesh=false;for(auto& m:inside.at("moved"))if(same(m.at("from"),mesh) && same(m.at("to"),mapName+".EST_STM.STM.GAR_aeration_E"))movedMesh=true;
            bool needsPack=false;for(auto& n:inside.at("needs"))if(n.at("name")=="EST_STM" || n.at("name")=="EST_TXT")needsPack=true;
            const auto releaseFile=folderA/"Packages"/"Maps"/(std::filesystem::path(mapFile).stem().string()+".sdc");
            require(movedMesh && inside.at("written").size()>=2 && inside.at("left").empty() && !needsPack && std::filesystem::exists(releaseFile) &&
                    std::filesystem::exists(folderA/"Packages"/"MapsEd"/releaseFile.filename()) && inside.at("playable").get<std::string>()==releaseFile.string(),
                    "the release copy, under the map's name in its folder's MapsEd and Maps, carries the mesh and its material and needs neither pack");
            auto after=call({{"op","optimise.report"}});
            require(after.at("mapFile")==mapFile && !mapFile.empty() && !pack(after,"EST_STM").is_null() && pack(after,"EST_STM").at("assets")==stm.at("assets"),"the working map still uses the packs, as before");
            require(call({{"op","actors"}}).size()==actorCount,"the working map's actors are untouched");
            auto own=call({{"op","optimise.release"},{"packs",{"EST_STM","EST_TXT"}},{"destination","package"},{"folder",folderB.string()},{"assetPackage","NativeFixture_Assets"},{"overwrite",true}});
            Record("optimise_package",own.dump().substr(0,4000).c_str());
            bool needsOwn=false;needsPack=false;
            for(auto& n:own.at("needs")){if(n.at("name")=="NativeFixture_Assets")needsOwn=true;if(n.at("name")=="EST_STM" || n.at("name")=="EST_TXT")needsPack=true;}
            require(own.at("written").size()>=3 && own.at("left").empty() && needsOwn && !needsPack,"the second copy takes its assets from its own package only");
            require(!pack(call({{"op","optimise.report"}}),"EST_STM").is_null(),"the working map is back on the packs");
            // Each copy loads and keeps the mesh without the packs' assets. A saved map
            // drops editor-only actors, so the mesh actor is looked up by name.
            const auto maps=std::filesystem::path(mapFile).parent_path();
            Exec("MAP LOAD FILE=\""+(folderA/"Packages"/"MapsEd"/releaseFile.filename()).string()+"\"");
            auto loaded=call({{"op","optimise.report"}});
            Record("optimise_loaded",loaded.dump().substr(0,2000).c_str());
            bool carried=false;for(auto& a:loaded.at("inside"))if(same(a.at("path"),loaded.at("map").get<std::string>()+".EST_STM.STM.GAR_aeration_E"))carried=true;
            require(carried && pack(loaded,"EST_STM").is_null() && pack(loaded,"EST_TXT").is_null(),"the release copy loads with the mesh inside it");
            // Its asset package is read from the game's folders, as it is once installed.
            const auto installedAssets=game/"Packages"/"StaticMeshes"/"NativeFixture_Assets.usx";
            std::filesystem::copy_file(folderB/"Packages"/"StaticMeshes"/"NativeFixture_Assets.usx",installedAssets,std::filesystem::copy_options::overwrite_existing);
            Exec("MAP LOAD FILE=\""+(folderB/"Packages"/"MapsEd"/releaseFile.filename()).string()+"\"");
            auto loaded2=call({{"op","optimise.report"}});
            Record("optimise_loaded2",loaded2.dump().substr(0,2000).c_str());
            auto assets=pack(loaded2,"NativeFixture_Assets");
            bool fromOwn=false;if(!assets.is_null())for(auto& a:assets.at("assets"))if(same(a.at("path"),"NativeFixture_Assets.EST_STM.STM.GAR_aeration_E") && a.at("found")==true)fromOwn=true;
            require(fromOwn && pack(loaded2,"EST_STM").is_null(),"the second copy loads its mesh from its own package");
            {std::error_code error;std::filesystem::remove(installedAssets,error);}
            // A full-size case when the runner copies one in (-OptimiseExtraMap), e.g. a
            // CoD4 port map: every pack it uses is moved into the release copy.
            const auto big=maps/"COD4_shipment.sdc";
            if(std::filesystem::exists(big))
            {
                Exec("MAP LOAD FILE=\""+big.string()+"\"");
                call({{"op","map.setfile"},{"file",big.string()}});
                auto started=GetTickCount64();
                auto bigReport=call({{"op","optimise.report"}});
                const auto scanTime=GetTickCount64()-started;
                J ticked=J::array();std::string rows;
                for(auto& p:bigReport.at("packs"))
                {
                    rows+=p.at("name").get<std::string>()+" "+std::to_string(p.at("assets").size())+" assets "+std::to_string(p.at("usedSize").get<std::uint64_t>()/1024)+" of "+std::to_string(p.at("fileSize").get<std::uint64_t>()/1024)+" KB"+(p.at("suggested")==true?" suggested":"")+"; ";
                    ticked.push_back(p.at("name")); // Every pack: the most there is to move.
                }
                Record("optimise_big_report",(std::to_string(scanTime)+" ms: "+rows).c_str());
                require(!bigReport.at("packs").empty(),"the big map's packs are reported");
                if(!ticked.empty())
                {
                    started=GetTickCount64();
                    auto bigRelease=call({{"op","optimise.release"},{"packs",ticked},{"destination","map"},{"overwrite",true}});
                    const auto releaseTime=GetTickCount64()-started;
                    std::string summary=std::to_string(releaseTime)+" ms, rounds "+std::to_string(bigRelease.at("rounds").get<int>())+", moved "+std::to_string(bigRelease.at("moved").size())+", left "+std::to_string(bigRelease.at("left").size())+", written: ";
                    for(auto& f:bigRelease.at("written"))summary+=f.at("file").get<std::string>()+" "+std::to_string(f.at("size").get<std::uint64_t>()/1024)+" KB; ";
                    summary+="needs: ";for(auto& n:bigRelease.at("needs"))summary+=n.at("name").get<std::string>()+" ";
                    Record("optimise_big_release",summary.c_str());
                    bool needsTicked=false;for(auto& n:bigRelease.at("needs"))for(auto& t:ticked)if(n.at("name")==t)needsTicked=true;
                    require(bigRelease.at("left").empty() && !needsTicked,"the big map's release copy needs none of the ticked packs");
                    auto again=call({{"op","optimise.report"}});
                    require(again.at("packs").size()==bigReport.at("packs").size(),"the big map is back on its packs");
                }
            }
        };
        if(optimiseOnly){optimiseAssets();finish("Optimise Map Assets");return;}
        // MCP bridge (tests/McpBridgeNativeNotes.md): the probe is the client
        // Reloaded.Editor.Mcp.exe would be, on the editor's own pipe, working
        // from a thread of its own while this, the frame's thread, runs the
        // calls. It starts from a new map, so it needs no fixture.
        auto mcpBridge=[&]()
        {
            namespace B=Mcp::Bridge;
            constexpr UINT enableServer=41460,requireApproval=41461,disconnectClient=41463; // McpBridge::kEnable, kRequireApproval, kDisconnect
            const auto ini=(directory/"Reloaded_Editor.ini").string();
            auto setting=[&](const char* key){return GetPrivateProfileIntA("Mcp",key,0,ini.c_str())!=0;};
            auto lower=[](std::string text){for(auto& c:text)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return text;};
            auto pick=[](const J& value,const char* pointer){const J::json_pointer at(pointer);return value.contains(at)?value.at(at):J();};
            const DWORD pid=GetCurrentProcessId();
            const bool enabledAtStart=setting("Enabled");
            if(!enabledAtStart)SendMessageW(frameWindow,WM_COMMAND,enableServer,0);
            require(setting("Enabled"),"RE+ Tools > MCP > Enable MCP server writes [Mcp] Enabled=1");
            if(setting("RequireApproval"))SendMessageW(frameWindow,WM_COMMAND,requireApproval,0);
            McpPipe::Client client;McpPipe::Worker worker;
            auto onWorker=[&](std::function<void()> job,DWORD budget=600000){worker.Run(std::move(job),budget);};
            // This thread pumps messages for that long: windows paint, timers run.
            auto pump=[&](DWORD ms){onWorker([ms]{Sleep(ms);});};
            // One call and its whole reply, ok or not.
            auto bridge=[&](const std::string& op,const J& args=J::object(),const J& options=J::object(),DWORD timeout=180000)
            {
                Record("workflow",("mcp "+op).c_str());
                J reply;onWorker([&]{reply=client.Call(op,args,options,timeout);},timeout+60000);
                return reply;
            };
            // One call that must succeed: its result.
            auto run=[&](const std::string& op,const J& args=J::object(),const J& options=J::object(),DWORD timeout=180000)
            {
                const auto reply=bridge(op,args,options,timeout);
                if(!McpPipe::Ok(reply))throw std::runtime_error(op+" over the bridge failed: "+reply.dump().substr(0,3000));
                return reply.at("result");
            };
            auto paths=[&]{std::set<std::string> found;for(const auto& a:call({{"op","actors"}}))found.insert(a.at("path").get<std::string>());return found;};
            auto members=[](const J& piece){std::set<std::string> found;if(piece.is_object())for(const auto& m:piece.value("members",J::array()))found.insert(m.at("path").get<std::string>());return found;};
            auto within=[](const std::set<std::string>& part,const std::set<std::string>& whole){return !part.empty() && std::includes(whole.begin(),whole.end(),part.begin(),part.end());};
            auto apart=[](const std::set<std::string>& part,const std::set<std::string>& whole){for(const auto& p:part)if(whole.count(p))return false;return !part.empty();};
            auto room=[](const std::string& name){return J{{"kind","Room"},{"construction","Carve"},{"width",512},{"length",512},{"height",256},{"thickness",16},{"ceiling",true},{"portal",false},{"name",name}};};
            auto block=[&](const std::string& name,int x,int y){return J{{"spec",room(name)},{"position",{x,y,0}},{"rotation",{0,0,0}}};};
            auto undoTitle=[](unsigned n,const std::string& label){return "MCP "+std::to_string(n)+": "+label;};

            // Handshake: nothing before hello, the same protocol major or nothing, one client.
            J early,mismatch,hello,second;
            onWorker([&]
            {
                client.Open(pid,20000);
                early=client.Wait(client.Post("editor.state"),10000);
                auto newer=B::Hello("native-probe","1");newer["protocol"]["major"]=B::kProtocolMajor+8;
                client.Send(newer);mismatch=client.Wait(0,10000);
                hello=client.Hello("native-probe","1",10000);
            });
            require(McpPipe::Code(early)=="protocol_mismatch","a call before hello is refused (protocol_mismatch)");
            require(McpPipe::Code(mismatch)=="protocol_mismatch","a hello of another protocol major is refused and the pipe stays open");
            require(hello.value("t",std::string())=="hello" && hello.value("pid",0ul)==pid && hello.value("enabled",false),"hello names the editor's process and says the server is on");
            require(hello.value("ops",J())==B::OpList(),"hello lists the policy table's ops, none of them blocked");
            onWorker([&]{McpPipe::Client other;other.Open(pid,10000);second=other.Wait(0,10000);});
            require(McpPipe::Code(second)=="busy" && McpPipe::Reason(second)=="another_client","a second client is told another client holds the editor (busy, another_client)");

            // What the bridge refuses before the editor sees it.
            auto refused=[&](const std::string& op,const J& args,const char* why){const auto reply=bridge(op,args);return McpPipe::Code(reply)=="refused" && McpPipe::Reason(reply)==why;};
            require(refused("light.set",{{"actor",{{"path","MyLevel.Light0"},{"class","Engine.Light"}}},{"properties",J::object()}},"blocked"),"light.set is refused over the bridge (blocked)");
            require(refused("mcp.probe.unknown",J::object(),"unknown_op"),"an op the bridge does not have is refused (unknown_op)");
            if(!setting("AllowRawExec"))require(refused("exec",{{"command","STAT FPS"}},"raw_exec_off"),"exec is refused while [Mcp] AllowRawExec is off");
            if(!setting("AllowPlay"))require(refused("play.level",J::object(),"play_off"),"play.level is refused while [Mcp] AllowPlay is off");

            // Reading.
            {
                const auto reply=bridge("editor.state");
                const auto state=reply.value("result",J::object());
                require(McpPipe::Ok(reply) && state.value("open",false),"editor.state answers for the open map");
                require(pick(reply,"/state/expect")==J{{"level",state.value("level",J())},{"generation",state.value("generation",J())},{"revision",state.value("contentRevision",J())}},
                    "the state block's expect is {level, generation, revision: contentRevision}");
                const auto query=bridge("actors.query",{{"limit",5}});
                require(McpPipe::Ok(query) && pick(query,"/result/actors").is_array() && pick(query,"/log").is_array() && pick(query,"/undo/steps")==0,
                    "actors.query reads the map, with the call's log lines and no Undo step");
            }

            // A new map: what follows needs no fixture.
            const auto fresh=run("map.new",{{"discard",true}},{{"label","probe new map"}},300000);
            require(fresh.value("open",false) && fresh.value("mapFile",std::string()).empty() && fresh.value("dirty",true)==false,"map.new starts an empty map with no file and nothing to save");

            // One change: its own result, one Undo step named after the call, its history file.
            {
                const auto before=paths();
                const auto placed=bridge("design.block",block("MCP probe room",-2048,-2048),{{"label","probe room"}});
                const unsigned placedCall=placed.value("call",0u);
                const auto piece=members(pick(placed,"/result"));
                require(McpPipe::Ok(placed) && within(piece,paths()) && apart(piece,before),"design.block over the bridge places the piece and answers with the piece, not its one-step plan");
                require(pick(placed,"/undo")==J{{"call",placedCall},{"steps",1}},"the change is one Undo step of its call");
                require(WorkflowProbe::NewestUndoTitle()==undoTitle(placedCall,"probe room"),"Undo History names the step \"MCP <call>: <label>\"");
                bool logged=false;
                std::error_code error;
                const auto suffix="-"+std::to_string(placedCall)+"-design.block.json";
                for(std::filesystem::directory_iterator it(directory/"ReloadedEditor"/"Mcp"/"history",error),end;!error && it!=end;it.increment(error))
                {
                    const auto name=it->path().filename().string();
                    logged=logged || (name.size()>suffix.size() && name.compare(name.size()-suffix.size(),suffix.size(),suffix)==0);
                }
                require(logged,"the change is written to System\\ReloadedEditor\\Mcp\\history");
            }

            // A dry run changes nothing.
            {
                const auto revision=pick(run("editor.state"),"/contentRevision");
                const auto actorsNow=paths();
                const auto dry=bridge("design.block",block("MCP probe dry run",2048,-2048),{{"label","probe dry run"},{"dryRun",true}});
                require(McpPipe::Ok(dry) && pick(dry,"/result/dryRun")==true && pick(dry,"/result/preview").is_array() && !pick(dry,"/result/preview").empty() && pick(dry,"/undo/steps")==0,
                    "a dry run answers with its preview rows and makes no Undo step");
                require(paths()==actorsNow && pick(run("editor.state"),"/contentRevision")==revision,"a dry run changes nothing");
            }

            // expect: a change against an older revision is refused; the current one runs.
            {
                const auto expect=pick(bridge("editor.state"),"/state/expect");
                auto older=expect;older["revision"]=older.value("revision",0u)-1;
                const auto actorsNow=paths();
                const auto stale=bridge("design.block",block("MCP probe stale",2048,2048),{{"label","probe stale"},{"expect",older}});
                require(McpPipe::Code(stale)=="stale" && McpPipe::Reason(stale)=="map_edited","a change against an older revision is refused as stale (map_edited)");
                require(paths()==actorsNow && pick(bridge("editor.state"),"/state/expect")==expect,"a stale change changes nothing");
                require(McpPipe::Ok(bridge("design.block",block("MCP probe expected",2048,2048),{{"label","probe expected"},{"expect",expect}})),"the same change against the current revision runs");
            }

            // A modal dialog: calls are answered busy, editor.state says why.
            {
                J read,state;
                worker.Start([&]
                {
                    HWND box=nullptr;
                    for(int i=0;i<400 && !(box=McpPipe::OwnWindow(L"#32770",WorkflowProbe::kMcpProbeBox));++i)Sleep(25);
                    if(!box)throw std::runtime_error("The probe's message box did not open.");
                    read=client.Call("actors.query",{{"limit",1}},J::object(),20000);
                    state=client.Call("editor.state",J::object(),J::object(),20000);
                    PostMessageW(box,WM_COMMAND,IDOK,0);
                });
                const UINT_PTR timer=SetTimer(nullptr,0,30000,WorkflowProbe::CloseMcpProbeBox);
                MessageBoxW(frameWindow,L"The MCP bridge probe closes this box.",WorkflowProbe::kMcpProbeBox,MB_OK);
                KillTimer(nullptr,timer);
                worker.Finish(120000);
                require(McpPipe::Code(read)=="busy" && McpPipe::Reason(read)=="modal","a call while a modal dialog is open is answered busy (modal)");
                require(McpPipe::Ok(state) && pick(state,"/result/busy")=="modal","editor.state still answers while a modal dialog is open, saying busy: modal");
                require(McpPipe::Ok(bridge("actors.query",{{"limit",1}})),"calls run again once the dialog is closed");
            }

            // A plan whose second step fails: the first step's change is rolled back.
            {
                const auto actorsNow=paths();const auto titleNow=WorkflowProbe::NewestUndoTitle();
                const auto revision=pick(run("editor.state"),"/contentRevision");
                const J missing={{"$bind",{{"step","good"},{"ptr","/no/such/members"}}}};
                J plan={{"label","probe rollback"},{"steps",J::array()}};
                plan["steps"].push_back({{"id","good"},{"op","design.block"},{"args",block("MCP probe rolled back",-2048,2048)}});
                plan["steps"].push_back({{"id","bad"},{"op","select"},{"args",{{"actors",missing}}}});
                const auto rolled=bridge("mcp.plan",plan,{{"label","probe rollback"}});
                require(!McpPipe::Ok(rolled) && pick(rolled,"/error/details/failedStep")=="bad" && pick(rolled,"/error/details/rolledBack")==true && pick(rolled,"/error/details/section")=="steps",
                    "a plan whose second step fails names that step and is rolled back");
                require(!pick(rolled,"/error/details/steps/good").is_null(),"the failure reports what the step before it returned");
                const auto stateAfter=run("editor.state");
                require(paths()==actorsNow,"the rolled-back plan leaves the map's actors as they were");
                require(stateAfter.value("canRedo",true)==false,"no Redo is left to replay the rolled-back plan");
                require(pick(rolled,"/undo/steps")==0 && WorkflowProbe::NewestUndoTitle()==titleNow,"the rolled-back plan leaves no Undo step");
                // ContentRevision counts every transaction, the rolled-back one too, so it moves on.
                Record("mcp_rollback_content_revision",(revision.dump()+" -> "+pick(stateAfter,"/contentRevision").dump()).c_str());
            }

            // A plan of two changes is one Undo step.
            {
                const auto actorsNow=paths();
                J plan={{"label","probe two pieces"},{"steps",J::array()}};
                plan["steps"].push_back({{"id","first"},{"op","design.block"},{"args",block("MCP probe first",-3072,2048)}});
                plan["steps"].push_back({{"id","second"},{"op","design.block"},{"args",block("MCP probe second",-3072,3072)}});
                const auto joined=bridge("mcp.plan",plan,{{"label","probe two pieces"}});
                const unsigned joinedCall=joined.value("call",0u);
                const auto both=paths();
                require(McpPipe::Ok(joined) && pick(joined,"/undo/steps")==1 && within(members(pick(joined,"/result/steps/first")),both) && within(members(pick(joined,"/result/steps/second")),both),
                    "a two-step plan places both pieces as one Undo step");
                require(WorkflowProbe::NewestUndoTitle()==undoTitle(joinedCall,"probe two pieces"),"Undo History shows the plan as one entry");
                Exec("TRANSACTION UNDO");
                require(paths()==actorsNow,"one TRANSACTION UNDO takes both pieces back");
                Exec("TRANSACTION REDO");
                require(paths()==both,"one TRANSACTION REDO puts both back");
                Exec("TRANSACTION UNDO");
            }

            // undo.call: only while the call's steps are the newest.
            {
                const auto own=bridge("design.block",block("MCP probe undo",3072,-3072),{{"label","probe undo"}});
                const unsigned ownCall=own.value("call",0u);
                const auto ownPiece=members(pick(own,"/result"));
                const auto withOwn=paths();
                call({{"op","design.block"},{"spec",room("Probe interleaved edit")},{"position",{3072,3072,0}},{"rotation",{0,0,0}}});
                const auto blocked=bridge("undo.call",{{"call",ownCall}});
                require(McpPipe::Code(blocked)=="refused" && McpPipe::Reason(blocked)=="not_newest" && within(ownPiece,paths()),"undo.call refuses while an edit made since sits on top of the call (not_newest)");
                Exec("TRANSACTION UNDO");
                require(paths()==withOwn,"the editor's own Undo takes the later edit back");
                const auto undone=bridge("undo.call",{{"call",ownCall}});
                require(McpPipe::Ok(undone) && pick(undone,"/result/done")==1 && apart(ownPiece,paths()),"undo.call undoes the call once its steps are the newest again");
                const auto again=bridge("undo.call",{{"call",ownCall}});
                require(McpPipe::Code(again)=="refused" && McpPipe::Reason(again)=="already_undone","undo.call refuses a call that is undone already");
                const auto latest=bridge("design.block",block("MCP probe undo latest",3072,-2048),{{"label","probe undo latest"}});
                const auto latestUndo=bridge("undo.call",{{"call",latest.value("call",0u)}});
                require(McpPipe::Ok(latestUndo) && apart(members(pick(latest,"/result")),paths()),"undo.call undoes the newest call at once");
            }

            // spec.material: the piece's polygons take that material, not the browser's current one.
            {
                const auto materials=run("assets.search",{{"kind","material"},{"limit",500}}).value("assets",J::array());
                auto captured=[&](const J& piece)
                {
                    const auto capture=call({{"op","assembly.capture"},{"members",piece.at("members")},{"position",{0,0,0}},{"rotation",{0,0,0}}});
                    std::string text;for(const auto& a:capture.at("actors"))text+=a.value("text",std::string());
                    return lower(text);
                };
                const auto plainText=captured(run("design.block",block("MCP probe default texture",0,-3072),{{"label","probe default texture"}}));
                std::string material;
                for(const auto& asset:materials)
                {
                    const auto path=asset.value("path",std::string());
                    if(!path.empty() && path.find_first_of("\"' \r\n")==std::string::npos && plainText.find(lower(path))==std::string::npos){material=path;break;}
                }
                Record("mcp_spec_material",material.c_str());
                require(!material.empty(),"assets.search finds a loaded material other than the blockout's default");
                if(!material.empty())
                {
                    auto textured=block("MCP probe material",0,3072);textured["spec"]["material"]=material;
                    const auto piece=run("design.block",textured,{{"label","probe material"}});
                    require(pick(piece,"/spec/material")==material && captured(piece).find(lower(material))!=std::string::npos,"spec.material textures the piece's polygons");
                }
            }

            // Build All, and Map Check's answer.
            {
                const auto built=bridge("build.all",J::object(),{{"label","probe build"}},900000);
                require(McpPipe::Ok(built) && pick(built,"/result/ok")==true,"build.all builds the map over the bridge");
                require(pick(built,"/dialogs").is_array() && pick(built,"/dialogs").empty(),"the build's progress window is left to run: the dialog watchdog answered nothing");
                const auto check=run("mapcheck.run",J::object(),J::object(),300000);
                bool shaped=pick(check,"/rows").is_array() && pick(check,"/summary").is_object();
                if(shaped)for(const auto& row:check.at("rows"))
                {
                    const auto severity=row.value("severity",std::string());
                    shaped=shaped && (severity=="error" || severity=="warning" || severity=="info") && row.contains("group") && row.contains("check")
                        && pick(row,"/message").is_string() && pick(row,"/actors").is_array() && pick(row,"/surfaces").is_array();
                }
                for(const char* count:{"/summary/errors","/summary/warnings","/summary/info"})shaped=shaped && pick(check,count).is_number();
                Record("mcp_mapcheck",pick(check,"/summary").dump().c_str());
                require(shaped,"mapcheck.run answers {rows:[{severity, group, check, message, actors, surfaces}], summary:{errors, warnings, info}}");
            }

            // A geometric event: GE ADD with the points in Map Design's order.
            {
                const auto ladder=bridge("ge.add",{{"command","LADDER"},{"points",J::array({J::array({-2048,-2048,16}),J::array({-2048,-2048,272}),J::array({-2016,-2048,16})})}},{{"label","probe ladder"}});
                const auto command=pick(ladder,"/result/command");
                require(McpPipe::Ok(ladder) && command.is_string() && command.get<std::string>().rfind("GE ADD LADDER X1=",0)==0,"ge.add runs GE ADD LADDER with its points");
                Record("mcp_ge_add_undo",pick(ladder,"/undo").dump().c_str()); // spike S1: whether GE ADD makes an Undo step
            }

            // A screenshot: the viewport must be visible, which the probe's editor is not.
            {
                const bool hidden=!IsWindowVisible(frameWindow);
                if(hidden){ShowWindow(frameWindow,SW_SHOWNOACTIVATE);pump(1500);}
                const auto shot=bridge("view.screenshot",{{"viewport","perspective"},{"maxEdge",1024}});
                if(hidden)ShowWindow(frameWindow,SW_HIDE);
                const auto encoded=pick(shot,"/result/png");
                const auto png=encoded.is_string()?B::Unbase64(encoded.get<std::string>()):std::string();
                int width=0,height=0;
                const bool decoded=WorkflowProbe::PngSize(png,width,height);
                if(decoded)std::ofstream(directory/"mcp_screenshot.png",std::ios::binary)<<png;
                require(decoded && pick(shot,"/result/width")==width && pick(shot,"/result/height")==height && (std::max)(width,height)>256,"view.screenshot returns a PNG of the perspective view over 256 pixels");
            }

            // A blob over 8 MiB: authoring.export of a map made big enough, read back in pieces.
            {
                constexpr uint64_t eightMiB=8ull*1024*1024;
                auto exported=run("authoring.export",J::object(),J::object(),600000);
                unsigned batches=0;size_t fillers=0;
                while(exported.value("size",uint64_t{0})<=eightMiB && batches<12)
                {
                    J document={{"format","scct.map-changes"},{"version",2},{"map","*"},{"description","MCP probe: actors that make the export big"},{"operations",J::array()}};
                    for(int i=0;i<400;++i,++fillers)
                        document["operations"].push_back({{"op","create"},{"id","McpProbeFill"+std::to_string(fillers)},{"class","Engine.Light"},
                            {"properties",{{"Location",{{"X",std::to_string(-6000+static_cast<int>(fillers%100)*120)},{"Y",std::to_string(-6000+static_cast<int>(fillers/100)*120)},{"Z","128"}}}}}});
                    run("blob.free",{{"blob",exported.at("blob")}});
                    run("authoring.apply",{{"document",document}},{{"label","probe export filler"}},600000);++batches;
                    exported=run("authoring.export",J::object(),J::object(),600000);
                }
                const auto size=exported.value("size",uint64_t{0});
                Record("mcp_blob",(std::to_string(size)+" bytes, "+std::to_string(fillers)+" filler lights").c_str());
                require(size>eightMiB,"authoring.export over the bridge writes the export to a blob over 8 MiB");
                std::string data;bool eof=false;size_t chunks=0;
                onWorker([&]
                {
                    while(!eof && data.size()<=size)
                    {
                        const auto chunk=client.Call("blob.read",{{"blob",exported.at("blob")},{"offset",data.size()},{"size",B::kBlobChunk}},J::object(),120000);
                        if(!McpPipe::Ok(chunk))throw std::runtime_error("blob.read failed: "+chunk.dump().substr(0,2000));
                        const auto bytes=B::Unbase64(chunk.at("result").value("data",std::string()));
                        eof=chunk.at("result").value("eof",true);data+=bytes;++chunks;
                        if(bytes.empty())break;
                    }
                },900000);
                bool parsed=false;
                try{parsed=J::parse(data).value("format",std::string())=="scct.map-authoring";}catch(const std::exception&){}
                require(eof && data.size()==size && chunks>=3 && parsed,"blob.read returns the whole export in 4 MiB pieces");
                require(McpPipe::Ok(bridge("blob.free",{{"blob",exported.at("blob")}})) && McpPipe::Code(bridge("blob.read",{{"blob",exported.at("blob")}}))=="invalid","blob.free deletes the blob");
                require(McpPipe::Code(bridge("blob.read",{{"blob","..\\library"}}))=="invalid","a blob handle naming another file is refused");
                if(batches)require(pick(run("undo.steps",{{"count",batches}}),"/done")==batches,"the filler actors undo, one step per batch");
            }

            // map.save, map.new, map.open: the saved map comes back.
            {
                const auto savedPiece=run("design.block",block("MCP probe saved",-1024,1024),{{"label","probe saved piece"}});
                const auto file=directory.parent_path()/"Packages"/"MapsEd"/"McpProbeD.sdc";
                const auto saved=run("map.save",{{"path","McpProbeD.sdc"},{"overwrite",true}},{{"label","probe save"}},900000);
                require(saved.value("saved",false) && std::filesystem::exists(file) && lower(saved.value("file",std::string())).find("mcpprobed.sdc")!=std::string::npos,
                    "map.save writes Packages\\MapsEd\\McpProbeD.sdc");
                require(run("editor.state").value("dirty",true)==false,"the saved map has no unsaved changes");
                run("map.new",{{"discard",true}},J::object(),300000);
                require(apart(members(savedPiece),paths()),"map.new leaves the saved piece behind");
                const auto opened=run("map.open",{{"path","McpProbeD.sdc"}},J::object(),900000);
                require(lower(opened.value("mapFile",std::string())).find("mcpprobed.sdc")!=std::string::npos && opened.value("dirty",true)==false,"map.open opens the saved map, with nothing to save");
                require(within(members(savedPiece),paths()),"the saved piece comes back with the map");
            }

            // design.register on the saved map: the Map Design panel's library lists the
            // piece (design.pieces), and library.json has it under the map's file. An
            // unsaved map's entry stays in memory until its first save.
            {
                const auto placedPiece=run("design.block",block("MCP probe registered",-1024,-3072),{{"label","probe register"}});
                const auto key=call({{"op","map"}}).at("key").get<std::string>();
                auto listed=[&]
                {
                    std::ifstream in(directory/"ReloadedEditor"/"library.json");J library;in>>library;
                    const auto pieces=library["maps"].value(key,J::object()).value("design",J::object()).value("pieces",J::array());
                    for(const auto& entry:pieces)if(entry.value("members",J())==placedPiece.at("members"))return true;
                    return false;
                };
                auto shown=[&]
                {
                    const auto pieces=pick(run("design.pieces"),"/pieces");
                    if(pieces.is_array())for(const auto& entry:pieces)if(entry.value("members",J())==placedPiece.at("members"))return true;
                    return false;
                };
                require(!key.empty(),"the reopened map has a library key");
                require(run("design.register",{{"pieces",J::array({placedPiece})}},{{"label","probe register"}}).value("registered",0)==1 && shown() && listed(),
                    "design.register records the piece for the Map Design panel, in library.json under the map's file");
                require(run("design.register",{{"pieces",J::array({placedPiece})}}).value("registered",-1)==0,"registering the same brushes again adds nothing");
                require(run("design.unregister",{{"members",placedPiece.at("members")}}).value("unregistered",0)==1 && !shown() && !listed(),"design.unregister takes the record out again");
            }

            // Require approval: the change waits in the review window, the rest is answered busy.
            {
                SendMessageW(frameWindow,WM_COMMAND,requireApproval,0);
                require(setting("RequireApproval"),"RE+ Tools > MCP > Require approval writes [Mcp] RequireApproval=1");
                const UINT_PTR timer=SetTimer(nullptr,0,1000,WorkflowProbe::CancelStuckMcpReview);
                // answer: IDOK, IDCANCEL, or 0 for a cancel message from the client.
                auto review=[&](const std::string& name,int answer,J& during)
                {
                    J reply;
                    onWorker([&]
                    {
                        const auto id=client.Post("design.block",block(name,1024,-1024),{{"label",name}});
                        during["asked"]=client.Until(id,"awaiting_approval",60000);
                        HWND window=nullptr;
                        for(int i=0;i<400 && !(window=McpPipe::OwnWindow(WorkflowProbe::kMcpReviewClass,WorkflowProbe::kMcpReviewTitle));++i)Sleep(25);
                        if(!window)throw std::runtime_error("The approval window did not open: "+during["asked"].dump().substr(0,2000));
                        const auto ticket=during["asked"].value("ticket",std::string());
                        during["read"]=client.Call("actors.query",{{"limit",1}},J::object(),20000);
                        during["change"]=client.Call("design.block",block("MCP probe queued",1024,1024),J::object(),20000);
                        during["state"]=client.Call("editor.state",J::object(),J::object(),20000);
                        during["status"]=client.Call("approval.status",{{"ticket",ticket}},J::object(),20000);
                        during["log"]=client.Call("log.tail",{{"since",0}},J::object(),20000);
                        if(answer)PostMessageW(window,WM_COMMAND,MAKEWPARAM(answer,BN_CLICKED),0);
                        else client.Cancel(id);
                        J later=J::array();
                        reply=client.Wait(id,120000,[&](const J& message){later.push_back(message);});
                        during["later"]=later;
                        during["outcome"]=client.Call("approval.status",{{"ticket",ticket}},J::object(),20000);
                    },300000);
                    return reply;
                };
                J during;
                const auto approved=review("probe approved",IDOK,during);
                require(during["asked"].value("stage",std::string())=="awaiting_approval" && !during["asked"].value("ticket",std::string()).empty(),"with Require approval on, a change first asks for approval, with a ticket");
                require(McpPipe::Code(during["read"])=="busy" && McpPipe::Reason(during["read"])=="approval_pending" && McpPipe::Code(during["change"])=="busy" && McpPipe::Reason(during["change"])=="approval_pending",
                    "while the approval window is open other calls are answered busy (approval_pending)");
                require(McpPipe::Ok(during["state"]) && pick(during["state"],"/result/cached")==true && pick(during["state"],"/result/busy")=="approval_pending","editor.state answers from its cache during the review, saying busy");
                require(pick(during["status"],"/result/status")=="pending" && McpPipe::Ok(during["log"]),"approval.status says pending, and log.tail answers, during the review");
                bool approvedStage=false;for(const auto& message:during["later"])approvedStage=approvedStage || message.value("stage",std::string())=="approved";
                require(McpPipe::Ok(approved) && approvedStage && within(members(pick(approved,"/result")),paths()),"Approve applies the change, after an approved progress message");
                require(pick(during["outcome"],"/result/status")=="done" && pick(during["outcome"],"/result/outcome/ok")==true,"approval.status then reports the ticket done, with its outcome");
                const auto beforeDecline=paths();
                J declinedDuring;const auto declined=review("probe declined",IDCANCEL,declinedDuring);
                require(McpPipe::Code(declined)=="denied" && McpPipe::Reason(declined)=="user_declined" && paths()==beforeDecline && pick(declinedDuring["outcome"],"/result/status")=="denied",
                    "Cancel in the approval window declines the change (denied) and changes nothing");
                J cancelledDuring;const auto cancelled=review("probe cancelled",0,cancelledDuring);
                require(McpPipe::Code(cancelled)=="cancelled" && paths()==beforeDecline && pick(cancelledDuring["outcome"],"/result/status")=="cancelled",
                    "a cancel message from the client closes the approval window (cancelled)");
                std::vector<J> progress;J readPlan;
                onWorker([&]{readPlan=client.Call("mcp.plan",{{"steps",J::array({{{"id","look"},{"op","actors.query"},{"args",{{"limit",1}}}}})}},J::object(),60000,&progress);});
                bool asked=false;for(const auto& message:progress)asked=asked || message.value("stage",std::string())=="awaiting_approval";
                require(McpPipe::Ok(readPlan) && !asked,"a plan of reads only runs without asking for approval");
                SendMessageW(frameWindow,WM_COMMAND,requireApproval,0);
                KillTimer(nullptr,timer);
                require(!setting("RequireApproval"),"Require approval turns off again");
            }

            // The menu: Disconnect client, then the server off and on.
            {
                SendMessageW(frameWindow,WM_COMMAND,disconnectClient,0);
                bool closed=false;
                onWorker([&]{try{for(int i=0;i<60 && !closed;++i)client.Receive(250);}catch(const McpPipe::Closed&){closed=true;}});
                require(closed,"RE+ Tools > MCP > Disconnect client closes the pipe");
                J again;onWorker([&]{client.Open(pid,15000);again=client.Hello("native-probe","1",10000);});
                require(again.value("t",std::string())=="hello","a client connects again after a disconnect");
                SendMessageW(frameWindow,WM_COMMAND,enableServer,0);
                client.Close();
                bool gone=false;
                onWorker([&]{for(int i=0;i<60 && !gone;++i){gone=!McpPipe::Listening(pid);if(!gone)Sleep(250);}});
                require(!setting("Enabled") && gone,"turning the server off writes Enabled=0 and takes the pipe away");
                SendMessageW(frameWindow,WM_COMMAND,enableServer,0);
                J back;onWorker([&]{client.Open(pid,20000);back=client.Hello("native-probe","1",10000);});
                require(setting("Enabled") && back.value("t",std::string())=="hello","turning it on again lets a client connect");
            }
            client.Close();
            if(!enabledAtStart)SendMessageW(frameWindow,WM_COMMAND,enableServer,0);
        };
        // map.recover over the bridge (run_native_map_recovery_test.ps1
        // -McpRecoverSource <compiled .sdc>, copied to the disposable
        // Packages\Maps): map.open refusing the compiled file, the refusals
        // and dry runs that change nothing, a cancel at an early stage that
        // leaves nothing behind, then a real recovery and its report.
        auto mcpRecover=[&](const std::filesystem::path& source)
        {
            constexpr UINT enableServer=41460,requireApproval=41461; // McpBridge::kEnable, kRequireApproval
            const auto ini=(directory/"Reloaded_Editor.ini").string();
            auto setting=[&](const char* key){return GetPrivateProfileIntA("Mcp",key,0,ini.c_str())!=0;};
            auto pick=[](const J& value,const char* pointer){const J::json_pointer at(pointer);return value.contains(at)?value.at(at):J();};
            auto text=[&](const J& value,const char* pointer){const auto found=pick(value,pointer);return found.is_string()?found.get<std::string>():std::string();};
            auto has=[](const std::string& whole,const std::string& part){return whole.find(part)!=std::string::npos;};
            const bool enabledAtStart=setting("Enabled");
            if(!enabledAtStart)SendMessageW(frameWindow,WM_COMMAND,enableServer,0);
            if(setting("RequireApproval"))SendMessageW(frameWindow,WM_COMMAND,requireApproval,0);
            require(setting("Enabled") && !setting("RequireApproval"),"the MCP server is on, without approval");
            McpPipe::Client client;McpPipe::Worker worker;
            auto onWorker=[&](std::function<void()> job,DWORD budget){worker.Run(std::move(job),budget);};
            auto bridge=[&](const std::string& op,const J& args,const J& options=J::object(),DWORD timeout=600000)
            {
                Record("workflow",("mcp "+op+" "+args.dump().substr(0,200)).c_str());
                J reply;onWorker([&]{reply=client.Call(op,args,options,timeout);},timeout+60000);
                Record("mcp_reply",reply.dump().substr(0,4000).c_str());
                return reply;
            };
            onWorker([&]{client.Open(GetCurrentProcessId(),20000);client.Hello("native-probe","1",10000);},60000);
            const auto packages=directory.parent_path()/"Packages";
            const auto stem=source.stem().string();
            auto outputs=[&](const std::string& name)
            {
                return std::vector<std::filesystem::path>{packages/"MapsEd"/(name+".sdc"),packages/"Maps"/(name+".sdc"),packages/"MapsEd"/"Recovery"/name,
                                                          packages/"StaticMeshes"/(name+"_Assets.usx")};
            };
            auto untouched=[&](const std::string& name){for(const auto& path:outputs(name))if(std::filesystem::exists(path))return false;return true;};
            auto refused=[&](const J& reply,const char* reason){return McpPipe::Code(reply)=="refused" && McpPipe::Reason(reply)==reason;};
            require(std::filesystem::exists(source) && source.parent_path().filename()=="Maps","the compiled map is in the disposable Packages\\Maps");
            bridge("map.new",{{"discard",true}});

            // map.open of a compiled map is refused, naming map.recover.
            const auto opened=bridge("map.open",{{"path","Packages\\Maps\\"+source.filename().string()},{"discard",true}});
            require(refused(opened,"compiled_map") && has(text(opened,"/error/message"),"map.recover"),"map.open refuses a compiled map (compiled_map) and names map.recover");

            // Refusals and dry runs change nothing.
            const std::string cancelled=stem+"_McpCancel",target=stem+"_Mcp";
            const J dry{{"dryRun",true}};
            require(McpPipe::Code(bridge("map.recover",{{"source","NoSuchCompiledMap"},{"destination",target}},dry))=="invalid","a compiled map that is not there is invalid");
            require(McpPipe::Code(bridge("map.recover",{{"source",stem},{"destination","Not a name"}},dry))=="invalid","a destination the editor cannot name a map after is invalid");
            {
                const auto copy=packages/"MapsEd"/(stem+"_McpSource.sdc");
                std::filesystem::copy_file(source,copy,std::filesystem::copy_options::overwrite_existing);
                const auto reply=bridge("map.recover",{{"source","Packages\\MapsEd\\"+copy.filename().string()},{"destination",target}},dry);
                require(refused(reply,"source_map") && has(text(reply,"/error/message"),"map.open"),"a map in Packages\\MapsEd is refused (source_map), pointing at map.open");
                std::filesystem::remove(copy);
            }
            {
                const auto taken=packages/"MapsEd"/"Recovery"/(stem+"_McpTaken");
                std::filesystem::create_directories(taken);
                const auto reply=bridge("map.recover",{{"source",stem},{"destination",stem+"_McpTaken"}},dry);
                require(refused(reply,"destination_exists") && pick(reply,"/error/details/existing").size()==1 && pick(reply,"/error/details/suggestion").is_string(),
                    "a destination whose recovery folder is left from an earlier run is refused (destination_exists), with a free name");
                std::filesystem::remove_all(taken);
            }
            {
                // Unsaved work: refused unless discarded.
                const J room{{"kind","Room"},{"construction","Carve"},{"width",512},{"length",512},{"height",256},{"thickness",16},{"ceiling",true},{"portal",false},{"name","MCP recover unsaved"}};
                const auto placed=bridge("design.block",{{"spec",room},{"position",{0,0,0}},{"rotation",{0,0,0}}});
                require(McpPipe::Ok(placed) && pick(bridge("editor.state",J::object()),"/result/dirty")==true,"a placed piece leaves unsaved changes");
                require(refused(bridge("map.recover",{{"source",stem},{"destination",target}},dry),"unsaved_changes"),"unsaved changes are refused (unsaved_changes)");
            }
            const auto checked=bridge("map.recover",{{"source",stem},{"destination",target},{"discard",true}},dry);
            const auto& d=checked.value("result",J::object());
            Record("mcp_recover_dry_run",d.dump().c_str());
            require(McpPipe::Ok(checked) && d.value("dryRun",false) && has(d.value("source",std::string()),source.filename().string()) && has(d.value("destination",std::string()),target+".sdc"),
                "a dry run names the compiled map and the new one");
            require(d.value("missing",J()).is_array() && d.value("missing",J()).empty() && d.value("imports",0)>0 && d.value("sourceBytes",std::uint64_t{0})==std::filesystem::file_size(source) &&
                    d.value("checks",J()).is_array() && !d.value("checks",J()).empty(),"it checked the imports and the file, and lists its checks");
            const auto inArgs=bridge("map.recover",{{"source",stem},{"destination",target},{"discard",true},{"dryRun",true}});
            require(McpPipe::Ok(inArgs) && pick(inArgs,"/result/dryRun")==true && pick(inArgs,"/undo/steps")==0,"dryRun in the arguments is a dry run too");
            require(untouched(target) && pick(bridge("editor.state",J::object()),"/result/dirty")==true,"the dry runs wrote nothing and left the open map alone");

            // A cancel at an early stage: a new empty map, and none of its files.
            {
                J reply;std::vector<J> progress;
                onWorker([&]
                {
                    const auto id=client.Post("map.recover",{{"source",stem},{"destination",cancelled},{"discard",true}},{{"label","probe recover cancel"}});
                    const auto first=client.Until(id,"recovering",600000);
                    progress.push_back(first);
                    client.Cancel(id);
                    reply=client.Wait(id,1800000,[&](const J& message){progress.push_back(message);});
                },1900000);
                Record("mcp_recover_cancel",reply.dump().substr(0,4000).c_str());
                const auto state=bridge("editor.state",J::object());
                require(McpPipe::Code(reply)=="cancelled" && McpPipe::Reason(reply)=="cancelled" && has(text(reply,"/error/message"),"new empty map"),
                    "a cancel while it runs stops the recovery (cancelled), saying the editor has a new empty map");
                require(pick(reply,"/error/details/stageIndex").is_number() && pick(reply,"/error/details/stageIndex").get<int>()<static_cast<int>(MapRecoveryModel::Stage::Saving),
                    "it stopped before saving, and says where");
                require(untouched(cancelled),"the cancelled recovery left none of its files");
                require(text(state,"/result/mapFile").empty() && pick(state,"/result/dirty")==false,"the editor holds a new map with no file");
            }

            // The recovery itself.
            {
                J reply;std::vector<J> progress;
                onWorker([&]{reply=client.Call("map.recover",{{"source",stem},{"destination",target},{"discard",true},{"maxRows",5}},{{"label","probe recover"}},3600000,&progress);},3700000);
                Record("mcp_recover",reply.dump().substr(0,8000).c_str());
                const auto r=reply.value("result",J::object());
                require(McpPipe::Ok(reply) && r.value("ok",false),"map.recover recovers the compiled map");
                const auto file=r.value("file",std::string());
                require(has(file,target+".sdc") && std::filesystem::exists(packages/"MapsEd"/(target+".sdc")) && std::filesystem::exists(packages/"Maps"/(target+".sdc")),
                    "the new map and its playable copy are written");
                const auto report=r.value("report",std::string());
                require(std::filesystem::exists(report) && std::filesystem::is_directory(r.value("folder",std::string())),"Recovery.txt and the recovery folder are there");
                const auto& c=r.value("counts",J::object());
                require(c.value("cells",0)>0 && c.value("brushes",0)>0 && c.value("polygons",0)>0 && c.value("actors",0)>0,"the counts describe the recovered map");
                size_t categorised=0;for(const auto& entry:r.value("categories",J::object()).items())categorised+=entry.value().get<size_t>();
                require(r.value("rows",J()).is_array() && r.value("rows",J()).size()<=5 && categorised==r.value("rowsTotal",size_t{0}) &&
                        r.value("rows",J()).size()==(std::min)(size_t{5},r.value("rowsTotal",size_t{0})),"the first maxRows rows come inline and every row is counted");
                require(!has(r.value("summary",std::string()),"\r") && has(r.value("summary",std::string()),"structural brushes") && r.value("seconds",0.0)>0,"the summary in plain lines, and the time taken");
                {
                    std::ifstream in(report);std::stringstream all;all<<in.rdbuf();
                    const auto rows=r.value("rowsTotal",size_t{0});
                    require(rows==0 ? has(all.str(),"Nothing to report") : has(all.str(),"Items that did not come back cleanly ("+std::to_string(rows)+")"),"Recovery.txt lists the same rows");
                }
                int lastPercent=-1,lastStage=-1;bool ordered=true;size_t messages=0;
                for(const auto& message:progress)
                {
                    if(message.value("stage",std::string())!="recovering")continue;
                    ++messages;
                    const int stage=message.value("stageIndex",-1),percent=message.value("percent",-1);
                    if(stage<lastStage || percent<0 || percent>100)ordered=false;
                    lastStage=stage;lastPercent=percent;
                }
                Record("mcp_recover_progress",(std::to_string(messages)+" messages, last "+std::to_string(lastPercent)+"%").c_str());
                require(messages>=5 && messages<=200 && ordered && lastPercent>=90,"progress comes stage by stage, a message per change of stage or percent");
                const auto state=bridge("editor.state",J::object());
                require(has(text(state,"/result/mapFile"),target+".sdc") && pick(state,"/result/dirty")==false,"the recovered map is open, saved");
                const auto again=bridge("map.recover",{{"source",stem},{"destination",target}},dry);
                require(refused(again,"destination_exists") && pick(again,"/error/details/existing").size()>=3,"recovering into the same name again is refused");
                const auto playable=bridge("map.open",{{"path","Packages\\Maps\\"+target+".sdc"}});
                require(refused(playable,"compiled_map") && has(text(playable,"/error/details/source"),target+".sdc"),"map.open of its playable copy points at the editable map");
            }
            client.Close();
            if(!enabledAtStart)SendMessageW(frameWindow,WM_COMMAND,enableServer,0);
        };
        if(mcpOnly)
        {
            char recoverSource[MAX_PATH]{};
            GetPrivateProfileStringA("test","mcp_recover_source","",recoverSource,MAX_PATH,(directory/"native_recovery_test.ini").string().c_str());
            if(*recoverSource){mcpRecover(recoverSource);finish("MCP map.recover: map.open refused, refusals and dry runs, an early cancel, a recovery and its report");return;}
        }
        if(mcpOnly){mcpBridge();finish("MCP bridge: handshake, refusals, busy and stale replies, plans with rollback and Undo, blobs, map files, design records, builds, pictures and approval");return;}
        auto checkSurfaceBrushSelection=[&](size_t minimumBrushes)
        {
            auto read=[](uintptr_t p){return *reinterpret_cast<uintptr_t*>(p);};
            auto model=read(read(read(kEditor)+0x130)+0x13c),surfaces=read(model+0x94);
            auto count=read(model+0x98);require(count>0,"source BSP faces available for brush selection");
            std::vector<DWORD> flags;for(size_t i=0;i<count;++i)flags.push_back(read(surfaces+i*0x2c+0x14));
            auto setFlags=[&](bool all){for(size_t i=0;i<count;++i)*reinterpret_cast<DWORD*>(surfaces+i*0x2c+0x14)=(flags[i]&~0x02000000u)|(all?0x02000000u:0);};
            auto menuEnabled=[&]()
            {
                using Load=HMENU(WINAPI*)(HINSTANCE,LPCSTR);
                auto menu=(*reinterpret_cast<Load*>(0x11af23f0))(GetModuleHandle(nullptr),MAKEINTRESOURCEA(108));
                require(menu!=nullptr,"native surface context menu loads through hook");
                auto state=GetMenuState(GetSubMenu(menu,0),40926,MF_BYCOMMAND);DestroyMenu(menu);
                require(state!=UINT(-1),"Select Brush appears in native face context menu");return !(state&(MF_DISABLED|MF_GRAYED));
            };
            setFlags(false);require(!menuEnabled(),"Select Brush disabled without selected faces");
            *reinterpret_cast<DWORD*>(surfaces+0x14)|=0x02000000u;
            auto cameras=call({{"op","view.capture"}})["cameras"];
            auto expected=call({{"op","surface.brushes"}});require(expected.size()==1 && menuEnabled(),"selected face resolves one editable source brush");
            call({{"op","select"},{"actors",J::array()}});SendMessage(frameWindow,WM_COMMAND,40926,0);
            auto selected=call({{"op","actors"},{"selected",true}});
            require(selected.size()==1 && selected[0]["path"]==expected[0]["path"],"face menu command selects its source brush through native actor selection");
            require(call({{"op","view.capture"}})["cameras"]==cameras,"selecting source brush preserves all viewport cameras");
            require((read(surfaces+0x14)&0x02000000u)!=0,"brush selection preserves selected face");
            {
                auto actor=WorkflowProbe::MagicActor(expected[0]);float original[3];memcpy(original,actor+0x80,12);
                for(int axis=0;axis<3;++axis)reinterpret_cast<float*>(actor+0x80)[axis]+=.375f;
                const auto before=call({{"op","brush.snap.bounds"},{"surfaces",true}});
                using Load=HMENU(WINAPI*)(HINSTANCE,LPCSTR);
                auto menu=(*reinterpret_cast<Load*>(0x11af23f0))(GetModuleHandle(nullptr),MAKEINTRESOURCEA(108));
                require(menu && GetMenuState(GetSubMenu(menu,0),40943,MF_BYCOMMAND)!=UINT(-1),"surface context offers all-axis edge snap");DestroyMenu(menu);
                SendMessage(frameWindow,WM_COMMAND,40943,0);
                const auto after=call({{"op","brush.snap.bounds"},{"surfaces",true}});
                for(int axis=0;axis<3;++axis){double grid=after["grid"][axis];bool aligned=false;for(const char* bound:{"min","max"}){double v=after[bound][axis].get<double>()/grid;aligned|=std::abs(v-std::round(v))<.0001;}require(aligned,"face menu snaps source brush bounds");}
                Exec("TRANSACTION UNDO");require(call({{"op","brush.snap.bounds"},{"surfaces",true}})==before,"surface snap Undo restores source brush and selection");
                memcpy(actor+0x80,original,12);
            }
            auto poly=read(surfaces+0x20);*reinterpret_cast<uintptr_t*>(surfaces+0x20)=0;
            require(!menuEnabled(),"face without source polygon disables Select Brush");
            bool rejected=false;try{call({{"op","surface.brushes"}});}catch(const std::exception&){rejected=true;}
            *reinterpret_cast<uintptr_t*>(surfaces+0x20)=poly;
            require(rejected && call({{"op","actors"},{"selected",true}})==selected,"missing source is rejected without changing actor selection");
            setFlags(true);expected=call({{"op","surface.brushes"}});
            require(expected.size()>=minimumBrushes,"multiple selected faces resolve distinct source brushes");
            SendMessage(frameWindow,WM_COMMAND,40926,0);selected=call({{"op","actors"},{"selected",true}});
            require(selected.size()==expected.size(),"multiple faces select each owning brush once");
            for(const auto& brush:expected){bool found=false;for(const auto& a:selected)if(a["path"]==brush["path"])found=true;require(found,"owning brush remains selected across viewports");}
            for(size_t i=0;i<count;++i)*reinterpret_cast<DWORD*>(surfaces+i*0x2c+0x14)=flags[i];
        };
        std::ofstream(directory/"workflow_magic_schema.json")<<WorkflowProbe::MagicSchema().dump(2);
        if(restart)
        {
            require(Exec(std::string("MAP LOAD FILE=\"")+destination+"\"")!=0,"fresh editor opens saved workflow map");
            J savedAuthoring;std::ifstream(directory/"map_authoring_saved.json")>>savedAuthoring;
            for(const auto& actor:savedAuthoring)require(call({{"op","magic.inspect"},{"actor",actor.at("actor")}})["values"]==actor.at("values"),"batch-authored actors, particle components and delayed links survive restart");
            J savedCameras;std::ifstream cameraInput(directory/"camera_network_saved.json");cameraInput>>savedCameras;
            for(const auto& camera:savedCameras)require(call({{"op","magic.inspect"},{"actor",camera.at("actor")}})["values"]==camera.at("values"),"camera names, pose and network links survive editor restart");
            std::ifstream magicInput(directory/"magic_setpiece.json");J savedMagic;magicInput>>savedMagic;
            for(const auto& savedActor:savedMagic)require(call({{"op","magic.inspect"},{"actor",savedActor.at("actor")}})["values"]==savedActor.at("values"),"set piece actor settings survive a full editor restart");
            auto reopenedEvent=call({{"op","magic.inspect"},{"actor",savedMagic[0]["actor"]}});auto reopenedGroups=reopenedEvent["values"]["Groups"];reopenedGroups[0]["EventGroup"][0]["Delay"]="1.5";
            call({{"op","magic.edit"},{"snapshot",reopenedEvent},{"changes",{{"Groups",reopenedGroups}}}});Exec("TRANSACTION UNDO");
            require(call({{"op","magic.inspect"},{"actor",savedMagic[0]["actor"]}})["values"]==reopenedEvent["values"],"reopened event uses the same editing and undo workflow");
            using Filename=void(__thiscall*)(void*,const char*);
            reinterpret_cast<Filename>(0x10e05e1c)(*reinterpret_cast<void**>(0x1165e80c),destination);
            {
                // Design pieces are still whole after a restart: their brushes
                // came back with the saved map and the library still names them.
                const auto scene=call({{"op","design.scene"}});
                const auto key=call({{"op","map"}})["key"].get<std::string>();
                std::ifstream libraryIn(directory/"ReloadedEditor"/"library.json");J libraryDoc;libraryIn>>libraryDoc;libraryIn.close();
                const J pieces=libraryDoc["maps"].value(key,J::object()).value("design",J::object()).value("pieces",J::array());
                size_t wholePieces=0,wholeSpirals=0,brokenPieces=0;
                for(const auto& piece:pieces)
                {
                    size_t live=0;
                    for(const auto& member:piece["members"])for(const auto& actor:scene)if(actor["path"]==member["path"]){++live;break;}
                    if(live>0 && live==piece["members"].size()){++wholePieces;if(piece["spec"]["kind"]=="Spiral")++wholeSpirals;}
                    else ++brokenPieces;
                }
                const std::string why="design pieces are still whole after a restart ("+std::to_string(wholePieces)+" whole, "+std::to_string(brokenPieces)+" not, "+std::to_string(pieces.size())+" recorded, key "+key+")";
                require(wholePieces>0 && wholeSpirals>0,why.c_str());
            }
            std::ifstream input(oldLibrary);J saved;input>>saved;input.close();
            auto key=call({{"op","map"}}).at("key").get<std::string>();auto view=saved.at("maps").at(key).at("views").at(0);
            SendMessage(frameWindow,WM_COMMAND,40921,0);auto window=WorkflowProbe::FindDialog("Working Views");require(window!=nullptr,"working views reopen after process restart");
            ListView_SetItemState(GetDlgItem(window,100),0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
            WorkflowProbe::Click(window,201);
            require(call({{"op","view.capture"}}).at("cameras")==view.at("cameras"),"saved adjusted cameras restore after process restart");
            WorkflowProbe::Click(window,203);std::ifstream updatedInput(oldLibrary);J updated;updatedInput>>updated;
            require(updated["maps"][key]["views"].size()==1 && updated["maps"][key]["views"][0]["id"]==view["id"] && updated["maps"][key]["views"][0]["name"]==view["name"],"view update after restart preserves identity and name without duplicates");
            auto count=call({{"op","actors"}}).size();auto entry=updated.at("assemblies").at(0);
            auto placed=call({{"op","assembly.place"},{"definition",entry},{"position",{0,512,0}},{"rotation",{0,0,0}},{"bindings",J::object()}});
            require(placed.at("members").size()==entry.at("actors").size() && call({{"op","actors"}}).size()==count+entry.at("actors").size(),"updated assembly persists for future placements after restart");
            DestroyWindow(window);finish("Workflow views and updated assemblies survive a full editor restart.");return;
        }
        J actors=call({{"op","actors"}});require(actors.size()>=3,"fixture actors available");
        checkSurfaceBrushSelection(1);
        {
            auto selection=call({{"op","actors"},{"selected",true}});
            auto brush=std::find_if(actors.begin(),actors.end(),[](const J& a){return a.at("class")=="Engine.Brush" && a.value("authorable",false);});
            require(brush!=actors.end(),"grid snap fixture brush available");
            {
                const auto view=call({{"op","view.capture"}});
                const auto rows=call({{"op","brush.visibility"}});
                require(rows.size()==7,"brush visibility has all categories");
                const int category=rows[3]["total"].get<int>()?3:2;
                require(rows[category]["total"].get<int>()>0,"CSG brushes classified");
                call({{"op","brush.visibility.set"},{"category",category},{"action","select"}});
                require(call({{"op","actors"},{"selected",true}}).size()==rows[category]["total"].get<size_t>(),"select type selects matching brushes");
                call({{"op","brush.visibility.set"},{"category",category},{"action","hide"}});
                require(call({{"op","brush.visibility"}})[category]["visible"]==0,"hide type hides brushes");
                require(call({{"op","actors"},{"selected",true}}).empty(),"hiding brushes clears their selection");
                Exec("TRANSACTION UNDO");
                require(call({{"op","brush.visibility"}})[category]["visible"]==rows[category]["total"],"visibility Undo restores brushes");
                call({{"op","brush.visibility.set"},{"category",category},{"action","only"}});
                const auto isolated=call({{"op","brush.visibility"}});
                for(int i=0;i<7;++i)require(isolated[i]["visible"]==(i==category?isolated[i]["total"]:J(0)),"only type isolates matching actors");
                SendMessage(frameWindow,WM_COMMAND,40953,0);
                HWND panel=WorkflowProbe::FindDialog("Brush Visibility");require(panel!=nullptr,"brush visibility panel opens");
                WorkflowProbe::Screenshot(panel,directory/"brush_visibility.bmp");
                WorkflowProbe::Click(panel,200);
                const auto shown=call({{"op","brush.visibility"}});
                for(auto& row:shown)require(row["visible"]==row["total"],"show all reveals every category");
                SendMessage(GetDlgItem(panel,100+category*3),BM_CLICK,0,0);
                require(call({{"op","brush.visibility"}})[category]["visible"]==0,"visibility checkbox hides a type");
                SendMessage(GetDlgItem(panel,100+category*3),BM_CLICK,0,0);
                require(call({{"op","brush.visibility"}})[category]["visible"]==rows[category]["total"],"visibility checkbox reveals a type");
                DestroyWindow(panel);
                SetActiveWindow(frameWindow);SetForegroundWindow(frameWindow);
                call({{"op","view.restore"},{"view",view}});
            }
            auto native=WorkflowProbe::MagicActor(*brush);
            float original[3];memcpy(original,native+0x80,12);
            const float displaced[3]={original[0]+1.25f,original[1]-2.25f,original[2]+3.25f};memcpy(native+0x80,displaced,12);
            auto editor=*reinterpret_cast<unsigned char**>(kEditor);float savedGrid[3];memcpy(savedGrid,editor+0x200,12);
            const float grid[3]={16,32,8};memcpy(editor+0x200,grid,12);
            call({{"op","select"},{"actors",J::array({*brush})}});
            const auto before=call({{"op","brush.snap.bounds"}});
            using Load=HMENU(WINAPI*)(HINSTANCE,LPCSTR);
            auto menu=(*reinterpret_cast<Load*>(0x11af23f0))(GetModuleHandle(nullptr),MAKEINTRESOURCEA(107));
            require(menu && GetMenuState(GetSubMenu(menu,0),40936,MF_BYCOMMAND)!=UINT(-1),"brush context has axis snap commands");DestroyMenu(menu);
            SendMessage(frameWindow,WM_COMMAND,40936,0);
            auto after=call({{"op","brush.snap.bounds"}});
            auto aligned=[&](const J& b,int axis){for(const char* bound:{"min","max"}){double v=b.at(bound).at(axis).get<double>();if(std::abs(v/std::abs(grid[axis])-std::round(v/std::abs(grid[axis])))<.0001)return true;}return false;};
            require(aligned(after,0),"X outer bound lands on active grid");
            for(int axis=1;axis<3;++axis)require(after["min"][axis]==before["min"][axis] && after["max"][axis]==before["max"][axis],"X snap preserves other axes");
            Exec("TRANSACTION UNDO");require(call({{"op","brush.snap.bounds"}})==before,"grid snap Undo restores bounds and selection");
            Exec("TRANSACTION REDO");require(call({{"op","brush.snap.bounds"}})==after,"grid snap Redo restores snapped bounds");
            call({{"op","brush.snap"},{"axes",7}});after=call({{"op","brush.snap.bounds"}});
            for(int axis=0;axis<3;++axis){require(aligned(after,axis),"all-axis bounds snap");require(std::abs((after["max"][axis].get<double>()-after["min"][axis].get<double>())-(before["max"][axis].get<double>()-before["min"][axis].get<double>()))<.001,"snap preserves brush dimensions");}
            call({{"op","brush.snap"},{"axes",7}});
            Exec("TRANSACTION UNDO");require(aligned(call({{"op","brush.snap.bounds"}}),0),"already aligned snap does not add an Undo step");
            Exec("TRANSACTION UNDO");require(call({{"op","brush.snap.bounds"}})==before,"two edits require two Undo steps");
            call({{"op","select"},{"actors",J::array()}});bool rejected=false;
            try{call({{"op","brush.snap"},{"axes",1}});}catch(const std::exception&){rejected=true;}require(rejected,"empty brush snap rejected");
            memcpy(native+0x80,original,12);memcpy(editor+0x200,savedGrid,12);
            call({{"op","select"},{"actors",selection}});
        }
        {
            const auto brush=*std::find_if(actors.begin(),actors.end(),[](const J& a){return a.at("class")=="Engine.Brush" && a.value("authorable",false);});
            call({{"op","select"},{"actors",J::array({brush})}});
            auto actor=WorkflowProbe::MagicActor(brush),editor=*reinterpret_cast<unsigned char**>(kEditor);
            auto model=*reinterpret_cast<unsigned char**>(actor+0x238),polys=*reinterpret_cast<unsigned char**>(model+0x50);
            const int count=*reinterpret_cast<int*>(polys+0x2c);auto data=*reinterpret_cast<unsigned char**>(polys+0x28);
            std::vector<unsigned char> original(data,data+count*0x14c);
            const int mode=*reinterpret_cast<int*>(editor+0x1ac);*reinterpret_cast<int*>(editor+0x1ac)=0x19;
            float grid[3];memcpy(grid,editor+0x200,12);const float testGrid[3]={16,16,16};memcpy(editor+0x200,testGrid,12);
            auto selectionHeader=reinterpret_cast<uintptr_t*>(0x11685a9c);std::array<uintptr_t,3> savedHeader{selectionHeader[0],selectionHeader[1],selectionHeader[2]};
            std::vector<std::array<uintptr_t,3>> entries;
            float corner[3];memcpy(corner,data+0x18,12);
            for(int pi=0;pi<count;++pi){auto p=data+pi*0x14c;for(int vi=0;vi<*reinterpret_cast<unsigned short*>(p+0x148);++vi){auto v=reinterpret_cast<float*>(p+0x18+vi*12);if(!memcmp(v,corner,12)){v[0]+=1.25f;v[1]-=2.25f;v[2]+=3.25f;entries.push_back({reinterpret_cast<uintptr_t>(actor),static_cast<uintptr_t>(pi),static_cast<uintptr_t>(vi)});}}}
            require(entries.size()>=3,"vertex fixture includes all polygon copies of one corner");
            selectionHeader[0]=reinterpret_cast<uintptr_t>(entries.data());selectionHeader[1]=selectionHeader[2]=entries.size();
            auto before=call({{"op","vertex.selection"}});std::vector<unsigned char> displaced(data,data+count*0x14c);
            unsigned cause[3]={0,0,2};auto timer=SetTimer(nullptr,0,50,WorkflowProbe::InspectVertexPopup);
            reinterpret_cast<void(__thiscall*)(void*,void*,void*)>(0x10e045e9)(nullptr,cause,nullptr);KillTimer(nullptr,timer);
            require(WorkflowProbe::vertexPopupChecked && call({{"op","vertex.selection"}})==before,"right-click vertex menu preserves selection and cancelled geometry");
            require(!WorkflowProbe::vertexPortalEnabled,"portal action is disabled for only one distinct corner");
            SendMessage(frameWindow,WM_COMMAND,40944,0);auto after=call({{"op","vertex.selection"}});
            for(size_t i=0;i<after.size();++i){double x=after[i]["world"][0];require(std::abs(x/16-std::round(x/16))<.0001,"selected vertex X snaps in world space");for(int axis=1;axis<3;++axis)require(before[i]["world"][axis]==after[i]["world"][axis],"vertex X snap preserves Y and Z");}
            for(int pi=0;pi<count;++pi){auto p=data+pi*0x14c;for(int vi=0;vi<*reinterpret_cast<unsigned short*>(p+0x148);++vi){bool chosen=false;for(auto e:entries)chosen|=e[1]==pi && e[2]==vi;if(!chosen)require(!memcmp(p+0x18+vi*12,displaced.data()+pi*0x14c+0x18+vi*12,12),"unselected vertices remain exact");}}
            Exec("TRANSACTION UNDO");auto undone=call({{"op","vertex.selection"}});if(undone!=before){Record("vertex_before",before.dump().c_str());Record("vertex_undone",undone.dump().c_str());}require(undone==before,"vertex snap Undo restores selected corner");
            Exec("TRANSACTION REDO");require(call({{"op","vertex.selection"}})==after,"vertex snap Redo restores selected corner");
            call({{"op","vertex.snap"},{"axes",7}});after=call({{"op","vertex.selection"}});
            for(auto v:after)for(double coordinate:v["world"]){require(std::abs(coordinate/16-std::round(coordinate/16))<.0001,"selected corner all-axis alignment");}
            require(after[0]["world"]==after.back()["world"],"shared corner stays joined");
            Exec("TRANSACTION UNDO");Exec("TRANSACTION UNDO");
            // Undo may reallocate the UPolys array. Always resolve it again.
            data=*reinterpret_cast<unsigned char**>(polys+0x28);memcpy(data,original.data(),original.size());
            std::copy(savedHeader.begin(),savedHeader.end(),selectionHeader);*reinterpret_cast<int*>(editor+0x1ac)=mode;memcpy(editor+0x200,grid,12);
        }
        {
            const auto brush=*std::find_if(actors.begin(),actors.end(),[](const J& a){return a.at("class")=="Engine.Brush" && a.value("authorable",false);});
            call({{"op","select"},{"actors",J::array({brush})}});
            auto actor=WorkflowProbe::MagicActor(brush),editor=*reinterpret_cast<unsigned char**>(kEditor);
            auto field=[&](const char* name)->unsigned char*{
                for(auto type=*reinterpret_cast<unsigned char**>(actor+0x24);type;type=*reinterpret_cast<unsigned char**>(type+0x28))
                    for(auto p=*reinterpret_cast<unsigned char**>(type+0x58);p;p=*reinterpret_cast<unsigned char**>(p+0x40))
                        if(std::string(ObjectName(p))==name)return actor+*reinterpret_cast<int*>(p+0x3c);
                throw std::runtime_error("missing brush transform property");
            };
            std::array<int,3> oldRotation{};memcpy(oldRotation.data(),field("Rotation"),12);
            const std::array<int,3> rotation{7000,11000,3000};memcpy(field("Rotation"),rotation.data(),12);
            auto model=*reinterpret_cast<unsigned char**>(actor+0x238),polys=*reinterpret_cast<unsigned char**>(model+0x50);
            auto data=*reinterpret_cast<unsigned char**>(polys+0x28);const int count=*reinterpret_cast<int*>(polys+0x2c);
            const std::vector<unsigned char> sourcePolygons(data,data+count*0x14c);
            const float shapeScale[3]={2,.5f,.25f},shapeOffset[3]={17,-31,9};
            for(int pi=0;pi<count;++pi)for(int vi=0;vi<*reinterpret_cast<unsigned short*>(data+pi*0x14c+0x148);++vi)
                for(int axis=0;axis<3;++axis){auto v=reinterpret_cast<float*>(data+pi*0x14c+0x18+vi*12+axis*4);*v=*v*shapeScale[axis]+shapeOffset[axis];}
            const int mode=*reinterpret_cast<int*>(editor+0x1ac);*reinterpret_cast<int*>(editor+0x1ac)=0x19;
            auto header=reinterpret_cast<uintptr_t*>(0x11685a9c);std::array<uintptr_t,3> saved{header[0],header[1],header[2]};
            std::vector<std::array<uintptr_t,3>> entries;
            for(int pi=0;pi<count;++pi)for(int vi=0;vi<*reinterpret_cast<unsigned short*>(data+pi*0x14c+0x148);++vi)
                entries.push_back({reinterpret_cast<uintptr_t>(actor),static_cast<uintptr_t>(pi),static_cast<uintptr_t>(vi)});
            header[0]=reinterpret_cast<uintptr_t>(entries.data());header[1]=header[2]=entries.size();
            std::array<float,3> oldLocation{},oldPivot{};
            memcpy(oldLocation.data(),field("Location"),12);memcpy(oldPivot.data(),field("PrePivot"),12);
            const std::array<float,3> location{3200,-1856,768},pivot{41,-23,17};
            memcpy(field("Location"),location.data(),12);memcpy(field("PrePivot"),pivot.data(),12);
            // Independent Euler rotation: do not derive the oracle from the same
            // native ToWorld/vertex.selection helper as the implementation.
            const double radians=6.28318530717958647692/65536;
            const double cp=cos(rotation[0]*radians),sp=sin(rotation[0]*radians);
            const double cy=cos(rotation[1]*radians),sy=sin(rotation[1]*radians);
            const double cr=cos(rotation[2]*radians),sr=sin(rotation[2]*radians);
            const double axes[3][3]={{cp*cy,sr*sp*cy-cr*sy,-cr*sp*cy-sr*sy},
                {cp*sy,sr*sp*sy+cr*cy,sr*cy-cr*sp*sy},{sp,-sr*cp,cr*cp}};
            double expectedLow[3]={1e20,1e20,1e20},expectedHigh[3]={-1e20,-1e20,-1e20};
            for(const auto& entry:entries)
            {
                const auto local=reinterpret_cast<float*>(data+entry[1]*0x14c+0x18+entry[2]*12);
                for(int axis=0;axis<3;++axis)
                {
                    double world=location[axis];for(int j=0;j<3;++j)world+=axes[axis][j]*(local[j]-pivot[j]);
                    expectedLow[axis]=(std::min)(expectedLow[axis],world);expectedHigh[axis]=(std::max)(expectedHigh[axis],world);
                }
            }
            auto bounds=call({{"op","brush.snap.bounds"}});
            for(int axis=0;axis<3;++axis)require(std::abs(bounds["min"][axis].get<double>()-expectedLow[axis])<.02 && std::abs(bounds["max"][axis].get<double>()-expectedHigh[axis])<.02,"rotated brush bounds apply pivot then rotation then world translation");
            const auto fitView=call({{"op","view.capture"}}),fitSelection=call({{"op","actors"},{"selected",true}});
            auto level=*reinterpret_cast<unsigned char**>(editor+0x130),builder=(*reinterpret_cast<unsigned char***>(level+0x2c))[1];
            auto builderPose=[&](){std::array<unsigned char,36> state{};for(int i=0;i<3;++i)memcpy(state.data()+12*i,builder+(field(i==0?"Location":i==1?"Rotation":"PrePivot")-actor),12);return state;};
            const auto oldBuilderPose=builderPose();
            const std::vector<unsigned char> fittedSourcePolygons(data,data+count*0x14c);
            SendMessage(frameWindow,WM_COMMAND,40980,0);
            const auto fittedBuilderPose=builderPose();
            require(!memcmp(data,fittedSourcePolygons.data(),fittedSourcePolygons.size()) && !memcmp(field("Rotation"),rotation.data(),12) && !memcmp(field("Location"),location.data(),12) && !memcmp(field("PrePivot"),pivot.data(),12),"fitting leaves source brush geometry and transform unchanged");
            auto bm=*reinterpret_cast<unsigned char**>(builder+0x238),bp=*reinterpret_cast<unsigned char**>(bm+0x50),bd=*reinterpret_cast<unsigned char**>(bp+0x28);
            auto builderField=[&](const char* name){return builder+(field(name)-actor);};
            const std::array<int,3> zeroRotation{};require(!memcmp(builderField("Rotation"),zeroRotation.data(),12),"builder orientation is baked into vertices for native BSP building");
            double low[3]={1e20,1e20,1e20},high[3]={-1e20,-1e20,-1e20};
            double localLow[3]={1e20,1e20,1e20},localHigh[3]={-1e20,-1e20,-1e20};
            for(int pi=0;pi<*reinterpret_cast<int*>(bp+0x2c);++pi)for(int vi=0;vi<*reinterpret_cast<unsigned short*>(bd+pi*0x14c+0x148);++vi)
            {
                const auto local=reinterpret_cast<float*>(bd+pi*0x14c+0x18+vi*12);
                for(int axis=0;axis<3;++axis)
                {
                    double unrotated=0;for(int j=0;j<3;++j)unrotated+=axes[j][axis]*local[j];
                    localLow[axis]=(std::min)(localLow[axis],unrotated);localHigh[axis]=(std::max)(localHigh[axis],unrotated);
                    double world=*reinterpret_cast<float*>(builderField("Location")+axis*4)+local[axis];
                    low[axis]=(std::min)(low[axis],world);high[axis]=(std::max)(high[axis],world);
                }
            }
            for(int axis=0;axis<3;++axis)
            {
                require(std::abs(low[axis]-expectedLow[axis])<.02 && std::abs(high[axis]-expectedHigh[axis])<.02,"oriented builder tightly encloses the rotated brush in world space");
                require(std::abs(localHigh[axis]-localLow[axis]-1024*shapeScale[axis])<.02,"rotated fit preserves the brush local dimensions instead of inflating them");
            }
            require(call({{"op","actors"},{"selected",true}})==fitSelection && call({{"op","view.capture"}})==fitView,"rotated brush fit preserves selection and viewports");
            auto builderVertices=[&](){std::vector<std::array<float,3>> result;auto current=*reinterpret_cast<unsigned char**>(bp+0x28);for(int pi=0;pi<*reinterpret_cast<int*>(bp+0x2c);++pi)for(int vi=0;vi<*reinterpret_cast<unsigned short*>(current+pi*0x14c+0x148);++vi){std::array<float,3> p{};memcpy(p.data(),current+pi*0x14c+0x18+vi*12,12);for(int a=0;a<3;++a)p[a]+=*reinterpret_cast<float*>(builderField("Location")+a*4);result.push_back(p);}return result;};
            const auto orientedVertices=builderVertices();
            Exec("TRANSACTION UNDO");require(builderPose()==oldBuilderPose,"oriented builder fit undoes position rotation and pivot in one step");
            Exec("TRANSACTION REDO");require(builderPose()==fittedBuilderPose,"oriented builder fit redoes position rotation and pivot in one step");
            Exec("TRANSACTION UNDO");
            // The editor also stores rotations directly in brush polygons. That
            // identical visible shape must fit identically with Rotation=0.
            for(int pi=0;pi<count;++pi)for(int vi=0;vi<*reinterpret_cast<unsigned short*>(data+pi*0x14c+0x148);++vi)
            {
                auto p=reinterpret_cast<float*>(data+pi*0x14c+0x18+vi*12);std::array<float,3> original{p[0],p[1],p[2]};
                for(int a=0;a<3;++a){double v=0;for(int j=0;j<3;++j)v+=axes[a][j]*(original[j]-pivot[j]);p[a]=static_cast<float>(v);}
            }
            memset(field("Rotation"),0,12);memset(field("PrePivot"),0,12);
            SendMessage(frameWindow,WM_COMMAND,40980,0);
            const auto bakedVertices=builderVertices();bool sameShape=bakedVertices.size()==orientedVertices.size();
            if(sameShape)for(size_t i=0;i<bakedVertices.size();++i)for(int a=0;a<3;++a)sameShape&=std::abs(bakedVertices[i][a]-orientedVertices[i][a])<.02;
            require(sameShape,"fitting a rotation baked into brush polygons preserves the same oriented box");
            Exec("TRANSACTION UNDO");memcpy(field("Rotation"),rotation.data(),12);
            memcpy(field("Location"),oldLocation.data(),12);memcpy(field("PrePivot"),oldPivot.data(),12);
            data=*reinterpret_cast<unsigned char**>(polys+0x28);memcpy(data,sourcePolygons.data(),sourcePolygons.size());
            require(*reinterpret_cast<unsigned short*>(data+0x148)==4,"portal fixture starts with a quad");
            entries.clear();for(uintptr_t vi=0;vi<4;++vi)entries.push_back({reinterpret_cast<uintptr_t>(actor),0,vi});
            entries.push_back(entries[0]);header[0]=reinterpret_cast<uintptr_t>(entries.data());header[1]=header[2]=entries.size();
            const auto before=call({{"op","vertex.selection"}});const auto total=call({{"op","actors"}}).size();
            unsigned cause[3]={0,0,2};auto timer=SetTimer(nullptr,0,50,WorkflowProbe::InspectVertexPopup);
            reinterpret_cast<void(__thiscall*)(void*,void*,void*)>(0x10e045e9)(nullptr,cause,nullptr);KillTimer(nullptr,timer);
            require(WorkflowProbe::vertexPortalEnabled,"right-click enables the portal action for four rotated corners");
            auto portals=call({{"op","vertex.portal"}});require(portals.size()==1 && call({{"op","actors"}}).size()==total+1,"four selected corners add exactly one portal");
            auto portal=WorkflowProbe::MagicActor(portals[0]);require((*reinterpret_cast<unsigned*>(portal+0x344)&0x04000109u)==0x04000109u,"portal actor has invisible non-solid two-sided zone portal flags");
            auto pm=*reinterpret_cast<unsigned char**>(portal+0x238),pp=*reinterpret_cast<unsigned char**>(pm+0x50);require(*reinterpret_cast<int*>(pp+0x2c)==1,"portal is a single sheet polygon");
            auto pd=*reinterpret_cast<unsigned char**>(pp+0x28);require(*reinterpret_cast<unsigned short*>(pd+0x148)==4,"portal sheet has four corners");
            auto portalField=[&](const char* name){return portal+(field(name)-actor);};
            const std::array<int,3> noRotation{};const std::array<float,3> noPivot{};
            require(!memcmp(portalField("Rotation"),noRotation.data(),12) && !memcmp(portalField("PrePivot"),noPivot.data(),12),"portal brush carries no rotation or pivot for the build to ignore");
            const auto a=before[0]["world"].get<std::array<double,3>>(),b=before[1]["world"].get<std::array<double,3>>(),c=before[2]["world"].get<std::array<double,3>>();
            std::array<double,3> u{},v{},n{};for(int axis=0;axis<3;++axis){u[axis]=b[axis]-a[axis];v[axis]=c[axis]-a[axis];}
            n={u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};const auto length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            bool onPlane=true;std::array<bool,4> matched{};const bool flags=(*reinterpret_cast<unsigned*>(pd+0x140)&0x04000109u)==0x04000109u;
            for(int vi=0;vi<4;++vi)
            {
                // The geometry build's own transform, local - PrePivot + Location, with no pivot.
                std::array<double,3> world{};const auto local=reinterpret_cast<float*>(pd+0x18+vi*12),origin=reinterpret_cast<float*>(portal+0x80);
                for(int axis=0;axis<3;++axis)world[axis]=static_cast<double>(local[axis])+origin[axis];
                double distance=0;for(int axis=0;axis<3;++axis)distance+=(world[axis]-a[axis])*n[axis]/length;
                onPlane&=std::abs(distance)<.002;
                for(size_t k=0;k<4;++k){const auto corner=before[k]["world"].get<std::array<double,3>>();double d=0;for(int axis=0;axis<3;++axis)d+=(world[axis]-corner[axis])*(world[axis]-corner[axis]);if(std::sqrt(d)<.002)matched[k]=true;}
            }
            require(onPlane && flags && std::all_of(matched.begin(),matched.end(),[](bool m){return m;}),"native portal sheet corners are the selected rotated corners, with sheet portal flags");
            require(call({{"op","vertex.selection"}})==before,"adding a portal preserves the selected source vertices");
            Exec("TRANSACTION UNDO");require(call({{"op","actors"}}).size()==total,"portal creation undoes in one step");
            Exec("TRANSACTION REDO");require(call({{"op","actors"}}).size()==total+1,"portal creation redoes in one step");Exec("TRANSACTION UNDO");
            // A sliver with long edges passes the corner rules, but the polygon
            // importer (FPoly::CalcNormal 0x110c0970) needs area: refuse it up front.
            auto quadData=*reinterpret_cast<unsigned char**>(polys+0x28);const std::vector<unsigned char> quad(quadData,quadData+0x14c);
            const float sliver[4][3]={{0,0,0},{64,0,.001f},{128,0,0},{64,0,-.001f}};memcpy(quadData+0x18,sliver,sizeof(sliver));
            timer=SetTimer(nullptr,0,50,WorkflowProbe::InspectVertexPopup);
            reinterpret_cast<void(__thiscall*)(void*,void*,void*)>(0x10e045e9)(nullptr,cause,nullptr);KillTimer(nullptr,timer);
            require(!WorkflowProbe::vertexPortalEnabled,"right-click disables the portal action for a sliver of corners");
            bool refused=false;try{call({{"op","vertex.portal"}});}catch(const std::exception&){refused=true;}
            require(refused && call({{"op","actors"}}).size()==total,"a sliver of corners adds no portal");
            memcpy(quadData,quad.data(),quad.size());
            // A failed edit rolls back without leaving a Redo step that replays it.
            auto tagOf=[&](){for(const auto& item:call({{"op","actors"}}))if(item["path"]==brush["path"])return item["tag"].get<std::string>();throw std::runtime_error("portal fixture brush missing");};
            const auto tag=tagOf();bool failed=false;
            try{call({{"op","light.set"},{"actor",brush},{"properties",{{"Tag","WorkflowRolledBack"},{"ZzNotAProperty",1}}}});}catch(const std::exception&){failed=true;}
            require(failed && tagOf()==tag,"a failed edit rolls back its earlier changes");
            Exec("TRANSACTION REDO");require(tagOf()==tag && call({{"op","actors"}}).size()==total,"Redo after a rolled-back edit replays nothing");
            std::copy(saved.begin(),saved.end(),header);*reinterpret_cast<int*>(editor+0x1ac)=mode;memcpy(field("Rotation"),oldRotation.data(),12);
        }
        if(snapOnly){finish("native brush fitting, vertex portals, grid snap commands and transactions");return;}
        characterPreview();
        {
            auto inspect=[&](const J& actor){return call({{"op","magic.inspect"},{"actor",actor}});};
            auto ref=[](const J& actor){auto type=actor.at("class").get<std::string>();return type.substr(type.find_last_of('.')+1)+"'"+actor.at("path").get<std::string>()+"'";};
            auto add=[&](const J& owner,const char* type){return call({{"op","objective.add"},{"owner",inspect(owner)},{"class",type}});};
            auto menuHas=[&](const J& actor,UINT command)
            {
                call({{"op","select"},{"actors",J::array({actor})}});
                using Load=HMENU(WINAPI*)(HINSTANCE,LPCSTR);
                auto menu=(*reinterpret_cast<Load*>(0x11af23f0))(GetModuleHandle(nullptr),MAKEINTRESOURCEA(107));
                auto state=GetMenuState(GetSubMenu(menu,0),command,MF_BYCOMMAND);DestroyMenu(menu);return state!=UINT(-1);
            };
            auto mission=call({{"op","magic.create"},{"class","SBase.SMission"}});
            require(menuHas(mission,40949) && !menuHas(mission,40950),"mission context offers Add SObjective only");
            auto objective=add(mission,"SBase.SObjective").at(0);
            auto second=add(mission,"SBase.SObjective").at(0);
            require(inspect(mission)["values"]["Objectives"]==J::array({ref(objective),ref(second)}),"adding objectives preserves existing mission links");
            require(menuHas(objective,40950) && menuHas(objective,40951) && menuHas(objective,40952) && !menuHas(objective,40949),"objective context offers computer, bomb and flag pair");
            auto computer=add(objective,"SBase.SComputerObjectiveTrigger").at(0);
            auto bomb=add(objective,"SBase.SBombTargetObjectiveTrigger").at(0);
            auto before=inspect(objective);auto pair=add(objective,"SBase.SFlag");
            require(pair.size()==2,"flag action creates two actors");
            auto flag=pair.at(1),drop=pair.at(0);
            require(inspect(flag)["values"]["DropZone"][0]==ref(drop),"flag references its new drop zone");
            require(inspect(objective)["values"]["Triggers"]==J::array({ref(computer),ref(bomb),ref(flag)}),"objective retains all three linked triggers");
            auto after=inspect(objective)["values"];
            auto nearby=[&](const J& actor,const J& parent){auto a=inspect(actor)["values"]["Location"],b=inspect(parent)["values"]["Location"];double d=0;for(const char* axis:{"X","Y","Z"}){auto delta=std::stod(a[axis].get<std::string>())-std::stod(b[axis].get<std::string>());d+=delta*delta;}return d>0 && d<=32*32;};
            require(nearby(objective,mission) && nearby(computer,objective) && nearby(bomb,objective) && nearby(flag,objective) && nearby(drop,objective),"all objective actors including drop zone are within 32 units of their parent");
            auto count=call({{"op","actors"}}).size();
            Exec("TRANSACTION UNDO");
            require(inspect(objective)["values"]==before["values"] && call({{"op","actors"}}).size()==count-2,"one undo removes flag pair and restores objective links");
            Exec("TRANSACTION REDO");
            require(inspect(objective)["values"]==after && inspect(flag)["values"]["DropZone"][0]==ref(drop),"redo restores flag pair and all links");
            auto edges=call({{"op","connections"},{"actor",flag["path"]}});
            require(edges.size()>=2,"connections manager reports objective and drop-zone links");
            bool stale=false;try{call({{"op","objective.add"},{"owner",before},{"class","SBase.SFlag"}});}catch(const std::exception&){stale=true;}
            require(stale && call({{"op","actors"}}).size()==count,"stale objective request leaves map unchanged");
            bool wrong=false;try{add(mission,"SBase.SComputerObjectiveTrigger");}catch(const std::exception&){wrong=true;}
            require(wrong && call({{"op","actors"}}).size()==count,"wrong parent is rejected without creating actors");
            call({{"op","select"},{"actors",J::array({mission})}});
            SendMessage(frameWindow,WM_COMMAND,40949,0);
            require(inspect(mission)["values"]["Objectives"].size()==3,"native context command creates and links objective");
            Exec("TRANSACTION UNDO");
            // Exercise the actual popup import with a non-frame owner; direct
            // WM_COMMAND to frameWindow misses the viewport routing regression.
            call({{"op","select"},{"actors",J::array({mission})}});
            auto popupOwner=CreateWindowExA(0,"STATIC","Objective popup fixture",WS_POPUP,20,20,300,200,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
            ShowWindow(popupOwner,SW_SHOWNOACTIVATE);
            using Load=HMENU(WINAPI*)(HINSTANCE,LPCSTR);using Track=BOOL(WINAPI*)(HMENU,UINT,int,int,int,HWND,const RECT*);
            auto menu=(*reinterpret_cast<Load*>(0x11af23f0))(GetModuleHandle(nullptr),MAKEINTRESOURCEA(107));
            WorkflowProbe::objectivePopupChoice=40949;WorkflowProbe::objectivePopupFound=false;WorkflowProbe::objectivePopupTicks=0;
            auto popupTimer=SetTimer(nullptr,0,50,WorkflowProbe::ChooseObjectivePopup);
            auto popupResult=(*reinterpret_cast<Track*>(0x11af23c0))(GetSubMenu(menu,0),TPM_RIGHTBUTTON,40,40,0,popupOwner,nullptr);
            KillTimer(nullptr,popupTimer);DestroyMenu(menu);DestroyWindow(popupOwner);
            Record("objective_popup_result",(std::to_string(popupResult)+" ticks="+std::to_string(WorkflowProbe::objectivePopupTicks)).c_str());
            require(WorkflowProbe::objectivePopupFound && inspect(mission)["values"]["Objectives"].size()==3,"viewport-style popup owner routes Add SObjective without the frame dispatcher");
            Exec("TRANSACTION UNDO");
            call({{"op","select"},{"actors",J::array({objective})}});
            SendMessage(frameWindow,WM_COMMAND,40920,0);
            auto manager=WorkflowProbe::FindDialog("Gameplay Connections");
            require(manager && IsWindowEnabled(GetDlgItem(manager,202)),"connections manager enables objective creation for its current actor");
            SendMessage(manager,WM_COMMAND,203,0);auto objectiveGraph=WorkflowProbe::FindDialog("Gameplay Connection Graph");
            require(objectiveGraph!=nullptr,"objective graph opens");
            SendMessage(GetDlgItem(objectiveGraph,104),BM_SETCHECK,BST_UNCHECKED,0);
            call({{"op","select"},{"actors",J::array({mission})}});
            WorkflowProbe::objectivePopupChoice=40952;WorkflowProbe::objectivePopupFound=false;WorkflowProbe::objectivePopupTicks=0;
            popupTimer=SetTimer(nullptr,0,50,WorkflowProbe::ChooseObjectivePopup);
            SendMessage(GetDlgItem(objectiveGraph,120),WM_CONTEXTMENU,0,MAKELPARAM(-1,-1));KillTimer(nullptr,popupTimer);
            require(WorkflowProbe::objectivePopupFound && inspect(objective)["values"]["Triggers"].size()==4,"graph node popup adds linked flag pair to its node despite different viewport selection");
            Exec("TRANSACTION UNDO");SendMessage(objectiveGraph,WM_CLOSE,0,0);
            SendMessage(GetDlgItem(manager,103),BM_SETCHECK,BST_UNCHECKED,0);
            call({{"op","select"},{"actors",J::array({computer})}});SendMessage(manager,WM_COMMAND,200,0);
            require(IsWindowEnabled(GetDlgItem(manager,202)),"connections manager retains objective target with Follow Selection off");
            SendMessage(GetDlgItem(manager,103),BM_SETCHECK,BST_CHECKED,0);SendMessage(manager,WM_COMMAND,200,0);
            require(!IsWindowEnabled(GetDlgItem(manager,202)),"connections manager disables objective creation for unrelated actors");
            SendMessage(manager,WM_CLOSE,0,0);
            call({{"op","select"},{"actors",J::array()}});
        }
        {
            auto baseline=call({{"op","design.scene"}});
            require(Exec(std::string("MAP SAVE FILE=\"")+destination+"\"")!=0,"save disposable map before design metadata persistence checks");
            using DesignFilename=void(__thiscall*)(void*,const char*);
            reinterpret_cast<DesignFilename>(0x10e05e1c)(*reinterpret_cast<void**>(0x1165e80c),destination);
            std::ofstream(directory/"design_clearances.json")<<call({{"op","design.clearances"}}).dump(2);
            {
                // Diagnostic: the security-related actor classes this build
                // loads, with their reflected properties, for the security
                // tools in Map Design.
                J dump=J::object();
                for(auto& cls:call({{"op","magic.classes"}}))
                {
                    const auto name=cls["class"].get<std::string>();
                    bool security=false;
                    for(const char* key:{"Laser","Mine","Cam","Detector","Sensor","Alarm","Lift","Door","Sticky","Trigger","Switch","Light","Objective","Mission","Turret","Grenade",
                                         "GE","Ladder","Pipe","Zip","Fence","HandOver","PlayerStart","Rappel","Flag","Hoist","Rope","PawnAttaque","PAWNDEFENSE","Engine.Brush","Engine.Volume","LevelInfo","ZoneInfo","Engine.Mover","StaticMeshActor"})
                        if(name.find(key)!=std::string::npos)security=true;
                    if(!security)continue;
                    try
                    {
                        auto created=call({{"op","magic.create"},{"class",name},{"geometry",cls["brush"].get<bool>()?"box":"point"}});
                        auto info=call({{"op","magic.inspect"},{"actor",created}});
                        dump[name]={{"brush",cls["brush"]},{"schema",info["schema"]},{"values",info["values"]}};
                        Exec("TRANSACTION UNDO");
                    }
                    catch(const std::exception& e){dump[name]={{"error",e.what()}};}
                }
                std::ofstream(directory/"security_classes.json")<<dump.dump(2);
            }
            J spec={{"kind","Room"},{"width",512},{"length",768},{"height",256},{"thickness",16},{"steps",8},{"ceiling",true},{"name","Native design room"}};
            auto create=[&](J value,J previous=J{}){return call({{"op","design.block"},{"spec",value},{"position",{1024,2048,64}},{"rotation",{0,16384,0}},{"previous",previous}});};
            auto room=create(spec);require(room["members"].size()==7,"blockout creates subtractive space and six boundary brushes");
            auto live=call({{"op","design.scene"}});require(live.size()==baseline.size()+7,"blockout native actor count");
            for(auto& member:room["members"]){auto a=std::find_if(live.begin(),live.end(),[&](const J& item){return item["path"]==member["path"];});require(a!=live.end() && (*a)["edges"].size()==24,"blockout exports real native brush geometry");}
            spec["width"]=640;auto resized=create(spec,room);require(call({{"op","design.scene"}}).size()==live.size(),"dimension edit replaces brushes without accumulating duplicates");
            Exec("TRANSACTION UNDO");auto undone=call({{"op","design.scene"}});
            require(undone.size()==live.size(),"resize undoes as one transaction");
            for(auto& member:room["members"])require(std::any_of(undone.begin(),undone.end(),[&](const J& item){return item["path"]==member["path"];}),"resize undo restores original brush identities");
            Exec("TRANSACTION REDO");call({{"op","design.layer"},{"members",resized["members"]},{"hidden",true},{"locked",true}});
            auto hidden=call({{"op","design.scene"}});for(auto& member:resized["members"])for(auto& a:hidden)if(a["path"]==member["path"])require(a["hidden"]==true && a["locked"]==true,"native layer hides and locks actors");
            {
                // Locks, groups, group moves and lifts: each one Undo step, undone here.
                auto flagsOf=[&](const J& member){for(auto& a:call({{"op","design.scene"}}))if(a["path"]==member["path"])return a;return J{};};
                const J first=resized["members"][0];
                call({{"op","design.flags"},{"members",resized["members"]},{"hidden",0},{"locked",-1}});
                require(flagsOf(first)["hidden"]==false && flagsOf(first)["locked"]==true,"design.flags changes one flag and leaves the other");
                Exec("TRANSACTION UNDO");
                require(flagsOf(first)["hidden"]==true,"a flag change is one Undo step");
                call({{"op","design.group"},{"members",resized["members"]},{"group","Lobby"},{"action","add"}});
                require(flagsOf(first)["group"].get<std::string>().find("Lobby")!=std::string::npos,"design.group writes the map's own Group field");
                Exec("TRANSACTION UNDO");
                require(flagsOf(first)["group"].get<std::string>().find("Lobby")==std::string::npos,"a group change is one Undo step");
                const auto at=flagsOf(first)["position"].get<std::array<double,3>>();
                call({{"op","design.translate"},{"members",resized["members"]},{"delta",{64,0,0}}});
                require(std::abs(flagsOf(first)["position"].get<std::array<double,3>>()[0]-at[0])<.01,"design.translate leaves locked actors where they are");
                call({{"op","design.flags"},{"members",resized["members"]},{"hidden",-1},{"locked",0}});
                call({{"op","design.translate"},{"members",resized["members"]},{"delta",{64,0,0}}});
                require(std::abs(flagsOf(first)["position"].get<std::array<double,3>>()[0]-at[0]-64)<.01,"design.translate moves unlocked actors by the offset");
                Exec("TRANSACTION UNDO");
                require(std::abs(flagsOf(first)["position"].get<std::array<double,3>>()[0]-at[0])<.01,"a group move is one Undo step");
                Exec("TRANSACTION UNDO");
                require(flagsOf(first)["locked"]==true,"the unlock undoes too");
                const auto beforeLift=call({{"op","design.scene"}}).size();
                auto lift=call({{"op","design.lift"},{"position",{1024,3072,56}},{"rise",272}});
                auto liftInfo=call({{"op","magic.inspect"},{"actor",lift}});
                require(liftInfo["values"]["NumKeys"]=="2" && liftInfo["values"]["KeyPos"][1]["Z"].get<std::string>().rfind("272",0)==0,"design.lift makes a two-key mover rising by the floor spacing");
                require(liftInfo["values"]["InitialState"]=="StandOpenTimed","the lift moves when stood on");
                require(call({{"op","design.scene"}}).size()==beforeLift+1,"a lift is one actor");
                Exec("TRANSACTION UNDO");
                require(call({{"op","design.scene"}}).size()==beforeLift,"a lift is one Undo step");
            }
            Exec("TRANSACTION UNDO");Exec("TRANSACTION UNDO");Exec("TRANSACTION UNDO");require(call({{"op","design.scene"}}).size()==baseline.size(),"blockout undo returns to baseline");
            // Carve construction: one subtractive brush where the shell used seven.
            J carve=spec;carve["construction"]="Carve";carve["width"]=512;carve["name"]="Native carved room";
            auto carved=create(carve);
            require(carved["members"].size()==1,"carve construction places a single subtractive brush");
            auto carvedScene=call({{"op","design.scene"}});
            require(carvedScene.size()==baseline.size()+1,"carved room adds one native actor");
            {
                // A spiral stair, with its wedges, post and glide prisms, places and then moves as a whole.
                J spiral={{"kind","Spiral"},{"construction","Carve"},{"width",256},{"length",256},{"height",256},{"thickness",16},{"steps",12},{"ceiling",false},{"portal",false},{"name","Native spiral"}};
                auto placedSpiral=call({{"op","design.block"},{"spec",spiral},{"position",{2048,2048,0}},{"rotation",{0,0,0}}});
                require(placedSpiral["members"].size()==13,"a spiral places its treads and post");
                auto movedSpiral=call({{"op","design.block"},{"spec",spiral},{"position",{2304,2048,0}},{"rotation",{0,0,0}},{"previous",placedSpiral}});
                require(movedSpiral["members"].size()==13 && std::abs(movedSpiral["position"][0].get<double>()-2304)<.01,"a spiral moves to where it was dragged");
                // The move, the build and the placement, in that order.
                std::string trail="sizes: carved "+std::to_string(carvedScene.size());
                for(int step=0;step<5;++step)
                {
                    const auto size=call({{"op","design.scene"}}).size();
                    trail+=" -> "+std::to_string(size);
                    if(size==carvedScene.size())break;
                    Exec("TRANSACTION UNDO");
                }
                const std::string undoWhy="spiral placement and move undo cleanly ("+trail+")";
                require(call({{"op","design.scene"}}).size()==carvedScene.size(),undoWhy.c_str());
            }
            for(auto& a:carvedScene)if(a["path"]==carved["members"][0]["path"])require(a["edges"].size()==24,"carved room exports real brush geometry");
            // The brushes a piece generates must sit exactly where its preview
            // says, including at positions that are not on the editor grid.
            {
                J sync={{"kind","Room"},{"construction","Carve"},{"width",500},{"length",300},{"height",200},
                        {"thickness",16},{"steps",8},{"ceiling",true},{"name","Sync"}};
                const double x=1000.5,y=2000.25,z=64;
                auto piece=call({{"op","design.block"},{"spec",sync},{"position",{x,y,z}},{"rotation",{0,0,0}},{"previous",J{}}});
                auto extent=[&](const J& members,int axis)
                {
                    auto scene=call({{"op","design.scene"}});
                    double low=1e9,high=-1e9;
                    for(auto& member:members)
                        for(auto& actor:scene)
                            if(actor["path"]==member["path"])
                                for(auto& edge:actor["edges"])
                                    for(int end=0;end<2;++end)
                                    {
                                        // windows.h defines min/max macros in this probe.
                                        const double value=edge[end][axis].get<double>();
                                        if(value<low)low=value;
                                        if(value>high)high=value;
                                    }
                    return std::pair<double,double>{low,high};
                };
                auto [low,high]=extent(piece["members"],0);
                auto report="a piece's brushes match its preview (expected "+std::to_string(x-250)+" to "+std::to_string(x+250)
                    +", got "+std::to_string(low)+" to "+std::to_string(high)+")";
                require(std::abs(low-(x-250))<.01 && std::abs(high-(x+250))<.01,report.c_str());
                Exec("TRANSACTION UNDO");
                // A quarter-turned piece: its rotation lives in the brush shape,
                // not on the actor, because the geometry build ignores actor
                // Rotation and would otherwise build it sideways.
                auto turned=call({{"op","design.block"},{"spec",sync},{"position",{x,y,z}},{"rotation",{0,16384,0}},{"previous",J{}}});
                auto [turnedLow,turnedHigh]=extent(turned["members"],0);
                auto turnedReport="a quarter-turned piece is shaped in its polygons (expected "+std::to_string(x-150)+" to "+std::to_string(x+150)
                    +", got "+std::to_string(turnedLow)+" to "+std::to_string(turnedHigh)+")";
                require(std::abs(turnedLow-(x-150))<.01 && std::abs(turnedHigh-(x+150))<.01,turnedReport.c_str());
                for(auto& actor:call({{"op","design.scene"}}))
                    if(actor["path"]==turned["members"][0]["path"])
                        require(actor["rotation"]==J::array({0,0,0}),"a placed piece's brush actor carries no rotation for the build to ignore");
                Exec("TRANSACTION UNDO");
            }
            // A doorway can carry a zone portal sheet, which the editor counts
            // as a zone/portal brush.
            auto portalsBefore=call({{"op","brush.visibility"}})[0]["total"].get<int>();
            J doorway={{"kind","Doorway"},{"construction","Carve"},{"width",96},{"length",16},{"height",128},{"thickness",16},{"steps",8},{"portal",true},{"name","Native doorway"}};
            auto door=create(doorway);
            require(door["members"].size()==2,"portal doorway places its opening and a portal sheet");
            require(call({{"op","brush.visibility"}})[0]["total"].get<int>()==portalsBefore+1,"doorway portal sheet registers as a zone portal brush");
            Exec("TRANSACTION UNDO");
            require(call({{"op","brush.visibility"}})[0]["total"].get<int>()==portalsBefore,"portal doorway undoes as one transaction");
            // Layers reach the map's own Group field.
            call({{"op","design.layer"},{"members",carved["members"]},{"hidden",false},{"locked",false},{"group","Native_layer"},{"groupAction","add"}});
            auto grouped=call({{"op","design.scene"}});
            for(auto& member:carved["members"])
                for(auto& a:grouped)
                    if(a["path"]==member["path"])require(a["group"].get<std::string>().find("Native_layer")!=std::string::npos,"layer membership is written into the native Group field");
            call({{"op","design.layer"},{"members",carved["members"]},{"hidden",false},{"locked",false},{"group","Native_layer"},{"groupAction","remove"}});
            auto ungrouped=call({{"op","design.scene"}});
            for(auto& member:carved["members"])
                for(auto& a:ungrouped)
                    if(a["path"]==member["path"])require(a["group"]=="None","removing a layer clears its native group");
            Exec("TRANSACTION UNDO");Exec("TRANSACTION UNDO");Exec("TRANSACTION UNDO");
            require(call({{"op","design.scene"}}).size()==baseline.size(),"carved room, doorway and layer changes undo back to baseline");
            auto grid=call({{"op","design.grid"}});
            require(grid.size()==3 && grid[0].get<double>()>=0 && grid[0].get<double>()<=65536,"design views read the editor's grid spacing");
            spec["kind"]="Stairs";auto stairs=create(spec);require(stairs["members"].size()==9,"native stair brush generation: steps and their glide ramp");
            {
                // The ramp must be a semi-solid brush in the actor's own flags, or the BSP build swallows the steps.
                auto ramp=call({{"op","magic.inspect"},{"actor",stairs["members"].back()}});
                auto step=call({{"op","magic.inspect"},{"actor",stairs["members"][0]}});
                const std::string flagsWhy="the glide ramp brush is flagged invisible and semi-solid (ramp PolyFlags "+ramp["values"].value("PolyFlags",std::string("?"))+", step "+step["values"].value("PolyFlags",std::string("?"))+")";
                require(ramp["values"].value("PolyFlags",std::string())=="33" && step["values"].value("PolyFlags",std::string())=="0",flagsWhy.c_str());
            }
            Exec("TRANSACTION UNDO");
            spec["kind"]="Ramp";auto ramp=create(spec);auto rampScene=call({{"op","design.scene"}});J selected=J::array();for(auto& a:rampScene)if(a["path"]==ramp["members"][0]["path"])selected.push_back(a);
            require(selected.size()==1 && selected[0]["edges"].size()==18,"ramp imports five outward polygon faces");
            call({{"op","design.align"},{"scene",selected},{"axis",0},{"mode","Align"}});Exec("TRANSACTION UNDO");Exec("TRANSACTION UNDO");
            auto starts=call({{"op","design.spawns"}});
            if(starts.empty()){call({{"op","magic.create"},{"class","Engine.PlayerStart"}});starts=call({{"op","design.spawns"}});}
            require(!starts.empty(),"temporary spawn candidates available");
            call({{"op","design.play"},{"start",starts[0]},{"position",{300,400,500}},{"rotation",{0,16384,0}},{"launch",false}});
            require(call({{"op","design.spawns"}})==starts,"temporary playtest restores exact spawn positions and rotations");
            WorkflowProbe::designLaunchStarts=J::array();for(auto& start:starts)if(start["team"]==starts[0]["team"])WorkflowProbe::designLaunchStarts.push_back(start);
            auto launchSlot=reinterpret_cast<void**>(0x11af228c);auto originalLaunch=*launchSlot;DWORD protection=0;require(VirtualProtect(launchSlot,sizeof(void*),PAGE_READWRITE,&protection)!=0,"test launch interception available");
            auto minimizeSlot=reinterpret_cast<void**>(0x11af23cc);auto originalMinimize=*minimizeSlot;*minimizeSlot=reinterpret_cast<void*>(&WorkflowProbe::DesignKeepEditorWindow);
            *launchSlot=reinterpret_cast<void*>(&WorkflowProbe::DesignCaptureLaunch);auto playTimer=SetTimer(nullptr,0,50,WorkflowProbe::AcceptDesignPlay);
            try{call({{"op","design.play"},{"start",starts[0]},{"position",{300,400,500}},{"rotation",{0,16384,0}},{"launch",true}});}
            catch(...){KillTimer(nullptr,playTimer);*minimizeSlot=originalMinimize;*launchSlot=originalLaunch;DWORD ignored;VirtualProtect(launchSlot,sizeof(void*),protection,&ignored);throw;}
            KillTimer(nullptr,playTimer);*minimizeSlot=originalMinimize;*launchSlot=originalLaunch;DWORD ignored;VirtualProtect(launchSlot,sizeof(void*),protection,&ignored);
            require(WorkflowProbe::designLaunchSeen && WorkflowProbe::designLaunchPose,"native Play Level reaches launch with the temporary team spawn pose");
            require(call({{"op","design.spawns"}})==starts,"native Play Level returns with original editor spawns restored");
            // A team without a start gets a temporary one for the playtest only.
            auto temporary=call({{"op","design.tempstart"},{"class","Engine.PlayerStart"},{"team","1"},{"position",{600,700,800}},{"rotation",{0,16384,0}}});
            require(call({{"op","design.spawns"}}).size()==starts.size()+1,"temporary playtest start is created");
            call({{"op","design.play"},{"start",temporary},{"position",{300,400,500}},{"rotation",{0,16384,0}},{"launch",false}});
            call({{"op","design.removestart"},{"start",temporary}});
            require(call({{"op","design.spawns"}})==starts,"temporary playtest start is removed again, leaving the author's spawns");
            SendMessage(frameWindow,WM_COMMAND,40948,0);auto design=WorkflowProbe::FindDialog("Map Design");require(design!=nullptr,"Map Design menu opens native workspace");
            // Viewport cameras come and go as the editor works; count map actors.
            auto actorCount=[&]{size_t n=0;for(auto& a:call({{"op","actors"}}))if(a["class"].get<std::string>().find("Camera")==std::string::npos)++n;return n;};
            WorkflowProbe::Click(design,706); // New blockout, then verify a preview has no native actors.
            auto previewCount=actorCount();WorkflowProbe::Screenshot(design,directory/"map_design_preview.bmp");
            // The preview arrives through the inspector, with no dialog.
            auto field=[&](int id){char text[256]{};GetDlgItemTextA(design,id,text,sizeof(text));return std::string(text);};
            require(field(754).find("New preview")!=std::string::npos,"a new blockout opens in the inspector rather than a dialog");
            require(field(743)=="1024" && field(740)=="Blockout","inspector fields describe the live preview");
            SendMessage(design,WM_COMMAND,708,0);require(actorCount()==previewCount+1,"design preview applies through its actual Place button as one carved brush");
            require(field(754).find("Selected piece")!=std::string::npos,"placing hands the new piece straight back to the inspector");
            auto placedRoom=call({{"op","actors"},{"selected",true}});
            require(!placedRoom.empty(),"placing keeps the new piece selected for further editing");
            // The placed room stays selected: put a doorway in its wall.
            auto beforeDoorway=actorCount();
            WorkflowProbe::Click(design,724);
            WorkflowProbe::Click(design,708);
            require(actorCount()==beforeDoorway+2,"doorway in the selected room places its opening and zone portal");
            // Direct editing: select the room again, edit it through the
            // inspector and nudge it, each change replacing its brushes.
            {
                auto piecesOf=[&]{std::ifstream in(directory/"ReloadedEditor"/"library.json");J library;in>>library;return library["maps"][call({{"op","map"}})["key"].get<std::string>()]["design"]["pieces"];};
                call({{"op","select"},{"actors",placedRoom}});
                WorkflowProbe::Click(design,707);
                require(field(754).find("Selected piece")!=std::string::npos && field(743)=="1024","editing a placed piece loads it into the inspector without a dialog");
                auto before=actorCount();
                SetDlgItemTextA(design,743,"768");
                SendMessage(design,WM_COMMAND,MAKEWPARAM(743,EN_KILLFOCUS),reinterpret_cast<LPARAM>(GetDlgItem(design,743)));
                require(actorCount()==before,"an inspector edit replaces the piece's brushes instead of adding more");
                require(piecesOf().back()["spec"]["width"]==768,"an inspector edit reaches the placed piece");
                auto position=piecesOf().back()["position"][0].get<double>();
                auto grid=call({{"op","design.grid"}})[0].get<double>();
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_RIGHT,0);
                require(std::abs(piecesOf().back()["position"][0].get<double>()-(position+grid))<.001,"an arrow key nudges the piece by one grid step");
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                require(field(754).find("No preview")!=std::string::npos,"Escape ends the edit and clears the inspector");
                // A moved room takes the doorway in its wall with it, in one step.
                {
                    auto room=piecesOf();
                    size_t roomIndex=0,doorIndex=0;
                    for(size_t i=0;i<room.size();++i){if(room[i]["spec"]["kind"]=="Room")roomIndex=i;if(room[i]["spec"]["kind"]=="Doorway")doorIndex=i;}
                    call({{"op","select"},{"actors",room[roomIndex]["members"]}});
                    WorkflowProbe::Click(design,707);
                    const auto doorBefore=room[doorIndex]["position"][1].get<double>();
                    SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_UP,0);
                    auto moved=piecesOf();
                    bool doorFollowed=false;
                    for(auto& p:moved)if(p["spec"]["kind"]=="Doorway" && std::abs(p["position"][1].get<double>()-doorBefore-grid)<.001)doorFollowed=true;
                    require(doorFollowed,"a doorway in the wall moves with its room");
                    Exec("TRANSACTION UNDO");
                    SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                }
                // The panel's own Undo and Redo drive the editor's history. A
                // piece edit replaces its brushes, so compare the geometry
                // rather than how many actors there are.
                auto geometry=[&]{J list=J::array();for(auto& actor:call({{"op","design.scene"}}))list.push_back({{"path",actor["path"]},{"position",actor["position"]}});return list;};
                const auto edited=geometry();
                WorkflowProbe::Click(design,728);
                require(geometry()!=edited,"the panel's Undo reaches the editor's history");
                WorkflowProbe::Click(design,729);
                require(geometry()==edited,"the panel's Redo puts it back");
                Exec("TRANSACTION UNDO");Exec("TRANSACTION UNDO");
            }
            {
                // Clipboard: Copy takes the selected room, Paste lands a copy at
                // the cursor or at a right-clicked point, Duplicate one beside
                // the original; each placement is one Undo step. Delete takes
                // the copy's brushes away and keeps its library entry, so Undo
                // brings them back as that piece. A size preset in the
                // inspector resizes a placed piece in place.
                auto pieces=[&]{std::ifstream in(directory/"ReloadedEditor"/"library.json");J document;in>>document;return document["maps"][call({{"op","map"}})["key"].get<std::string>()]["design"]["pieces"];};
                auto status=[&]{char text[1024]{};GetWindowTextA(GetDlgItem(design,720),text,sizeof(text));return std::string(text);};
                const double grid=call({{"op","design.grid"}})[0].get<double>();
                SendMessage(design,WM_COMMAND,702,0); // Refresh from editor: the test pumps no timer.
                call({{"op","select"},{"actors",placedRoom}});
                SendMessage(design,WM_COMMAND,702,0);
                const auto actors0=actorCount();
                J original;
                for(auto& p:pieces())for(auto& member:p["members"])if(member["path"]==placedRoom[0]["path"])original=p;
                require(!original.is_null(),"the placed room is still a library piece");
                SendMessage(design,WM_COMMAND,800,0); // Copy selected pieces.
                require(status().find("Copied 1 piece")!=std::string::npos,("Copy reports the copied piece (status: "+status()+")").c_str());
                const auto view=call({{"op","design.view"}});
                const double zoom=view["zoom"],panX=view["panX"],panY=view["panY"];
                const double ox=original["position"][0].get<double>(),oy=original["position"][1].get<double>();
                const double px=ox+4096,py=oy+4096; // Clear of everything, on the grid.
                const POINT at{static_cast<LONG>(panX+px*zoom),static_cast<LONG>(panY-py*zoom)};
                auto removeCopy=[&](const J& copy)
                {
                    // Delete drops the copy's brushes and keeps its library
                    // entry; Undo brings the brushes back to that entry.
                    SendMessage(design,WM_COMMAND,702,0);
                    call({{"op","select"},{"actors",copy["members"]}});
                    SendMessage(design,WM_COMMAND,702,0);
                    SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_DELETE,0);
                    require(actorCount()==actors0,"Delete removes the copy's brushes");
                    auto listed=[&]{for(auto& p:pieces())if(p["members"]==copy["members"])return true;return false;};
                    require(listed(),"Delete keeps the copy in the library, for Undo");
                    Exec("TRANSACTION UNDO");
                    bool back=actorCount()==actors0+1;
                    const auto live=call({{"op","actors"}});
                    for(auto& member:copy["members"])
                    {
                        bool found=false;
                        for(auto& a:live)if(a["path"]==member["path"])found=true;
                        back=back && found;
                    }
                    require(back && listed(),"Undo brings the deleted copy's brushes back as the same piece");
                    Exec("TRANSACTION REDO");
                    require(actorCount()==actors0,"Redo deletes the copy again");
                    SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                };
                SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,0,MAKELPARAM(at.x,at.y));
                SendMessage(design,WM_COMMAND,801,0); // Paste at the cursor.
                require(actorCount()==actors0+1,("Paste places one copy of the copied room (status: "+status()+")").c_str());
                auto pasted=pieces().back();
                require(pasted["spec"]["kind"]=="Room" && std::abs(pasted["position"][0].get<double>()-px)<=grid+1 && std::abs(pasted["position"][1].get<double>()-py)<=grid+1,"the pasted room lands at the cursor, snapped to the grid");
                Exec("TRANSACTION UNDO");
                require(actorCount()==actors0,"a paste is one Undo step");
                Exec("TRANSACTION REDO");
                require(actorCount()==actors0+1,"Redo brings the pasted room back");
                removeCopy(pasted);
                // The right-click menu pastes at the clicked point.
                call({{"op","select"},{"actors",J::array()}});
                SendMessage(design,WM_COMMAND,702,0);
                WorkflowProbe::objectivePopupChoice=817; // Paste copied pieces here.
                WorkflowProbe::objectivePopupFound=false;WorkflowProbe::objectivePopupTicks=0;
                auto pastePopup=SetTimer(nullptr,0,50,WorkflowProbe::ChooseObjectivePopup);
                SendMessage(GetDlgItem(design,719),WM_RBUTTONDOWN,0,MAKELPARAM(at.x,at.y));
                KillTimer(nullptr,pastePopup);
                require(WorkflowProbe::objectivePopupFound,"the right-click menu offers to paste the copied piece");
                require(actorCount()==actors0+1,("Paste here places the copy (status: "+status()+")").c_str());
                pasted=pieces().back();
                require(std::abs(pasted["position"][0].get<double>()-px)<=grid+1 && std::abs(pasted["position"][1].get<double>()-py)<=grid+1,"the copy lands at the right-clicked point");
                removeCopy(pasted);
                // Duplicate puts the copy beside the original along X, a wall apart.
                call({{"op","select"},{"actors",placedRoom}});
                SendMessage(design,WM_COMMAND,702,0);
                SendMessage(design,WM_COMMAND,802,0); // Duplicate selected pieces beside.
                require(actorCount()==actors0+1,("Duplicate places one copy (status: "+status()+")").c_str());
                auto twin=pieces().back();
                const double width=original["spec"]["width"].get<double>(),thickness=original["spec"]["thickness"].get<double>();
                require(twin["position"][0].get<double>()>=ox+width && twin["position"][0].get<double>()<=ox+width+3*thickness+grid && std::abs(twin["position"][1].get<double>()-oy)<.001,"the duplicate sits beside the original, a wall apart");
                Exec("TRANSACTION UNDO");
                require(actorCount()==actors0,"a duplicate is one Undo step");
                Exec("TRANSACTION REDO");
                removeCopy(twin);
                // A rectangle dragged round the room selects its brushes.
                {
                    const double length=original["spec"]["length"].get<double>();
                    const POINT a{static_cast<LONG>(panX+(ox-width/2-40)*zoom),static_cast<LONG>(panY-(oy+length/2+40)*zoom)};
                    const POINT b{static_cast<LONG>(panX+(ox+width/2+40)*zoom),static_cast<LONG>(panY-(oy-length/2-40)*zoom)};
                    call({{"op","select"},{"actors",J::array()}});
                    SendMessage(design,WM_COMMAND,702,0);
                    SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(a.x,a.y));
                    SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(b.x,b.y));
                    SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(b.x,b.y));
                    const auto boxed=call({{"op","actors"},{"selected",true}});
                    require(!boxed.empty(),("a rectangle dragged round a room selects its brushes (status: "+status()+")").c_str());
                    SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                    call({{"op","select"},{"actors",J::array()}});
                }
                // The settings sheets read the LevelInfo and the mission through reflection.
                {
                    auto title=[&]{char text[512]{};GetDlgItemTextA(design,754,text,sizeof(text));return std::string(text);};
                    SendMessage(design,WM_COMMAND,812,0); // Level settings.
                    require(title().find("Level settings")!=std::string::npos && GetDlgItem(design,1000)!=nullptr,("Level settings opens a sheet with rows (title: "+title()+"; status: "+status()+")").c_str());
                    SendMessage(design,WM_COMMAND,813,0); // Environment.
                    require(title().find("Environment")!=std::string::npos && GetDlgItem(design,1000)!=nullptr,("the environment sheet has rows (status: "+status()+")").c_str());
                    SendMessage(design,WM_COMMAND,814,0); // Map settings (mission).
                    const bool mission=title().find("Map settings")!=std::string::npos;
                    require(mission || status().find("no SMission")!=std::string::npos,("map settings open the mission or say there is none (status: "+status()+")").c_str());
                    SendMessage(design,WM_COMMAND,702,0);
                }
                // A preset resizes the placed piece in place.
                call({{"op","select"},{"actors",placedRoom}});
                WorkflowProbe::Click(design,707);
                require(SendDlgItemMessage(design,807,CB_GETCOUNT,0,0)>1,"the inspector lists size presets for a room");
                SendDlgItemMessage(design,807,CB_SETCURSEL,3,0); // Medium room 768 x 768 x 256.
                SendMessage(design,WM_COMMAND,MAKEWPARAM(807,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(design,807)));
                require(actorCount()==actors0,("a preset replaces the piece's brushes instead of adding more (status: "+status()+")").c_str());
                const auto resized=pieces().back();
                require(resized["spec"]["width"]==768 && resized["spec"]["length"]==768 && resized["spec"]["height"]==256,"a preset sets the placed piece's size");
                Exec("TRANSACTION UNDO");
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                // A sightline through the room reports itself in the status line.
                SendMessage(design,WM_COMMAND,702,0);
                SetDlgItemTextA(design,721,std::to_string(static_cast<int>(original["position"][2].get<double>())).c_str()); // The plan's depth: the room's floor.
                SendMessage(design,WM_COMMAND,MAKEWPARAM(721,EN_KILLFOCUS),reinterpret_cast<LPARAM>(GetDlgItem(design,721)));
                SendMessage(design,WM_COMMAND,806,0); // Sightline between two points.
                const POINT eye{static_cast<LONG>(panX+(ox-width/4)*zoom),static_cast<LONG>(panY-oy*zoom)},look{static_cast<LONG>(panX+(ox+width/4)*zoom),static_cast<LONG>(panY-oy*zoom)};
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(eye.x,eye.y));
                SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(eye.x,eye.y));
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(look.x,look.y));
                SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(look.x,look.y));
                require(status().find("Clear line of sight")!=std::string::npos,("a sightline inside one room is clear (status: "+status()+")").c_str());
                {
                    std::ifstream in(directory/"ReloadedEditor"/"library.json");J document;in>>document;
                    auto annotations=document["maps"][call({{"op","map"}})["key"].get<std::string>()]["design"]["annotations"];
                    require(!annotations.empty() && annotations.back()["kind"]=="Sightline","the sightline is kept as an annotation");
                }
                WorkflowProbe::Click(design,728); // Undo: the workspace change comes first.
            }
            {
                // Editor extras: the frame's selection and visibility commands,
                // and an autosave copy that leaves the map's own file alone.
                auto actors=call({{"op","design.scene"}}); // Every actor but the cameras, with its hidden flag.
                std::map<std::string,int> counts;
                for(auto& a:actors)++counts[a["class"].get<std::string>()];
                std::string cls;int best=0;
                for(auto& [c,n]:counts)if(n>best){best=n;cls=c;}
                require(best>=2,"the fixture has a class with several actors");
                J one=J::array();
                for(auto& a:actors)if(a["class"]==cls){one.push_back(a);break;}
                call({{"op","select"},{"actors",one}});
                SendMessage(frameWindow,WM_COMMAND,40954,0); // RE+: Select > All of this class.
                auto selected=call({{"op","actors"},{"selected",true}});
                require(static_cast<int>(selected.size())==best,("Select all of this class selects every actor of the class ("+std::to_string(selected.size())+" of "+std::to_string(best)+" "+cls+")").c_str());
                for(auto& a:selected)require(a["class"]==cls,"Select all of this class selects nothing else");
                SendMessage(frameWindow,WM_COMMAND,40956,0); // Invert selection.
                auto inverted=call({{"op","actors"},{"selected",true}});
                for(auto& a:inverted)require(a["class"]!=cls,("Invert selection drops the class ("+a["path"].get<std::string>()+" stayed)").c_str());
                require(!inverted.empty(),"Invert selection selects the rest");
                call({{"op","select"},{"actors",one}});
                SendMessage(frameWindow,WM_COMMAND,40957,0); // Hide selected.
                bool hidden=false;
                for(auto& a:call({{"op","design.scene"}}))if(a["path"]==one[0]["path"])hidden=a["hidden"].get<bool>();
                require(hidden,"Hide selected hides the actor in the editor");
                SendMessage(frameWindow,WM_COMMAND,40959,0); // Unhide all.
                hidden=true;
                for(auto& a:call({{"op","design.scene"}}))if(a["path"]==one[0]["path"])hidden=a["hidden"].get<bool>();
                require(!hidden,"Unhide all shows it again");
                call({{"op","select"},{"actors",J::array()}});
                require(!call({{"op","map.file"}}).get<std::string>().empty(),"the frame reports the map's file for the recent list");
            }
            {
                // Quick add: hover the wall of a placed piece and pick from the
                // badge menu. The view mapping tells the test where that wall is.
                // The panel refreshes on its timer; this test pumps no messages,
                // so refresh explicitly after the undo above.
                SendMessage(design,WM_COMMAND,702,0);
                SendMessage(design,WM_COMMAND,701,0); // Fit map / preview.
                auto view=call({{"op","design.view"}});
                require(view["open"]==true,"the design view reports its mapping");
                call({{"op","select"},{"actors",placedRoom}});
                WorkflowProbe::Click(design,707);
                auto library=[&]{std::ifstream in(directory/"ReloadedEditor"/"library.json");J document;in>>document;return document["maps"][call({{"op","map"}})["key"].get<std::string>()]["design"];};
                // The piece being edited is the one whose brushes are selected,
                // not necessarily the newest entry in the library.
                J piece;
                const J libraryPieces=library()["pieces"]; // Iterating a temporary's member dangles.
                for(auto& candidate:libraryPieces)
                    for(auto& member:candidate["members"])
                        for(auto& actor:placedRoom)
                            if(member["path"]==actor["path"])piece=candidate;
                require(!piece.is_null(),"the edited room is found in the library");
                const double zoom=view["zoom"],panX=view["panX"],panY=view["panY"];
                const double width=piece["spec"]["width"].get<double>();
                // The middle of the piece's +X wall, in canvas coordinates.
                const double wallX=piece["position"][0].get<double>()+width/2,wallY=piece["position"][1].get<double>();
                POINT at{static_cast<LONG>(panX+wallX*zoom),static_cast<LONG>(panY-wallY*zoom)};
                SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,0,MAKELPARAM(at.x,at.y));
                // The badge sits outside the wall, so the wall itself stays
                // clickable: a click there selects the piece, not the menu.
                const auto badge=call({{"op","design.view"}})["badge"];
                require(!badge.is_null() && badge["wall"]=="+X","hovering a wall reports its quick-add badge");
                require(badge["x"].get<int>()>at.x+15 && std::abs(badge["y"].get<int>()-at.y)<3,"the badge sits outside the hovered wall rather than under the cursor");
                // Moving the cursor from the wall onto the badge keeps it showing.
                SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,0,MAKELPARAM(badge["x"].get<int>(),badge["y"].get<int>()));
                require(call({{"op","design.view"}})["badge"]==badge,"the badge stays while the cursor moves onto it");
                SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,0,MAKELPARAM(at.x,at.y));
                auto before=actorCount();
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                call({{"op","select"},{"actors",J::array()}});
                WorkflowProbe::objectivePopupChoice=731; // Corridor from this wall.
                WorkflowProbe::objectivePopupFound=false;WorkflowProbe::objectivePopupTicks=0;
                auto popup=SetTimer(nullptr,0,50,WorkflowProbe::ChooseObjectivePopup);
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(at.x,at.y));
                SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(at.x,at.y));
                KillTimer(nullptr,popup);
                require(!WorkflowProbe::objectivePopupFound,"clicking the wall itself does not open the quick-add menu");
                {
                    // The panel's status line explains a click that selected nothing.
                    char statusText[1024]{};GetWindowTextA(GetDlgItem(design,720),statusText,sizeof(statusText));
                    const std::string why=std::string("clicking the wall selects the piece (status: ")+statusText+"; at "+std::to_string(at.x)+","+std::to_string(at.y)+"; view "+call({{"op","design.view"}}).dump()+")";
                    require(!call({{"op","actors"},{"selected",true}}).empty(),why.c_str());
                }
                SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,0,MAKELPARAM(at.x,at.y));
                WorkflowProbe::objectivePopupFound=false;WorkflowProbe::objectivePopupTicks=0;
                popup=SetTimer(nullptr,0,50,WorkflowProbe::ChooseObjectivePopup);
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(badge["x"].get<int>(),badge["y"].get<int>()));
                KillTimer(nullptr,popup);
                require(WorkflowProbe::objectivePopupFound,"the quick-add badge opens its menu on the hovered wall");
                WorkflowProbe::Click(design,708);
                require(actorCount()==before+1,"the quick-add corridor is placed from the menu");
                auto added=library()["pieces"].back();
                require(added["spec"]["kind"]=="Corridor","the menu choice decides the shape");
                const double expected=piece["position"][0].get<double>()+width/2+piece["spec"]["thickness"].get<double>()+added["spec"]["length"].get<double>()/2;
                require(std::abs(added["position"][0].get<double>()-expected)<1,"the new corridor meets the outside of that wall");
                // The plan knows its built BSP is out of date, and B rebuilds it.
                require(view["open"]==true && call({{"op","design.view"}})["stale"]==true,"the design view reports brushes changed since the last geometry build");
                const auto builds=call({{"op","design.builds"}}).get<unsigned long>();
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,'B',0);
                require(call({{"op","design.builds"}}).get<unsigned long>()>builds,"B in the design view runs a geometry build");
                require(call({{"op","design.view"}})["stale"]==false,"a completed build makes the plan current again");
                // The design check lists real problems and finds none once they are fixed.
                WorkflowProbe::Click(design,761);
                auto check=WorkflowProbe::FindDialog("Design Check");
                require(check!=nullptr,"the design check opens its own window");
                auto rows=static_cast<int>(SendMessage(GetDlgItem(check,780),LB_GETCOUNT,0,0));
                char firstRow[512]{};SendMessageA(GetDlgItem(check,780),LB_GETTEXT,0,reinterpret_cast<LPARAM>(firstRow));
                require(rows>=1 && call({{"op","design.view"}})["issues"].get<size_t>()==static_cast<size_t>(rows),"the design check lists each issue once");
                DestroyWindow(check);
                Exec("TRANSACTION UNDO");
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
            }
            {
                // Zones: a mission with two objectives and their terminals, a
                // door and a switchable light; a two-zone plan gives each zone
                // its own mission under the map's, in one Undo step, reads back,
                // and the Zones window lists it.
                auto inspect=[&](const J& actor){return call({{"op","magic.inspect"},{"actor",actor}});};
                auto stageOf=[&](const J& identity){for(auto& a:call({{"op","stage.actors"}}))if(a["path"]==identity["path"])return a;return J{};};
                // Earlier tests leave missions behind; one mission keeps Objective
                // here a top-level menu entry the popup chooser can pick.
                for(auto& a:call({{"op","objective.actors"}}))if(a["kind"]=="Mission")call({{"op","security.delete"},{"actor",{{"path",a["path"]},{"class",a["class"]}}}});
                auto mission=call({{"op","magic.create"},{"class","SBase.SMission"}});
                auto first=call({{"op","objective.add"},{"owner",inspect(mission)},{"class","SBase.SObjective"}}).at(0);
                auto second=call({{"op","objective.add"},{"owner",inspect(mission)},{"class","SBase.SObjective"}}).at(0);
                auto terminalA=call({{"op","objective.add"},{"owner",inspect(first)},{"class","SBase.SComputerObjectiveTrigger"}}).at(0);
                auto terminalB=call({{"op","objective.add"},{"owner",inspect(second)},{"class","SBase.SComputerObjectiveTrigger"}}).at(0);
                auto door=call({{"op","security.create"},{"class","Engine.Mover"},{"position",{1300,2000,64}}});
                auto lamp=call({{"op","security.create"},{"class","SBase.STriggerLight"},{"position",{1300,2100,200}}});
                require(stageOf(first)["kind"]=="Objective" && stageOf(terminalB)["kind"]=="Objective trigger" && stageOf(door)["kind"]=="Mover" && stageOf(lamp)["kind"]=="Light","the stage inventory classifies the actors");
                {
                    // Objective here on the plan: the right-click menu adds an
                    // objective under the map's one mission, linked and placed.
                    int missions=0;for(auto& a:call({{"op","objective.actors"}}))if(a["kind"]=="Mission")++missions;
                    require(missions==1,"the stage fixture has the map's only mission");
                    {
                        const auto linked=inspect(mission)["values"]["Objectives"].size();
                        const auto count=actorCount();
                        const auto view=call({{"op","design.view"}});
                        const double zoom=view["zoom"],panX=view["panX"],panY=view["panY"];
                        POINT at{static_cast<LONG>(panX+1500*zoom),static_cast<LONG>(panY-1900*zoom)};
                        SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                        WorkflowProbe::objectivePopupChoice=983; // Objective here (under the mission).
                        WorkflowProbe::objectivePopupFound=false;WorkflowProbe::objectivePopupTicks=0;
                        auto popup=SetTimer(nullptr,0,50,WorkflowProbe::ChooseObjectivePopup);
                        SendMessage(GetDlgItem(design,719),WM_RBUTTONDOWN,0,MAKELPARAM(at.x,at.y));
                        KillTimer(nullptr,popup);
                        require(WorkflowProbe::objectivePopupFound,"the right-click menu offers Objective here");
                        require(actorCount()==count+1 && inspect(mission)["values"]["Objectives"].size()==linked+1,"Objective here adds an objective linked to the mission");
                        // Placement is the link, then a move that may be refused: undo
                        // only as far as the objective's own steps.
                        for(int step=0;step<2 && actorCount()>count;++step)Exec("TRANSACTION UNDO");
                        require(actorCount()==count && inspect(mission)["values"]["Objectives"].size()==linked,"the placed objective undoes");
                    }
                }
                const auto before=actorCount();
                J plan={{"format","scct.stages"},{"version",2},{"zones",J::array({
                    {{"name","Docks"},{"objectives",J::array({first["path"]})},{"required",0},{"actions",J::array({
                        {{"kind","open"},{"target",door["path"]},{"delay","1.5"}},
                        {{"kind","light"},{"target",lamp["path"]}},
                        {{"kind","announce"},{"title","Zone 2"},{"merc","East wing open"},{"spy","East wing open"},{"seconds","6"}}})}},
                    {{"objectives",J::array({second["path"]})},{"required",0},{"actions",J::array()}}})}};
                auto preview=call({{"op","stage.preview"},{"plan",plan}});
                require(preview["creates"].get<size_t>()==4 && actorCount()==before,"stage.preview reports the batch without touching the map");
                call({{"op","stage.apply"},{"plan",plan}});
                require(actorCount()==before+4,"stage.apply creates a mission per zone, the first zone's event and its announcement");
                auto missionValues=inspect(mission)["values"];
                require(missionValues["Objectives"].size()==2 && missionValues["bChained"]=="True","the map's mission now holds both zones and runs them in order");
                require(missionValues["MinimumObjectives"]=="2","the spies win after the objectives both zones' thresholds add up to");
                J zone1,zone2,complete,announce;
                for(auto& a:call({{"op","stage.actors"}}))
                {
                    if(a["tag"]=="Zone1Mission")zone1=a;if(a["tag"]=="Zone2Mission")zone2=a;
                    if(a["tag"]=="Zone1_Complete")complete=a;if(a["tag"]=="Zone1_Announce")announce=a;
                }
                require(!zone1.is_null() && zone1["kind"]=="Mission" && zone1["objectives"].size()==1 && zone1["objectives"][0]==first["path"],"the first zone's mission holds its objective");
                require(zone1["chained"]==false && zone1["mode"]=="GM_Undefined" && zone1["minimum"]=="1" && zone1["name"]=="Docks","a zone's objectives are all available at once and it is not a game mode of its own");
                require(zone1["event"]=="Zone1_Complete" && zone2["event"]=="None","a zone with actions fires its completion event; one without fires nothing");
                auto actions=complete["groups"][0]["EventGroup"];
                require(actions.size()==3 && actions[0]["Event"]==stageOf(door)["tag"] && std::stod(actions[0]["Delay"].get<std::string>())==1.5 && actions[1]["Event"]==stageOf(lamp)["tag"] && actions[2]["Event"]=="Zone1_Announce","the completion opens, lights and announces");
                require(stageOf(terminalA)["usable"]==true && stageOf(terminalB)["usable"]==true,"no terminal is locked: the chained mission decides when a zone is live");
                require(stageOf(door)["state"]=="TriggerToggle","the door stays open once triggered");
                require(announce["kind"]=="Alarm" && announce["title"]=="Zone 2" && announce["spy"]=="East wing open" && std::stod(announce["duration"].get<std::string>())==6,"the announcement is an alarm with the texts and duration");
                auto readBack=call({{"op","stage.read"}});
                require(readBack["zones"].size()==2 && readBack["zones"][0]["objectives"][0]==first["path"] && readBack["zones"][0]["name"]=="Docks" && readBack["zones"][0]["actions"].size()==3 && readBack["zones"][0]["actions"][2]["kind"]=="announce" && readBack["zones"][0]["actions"][2]["title"]=="Zone 2","the plan reads back from the map");
                auto again=call({{"op","stage.preview"},{"plan",readBack}});
                require(again["creates"].get<size_t>()==0 && again["updates"].get<size_t>()==0,"a map matching its plan needs no changes");
                Exec("TRANSACTION UNDO");
                require(actorCount()==before && inspect(mission)["values"]["Objectives"].size()==2 && inspect(mission)["values"]["Objectives"][0].get<std::string>().find("SObjective")!=std::string::npos,"one Undo removes the zone missions and gives the objectives back to the map's mission");
                Exec("TRANSACTION REDO");
                require(actorCount()==before+4 && inspect(mission)["values"]["bChained"]=="True","Redo restores the zones");
                // The Zones window lists the plan the map carries.
                WorkflowProbe::Click(design,768);
                auto stages=WorkflowProbe::FindDialog("Zones");
                require(stages!=nullptr,"the Zones menu item opens its window");
                require(SendMessage(GetDlgItem(stages,1000),LB_GETCOUNT,0,0)==2,"the window lists both zones");
                require(SendMessage(GetDlgItem(stages,1009),LB_GETCOUNT,0,0)==3,"the first zone shows its three actions");
                SendMessage(GetDlgItem(stages,1000),LB_SETCURSEL,1,0);
                SendMessage(stages,WM_COMMAND,MAKEWPARAM(1000,LBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(stages,1000)));
                require(SendMessage(GetDlgItem(stages,1009),LB_GETCOUNT,0,0)==0,"the last zone has no actions");
                DestroyWindow(stages);
                for(int i=0;i<8;++i)Exec("TRANSACTION UNDO");
                require(actorCount()==before-7,"the stage fixture is undone");
            }
            {
                // Security devices: the ops create and wire them; the Security
                // window places them from clicks in the plan.
                auto securityOf=[&](const J& identity){for(auto& a:call({{"op","security.actors"}}))if(a["path"]==identity["path"])return a;return J{};};
                auto alarm=call({{"op","security.create"},{"class","SBase.SAlarm"},{"position",{1500,2600,128}},{"properties",{{"AlarmName","\"Native alarm\""}}}});
                auto laser=call({{"op","security.create"},{"class","SBase.SLaserTrigger"},{"position",{1200,2600,110}},{"rotation",{0,16384,0}},{"properties",{{"LaserLength","640"}}}});
                require(securityOf(alarm)["kind"]=="Alarm" && securityOf(laser)["kind"]=="Laser","security.create makes actors the inventory recognises");
                require(securityOf(laser)["length"].get<double>()==640,"the laser keeps its LaserLength");
                require(securityOf(alarm)["tag"].get<std::string>()!="None" && !securityOf(alarm)["tag"].get<std::string>().empty(),"a new alarm gets a Tag straight away");
                call({{"op","security.link"},{"detector",laser},{"alarm",alarm}});
                require(securityOf(laser)["event"]==securityOf(alarm)["tag"],"linking sets the detector Event to the alarm Tag");
                auto sensor=call({{"op","security.sensor"},{"low",{800,2200,64}},{"high",{1100,2500,160}}});
                require(securityOf(sensor["sensor"])["kind"]=="Motion sensor" && securityOf(sensor["sensor"])["volumes"].size()==1,"a motion sensor arrives with its own volume");
                call({{"op","security.outputs"},{"alarm",alarm},{"targets",J::array({laser})}});
                require(securityOf(alarm)["events"].size()==1 && securityOf(alarm)["events"][0]==securityOf(laser)["tag"],"alarm outputs name the targets' Tags");
                // The Security window: an alarm from one click, then a laser
                // from two clicks, wired to that alarm automatically.
                WorkflowProbe::Click(design,763);
                auto security=WorkflowProbe::FindDialog("Security");
                require(security!=nullptr,"the Security button opens its window");
                require(SendMessage(GetDlgItem(security,800),LB_GETCOUNT,0,0)>=3,"the wiring list shows the alarm, its laser and its output");
                auto before=actorCount();
                WorkflowProbe::Click(security,807);
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(120,140));
                require(actorCount()==before+1,"one click places an alarm");
                require(SendMessage(GetDlgItem(security,801),CB_GETCURSEL,0,0)>=1,"the new alarm is chosen for new detectors");
                WorkflowProbe::Click(security,802);
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(200,200));
                require(actorCount()==before+1,"the first laser click only marks the start");
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(300,200));
                require(actorCount()==before+2,"the second click places the laser");
                J placedLaser,placedAlarm;
                for(auto& a:call({{"op","security.actors"}})){if(a["kind"]=="Laser" && a["path"]!=laser["path"])placedLaser=a;if(a["kind"]=="Alarm" && a["path"]!=alarm["path"])placedAlarm=a;}
                require(!placedLaser.is_null() && !placedAlarm.is_null(),"the placed laser and alarm are in the inventory");
                require(placedLaser["event"]==placedAlarm["tag"],"a laser placed in the plan is wired to the chosen alarm");
                const double zoom=call({{"op","design.view"}})["zoom"];
                require(std::abs(placedLaser["length"].get<double>()-100/zoom)<=33,"the laser length spans the two clicks (to the snap grid)");
                require(placedLaser["rotation"][1].get<int>()==0,"a beam drawn towards +X points east");
                WorkflowProbe::Click(security,804);
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(150,300));
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(350,400));
                require(actorCount()==before+4,"two corner clicks place a motion sensor and its volume");
                SendMessage(GetDlgItem(design,719),WM_RBUTTONDOWN,0,MAKELPARAM(10,10));
                require(SendMessage(GetDlgItem(security,800),LB_GETCOUNT,0,0)>=7,"the wiring list follows the placed devices");
                // Devices move and turn: through the op, then by dragging in the plan.
                call({{"op","security.move"},{"actor",laser},{"position",{1300,2700,110}},{"rotation",{0,32768,0}},{"properties",{{"LaserLength","512"}}}});
                auto moved=securityOf(laser);
                require(moved["position"][0].get<double>()==1300 && moved["rotation"][1].get<int>()==32768 && moved["length"].get<double>()==512,"security.move sets position, direction and length in one step");
                Exec("TRANSACTION UNDO");
                require(securityOf(laser)["position"][0].get<double>()==1200 && securityOf(laser)["length"].get<double>()==640,"a device move is one Undo step");
                const auto sensorBefore=securityOf(sensor["sensor"])["position"].get<std::array<double,3>>();
                call({{"op","security.move"},{"actor",sensor["sensor"]},{"position",{sensorBefore[0]+96,sensorBefore[1],sensorBefore[2]}}});
                J volumeActor;for(auto& a:call({{"op","actors"}}))if(a["path"]==securityOf(sensor["sensor"])["volumes"][0])volumeActor=a;
                require(!volumeActor.is_null(),"the sensor's volume is still referenced after a move");
                Exec("TRANSACTION UNDO");
                {
                    const auto view2=call({{"op","design.view"}});
                    const double zoom2=view2["zoom"],panX2=view2["panX"],panY2=view2["panY"];
                    const auto laserAt=placedLaser["position"].get<std::array<double,3>>();
                    POINT body{static_cast<LONG>(panX2+laserAt[0]*zoom2),static_cast<LONG>(panY2-laserAt[1]*zoom2)};
                    SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(body.x,body.y));
                    SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(body.x,body.y+60));
                    SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(body.x,body.y+60));
                    J draggedLaser;for(auto& a:call({{"op","security.actors"}}))if(a["path"]==placedLaser["path"])draggedLaser=a;
                    require(draggedLaser["position"][1].get<double>()<laserAt[1]-30,"dragging a laser in the plan moves it");
                    require(draggedLaser["position"][0].get<double>()==laserAt[0],"a drag keeps the other axis");
                    // Dragging a device that is not selected moves it alone, whatever
                    // else is selected; dragging a selected device brings the rest of
                    // the selection with it.
                    {
                        const auto alarmAt=securityOf(alarm)["position"].get<std::array<double,3>>();
                        const auto soloAt=draggedLaser["position"].get<std::array<double,3>>();
                        call({{"op","select"},{"actors",J::array({alarm,placedAlarm})}});
                        SendMessage(design,WM_COMMAND,702,0);
                        POINT solo{static_cast<LONG>(panX2+soloAt[0]*zoom2),static_cast<LONG>(panY2-soloAt[1]*zoom2)};
                        SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(solo.x,solo.y));
                        SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(solo.x,solo.y+40));
                        SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(solo.x,solo.y+40));
                        require(securityOf(placedLaser)["position"][1].get<double>()<soloAt[1]-20,"an unselected laser still drags");
                        require(securityOf(alarm)["position"].get<std::array<double,3>>()==alarmAt,"dragging an unselected device leaves the selected actors where they are");
                        Exec("TRANSACTION UNDO");
                        call({{"op","select"},{"actors",J::array({alarm,placedLaser})}});
                        SendMessage(design,WM_COMMAND,702,0);
                        SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(solo.x,solo.y));
                        SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(solo.x,solo.y+40));
                        SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(solo.x,solo.y+40));
                        require(securityOf(alarm)["position"][1].get<double>()<alarmAt[1]-20,"dragging a selected device brings the rest of the selection along");
                        Exec("TRANSACTION UNDO");
                        call({{"op","select"},{"actors",J::array()}});
                        SendMessage(design,WM_COMMAND,702,0);
                        draggedLaser=securityOf(placedLaser);
                    }
                    // The aim handle at the beam end turns and stretches the beam.
                    const auto newAt=draggedLaser["position"].get<std::array<double,3>>();
                    const double length=draggedLaser["length"];
                    POINT tip{static_cast<LONG>(panX2+(newAt[0]+length)*zoom2),static_cast<LONG>(panY2-newAt[1]*zoom2)};
                    SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(tip.x,tip.y));
                    SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(body.x,tip.y+120));
                    SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(body.x,tip.y+120));
                    for(auto& a:call({{"op","security.actors"}}))if(a["path"]==placedLaser["path"])draggedLaser=a;
                    require(std::abs(draggedLaser["rotation"][1].get<int>()-49152)<1500,"dragging the beam end aims the laser south");
                    require(std::abs(draggedLaser["length"].get<double>()-120/zoom2)<=48,"dragging the beam end sets the laser length");
                    // In the front view the same handle sets the pitch.
                    call({{"op","security.move"},{"actor",placedLaser},{"position",newAt},{"rotation",{0,0,0}},{"properties",{{"LaserLength","256"}}}});
                    SendMessage(design,WM_COMMAND,702,0); // Refresh from editor: the test pumps no timer.
                    SendDlgItemMessage(design,700,CB_SETCURSEL,1,0);
                    SendMessage(design,WM_COMMAND,MAKEWPARAM(700,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(design,700)));
                    const auto front=call({{"op","design.view"}});
                    require(front["plane"]==1,"the plane combo switches the design view to the front");
                    const double fz=front["zoom"],fx=front["panX"],fy=front["panY"];
                    POINT ftip{static_cast<LONG>(fx+(newAt[0]+256)*fz),static_cast<LONG>(fy-newAt[2]*fz)};
                    SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(ftip.x,ftip.y));
                    SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(ftip.x,ftip.y-static_cast<LONG>(256*fz)));
                    SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(ftip.x,ftip.y-static_cast<LONG>(256*fz)));
                    for(auto& a:call({{"op","security.actors"}}))if(a["path"]==placedLaser["path"])draggedLaser=a;
                    require(std::abs(draggedLaser["rotation"][0].get<int>()-8192)<900 && draggedLaser["rotation"][1].get<int>()==0,"dragging the beam end in the front view pitches the laser up");
                    require(std::abs(draggedLaser["length"].get<double>()-362)<48,"a pitched beam keeps its full length");
                    SendDlgItemMessage(design,700,CB_SETCURSEL,0,0);
                    SendMessage(design,WM_COMMAND,MAKEWPARAM(700,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(design,700)));
                    require(call({{"op","design.view"}})["plane"]==0,"the design view returns to the top");
                }
                // Unwiring and deleting through the ops, then from the right-click menu.
                call({{"op","security.unlink"},{"detector",placedLaser}});
                require(securityOf(placedLaser)["event"]=="None","security.unlink clears the detector's Event");
                Exec("TRANSACTION UNDO");
                require(securityOf(placedLaser)["event"]==placedAlarm["tag"],"unwiring is one Undo step");
                auto mine=call({{"op","security.create"},{"class","SBase.SMineProx"},{"position",{1400,2400,64}}});
                const auto beforeDelete=actorCount();
                call({{"op","security.delete"},{"actor",mine}});
                require(securityOf(mine).is_null() && actorCount()==beforeDelete-1,"security.delete removes the device");
                Exec("TRANSACTION UNDO");
                require(!securityOf(mine).is_null(),"a delete is one Undo step");
                {
                    SendMessage(design,WM_COMMAND,702,0); // Refresh from editor: the test pumps no timer.
                    const auto view3=call({{"op","design.view"}});
                    const double zoom3=view3["zoom"],panX3=view3["panX"],panY3=view3["panY"];
                    const auto mineAt=securityOf(mine)["position"].get<std::array<double,3>>();
                    POINT at3{static_cast<LONG>(panX3+mineAt[0]*zoom3),static_cast<LONG>(panY3-mineAt[1]*zoom3)};
                    SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,0,MAKELPARAM(at3.x,at3.y));
                    WorkflowProbe::objectivePopupChoice=865; // Delete this device.
                    WorkflowProbe::objectivePopupFound=false;WorkflowProbe::objectivePopupTicks=0;
                    auto popup3=SetTimer(nullptr,0,50,WorkflowProbe::ChooseObjectivePopup);
                    SendMessage(GetDlgItem(design,719),WM_RBUTTONDOWN,0,MAKELPARAM(at3.x,at3.y));
                    KillTimer(nullptr,popup3);
                    require(WorkflowProbe::objectivePopupFound,"right-clicking a device offers to delete it");
                    require(securityOf(mine).is_null(),"the menu deletes the device");
                    // Right-clicking an alarm makes it the one new detectors wire to.
                    const auto alarmAt=securityOf(alarm)["position"].get<std::array<double,3>>();
                    POINT at4{static_cast<LONG>(panX3+alarmAt[0]*zoom3),static_cast<LONG>(panY3-alarmAt[1]*zoom3)};
                    SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,0,MAKELPARAM(at4.x,at4.y));
                    WorkflowProbe::objectivePopupChoice=868; // Use this alarm for new detectors.
                    WorkflowProbe::objectivePopupFound=false;WorkflowProbe::objectivePopupTicks=0;
                    popup3=SetTimer(nullptr,0,50,WorkflowProbe::ChooseObjectivePopup);
                    SendMessage(GetDlgItem(design,719),WM_RBUTTONDOWN,0,MAKELPARAM(at4.x,at4.y));
                    KillTimer(nullptr,popup3);
                    require(WorkflowProbe::objectivePopupFound,"right-clicking an alarm offers wiring choices");
                    require(SendMessage(GetDlgItem(security,801),CB_GETCURSEL,0,0)==1,"the first alarm in the map is now chosen for new detectors");
                }
                DestroyWindow(security);
            }
            {
                // Brushes moved in the editor keep their piece: the library
                // follows them, so the outline appears on the brushes.
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                auto library=[&]{std::ifstream in(directory/"ReloadedEditor"/"library.json");J document;in>>document;return document["maps"][call({{"op","map"}})["key"].get<std::string>()]["design"];};
                J followed;
                {
                    const J libraryPieces=library()["pieces"];
                    for(auto& candidate:libraryPieces)
                        for(auto& member:candidate["members"])
                            for(auto& actor:placedRoom)
                                if(member["path"]==actor["path"])followed=candidate;
                }
                require(!followed.is_null(),"the placed room is still in the library");
                const auto was=followed["position"].get<std::array<double,3>>();
                // Move each brush as the editor would, outside the toolkit.
                size_t movedMembers=0;
                for(auto& member:followed["members"])
                    for(auto& item:call({{"op","design.scene"}}))
                        if(item["path"]==member["path"])
                        {
                            auto position=item["position"].get<std::array<double,3>>();
                            position[0]+=128;
                            call({{"op","security.move"},{"actor",member},{"position",position},{"rotation",item["rotation"]}});
                            ++movedMembers;
                        }
                require(movedMembers==followed["members"].size(),"every brush of the room was moved");
                SendMessage(design,WM_COMMAND,702,0); // Refresh from editor.
                J after;
                {
                    const J libraryPieces=library()["pieces"];
                    for(auto& candidate:libraryPieces)if(candidate["members"]==followed["members"])after=candidate;
                }
                require(!after.is_null() && std::abs(after["position"][0].get<double>()-(was[0]+128))<0.1 && after["position"][1]==was[1],"a piece moved in the editor follows its brushes in the library");
                call({{"op","select"},{"actors",placedRoom}});
                WorkflowProbe::Click(design,707);
                require(std::abs(std::stod(field(750))-(was[0]+128))<0.1,"editing that piece shows the outline on the moved brushes");
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                for(size_t i=0;i<movedMembers;++i)Exec("TRANSACTION UNDO");
                SendMessage(design,WM_COMMAND,702,0);
                {
                    const J libraryPieces=library()["pieces"];
                    for(auto& candidate:libraryPieces)if(candidate["members"]==followed["members"])after=candidate;
                }
                require(std::abs(after["position"][0].get<double>()-was[0])<0.1,"undoing the editor move brings the piece back too");
            }
            {
                // Right-click on the plan offers actions at that point.
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                require(GetMenu(design)!=nullptr && GetMenuItemCount(GetMenu(design))==4,"the design window has a menu bar in place of most side buttons");
                require(GetDlgItem(design,703)==nullptr && GetDlgItem(design,706)!=nullptr,"rarely used actions left the side panel for the menus");
                WorkflowProbe::objectivePopupChoice=859; // New corridor here.
                WorkflowProbe::objectivePopupFound=false;WorkflowProbe::objectivePopupTicks=0;
                auto popup=SetTimer(nullptr,0,50,WorkflowProbe::ChooseObjectivePopup);
                SendMessage(GetDlgItem(design,719),WM_RBUTTONDOWN,0,MAKELPARAM(80,560));
                KillTimer(nullptr,popup);
                require(WorkflowProbe::objectivePopupFound,"right-click in the design view opens its context menu");
                require(field(754).find("New preview")!=std::string::npos && field(741)=="Corridor","the context menu starts a corridor preview at that point");
                const auto view=call({{"op","design.view"}});
                const double expectedX=(80-view["panX"].get<double>())/view["zoom"].get<double>();
                require(std::abs(std::stod(field(750))-expectedX)<=64,"the preview sits where the plan was right-clicked (to the snap grid)");
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                require(field(754).find("New preview")==std::string::npos,"Escape discards the preview from the menu");
            }
            // A workspace edit that nothing native followed is what Undo takes back.
            WorkflowProbe::Click(design,710);
            {
                auto guides=[&]{std::ifstream in(directory/"ReloadedEditor"/"library.json");J document;in>>document;return document["maps"][call({{"op","map"}})["key"].get<std::string>()]["design"]["guides"].size();};
                const auto added=guides();
                require(added>=1,"the player reference dialog adds a reference");
                WorkflowProbe::Click(design,728);
                require(guides()==added-1,"Undo takes back a workspace change made after the last map change");
                WorkflowProbe::Click(design,729);
                require(guides()==added,"Redo restores the workspace change");
            }
            WorkflowProbe::Click(design,710); // Configurable player clearance guide.
            SendMessage(design,WM_COMMAND,711,0);auto canvas=GetDlgItem(design,719);
            SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(100,100));SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(300,100));
            WorkflowProbe::Click(design,716);SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(200,220));SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(400,300));SendMessage(design,WM_COMMAND,717,0);
            // Groups are made in the Scene panel: open it, choose every row,
            // New group; the name dialog's default name is accepted.
            call({{"op","select"},{"actors",placedRoom}});
            HWND scene=FindWindowExA(design,nullptr,"ReloadedScene",nullptr);
            require(scene!=nullptr,"the Scene panel is docked inside the design window from the start");
            if(!scene){SendMessage(design,WM_COMMAND,765,0);scene=FindWindowExA(design,nullptr,"ReloadedScene",nullptr);}
            if(scene)
            {
                auto sceneList=GetDlgItem(scene,1200);
                LVITEMA everyRow{};everyRow.stateMask=LVIS_SELECTED;everyRow.state=LVIS_SELECTED;
                SendMessageA(sceneList,LVM_SETITEMSTATE,static_cast<WPARAM>(-1),reinterpret_cast<LPARAM>(&everyRow));
                KillTimer(scene,2);
                WorkflowProbe::Click(scene,1206); // New group from selection.
                char sceneStatus[1024]{};GetWindowTextA(GetDlgItem(design,720),sceneStatus,sizeof(sceneStatus));
                const std::string why=std::string("the Scene panel groups the chosen rows (status: ")+sceneStatus+")";
                require(std::string(sceneStatus).find("Grouped")!=std::string::npos,why.c_str());
                SendMessage(scene,WM_CLOSE,0,0);
            }
            // Generate a tiny reference image locally; no copyrighted reference
            // imagery or user's files are needed for this UI test.
            auto image=directory/"design_reference.bmp";BITMAPFILEHEADER bh{};BITMAPINFOHEADER bi{};bi.biSize=sizeof(bi);bi.biWidth=64;bi.biHeight=64;bi.biPlanes=1;bi.biBitCount=32;bh.bfType=0x4d42;bh.bfOffBits=sizeof(bh)+sizeof(bi);bh.bfSize=bh.bfOffBits+64*64*4;
            {std::ofstream out(image,std::ios::binary);out.write(reinterpret_cast<char*>(&bh),sizeof(bh));out.write(reinterpret_cast<char*>(&bi),sizeof(bi));for(int y=0;y<64;++y)for(int x=0;x<64;++x){unsigned colour=(x%16<2||y%16<2)?0x00406080:0x00f0e0c0;out.write(reinterpret_cast<char*>(&colour),4);}}
            WorkflowProbe::jsonDialogPath=image.wstring();WorkflowProbe::jsonDialogCancel=false;
            auto fileTimer=SetTimer(nullptr,0,50,WorkflowProbe::AnswerJsonDialog);auto formTimer=SetTimer(nullptr,0,60,WorkflowProbe::Answer);SendMessage(design,WM_COMMAND,703,0);KillTimer(nullptr,fileTimer);KillTimer(nullptr,formTimer);
            SendMessage(design,WM_COMMAND,704,0);SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(100,100));
            formTimer=SetTimer(nullptr,0,50,WorkflowProbe::Answer);SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(300,100));KillTimer(nullptr,formTimer);
            WorkflowProbe::Screenshot(design,directory/"map_design_workspace.bmp");
            // Grid snapping, floor filter and movement limits report through the
            // status line of the real panel.
            auto status=[&]{char text[1024]{};GetWindowTextA(GetDlgItem(design,720),text,sizeof(text));return std::string(text);};
            SendMessage(GetDlgItem(design,722),BM_CLICK,0,0);
            require(status().find("Snapping off")!=std::string::npos,"the snap checkbox turns grid snapping off");
            SendMessage(GetDlgItem(design,722),BM_CLICK,0,0);
            require(status().find("snap to the editor's grid")!=std::string::npos,"grid snapping can be turned back on");
            WorkflowProbe::Click(design,723);
            require(status().find("Showing actors")!=std::string::npos,"the floor filter limits the view to one height range");
            WorkflowProbe::Click(design,725);
            require(status().find("Movement limits saved")!=std::string::npos,"movement limits are saved for this map");
            // Dragging empty space box-selects; a rectangle over nothing clears
            // the selection rather than picking the nearest actor.
            SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(12,12));
            SendMessage(canvas,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(40,40));
            SendMessage(canvas,WM_LBUTTONUP,0,MAKELPARAM(40,40));
            require(status().find("Nothing touched by the rectangle")!=std::string::npos,"an empty rectangle selects nothing");
            require(call({{"op","actors"},{"selected",true}}).empty(),"box selection replaces the editor selection");
            // Two teams' routes, then their timing comparison.
            WorkflowProbe::routeTeamChoice=1;WorkflowProbe::Click(design,716,"Spy route");
            SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(120,140));SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(420,340));WorkflowProbe::Click(design,717);
            WorkflowProbe::routeTeamChoice=2;WorkflowProbe::Click(design,716,"Merc route");
            SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(140,160));SendMessage(canvas,WM_LBUTTONDOWN,0,MAKELPARAM(300,260));WorkflowProbe::Click(design,717);
            WorkflowProbe::routeTeamChoice=0;
            WorkflowProbe::Click(design,726);
            require(status().find("(spy)")!=std::string::npos && status().find("(merc)")!=std::string::npos && status().find("arrives")!=std::string::npos,"route comparison reports both teams' timings and who arrives first");
            // Portable workspace files, as used by map packaging.
            auto workspaceFile=directory/"design_workspace.json";
            WorkflowProbe::jsonDialogPath=workspaceFile.wstring();WorkflowProbe::jsonDialogCancel=false;WorkflowProbe::jsonDialogTicks=0;WorkflowProbe::workspaceChoice=0;
            {auto file=SetTimer(nullptr,0,50,WorkflowProbe::AnswerJsonDialog);auto form=SetTimer(nullptr,0,60,WorkflowProbe::Answer);SendMessage(design,WM_COMMAND,727,0);KillTimer(nullptr,file);KillTimer(nullptr,form);}
            {
                std::ifstream input(workspaceFile);J workspace;input>>workspace;
                require(workspace["version"]==1 && workspace["design"]["references"].size()==1 && workspace["design"]["annotations"].size()==4,"exported workspace carries this map's references and annotations");
            }
            require(std::filesystem::exists(directory/"ReloadedEditor"/"Workspaces"/(std::filesystem::path(destination).stem().string()+".json")),"a workspace copy is kept for map packaging");
            WorkflowProbe::workspaceChoice=1;WorkflowProbe::jsonDialogTicks=0;
            {auto file=SetTimer(nullptr,0,50,WorkflowProbe::AnswerJsonDialog);auto form=SetTimer(nullptr,0,60,WorkflowProbe::Answer);SendMessage(design,WM_COMMAND,727,0);KillTimer(nullptr,file);KillTimer(nullptr,form);}
            require(status().find("Workspace imported")!=std::string::npos,"a workspace file imports back into the map");
            {
                // A placed room turned into a spiral in the inspector, then dragged
                // in the plan: the piece keeps up with the drag like any other.
                SendMessage(design,WM_COMMAND,702,0);
                call({{"op","select"},{"actors",placedRoom}});
                WorkflowProbe::Click(design,707);
                SendMessage(GetDlgItem(design,741),CB_SETCURSEL,8,0); // Spiral.
                SendMessage(design,WM_COMMAND,MAKEWPARAM(741,CBN_SELCHANGE),reinterpret_cast<LPARAM>(GetDlgItem(design,741)));
                char shapeStatus[1024]{};GetWindowTextA(GetDlgItem(design,720),shapeStatus,sizeof(shapeStatus));
                const std::string shapeWhy=std::string("the inspector turns a placed room into a spiral (status: ")+shapeStatus+")";
                require(field(741)=="Spiral" && std::string(shapeStatus).find("not changed")==std::string::npos,shapeWhy.c_str());
                const double x0=std::stod(field(750)),y0=std::stod(field(751));
                const auto spiralView=call({{"op","design.view"}});
                const double sz=spiralView["zoom"],spx=spiralView["panX"],spy=spiralView["panY"];
                // Inside the spiral's footprint but off its centre, where a player guide may sit.
                const POINT from{static_cast<LONG>(spx+(x0+300)*sz),static_cast<LONG>(spy-(y0-300)*sz)},to{from.x+static_cast<LONG>(std::lround(256*sz)),from.y};
                SendMessage(GetDlgItem(design,719),WM_LBUTTONDOWN,0,MAKELPARAM(from.x,from.y));
                SendMessage(GetDlgItem(design,719),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(to.x,to.y));
                SendMessage(GetDlgItem(design,719),WM_LBUTTONUP,0,MAKELPARAM(to.x,to.y));
                char dragStatus[1024]{};GetWindowTextA(GetDlgItem(design,720),dragStatus,sizeof(dragStatus));
                const std::string dragWhy=std::string("dragging a spiral in the plan moves it (status: ")+dragStatus+"; base X "+field(750)+" from "+std::to_string(x0)+")";
                require(std::stod(field(750))>x0+128 && std::string(dragStatus).find("not changed")==std::string::npos,dragWhy.c_str());
                // R turns the edited piece a quarter turn, replacing its brushes.
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,'R',0);
                char turnStatus[1024]{};GetWindowTextA(GetDlgItem(design,720),turnStatus,sizeof(turnStatus));
                const std::string turnWhy=std::string("R turns the edited piece 90 degrees (status: ")+turnStatus+"; yaw "+field(753)+")";
                require(field(753)=="90" && std::string(turnStatus).find("not changed")==std::string::npos,turnWhy.c_str());
                // A geometry build recomputes polygon normals and texture axes (and
                // resets the editor's undo history): the piece must still be
                // recognised as the toolkit's own afterwards.
                call({{"op","design.build"}});
                SendMessage(design,WM_COMMAND,702,0);
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,'R',0);
                char builtStatus[1024]{};GetWindowTextA(GetDlgItem(design,720),builtStatus,sizeof(builtStatus));
                const std::string builtWhy=std::string("a spiral is still editable after a geometry build (status: ")+builtStatus+"; yaw "+field(753)+")";
                require(field(753)=="180" && std::string(builtStatus).find("not changed")==std::string::npos,builtWhy.c_str());
                {
                    // A stair placed before its ramp flags reached the actor is
                    // repaired before the next build: knock the flags off and ask.
                    J spiralMembers;
                    {
                        std::ifstream in(directory/"ReloadedEditor"/"library.json");J document;in>>document;
                        for(auto& candidate:document["maps"][call({{"op","map"}})["key"].get<std::string>()]["design"]["pieces"])
                            if(candidate["spec"]["kind"]=="Spiral")
                            {
                                size_t live=0;const auto sceneNow=call({{"op","design.scene"}});
                                for(auto& member:candidate["members"])for(auto& actor:sceneNow)if(actor["path"]==member["path"]){++live;break;}
                                if(live==candidate["members"].size())spiralMembers=candidate["members"];
                            }
                    }
                    require(!spiralMembers.is_null(),"the spiral is a whole piece in the library");
                    if(!spiralMembers.is_null())
                    {
                        J wrong=J::array();for(size_t i=0;i<spiralMembers.size();++i)wrong.push_back(33);
                        call({{"op","design.polyflags"},{"members",spiralMembers},{"flags",wrong}});
                        const auto repaired=call({{"op","design.repairflags"}}).get<size_t>();
                        const auto after=call({{"op","design.polyflags"},{"members",spiralMembers}});
                        size_t plain=0;for(auto& f:after)if(!f.is_null() && f.get<unsigned>()==0)++plain;
                        const std::string repairWhy="repairing piece flags puts a spiral's brushes back to plain ("+std::to_string(repaired)+" repaired, "+std::to_string(plain)+" plain of "+std::to_string(spiralMembers.size())+" brushes)";
                        require(repaired==spiralMembers.size() && plain==spiralMembers.size(),repairWhy.c_str());
                    }
                    // What the BSP build keeps: every step of a spiral and of a
                    // straight stair must still own faces, or it is invisible in game.
                    J straight={{"kind","Stairs"},{"construction","Carve"},{"width",128},{"length",512},{"height",256},{"thickness",16},{"steps",16},{"ceiling",false},{"portal",false},{"name","Built stairs"}};
                    auto builtStairs=call({{"op","design.block"},{"spec",straight},{"position",{4096,4096,0}},{"rotation",{0,0,0}}});
                    // A room carved over the stairs later in the actor order hollows them out of the build.
                    J hollow={{"kind","Room"},{"construction","Carve"},{"width",512},{"length",768},{"height",512},{"thickness",16},{"steps",8},{"ceiling",true},{"portal",false},{"name","Carved after"}};
                    call({{"op","design.block"},{"spec",hollow},{"position",{4096,4096,0}},{"rotation",{0,0,0}}});
                    call({{"op","design.build"}});
                    auto stepsWithFaces=[&]
                    {
                        const auto owners=call({{"op","design.bspowners"}});
                        size_t with=0;
                        for(size_t i=0;i+1<builtStairs["members"].size();++i)if(owners.value(builtStairs["members"][i]["path"].get<std::string>(),0)>0)++with;
                        return with;
                    };
                    const size_t before=stepsWithFaces();
                    const auto moved=call({{"op","design.sendtolast"},{"members",builtStairs["members"]}}).get<size_t>();
                    call({{"op","design.build"}});
                    const size_t after=stepsWithFaces();
                    const std::string orderWhy="stairs sent to last in the actor order keep every step's faces ("+std::to_string(before)+" steps had faces before, "+std::to_string(after)+" of "+std::to_string(builtStairs["members"].size()-1)+" after, "+std::to_string(moved)+" moved)";
                    require(moved>0 && after==builtStairs["members"].size()-1,orderWhy.c_str());
                    const auto owners=call({{"op","design.bspowners"}});
                    auto facesOf=[&](const J& member){return owners.value(member["path"].get<std::string>(),0);};
                    if(!spiralMembers.is_null())
                    {
                        // Treads come first, then the post.
                        const size_t wedges=spiralMembers.size()-1;
                        size_t spiralSteps=0,spiralMissing=0;
                        for(size_t i=0;i<wedges;++i){if(facesOf(spiralMembers[i])>0)++spiralSteps;else ++spiralMissing;}
                        const std::string spiralWhy="a spiral's wedges keep their faces after a build ("+std::to_string(spiralSteps)+" with faces, "+std::to_string(spiralMissing)+" without, post faces "+std::to_string(facesOf(spiralMembers[wedges]))+")";
                        require(spiralMissing==0,spiralWhy.c_str());
                    }
                }
                SendMessage(GetDlgItem(design,719),WM_KEYDOWN,VK_ESCAPE,0);
                SendMessage(design,WM_COMMAND,702,0);
            }
            auto key=call({{"op","map"}})["key"].get<std::string>();std::ifstream libraryInput(directory/"ReloadedEditor"/"library.json");J designLibrary;libraryInput>>designLibrary;
            auto savedDesign=designLibrary["maps"][key]["design"];
            require(savedDesign["annotations"].size()==4 && savedDesign["layers"].size()==1 && savedDesign["guides"].size()==2 && savedDesign["references"].size()==1 && savedDesign.contains("movement"),"design references, measurements, routes, guides, layers and movement limits persist from actual dialogs");
            DestroyWindow(design);Exec("TRANSACTION UNDO");
            Record("workflow_check","Map Design native blockout, layers, spawn restoration and UI passed");
        }
        {
            // A brush from the map, whatever the design tests left selected,
            // so the mixed-selection check below always mixes a brush with the mesh.
            J originalSelection=J::array();
            for(auto& a:call({{"op","actors"}}))if(a["class"]=="Engine.Brush" && a.value("authorable",false)){originalSelection.push_back(a);break;}
            require(!originalSelection.empty(),"the fixture has a brush for the mixed-selection check");
            require(Exec("OBJ LOAD FILE=\"..\\Packages\\StaticMeshes\\TestMapStaticM.usx\"")!=0,"load mesh fixture package");
            auto assets=call({{"op","magic.assets"},{"type","StaticMesh"}});require(!assets.empty(),"mesh fixture asset available");
            auto mesh=call({{"op","magic.create"},{"class","Engine.StaticMeshActor"}});
            auto native=WorkflowProbe::MagicActor(mesh);
            auto field=[&](const char* name)->unsigned char* {
                for(auto type=*reinterpret_cast<unsigned char**>(native+0x24);type;type=*reinterpret_cast<unsigned char**>(type+0x28))
                    for(auto p=*reinterpret_cast<unsigned char**>(type+0x58);p;p=*reinterpret_cast<unsigned char**>(p+0x40))
                        if(!strcmp(ObjectName(p),name))return native+*reinterpret_cast<int*>(p+0x3c);
                throw std::runtime_error(std::string("Mesh fixture property missing: ")+name);
            };
            auto wanted=assets[0]["path"].get<std::string>();auto objects=*reinterpret_cast<unsigned char***>(0x11697b70);int objectCount=*reinterpret_cast<int*>(0x11697b74);unsigned char* meshObject=nullptr;
            for(int i=0;i<objectCount;++i)if(objects[i]){std::string path;for(auto object=objects[i];object;object=*reinterpret_cast<unsigned char**>(object+0x18))path=std::string(ObjectName(object))+(path.empty()?"":"."+path);if(path==wanted){meshObject=objects[i];break;}}
            require(meshObject!=nullptr,"resolve fixture mesh object");*reinterpret_cast<void**>(field("StaticMesh"))=meshObject;
            auto vector=[&](const char* name,float x,float y,float z){float value[3]={x,y,z};memcpy(field(name),value,sizeof(value));};
            vector("Location",0,0,0);vector("PrePivot",0,0,0);vector("DrawScale3D",1,1,1);*reinterpret_cast<float*>(field("DrawScale"))=1;
            memset(field("Rotation"),0,12);
            call({{"op","select"},{"actors",J::array({mesh})}});
            auto local=call({{"op","mesh.bounds"}});
            vector("Location",200,-100,75);vector("PrePivot",3,5,7);vector("DrawScale3D",-1,0.5f,1.5f);*reinterpret_cast<float*>(field("DrawScale"))=2;
            int rotation[3]={0,16384,0};memcpy(field("Rotation"),rotation,sizeof(rotation));
            auto bounds=call({{"op","mesh.bounds"}});
            // Independent quarter-turn oracle: world=(200-y, -100-2*x, 75+3*z), after subtracting PrePivot.
            double lo[3]={200-(local["max"][1].get<double>()-5),-100-2*(local["max"][0].get<double>()-3),75+3*(local["min"][2].get<double>()-7)};
            double hi[3]={200-(local["min"][1].get<double>()-5),-100-2*(local["min"][0].get<double>()-3),75+3*(local["max"][2].get<double>()-7)};
            for(int axis=0;axis<3;++axis)require(std::abs(bounds["min"][axis].get<double>()-lo[axis])<0.01 && std::abs(bounds["max"][axis].get<double>()-hi[axis])<0.01,"mesh bounds include rotation, signed nonuniform scale and pivot");
            auto level=*reinterpret_cast<unsigned char**>(*reinterpret_cast<uintptr_t*>(kEditor)+0x130);
            auto builder=(*reinterpret_cast<unsigned char***>(level+0x2c))[1];
            auto brushState=[&](){
                J value={{"Location",{{"X",std::to_string(*reinterpret_cast<float*>(builder+0x80))},{"Y",std::to_string(*reinterpret_cast<float*>(builder+0x84))},{"Z",std::to_string(*reinterpret_cast<float*>(builder+0x88))}}}};
                for(auto type=*reinterpret_cast<unsigned char**>(builder+0x24);type;type=*reinterpret_cast<unsigned char**>(type+0x28))
                    for(auto p=*reinterpret_cast<unsigned char**>(type+0x58);p;p=*reinterpret_cast<unsigned char**>(p+0x40)){
                        std::string name=ObjectName(p);if(name!="Rotation" && name!="PrePivot" && name!="DrawScale" && name!="DrawScale3D" && name!="MainScale" && name!="PostScale")continue;
                        auto at=builder+*reinterpret_cast<int*>(p+0x3c);auto size=*reinterpret_cast<unsigned short*>(p+0x32);value[name]=std::vector<unsigned char>(at,at+size);
                    }return value;
            };
            auto brushText=[&](){auto file=directory/"builder-fit.t3d";require(Exec("BRUSH EXPORT FILE=\""+file.string()+"\"")!=0,"export builder brush for verification");std::ifstream in(file);return std::string(std::istreambuf_iterator<char>(in),{});};
            auto before=brushState();auto beforeText=brushText();auto viewBefore=call({{"op","view.capture"}})["cameras"];
            using Load=HMENU(WINAPI*)(HINSTANCE,LPCSTR);
            auto menu=(*reinterpret_cast<Load*>(0x11af23f0))(GetModuleHandle(nullptr),MAKEINTRESOURCEA(107));
            require(menu && GetMenuState(GetSubMenu(menu,0),40933,MF_BYCOMMAND)!=UINT(-1),"static mesh context menu contains builder fit action");DestroyMenu(menu);
            auto currentMaterial=reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(kEditor)+0x138);
            auto oldMaterial=*currentMaterial;*currentMaterial=0;
            SendMessage(frameWindow,WM_COMMAND,40933,0);
            *currentMaterial=oldMaterial;
            auto after=brushState();auto afterText=brushText();
            require(afterText!=beforeText,"builder fit replaces brush geometry");
            size_t materialCount=0;for(size_t pos=0;(pos=afterText.find("Texture=Engine.DefaultTexture",pos))!=std::string::npos;++pos)++materialCount;
            require(materialCount==6,"builder fit assigns default texture to every face when no material is selected");
            require(call({{"op","actors"},{"selected",true}})[0]["path"]==mesh["path"] && call({{"op","view.capture"}})["cameras"]==viewBefore,"builder fit preserves mesh selection and viewport cameras");
            for(int axis=0;axis<3;++axis){const char* key=axis==0?"X":axis==1?"Y":"Z";require(std::abs(std::stod(after["Location"][key].get<std::string>())-(lo[axis]+hi[axis])*0.5)<0.01,"builder centered on mesh bounds");}
            std::istringstream lines(afterText);std::string line;int vertices=0;
            while(std::getline(lines,line)){auto pos=line.find("Vertex");if(pos==std::string::npos)continue;auto xyz=line.substr(pos+6);std::replace(xyz.begin(),xyz.end(),',',' ');std::istringstream values(xyz);double x,y,z;if(values>>x>>y>>z){double v[3]={x,y,z};for(int axis=0;axis<3;++axis)require(std::abs(std::abs(v[axis])-(hi[axis]-lo[axis])*0.5)<0.02,"builder vertices match fitted half extents");++vertices;}}
            require(vertices==24,"builder fit creates six quad faces");
            require(Exec("TRANSACTION UNDO")!=0,"undo builder fit");require(brushState()==before && brushText()==beforeText,"one undo restores builder geometry and transform");
            require(Exec("TRANSACTION REDO")!=0,"redo builder fit");require(brushState()==after && brushText()==afterText,"redo restores fitted brush");
            Exec("TRANSACTION UNDO");
            call({{"op","select"},{"actors",J::array()}});bool rejected=false;try{call({{"op","builder.fit"}});}catch(const std::exception&){rejected=true;}
            require(rejected && brushState()==before && brushText()==beforeText,"empty selection rejected without changing builder");
            auto mixed=originalSelection;mixed.push_back(mesh);call({{"op","select"},{"actors",mixed}});
            rejected=false;try{call({{"op","builder.fit"}});}catch(const std::exception&){rejected=true;}
            require(rejected && brushState()==before && brushText()==beforeText,"mixed mesh and brush selection rejected without edits");
            menu=(*reinterpret_cast<Load*>(0x11af23f0))(GetModuleHandle(nullptr),MAKEINTRESOURCEA(107));
            require(menu && GetMenuState(GetSubMenu(menu,0),40933,MF_BYCOMMAND)==UINT(-1),"builder fit omitted from non-mesh context menu");DestroyMenu(menu);
            auto second=call({{"op","magic.create"},{"class","Engine.StaticMeshActor"}});native=WorkflowProbe::MagicActor(second);
            *reinterpret_cast<void**>(field("StaticMesh"))=meshObject;vector("Location",-200,300,400);vector("PrePivot",0,0,0);vector("DrawScale3D",1,1,1);*reinterpret_cast<float*>(field("DrawScale"))=1;memset(field("Rotation"),0,12);
            call({{"op","select"},{"actors",J::array({mesh,second})}});auto combined=call({{"op","mesh.bounds"}});
            require(combined["count"]==2,"builder fit includes both selected meshes");
            const double offset[3]={-200,300,400};
            for(int axis=0;axis<3;++axis){lo[axis]=(std::min)(lo[axis],local["min"][axis].get<double>()+offset[axis]);hi[axis]=(std::max)(hi[axis],local["max"][axis].get<double>()+offset[axis]);require(std::abs(combined["min"][axis].get<double>()-lo[axis])<0.01 && std::abs(combined["max"][axis].get<double>()-hi[axis])<0.01,"multiple mesh bounds form their union");}
            uintptr_t selectedMaterial=0;
            for(int i=0;i<objectCount;++i)if(objects[i] && !strcmp(ObjectName(objects[i]),"BETON_Lit_Spec")){selectedMaterial=reinterpret_cast<uintptr_t>(objects[i]);break;}
            require(selectedMaterial!=0,"selected material fixture available");*currentMaterial=selectedMaterial;
            call({{"op","builder.fit"}});auto together=brushState();*currentMaterial=oldMaterial;
            auto textured=brushText();require(textured.find("Texture=TXT_INI.BSP.BETON_Lit_Spec")!=std::string::npos,"builder fit uses selected material when available");
            for(int axis=0;axis<3;++axis){const char* key=axis==0?"X":axis==1?"Y":"Z";require(std::abs(std::stod(together["Location"][key].get<std::string>())-(lo[axis]+hi[axis])*0.5)<0.01,"builder centers on all selected meshes");}
            require(call({{"op","actors"},{"selected",true}}).size()==2,"multi-mesh fit preserves both selected meshes");
            Exec("TRANSACTION UNDO");require(brushState()==before && brushText()==beforeText,"multi-mesh fit undoes in one step");Exec("TRANSACTION UNDO");
            // Remove this test actor and restore the original builder/selection before the remaining suite.
            Exec("TRANSACTION UNDO");call({{"op","select"},{"actors",originalSelection}});
        }
        J view=call({{"op","view.capture"}});require(!view.at("cameras").empty(),"capture native cameras");
        auto adjusted=view;
        for(auto& c:adjusted["cameras"]) {c["position"][0]=c["position"][0].get<double>()+32.0;}
        call({{"op","view.restore"},{"view",adjusted}});
        auto again=call({{"op","view.capture"}});require(again["cameras"]==adjusted["cameras"],"camera adjustment restores exactly");
        call({{"op","view.restore"},{"view",view}});
        auto map=call({{"op","map"}});Record("workflow_map",map.dump().c_str());
        std::string material="TXT_INI.BSP.BETON_Lit_Spec";
        auto usages=call({{"op","usages"},{"asset",material}});
        require(!usages.empty(),"find fixture BSP material usages");
        J replace=J::array();for(auto& usage:usages)if(usage.at("surface").get<int>()>=0 && usage.at("replaceable").get<bool>()){replace.push_back(usage);break;}
        require(replace.size()==1,"single BSP usage chosen for replacement");
        call({{"op","replace"},{"source",material},{"replacement","Engine.DefaultTexture"},{"usages",replace}});
        require(call({{"op","usages"},{"asset",material}}).size()+1==usages.size(),"replacement changes only checked surface");
        Exec("TRANSACTION UNDO");require(call({{"op","usages"},{"asset",material}}).size()==usages.size(),"replacement undo restores source material");
        Exec("TRANSACTION REDO");require(call({{"op","usages"},{"asset",material}}).size()+1==usages.size(),"replacement redo works");
        require(Rebuild(),"rebuild after material replacement");
        require(call({{"op","usages"},{"asset",material}}).size()+1==usages.size(),"replacement survives rebuild through source polygons");
        J linked={{"id","links"},{"name","Link fixture"},{"pivot",{0,0,0}},{"bindings",J::array()},{"dependencies",J::array({"Engine.Light"})},{"actors",J::array()}};
        for(int i=0;i<3;++i)
        {
            std::string name="Relay"+std::to_string(i),tag=i==0?"Start":"End",event=i==0?"End":"Start";
            linked["actors"].push_back({{"name",name},{"path","Assembly."+name},{"class","Engine.Light"},{"text","Begin Actor Class=Light Name="+name+"\nTag="+tag+"\nEvent="+event+"\nEnd Actor\n"},{"tag",tag},{"event",event},{"position",{i*64,0,0}},{"rotation",{0,0,0}}});
        }
        auto linkInstance=call({{"op","assembly.place"},{"definition",linked},{"position",{0,0,0}},{"rotation",{0,0,0}},{"bindings",J::object()}});
        auto edges=call({{"op","connections"},{"actor",linkInstance["members"][0]["path"]}});
        require(edges.size()==4,"connection inspector resolves cyclic and one-to-many Event/Tag links");
        auto allLinked=call({{"op","actors"}});std::string tag0;
        for(const auto& a:allLinked)if(a["path"]==linkInstance["members"][0]["path"])tag0=a["tag"];
        auto secondLinks=call({{"op","assembly.place"},{"definition",linked},{"position",{0,256,0}},{"rotation",{0,0,0}},{"bindings",J::object()}});
        auto secondEdges=call({{"op","connections"},{"actor",secondLinks["members"][0]["path"]}});
        require(secondEdges.size()==4 && call({{"op","connections"},{"actor",linkInstance["members"][0]["path"]}}).size()==4,"repeated assembly insertion isolates internal tag groups");
        std::string endTag;for(const auto& a:allLinked)if(a["path"]==linkInstance["members"][1]["path"])endTag=a["tag"];
        J magic={{"id","magic-fixture"},{"name","Magic event fixture"},{"pivot",{0,0,0}},{"bindings",J::array()},{"dependencies",J::array({"SBase.SMagicEvent"})},{"actors",J::array()}};
        std::string magicText="Begin Actor Class=SMagicEvent Name=MagicFixture\nGroups(0)=(EventGroup=((Event="+tag0+"),(Event="+endTag+"),(Event=None)))\nGroups(1)=(EventGroup=((Event=WorkflowMissingTarget)))\nEnd Actor\n";
        magic["actors"].push_back({{"name","MagicFixture"},{"path","Assembly.MagicFixture"},{"class","SBase.SMagicEvent"},{"text",magicText},{"tag","MagicFixture"},{"event","None"},{"position",{0,0,0}},{"rotation",{0,0,0}}});
        auto magicInstance=call({{"op","assembly.place"},{"definition",magic},{"position",{0,512,0}},{"rotation",{0,0,0}},{"bindings",J::object()}});
        auto magicSnapshot=call({{"op","magic.inspect"},{"actor",magicInstance["members"][0]}});
        std::ofstream(directory/"magic_workbench_schema.json")<<magicSnapshot.dump(2);
        {
            auto document=call({{"op","magic.json.export"},{"actor",magicSnapshot["actor"]}});
            std::ofstream(directory/"magic_event_export.json")<<document.dump(2);
            require(document["properties"].contains("Groups") && !document["properties"].contains("Tag") && !document["properties"].contains("Location") && !document["context"]["actors"].empty(),"event JSON includes settings and actor context while preserving destination identity");
            auto edited=document;edited["properties"]["Groups"][0]["EventGroup"][0]["Delay"]="2.25";edited["properties"]["Groups"][0]["Repeat"]="3";
            call({{"op","magic.json.import"},{"snapshot",magicSnapshot},{"document",edited}});
            auto imported=call({{"op","magic.inspect"},{"actor",magicSnapshot["actor"]}});
            require(std::stod(imported["values"]["Groups"][0]["EventGroup"][0]["Delay"].get<std::string>())==2.25 && imported["values"]["Groups"][0]["Repeat"]=="3","JSON import applies group timing and repeat settings together");
            require(imported["values"]["Tag"]==magicSnapshot["values"]["Tag"] && imported["values"]["Location"]==magicSnapshot["values"]["Location"],"JSON import preserves Tag and transform");
            bool rejected=false;try{call({{"op","magic.json.import"},{"snapshot",magicSnapshot},{"document",document}});}catch(const std::exception&){rejected=true;}
            require(rejected,"JSON import rejects a stale event snapshot");
            Exec("TRANSACTION UNDO");require(call({{"op","magic.inspect"},{"actor",magicSnapshot["actor"]}})["values"]==magicSnapshot["values"],"JSON import is one undo step");
            Exec("TRANSACTION REDO");require(call({{"op","magic.inspect"},{"actor",magicSnapshot["actor"]}})["values"]==imported["values"],"JSON import redo restores all changes");Exec("TRANSACTION UNDO");
            auto invalid=edited;invalid["properties"]["Groups"][0]["EventGroup"][1]["Delay"]="-2";rejected=false;
            try{call({{"op","magic.json.import"},{"snapshot",magicSnapshot},{"document",invalid}});}catch(const std::exception&){rejected=true;}
            require(rejected && call({{"op","magic.inspect"},{"actor",magicSnapshot["actor"]}})["values"]==magicSnapshot["values"],"invalid later action leaves the whole event unchanged");
            invalid=edited;invalid["properties"]["StopActor"]="Actor'MyLevel.MissingJsonTarget'";rejected=false;
            try{call({{"op","magic.json.import"},{"snapshot",magicSnapshot},{"document",invalid}});}catch(const std::exception&){rejected=true;}
            require(rejected && call({{"op","magic.inspect"},{"actor",magicSnapshot["actor"]}})["values"]==magicSnapshot["values"],"missing JSON object reference is rejected before edits");
        }

        {
            auto selectionBefore=call({{"op","actors"},{"selected",true}});
            auto exported=call({{"op","authoring.export"}});
            require(exported["actors"].size()>0 && exported["classes"].size()>0 && exported.contains("geometryT3d"),"map authoring exports actor properties, schemas, assets and world geometry");
            require(call({{"op","actors"},{"selected",true}})==selectionBefore,"map export preserves selection");
            std::ofstream(directory/"map-authoring.json")<<exported.dump(2);
            auto doc=exported["changes"];
            auto location=J{{"X","128"},{"Y","96"},{"Z","64"}};
            auto create=[&](const char* id,const char* type){return J{{"op","create"},{"id",id},{"class",type},{"properties",{{"Location",location}}}};};
            doc["operations"]=J::array({create("AuthoringEvent","SBase.SMagicEvent"),create("AuthoringLight","Engine.Light"),create("AuthoringSound","SBase.SAmbientSoundTrigger"),create("AuthoringSteam","SBase.SSpawnableEmitter"),create("AuthoringTrigger","Engine.Trigger"),
                {{"op","component"},{"id","SteamParticles"},{"owner","AuthoringSteam"},{"class","Engine.SpriteEmitter"},{"properties",J::object()}},
                {{"op","link"},{"event","AuthoringEvent"},{"target","AuthoringTrigger"},{"trigger",true}},
                {{"op","link"},{"event","AuthoringEvent"},{"target","AuthoringSound"},{"trigger",false}},
                {{"op","link"},{"event","AuthoringEvent"},{"target","AuthoringSteam"},{"trigger",false}}});
            doc["operations"][0]["properties"]["StopActor"]={{"$ref","AuthoringLight"}};
            doc["operations"].back()["delay"]="1.5";
            std::ofstream(directory/"map-changes.json")<<doc.dump(2);
            const auto beforeActors=call({{"op","actors"}});
            auto preview=call({{"op","authoring.preview"},{"document",doc}});
            require(!preview["changes"].empty() && call({{"op","actors"}})==beforeActors,"preview lists map changes without mutating actors");
            auto invalid=doc;invalid["operations"][0]["properties"]["StopActor"]={{"$ref","Missing"}};
            bool rejected=false;try{call({{"op","authoring.apply"},{"document",invalid}});}catch(...){rejected=true;}
            require(rejected && call({{"op","actors"}})==beforeActors,"missing batch references fail without partial creation");
            invalid=doc;invalid["operations"].push_back({{"op","link"},{"event","AuthoringEvent"},{"target","AuthoringSound"},{"trigger",false},{"group",999}});
            rejected=false;try{call({{"op","authoring.apply"},{"document",invalid}});}catch(...){rejected=true;}
            require(rejected && call({{"op","actors"}})==beforeActors,"late batch failure rolls back all created actors and links");
            auto applied=call({{"op","authoring.apply"},{"document",doc}});
            require(applied["created"].size()==6,"batch creates lights, sound, trigger, event, emitter and particle component");
            J states=J::array();for(const auto& identity:applied["created"])states.push_back(call({{"op","magic.inspect"},{"actor",identity}}));
            require(states[0]["values"]["StopActor"].get<std::string>().find("AuthoringLight")!=std::string::npos,"forward actor references resolve after batch creation");
            require(states[4]["values"]["Event"]==states[0]["values"]["Tag"] && states[0]["values"]["Groups"][0]["EventGroup"].size()==2,"batch connects trigger and timed-event targets");
            require(std::stod(states[0]["values"]["Groups"][0]["EventGroup"][1]["Delay"].get<std::string>())==1.5,"batch preserves an authored action delay");
            require(std::stod(states[1]["values"]["Location"]["X"].get<std::string>())==128 && std::stod(states[1]["values"]["Location"]["Y"].get<std::string>())==96 && std::stod(states[1]["values"]["Location"]["Z"].get<std::string>())==64,"batch places actors at exact absolute coordinates");
            require(states[3]["values"]["Emitters"].size()==1,"batch attaches an owned particle component");
            require(call({{"op","actors"},{"selected",true}})==selectionBefore,"batch preserves selection");
            Exec("TRANSACTION UNDO");require(call({{"op","actors"}})==beforeActors,"one undo removes the entire set piece");
            Exec("TRANSACTION REDO");
            for(const auto& state:states)require(call({{"op","magic.inspect"},{"actor",state["actor"]}})["values"]==state["values"],"one redo restores set piece properties and connections");
            auto existingLink=exported["changes"];existingLink["operations"]=J::array({{{"op","link"},{"event",states[0]["actor"]["path"]},{"target",states[2]["actor"]["path"]},{"trigger",false}}});
            rejected=false;try{call({{"op","authoring.preview"},{"document",existingLink}});}catch(...){rejected=true;}
            require(rejected,"linking existing actors requires exported before values");
            existingLink["expect"]={{states[0]["actor"]["path"].get<std::string>(),states[0]["values"]},{states[2]["actor"]["path"].get<std::string>(),states[2]["values"]}};
            call({{"op","authoring.apply"},{"document",existingLink}});
            rejected=false;try{call({{"op","authoring.apply"},{"document",existingLink}});}catch(...){rejected=true;}
            require(rejected,"changed event wiring rejects stale link expectations");
            Exec("TRANSACTION UNDO");
            auto update=exported["changes"];update["operations"]=J::array({{{"op","update"},{"actor",states[1]["actor"]},{"before",states[1]["values"]},{"properties",{{"LightBrightness","123"}}}}});
            call({{"op","authoring.apply"},{"document",update}});
            rejected=false;try{call({{"op","authoring.apply"},{"document",update}});}catch(...){rejected=true;}
            require(rejected,"map authoring rejects stale existing-actor edits");
            Exec("TRANSACTION UNDO");require(call({{"op","magic.inspect"},{"actor",states[1]["actor"]}})["values"]==states[1]["values"],"batch update undo restores old light settings");
            Exec("TRANSACTION UNDO");
            require(call({{"op","actors"}})==beforeActors,"authoring fixture leaves pre-existing actors unchanged");
            auto uiFile=directory/"map-authoring-ui.json";WorkflowProbe::jsonDialogPath=uiFile.wstring();WorkflowProbe::jsonDialogCancel=false;WorkflowProbe::jsonDialogTicks=0;
            auto timer=SetTimer(nullptr,0,100,WorkflowProbe::AnswerJsonDialog);SendMessage(frameWindow,WM_COMMAND,40934,0);KillTimer(nullptr,timer);
            require(std::filesystem::exists(uiFile),"map authoring export menu saves through the native file dialog");
            WorkflowProbe::jsonDialogPath=(directory/"map-changes.json").wstring();WorkflowProbe::jsonDialogTicks=0;WorkflowProbe::authoringPreviewCancel=true;WorkflowProbe::authoringPreviewSeen=false;
            WorkflowProbe::previewScreenshot=directory/"map_authoring_preview.bmp";
            timer=SetTimer(nullptr,0,100,WorkflowProbe::AnswerJsonDialog);SendMessage(frameWindow,WM_COMMAND,40935,0);KillTimer(nullptr,timer);
            require(WorkflowProbe::authoringPreviewSeen && call({{"op","actors"}})==beforeActors,"cancelled map preview leaves actors unchanged");
            WorkflowProbe::previewScreenshot.clear();WorkflowProbe::jsonDialogTicks=0;WorkflowProbe::authoringPreviewCancel=false;WorkflowProbe::authoringPreviewSeen=false;
            timer=SetTimer(nullptr,0,100,WorkflowProbe::AnswerJsonDialog);SendMessage(frameWindow,WM_COMMAND,40935,0);KillTimer(nullptr,timer);
            require(WorkflowProbe::authoringPreviewSeen && call({{"op","actors"}}).size()==beforeActors.size()+5,"map import menu applies the reviewed set piece");
            Exec("TRANSACTION UNDO");require(call({{"op","actors"}})==beforeActors,"UI batch import has one-step Undo");
        }
        std::ofstream(directory/"magic_workbench_classes.json")<<call({{"op","magic.classes"}}).dump(2);
        for(const auto* type:{"Engine.Trigger","Engine.Mover","SBase.SAmbientSoundTrigger","SBase.SSpawnableEmitter","SBase.SDamageVolume"})
        {
            auto created=call({{"op","magic.create"},{"class",type},{"geometry",std::string(type)=="SBase.SDamageVolume"?"box":"point"}});
            auto snapshot=call({{"op","magic.inspect"},{"actor",created}});
            std::ofstream(directory/(std::string(type)+".json"))<<snapshot.dump(2);
            if(std::string(type)=="Engine.Mover")
            {
                require(snapshot["values"].contains("KeyPos") && snapshot["values"].contains("Location"),"workbench exposes mover key arrays and actor transforms");
                bool invalidKey=false;try{call({{"op","magic.edit"},{"snapshot",snapshot},{"changes",{{"KeyNum","255"}}}});}catch(...){invalidKey=true;}require(invalidKey,"out-of-range mover key is rejected before native editing");
                call({{"op","magic.key"},{"snapshot",snapshot},{"key",1}});Exec("TRANSACTION UNDO");
                require(call({{"op","magic.inspect"},{"actor",created}})["values"]==snapshot["values"],"mover key capture undo restores full pose");
            }
            if(std::string(type)=="SBase.SSpawnableEmitter")
            {
                auto component=call({{"op","magic.component"},{"snapshot",snapshot},{"class","Engine.SpriteEmitter"}});
                auto componentSnapshot=call({{"op","magic.inspect"},{"actor",component}});
                require(!componentSnapshot["values"].empty(),"particle component exposes editable reflected options");
                std::ofstream(directory/"magic_particle_component.json")<<componentSnapshot.dump(2);
                Exec("TRANSACTION UNDO");require(call({{"op","magic.inspect"},{"actor",created}})["values"]==snapshot["values"],"particle component insertion uses native undo");
            }
            Exec("TRANSACTION UNDO");
        }
        auto builderVolume=call({{"op","magic.create"},{"class","SBase.SDamageVolume"},{"geometry","builder"}});
        require(!builderVolume.empty(),"workbench creates a damage volume from builder polygons");Exec("TRANSACTION UNDO");
        auto editedGroups=magicSnapshot["values"]["Groups"];editedGroups[0]["EventGroup"][0]["Delay"]="1.25";
        call({{"op","magic.edit"},{"snapshot",magicSnapshot},{"changes",{{"Groups",editedGroups}}}});
        require(call({{"op","magic.inspect"},{"actor",magicInstance["members"][0]}})["values"]["Groups"][0]["EventGroup"][0]["Delay"]=="1.250000","workbench edits nested native delay");
        Exec("TRANSACTION UNDO");
        require(call({{"op","magic.inspect"},{"actor",magicInstance["members"][0]}})["values"]==magicSnapshot["values"],"workbench nested edit uses one native undo");
        auto runtime=call({{"op","magic.create"},{"class","SBase.SMagicEvent"}});
        auto runtimeSnapshot=call({{"op","magic.inspect"},{"actor",runtime}});
        auto runtimeGroups=magicSnapshot["values"]["Groups"];runtimeGroups.erase(runtimeGroups.begin()+1);runtimeGroups[0]["EventGroup"].erase(runtimeGroups[0]["EventGroup"].begin()+2);
        runtimeGroups[0]["EventGroup"][0]["Event"]="None";runtimeGroups[0]["EventGroup"][1]["Event"]="None";
        runtimeGroups[0]["EventGroup"][0]["Delay"]="5";runtimeGroups[0]["EventGroup"][1]["Delay"]="2";runtimeGroups[0]["Repeat"]="2";
        call({{"op","magic.edit"},{"snapshot",runtimeSnapshot},{"changes",{{"Groups",runtimeGroups}}}});
        auto nativeState=WorkflowProbe::ActivateMagic(runtime,linkInstance["members"][0],true);
        std::ofstream(directory/"magic_activation.json")<<nativeState.dump(2);
        require(nativeState["queue"].size()==2 && nativeState["groups"][0]["repeat"]==1,"native activation schedules parallel actions and consumes one repeat");
        double dueDifference=std::abs(nativeState["queue"][0]["due"].get<double>()-nativeState["queue"][1]["due"].get<double>());
        require(std::abs(dueDifference-3.0)<0.01,"native delays share activation time rather than accumulating");
        Exec("TRANSACTION UNDO");
        runtimeSnapshot=call({{"op","magic.inspect"},{"actor",runtime}});runtimeGroups[0]["Sequence"]="True";runtimeGroups[0]["Repeat"]="1";
        call({{"op","magic.edit"},{"snapshot",runtimeSnapshot},{"changes",{{"Groups",runtimeGroups}}}});
        auto ignoredActivation=WorkflowProbe::ActivateMagic(runtime,linkInstance["members"][0],false);
        require(ignoredActivation["queue"].empty() && ignoredActivation["groups"][0]["index"]==0,"ValidOn rejects untrigger without advancing sequence");
        auto firstActivation=WorkflowProbe::ActivateMagic(runtime,linkInstance["members"][0],true);
        require(firstActivation["queue"].size()==1 && firstActivation["groups"][0]["index"]==1 && firstActivation["groups"][0]["repeat"]==1,"sequence schedules one action per activation");
        auto secondActivation=WorkflowProbe::ActivateMagic(runtime,linkInstance["members"][0],true);
        require(secondActivation["queue"].size()==2 && secondActivation["groups"][0]["index"]==0 && secondActivation["groups"][0]["repeat"]==-1,"sequence consumes repeat after completing its action cycle");
        auto exhausted=WorkflowProbe::ActivateMagic(runtime,linkInstance["members"][0],true);require(exhausted==secondActivation,"exhausted event group schedules no further actions");
        Exec("TRANSACTION UNDO");Exec("TRANSACTION UNDO");
        call({{"op","select"},{"actors",J::array({magicInstance["members"][0]})}});
        SendMessage(frameWindow,WM_COMMAND,40927,0);auto workbench=WorkflowProbe::FindDialog("SMagicEvent Workbench");require(workbench!=nullptr,"SMagicEvent workbench opens for an existing actor");
        {
            require(IsWindowEnabled(GetDlgItem(workbench,144)) && IsWindowEnabled(GetDlgItem(workbench,145)),"JSON buttons enabled for open event");
            auto path=directory/L"event-ui-roundtrip.json";WorkflowProbe::jsonDialogPath=path.wstring();WorkflowProbe::jsonDialogCancel=false;WorkflowProbe::jsonDialogTicks=0;
            auto timer=SetTimer(nullptr,0,100,WorkflowProbe::AnswerJsonDialog);SendMessage(workbench,WM_COMMAND,144,0);KillTimer(nullptr,timer);
            require(std::filesystem::exists(path),"Export JSON button saves a file through the native dialog");
            J document;{std::ifstream input(path);input>>document;}require(document["format"]=="scct.smagic-event","UI exports versioned event JSON");
            document["properties"]["Groups"][0]["EventGroup"][0]["Delay"]="3.5";{std::ofstream output(path);output<<document.dump(2);}
            WorkflowProbe::jsonDialogTicks=0;timer=SetTimer(nullptr,0,100,WorkflowProbe::AnswerJsonDialog);SendMessage(workbench,WM_COMMAND,145,0);KillTimer(nullptr,timer);
            require(std::stod(call({{"op","magic.inspect"},{"actor",magicSnapshot["actor"]}})["values"]["Groups"][0]["EventGroup"][0]["Delay"].get<std::string>())==3.5,"Import JSON button applies edited file and refreshes event");
            WorkflowProbe::Click(workbench,104);
            require(call({{"op","magic.inspect"},{"actor",magicSnapshot["actor"]}})["values"]==magicSnapshot["values"],"Workbench Undo restores JSON import");
            WorkflowProbe::jsonDialogCancel=true;WorkflowProbe::jsonDialogTicks=0;timer=SetTimer(nullptr,0,100,WorkflowProbe::AnswerJsonDialog);SendMessage(workbench,WM_COMMAND,145,0);KillTimer(nullptr,timer);
            require(call({{"op","magic.inspect"},{"actor",magicSnapshot["actor"]}})["values"]==magicSnapshot["values"],"cancelled JSON import leaves event untouched");WorkflowProbe::jsonDialogCancel=false;
        }
        WorkflowProbe::Click(workbench,116);require(call({{"op","magic.inspect"},{"actor",magicInstance["members"][0]}})["values"]["Groups"].size()==3,"workbench Add Group changes native groups");WorkflowProbe::Click(workbench,104);
        SendMessage(GetDlgItem(workbench,115),CB_SETCURSEL,0,0);SendMessage(workbench,WM_COMMAND,MAKEWPARAM(115,CBN_SELCHANGE),0);
        SendMessage(GetDlgItem(workbench,121),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(175,72));SendMessage(GetDlgItem(workbench,121),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(305,72));SendMessage(GetDlgItem(workbench,121),WM_LBUTTONUP,0,MAKELPARAM(305,72));
        require(std::stod(call({{"op","magic.inspect"},{"actor",magicInstance["members"][0]}})["values"]["Groups"][0]["EventGroup"][0]["Delay"].get<std::string>())==2.0,"timeline drag commits snapped native delay");WorkflowProbe::Click(workbench,104);
        auto tree=GetDlgItem(workbench,132);
        auto findDelay=[&](auto&& self,HTREEITEM node)->HTREEITEM {for(auto item=node;item;item=TreeView_GetNextSibling(tree,item)){char label[256]{};TVITEMA value{};value.mask=TVIF_TEXT;value.hItem=item;value.pszText=label;value.cchTextMax=256;SendMessageA(tree,TVM_GETITEMA,0,reinterpret_cast<LPARAM>(&value));if(!strncmp(label,"Delay ",6))return item;if(auto child=self(self,TreeView_GetChild(tree,item)))return child;}return nullptr;};
        auto delayRow=findDelay(findDelay,TreeView_GetRoot(tree));require(delayRow!=nullptr,"selected action exposes a precise delay inspector");TreeView_SelectItem(tree,delayRow);SetWindowTextA(GetDlgItem(workbench,133),"1.75");SendMessage(GetDlgItem(workbench,133),WM_KEYDOWN,VK_RETURN,0);
        require(std::stod(call({{"op","magic.inspect"},{"actor",magicInstance["members"][0]}})["values"]["Groups"][0]["EventGroup"][0]["Delay"].get<std::string>())==1.75,"Enter commits a precise inspector field");WorkflowProbe::Click(workbench,104);
        WorkflowProbe::Screenshot(workbench,directory/"magic_workbench.bmp");
        RECT originalWindow{};GetWindowRect(workbench,&originalWindow);RECT scaled{originalWindow.left,originalWindow.top,originalWindow.left+2070,originalWindow.top+1230};SendMessage(workbench,WM_DPICHANGED,MAKEWPARAM(144,144),reinterpret_cast<LPARAM>(&scaled));WorkflowProbe::Screenshot(workbench,directory/"magic_workbench_144dpi.bmp");SendMessage(workbench,WM_DPICHANGED,MAKEWPARAM(96,96),reinterpret_cast<LPARAM>(&originalWindow));
        SetFocus(GetDlgItem(workbench,107));SendMessage(GetDlgItem(workbench,107),WM_KEYDOWN,VK_TAB,0);require(GetFocus()==GetDlgItem(workbench,108),"workbench keyboard Tab moves between controls");SendMessage(workbench,WM_CLOSE,0,0);
        auto magicEdges=call({{"op","connections"},{"actor",magicInstance["members"][0]["path"]}});
        std::ofstream(directory/"workflow_magic_edges.json")<<magicEdges.dump(2);
        require(magicEdges.size()==4,"nested SMagicEvent groups resolve all matching tags and omit None");
        int missing=0,shared=0;for(const auto& e:magicEdges){if(!e.at("resolved").get<bool>())++missing;if(e.at("property")=="Groups[0].EventGroup[1].Event")++shared;}
        require(missing==1 && shared==2,"SMagicEvent preserves one-to-many targets and reports unmatched nested event");
        auto incomingMagic=call({{"op","connections"},{"actor",linkInstance["members"][0]["path"]}});require(incomingMagic.size()==5,"SMagicEvent group link appears as incoming on target actor");
        auto sharedPreview=call({{"op","tag.preview"},{"actor",linkInstance["members"][1]},{"tag","WorkflowSharedRenamed"}});
        size_t renamedTargets=0;for(const auto& change:sharedPreview["changes"])if(change["property"]=="Tag")++renamedTargets;
        require(renamedTargets==2 && sharedPreview["changes"].size()==4,"shared tag preview includes both target actors and both dependent event fields");
        call({{"op","tag.rename"},{"preview",sharedPreview}});
        auto currentTag=[&](const J& id){for(const auto& a:call({{"op","actors"}}))if(a["path"]==id["path"])return a["tag"].get<std::string>();throw std::runtime_error("Tag test actor missing");};
        require(currentTag(linkInstance["members"][1])=="WorkflowSharedRenamed" && currentTag(linkInstance["members"][2])=="WorkflowSharedRenamed","shared tag rename updates every matching actor");
        require(call({{"op","connections"},{"actor",magicInstance["members"][0]["path"]}})==magicEdges,"rename preserves nested EventGroup links and unmatched targets");
        require(call({{"op","connections"},{"actor",linkInstance["members"][0]["path"]}})==incomingMagic,"rename preserves ordinary Event one-to-many links");
        Exec("TRANSACTION UNDO");require(currentTag(linkInstance["members"][1])==endTag && currentTag(linkInstance["members"][2])==endTag,"single undo restores shared actor Tags");
        require(call({{"op","connections"},{"actor",magicInstance["members"][0]["path"]}})==magicEdges,"undo restores nested EventGroup name assignments");
        Exec("TRANSACTION REDO");require(currentTag(linkInstance["members"][1])=="WorkflowSharedRenamed","single redo reapplies group rename");
        require(call({{"op","connections"},{"actor",magicInstance["members"][0]["path"]}})==magicEdges,"redo preserves nested links");
        bool collision=false;try{call({{"op","tag.preview"},{"actor",linkInstance["members"][1]},{"tag",tag0}});}catch(const std::exception&){collision=true;}require(collision,"rename rejects a Tag already used by another group");
        bool invalid=false;try{call({{"op","tag.preview"},{"actor",linkInstance["members"][1]},{"tag","None"}});}catch(const std::exception&){invalid=true;}require(invalid,"rename rejects None without disconnecting targets");
        bool stale=false;try{call({{"op","tag.rename"},{"preview",sharedPreview}});}catch(const std::exception&){stale=true;}require(stale,"stale rename previews are rejected without mutation");
        auto partialPreview=call({{"op","tag.preview"},{"actor",linkInstance["members"][1]},{"tag","WorkflowPartial"}});
        partialPreview["excluded"]=J::array();
        for(size_t i=0;i<partialPreview["changes"].size();++i)
            if(partialPreview["changes"][i]["actor"]["path"]!=linkInstance["members"][1]["path"] || partialPreview["changes"][i]["property"]!="Tag")partialPreview["excluded"].push_back(i);
        call({{"op","tag.rename"},{"preview",partialPreview}});
        require(currentTag(linkInstance["members"][1])=="WorkflowPartial" && currentTag(linkInstance["members"][2])=="WorkflowSharedRenamed","partial rename leaves excluded actor unchanged");
        auto remaining=call({{"op","connections"},{"actor",magicInstance["members"][0]["path"]}});
        require(remaining.size()==magicEdges.size()-1,"excluded nested event still targets the unrenamed group member");
        Exec("TRANSACTION UNDO");require(currentTag(linkInstance["members"][1])=="WorkflowSharedRenamed" && call({{"op","connections"},{"actor",magicInstance["members"][0]["path"]}})==magicEdges,"undo restores partial rename and connections");
        std::ofstream(directory/"workflow_actors.json")<<actors.dump(2);
        J members=J::array();for(const auto& a:actors) if(a.at("class")=="Engine.Brush" && a.at("path")!=actors.at(1).at("path")) members.push_back(a);
        require(!members.empty(),"authored brush selected for assembly");
        // Deliberately select only the non-builder brush using native identities.
        members=J::array({members.back()});call({{"op","select"},{"actors",members}});
        J definition=call({{"op","assembly.capture"},{"members",members},{"position",{0,0,0}},{"rotation",{0,0,0}}});
        definition["id"]="native-test";definition["name"]="Native fixture brush";
        std::ofstream(directory/"workflow_definition.json")<<definition.dump(2);
        require(definition.at("actors").size()==1,"brush export captured");
        J bindings=J::object();for(auto& b:definition.at("bindings")) bindings[b.at("id").get<std::string>()]="";
        auto count=call({{"op","actors"}}).size();
        auto instance=call({{"op","assembly.place"},{"definition",definition},{"position",{1200,0,0}},{"rotation",{0,16384,0}},{"bindings",bindings}});
        require(call({{"op","actors"}}).size()==count+1,"assembly creates exactly one actor");
        Exec("TRANSACTION UNDO");require(call({{"op","actors"}}).size()==count,"assembly insertion is undoable");
        Exec("TRANSACTION REDO");require(call({{"op","actors"}}).size()==count+1,"assembly insertion supports redo");
        auto recaptured=call({{"op","assembly.capture"},{"members",instance.at("members")},{"position",{1200,0,0}},{"rotation",{0,16384,0}}});
        require(recaptured.at("actors").size()==1,"placed assembly can be edited again");
        {
            // Emitter Library: a workbench emitter, whose particle emitter is owned
            // by the actor, is saved and placed with a live sub-emitter in one Undo
            // step, and saving the placed copy again does not grow its names.
            auto emitterActors=call({{"op","actors"}}).size();
            auto emitter=call({{"op","magic.create"},{"class","Engine.Emitter"}});
            auto particles=call({{"op","magic.component"},{"snapshot",call({{"op","magic.inspect"},{"actor",emitter}})},{"class","Engine.SpriteEmitter"}});
            call({{"op","magic.edit"},{"snapshot",call({{"op","magic.inspect"},{"actor",particles}})},{"changes",{{"DrawStyle","PTDS_Brighten"}}}});
            auto offersSave=[&]()
            {
                using Load=HMENU(WINAPI*)(HINSTANCE,LPCSTR);
                auto menu=(*reinterpret_cast<Load*>(0x11af23f0))(GetModuleHandle(nullptr),MAKEINTRESOURCEA(107));
                auto state=GetMenuState(GetSubMenu(menu,0),40968,MF_BYCOMMAND);DestroyMenu(menu);return state!=UINT(-1);
            };
            call({{"op","select"},{"actors",J::array()}});
            require(!offersSave() && call({{"op","emitterlib.selected"}}).empty(),"actor menu hides Save to Emitter Library without a selected emitter");
            call({{"op","select"},{"actors",J::array({emitter})}});
            require(offersSave() && call({{"op","emitterlib.selected"}}).size()==1,"actor menu offers Save to Emitter Library for a selected emitter");
            auto draft=call({{"op","emitterlib.capture"},{"members",J::array({emitter})}});
            bool mapLocal=false;for(const auto& dependency:draft.at("dependencies"))mapLocal|=dependency.get<std::string>().rfind("MyLevel.",0)==0;
            require(!mapLocal && draft.at("bindings").empty() && draft.at("actors").size()==1,"emitter capture records no map-local dependency or binding");
            auto saved=call({{"op","emitterlib.save"},{"entry",draft},{"name","Native test emitter"},{"category","Test FX"}});
            require(std::filesystem::exists(directory/"ReloadedEditor"/"emitter_library.json") && saved.at("category")=="Test FX","emitter entry is written to the user library");
            auto placed=call({{"op","emitterlib.place"},{"id",saved.at("id")},{"position",{0,1536,0}},{"rotation",{0,0,0}}});
            auto values=call({{"op","magic.inspect"},{"actor",placed.at(0)}}).at("values");
            const auto sub=values.at("Emitters").size()==1?values.at("Emitters").at(0).get<std::string>():std::string();
            require(sub.find("'MyLevel.RE_")!=std::string::npos && sub.find(emitter.at("path").get<std::string>().substr(8)+".")==std::string::npos,"placed emitter references its own sub-emitter in the map package");
            auto resaved=call({{"op","emitterlib.capture"},{"members",placed}});
            require(resaved.at("actors").at(0).at("name")==draft.at("actors").at(0).at("name") && resaved.at("actors").at(0).at("text").get<std::string>().find("RE_")==std::string::npos,"saving a placed emitter again does not grow its names");
            Exec("TRANSACTION UNDO");require(call({{"op","actors"}}).size()==emitterActors+1,"emitter placement undoes in one step");
            call({{"op","emitterlib.delete"},{"id",saved.at("id")}});
            bool gone=true;for(const auto& entry:call({{"op","emitterlib.list"}}))gone=gone && entry.at("id")!=saved.at("id");require(gone,"deleting a user emitter removes it from the library");
            for(int i=0;i<3;++i)Exec("TRANSACTION UNDO");
            require(call({{"op","actors"}}).size()==emitterActors,"workbench emitter fixture is undone");
        }
        {
            // Built-in Emitter Library (tools/emitter_library): every bundled entry
            // places with each Emitters(i) a live sub-emitter in the map package,
            // loads the textures and sounds it names, and undoes in one step. The
            // placed actors' own export names each dependency and capture resolves
            // every exported reference (FindUsages does not follow particle
            // emitters: DependsOn reads only materials and meshes).
            auto builtinActors=call({{"op","actors"}}).size();size_t builtins=0;
            for(const auto& entry:call({{"op","emitterlib.list"}}))
            {
                if(!entry.at("builtin").get<bool>())continue;
                const auto id=entry.at("id").get<std::string>();
                auto placed=call({{"op","emitterlib.place"},{"id",id},{"position",{0,-1536,256}}});
                require(placed.size()==entry.at("actors").size(),("built-in "+id+" places every emitter").c_str());
                for(const auto& actor:placed)
                {
                    auto emitters=call({{"op","magic.inspect"},{"actor",actor}}).at("values").at("Emitters");
                    bool live=!emitters.empty();for(const auto& e:emitters)live=live && e.get<std::string>().find("'MyLevel.RE_")!=std::string::npos;
                    require(live,("built-in "+id+" references live sub-emitters in the map").c_str());
                }
                auto fold=[](std::string text){for(auto& c:text)c=static_cast<char>(::tolower(static_cast<unsigned char>(c)));return text;};
                std::set<std::string> reached;
                for(const auto& dependency:call({{"op","assembly.capture"},{"members",placed},{"position",{0,-1536,256}},{"rotation",{0,0,0}}}).at("dependencies"))reached.insert(fold(dependency.get<std::string>()));
                for(const auto& dependency:entry.at("dependencies"))
                    require(reached.count(fold(dependency.get<std::string>()))!=0,("built-in "+id+" loads and uses "+dependency.get<std::string>()).c_str());
                Exec("TRANSACTION UNDO");++builtins;
            }
            require(builtins>=30 && call({{"op","actors"}}).size()==builtinActors,"every built-in emitter places and undoes");
        }
        require(Rebuild(),"ordinary geometry rebuild after insertion");
        checkSurfaceBrushSelection(2);
        using Save=int(__thiscall*)(void*,const char*);
        require(reinterpret_cast<Save>(0x10E0416B)(*reinterpret_cast<void**>(kEditor),destination)!=0,"source map save after assembly insertion");
        require(Exec(std::string("MAP LOAD FILE=\"")+destination+"\"")!=0,"ordinary reopen after assembly insertion");
        auto reopened=call({{"op","actors"}});std::ofstream(directory/"workflow_reopened.json")<<reopened.dump(2);
        for(const auto& member:instance.at("members"))
        {
            auto path=member.at("path").get<std::string>();auto name=path.substr(path.find_last_of('.')+1);bool found=false;
            for(const auto& a:reopened) if(a.at("name")==name && a.at("class")==member.at("class"))found=true;
            require(found,"assembly actor survives reopening");
        }
        using SetFilename=void(__thiscall*)(void*,const char*);
        reinterpret_cast<SetFilename>(0x10e05e1c)(*reinterpret_cast<void**>(0x1165e80c),destination);
        SendMessage(frameWindow,WM_COMMAND,40921,0);
        HWND views=WorkflowProbe::FindDialog("Working Views");require(views!=nullptr,"working views dialog opens");
        WorkflowProbe::Click(views,202,"Native view");
        auto library=directory/"ReloadedEditor"/"library.json";
        auto readLibrary=[&](){std::ifstream input(library);J value;input>>value;return value;};
        auto saved=readLibrary();auto key=call({{"op","map"}}).at("key").get<std::string>();
        require(saved["maps"][key]["views"].size()==1,"view saved once");
        auto id=saved["maps"][key]["views"][0]["id"];
        auto edited=call({{"op","view.capture"}});edited["cameras"][0]["position"][0]=777.0;call({{"op","view.restore"},{"view",edited}});
        WorkflowProbe::Click(views,203);
        auto updated=readLibrary();require(updated["maps"][key]["views"].size()==1 && updated["maps"][key]["views"][0]["id"]==id,"view update retains saved identity without duplication");
        require(updated["maps"][key]["views"][0]["cameras"][0]["position"][0]==777.0,"view update stores adjusted camera");
        WorkflowProbe::Screenshot(views,directory/"workflow_views.bmp");
        call({{"op","select"},{"actors",instance.at("members")}});
        SendMessage(frameWindow,WM_COMMAND,40922,0);
        HWND assemblies=WorkflowProbe::FindDialog("Actor Assemblies");require(assemblies!=nullptr,"assembly browser opens");
        WorkflowProbe::Click(assemblies,202,"Native assembly");
        auto firstLibrary=readLibrary();require(firstLibrary["assemblies"].size()==1,"assembly definition saved");auto assemblyId=firstLibrary["assemblies"][0]["id"];
        WorkflowProbe::Click(assemblies,201); // place a tracked instance at the default pose
        WorkflowProbe::Click(assemblies,203); // edit that instance
        Exec("ACTOR ADD CLASS=Engine.Light");auto lights=call({{"op","actors"},{"selected",true}});require(!lights.empty(),"extra light created for membership editing");
        WorkflowProbe::Click(assemblies,207);WorkflowProbe::Click(assemblies,210);
        auto withLight=readLibrary();require(withLight["assemblies"].size()==1 && withLight["assemblies"][0]["id"]==assemblyId && withLight["assemblies"][0]["actors"].size()==2,"add member updates existing assembly in place");
        WorkflowProbe::Click(assemblies,203);call({{"op","select"},{"actors",lights}});WorkflowProbe::Click(assemblies,208);WorkflowProbe::Click(assemblies,210);
        auto withoutLight=readLibrary();require(withoutLight["assemblies"][0]["actors"].size()==1 && withoutLight["assemblies"][0]["id"]==assemblyId,"remove member updates same assembly definition");
        bool lightStillExists=false;for(const auto& actor:call({{"op","actors"}}))if(actor.at("path")==lights[0]["path"])lightStillExists=true;
        require(lightStillExists,"removing membership does not delete scene actor");
        auto selection=withoutLight["maps"][key]["instances"][0]["members"];selection.push_back(lights[0]);
        call({{"op","select"},{"actors",selection}});WorkflowProbe::Click(assemblies,206);
        auto selectionUpdate=readLibrary();
        require(selectionUpdate["assemblies"].size()==1 && selectionUpdate["assemblies"][0]["id"]==assemblyId && selectionUpdate["assemblies"][0]["actors"].size()==2,"selection shortcut updates same assembly entry");
        require(selectionUpdate["assemblies"][0]["pivot"]==firstLibrary["assemblies"][0]["pivot"],"membership updates preserve saved pivot");
        auto beforeFuture=call({{"op","actors"}}).size();WorkflowProbe::Click(assemblies,201);
        require(call({{"op","actors"}}).size()==beforeFuture+2,"future placement uses updated membership without changing existing copies");
        WorkflowProbe::Screenshot(assemblies,directory/"workflow_assemblies.bmp");
        SendMessage(frameWindow,WM_COMMAND,40920,0);auto connections=WorkflowProbe::FindDialog("Gameplay Connections");require(connections!=nullptr,"gameplay connection inspector opens");
        WorkflowProbe::Screenshot(connections,directory/"workflow_connections.bmp");
        WorkflowProbe::Click(connections,203);auto graph=WorkflowProbe::FindDialog("Gameplay Connection Graph");require(graph!=nullptr,"connection graph opens from list");
        SendMessage(GetDlgItem(graph,100),CB_SETCURSEL,1,0);SendMessage(graph,WM_COMMAND,MAKEWPARAM(100,CBN_SELCHANGE),0);
        auto allEdges=call({{"op","connections"},{"actor",""}});require(allEdges.size()>=8,"whole-level snapshot includes both independent connection islands");
        WorkflowProbe::Screenshot(graph,directory/"workflow_graph_level.bmp");
        auto graphTarget=linkInstance["members"][0]["path"].get<std::string>();SetWindowTextA(GetDlgItem(graph,105),graphTarget.c_str());WorkflowProbe::Click(graph,106);WorkflowProbe::Click(graph,111);
        auto graphSelection=call({{"op","actors"},{"selected",true}});require(graphSelection.size()==1 && graphSelection[0]["path"]==graphTarget,"graph search and focus select native counterpart actor");
        call({{"op","select"},{"actors",J::array()}});RECT graphCanvas{};GetClientRect(GetDlgItem(graph,120),&graphCanvas);
        SendMessage(GetDlgItem(graph,120),WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(graphCanvas.right/2,graphCanvas.bottom/2));
        auto doubleClickSelection=call({{"op","actors"},{"selected",true}});require(doubleClickSelection.size()==1 && doubleClickSelection[0]["path"]==graphTarget,"double-clicking graph node selects and focuses counterpart");
        auto renameTarget=linkInstance["members"][1]["path"].get<std::string>();SetWindowTextA(GetDlgItem(graph,105),renameTarget.c_str());WorkflowProbe::Click(graph,106);
        WorkflowProbe::Click(graph,112,"GraphSharedRenamed");require(currentTag(linkInstance["members"][1])=="GraphSharedRenamed" && currentTag(linkInstance["members"][2])=="GraphSharedRenamed","graph rename dialog updates whole shared tag group");
        require(call({{"op","connections"},{"actor",magicInstance["members"][0]["path"]}})==magicEdges,"graph rename dialog preserves dependent links");
        WorkflowProbe::cancelTagPreview=true;WorkflowProbe::Click(graph,112,"CancelledTag");WorkflowProbe::cancelTagPreview=false;
        require(currentTag(linkInstance["members"][1])=="GraphSharedRenamed","cancelling graph rename preview leaves Tags unchanged");
        WorkflowProbe::previewScreenshot=directory/"tag_rename_exclusions.bmp";
        WorkflowProbe::excludeTagEntity=linkInstance["members"][2]["path"].get<std::string>();WorkflowProbe::Click(graph,112,"GraphPartialRenamed");WorkflowProbe::excludeTagEntity.clear();WorkflowProbe::previewScreenshot.clear();
        require(currentTag(linkInstance["members"][1])=="GraphPartialRenamed" && currentTag(linkInstance["members"][2])=="GraphSharedRenamed","popup checkbox excludes an entity from shared tag rename");
        Exec("TRANSACTION UNDO");require(currentTag(linkInstance["members"][1])=="GraphSharedRenamed","popup partial rename is one undo operation");
        WorkflowProbe::Click(graph,109); // fit the searched node's island
        WorkflowProbe::Screenshot(graph,directory/"workflow_graph_island.bmp");
        SendMessage(GetDlgItem(graph,100),CB_SETCURSEL,0,0);SendMessage(graph,WM_COMMAND,MAKEWPARAM(100,CBN_SELCHANGE),0);WorkflowProbe::Click(graph,107);
        WorkflowProbe::Screenshot(graph,directory/"workflow_graph_selected.bmp");
        J busy=linked;busy["id"]="radial-fixture";busy["actors"]=J::array();
        for(int i=0;i<17;++i)
        {
            std::string name=i==0?"EventHub":"EventTarget"+std::to_string(i),tag=i==0?"Hub":"Targets",event=i==0?"Targets":"None";
            busy["actors"].push_back({{"name",name},{"path","Assembly."+name},{"class","Engine.Light"},{"text","Begin Actor Class=Light Name="+name+"\nTag="+tag+"\nEvent="+event+"\nEnd Actor\n"},{"tag",tag},{"event",event},{"position",{i*64,0,0}},{"rotation",{0,0,0}}});
        }
        auto busyInstance=call({{"op","assembly.place"},{"definition",busy},{"position",{0,1024,0}},{"rotation",{0,0,0}},{"bindings",J::object()}});
        auto busyHub=busyInstance["members"][0]["path"].get<std::string>();
        require(call({{"op","connections"},{"actor",busyHub}}).size()==16,"radial graph fixture retains all sixteen event targets");
        SendMessage(GetDlgItem(graph,100),CB_SETCURSEL,1,0);SendMessage(graph,WM_COMMAND,MAKEWPARAM(100,CBN_SELCHANGE),0);
        WorkflowProbe::Click(graph,110);WorkflowProbe::Click(graph,108);WorkflowProbe::Screenshot(graph,directory/"workflow_graph_radial_level.bmp");
        SetWindowTextA(GetDlgItem(graph,105),busyHub.c_str());WorkflowProbe::Click(graph,106);WorkflowProbe::Click(graph,111);WorkflowProbe::Click(graph,109);
        auto busySelection=call({{"op","actors"},{"selected",true}});require(busySelection.size()==1 && busySelection[0]["path"]==busyHub,"radial island hub remains searchable and selectable");
        WorkflowProbe::Screenshot(graph,directory/"workflow_graph_radial_island.bmp");
        DestroyWindow(graph);
        DestroyWindow(connections);DestroyWindow(assemblies);DestroyWindow(views);
        J setpiece=J::array();auto setEvent=call({{"op","magic.create"},{"class","SBase.SMagicEvent"}});setpiece.push_back(setEvent);
        for(const auto* type:{"Engine.Trigger","Engine.Trigger","Engine.Mover","SBase.SAmbientSoundTrigger","SBase.SSmokeEmitter","SBase.SDamageVolume"})
        {
            auto beforeEvent=call({{"op","magic.inspect"},{"actor",setEvent}});auto countBefore=call({{"op","actors"}}).size();
            auto created=call({{"op","magic.create"},{"class",type},{"event",beforeEvent},{"trigger",std::string(type)=="Engine.Trigger"},{"geometry",std::string(type)=="SBase.SDamageVolume"?"box":"point"}});
            auto afterEvent=call({{"op","magic.inspect"},{"actor",setEvent}});
            Exec("TRANSACTION UNDO");require(call({{"op","actors"}}).size()==countBefore && call({{"op","magic.inspect"},{"actor",setEvent}})["values"]==beforeEvent["values"],"compound actor creation and wiring undo together");
            Exec("TRANSACTION REDO");require(call({{"op","magic.inspect"},{"actor",setEvent}})["values"]==afterEvent["values"],"compound creation and wiring redo restores event");setpiece.push_back(created);
        }
        auto setSnapshot=call({{"op","magic.inspect"},{"actor",setEvent}});auto setGroups=setSnapshot["values"]["Groups"];
        auto countBeforeFailure=call({{"op","actors"}}).size();bool failedCompound=false;
        try{call({{"op","magic.create"},{"class","Engine.Trigger"},{"event",setSnapshot},{"group",999}});}catch(...){failedCompound=true;}
        require(failedCompound && call({{"op","actors"}}).size()==countBeforeFailure && call({{"op","magic.inspect"},{"actor",setEvent}})["values"]==setSnapshot["values"],"failed compound creation rolls back actors and event data");
        for(size_t i=0;i<setGroups[0]["EventGroup"].size();++i)setGroups[0]["EventGroup"][i]["Delay"]=std::to_string(i*0.75);
        auto off=setGroups[0]["EventGroup"].back();off["Delay"]="5";off["Type"]="EVT_Untrigger";setGroups[0]["EventGroup"].push_back(off);
        call({{"op","magic.edit"},{"snapshot",setSnapshot},{"changes",{{"Groups",setGroups}}}});
        bool staleRejected=false;try{call({{"op","magic.edit"},{"snapshot",setSnapshot},{"changes",{{"Groups",setGroups}}}});}catch(...){staleRejected=true;}require(staleRejected,"workbench rejects stale edits without overwriting newer data");
        auto moverSnapshot=call({{"op","magic.inspect"},{"actor",setpiece[3]}});call({{"op","magic.edit"},{"snapshot",moverSnapshot},{"changes",{{"InitialState","TriggerOpenTimed"},{"MoveTime","2.5"}}}});
        auto componentOwner=call({{"op","magic.inspect"},{"actor",setpiece[5]}});std::ofstream(directory/"magic_setpiece_emitter.json")<<componentOwner.dump(2);
        J savedMagic=J::array();for(const auto& identity:setpiece){auto snapshot=call({{"op","magic.inspect"},{"actor",identity}});savedMagic.push_back({{"actor",identity},{"values",snapshot["values"]}});}
        std::ofstream(directory/"magic_setpiece.json")<<savedMagic.dump(2);
        call({{"op","select"},{"actors",J::array({setEvent})}});SendMessage(frameWindow,WM_COMMAND,40927,0);auto setWindow=WorkflowProbe::FindDialog("SMagicEvent Workbench");require(setWindow!=nullptr,"set piece opens in the same workbench");WorkflowProbe::Screenshot(setWindow,directory/"magic_setpiece_workbench.bmp");SendMessage(setWindow,WM_CLOSE,0,0);
        {
            auto snapshot=call({{"op","camera.snapshot"}});require(snapshot.empty(),"camera fixture starts empty");
            J paths=J::array(),identities=J::array();
            for(int i=0;i<3;++i)
            {
                auto actor=call({{"op","camera.add"},{"snapshot",snapshot},{"paths",paths},{"loop",true}});
                paths.push_back(actor.at("path"));identities.push_back(actor);snapshot=call({{"op","camera.snapshot"}});
            }
            auto original=snapshot;
            Exec("TRANSACTION UNDO");require(call({{"op","camera.snapshot"}}).size()==2,"adding a camera and updating its neighbours undo together");
            Exec("TRANSACTION REDO");require(call({{"op","camera.snapshot"}})==original,"camera creation redo restores all reciprocal links");
            auto reversed=J::array({paths[2],paths[1],paths[0]});
            call({{"op","camera.order"},{"snapshot",snapshot},{"paths",reversed},{"loop",true}});
            auto first=call({{"op","magic.inspect"},{"actor",identities[2]}});
            require(first["values"]["bFirstCam"]=="True" && first["values"]["NextCam"].get<std::string>().find(paths[1].get<std::string>())!=std::string::npos,"reorder sets first flag and next camera");
            bool rejected=false;try{call({{"op","camera.order"},{"snapshot",original},{"paths",paths}});}catch(...){rejected=true;}require(rejected,"stale network snapshot is rejected");
            Exec("TRANSACTION UNDO");require(call({{"op","camera.snapshot"}})==original,"reordering all cameras uses one undo");
            rejected=false;try{call({{"op","camera.order"},{"snapshot",original},{"paths",J::array({paths[0]})}});}catch(...){rejected=true;}
            require(rejected && call({{"op","camera.snapshot"}})==original,"partial component edits leave camera links unchanged");
            call({{"op","select"},{"actors",J::array({identities[0]})}});SendMessage(frameWindow,WM_COMMAND,40932,0);
            auto manager=WorkflowProbe::FindDialog("SCamNetwork Manager");require(manager!=nullptr,"camera manager menu opens");
            require(SendMessage(GetDlgItem(manager,101),LB_GETCOUNT,0,0)==3,"camera manager shows all three cameras in network order");
            SetWindowTextA(GetDlgItem(manager,107),"Loading bay east");WorkflowProbe::Click(manager,108);
            first=call({{"op","magic.inspect"},{"actor",identities[0]}});
            require(first["values"]["CamName"]=="\"Loading bay east\"","camera manager renames in-game display name");
            auto oldTag=first["values"]["Tag"];
            SetWindowTextA(GetDlgItem(manager,107),"Loading bay \"east\"");WorkflowProbe::Click(manager,108);
            first=call({{"op","magic.inspect"},{"actor",identities[0]}});
            require(first["values"]["CamName"]==J("Loading bay \"east\"").dump() && first["values"]["Tag"]==oldTag,"camera names preserve spaces and quotes without changing Tags");
            WorkflowProbe::Click(manager,105);require(call({{"op","magic.inspect"},{"actor",identities[0]}})["values"]["CamName"]=="\"Loading bay east\"","rename undo restores the previous display name");
            WorkflowProbe::Click(manager,119);first=call({{"op","magic.inspect"},{"actor",identities[0]}});
            auto capturedViews=call({{"op","view.capture"}})["cameras"];
            require(std::stod(first["values"]["Location"]["X"].get<std::string>())==capturedViews[0]["position"][0].get<double>(),"place at viewport captures camera position");
            auto camerasBefore=call({{"op","view.capture"}})["cameras"];
            WorkflowProbe::Click(manager,115);require(SendMessage(GetDlgItem(manager,101),LB_GETCURSEL,0,0)==1,"Next steps to the second camera");
            auto previewViews=call({{"op","view.capture"}})["cameras"];auto second=call({{"op","magic.inspect"},{"actor",identities[1]}});
            require(previewViews[0]["position"][0].get<double>()==std::stod(second["values"]["Location"]["X"].get<std::string>()),"Next previews the selected camera position");
            WorkflowProbe::Click(manager,117);require(call({{"op","view.capture"}})["cameras"]==camerasBefore,"return view restores the original viewport cameras exactly");
            WorkflowProbe::Click(manager,109);first=call({{"op","magic.inspect"},{"actor",identities[1]}});
            require(first["values"]["bFirstCam"]=="True","Move up rewires and sets the first camera");
            WorkflowProbe::Click(manager,120);snapshot=call({{"op","camera.snapshot"}});first=call({{"op","magic.inspect"},{"actor",identities[1]}});
            require(snapshot.size()==3 && first["values"]["NextCam"]=="None" && first["values"]["PrevCam"]=="None","detach keeps the camera actor and removes both links");
            WorkflowProbe::Click(manager,105);require(call({{"op","magic.inspect"},{"actor",identities[1]}})["values"]["bFirstCam"]=="True","detach undo restores the original network");
            WorkflowProbe::Screenshot(manager,directory/"camera_network_manager.bmp");
            RECT cameraWindow{};GetWindowRect(manager,&cameraWindow);RECT cameraScaled{cameraWindow.left,cameraWindow.top,cameraWindow.left+1500,cameraWindow.top+1095};SendMessage(manager,WM_DPICHANGED,MAKEWPARAM(144,144),reinterpret_cast<LPARAM>(&cameraScaled));WorkflowProbe::Screenshot(manager,directory/"camera_network_manager_144dpi.bmp");SendMessage(manager,WM_DPICHANGED,MAKEWPARAM(96,96),reinterpret_cast<LPARAM>(&cameraWindow));
            SendMessage(manager,WM_CLOSE,0,0);std::ofstream(directory/"camera_network_saved.json")<<call({{"op","camera.snapshot"}}).dump(2);
        }
        {
            J document;std::ifstream(directory/"map-changes.json")>>document;document["map"]=call({{"op","authoring.export"}}).at("map");
            auto applied=call({{"op","authoring.apply"},{"document",document}});J states=J::array();
            for(const auto& actor:applied["created"])states.push_back(call({{"op","magic.inspect"},{"actor",actor}}));
            std::ofstream(directory/"map_authoring_saved.json")<<states.dump(2);
        }
        {
            // Emitter Library live preview: a private level and viewport that never reach the map.
            const std::string burst="Begin Actor Class=Emitter Name=Emitter602\n Begin Object Class=SpriteEmitter Name=SpriteEmitter604\n  Acceleration=(Z=20.000000)\n  DrawStyle=PTDS_Translucent\n  MaxParticles=15\n"
                "  StartLocationRange=(Y=(Min=-20.000000,Max=20.000000))\n  RespawnDeadParticles=False\n  Disabled=True\n  AutomaticInitialSpawning=False\n  InitialParticlesPerSecond=1000.000000\n"
                "  StartSizeRange=(X=(Min=30.000000,Max=30.000000))\n  Texture=Texture'sfx.Emitter.smoke_grenade'\n  MeshSpawningStaticMesh=StaticMesh'MyLevel.LocalMesh'\n  LifetimeRange=(Min=1.000000,Max=1.000000)\n  StartVelocityRange=(X=(Min=-10.000000,Max=-10.000000))\n"
                "  Name=\"SpriteEmitter604\"\n End Object\n Emitters(0)=SpriteEmitter'MyLevel.SpriteEmitter604'\n AmbientSound=Sound'MyLevel.Hum'\n Location=(X=100,Y=200,Z=300)\n Tag=\"Smoke\"\nEnd Actor\n";
            auto beforeActors=call({{"op","actors"}});auto undoBefore=call({{"op","preview.state"}})["undo"];
            auto opened=call({{"op","preview.open"}});
            require(opened["parentIsHost"]==true && opened["fillsHost"]==true,"emitter preview viewport is parented into and fills its host panel");
            require(opened["renDev"]==true && opened["rendMap"]==5 && opened["cameraInPreviewLevel"]==true && (opened["showFlags"].get<unsigned>()&0x4800u)==0,"emitter preview draws its private level without realtime flags");
            auto shown=call({{"op","preview.show"},{"entry",{{"actors",J::array({{{"text",burst},{"rotation",J::array({0,16384,0})}}})}}}});
            require(shown["actors"].size()==1 && shown["actors"][0]["outer"]=="ReloadedEmitterPreview" && shown["actors"][0]["emitters"][0]["outer"]=="ReloadedEmitterPreview","previewed emitter and sub-emitter are created in the preview package");
            require(shown["warnings"].size()==1,"map-owned assets in an entry are dropped with a warning");
            call({{"op","preview.step"},{"seconds",0.4}});
            auto frame=call({{"op","preview.capture"},{"path",(directory/"emitter_preview.bmp").string()}});
            require(frame["particles"].get<int>()>0 && frame["nonBlackPixels"].get<int>()>50,"triggered one-shot burst plays and draws in the preview");
            auto looped=call({{"op","preview.step"},{"seconds",2.0}});
            require(looped["loops"].get<int>()>=1 && looped["particles"].get<int>()>0,"one-shot burst loops in the preview");
            require(call({{"op","actors"}})==beforeActors && call({{"op","preview.state"}})["undo"]==undoBefore,"emitter preview leaves map actors and Undo history untouched");
            bool rejected=false;try{call({{"op","preview.show"},{"entry",{{"actors",J::array({{{"text",std::string("Begin Actor Class=Emitter Name=E\n Begin Object Class=SpriteEmitter Name=S\n  Texture=Texture'NoSuchPackage.Missing'\n End Object\n Emitters(0)=SpriteEmitter'MyLevel.S'\nEnd Actor\n")}}})}}}});}catch(const std::exception&){rejected=true;}
            require(rejected && call({{"op","preview.state"}})["particles"].get<int>()>0,"an entry with a missing texture package is refused and the previous preview stays");
            Exec("OBJ GARBAGE");auto collected=call({{"op","preview.state"}});
            require(collected["level"]==true && collected["viewport"]==true && collected["faulted"]==false,"emitter preview survives garbage collection");
            // A viewport never moves between windows: closing deletes it through the engine
            // (with its camera) and opening again creates a new one in the new host.
            auto liveOf=[](const J& state,const char* cls){return state.contains("liveActorClasses")?state["liveActorClasses"].value(cls,0):-1;};
            auto beforeClose=call({{"op","preview.state"}});
            call({{"op","preview.close"}});auto closed=call({{"op","preview.state"}});
            require(closed["viewport"]==false && closed["attached"]==false && closed["clientViewports"].get<int>()==beforeClose["clientViewports"].get<int>()-1
                && liveOf(beforeClose,"Camera")==1 && liveOf(closed,"Camera")==0 && closed["viewportsClosed"].get<int>()==beforeClose["viewportsClosed"].get<int>()+1,"closing the emitter preview deletes its viewport and camera");
            auto previewReopened=call({{"op","preview.open"}});
            require(previewReopened["attached"]==true && previewReopened["parentIsHost"]==true && previewReopened["parentWindow"]==true && previewReopened["viewport"]==true && previewReopened["renDev"]==true
                && previewReopened["clientViewports"]==beforeClose["clientViewports"] && liveOf(previewReopened,"Camera")==1 && previewReopened["viewportsOpened"].get<int>()==beforeClose["viewportsOpened"].get<int>()+1,"reopening the emitter preview opens a new viewport in its new host");
            auto openAgain=call({{"op","preview.open"}});
            require(openAgain["hwnd"]==previewReopened["hwnd"] && openAgain["viewportsOpened"]==previewReopened["viewportsOpened"],"opening the open preview again keeps its viewport");
            call({{"op","preview.clear"}});call({{"op","preview.close"}});
        }
        require(reinterpret_cast<Save>(0x10E0416B)(*reinterpret_cast<void**>(kEditor),destination)!=0,"save final workflow map for restart test");
        {std::ifstream savedMap(destination,std::ios::binary);std::string bytes(std::istreambuf_iterator<char>(savedMap),{});require(!bytes.empty() && bytes.find("ReloadedEmitterPreview")==std::string::npos,"saved map holds no emitter preview objects");}
        auto playable=directory.parent_path()/"Packages"/"Maps"/std::filesystem::path(destination).filename();
        WorkflowProbe::previewScreenshot=directory/"map_package_preview.bmp";WorkflowProbe::packagePreviewChecked=false;
        auto packageTimer=SetTimer(nullptr,0,50,WorkflowProbe::Answer);
        call({{"op","package.preview"},{"map",playable.string()}});KillTimer(nullptr,packageTimer);WorkflowProbe::previewScreenshot.clear();
        require(WorkflowProbe::packagePreviewChecked,"packaging dialog scans the saved map and keeps the playable map checked");
        WorkflowProbe::packagePreviewChecked=false;
        packageTimer=SetTimer(nullptr,0,50,WorkflowProbe::Answer);
        SendMessage(frameWindow,WM_COMMAND,40928,0);KillTimer(nullptr,packageTimer);
        require(WorkflowProbe::packagePreviewChecked,"File menu packaging command opens its map chooser");
        std::ofstream(directory/"workflow_result.json")<<J({{"view",view},{"instance",instance}}).dump(2);
        finish("Native workflow replacement, connections, views, assembly insertion and in-place updates, undo/redo, rebuild and save/reopen verified.");
    }
    catch(const std::exception& e){Record("FAIL",e.what());}
}
