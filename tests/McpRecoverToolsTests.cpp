// Recovering compiled maps over MCP: RecoveryReportModel.h (the report's
// categories, the editor's default names, Recovery.txt read back), map_recover
// against a FakeBridge (the preflight, dry runs, progress, each refusal,
// cancelling), map_file open of a compiled map, and map_query what:"recovery"
// on tests/mcp/fixtures/Recovery.txt, with and without the editor. Run from
// the repository root: it reads the fixture and Reloaded.Editor/MapRecovery.cpp.
#include "../Reloaded.Editor.Mcp/McpServer.h"
#include "../Reloaded.Editor.Mcp/FakeBridge.h"
#include "../Reloaded.Editor.Mcp/RecoveryReportModel.h"
#include "../Reloaded.Editor.Mcp/Tools/Common.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <regex>
#include <source_location>
#include <sstream>
using namespace Mcp;
using Tools::PathText;
namespace fs=std::filesystem;
void Check(bool value,const std::string& message,std::source_location where=std::source_location::current())
{
    if(!value)throw std::runtime_error(message+" (line "+std::to_string(where.line())+")");
}
std::string ReadText(const fs::path& file)
{
    std::ifstream in(file,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot read "+file.string());
    std::ostringstream text;text<<in.rdbuf();return text.str();
}
const fs::path kFixture="tests/mcp/fixtures/Recovery.txt";

// The server over strings, with the game's folders in a temporary directory:
// System (where the exe lives) and Packages beside it.
struct Harness
{
    FakeBridge fake;
    Registry registry;
    std::atomic<int> cancels{0};
    std::atomic<bool> stop{false};
    std::mutex m;
    std::vector<Json> out;
    fs::path dir;
    std::unique_ptr<Server> server;
    int next=1;
    Harness()
    {
        dir=fs::temp_directory_path()/("mcp-recover-tests-"+std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(dir/"System");
        fs::create_directories(dir/"Packages"/"MapsEd");
        fs::create_directories(dir/"Packages"/"Maps");
        RegisterSessionTools(registry);RegisterQueryTools(registry);
        ServerOptions o;o.systemDir=dir/"System";o.workDir=dir/"work";o.version="test-1";
        o.cancel=[this]{++cancels;stop=true;};
        server=std::make_unique<Server>(registry,fake,o,[this](const std::string& line){std::lock_guard<std::mutex> g(m);out.push_back(Json::parse(line));});
        Request("initialize",{{"protocolVersion","2025-06-18"},{"capabilities",Json::object()},{"clientInfo",{{"name","tests"},{"version","1"}}}});
        Send({{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        fake.On("editor.state",State());
    }
    ~Harness(){stop=true;server.reset();std::error_code e;fs::remove_all(dir,e);}
    static Json State(bool dirty=false,const std::string& file="")
    {
        return {{"open",true},{"level",7},{"generation",2},{"revision",11},{"mapFile",file},{"dirty",dirty},{"canUndo",false},{"canRedo",false}};
    }
    void Send(const Json& message){server->Handle(message.dump());}
    Json Find(const Json& id)
    {
        std::lock_guard<std::mutex> g(m);
        for(const auto& message:out)if(message.contains("id") && message.at("id")==id && (message.contains("result") || message.contains("error")))return message;
        return Json();
    }
    Json Request(const std::string& method,const Json& params=Json::object())
    {
        const Json id=next++;
        Send({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
        server->Wait();
        return Find(id);
    }
    Json Call(const std::string& tool,const Json& arguments=Json::object(),Json meta=Json())
    {
        Json params{{"name",tool},{"arguments",arguments}};
        if(!meta.is_null())params["_meta"]=meta;
        auto reply=Request("tools/call",params);
        if(!reply.contains("result"))throw std::runtime_error(tool+" failed: "+reply.dump());
        return reply.at("result");
    }
    std::vector<Json> Notes(const std::string& method)
    {
        std::lock_guard<std::mutex> g(m);std::vector<Json> notes;
        for(const auto& message:out)if(message.value("method",std::string())==method)notes.push_back(message);
        return notes;
    }
    void Touch(std::string relative){std::replace(relative.begin(),relative.end(),'\\','/');const auto path=dir/"Packages"/relative;fs::create_directories(path.parent_path());std::ofstream(path)<<"x";}
};
std::string Text(const Json& result){return result.at("content")[0].at("text").get<std::string>();}
const Json& Structured(const Json& result){return result.at("structuredContent");}
bool Contains(const std::string& text,const std::string& part){return text.find(part)!=std::string::npos;}
// The preflight's answer (a dry run of map.recover) for these arguments.
Json Preflight(const Json& args)
{
    return {{"dryRun",true},{"source","C:\\Game\\Packages\\Maps\\"+args.value("source",std::string())+".sdc"},{"destination","C:\\Game\\Packages\\MapsEd\\"+args.value("destination",std::string())},
            {"merge",args.value("merge",true)},{"keepXboxOnly",args.value("keepXboxOnly",false)},{"imports",42},{"missing",Json::array()},{"sourceBytes",9437184},
            {"checks",{"source is a compiled map","destination is free","all 42 imported packages are installed"}}};
}
Json Recovered(const Json& args)
{
    Json rows=Json::array({{{"category","Thin brush"},{"item","RecoveredVolume12"},{"detail","Only 0.5 units across."},{"actor","RecoveredVolume12"}},
                           {{"category","Reference cleared"},{"item","SMover7"},{"detail","Pointed at an actor that was left out."},{"actor","SMover7"}},
                           {{"category","Asset moved"},{"item","MyLevel.Props.Crate01"},{"detail","Now in StaticMeshes."}},
                           {{"category","BSP lighting not preserved"},{"item","Surface 42"},{"detail","300 of 512 texels."},{"surface",42}}});
    const auto stem=Recovery::Stem(args.value("destination",std::string()));
    return {{"ok",true},{"file","C:\\Game\\Packages\\MapsEd\\"+stem+".sdc"},{"source","C:\\Game\\Packages\\Maps\\OffsD.sdc"},{"merge",args.value("merge",true)},
            {"keepXboxOnly",args.value("keepXboxOnly",false)},{"seconds",312.4},
            {"summary","Brushes: 18,422 cells from the compiled BSP -> 2,104 structural brushes (89% fewer).\r\nBrush faces: 110,532 -> 14,980 (86% fewer)."},
            {"counts",{{"merged",true},{"cells",18422},{"cellFaces",110532},{"joinedBrushes",8552},{"brushes",2104},{"brushFaces",14980},{"grown",1204},{"absorbed",3410},
                       {"splitBack",0},{"sheetBrushes",12},{"polygons",16204},{"unifiedSurfaces",1877},{"actors",1311}}},
            {"categories",{{"Thin brush",7},{"Reference cleared",1},{"Asset moved",1},{"BSP lighting not preserved",1}}},
            {"rows",rows},{"rowsTotal",10},{"report","C:\\Game\\Packages\\MapsEd\\Recovery\\"+stem+"\\Recovery.txt"},{"folder","C:\\Game\\Packages\\MapsEd\\Recovery\\"+stem},
            {"playable","C:\\Game\\Packages\\Maps\\"+stem+".sdc"},{"assetPackage","C:\\Game\\Packages\\StaticMeshes\\"+stem+"_Assets.usx"}};
}

// ------------------------------------------------------------- the model
void Model()
{
    const auto text=ReadText(kFixture);
    const auto report=Recovery::Parse(text);
    Check(report.rows.size()==9 && report.declared==9 && !report.clean,"every row of the fixture, and the count its items line declares");
    Check(report.summary.rfind("Brushes: 18,422 cells",0)==0 && std::count(report.summary.begin(),report.summary.end(),'\n')==2 && !Contains(report.summary,"\r"),
          "the counts block, CRLF read as lines: "+report.summary);
    Check(report.facts.at("Retained actors")==1311 && report.facts.at("Target platform")=="PC (common and PC-only actors)","facts, numbers as numbers");
    Check(Contains(report.assetPackage,"OffsD_Recovered_Assets.usx") && !report.notes.empty(),"the asset dependency and the notes");
    const auto& rows=report.rows;
    Check(rows[4].at("category")=="Thin brush" && rows[4].at("actor")=="RecoveredVolume12" && Contains(rows[4].at("detail").get<std::string>(),"sliver"),"a thin brush names its actor");
    Check(!rows[0].contains("actor") && !rows[3].contains("actor"),"left-out actors and moved assets are not in the map to frame");
    Check(rows[7].at("surface")==42 && !rows[8].contains("surface") && !rows[7].contains("actor"),"a BSP surface by index; \"More surfaces\" is none");
    // LF only, a BOM, and the clean report.
    std::string lf=text;lf.erase(std::remove(lf.begin(),lf.end(),'\r'),lf.end());
    Check(Recovery::Parse("\xEF\xBB\xBF"+lf).rows==report.rows && Recovery::Parse(lf).summary==report.summary,"line endings and a BOM change nothing");
    const auto clean=Recovery::Parse("Normal source map: X.sdc\n\nBrushes: 4 cells from the compiled BSP -> 2 structural brushes (50% fewer).\r\nBrush faces: 24 -> 12 (50% fewer); 12 polygons written after restoring materials.\n\nNothing to report: every item came back cleanly.\n");
    Check(clean.clean && clean.rows.empty() && clean.declared==-1 && Contains(clean.summary,"Brush faces"),"nothing to report");
    // Every category the editor writes is explained (MapRecovery.cpp's outcome.rows).
    const auto source=ReadText("Reloaded.Editor/MapRecovery.cpp");
    static const std::regex pushed(R"re(outcome\.rows\.push_back\(\{"([^"]+)")re");
    size_t seen=0;
    for(std::sregex_iterator it(source.begin(),source.end(),pushed),end;it!=end;++it,++seen)
        Check(Recovery::FindCategory((*it)[1].str())!=nullptr,"the report category \""+(*it)[1].str()+"\" has a meaning and an action");
    Check(seen>=6,"MapRecovery.cpp's rows were found");
    for(const auto& category:Recovery::Categories())Check(Contains(source,"{\""+std::string(category.name)+"\""),std::string(category.name)+" is a category the editor writes");
    // The tally: the report's order, then unknown ones; with what to do.
    const auto tally=Recovery::Tally(Json{{"Zebra",1},{"Thin brush",3},{"Actor left out",2}});
    Check(tally.size()==3 && tally[0].at("category")=="Actor left out" && tally[1].at("category")=="Thin brush" && tally[2].at("category")=="Zebra","report order, unknown last");
    Check(tally[1].contains("meaning") && Contains(tally[1].at("action").get<std::string>(),"view_screenshot") && !tally[2].contains("meaning"),"meaning and action for known ones");
    Check(Recovery::TallyLine(tally)=="2 Actor left out, 3 Thin brush, 1 Zebra" && Recovery::Counts(report.rows).at("Actor left out")==2,"tally line and counts");
    // The names the menu's recovery gives (UniqueRecoveryPath).
    std::set<std::string> files;
    auto exists=[&](const std::string& relative){return files.count(relative)!=0;};
    Check(Recovery::DefaultName("OffsD",exists)=="OffsD_Recovered","the first name");
    files.insert("MapsEd\\OffsD_Recovered.sdc");
    Check(Recovery::DefaultName("OffsD",exists)=="OffsD_Recovered_2","a taken map");
    files.insert("StaticMeshes\\OffsD_Recovered_2_Assets.usx");
    Check(Recovery::DefaultName("OffsD",exists)=="OffsD_Recovered_3","a taken asset package");
    files.insert("Maps\\OffsD_Recovered_3.sdc");files.insert("MapsEd\\Recovery\\OffsD_Recovered_4");
    Check(Recovery::DefaultName("OffsD",exists)=="OffsD_Recovered_5","a playable copy or a scratch folder");
    Check(Recovery::AssetPackage("9Lives")=="Recovery_9Lives_Assets" && Recovery::AssetPackage("Offs-D")=="Offs_D_Assets","asset package names as the editor makes them");
    Check(Recovery::Stem("C:\\Game\\Packages\\Maps\\OffsD.SDC")=="OffsD" && Recovery::Stem("Packages/Maps/OffsD.sdc")=="OffsD" && Recovery::Stem("OffsD")=="OffsD","stems");
}

// ----------------------------------------------------------- map_recover
void DryRun()
{
    Harness h;
    h.Touch("MapsEd\\OffsD_Recovered.sdc");
    h.fake.On("map.recover",[](const Json& a,const CallOptions& o){Check(o.dryRun,"only the preflight runs");return Preflight(a);});
    auto dry=h.Call("map_recover",{{"source","OffsD"},{"dryRun",true}});
    Check(dry.at("isError")==false && h.fake.Count("map.recover")==1,"one dry run: "+Text(dry));
    const auto* call=h.fake.Last("map.recover");
    Check(call->dryRun && call->args.at("destination")=="OffsD_Recovered_2.sdc","the menu's default name, past the one Packages\\MapsEd has: "+call->args.dump());
    Check(call->args.at("source")=="OffsD" && call->args.at("discard")==false && call->args.at("keepXboxOnly")==false && call->args.at("maxRows")==200 && !call->args.contains("merge"),
          "the contract's arguments; merge left to the editor's setting");
    Check(Contains(Text(dry),"Dry run") && Contains(Text(dry),"42 imported packages") && Contains(Text(dry),"Nothing was changed"),"what would happen");
    Check(Contains(Text(dry),"MCP_TOOL_TIMEOUT"),"the client's timeout is mentioned before a long call");
    const auto& next=Structured(dry).at("next");
    Check(next[0].at("tool")=="map_recover" && next[0].at("args").at("name")=="OffsD_Recovered_2" && !next[0].at("args").contains("dryRun"),"next runs it for real");
    Check(Structured(dry).at("preflight").at("checks").size()==3 && Structured(dry).at("undo").is_null(),"the editor's checks; nothing to undo");
    // Unsaved changes: the dry run still checks everything else, and says what the run needs.
    h.fake.Clear("editor.state");h.fake.On("editor.state",Harness::State(true,"C:\\Game\\Packages\\MapsEd\\WorkD.sdc"));
    auto dirty=h.Call("map_recover",{{"source","Packages\\Maps\\OffsD.sdc"},{"name","OffsR"},{"merge",false},{"keepXboxOnly",true},{"maxRows",5},{"dryRun",true}});
    call=h.fake.Last("map.recover");
    Check(dirty.at("isError")==false && call->args.at("discard")==true && call->args.at("destination")=="OffsR.sdc" && call->args.at("merge")==false && call->args.at("keepXboxOnly")==true &&
          call->args.at("maxRows")==5,"the preflight runs as if discarding: "+call->args.dump());
    Check(Contains(Text(dirty),"unsaved changes") && Contains(Text(dirty),"discard: true") && Contains(Text(dirty),"Xbox-only actors kept") && Contains(Text(dirty),"merging off"),
          "the warning and the options: "+Text(dirty));
    // Names the editor's save would refuse are refused here.
    for(const char* name:{"Offs R","Offs\"R","OffsR.txt"})
        Check(h.Call("map_recover",{{"source","OffsD"},{"name",name},{"dryRun",true}}).at("isError")==true,std::string("name refused: ")+name);
    Check(h.Call("map_recover",{{"source","OffsD.txt"},{"dryRun",true}}).at("isError")==true && h.fake.Count("map.recover")==2,"a source that is not a .sdc never reaches the editor");
}
void Success()
{
    Harness h;
    h.fake.On("map.recover",[](const Json& a,const CallOptions& o)
    {
        if(o.dryRun)return Preflight(a);
        Check(o.timeoutMs>=3u*3600u*1000u,"hours to finish");
        const char* stages[]={"Preparing","Loading the compiled map","Building geometry","Building geometry","Saving"};
        const int percents[]={0,4,51,62,93};
        for(int i=0;i<5;++i)o.progress({{"t","progress"},{"id",9},{"stage",stages[i]},{"stageIndex",i},{"percent",percents[i]}});
        o.progress({{"t","progress"},{"id",9},{"stage","busy"},{"reason","modal"},{"message","A dialog is open."}});
        return Recovered(a);
    });
    auto done=h.Call("map_recover",{{"source","OffsD"}},{{"progressToken","r1"}});
    Check(done.at("isError")==false,"recovered: "+Text(done));
    const auto ops=h.fake.Ops();
    Check(ops==std::vector<std::string>({"editor.state","map.recover","map.recover"}) && h.fake.calls[1].dryRun && !h.fake.calls[2].dryRun,"state, the preflight, then the recovery");
    Check(h.fake.Last("map.recover")->label=="map_recover OffsD" && h.fake.Last("map.recover")->args.at("destination")=="OffsD_Recovered.sdc","label and destination");
    const auto& s=Structured(done);
    Check(s.at("file")=="C:\\Game\\Packages\\MapsEd\\OffsD_Recovered.sdc" && s.at("created")[0]==s.at("file") && s.at("seconds")==312.4 && s.at("counts").at("brushes")==2104,"file, seconds and counts");
    Check(s.at("created").size()==3 && s.at("created")[1]=="C:\\Game\\Packages\\Maps\\OffsD_Recovered.sdc" && s.at("created")[2]=="C:\\Game\\Packages\\StaticMeshes\\OffsD_Recovered_Assets.usx",
          "the playable copy and the asset package the editor wrote: "+s.at("created").dump());
    Check(!Contains(s.at("recoverySummary").get<std::string>(),"\r") && Contains(s.at("recoverySummary").get<std::string>(),"\nBrush faces"),"the summary's CRLF is \\n");
    Check(s.at("rowsTotal")==10 && s.at("rows").size()==4 && s.at("categories")[0].at("category")=="Reference cleared" && s.at("categories")[0].contains("action"),"rows and the explained tally");
    Check(Contains(Text(done),"2104 structural brushes from 18422 BSP cells") && Contains(Text(done),"10 items: 1 Reference cleared, 1 Asset moved, 7 Thin brush"),
          "the summary line: "+s.at("summary").get<std::string>());
    Check(Contains(Text(done),"map_query what:\"recovery\""),"more rows than shown point at map_query");
    std::string warnings=s.at("warnings").dump();
    Check(Contains(warnings,"OffsD_Recovered_Assets.usx") && Contains(warnings,"Recalculate Selected Lighting") && Contains(warnings,"Recalculate Lighting re-bakes the whole map"),"asset package and lighting warnings");
    std::set<std::string> tools;Json frame;
    for(const auto& n:s.at("next")){tools.insert(n.at("tool").get<std::string>());if(n.at("tool")=="view_screenshot")frame=n.at("args").at("frame");}
    Check(tools.count("map_query") && tools.count("map_validate") && frame==Json::array({"RecoveredVolume12","SMover7"}),"next: the report, screenshots of the actor rows, map_validate");
    Check(s.at("undo").is_null() && s.at("limits").size()==3,"not undoable; what is not 1:1");
    // Progress: the editor's stages as percent of the whole, rising.
    const auto progress=h.Notes("notifications/progress");
    Check(progress.size()>=6,"every stage reported");
    bool geometry=false;
    for(size_t i=0;i<progress.size();++i)
    {
        const auto& p=progress[i].at("params");
        geometry=geometry || (p.value("message",std::string())=="Recovering OffsD: Building geometry (51%)" && p.at("progress")==51 && p.at("total")==100);
        if(i)Check(p.at("progress").get<double>()>progress[i-1].at("params").at("progress").get<double>(),"progress rises");
    }
    Check(geometry,"a stage reads with its percent: "+progress[3].dump());
    // A clean report.
    h.fake.Clear("map.recover");
    h.fake.On("map.recover",[](const Json& a,const CallOptions& o){if(o.dryRun)return Preflight(a);auto r=Recovered(a);r["rows"]=Json::array();r["rowsTotal"]=0;r["categories"]=Json::object();return r;});
    auto clean=h.Call("map_recover",{{"source","OffsD"},{"name","OffsC"}});
    Check(clean.at("isError")==false && Contains(Text(clean),"everything came back cleanly") && !Contains(Structured(clean).at("next").dump(),"\"recovery\""),"nothing to report");
}
// Every refusal reads as what to do, with the call that does it.
void Refusals()
{
    Harness h;
    auto refuse=[&](const std::string& code,const std::string& message,const std::string& reason,const Json& details=Json())
    {
        h.fake.Clear("map.recover");
        h.fake.Fail("map.recover",code,message,reason,details);
        return h.Call("map_recover",{{"source","OffsD"},{"name","OffsR"}});
    };
    auto missing=refuse(Bridge::Code::Refused,"OffsD.sdc imports packages this game does not have.","missing_packages",Json{{"missing",{"OffsD_TEX","Pipes_STM"}}});
    Check(missing.at("isError")==true && Structured(missing).at("reason")=="missing_packages" && Contains(Text(missing),"OffsD_TEX, Pipes_STM") && Contains(Text(missing),"SCCT-Maps") &&
          Contains(Text(missing),"install"),"missing packages listed, with where to get them: "+Text(missing));
    Check(Structured(missing).at("next")[0].at("args").at("dryRun")==true && h.fake.Count("map.recover")==1,"checked again with a dry run; the recovery never started");
    h.Touch("MapsEd\\OffsD_Recovered.sdc");
    auto taken=refuse(Bridge::Code::Refused,"Packages\\MapsEd\\OffsR.sdc already exists.","destination_exists");
    Check(Contains(Text(taken),"another name") && Contains(Text(taken),"OffsR_Assets.usx") && Structured(taken).at("next")[0].at("args").at("name")=="OffsD_Recovered_2","another name is suggested: "+Text(taken));
    auto unsaved=refuse(Bridge::Code::Refused,"The open map has unsaved changes.","unsaved_changes");
    Check(Contains(Text(unsaved),"MAP NEW") && Contains(Text(unsaved),"discard: true") && Structured(unsaved).at("next")[0].at("tool")=="map_file","unsaved changes");
    auto sourceMap=refuse(Bridge::Code::Refused,"OffsD.sdc is in Packages\\MapsEd.","source_map");
    Check(Contains(Text(sourceMap),"map_file action:\"open\"") && Structured(sourceMap).at("next")[0].at("args").at("path")=="OffsD","a source map is opened, not recovered");
    auto busy=refuse(Bridge::Code::Busy,"A map recovery is running.","rebuild");
    Check(Contains(Text(busy),"Rebuild All Maps") && Structured(busy).at("next")[0].at("tool")=="editor_status" && Structured(busy).at("retryable")==true,"a running recovery");
    // Failures of the recovery itself, after the preflight passed.
    auto failing=[&](const std::string& code,const std::string& message,const Json& extra=Json::object())
    {
        h.fake.Clear("map.recover");
        h.fake.On("map.recover",[code,message](const Json& a,const CallOptions& o)->Json{if(o.dryRun)return Preflight(a);throw Bridge::Error(code,message);});
        Json args{{"source","OffsD"},{"name","OffsR"}};
        for(auto it=extra.begin();it!=extra.end();++it)args[it.key()]=it.value();
        return h.Call("map_recover",args);
    };
    auto bsp=failing(Bridge::Code::EditorError,"The normal BSP rebuild changed 3 of 4000 solid/empty space samples. The reconstructed source T3D has been kept for inspection.");
    Check(Contains(Text(bsp),"The recovery failed") && Contains(Text(bsp),"merge: false") && Structured(bsp).at("next")[1].at("args").at("merge")==false,"merging off is offered: "+Text(bsp));
    auto unmerged=failing(Bridge::Code::EditorError,"The normal BSP rebuild changed 3 of 4000 solid/empty space samples.",{{"merge",false}});
    Check(!Contains(Text(unmerged),"try again with merge: false") && Structured(unmerged).at("next").size()==1,"not when merging was already off");
    auto cancelled=failing(Bridge::Code::Cancelled,"map.recover was cancelled; it may still have run.");
    Check(Structured(cancelled).at("code")=="cancelled" && Contains(Text(cancelled),"new, empty map") && Contains(Text(cancelled),"were removed"),"a cancelled recovery says what is left: "+Text(cancelled));
    auto slow=failing(Bridge::Code::Timeout,"map.recover did not finish within 10800 s.");
    Check(Contains(Text(slow),"MapRecovery") && Structured(slow).at("next")[0].at("tool")=="editor_log","a timeout points at the log");
    // A failed recovery that left its files: those names stay taken, so the retry gets another.
    h.fake.Clear("map.recover");
    h.fake.On("map.recover",[](const Json& a,const CallOptions& o)->Json
    {
        if(o.dryRun)return Preflight(a);
        throw Bridge::Error(Bridge::Code::EditorError,"The normal BSP rebuild changed 3 of 4000 solid/empty space samples.","recovery_failed",
                            {{"stage","Verifying"},{"left",{"C:\\Game\\Packages\\MapsEd\\OffsR.sdc","C:\\Game\\Packages\\MapsEd\\Recovery\\OffsR"}}});
    });
    auto leftover=h.Call("map_recover",{{"source","OffsD"},{"name","OffsR"}});
    Check(Contains(Text(leftover),"it left C:\\Game\\Packages\\MapsEd\\OffsR.sdc") && Contains(Text(leftover),"under another name") &&
          Structured(leftover).at("reason")=="recovery_failed","the files a failure left are named: "+Text(leftover));
    for(const auto& step:Structured(leftover).at("next"))
        if(step.at("tool")=="map_recover")Check(step.at("args").at("name")!="OffsR","a retry never reuses the taken name: "+step.dump());
    // The server's own checks: unsaved changes before the recovery, and a
    // preflight that lists missing packages without refusing.
    h.fake.Clear("map.recover");
    h.fake.On("map.recover",[](const Json& a,const CallOptions& o){Check(o.dryRun,"never past the preflight");return Preflight(a);});
    h.fake.Clear("editor.state");h.fake.On("editor.state",Harness::State(true));
    const auto before=h.fake.Count("map.recover");
    auto dirty=h.Call("map_recover",{{"source","OffsD"}});
    Check(dirty.at("isError")==true && Structured(dirty).at("reason")=="unsaved_changes" && h.fake.Count("map.recover")==before+1,"unsaved changes stop it after the preflight");
    h.fake.Clear("editor.state");h.fake.On("editor.state",Harness::State());
    h.fake.Clear("map.recover");
    h.fake.On("map.recover",[](const Json& a,const CallOptions& o){Check(o.dryRun,"never past the preflight");auto p=Preflight(a);p["missing"]=Json::array({"Lost_TEX"});return p;});
    Check(Contains(Text(h.Call("map_recover",{{"source","OffsD"}})),"Lost_TEX"),"missing packages in a preflight result are a refusal too");
    h.fake.connected=false;
    Check(Structured(h.Call("map_recover",{{"source","OffsD"}})).at("code")=="not_connected","no editor");
}
// Cancelling the request asks the bridge to stop the recovery in flight.
void Cancel()
{
    Harness h;
    std::atomic<bool> running{false};
    h.fake.On("map.recover",[&](const Json& a,const CallOptions& o)->Json
    {
        if(o.dryRun)return Preflight(a);
        running=true;
        for(int percent=0;!h.stop;percent=(percent+1)%90)
        {
            o.progress({{"t","progress"},{"id",3},{"stage","Merging fragmented geometry"},{"stageIndex",4},{"percent",percent}});
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw Bridge::Error(Bridge::Code::Cancelled,"map.recover was cancelled; it may still have run.");
    });
    h.Send({{"jsonrpc","2.0"},{"id","rec"},{"method","tools/call"},{"params",{{"name","map_recover"},{"arguments",{{"source","OffsD"}}},{"_meta",{{"progressToken","c"}}}}}});
    const auto started=std::chrono::steady_clock::now();
    while(!running && std::chrono::steady_clock::now()-started<std::chrono::seconds(10))std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Check(running,"the recovery started");
    h.Send({{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",{{"requestId","rec"},{"reason","too slow"}}}});
    h.server->Wait();
    Check(h.cancels==1 && h.Find("rec").is_null(),"the bridge was asked to cancel, and the cancelled request has no reply");
    Check(!h.Notes("notifications/progress").empty(),"progress came while it ran");
}

// --------------------------------------------- map_file open, map_query
void OpenCompiled()
{
    Harness h;
    auto local=h.Call("map_file",{{"action","open"},{"path","Packages\\Maps\\OffsD.sdc"}});
    Check(local.at("isError")==true && Structured(local).at("reason")=="compiled_map" && h.fake.Count("map.open")==0,"a compiled map is not opened: "+Text(local));
    const auto& next=Structured(local).at("next")[0];
    Check(Contains(Text(local),"map_recover") && next.at("tool")=="map_recover" && next.at("args").at("source")=="Packages\\Maps\\OffsD.sdc" && next.at("args").at("name")=="OffsD_Recovered",
          "the tool and the arguments to use instead");
    const auto inGame=PathText(h.dir/"Packages"/"Maps"/"OffsD.sdc");
    Check(Structured(h.Call("map_file",{{"action","open"},{"path",inGame}})).at("reason")=="compiled_map" && h.fake.Count("map.open")==0,"this game's Packages\\Maps in full: "+inGame);
    h.fake.Fail("map.open",Bridge::Code::Refused,"D:\\Other\\Packages\\Maps\\OffsD.sdc is a compiled map.","compiled_map");
    auto bridge=h.Call("map_file",{{"action","open"},{"path","D:\\Other\\Packages\\Maps\\OffsD.sdc"},{"dryRun",true}});
    Check(bridge.at("isError")==true && Contains(Text(bridge),"is a compiled map.") && Contains(Text(bridge),"map_recover (source \"D:\\Other\\Packages\\Maps\\OffsD.sdc\""),
          "the bridge's refusal names map_recover too: "+Text(bridge));
    // A stock map's editable source in Packages\MapsEd comes first: the original file.
    h.Touch("MapsEd\\OffsD.sdc");
    auto twin=h.Call("map_file",{{"action","open"},{"path","Packages\\Maps\\OffsD.sdc"}});
    const auto& twinNext=Structured(twin).at("next");
    Check(twin.at("isError")==true && Contains(Text(twin),"original editable source") && twinNext.size()==2 && twinNext[0].at("tool")=="map_file" &&
          twinNext[0].at("args").at("path")==PathText(h.dir/"Packages"/"MapsEd"/"OffsD.sdc") && twinNext[1].at("tool")=="map_recover","the MapsEd source is offered first: "+Text(twin));
    h.fake.Clear("map.open");
    h.fake.On("map.open",Harness::State(false,"C:\\Game\\Packages\\MapsEd\\OffsD.sdc"));
    const auto opens=h.fake.Count("map.open");
    Check(h.Call("map_file",{{"action","open"},{"path","OffsD"}}).at("isError")==false && h.fake.Count("map.open")==opens+1,"a source map opens as before");
}
void QueryRecovery()
{
    Harness h;
    const auto fixture=fs::absolute(kFixture);
    h.fake.connected=false;
    auto offline=h.Call("map_query",{{"what","recovery"},{"file",PathText(fixture)}});
    const auto& s=Structured(offline);
    Check(offline.at("isError")==false && s.at("rowsAll")==9 && s.at("rowsTotal")==9 && s.at("rows").size()==9 && s.at("declared")==9,"read without the editor: "+Text(offline));
    Check(s.at("categories").size()==6 && s.at("categories")[0].at("category")=="Actor left out" && s.at("categories")[0].at("count")==2 && s.at("categories")[0].contains("meaning"),"the tally");
    Check(s.at("rows")[4].at("actor")=="RecoveredVolume12" && s.at("facts").at("Retained actors")==1311 && Contains(s.at("assetPackage").get<std::string>(),"_Assets.usx"),"rows, facts, asset package");
    Check(Contains(Text(offline),"9 items: 2 Actor left out") && Contains(Text(offline),"To do:") && Contains(Text(offline),"[Thin brush] RecoveredVolume12"),"the text: "+Text(offline));
    auto thin=h.Call("map_query",{{"what","recovery"},{"file",PathText(fixture)},{"category","thin BRUSH"},{"limit",1},{"offset",1}});
    Check(Structured(thin).at("rowsTotal")==2 && Structured(thin).at("rows").size()==1 && Structured(thin).at("rows")[0].at("item")=="RecoveredVolume907" && !Structured(thin).contains("nextOffset"),
          "by category, any case, paged");
    auto first=h.Call("map_query",{{"what","recovery"},{"file",PathText(fixture)},{"category",{"Thin brush","Asset moved"}},{"limit",1}});
    Check(Structured(first).at("rowsTotal")==3 && Structured(first).at("nextOffset")==1 && Contains(Text(first),"pass offset 1"),"several categories; the next page");
    Check(Contains(Text(h.Call("map_query",{{"what","recovery"},{"file",PathText(fixture)},{"category","Thin brushes"}})),"is not a report category"),"an unknown category is named");
    // By the recovered map's name, in Packages\MapsEd\Recovery.
    fs::create_directories(h.dir/"Packages"/"MapsEd"/"Recovery"/"OffsD_Recovered");
    fs::copy_file(fixture,h.dir/"Packages"/"MapsEd"/"Recovery"/"OffsD_Recovered"/"Recovery.txt");
    for(const char* file:{"OffsD_Recovered","OffsD_Recovered.sdc","Packages\\MapsEd\\Recovery\\OffsD_Recovered\\Recovery.txt"})
        Check(Structured(h.Call("map_query",{{"what","recovery"},{"file",file}})).value("rowsAll",0)==9,std::string("found by ")+file);
    Check(h.Call("map_query",{{"what","recovery"},{"file","NoSuchD"}}).at("isError")==true,"a map without a report");
    // The open map's own report.
    h.fake.connected=true;
    h.fake.Clear("editor.state");h.fake.On("editor.state",Harness::State(false,PathText(h.dir/"Packages"/"MapsEd"/"OffsD_Recovered.sdc")));
    auto live=h.Call("map_query",{{"what","recovery"}});
    Check(live.at("isError")==false && Structured(live).at("map")=="OffsD_Recovered" && Structured(live).at("rowsAll")==9,"the open map's report");
    h.fake.Clear("editor.state");h.fake.On("editor.state",Harness::State());
    auto unsaved=h.Call("map_query",{{"what","recovery"}});
    Check(unsaved.at("isError")==true && Contains(Text(unsaved),"Pass file"),"an unsaved map has none");
    h.fake.Clear("editor.state");h.fake.On("editor.state",Harness::State(false,"C:\\Game\\Packages\\MapsEd\\WorkD.sdc"));
    Check(Contains(Text(h.Call("map_query",{{"what","recovery"}})),"was not made by map_recover"),"a map that was never recovered");
}
// The tool list keeps its budget with map_recover in it.
void Listed()
{
    Harness h;
    const auto tools=h.Request("tools/list").at("result").at("tools");
    bool found=false;
    for(const auto& t:tools)
    {
        if(t.at("name")!="map_recover")continue;
        found=true;
        Check(t.at("annotations").at("destructiveHint")==true && t.at("annotations").at("readOnlyHint")==false && t.contains("outputSchema"),"annotations and output schema");
        Check(t.at("inputSchema").at("required")==Json::array({"source"}) && t.at("inputSchema").at("properties").contains("dryRun") && t.dump().size()<3000,
              "source required, dryRun and label, and small: "+std::to_string(t.dump().size()));
    }
    Check(found,"map_recover is listed");
}

int main()
{
    try{Model();DryRun();Success();Refusals();Cancel();OpenCompiled();QueryRecovery();Listed();}
    catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    std::cout<<"McpRecoverTools passed\n";
    return 0;
}
