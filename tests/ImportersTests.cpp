// Every importer through Mcp::Import::Import, on the fixtures in
// tests/mcp/fixtures. Each result must be a valid level spec whose polygon
// brushes pass the editor's own polygon check (Authoring::BrushPolygons) and
// wind outward; when the spec compiler is in the tree, it must compile with
// no errors (its own wiki checks, RE_SPEC_NO_RULES). Links WorkflowModel.cpp
// and RecoveredPolygonImport.cpp. "--render <dir>" also writes each plan as
// a PNG for a person to look at.
#define RE_SPEC_NO_RULES 1
#include "../Reloaded.Editor.Mcp/Importers/Import.h"
#include "../Reloaded.Editor.Mcp/PlanRender.h"
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "../Reloaded.Editor/MapAuthoringModel.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#if __has_include("../Reloaded.Editor.Mcp/LevelSpecCompiler.h")
#include "../Reloaded.Editor.Mcp/LevelSpecCompiler.h"
#define RE_TEST_COMPILER 1
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace Mcp;
using Json=Workflow::Json;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void Reject(F f,const std::string& expected={},std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception& e){caught=true;if(!expected.empty() && std::string(e.what()).find(expected)==std::string::npos)throw std::runtime_error("line "+std::to_string(where.line())+": wrong error: "+e.what());}
    if(!caught)throw std::runtime_error("invalid input accepted at line "+std::to_string(where.line()));
}
std::filesystem::path Dir()
{
    for(const auto& dir:{std::filesystem::path("tests/mcp/fixtures"),std::filesystem::path("mcp/fixtures"),std::filesystem::path(__FILE__).parent_path()/"mcp/fixtures"})
        if(std::filesystem::exists(dir/"plan.svg"))return dir;
    throw std::runtime_error("missing fixtures (tests/mcp/fixtures)");
}
std::string Read(const std::string& name){std::ifstream in(Dir()/name,std::ios::binary);if(!in)throw std::runtime_error("missing fixture "+name);std::ostringstream s;s<<in.rdbuf();return s.str();}
std::string renderDir;
bool Warned(const Json& report,const std::string& part){for(const auto& w:report.at("warnings"))if(w.get<std::string>().find(part)!=std::string::npos)return true;return false;}
bool Assumed(const Json& report,const std::string& part){for(const auto& w:report.at("assumptions"))if(w.get<std::string>().find(part)!=std::string::npos)return true;return false;}
const Json* ById(const Json& list,const std::string& id){for(const auto& item:list)if(item.value("id",std::string())==id)return &item;return nullptr;}
const Json& At(const Json& spec,std::initializer_list<const char*> path)
{
    const Json* at=&spec;
    for(const char* key:path){if(!at->contains(key))throw std::runtime_error(std::string("spec has no ")+key);at=&at->at(key);}
    return *at;
}
// A valid spec: the schema, the editor's polygon rules, outward faces, the
// compiler, and a plan that renders.
void Valid(const Json& spec,const std::string& what)
{
    Spec::Parse(spec);
    Json pieces=Json::array();
    if(spec.contains("geometry") && spec.at("geometry").contains("brushes"))
        for(const auto& brush:spec.at("geometry").at("brushes"))
        {
            try{Workflow::Authoring::BrushPolygons(brush.at("polygons"));}
            catch(const std::exception& e){throw std::runtime_error(what+": brush "+brush.at("id").get<std::string>()+": "+e.what());}
            double c[3]={0,0,0};size_t n=0;
            for(const auto& p:brush.at("polygons"))for(const auto& v:p.at("vertices")){for(int k=0;k<3;++k)c[k]+=v[k].get<double>();++n;}
            for(double& x:c)x/=static_cast<double>(n);
            for(const auto& p:brush.at("polygons"))
            {
                const auto& v=p.at("vertices");
                double e1[3],e2[3],m[3]={0,0,0};
                for(int k=0;k<3;++k){e1[k]=v[1][k].get<double>()-v[0][k].get<double>();e2[k]=v[2][k].get<double>()-v[0][k].get<double>();}
                const double nrm[3]={e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]};
                for(const auto& q:v)for(int k=0;k<3;++k)m[k]+=q[k].get<double>()/static_cast<double>(v.size());
                if(nrm[0]*(m[0]-c[0])+nrm[1]*(m[1]-c[1])+nrm[2]*(m[2]-c[2])<=0)throw std::runtime_error(what+": a face of "+brush.at("id").get<std::string>()+" winds inward");
            }
            pieces.push_back(Render::BrushPiece(brush));
        }
    Json scene{{"pieces",pieces}};
#ifdef RE_TEST_COMPILER
    Spec::CompileOptions options;options.build=false;options.screenshot=false;
    const auto compiled=Spec::Compile(spec,options);
    for(const auto& d:compiled.diagnostics)
        if(d.severity=="error")throw std::runtime_error(what+" does not compile: "+d.pointer+" "+d.message);
    scene=compiled.render;
    for(const auto& p:pieces)scene["pieces"].push_back(p);
    scene.erase("bounds");
#endif
    const auto png=Render::PlanPng(scene,{{"title",what}});
    Check(Image::IsPng(png),"plan renders");
    if(!renderDir.empty())std::ofstream(std::filesystem::path(renderDir)/(what+".png"),std::ios::binary)<<png;
}
Json Run(const std::string& file,const Json& options,Json& report,const std::string& format="auto")
{
    report=Json::object();
    return Import::Import(format,file,Read(file),options,report);
}
int main(int argc,char** argv)
{
    try
    {
        if(argc>2 && std::string(argv[1])=="--render")renderDir=argv[2];
        // --- SVG ---------------------------------------------------------------
        {
            Json report;
            const auto spec=Run("plan.svg",Json::object(),report);
            Check(report.at("format")=="svg" && spec.at("map").at("name")=="PLAN","svg detected, named after the file");
            Valid(spec,"svg");
            Check(spec.at("storeys").size()==2 && spec.at("storeys")[1].at("z")==384,"layers are storeys");
            const auto& pieces=At(spec,{"geometry","pieces"});
            const auto* west=ById(pieces,"west_room");
            Check(west && west->at("kind")=="Room" && west->at("size")==Json::array({800,800,320}) && west->at("name")=="Spy base","rect room named by its text");
            Check(ById(pieces,"hall")->at("kind")=="Corridor" && ById(pieces,"hall")->at("yaw")==90,"corridor along X");
            Check(ById(pieces,"annex")->at("size")==Json::array({640,640,320}),"group transform scales");
            Check(ById(pieces,"loft")->at("storey")=="Storey1" && ById(pieces,"loft")->at("size")[2]==256,"upper layer and data-height");
            Check(ById(pieces,"Vent1")->at("kind")=="Vent","closed path is a vent");
            Check(pieces.size()==6,"hidden rect skipped");
            const auto& brushes=At(spec,{"geometry","brushes"});
            Check(brushes.size()==3 && brushes[0].at("id")=="server" && brushes[1].at("id")=="server_2" && brushes[2].at("csg")=="CSG_Add","L room in two carved pieces, pillar added");
            Check(At(spec,{"spawns","spy"}).size()==3 && At(spec,{"spawns","merc"}).size()==3,"starts from circles and use");
            Check(At(spec,{"spawns","merc"})[2].at("yaw")==90,"data-yaw");
            const auto& objective=At(spec,{"gameplay","story","objectives"})[0];
            Check(objective.at("name")=="Main server" && objective.at("triggers")[0].at("at").is_array() && objective.at("triggers")[0].at("at")[2]==78,"terminal in a polygon room at its usual height");
            const auto& devices=At(spec,{"security","devices"});
            Check(devices[0].at("kind")=="camera" && devices[0].at("facing")==225,"camera faces along its line");
            Check(devices[1].at("kind")=="laser" && devices[1].at("facing")==270 && devices[1].at("length")==160,"laser line");
            Check(spec.at("ges")[0].at("kind")=="ledge" && spec.at("ges")[0].at("points")[0][2]==592,"ledge on the upper storey");
            Check(At(spec,{"security","camnet"})[0].at("yaw")==180,"camnet yaw");
            const auto& light=At(spec,{"environment","lights"})[0];
            Check(light.at("radius")==160 && light.at("props").at("LightHue")=="40","light radius and props");
            Check(Assumed(report,"data-scale"),"scale reported");
        }
        {
            Json report;
            const auto bare=Import::Import("svg","bare.svg","<svg><rect width=\"100%\" height=\"100%\"/><rect x=\"0\" y=\"0\" width=\"50\" height=\"60\"/></svg>",Json::object(),report);
            Check(At(bare,{"geometry","pieces"}).size()==1 && At(bare,{"geometry","pieces"})[0].at("size")==Json::array({400,480,320}) && Warned(report,"percent"),"background rect skipped, default scale 8");
            Check(Assumed(report,"the default"),"default scale reported");
        }
        // --- Tiled ---------------------------------------------------------------
        {
            Json report;
            const auto spec=Run("plan.tmj",{{"name","Tiled"}},report);
            Check(report.at("format")=="tiled","tmj detected");
            Valid(spec,"tmj");
            const auto& pieces=At(spec,{"geometry","pieces"});
            Check(pieces.size()==2 && ById(pieces,"Room1")->at("size")==Json::array({768,512,320}),"tile room");
            Check(ById(pieces,"vault") && ById(pieces,"vault")->at("size")==Json::array({256,384,320}),"room object");
            Check(At(spec,{"spawns","spy"}).size()==3 && At(spec,{"spawns","merc"}).size()==3,"tile starts");
            Check(At(spec,{"gameplay","story","objectives"})[0].at("triggers")[0].at("at").at("in")=="Room1","object marker lands in its tile's room");
            Check(At(spec,{"security","camnet"})[0].at("yaw")==180,"object property yaw");
            Check(At(spec,{"security","devices"})[0].at("kind")=="laser","polyline laser");
            Check(At(spec,{"geometry","doorways"}).size()==1,"room object joined to the tile room");
            // A tile object means what its tile means (gid 3 is a spy tile).
            Json tiled=Json::parse(Read("plan.tmj"));
            tiled["layers"][1]["objects"].push_back({{"id",9},{"name",""},{"x",64},{"y",96},{"width",32},{"height",32},{"gid",3},{"visible",true}});
            const auto withTile=Import::Import("tiled","t.tmj",tiled.dump(),Json::object(),report);
            Check(At(withTile,{"spawns","spy"}).size()==4,"tile object kind from its tile");
            const auto xml=Run("plan.tmx",Json::object(),report);
            Check(report.at("format")=="tiled","tmx detected");
            Valid(xml,"tmx");
            Check(xml.at("storeys")[0].at("id")=="Storey1","group name is the storey");
            Check(At(xml,{"spawns","merc"}).size()==3,"flipped tiles keep their meaning (base64 + zlib layer)");
            Check(xml.at("ges").size()==1,"polyline ledge");
        }
        // --- GeoJSON -------------------------------------------------------------
        {
            Json report;
            const auto spec=Run("plan.geojson",Json::object(),report);
            Check(report.at("format")=="geojson","geojson detected");
            Valid(spec,"geojson");
            Check(Assumed(report,"longitude and latitude"),"degrees projected");
            const auto& hall=*ById(At(spec,{"geometry","pieces"}),"Hall");
            Check(std::abs(hall.at("size")[0].get<double>()-2930)<20 && std::abs(hall.at("size")[1].get<double>()-3316)<20 && hall.at("name")=="Main hall","projected rectangle");
            const auto& brushes=At(spec,{"geometry","brushes"});
            size_t carved=0,pillars=0;
            for(const auto& b:brushes){carved+=b.at("csg")=="CSG_Subtract";pillars+=b.at("csg")=="CSG_Add";}
            Check(carved==2 && pillars==1,"L-shaped wing in two pieces, its hole a pillar");
            Check(At(spec,{"spawns","merc"})[0].at("yaw")==180,"multipoint properties");
            Check(At(spec,{"gameplay","story","objectives"})[0].at("name")=="Ledger","named terminal");
            Check(Warned(report,"without a kind"),"kindless point reported");
            Reject([&]{Json r;Import::Import("geojson","x.geojson","{\"type\":\"FeatureCollection\",\"features\":[]}",Json::object(),r);},"no geometry");
        }
        // --- CSV actor table -----------------------------------------------------
        {
            Json report;
            const auto spec=Run("actors.csv",Json::object(),report);
            Check(report.at("format")=="csv","actor table detected");
            Valid(spec,"csv");
            const auto& ops=At(spec,{"extra","operations"});
            Check(ops.size()==6 && report.at("counts").at("skippedRows")==2,"rows created, bad rows skipped");
            Check(ops[0].at("id")=="Lamp1" && ops[0].at("properties").at("LightBrightness")=="96" && ops[0].at("properties").at("Location")==Json{{"X","0"},{"Y","0"},{"Z","256"}},"first row");
            Check(ops[1].at("id")=="Lamp" && ops[1].at("properties").at("Rotation").at("Yaw")=="16384","tag as id, yaw in units");
            Check(ops[3].at("id")=="PlayerStart1" && ops[3].at("properties").at("Rotation").at("Yaw")=="8192","class name as id");
            Check(ops[4].at("properties").at("Event")=="MainAlarm" && ops[4].at("properties").at("Tag")=="Fireworks","tag and event");
            Check(ops[5].at("id")=="Lamp1_2" && ops[5].at("properties").at("LightColor").at("R")=="255","duplicate id renumbered, JSON cell");
            Check(Warned(report,"became Lamp1_2"),"renamed id reported");
            Check(Import::IsActorTable("Class;X;Y\nEngine.Light;0;0\n") && !Import::IsActorTable("#,#\n#,.\n"),"actor table sniffing");
        }
        // --- T3D -----------------------------------------------------------------
        {
            Json report;
            const auto spec=Run("room.t3d",{{"properties","all"}},report);
            Check(report.at("format")=="t3d","t3d detected");
            Valid(spec,"t3d");
            const auto& brushes=At(spec,{"geometry","brushes"});
            Check(brushes.size()==2,"builder brush skipped");
            const auto& carved=brushes[0];
            Check(carved.at("csg")=="CSG_Subtract" && carved.at("at")==Json::array({512,0,128}) && carved.at("polygons")[0].at("texture")=="Bunker.Walls.Concrete","subtractive brush");
            double lo[3]={1e9,1e9,1e9},hi[3]={-1e9,-1e9,-1e9};
            for(const auto& p:carved.at("polygons"))for(const auto& v:p.at("vertices"))for(int k=0;k<3;++k){lo[k]=std::min(lo[k],v[k].get<double>());hi[k]=std::max(hi[k],v[k].get<double>());}
            Check(lo[0]==-128 && hi[0]==128 && lo[1]==-256 && hi[1]==256 && lo[2]==0 && hi[2]==256,"PrePivot, MainScale and a quarter turn applied");
            Check(brushes[1].at("csg")=="CSG_Add" && brushes[1].at("polyFlags")==32,"mirrored added brush keeps its flags (and winds outward)");
            const auto& volume=At(spec,{"environment","volumes"})[0];
            Check(volume.at("class")=="BlockingVolume" && volume.at("box").at("min")==Json::array({256,0,0}) && volume.at("box").at("max")==Json::array({320,64,32}),"volume box");
            Check(At(spec,{"spawns","merc"})[0].at("yaw")==180 && At(spec,{"spawns","spy"}).size()==1,"starts by team");
            const auto& light=At(spec,{"environment","lights"})[0];
            Check(light.at("preset")=="spot" && light.at("brightness")==180 && light.at("yaw")==90,"spot light");
            const auto& crate=At(spec,{"geometry","meshes"})[0];
            Check(crate.at("id")=="Crate" && crate.at("mesh")=="StaticMesh'Crates.Wood.Crate01'" && crate.at("scale")==Json::array({2,2,1}) && crate.at("yaw")==45,"static mesh");
            const auto& magic=At(spec,{"environment","props"})[0];
            Check(magic.at("id")=="Fireworks" && magic.at("properties").at("Groups").size()==2,"generic actor with its properties");
            Check(magic.at("properties").at("Groups")[0].at("EventGroup")[0].at("Event")=="Boom","nested arrays of structs");
            Check(Warned(report,"movers") && Warned(report,"levelinfo") && Assumed(report,"builder brush"),"left-out actors reported");
            Check(Import::T3d::Value("(X=1,Y=(A=2,B=\"x,y\"))")==Json{{"X","1"},{"Y",{{"A","2"},{"B","\"x,y\""}}}},"struct values");
        }
        // --- Hammer and Quake, through the dispatcher -----------------------------
        {
            Json report;
            const auto vmf=Run("cube.vmf",{{"scale",1},{"mirrorY",false}},report);
            Check(report.at("format")=="vmf" && At(vmf,{"geometry","brushes"})[1].at("polygons").size()==6,"vmf cube: 6 polygons");
            Valid(vmf,"vmf");
            const auto quake=Run("quake-room.map",Json::object(),report);
            Check(report.at("format")=="quake" && At(quake,{"geometry","brushes"}).size()==4,"quake map");
            Valid(quake,"quake");
        }
        // --- Meshes as assets ----------------------------------------------------
        {
            Json report;
            const auto spec=Run("crate.obj",{{"scale",64},{"mapFile","TestD"}},report);
            Check(report.at("format")=="obj","obj detected");
            Spec::Parse(spec);
            const auto& asset=report.at("assets")[0];
            Check(asset.at("vertices")==8 && asset.at("faces")==6 && asset.at("triangles")==12,"counts");
            Check(asset.at("unrealBounds")==Json{{"min",{-64,-64,0}},{"max",{64,64,128}}},"Y-up mesh in Unreal axes");
            Check(asset.at("materials")==Json::array({"Wood","Metal"}) && asset.at("groups")==Json::array({"Crate","Lid"}) && asset.at("mtllib")=="crate.mtl","parts");
            Check(asset.at("suggestedPackage")=="TestD_STM" && asset.at("name")=="crate","suggested names");
            Check(Warned(report,"refer to vertices"),"bad face counted");
            Check(!spec.contains("geometry"),"a mesh is never geometry");
            const auto placed=Run("crate.obj",{{"mesh","StaticMesh'TestD_STM.Imported.crate'"}},report);
            Check(At(placed,{"geometry","meshes"})[0].at("mesh")=="StaticMesh'TestD_STM.Imported.crate'","placing an imported mesh");
            const auto ase=Run("crate.ase",Json::object(),report);
            Check(report.at("format")=="ase" && report.at("assets")[0].at("vertices")==4 && report.at("assets")[0].at("faces")==2,"ase counts");
            Check(report.at("assets")[0].at("unrealBounds")==Json{{"min",{-32,-16,0}},{"max",{32,16,64}}} && report.at("assets")[0].at("materials")[0]=="Wood","ase bounds and materials");
            Spec::Parse(ase);
        }
        // --- Map-authoring export --------------------------------------------------
        {
            Json report;
            const auto refs=Run("authoring-export.json",Json::object(),report);
            Check(report.at("format")=="map-authoring" && refs.at("map").at("name")=="TESTD","export detected, named after its map");
            Spec::Parse(refs);
            const auto& r=At(refs,{"existing","refs"});
            Check(r.size()==10 && report.at("counts").at("components")==1,"every actor a ref, components skipped");
            Check(r.contains("MercSpawn") && r.at("MercSpawn").at("path")=="MyLevel.PlayerStart1" && r.contains("PlayerStart0") && r.contains("Server") && r.contains("Steam"),"named by Tag or object name");
            Check(report.at("sections").at("spawn")==2 && report.at("sections").at("logic")==1,"sections counted");
            const auto rebuilt=Run("authoring-export.json",{{"mode","recreate"},{"values",true}},report);
            Valid(rebuilt,"authoring");
            Check(At(rebuilt,{"geometry","brushes"}).size()==1,"geometry from geometryT3d");
            Check(At(rebuilt,{"spawns","spy"})[0].at("yaw")==90 && At(rebuilt,{"spawns","merc"}).size()==1,"starts");
            const auto& objectives=At(rebuilt,{"gameplay","story","objectives"});
            Check(objectives.size()==2 && objectives[0].at("name")=="Main server" && objectives[0].at("triggers")[0].at("dominationCaptureTime")==12,"terminal");
            Check(objectives[1].at("triggers")[0].at("dropZones").size()==1 && objectives[1].at("triggers")[0].at("resetTime")==30,"flag and its drop zone");
            Check(At(rebuilt,{"security","camnet"})[0].at("first")==true && At(rebuilt,{"security","camnet"})[0].at("yaw")==270,"camnet");
            Check(At(rebuilt,{"environment","props"})[0].at("id")=="Steam" && At(rebuilt,{"environment","props"})[0].at("properties").contains("Emitters"),"other actors with values");
            Check(Warned(report,"missions"),"logic actors reported");
            Reject([&]{Json rr;Import::Import("map-authoring","x.json","{\"format\":\"scct.map-changes\"}",Json::object(),rr);},"not a map-authoring");
        }
        // --- R6 Vegas placements --------------------------------------------------
        {
            Json report;
            const auto scan=Run("r6v-placements.json",{{"package","VegasProps"},{"group","Static"}},report);
            Check(report.at("format")=="r6v","placements detected");
            Valid(scan,"r6v");
            const auto& props=At(scan,{"environment","props"});
            Check(props.size()==2 && report.at("counts").at("skipped")==1,"meshless component skipped");
            Check(props[0].at("mesh")=="VegasProps.Static.CrateLarge" && props[0].at("yaw")==90 && props[0].at("properties").at("DrawScale3D").at("Z")=="2","mesh from the package option");
            Check(props[1].at("properties").at("DrawScale3D").at("X")=="2" && Warned(report,"mirrored") && Warned(report,"component offsets"),"scale and the scan's limits");
            const auto exact=Run("r6v-inventory.json",Json::object(),report);
            const auto& item=At(exact,{"environment","props"})[0];
            Check(item.at("mesh")=="VegasProps.Static.CrateLarge" && item.at("at")==Json::array({100,200,0}) && item.at("properties").at("Skins").size()==2 && !item.at("properties").contains("Location"),"inventory kept exactly");
            Valid(exact,"r6v-inventory");
            Json r;
            Check(Import::Import("auto","p.json",Read("r6v-placements.json"),Json::object(),r).contains("environment")==false && Warned(r,"no SCCT mesh"),"no mesh mapping, nothing placed");
        }
        // --- A raster plan ---------------------------------------------------------
        {
            Image::Rgba plan(48,24,{0,0,0,255});
            auto fill=[&](int x0,int y0,int x1,int y1,std::array<uint8_t,4> c){for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)std::memcpy(plan.At(x,y),c.data(),4);};
            fill(2,2,22,22,{255,255,255,255});
            fill(26,2,46,22,{250,250,250,255});
            fill(22,10,26,14,{255,255,255,255});
            for(int i=0;i<3;++i){fill(4+i*4,4,6+i*4,6,{0,0,255,255});fill(30+i*4,4,32+i*4,6,{255,0,0,255});}
            fill(11,15,13,17,{0,255,0,255});
            fill(40,18,41,19,{0,250,250,255});
            const auto png=Image::EncodePng(plan);
            Json report;
            const auto spec=Import::Import("auto","plan.png",png,{{"pixelsPerCell",2}},report);
            Check(report.at("format")=="image","png detected");
            Valid(spec,"image");
            Check(At(spec,{"spawns","spy"}).size()==3 && At(spec,{"spawns","merc"}).size()==3,"coloured dots are starts, one per dot");
            Check(At(spec,{"gameplay","story","objectives"}).size()==1,"green terminal");
            Check(report.contains("overlay") && Image::IsPng(Bridge::Unbase64(report.at("overlay").get<std::string>())),"overlay PNG");
            Check(report.at("overlaySize")==Json::array({192,96}),"overlay size");
            Reject([&]{Json rr;Import::Import("auto","plan.jpg",std::string("\xFF\xD8\xFF\xE0 JFIF",10),Json::object(),rr);},"convert it to PNG");
        }
        // --- Pass-through formats and detection ------------------------------------
        {
            Json report;
            const std::string spec=R"({"format":"scct.level-spec","version":1,"map":{"name":"PASS"}})";
            Check(Import::Import("auto","x.json",spec,Json::object(),report).at("map").at("file")=="PassD","level spec checked and filled in");
            Reject([&]{Json r;Import::Import("auto","x.json",R"({"format":"scct.level-spec","version":1,"oops":1})",Json::object(),r);},"unknown field");
            const std::string changes=R"({"format":"scct.map-changes","version":3,"map":"x","allowDeletes":true,"operations":[{"op":"create","id":"A","class":"Engine.Light","properties":{"Location":{"X":"0","Y":"0","Z":"0"}}},{"op":"delete","actor":{"path":"MyLevel.B","class":"Engine.Light"},"before":{}}]})";
            const auto wrapped=Import::Import("auto","changes.json",changes,Json::object(),report);
            Check(report.at("format")=="map-changes" && At(wrapped,{"extra","operations"}).size()==2 && Warned(report,"delete"),"map-changes wrapped");
            Check(Import::Detect("level.txt","#####\n#S.M#\n#####\n")=="grid" && Import::Detect("grid.csv","#,.,#\n")=="grid","grid detection");
            Check(Import::Detect("noext",Read("plan.svg"))=="svg" && Import::Detect("noext",Read("room.t3d"))=="t3d" && Import::Detect("noext",Read("cube.vmf"))=="vmf","content sniffing");
            Check(Import::Detect("noext",Read("quake-room.map"))=="quake" && Import::Detect("noext",Read("crate.obj"))=="obj" && Import::Detect("noext",Read("plan.tmx"))=="tiled","more sniffing");
            Check(Import::Detect("level.map","Begin Map\nEnd Map\n")=="t3d","a .map that is T3D");
            Reject([&]{Json r;Import::Import("blender","x.blend","",Json::object(),r);},"Unknown import format");
            Reject([&]{Json r;Import::Import("auto","x.json","{\"hello\":1}",Json::object(),r);},"is not a level spec");
            Check(Import::FormatList().size()==Import::Formats().size(),"format list");
            Json grid;
            const auto fromGrid=Import::Import("grid","g.txt","#######\n#S...M#\n#######\n",{{"name","Grid named"}},grid);
            Check(fromGrid.at("map").at("name")=="GRID NAMED" && grid.at("format")=="grid","grid by name");
        }
        // Xml edge cases.
        {
            const auto doc=Import::Xml::Parse("<?xml version=\"1.0\"?>\n<!DOCTYPE svg [ <!ENTITY x \"y\"> ]>\n<!-- c --><a b='1 &amp; 2'>t&lt;<![CDATA[<raw>]]><c/>&#65;&#x42;</a>");
            Check(doc.name=="a" && doc.Get("b")=="1 & 2" && doc.text=="t<<raw>AB" && doc.children.size()==1,"xml basics");
            Reject([&]{Import::Xml::Parse("<a><b></a>");},"closes");
            Reject([&]{Import::Xml::Parse("<a b=c/>");},"not quoted");
        }
        std::cout<<"ImportersTests passed\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"ImportersTests failed: "<<e.what()<<"\n";return 1;}
}
