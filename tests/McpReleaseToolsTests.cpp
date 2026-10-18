// The release tool (Reloaded.Editor.Mcp/Tools/ReleaseTools.cpp) against a
// FakeBridge and a temporary game folder laid out like the game's: System
// with code packages, Packages\{Maps, MapsEd, Textures, StaticMeshes,
// Sounds}, a MapsEd map whose import table names its packages, a compressed
// <Map>-i.utc with the four interface textures and SMapSettings, and the
// .uas with RE+'s build log. Run from the repository root:
//   g++ -std=c++20 -Wall -Wextra -Wshadow -Werror -O1 tests/McpReleaseToolsTests.cpp Reloaded.Editor.Mcp/Tools/ReleaseTools.cpp
#include "../Reloaded.Editor.Mcp/FakeBridge.h"
#include "../Reloaded.Editor.Mcp/McpProtocolModel.h"
#include "../Reloaded.Editor.Mcp/ImageCodec.h"
#include "../Reloaded.Editor/LevelSnapshotModel.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
using namespace Mcp;
using Workflow::Json;
namespace fs=std::filesystem;
int checks=0;
void Check(bool value,const std::string& message,std::source_location where=std::source_location::current())
{
    ++checks;
    if(!value)throw std::runtime_error(message+" (line "+std::to_string(where.line())+")");
}

// ------------------------------------------------------------ packages
// A package writer independent of the reader under test: the SCCT layout
// (names XOR-coded by position from version 175, name references carrying a
// number), textures as the editor saves them, a MapSettings object, and
// extra imports (top-level packages the map takes objects from).
using Bytes=Snapshot::Bytes;
struct Builder
{
    Bytes b;std::vector<std::string> names;
    void U8(unsigned v){b.push_back(static_cast<std::uint8_t>(v));}
    void U16(unsigned v){U8(v);U8(v>>8);}
    void U32(std::uint32_t v){for(int i=0;i<4;++i)U8(v>>(8*i));}
    static void CompactInto(Bytes& out,int v)
    {
        unsigned a=static_cast<unsigned>(v<0?-v:v);
        std::uint8_t first=static_cast<std::uint8_t>((v<0?0x80:0)|(a&0x3f));
        a>>=6;if(a)first|=0x40;out.push_back(first);
        while(a){std::uint8_t c=static_cast<std::uint8_t>(a&0x7f);a>>=7;if(a)c|=0x80;out.push_back(c);}
    }
    int Name(const std::string& n)
    {
        for(size_t i=0;i<names.size();++i)if(names[i]==n)return static_cast<int>(i);
        names.push_back(n);return static_cast<int>(names.size()-1);
    }
    void NameRef(Bytes& out,const std::string& n){CompactInto(out,Name(n));CompactInto(out,4242);}
};
struct Tex{std::string name;int size,format;};
struct Import{std::string classPackage,className,name;int outer;};
Bytes TextureBlob(Builder& w,const Tex& t)
{
    Bytes blob;
    auto intProperty=[&](const char* name,int value){w.NameRef(blob,name);blob.push_back(0x22);for(int i=0;i<4;++i)blob.push_back(static_cast<std::uint8_t>(value>>(8*i)));};
    auto byteProperty=[&](const char* name,int value){w.NameRef(blob,name);blob.push_back(0x01);blob.push_back(static_cast<std::uint8_t>(value));};
    w.NameRef(blob,"bAlphaTexture");blob.push_back(0x83);
    intProperty("USize",t.size);intProperty("VSize",t.size);byteProperty("Format",t.format);
    w.NameRef(blob,"None");
    Builder::CompactInto(blob,1);
    for(int i=0;i<4;++i)blob.push_back(0);
    Builder::CompactInto(blob,16);
    for(int i=0;i<16;++i)blob.push_back(0x55);
    for(int v:{t.size,t.size})for(int i=0;i<4;++i)blob.push_back(static_cast<std::uint8_t>(v>>(8*i)));
    blob.push_back(8);blob.push_back(8);
    return blob;
}
Bytes MakePackage(const std::vector<Tex>& textures,bool mapSettings,const std::vector<Import>& extra={})
{
    const int version=300;
    Builder w;w.Name("None");
    std::vector<Bytes> blobs;
    for(const auto& t:textures)blobs.push_back(TextureBlob(w,t));
    if(mapSettings){Bytes blob;w.NameRef(blob,"None");blobs.push_back(blob);}
    std::vector<Import> imports={{"Core","Class","Texture",-2},{"Core","Package","Engine",0},{"Core","Class","SMapSettings",-4},{"Core","Package","SBase",0}};
    imports.insert(imports.end(),extra.begin(),extra.end());
    for(const auto& i:imports){w.Name(i.classPackage);w.Name(i.className);w.Name(i.name);}
    for(const auto& t:textures)w.Name(t.name);
    w.Name("MapSettings");
    Bytes nameTable;size_t position=64;
    for(const auto& n:w.names)
    {
        Bytes entry;Builder::CompactInto(entry,static_cast<int>(n.size()+1));position+=entry.size();
        for(size_t i=0;i<=n.size();++i){const std::uint8_t c=static_cast<std::uint8_t>(i<n.size()?static_cast<unsigned char>(n[i]):0);entry.push_back(static_cast<std::uint8_t>(c^(position&255)));++position;}
        for(int i=0;i<4;++i)entry.push_back(0x10);
        position+=4;
        nameTable.insert(nameTable.end(),entry.begin(),entry.end());
    }
    std::vector<std::uint32_t> offsets;std::uint32_t at=static_cast<std::uint32_t>(64+nameTable.size());
    for(const auto& blob:blobs){offsets.push_back(at);at+=static_cast<std::uint32_t>(blob.size());}
    Bytes importTable;
    for(const auto& i:imports)
    {
        w.NameRef(importTable,i.classPackage);w.NameRef(importTable,i.className);
        for(int k=0;k<4;++k)importTable.push_back(static_cast<std::uint8_t>(i.outer>>(8*k)));
        w.NameRef(importTable,i.name);
    }
    Bytes exportTable;
    for(size_t i=0;i<blobs.size();++i)
    {
        const bool texture=i<textures.size();
        Builder::CompactInto(exportTable,texture?-1:-3);Builder::CompactInto(exportTable,0);
        for(int k=0;k<4;++k)exportTable.push_back(0);
        w.NameRef(exportTable,texture?textures[i].name:"MapSettings");
        for(int k=0;k<4;++k)exportTable.push_back(k==0?4:0);
        Builder::CompactInto(exportTable,static_cast<int>(blobs[i].size()));
        Builder::CompactInto(exportTable,static_cast<int>(offsets[i]));
    }
    w.U32(Snapshot::kPackageMagic);w.U16(version);w.U16(0);w.U32(1);
    w.U32(static_cast<std::uint32_t>(w.names.size()));w.U32(64);
    w.U32(static_cast<std::uint32_t>(blobs.size()));w.U32(static_cast<std::uint32_t>(at+importTable.size()));
    w.U32(static_cast<std::uint32_t>(imports.size()));w.U32(at);
    for(int i=0;i<4;++i)w.U32(0x11111111u*static_cast<std::uint32_t>(i+1));
    w.U32(1);w.U32(static_cast<std::uint32_t>(blobs.size()));w.U32(static_cast<std::uint32_t>(w.names.size()));
    Check(w.b.size()==64,"header size");
    w.b.insert(w.b.end(),nameTable.begin(),nameTable.end());
    for(const auto& blob:blobs)w.b.insert(w.b.end(),blob.begin(),blob.end());
    w.b.insert(w.b.end(),importTable.begin(),importTable.end());
    w.b.insert(w.b.end(),exportTable.begin(),exportTable.end());
    return w.b;
}
// The .utc container: the package deflated behind its two sizes.
Bytes Utc(const Bytes& package)
{
    const auto stream=Image::ZlibCompress(std::string(package.begin(),package.end()));
    return Snapshot::Contain(package,Bytes(stream.begin(),stream.end()));
}
std::vector<Tex> GoodTextures()
{
    return {{"Menu",256,Snapshot::kFormatDxt5},{"Briefing",256,Snapshot::kFormatDxt5},{"LoadingATT0",512,Snapshot::kFormatDxt1},{"LoadingDEF0",512,Snapshot::kFormatDxt1}};
}

// ------------------------------------------------------------ the game
struct Game
{
    fs::path root;
    Game():root(fs::temp_directory_path()/("mcp-release-tests-"+std::to_string(reinterpret_cast<uintptr_t>(this))))
    {
        std::error_code e;fs::remove_all(root,e);
        for(const char* d:{"System","Packages/Maps","Packages/MapsEd","Packages/Textures","Packages/StaticMeshes","Packages/Sounds","Packages/Animations"})fs::create_directories(root/d);
        for(const char* code:{"Core","Engine","SBase","Echelon"})Text("System/"+std::string(code)+".u","code");
    }
    ~Game(){std::error_code e;fs::remove_all(root,e);}
    Game(const Game&)=delete;Game& operator=(const Game&)=delete;
    fs::path At(const std::string& relative)const{return root/relative;}
    void Put(const std::string& relative,const Bytes& bytes)
    {
        fs::create_directories(At(relative).parent_path());
        std::ofstream out(At(relative),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
        Check(static_cast<bool>(out),"wrote "+relative);
    }
    void Text(const std::string& relative,const std::string& text){Put(relative,Bytes(text.begin(),text.end()));}
    void Remove(const std::string& relative){std::error_code e;fs::remove(At(relative),e);}
    void Age(const std::string& relative,int minutes){fs::last_write_time(At(relative),fs::last_write_time(At(relative))-std::chrono::minutes(minutes));}
    // A finished AquaD as the wiki wants it.
    void Release()
    {
        Put("Packages/MapsEd/AquaD.sdc",MakePackage({},false,{{"Core","Package","AquaD_TXT",0},{"Core","Package","AquaD_STM",0},{"Core","Package","Amb_AquaD",0},
                                                             {"Core","Package","StockTex",0},{"Core","Package","MyLevel",0},{"Engine","Texture","Wall",-6}}));
        Text("Packages/Maps/AquaD.sdc","playable");
        Put("Packages/Textures/AquaD-i.utc",Utc(MakePackage(GoodTextures(),true)));
        Text("Packages/Textures/AquaD_TXT.utx","textures");
        Text("Packages/StaticMeshes/AquaD_STM.usx","meshes");
        Text("Packages/Sounds/Amb_AquaD.uax","sounds");
        Text("Packages/Sounds/AquaD.uas","uas");
        Log({"Interface","Amb_AquaD"});
        Text("Packages/Textures/StockTex.utx","stock textures");
        Text("Packages/Sounds/Interface.uax","interface sounds");
        Text("Packages/MapsEd/OtherD.sdc","another map");
        Text("Packages/Textures/OtherD-i.utx","another map's stray");
    }
    void Log(const std::vector<std::string>& packages)
    {
        std::string text="Build UAS log for AquaD.uas\r\n  Total Stream-flagged sounds in selected packages: 3\r\n\r\n";
        text+="Package                   Sound                                     Flags         WAV bytes      OGG bytes      Status / WAV path\r\n";
        text+="-------                   -----                                     -----         ---------      ---------      -----------------\r\n";
        for(const auto& p:packages)text+=p+"                 "+p+"_music                                0x00000004  1000           500            OK (x.wav)\r\n";
        text+="\r\nTotal OGG payload bytes (sum across 2 files): 1000\r\n";
        Text("Packages/Sounds/AquaD.uas.buildlog.txt",text);
    }
};

struct Harness
{
    Game game;
    FakeBridge fake;
    Registry registry;
    ToolContext context;
    Harness():context{fake,game.At("System"),game.At("work")}
    {
        RegisterReleaseTools(registry);
        game.Release();
        fake.On("editor.state",Json{{"open",true},{"mapFile",(game.At("Packages/MapsEd/AquaD.sdc")).string()},{"dirty",false}});
        fake.On("mcp.sidecar.get",Json{{"sidecar",{{"spec",{{"map",{{"name","AQUARIUS"}}}}}}}});
        fake.On("optimise.report",Json{{"packs",Json::array({{{"name","StockTex"},{"file",game.At("Packages/Textures/StockTex.utx").string()},{"fileSize",8000000},{"usedSize",40000},
                                                              {"suggested",true},{"installed",false},{"assets",Json::array({{{"path","StockTex.Walls.A"},{"class","Texture"},{"size",40000},{"found",true}}})}}})},
                                       {"inside",Json::array()},{"text","report"}});
    }
    ToolResult Call(const Json& args)
    {
        const auto* tool=registry.FindTool("release");
        Check(tool!=nullptr,"release is registered");
        const auto problem=Protocol::CheckArguments(tool->inputSchema,args);
        Check(problem.empty(),"arguments fit the schema: "+problem);
        return tool->run(args,context);
    }
    Json Ok(const Json& args)
    {
        auto r=Call(args);
        Check(!r.isError,"release "+args.dump()+" succeeds: "+(r.content.empty()?std::string():r.content.front().text.substr(0,2000)));
        return r.structured;
    }
    std::string Error(const Json& args)
    {
        auto r=Call(args);
        Check(r.isError,"release "+args.dump()+" fails");
        return r.structured.value("error",std::string());
    }
};
size_t Findings(const Json& r,const std::string& severity,const std::string& rule={})
{
    size_t n=0;
    for(const auto& f:r.at("findings"))n+=f.at("severity")==severity && (rule.empty() || f.at("rule")==rule);
    return n;
}
bool Says(const Json& r,const std::string& rule,const std::string& text)
{
    for(const auto& f:r.at("findings"))if(f.at("rule")==rule && f.at("message").get<std::string>().find(text)!=std::string::npos)return true;
    return false;
}
bool Contains(const Json& list,const std::string& text)
{
    for(const auto& item:list)if((item.is_string()?item.get<std::string>():item.dump()).find(text)!=std::string::npos)return true;
    return false;
}
const Json* Entry(const Json& layout,const std::string& archive)
{
    for(const auto& f:layout.at("files"))if(f.at("archive")==archive)return &f;
    return nullptr;
}

void CleanCheck()
{
    Harness h;
    const auto r=h.Ok({{"action","check"}});
    Check(r.at("map")=="AquaD","the open map is checked");
    Check(r.at("ready")==true && Findings(r,"error")==0 && Findings(r,"warning")==0,"a finished map has no errors or warnings: "+r.at("findings").dump());
    Check(Findings(r,"info","release.optimise")==1,"Optimise Map Assets' suggestion is a note");
    Check(r.at("interface").at("settings")==true && r.at("interface").at("textures").at("Menu").at("format")=="DXT5" &&
          r.at("interface").at("textures").at("LoadingDEF0").at("width")==512,"the .utc is read: "+r.at("interface").dump());
    Check(r.at("uas").at("verified")==true && Contains(r.at("uas").at("packages"),"Interface"),"the .uas's packages come from its build log");
    Check(r.at("release").at("uas").at("packages").size()==2 && r.at("release").at("packages").size()==3,"the release block for map_validate: "+r.at("release").dump());
    Check(Contains(r.at("notes"),"StockTex"),"stock dependencies are noted: "+r.at("notes").dump());
    bool sawStock=false,sawOwn=false;
    for(const auto& d:r.at("dependencies")){if(d.at("name")=="StockTex")sawStock=d.at("mapOwned")==false;if(d.at("name")=="AquaD_TXT")sawOwn=d.at("mapOwned")==true;}
    Check(sawStock && sawOwn,"dependencies come from the import table, without code packages and MyLevel: "+r.at("dependencies").dump());
    for(const auto& d:r.at("dependencies"))Check(d.at("name")!="Engine" && d.at("name")!="MyLevel" && d.at("name")!="SBase","no code package or MyLevel");
    const auto& layout=r.at("layout");
    for(const char* file:{"Packages\\Maps\\AquaD.sdc","Packages\\MapsEd\\AquaD.sdc","Packages\\Textures\\AquaD-i.utc","Packages\\Sounds\\AquaD.uas",
                          "Packages\\Textures\\AquaD_TXT.utx","Packages\\StaticMeshes\\AquaD_STM.usx","Packages\\Sounds\\Amb_AquaD.uax"})
        Check(Entry(layout,file) && Entry(layout,file)->at("present")==true,std::string("layout lists ")+file);
    Check(layout.at("files").size()==7 && layout.at("missing").empty(),"only the map's files are listed: "+layout.dump());
    Check(h.fake.Count("optimise.report")==1 && h.fake.Count("mcp.sidecar.get")==1,"the editor is asked for the report and the map name");
    Check(r.at("summary").get<std::string>().find("Ready to package")!=std::string::npos,"the summary says so");
    // The shorthand map_validate's fixes use.
    Check(h.Ok({{"check",true}}).at("ready")==true,"check: true is action check");
    Check(h.Error({{"action","check"},{"sounds",true}}).find("one action")!=std::string::npos,"two actions are refused");
    Check(h.Error(Json::object()).find("action is")!=std::string::npos,"an action is needed");
}

void BrokenNames()
{
    Harness h;
    h.game.Text("Packages/Textures/AquaD-i.utx","stray");
    h.game.Text("Packages/StaticMeshes/AquaD_TXT.usx","clash");
    h.game.Text("Packages/Textures/AquaD.utx","clash with the map");
    h.game.Text("Packages/Sounds/AquaD_STM.uax","another clash");
    auto r=h.Ok({{"action","check"}});
    Check(Says(r,"release.files","stray copy"),"a stray -i.utx is a warning to delete it");
    Check(Says(r,"release.package-names","AquaD_TXT") && Says(r,"release.package-names","one package"),"one base name in two folders is an error");
    Check(Says(r,"release.package-names","map's own base name"),"a package named like the map is an error");
    Check(Says(r,"release.naming-convention","named like a texture package"),"_TXT as .usx is noted");
    Check(r.at("ready")==false && Findings(r,"error","release.package-names")==3,"three name errors: "+r.at("findings").dump());
    Check(r.at("findings").at(0).at("severity")=="error","errors come first");
    const auto* stray=Entry(r.at("layout"),"Packages\\Textures\\AquaD-i.utx");
    Check(stray==nullptr && Contains(r.at("warnings"),"left out"),"the stray .utx is not in the archive");
}

void MissingFiles()
{
    Harness h;
    h.game.Remove("Packages/Maps/AquaD.sdc");
    h.game.Remove("Packages/Sounds/AquaD.uas");
    h.game.Remove("Packages/Textures/AquaD-i.utc");
    h.game.Remove("Packages/StaticMeshes/AquaD_STM.usx");
    auto r=h.Ok({{"action","check"}});
    Check(Says(r,"release.files","Packages\\Maps\\AquaD.sdc"),"no playable copy");
    Check(Says(r,"release.uas","always build one"),"no .uas is an error");
    Check(Says(r,"release.files","AquaD-i.utc"),"no interface package");
    Check(Says(r,"release.dependencies","AquaD_STM"),"a map-owned package the map uses with no file");
    Check(Findings(r,"error")==4 && r.at("ready")==false,"four errors: "+r.at("findings").dump());
    Check(r.at("layout").at("missing").size()==3,"the layout lists what is missing");
    bool sounds=false;
    for(const auto& n:r.at("next"))sounds=sounds || (n.at("tool")=="release" && n.at("args").value("action",std::string())=="sounds");
    Check(sounds,"next offers the fixes");

    Harness stale;
    stale.game.Age("Packages/Maps/AquaD.sdc",30);
    stale.game.Age("Packages/Sounds/AquaD.uas.buildlog.txt",30);
    stale.fake.Clear("editor.state");
    stale.fake.On("editor.state",Json{{"open",true},{"mapFile",stale.game.At("Packages/MapsEd/AquaD.sdc").string()},{"dirty",true}});
    r=stale.Ok({{"action","check"}});
    Check(Says(r,"release.files","older than the MapsEd file"),"an old playable copy is a warning");
    Check(Says(r,"release.files","unsaved changes"),"unsaved changes are a warning");
    Check(Says(r,"release.uas","cannot be checked") && r.at("uas").at("verified")==false,"an old build log cannot vouch for the .uas");
    Check(Findings(r,"error")==0,"no errors");

    Harness noInterface;
    noInterface.game.Log({"Amb_AquaD"});
    r=noInterface.Ok({{"action","check"}});
    Check(Says(r,"release.uas","without Interface"),"a .uas without Interface is a warning");
}

void InterfaceTextures()
{
    Harness h;
    auto textures=GoodTextures();
    textures[0].format=Snapshot::kFormatDxt1;
    textures.pop_back();
    h.game.Put("Packages/Textures/AquaD-i.utc",Utc(MakePackage(textures,false)));
    auto r=h.Ok({{"action","check"}});
    Check(Says(r,"interface.textures","Menu is 256x256 DXT1; the game expects 256x256 DXT5"),"a wrong format is a warning: "+r.at("findings").dump());
    Check(Says(r,"interface.textures","no LoadingDEF0"),"a missing slot is a warning");
    Check(Says(r,"settings.missing","SMapSettings"),"no SMapSettings is a warning");
    Check(Findings(r,"error")==0,"interface problems are warnings");
    // An uncompressed package reads too.
    h.game.Put("Packages/Textures/AquaD-i.utc",MakePackage(GoodTextures(),true));
    r=h.Ok({{"action","check"}});
    Check(Findings(r,"warning")==0 && r.at("interface").at("exports")==5,"a plain package is read as is");
    h.game.Text("Packages/Textures/AquaD-i.utc","garbage that is no package");
    r=h.Ok({{"action","check"}});
    Check(r.at("interface").contains("unreadable") && Says(r,"interface.textures","could not be read"),"an unreadable package is reported");
}

void NamingAndOffline()
{
    Harness h;
    auto r=h.Ok({{"action","check"},{"mapName","Hotel"}});
    Check(Says(r,"release.naming-convention","this map: HoteD"),"the name convention uses the map name");
    Check(Findings(r,"error")==0,"the convention is a note");
    r=h.Ok({{"action","check"},{"map","Aqua D"}});
    Check(Says(r,"release.file-name","ASCII name without spaces"),"a space in the file name is an error");
    Check(!r.at("live").get<bool>() && Contains(r.at("notes"),"only the files"),"another map than the open one is checked from its files");
    // No editor: the files are still checked.
    h.fake.connected=false;
    r=h.Ok({{"action","check"},{"map","AquaD.sdc"},{"mapName","Aquarius"}});
    Check(r.at("ready")==true && Contains(r.at("notes"),"No editor answered"),"offline checks work");
    Check(h.Error({{"action","check"}}).find("Name the map")!=std::string::npos,"with no editor the map must be named");
    // A map in no game folder.
    Harness lost;
    lost.context.systemDir=lost.game.At("nowhere/System");
    Check(lost.Error({{"action","check"},{"map","AquaD"}}).find("Packages folder")!=std::string::npos,"the exe must be in System");
}

void BaseInstall()
{
    Harness h;
    const auto base=h.game.At("clean");
    fs::create_directories(base/"Packages"/"Textures");
    fs::copy_file(h.game.At("Packages/Textures/StockTex.utx"),base/"Packages"/"Textures"/"StockTex.utx");
    auto r=h.Ok({{"action","check"},{"base",base.string()}});
    Check(Findings(r,"error")==0 && Findings(r,"warning")==0,"an unmodified stock package passes");
    Check(h.fake.Last("optimise.report")->args.at("base")==base.string(),"the report compares with the base install");
    h.game.Text("Packages/Textures/StockTex.utx","stock textures, edited");
    r=h.Ok({{"action","check"},{"base",base.string()}});
    Check(Says(r,"release.package-names","stock package was modified"),"a modified stock package is an error");
    bool flagged=false;
    for(const auto& p:r.at("release").at("packages"))flagged=flagged || (p.at("name")=="StockTex.utx" && p.value("modified",false));
    Check(flagged,"map_validate's release block marks it modified");
    fs::remove(base/"Packages"/"Textures"/"StockTex.utx");
    r=h.Ok({{"action","check"},{"base",base.string()}});
    Check(Says(r,"release.dependencies","not in the base install"),"a custom package players lack is a warning");
    Check(h.Error({{"action","check"},{"base",h.game.root.string()}}).find("separate")!=std::string::npos,"the game folder is no base install");
    Check(h.Error({{"action","check"},{"base",h.game.At("none").string()}}).find("Packages")!=std::string::npos,"a base without Packages is refused");
    // A package the map imports that is nowhere.
    h.game.Put("Packages/MapsEd/AquaD.sdc",MakePackage({},false,{{"Core","Package","Vanished",0}}));
    r=h.Ok({{"action","check"}});
    Check(Says(r,"release.dependencies","Vanished"),"a missing package the map uses is an error");
}

void Sounds()
{
    Harness h;
    Json sent;
    h.fake.On("uas.build",[&](const Json& args,const CallOptions&)
    {
        sent=args;
        return Json{{"file",h.game.At("Packages/Sounds/AquaD.uas").string()},{"size",123456},{"packages",{"Interface","Amb_AquaD","Amb_Extra"}},{"encoded",4},
                    {"skipped",Json::array()},{"withoutStreamSounds",Json::array({"Amb_Extra"})},{"quality",5.0}};
    });
    auto r=h.Ok({{"action","sounds"},{"packages",{"Amb_Extra","Interface"}}});
    Check(sent.at("quality")==5.0 && sent.at("packages")==Json::array({"Amb_Extra"}),"quality 5 and only the extra packages are sent: "+sent.dump());
    Check(r.at("summary").get<std::string>().find("Interface, Amb_AquaD, Amb_Extra")!=std::string::npos,"the summary names the packages");
    Check(Contains(r.at("warnings"),"Amb_Extra.uax") && Contains(r.at("warnings"),"no Stream-flagged"),"warnings for a package not on disk and one without streams");
    Check(h.fake.Last("uas.build")->timeoutMs>=1800000,"a build may take minutes");
    r=h.Ok({{"action","sounds"},{"quality",4},{"dryRun",true}});
    Check(h.fake.Last("uas.build")->dryRun && r.at("summary").get<std::string>().find("Dry run")==0,"a dry run is passed on");
    Check(Contains(r.at("warnings"),"below 5.0"),"a low quality is warned about");
    Check(h.Error({{"action","sounds"},{"packages",{"Amb Bad"}}}).find("letters")!=std::string::npos,"bad package names are refused");
    Check(h.Error({{"action","sounds"},{"map","HoteD"}}).find("open")!=std::string::npos,"another map than the open one is refused");
    h.fake.Clear("editor.state");
    h.fake.On("editor.state",Json{{"open",true},{"mapFile",""}});
    Check(h.Error({{"action","sounds"}}).find("no file yet")!=std::string::npos,"an unsaved map is refused");
}

void OptimiseAndPackage()
{
    Harness h;
    // The report alone.
    auto r=h.Ok({{"action","optimise"}});
    Check(r.at("packs").size()==1 && r.at("packs").at(0).at("suggested")==true && r.at("next").at(0).at("args").at("packs")==Json::array({"StockTex"}),"the report suggests StockTex");
    // The release copy.
    const auto folder=h.game.At("Releases/AquaD");
    Json sent;
    h.fake.On("optimise.release",[&](const Json& args,const CallOptions&)
    {
        sent=args;
        h.game.Text("Releases/AquaD/Packages/Maps/AquaD.sdc","release playable");
        h.game.Text("Releases/AquaD/Packages/MapsEd/AquaD.sdc","release editable");
        h.game.Put("Releases/AquaD/Packages/Textures/AquaD-i.utc",Utc(MakePackage(GoodTextures(),true)));
        if(args.value("destination",std::string())=="package")h.game.Text("Releases/AquaD/Packages/StaticMeshes/"+args.at("assetPackage").get<std::string>()+".usx","moved");
        return Json{{"moved",Json::array({{{"from","StockTex.Walls.A"},{"to","AquaD.Walls.A"}}})},{"written",Json::array()},{"needs",Json::array({{{"name","Engine"}},{{"name","OtherStock"}}})},
                    {"left",Json::array()},{"rounds",1},{"release","AquaD"},{"folder",folder.string()},{"playable",(folder/"Packages/Maps/AquaD.sdc").string()},{"intoMap",true}};
    });
    r=h.Ok({{"action","optimise"},{"suggested",true}});
    Check(sent.at("packs")==Json::array({"StockTex"}) && sent.at("destination")=="map" && sent.at("overwrite")==false,"suggested packs go to optimise.release: "+sent.dump());
    Check(r.at("summary").get<std::string>().find("1 asset moved")!=std::string::npos,"the summary counts the moved assets");
    Check(Contains(r.at("warnings"),"OtherStock") && !Contains(r.at("warnings"),"Engine"),"what the copy still needs is named, code packages aside");
    Check(h.Error({{"action","optimise"},{"packs",{"StockTex"}},{"destination","package"},{"assetPackage","StockTex2"}}).find("map's own")!=std::string::npos,"a stock name is refused");
    Check(h.Error({{"action","optimise"},{"packs",{"StockTex"}},{"destination","package"},{"assetPackage","AquaD_TXT"}}).find("exists already")!=std::string::npos,"an existing package is refused");
    Check(h.Error({{"action","optimise"},{"packs",{"StockTex"}},{"destination","package"}}).find("_STM.usx exists")!=std::string::npos,"the default name is not taken over an existing file");
    r=h.Ok({{"action","optimise"},{"packs",{"StockTex"}},{"destination","package"},{"assetPackage","AquaD_Pack"},{"overwrite",true},{"folder",folder.string()}});
    Check(sent.at("assetPackage")=="AquaD_Pack" && sent.at("overwrite")==true && sent.at("folder")==folder.string(),"the asset package, overwrite and folder are passed on");
    // The release copy goes only where the editor writes it: <Game>\Releases,
    // the work folder or [Mcp] WriteRoots; anything else is refused before
    // the editor is asked (it would delete and rewrite the map's files there).
    const auto calls=h.fake.Count("optimise.release");
    const auto elsewhere=h.game.At("OtherInstall");
    auto refused=h.Call({{"action","optimise"},{"packs",{"StockTex"}},{"overwrite",true},{"folder",elsewhere.string()}});
    Check(refused.isError && refused.structured.value("code",std::string())=="refused" && refused.structured.value("reason",std::string())=="outside_write_roots" &&
          refused.structured.value("error",std::string()).find("WriteRoots")!=std::string::npos && h.fake.Count("optimise.release")==calls,"a folder outside the roots is refused first");
    Check(h.Error({{"action","optimise"},{"packs",{"StockTex"}},{"folder",(h.game.At("Releases")/".."/"OtherInstall").string()}}).find("WriteRoots")!=std::string::npos,"\"..\" does not climb out");
    Check(h.Error({{"action","optimise"},{"packs",{"StockTex"}},{"folder","Releases/AquaD"}}).find("full path")!=std::string::npos,"a relative folder is refused");
    Check(h.Error({{"action","package"},{"packs",{"StockTex"}},{"folder",elsewhere.string()}}).find("WriteRoots")!=std::string::npos,"package's release copy too");
    h.game.Text("System/Reloaded_Editor.ini","[Mcp]\r\nWriteRoots=C:\\nowhere; \""+elsewhere.string()+"\"\r\n");
    h.Ok({{"action","optimise"},{"packs",{"StockTex"}},{"folder",(elsewhere/"AquaD").string()}});
    Check(h.fake.Count("optimise.release")==calls+1 && sent.at("folder")==(elsewhere/"AquaD").string(),"a WriteRoots folder is allowed");
    h.Ok({{"action","optimise"},{"packs",{"StockTex"}},{"folder",(h.game.At("work")/"rel").string()}});
    Check(h.fake.Count("optimise.release")==calls+2,"so is the work folder");
    fs::remove(h.game.At("System/Reloaded_Editor.ini"));

    // package: the layout from the release copy.
    r=h.Ok({{"action","package"},{"packs",{"StockTex"}}});
    const auto& layout=r.at("layout");
    Check(Entry(layout,"Packages\\Maps\\AquaD.sdc")->at("source").get<std::string>().find("Releases")!=std::string::npos,"the release copy's map is taken");
    Check(Entry(layout,"Packages\\Sounds\\AquaD.uas")->at("source").get<std::string>().find("Releases")==std::string::npos,"the .uas comes from the game folder");
    Check(Entry(layout,"Packages\\Textures\\AquaD_TXT.utx")!=nullptr && layout.at("missing").empty(),"the map's own packages are listed");
    Check(r.at("folder")==folder.string() && Contains(r.at("dependencies"),"StockTex"),"the folder and the dependencies are reported");
    // package without packs lists the game folder's files only.
    const auto before=h.fake.Count("optimise.release");
    r=h.Ok({{"action","package"}});
    Check(h.fake.Count("optimise.release")==before && Entry(r.at("layout"),"Packages\\Maps\\AquaD.sdc")->at("source").get<std::string>().find("Releases")==std::string::npos,
          "without packs no release copy is made");
    r=h.Ok({{"action","package"},{"packs",{"StockTex"}},{"dryRun",true}});
    Check(h.fake.Last("optimise.release")->dryRun && r.at("summary").get<std::string>().find("Dry run")==0,"a dry run makes no copy");
    Check(h.Error({{"action","package"},{"map","HoteD"},{"packs",{"StockTex"}}}).find("open in the editor")!=std::string::npos,"a release copy needs the map open");
    h.fake.connected=false;
    r=h.Ok({{"action","package"},{"map","AquaD"}});
    Check(r.at("layout").at("files").size()==7,"offline, the archive is listed from the game folder");
}

void InterfaceImages()
{
    Harness h;
    const auto menu=h.game.At("work/menu.png"),loading=h.game.At("work/loading.jpg");
    h.game.Text("work/menu.png","png");h.game.Text("work/loading.jpg","jpg");
    std::vector<std::string> slots;
    h.fake.On("interface.texture",[&](const Json& args,const CallOptions&)
    {
        slots.push_back(args.at("slot").get<std::string>());
        Check(fs::path(args.at("file").get<std::string>()).is_absolute(),"full paths are sent");
        return Json{{"slot",args.at("slot")},{"package","AquaD-i"}};
    });
    auto r=h.Ok({{"action","interface"},{"images",{{"menu",menu.string()},{"loadingDef",loading.string()}}}});
    Check(slots==std::vector<std::string>({"Menu","LoadingDEF0"}),"slots in the wiki's order");
    Check(r.at("changed").size()==2 && r.at("interface").at("textures").size()==4,"the package is read back");
    r=h.Ok({{"action","interface"},{"images",{{"menu",menu.string()},{"briefing","relative.png"},{"loadingAtt",h.game.At("work/none.png").string()}}}});
    Check(r.at("failures").size()==2 && Contains(r.at("warnings"),"full path") && Contains(r.at("warnings"),"no file"),"bad files are reported, the rest imported");
    Check(h.Error({{"action","interface"},{"images",{{"briefing","relative.png"}}}}).find("No interface texture")!=std::string::npos,"nothing imported is an error");
    Check(h.Error({{"action","interface"}}).find("images")!=std::string::npos,"images are needed");
    h.fake.Clear("interface.texture");
    h.fake.Fail("interface.texture",Bridge::Code::Invalid,"Menu is 256 x 256; menu.tga is 128 x 128.");
    Check(h.Error({{"action","interface"},{"images",{{"menu",menu.string()}}}}).find("128")!=std::string::npos,"the editor's refusal is passed on");
}

void Schema()
{
    Registry registry;
    RegisterReleaseTools(registry);
    Check(registry.tools.size()==1,"one tool");
    const auto& t=registry.tools.front();
    Check(t.name=="release" && !t.readOnly && t.destructive,"annotations");
    for(const char* text:{"scct://wiki/map-packaging","scct://docs/tool-reference#release","Amb_<Map>","<Map>-i.utc","Interface","5.0","base","folder"})
        Check(t.description.find(text)!=std::string::npos,std::string("the description cites ")+text);
    // The details moved to the tool reference, so tools/list stays small.
    std::ifstream file(fs::path(__FILE__).parent_path().parent_path()/"Reloaded.Editor.Mcp"/"Resources"/"docs"/"tool-reference.md",std::ios::binary);
    const std::string reference((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    const auto section=reference.substr(reference.find("## release"));
    for(const char* text:{"Amb_<Map>.uax","256x256 DXT5","512x512 DXT1","AquaD","-i.utx","[Mcp] WriteRoots"})
        Check(section.find(text)!=std::string::npos,std::string("the tool reference's release section cites ")+text);
    Check(Protocol::ToolJson(t,"2025-06-18").dump().size()<5000,"release stays under 5 KB in tools/list");
    const auto protocol=Protocol::ToolJson(t,"2025-06-18");
    Check(protocol.at("inputSchema").at("properties").contains("dryRun") && protocol.contains("outputSchema"),"the published tool");
    Check(!Protocol::CheckArguments(t.inputSchema,{{"action","check"},{"bogus",1}}).empty(),"the schema is closed");
    Check(!Protocol::CheckArguments(t.inputSchema,{{"action","sounds"},{"quality",11}}).empty(),"quality is 0-10");
}

int main()
{
    try
    {
        CleanCheck();
        BrokenNames();
        MissingFiles();
        InterfaceTextures();
        NamingAndOffline();
        BaseInstall();
        Sounds();
        OptimiseAndPackage();
        InterfaceImages();
        Schema();
        std::cout<<"McpReleaseToolsTests passed ("<<checks<<" checks)\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"McpReleaseToolsTests failed: "<<e.what()<<"\n";return 1;}
}
