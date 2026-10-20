// map.recover's pure part (Reloaded.Editor/McpRecoverModel.h): the files a
// request names, the compiled-map test map.open refuses by, what a recovery
// writes and must not find, missing packages, the request's checks, progress
// throttling and the replies.
#include "../Reloaded.Editor/McpRecoverModel.h"
#include <iostream>
#include <source_location>
using namespace Mcp;
namespace R=Mcp::Recover;
using Json=Workflow::Json;
int checks=0;
void Check(bool value,const char* message,std::source_location where=std::source_location::current())
{
    ++checks;
    if(!value)throw std::runtime_error(std::string(message)+" (line "+std::to_string(where.line())+")");
}
// The bridge error a call throws: its code, or "" when it returns.
template<class F> std::string Code(F f)
{
    try{f();}
    catch(const Bridge::Error& e){return e.code;}
    return {};
}
const std::string kRoot="C:\\Games\\SCCT Versus";

void Paths()
{
    Check(R::Normal("C:/Games//SCCT Versus/./Packages/../Packages/Maps/")=="C:\\Games\\SCCT Versus\\Packages\\Maps","slashes, dots and empty parts fold");
    Check(R::Normal("C:\\..\\..\\Maps")=="C:\\Maps",".. never climbs above the drive");
    Check(R::Normal("\\\\server\\share\\..\\..\\x")=="\\\\server\\x",".. never climbs above a share's server");
    Check(R::Normal("a\\..\\..\\b")=="..\\b","a relative path keeps the .. it cannot take back");
    Check(R::Normal("C:")=="C:\\","a bare drive is its root");
    Check(R::Absolute("C:\\x") && R::Absolute("\\\\server\\x") && R::Absolute("/x") && !R::Absolute("Packages\\Maps") && !R::Absolute("C:x"),"absolute paths");
    Check(R::Leaf("C:\\a\\OffsD.sdc")=="OffsD.sdc" && R::Stem("C:\\a\\OffsD.sdc")=="OffsD" && R::Extension("C:\\a\\OffsD.SDC")==".SDC","leaf, stem and extension");
    Check(R::Extension("C:\\a\\.hidden").empty() && R::Extension("C:\\a.b\\name").empty(),"a dot that starts a name or names a folder is no extension");
    Check(R::Parent("C:\\a\\b.sdc")=="C:\\a","parent");
    Check(R::SamePath("c:/games/scct versus/PACKAGES/maps","C:\\Games\\SCCT Versus\\Packages\\Maps\\"),"paths compare without case or slash style");
}

void Names()
{
    const auto maps=kRoot+"\\Packages\\Maps\\",mapsEd=kRoot+"\\Packages\\MapsEd\\";
    Check(R::MapFile("OffsD",kRoot,"Maps","source")==maps+"OffsD.sdc","a bare stem is a .sdc in the folder");
    Check(R::MapFile("OffsD.sdc",kRoot,"Maps","source")==maps+"OffsD.sdc","a bare file name too");
    Check(R::MapFile("OffsR",kRoot,"MapsEd","destination")==mapsEd+"OffsR.sdc","a destination's folder is MapsEd");
    Check(R::MapFile("OffsD.utx",kRoot,"Maps","source")==maps+"OffsD.utx","a bare name keeps the extension it has (the caller refuses it)");
    Check(R::MapFile("Packages\\Maps\\OffsD.sdc",kRoot,"MapsEd","source")==maps+"OffsD.sdc","Packages\\... is relative to the game folder");
    Check(R::SamePath(R::MapFile("packages/maps/OffsD.sdc",kRoot,"MapsEd","source"),maps+"OffsD.sdc"),"without case and with forward slashes");
    Check(R::MapFile("D:\\Backups\\OffsD.sdc",kRoot,"Maps","source")=="D:\\Backups\\OffsD.sdc","a full path stays as it is");
    Check(R::MapFile("Packages\\..\\..\\Elsewhere\\X.sdc",kRoot,"Maps","source")=="C:\\Games\\Elsewhere\\X.sdc",".. is folded before the folder tests see the path");
    for(const char* bad:{"","Maps\\OffsD.sdc","C:OffsD.sdc","Off\"sD","Offs*D","..","Off\nsD"})
        Check(Code([&]{R::MapFile(bad,kRoot,"Maps","source");})==Bridge::Code::Invalid,"an empty, relative, drive-relative or unspellable name is invalid");
    Check(Code([&]{R::MapFile(std::string(401,'a'),kRoot,"Maps","source");})==Bridge::Code::Invalid,"a name over 400 characters is invalid");
    try{R::MapFile("Maps\\X.sdc",kRoot,"Maps","source");}
    catch(const Bridge::Error& e){Check(std::string(e.what()).find("source")!=std::string::npos && std::string(e.what()).find("Packages\\Maps\\OffsD.sdc")!=std::string::npos,"the error names the argument and an example");}

    // map.open refuses compiled maps: a .sdc directly in Packages\Maps.
    Check(R::CompiledMapPath(maps+"OffsD.sdc",kRoot),"Packages\\Maps\\OffsD.sdc is compiled");
    Check(R::CompiledMapPath("c:/games/scct versus/packages/MAPS/offsd.SDC",kRoot),"compared without case or slash style");
    Check(R::CompiledMapPath(kRoot+"\\Packages\\MapsEd\\..\\Maps\\OffsD.sdc",kRoot),"after folding ..");
    Check(!R::CompiledMapPath(mapsEd+"OffsD.sdc",kRoot) && !R::CompiledMapPath(maps+"Sub\\OffsD.sdc",kRoot) && !R::CompiledMapPath(maps+"OffsD.utx",kRoot),
          "MapsEd, a subfolder of Maps and other kinds of file are not");
    Check(!R::CompiledMapPath("D:\\Other\\Packages\\Maps\\OffsD.sdc",kRoot),"another game folder's Maps is not this game's");
    Check(R::SourceMapPath(mapsEd+"OffsD.sdc",kRoot) && !R::SourceMapPath(mapsEd+"Recovery\\OffsD\\CompiledInput.sdc",kRoot) && !R::SourceMapPath(maps+"OffsD.sdc",kRoot),
          "a source map is directly in MapsEd");
}

void Outputs()
{
    const auto out=R::OutputsFor(kRoot,"OffsR");
    Check(out.map==kRoot+"\\Packages\\MapsEd\\OffsR.sdc" && out.playable==kRoot+"\\Packages\\Maps\\OffsR.sdc","the map and its compiled copy");
    Check(out.folder==kRoot+"\\Packages\\MapsEd\\Recovery\\OffsR" && out.report==out.folder+"\\Recovery.txt","the recovery folder and its report");
    Check(out.assets==kRoot+"\\Packages\\StaticMeshes\\OffsR_Assets.usx","the asset package");
    Check(R::OutputsFor(kRoot,"OffsD_Recovered").assets==kRoot+"\\Packages\\StaticMeshes\\OffsD_Recovered_Assets.usx","the menu's default name's asset package");
    Check(R::OutputsFor(kRoot,"2Fort").assets.find("Recovery_2Fort_Assets.usx")!=std::string::npos,"a leading digit, as MapRecovery names it");
    const auto mustBeFree=R::MustBeFree(out);
    Check(mustBeFree.size()==4 && mustBeFree[0]==out.map && mustBeFree[1]==out.playable && mustBeFree[2]==out.folder && mustBeFree[3]==out.assets,"every file a recovery writes must be free");
    Check(R::MapName("OffsR") && R::MapName("A_1") && !R::MapName("Offs R") && !R::MapName("") && !R::MapName(std::string(61,'a')),"map names");
    std::set<std::string> taken{"OffsD_Recovered","OffsD_Recovered_2"};
    auto isTaken=[&](const std::string& name){return taken.count(name)!=0;};
    Check(R::FreeName("OffsD",isTaken)=="OffsD_Recovered_3","the first free name, as the menu picks it");
    Check(R::FreeName("Offs D",isTaken)=="Offs_D_Recovered","a stem the editor cannot name a map after is made one");
    Check(R::MapName(R::FreeName(std::string(80,'x'),isTaken)),"a long stem is cut to fit");
    Check(R::FreeName("X",[](const std::string&){return true;}).empty(),"none free: none");
}

void Packages()
{
    const std::set<std::string> installed{"offsd_tex","stockmeshes","echelon"};
    auto runtime=[](const std::string& name){const auto key=R::Lower(name);return key=="core" || key=="engine";};
    const std::vector<std::string> imports{"Core","Engine","OffsD_Tex","StockMeshes","Echelon","OffsD","Missing_TEX","missing_tex","Other"};
    Check((R::Missing(imports,installed,"OffsD",runtime)==std::vector<std::string>{"Missing_TEX","Other"}),"the game's own, the map itself and installed files are not missing; each missing one once");
    Check(R::Missing(imports,{"offsd_tex","stockmeshes","echelon","missing_tex","other"},"offsd",runtime).empty(),"all there: none missing");
    Check((R::Missing({"Core"},{},"X",nullptr)==std::vector<std::string>{"Core"}),"without a runtime list, Core is just another package");
    for(const char* kind:{".u",".utx",".UTC",".usx",".uax",".ukx",".sdc"})Check(R::PackageFile(kind),"package kinds");
    for(const char* kind:{".int",".ini",".txt",""})Check(!R::PackageFile(kind),"other files");
}

void Options()
{
    const Json minimal{{"source","OffsD"},{"destination","OffsR"}};
    auto o=R::ReadOptions(minimal,true);
    Check(o.source=="OffsD" && o.destination=="OffsR" && o.merge && !o.keepXboxOnly && !o.discard && o.maxRows==200,"defaults");
    Check(!R::ReadOptions(minimal,false).merge,"merge defaults to File > Merge Recovered Geometry");
    auto full=minimal;full["merge"]=false;full["keepXboxOnly"]=true;full["discard"]=true;full["maxRows"]=0;full["op"]="map.recover";full["dryRun"]=true;
    o=R::ReadOptions(full,true);
    Check(!o.merge && o.keepXboxOnly && o.discard && o.maxRows==0,"every option");
    full["maxRows"]=2000;Check(R::ReadOptions(full,true).maxRows==2000,"2000 rows at most");
    auto nulls=minimal;nulls["merge"]=nullptr;nulls["maxRows"]=nullptr;
    Check(R::ReadOptions(nulls,true).merge && R::ReadOptions(nulls,true).maxRows==200,"null is the default");
    std::vector<Json> bad{Json::array(),Json{{"source","OffsD"}},Json{{"destination","OffsR"}},Json{{"source",1},{"destination","OffsR"}}};
    for(auto [key,value]:std::vector<std::pair<const char*,Json>>{{"merge","yes"},{"keepXboxOnly",1},{"discard","true"},{"maxRows",-1},{"maxRows",2001},{"maxRows",1.5},{"maxRows","10"}})
    {auto request=minimal;request[key]=value;bad.push_back(request);}
    for(const auto& request:bad)Check(Code([&]{R::ReadOptions(request,true);})==Bridge::Code::Invalid,"a request of the wrong shape is invalid");
}

void Progress()
{
    using MapRecoveryModel::Stage;
    R::Throttle throttle;
    const auto first=throttle.Next(Stage::Preparing,0);
    Check(first.at("stage")=="Preparing" && first.at("stageIndex")==0 && first.at("percent")==0,"the first checkpoint is sent");
    Check(throttle.Next(Stage::Preparing,0).is_null() && throttle.Next(Stage::Preparing,0.1).is_null(),"nothing new: nothing sent");
    const auto loading=throttle.Next(Stage::Loading,0);
    Check(!loading.is_null() && loading.at("stage")==MapRecoveryModel::Info(Stage::Loading).name && loading.at("stageIndex")==1,"a new stage is sent");
    Check(throttle.Next(Stage::BuildingGeometry,0.5).at("percent")==MapRecoveryModel::Percent(Stage::BuildingGeometry,0.5),"percent is the whole recovery's");
    int sent=0;R::Throttle all;
    for(int stage=0;stage<static_cast<int>(Stage::Count);++stage)
        for(int step=0;step<=1000;++step)if(!all.Next(static_cast<Stage>(stage),step/1000.0).is_null())++sent;
    Check(sent>=100 && sent<=114,"about one message per percent and stage, however often the recovery checks in");
}

void Replies()
{
    MapRecoveryModel::Counts counts;
    counts.cells=1803;counts.brushes=837;counts.cellFaces=12000;counts.brushFaces=9000;counts.polygons=8797;counts.actors=1200;counts.grown=40;
    std::vector<MapRecoveryModel::ReportRow> rows{
        {"Actor left out","Xbox7","Xbox-only.",MapRecoveryModel::Target::None,"",-1},
        {"Thin brush","Brush12","0.5 units",MapRecoveryModel::Target::Actor,"Brush12",-1},
        {"BSP lighting not preserved","Surface 4","texels",MapRecoveryModel::Target::Surface,"",4},
        {"Thin brush","Brush13","0.7 units",MapRecoveryModel::Target::Actor,"Brush13",-1}};
    R::Finished done;done.file="C:\\G\\Packages\\MapsEd\\OffsR.sdc";done.source="C:\\G\\Packages\\Maps\\OffsD.sdc";done.report="C:\\G\\Packages\\MapsEd\\Recovery\\OffsR\\Recovery.txt";
    done.folder="C:\\G\\Packages\\MapsEd\\Recovery\\OffsR";done.merge=true;done.keepXboxOnly=false;done.seconds=312.46;
    const auto result=R::Result(done,counts,rows,2);
    Check(result.at("ok")==true && result.at("file")==done.file && result.at("source")==done.source && result.at("report")==done.report && result.at("folder")==done.folder,"files");
    Check(result.at("merge")==true && result.at("keepXboxOnly")==false && result.at("seconds")==312.5,"options and seconds to a tenth");
    const auto summary=result.at("summary").get<std::string>();
    Check(summary.find("\r")==std::string::npos && summary.find("\n")!=std::string::npos && summary.find("1,803 cells")!=std::string::npos,"the summary, in plain line breaks");
    const auto& c=result.at("counts");
    for(const char* key:{"merged","cells","cellFaces","joinedBrushes","brushes","brushFaces","grown","absorbed","splitBack","sheetBrushes","polygons","unifiedSurfaces","actors"})
        Check(c.contains(key),"every count");
    Check(c.at("cells")==1803 && c.at("brushes")==837 && c.at("actors")==1200 && c.at("merged")==true,"count values");
    Check(result.at("categories")==Json{{"Actor left out",1},{"BSP lighting not preserved",1},{"Thin brush",2}},"every row counted by category");
    Check(result.at("rows").size()==2 && result.at("rowsTotal")==4,"maxRows rows inline, all of them counted");
    Check(result.at("rows")[0]==Json{{"category","Actor left out"},{"item","Xbox7"},{"detail","Xbox-only."}},"a row without a target");
    Check(result.at("rows")[1].at("actor")=="Brush12" && !result.at("rows")[1].contains("surface"),"an actor row names it");
    Check(R::Result(done,counts,rows,10).at("rows")[2].at("surface")==4,"a surface row names it");
    Check(R::Result(done,counts,rows,0).at("rows").empty() && R::Result(done,counts,{},5).at("categories").empty(),"no rows");
    Check(R::RowJson({"X","Y","Z",MapRecoveryModel::Target::Actor,"",-1}).size()==3 && R::RowJson({"X","Y","Z",MapRecoveryModel::Target::Surface,"",-1}).size()==3,"a target without its name is left out");
    Check(R::Lines("a\r\nb\r\n\r\nc\n")=="a\nb\n\nc\n","CRLF to LF");

    R::Checked checked;checked.source=done.source;checked.destination=done.file;checked.merge=false;checked.keepXboxOnly=true;checked.imports=12;checked.sourceBytes=123456789;
    checked.checks={"source is a compiled map in Packages\\Maps"};
    const auto dry=R::DryRun(checked);
    Check(dry.at("dryRun")==true && dry.at("source")==done.source && dry.at("destination")==done.file && dry.at("merge")==false && dry.at("keepXboxOnly")==true,"the dry run says what would run");
    Check(dry.at("imports")==12 && dry.at("missing")==Json::array() && dry.at("sourceBytes")==123456789 && dry.at("checks").size()==1,"and what it checked");
    Check(dry.at("note").get<std::string>().find("OffsD.sdc into OffsR.sdc")!=std::string::npos,"its note, for the approval window's row");
}

void Policies()
{
    Check(Bridge::FindPolicy("map.recover") && Bridge::FindPolicy("map.recover")->policy==Bridge::Policy::File && !Bridge::FindPolicy("map.recover")->joinable,"map.recover writes files and joins no Undo step");
    Check(Bridge::Cancellable("map.recover") && !Bridge::Cancellable("map.open") && !Bridge::Cancellable("build.all") && !Bridge::Cancellable("mcp.plan"),"only map.recover stops on a cancel");
    Check(Bridge::PreviewsItself("map.recover") && !Bridge::PreviewsItself("map.save"),"only map.recover's dry run is its own");
}

int main()
{
    try{Paths();Names();Outputs();Packages();Options();Progress();Replies();Policies();}
    catch(const std::exception& e){std::cerr<<"McpRecoverModelTests FAILED: "<<e.what()<<"\n";return 1;}
    std::cout<<"McpRecoverModelTests passed ("<<checks<<" checks)\n";
    return 0;
}
