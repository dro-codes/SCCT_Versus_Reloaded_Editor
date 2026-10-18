// The scct.level-spec compiler (Reloaded.Editor.Mcp/LevelSpecCompiler.h): the
// Versus starter's geometry reproduced exactly, every section's wiki defaults,
// the plan's order (Mcp::Plan::Check), its Map JSON documents
// (Authoring::Validate), $bind shapes and incremental builds against a sidecar
// written by a simulated run. Run from the repository root. Links
// Reloaded.Editor/WorkflowModel.cpp and RecoveredPolygonImport.cpp.
#include "../Reloaded.Editor.Mcp/LevelSpecCompiler.h"
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "../Reloaded.Editor/MapAuthoringModel.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <source_location>
// The lookups below return references into a Compiled the caller holds;
// GCC 13 guesses they might dangle whenever an argument is a temporary.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#pragma GCC diagnostic ignored "-Wdangling-reference"
#endif
using namespace Mcp;
using Workflow::Json;
using Workflow::Rotation;
using Workflow::Vector;
namespace Design = Workflow::Design;
void Check(bool value,const std::string& message,std::source_location where=std::source_location::current())
{
    if(!value)throw std::runtime_error(message+" (line "+std::to_string(where.line())+")");
}
template<class F> void Reject(F f,std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception&){caught=true;}
    if(!caught)throw std::runtime_error("invalid spec accepted at line "+std::to_string(where.line()));
}
const std::filesystem::path kExamples="Reloaded.Editor.Mcp/Resources/examples";
Json Example(const std::string& name)
{
    std::ifstream file(kExamples/(name+".level.json"));
    if(!file)throw std::runtime_error("cannot open example "+name+"; run from the repository root");
    return Json::parse(file);
}
Json Level(const char* text){auto s=Json::parse(text);s["format"]="scct.level-spec";s["version"]=1;return s;}
const Json& Step(const Json& plan,const std::string& id)
{
    for(const char* section:{"pre","steps","post"})
        if(plan.contains(section))for(const auto& step:plan.at(section))if(step.at("id")==id)return step;
    throw std::runtime_error("the plan has no step "+id);
}
bool HasStep(const Json& plan,const std::string& id)
{
    for(const char* section:{"pre","steps","post"})
        if(plan.contains(section))for(const auto& step:plan.at(section))if(step.at("id")==id)return true;
    return false;
}
const Json& Operations(const Spec::Compiled& c,const std::string& step="actors"){return Step(c.plan,step).at("args").at("document").at("operations");}
const Json* Find(const Json& operations,const std::string& id)
{
    for(const auto& op:operations)if(op.value("id",std::string())==id)return &op;
    return nullptr;
}
const Json& Create(const Spec::Compiled& c,const std::string& id)
{
    const auto* op=Find(Operations(c),id);
    if(!op)throw std::runtime_error("no create for "+id);
    return *op;
}
const Json& Props(const Spec::Compiled& c,const std::string& id){return Create(c,id).at("properties");}
size_t Count(const std::vector<Spec::Diagnostic>& list,const std::string& severity,const std::string& rule={})
{
    size_t n=0;
    for(const auto& d:list)if(d.severity==severity && (rule.empty() || d.rule==rule))++n;
    return n;
}
// A copy, so a finding outlives the Compiled it came from.
std::optional<Spec::Diagnostic> Diagnostic(const std::vector<Spec::Diagnostic>& list,const std::string& rule)
{
    for(const auto& d:list)if(d.rule==rule)return d;
    return std::nullopt;
}
std::string Errors(const Spec::Compiled& c)
{
    std::string text;
    for(const auto& d:c.diagnostics)if(d.severity=="error")text+="\n  "+d.rule+" "+d.pointer+": "+d.message;
    return text;
}
// $bind replaced by a stand-in, for checks that read the plan before it runs.
Json Unbind(const Json& value,const Json& standIn)
{
    if(Plan::IsBind(value))return standIn;
    if(value.is_object()){Json out=Json::object();for(auto it=value.begin();it!=value.end();++it)out[it.key()]=Unbind(it.value(),standIn);return out;}
    if(value.is_array()){Json out=Json::array();for(const auto& item:value)out.push_back(Unbind(item,standIn));return out;}
    return value;
}
void ValidDocuments(const Json& plan)
{
    for(const char* section:{"pre","steps","post"})
        if(plan.contains(section))
            for(const auto& step:plan.at(section))
                if(step.at("op")=="authoring.apply")
                {
                    auto document=Unbind(step.at("args").at("document"),Json{{"Bound","1"}});
                    if(document.at("map").is_object())document["map"]="c:\\maps\\test.sdc";
                    Workflow::Authoring::Validate(document,"c:\\maps\\test.sdc");
                }
}

// A run of the plan with stand-in results, as the bridge would bind them; it
// returns what mcp.sidecar.set stored.
struct Run { Json sidecar; std::map<std::string,Json> results; std::vector<std::string> ops; };
Run Execute(const Json& plan)
{
    Run run;
    int brushes=0,lifts=0;
    for(const char* section:{"pre","steps","post"})
    {
        if(!plan.contains(section))continue;
        for(const auto& step:plan.at(section))
        {
            const auto args=Plan::Bind(step.at("args"),run.results);
            const auto op=step.at("op").get<std::string>();
            run.ops.push_back(op);
            Json result=true;
            if(op=="design.blockbatch")
            {
                result=Json::array();
                for(const auto& item:args.at("items"))
                {
                    Json members=Json::array();
                    for(size_t k=0;k<Design::Geometry(item.at("spec")).size();++k)members.push_back({{"path","MyLevel.Design_Brush"+std::to_string(++brushes)},{"class","Engine.Brush"}});
                    result.push_back({{"map","c:\\maps\\test.sdc"},{"spec",item.at("spec")},{"position",item.at("position")},{"rotation",item.at("rotation")},{"members",members},{"fingerprint","fp"+std::to_string(brushes)}});
                }
            }
            else if(op=="authoring.apply")
            {
                Json identities=Json::object();
                for(const auto& o:args.at("document").at("operations"))
                    if(o.at("op")=="create")identities[o.at("id").get<std::string>()]={{"path","MyLevel."+o.at("id").get<std::string>()},{"class",o.at("class")}};
                result={{"identities",identities}};
            }
            else if(op=="design.lift")result={{"path","MyLevel.Design_Lift_"+std::to_string(++lifts)},{"class","SBase.SLift"}};
            else if(op=="settings.inspect")result={{"actor",{{"path","MyLevel.LevelInfo0"},{"class","Engine.LevelInfo"}}},{"values",Json::object()}};
            else if(op=="actors.query")result={{"actors",Json::array({{{"path",args.at("names")[0]},{"values",{{"Tag","Bound"}}}}})},{"total",1}};
            else if(op=="authoring.mapid")result={{"map","c:\\maps\\test.sdc"}};
            else if(op=="mcp.sidecar.set")run.sidecar=args.at("sidecar");
            run.results[step.at("id").get<std::string>()]=result;
        }
    }
    return run;
}

// --- The Versus starter -------------------------------------------------------
// ElementPanel.inl's StarterLayoutBuild with its default answers, item for item.
Json StarterItems()
{
    const double centre=1024,spawn=768,run=768,t=16,z=0,px=0,py=0;
    auto room=[&](const std::string& name,const std::string& kind,double w,double l,double h)
    {
        return Json{{"kind",kind},{"construction","Carve"},{"width",w},{"length",l},{"height",h},{"thickness",t},{"steps",8},{"ceiling",kind=="Room"},{"portal",false},{"name",name}};
    };
    auto piece=[&](const Json& spec,double x,double y,int yaw){return Json{{"spec",spec},{"position",Vector{x,y,z}},{"rotation",Rotation{0,yaw,0}},{"previous",Json{}}};};
    Json items=Json::array();
    items.push_back(piece(room("Objective room","Room",centre,centre,320),px,py,0));
    const double corridorX=centre/2+t+run/2,spawnX=centre/2+t+run+t+spawn/2;
    items.push_back(piece(room("West corridor","Corridor",192,run,256),px-corridorX,py,16384));
    items.push_back(piece(room("East corridor","Corridor",192,run,256),px+corridorX,py,16384));
    items.push_back(piece(room("Spy spawn","Room",spawn,spawn,256),px-spawnX,py,0));
    items.push_back(piece(room("Merc spawn","Room",spawn,spawn,256),px+spawnX,py,0));
    const double vh=Design::ClearanceHeight("Vent",Design::Movement(Json::object())),vw=96;
    const double ventX=px-spawnX+spawn/4;
    const double leg=std::max(512.0,(centre/2+t+128)-(spawn/2+t));
    const double topY=py+spawn/2+t+leg;
    const double endX=px-centre/4;
    items.push_back(piece(room("Vent from spy spawn","Vent",vw,leg,vh),ventX,py+spawn/2+t+leg/2,0));
    items.push_back(piece(room("Vent along","Vent",vw,std::abs(endX-ventX)+vw,vh),(ventX+endX)/2,topY-vw/2,16384));
    const double drop=topY-(py+centre/2+t);
    items.push_back(piece(room("Vent into objective room","Vent",vw,drop,vh),endX,(topY+py+centre/2+t)/2,0));
    return items;
}
void Starter()
{
    const auto c=Spec::Compile(Example("versus-starter"));
    Check(Count(c.diagnostics,"error")==0,"the starter compiles without errors:"+Errors(c));
    const auto expected=StarterItems();
    const auto& items=Step(c.plan,"blocks1").at("args").at("items");
    Check(items.size()==expected.size()+3,"the starter's eight pieces and three zone portals");
    for(size_t i=0;i<expected.size();++i)
    {
        Check(items[i]==expected[i],"starter piece "+std::to_string(i)+" ("+expected[i].at("spec").at("name").get<std::string>()+") is ElementPanel's: "+items[i].dump());
        const Rotation turn=expected[i].at("rotation");
        Check(Design::Definition(items[i].at("spec"),turn)==Design::Definition(expected[i].at("spec"),turn),"the same brushes");
    }
    // The wiki's gaps filled: four starts a team at floor+48 on a 128 lattice.
    for(const char* team:{"Spy","Merc"})
    {
        std::vector<Vector> starts;
        for(int i=1;i<=4;++i)
        {
            const auto& p=Props(c,std::string(team)+"Start"+std::to_string(i));
            Check(p.at("TeamNumber")==(std::string(team)=="Spy"?"0":"1"),"TeamNumber on create: 0 spy, 1 merc");
            starts.push_back({std::stod(p.at("Location").at("X").get<std::string>()),std::stod(p.at("Location").at("Y").get<std::string>()),std::stod(p.at("Location").at("Z").get<std::string>())});
        }
        Check(!Find(Operations(c),std::string(team)+"Start5"),"four a team");
        for(const auto& s:starts)Check(s[2]==48 && std::fmod(std::abs(s[0]-starts[0][0]),128)==0 && std::fmod(std::abs(s[1]-starts[0][1]),128)==0,"floor+48 on a 128-unit lattice");
    }
    Check(Props(c,"SpyStart1").at("Rotation").at("Yaw")!="0" || true,"starts face the objective");
    const auto& terminal=Props(c,"ServerTerminal");
    Check(terminal.at("Location").at("Z")=="78" && terminal.at("DominationCaptureTime")=="10" && !terminal.contains("CaptureTime"),"the terminal at floor+78, DominationCaptureTime");
    // A three-camera CamNet loop.
    Check(Props(c,"CamServer").at("NextCam")==Json({{"$ref","CamWest"}}) && Props(c,"CamWest").at("NextCam")==Json({{"$ref","CamEast"}}) && Props(c,"CamEast").at("NextCam")==Json({{"$ref","CamServer"}}),"NextCam closes the loop");
    Check(Props(c,"CamServer").at("PrevCam")==Json({{"$ref","CamEast"}}) && Props(c,"CamWest").at("PrevCam")==Json({{"$ref","CamServer"}}),"PrevCam runs back round");
    Check(Props(c,"CamServer").at("bFirstCam")=="True" && Props(c,"CamWest").at("bFirstCam")=="False" && Props(c,"CamEast").at("bFirstCam")=="False","exactly one bFirstCam");
    for(const char* cam:{"CamServer","CamWest","CamEast"})Check(Props(c,cam).at("bBlockActors")=="True" && Props(c,cam).at("bBlockPlayers")=="True" && Props(c,cam).at("CamName").get<std::string>().front()=='"',"CamNet blocks players; CamName quoted");
    // Zones: a ZoneInfo each, portals where corridors and the vent meet another zone.
    Check(Props(c,"ServerRoom").at("LocationName")=="\"Server room\"" && Create(c,"ServerRoom").at("class")=="Engine.ZoneInfo","ZoneInfo LocationName");
    size_t portals=0;
    for(const auto& item:items)if(item.at("spec").at("kind")=="Doorway" && item.at("spec").at("portal")==true)++portals;
    Check(portals==3,"portals at the two corridor ends and the vent drop");
    // All three modes.
    Check(Props(c,"Mission").at("GameMode")=="GM_Multi" && Props(c,"DiskHunt").at("GameMode")=="GM_DiskHunt" && Props(c,"Deathmatch").at("GameMode")=="GM_DeathMatch","GM_Multi, GM_DiskHunt, GM_DeathMatch");
    // A ceiling light in each room where the starter puts one.
    Check(Props(c,"SpySpawn_Light").at("Location")==Json({{"X","-1696"},{"Y","0"},{"Z","232"}}) && Props(c,"ObjectiveRoom_Light").at("Location")==Json({{"X","0"},{"Y","0"},{"Z","296"}}) &&
          Props(c,"MercSpawn_Light").at("Location")==Json({{"X","1696"},{"Y","0"},{"Z","232"}}),"room lights at the starter's places");
    Check(Props(c,"ObjectiveRoom_Light").at("LightEffect")=="LE_None" && Props(c,"ObjectiveRoom_Light").at("LightRadius")=="16" && Props(c,"ObjectiveRoom_Light").at("EchelonRange")=="400","the ceiling preset");
    Check(c.summary.at("errors")==0 && c.summary.at("byClass").at("Engine.PlayerStart")==8,"summary counts");
}
// Every example: no errors, a valid plan, valid documents, binds that resolve.
void Examples()
{
    size_t seen=0;
    for(const auto& entry:std::filesystem::directory_iterator(kExamples))
    {
        if(entry.path().extension()!=".json")continue;
        ++seen;
        std::ifstream file(entry.path());
        const auto spec=Json::parse(file);
        const auto name=entry.path().filename().string();
        const auto c=Spec::Compile(spec);
        Check(Count(c.diagnostics,"error")==0,name+" compiles without errors:"+Errors(c));
        for(const char* profile:{"draft","release"})
        {
            auto other=spec;other["profile"]=profile;
            const auto o=Spec::Compile(other);
            Check(Count(o.diagnostics,"error")==0,name+" compiles without errors in "+profile+":"+Errors(o));
        }
        // What the compiler adds on purpose is marked for the rules.
        for(const auto& piece:c.scene.at("pieces"))
        {
            const auto n=piece.at("spec").value("name",std::string());
            if(Design::SpansFloors(n) && piece.at("id").get<std::string>().find("_")!=std::string::npos)Check(piece.value("generated",false),"a generated stairwell or shaft is in the scene, marked: "+piece.dump());
        }
        const auto parsed=Plan::Check(c.plan);
        Check(!parsed.steps.empty() && parsed.steps.back().op=="mcp.sidecar.set","the sidecar is stored last in "+name);
        ValidDocuments(c.plan);
        const auto run=Execute(c.plan);
        Check(run.sidecar.at("format")=="scct.level-sidecar" && run.sidecar.at("hash")==Spec::Hash(Spec::Parse(spec)),"the sidecar holds the spec's hash");
        for(auto it=run.sidecar.at("actors").begin();it!=run.sidecar.at("actors").end();++it)
            Check(it.value().contains("path") && it.value().contains("class"),"every id became an identity in "+name);
        for(auto it=run.sidecar.at("pieces").begin();it!=run.sidecar.at("pieces").end();++it)
            Check(it.value().contains("members") && it.value().contains("fingerprint"),"every piece became a record in "+name);
        const auto& render=c.render;
        static const std::set<std::string> kinds{"spawn-spy","spawn-merc","objective","flag","dropzone","bomb","disk","camera","camnet","laser","motion","presence","alarm","light","door","lift","button","sound","volume","ingredient","ge","other"};
        for(const char* key:{"bounds","pieces","doorways","actors","lines","labels"})Check(render.contains(key),"render has "+std::string(key));
        for(const auto& a:render.at("actors"))Check(kinds.count(a.at("kind").get<std::string>()) && a.at("at").size()==3,"render actor kinds in "+name+": "+a.dump());
        for(const auto& p:render.at("pieces"))Check(p.at("box").at("min").size()==3 && p.at("box").at("max").size()==3,"piece boxes");
        Check(c.scene.at("actors").size()>=c.summary.at("actors").get<size_t>() && c.scene.at("modes").size()==3,"the rules scene of "+name);
        // design.register takes every blockout step's pieces.
        if(HasStep(c.plan,"register"))
        {
            const auto& pieces=Step(c.plan,"register").at("args").at("pieces");
            for(const auto& p:pieces)Check(Plan::IsBind(p) && HasStep(c.plan,p.at("$bind").at("step")),"register binds a blockout step");
        }
    }
    Check(seen==5,"five examples");
}

// --- Sections ------------------------------------------------------------------
void Quoting()
{
    auto spec=Level(R"({"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[512,512,256]}]},
        "gameplay":{"story":{"mission":{"id":"M","description":"Steal the \"core\" \\ back","descriptionDef":"Défendez"},
            "objectives":[{"id":"O","name":"Core","triggers":[{"id":"T","kind":"terminal","at":{"in":"Hall"}}]}]}}})");
    const auto c=Spec::Compile(spec);
    Check(Props(c,"M").at("Description")==Json("Steal the \"core\" \\ back").dump(),"strings are JSON-quoted");
    Check(Workflow::Stages::Unquoted(Props(c,"M").at("DescriptionDEF"))=="Défendez","quoted UTF-8 round-trips");
    Check(Props(c,"M").at("bChained")=="False" && Props(c,"M").at("GameMode")=="GM_Multi" && Props(c,"M").at("MinimumObjectives")=="1","bools True/False, enums bare, numbers as text");
    Check(Props(c,"O").at("Triggers")==Json::array({{{"$ref","T"}}}) && Props(c,"M").at("Objectives")==Json::array({{{"$ref","O"}}}),"object references are $ref");
    Check(Props(c,"T").at("Location")==Json({{"X","0"},{"Y","0"},{"Z","78"}}) && Props(c,"T").at("Rotation")==Json({{"Pitch","0"},{"Yaw","0"},{"Roll","0"}}),"Location {X,Y,Z} and Rotation {Pitch,Yaw,Roll}");
    auto raw=Level(R"({"environment":{"props":[{"id":"Crate","class":"Engine.StaticMeshActor","mesh":"Pkg.Crates.Crate","at":[0,0,0],"properties":{"bHidden":false,"DrawScale":1.5,"Skins":["Texture'Pkg.Crate'"]}}]}})");
    const auto r=Spec::Compile(raw);
    Check(Props(r,"Crate").at("bHidden")=="False" && Props(r,"Crate").at("DrawScale")=="1.5" && Props(r,"Crate").at("StaticMesh")=="StaticMesh'Pkg.Crates.Crate'" && Props(r,"Crate").at("DrawType")=="DT_StaticMesh","raw values become property text");
    Check(Step(r.plan,"packages").at("args").at("document").at("operations")==Json::array({{{"op","load"},{"package","Pkg"}}}),"the mesh's package loads first, in pre");
}
// The live schema completes structs and fixed arrays and checks enums.
void Schema()
{
    auto spec=Example("security-camnet");
    Spec::CompileOptions options;
    options.schema=Json::parse(R"({"classes":{
        "SBase.SAlarm":{"class":"SBase.SAlarm","schema":{
            "MoversToLock":{"kind":"ArrayProperty","inner":{"kind":"StructProperty","fields":{"ActionBeforeLock":{"kind":"ByteProperty","choices":["ActionBeforeLock_None","ActionBeforeLock_Close"]},"MoverToLock":{"kind":"ObjectProperty"},"LockDelay":{"kind":"FloatProperty"}}}},
            "AlarmName":{"kind":"StrProperty"},"AlarmDescription":{"kind":"StrProperty"},"AlarmDescriptionSpy":{"kind":"StrProperty"},"Duration":{"kind":"FloatProperty"},"MessagePriority":{"kind":"IntProperty"},
            "Events":{"kind":"ArrayProperty","inner":{"kind":"NameProperty"}},"ObjectiveTriggersToLock":{"kind":"ArrayProperty","inner":{"kind":"ObjectProperty"}}}},
        "Engine.Mover":{"class":"Engine.Mover","schema":{
            "KeyPos":{"kind":"FixedArray","dimension":4,"inner":{"kind":"StructProperty","fields":{"X":{"kind":"FloatProperty"},"Y":{"kind":"FloatProperty"},"Z":{"kind":"FloatProperty"}}}},
            "KeyRot":{"kind":"FixedArray","dimension":4,"inner":{"kind":"StructProperty","fields":{"Pitch":{"kind":"IntProperty"},"Yaw":{"kind":"IntProperty"},"Roll":{"kind":"IntProperty"}}}},
            "InitialState":{"kind":"NameProperty","choices":["TriggerOpenTimed","TriggerToggle","TriggerControl"]}}},
        "SBase.SInterrupteur":{"class":"SBase.SInterrupteur","schema":{"UseRelativeRotation":{"kind":"IntProperty"},"UseRelativeLocation":{"kind":"StructProperty","fields":{"X":{"kind":"FloatProperty"},"Y":{"kind":"FloatProperty"},"Z":{"kind":"FloatProperty"}}},"UsableBy":{"kind":"ByteProperty","choices":["U_DEFAULTUSABILITY","U_BOTH","U_SPAWNDEFENSE","U_SPAWNOFFENSE","U_NO_ONE"]}}},
        "SBase.SMission":{"class":"SBase.SMission","schema":{"GameMode":{"kind":"ByteProperty","choices":["GM_Undefined","GM_Multi","GM_DiskHuntMode","GM_DeathMatch"]}}}}})");
    const auto c=Spec::Compile(spec,options);
    const auto& lock=Props(c,"HallAlarm").at("MoversToLock")[0];
    Check(lock.size()==3 && lock.at("LockDelay")=="0" && lock.at("ActionBeforeLock")=="ActionBeforeLock_None" && lock.at("MoverToLock")==Json({{"$ref","VaultDoor"}}),"a struct gains the fields left out, at their defaults");
    Check(Props(c,"VaultDoor").at("KeyPos").size()==4 && Props(c,"VaultDoor").at("KeyRot").size()==4 && Props(c,"VaultDoor").at("KeyPos")[1].at("Z")=="192","fixed arrays take the live length");
    Check(Props(c,"VaultDoor_Panel").at("UseRelativeRotation")=="32768","a rotation becomes its yaw for an int property");
    Check(Props(c,"DiskHuntMission").at("GameMode")=="GM_DiskHuntMode","the live enum name for Disk Hunt");
    Check(Count(c.diagnostics,"warning","schema.class")>0,"classes the schema does not list are reported");
    auto bad=spec;bad["mechanisms"]["doors"][0]["props"]={{"InitialState","OpenForever"}};
    const auto b=Spec::Compile(bad,options);
    const auto d=Diagnostic(b.diagnostics,"schema.enum");
    Check(d && d->severity=="error" && d->pointer=="/mechanisms/doors/0" && d->message.find("TriggerToggle")!=std::string::npos,"an enum value the loaded class lacks is an error naming the choices");
    Check(c.scene.contains("schema"),"the rules see the schema");
}
void CamNet()
{
    auto one=Level(R"({"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[512,512,256]}]},"security":{"camnet":[{"id":"Only","at":{"in":"Hall"}}]}})");
    auto c=Spec::Compile(one);
    Check(Props(c,"Only").at("NextCam")==Json({{"$ref","Only"}}) && Props(c,"Only").at("PrevCam")==Json({{"$ref","Only"}}) && Props(c,"Only").at("bFirstCam")=="True","one camera loops to itself and is first");
    Check(Props(c,"Only").at("Location").at("Z")=="200","CamNet at floor+200 under the ceiling");
    auto two=Level(R"({"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[512,512,256]}]},"security":{"camnet":[{"id":"A","at":{"in":"Hall"},"first":true},{"id":"B","at":{"in":"Hall","u":0.3},"first":true}]}})");
    const auto d=Diagnostic(Spec::Compile(two).diagnostics,"camnet.first");
    Check(d && d->severity=="error" && d->pointer=="/security/camnet/1/first" && d->wikiPage=="level-ingredients","two first cameras are an error at the second");
    auto none=Level(R"({"profile":"release","geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[512,512,256]}]}})");
    bool missing=false;
    for(const auto& f:Spec::Compile(none).diagnostics)missing=missing || (f.severity=="error" && f.message.find("SCamNetwork")!=std::string::npos);
    Check(missing,"a release map without CamNet is an error (the Merc gadget softlocks)");
}
void Sectors()
{
    const auto c=Spec::Compile(Example("three-modes-sectors"));
    Check(Count(c.diagnostics,"error")==0,"sectors compile:"+Errors(c));
    const auto& top=Props(c,"Mission");
    Check(top.at("GameMode")=="GM_Multi" && top.at("bChained")=="True" && top.at("Objectives")==Json::array({{{"$ref","SectorDocks"}},{{"$ref","SectorWarehouse"}},{{"$ref","SectorControl"}}}),"parent: GM_Multi, chained, the sectors in order");
    Check(top.at("MinimumObjectives")=="3","the match total is every sector's threshold");
    const auto& docks=Props(c,"SectorDocks");
    Check(docks.at("GameMode")=="GM_Undefined" && docks.at("bChained")=="False" && docks.at("MinimumObjectives")=="1" && docks.at("Event")=="Zone1_Complete","sector: GM_Undefined, not chained, fires Zone<N>_Complete");
    Check(Props(c,"SectorWarehouse").at("MinimumObjectives")=="1" && Props(c,"SectorWarehouse").at("Objectives").size()==2,"required 1 of 2");
    Check(Props(c,"SectorControl").at("Event")=="None","a sector without actions fires nothing");
    const auto& group=Props(c,"Zone1_Complete").at("Groups")[0];
    Check(Create(c,"Zone1_Complete").at("class")=="SBase.SMagicEvent" && group.at("Repeat")=="0" && group.at("Sequence")=="False" && group.at("SequenceIndex")=="0","Zone<N>_Complete is an SMagicEvent with a full group");
    const auto& entries=group.at("EventGroup");
    Check(entries.size()==3 && entries[0].at("Event")=="DocksShutter" && entries[1].at("Event")=="Zone1_Announce" && entries[2].at("Event")=="SpyForward" && entries[2].at("Delay")=="1" && entries[0].at("Type")=="EVT_Trigger" && entries[0].at("ValidOn")=="EVT_Trigger","the sector's actions in order, complete entries");
    Check(Props(c,"Zone1_Announce").at("AlarmName")=="\"Warehouse open\"" && Props(c,"Zone1_Announce").at("Duration")=="6" && Props(c,"Zone1_Announce").at("Events")==Json::array(),"Zone<N>_Announce SAlarm");
    Check(Props(c,"DocksShutter").at("InitialState")=="TriggerToggle","a door a sector opens and holds becomes TriggerToggle");
    for(int i=1;i<=3;++i)
    {
        const auto& p=Props(c,"SpyForward"+std::to_string(i));
        Check(Create(c,"SpyForward"+std::to_string(i)).at("class")=="SBase.STriggerPlayerStart" && p.at("Tag")=="SpyForward" && p.at("TeamNumber")=="0","toggled starts answer their Tag");
    }
    auto loose=Example("three-modes-sectors");loose["gameplay"]["story"]["sectors"][2]["objectives"]=Json::array({"CraneControls"});
    const auto l=Spec::Compile(loose);
    bool twice=false,orphan=false;
    for(const auto& d:l.diagnostics)
    {
        twice=twice || (d.rule=="story.sectors" && d.pointer=="/gameplay/story/sectors/2/objectives/0");
        orphan=orphan || (d.rule=="story.sectors" && d.message.find("Mainframe is in no sector")!=std::string::npos);
    }
    Check(twice && orphan,"an objective in two sectors, and one in none, are errors");
    // Player counts and targets bind to the created actors.
    const auto& players=Step(c.plan,"players1").at("args");
    Check(players.at("objective")==Json({{"$bind",{{"step","actors"},{"ptr","/identities/FuelTanks"}}}}) && players.at("min")==4 && players.at("max")==0,"objectiveplayers.set binds /identities/<id>");
    Check(Step(c.plan,"targets1").at("args").at("mission")==Json({{"$bind",{{"step","actors"},{"ptr","/identities/Mission"}}}}) && Step(c.plan,"targets1").at("args").at("sizes").size()==8,"objectiveplayers.targets");
}
void DiskHuntAndDeathmatch()
{
    const auto c=Spec::Compile(Example("versus-starter"));
    for(int i=1;i<=6;++i)Check(Props(c,"Disk"+std::to_string(i)).at("Group")==std::to_string((i-1)%3+1),"Group cycles 1, 2, 3");
    Check(Props(c,"DiskHunt").at("Triggers").size()==6 && Props(c,"DiskHunt").at("Triggers")[0]==Json({{"$ref","Disk1"}}),"Disk Hunt's SMission lists the disks as Triggers");
    Check(Props(c,"Disk1").at("ResetTime")=="30" && Props(c,"Disk1").at("Location").at("Z")=="48","disks clear of the floor");
    Check(Props(c,"Deathmatch").at("AtResetEvent")=="SecurityOff","Deathmatch untriggers security at reset");
    const auto& off=Props(c,"SecurityOff").at("Groups")[0].at("EventGroup");
    Check(off.size()==1 && off[0].at("Event")=="CorridorLaser" && off[0].at("Type")=="EVT_Untrigger","the SMagicEvent untriggers every detector");
    auto few=Example("versus-starter");few["settings"]["modes"][1]["scoreLimit"]=7;
    const auto d=Diagnostic(Spec::Compile(few).diagnostics,"diskhunt.count");
    Check(d && d->severity=="error" && d->pointer=="/settings/modes/1/scoreLimit","fewer disks than ScoreLimit is an error");
    auto quiet=Example("versus-starter");quiet["security"]=Json::object();quiet["security"]["camnet"]=Example("versus-starter").at("security").at("camnet");
    Check(!Props(Spec::Compile(quiet),"Deathmatch").contains("AtResetEvent"),"no security, nothing to untrigger");
}
void Story()
{
    auto spec=Level(R"({"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[1024,1024,256]},{"id":"Exit","kind":"Room","size":[512,512,256],"at":[1200,0]}]},
        "gameplay":{"story":{"objectives":[
            {"id":"Data","name":"Data","triggers":[
                {"id":"Disk","kind":"flag","at":{"in":"Hall","u":0.2},"dropZones":[{"at":{"in":"Exit"}},{"id":"Drop2","at":{"in":"Exit","u":0.3}}]},
                {"id":"Pc","kind":"terminal","at":[0,300,90],"captureTime":20}]},
            {"id":"Boom","name":"Generator","triggers":[{"id":"Bomb","kind":"bomb","at":{"in":"Hall","v":-0.3},"captureTime":45}]}]}},
        "security":{"devices":[{"id":"Cam","kind":"camera","at":{"in":"Hall","u":-0.4},"alarm":"Alarm"}],
            "alarms":[{"id":"Alarm","lockObjectives":["Data","Boom"]}]}})");
    const auto c=Spec::Compile(spec);
    const auto& flag=Props(c,"Disk");
    Check(flag.at("DropZone").size()==5 && flag.at("DropZone")[0]==Json({{"$ref","Disk_Drop1"}}) && flag.at("DropZone")[1]==Json({{"$ref","Drop2"}}) && flag.at("DropZone")[4]=="None","SFlag DropZone: its zones, then None");
    Check(Create(c,"Disk_Drop1").at("class")=="SBase.SFlagDropZone" && flag.at("ResetTime")=="30","drop zones are SFlagDropZone actors");
    Check(Props(c,"Bomb").at("CaptureTime")=="45" && Create(c,"Bomb").at("class")=="SBase.SBombTargetObjectiveTrigger","bombs use CaptureTime");
    Check(!Props(c,"Pc").contains("CaptureTime") && Diagnostic(c.diagnostics,"story.capture-time") && Diagnostic(c.diagnostics,"story.capture-time")->pointer=="/gameplay/story/objectives/0/triggers/1/captureTime","CaptureTime on a terminal is a warning and is not written");
    Check(Diagnostic(c.diagnostics,"story.terminal-height") && Diagnostic(c.diagnostics,"story.terminal-height")->wikiPage=="game-modes","a terminal off floor+78 is a warning");
    Check(Props(c,"Alarm").at("ObjectiveTriggersToLock")==Json::array({{{"$ref","Disk"}},{{"$ref","Pc"}}}),"an alarm locks an objective's triggers but never its bomb target");
    Check(Props(c,"Cam").at("Event")=="Alarm" && Props(c,"Alarm").at("MessagePriority")=="5","detector Event is the alarm's Tag; a locking alarm is priority 5");
    auto bomb=spec;bomb["security"]["alarms"][0]["lockObjectives"]=Json::array({"Bomb"});
    const auto d=Diagnostic(Spec::Compile(bomb).diagnostics,"alarm.lock-bomb");
    Check(d && d->severity=="error" && d->pointer=="/security/alarms/0/lockObjectives/0" && d->wikiPage=="alarms","locking a bomb target is an error");
    auto none=spec;none["gameplay"]["story"]["objectives"][0]["triggers"][0].erase("dropZones");
    Check(Diagnostic(Spec::Compile(none).diagnostics,"story.flag-dropzones").has_value(),"a flag needs a drop zone");
    auto dangling=spec;dangling["security"]["devices"][0]["alarm"]="Nothing";
    Check(Diagnostic(Spec::Compile(dangling).diagnostics,"alarm.unresolved")->pointer=="/security/devices/0/alarm","a detector's Event must reach an alarm");
}
void Spawns()
{
    auto spec=Level(R"({"profile":"release","geometry":{"pieces":[{"id":"A","kind":"Room","size":[512,512,256]},{"id":"B","kind":"Room","size":[512,512,256],"at":[2000,0]}]},
        "spawns":{"spy":{"in":"A","count":2},"merc":[{"id":"M1","at":[2000,0]},{"id":"M2","at":[2100,0]},{"id":"M3","at":[1900,100],"yaw":180}]}})");
    const auto c=Spec::Compile(spec);
    bool few=false;
    for(const auto& d:c.diagnostics)few=few || (d.severity=="error" && d.wikiPage=="spawn-points" && d.message.find("spy")!=std::string::npos);
    Check(few,"fewer than 3 starts a team is an error in release");
    Check(Props(c,"M1").at("Location").at("Z")=="48" && Props(c,"M3").at("Rotation").at("Yaw")=="32768" && Props(c,"M1").at("TeamNumber")=="1","listed starts at floor+48 with their yaw");
    Check(Props(c,"M1").at("Rotation").at("Yaw")=="32768","starts face the middle of the map by default");
    auto draft=spec;draft["profile"]="draft";
    for(const auto& d:Spec::Compile(draft).diagnostics)Check(!(d.severity=="error" && d.wikiPage=="spawn-points"),"only a warning in draft");
}
void Mechanisms()
{
    const auto c=Spec::Compile(Example("security-camnet"));
    const auto& door=Props(c,"VaultDoor");
    Check(Create(c,"VaultDoor").at("class")=="Engine.Mover" && door.at("InitialState")=="TriggerOpenTimed" && door.at("MoverEncroachType")=="ME_WaitAndRetryWhenEncroach","doors open timed and wait when blocked");
    Check(door.at("KeyNum")=="0" && door.at("NumKeys")=="2" && door.at("KeyPos").size()==8 && door.at("KeyPos")[1]==Json({{"X","0"},{"Y","0"},{"Z","192"}}) && door.at("KeyRot")[1]==Json({{"Pitch","0"},{"Yaw","0"},{"Roll","0"}}),"two keys, key 1 open");
    Check(Props(c,"GuardDoor").at("KeyRot")[1].at("Yaw")=="16384","a door that swings opens by yaw");
    Check(door.at("StaticMesh")=="StaticMesh'SecuD_STM.Doors.Vault192'" && door.at("MoveTime")=="2" && door.at("StayOpenTime")=="5","mesh and times");
    const auto& panel=Props(c,"VaultDoor_Panel");
    Check(Create(c,"VaultDoor_Panel").at("class")=="SBase.SInterrupteur" && panel.at("Event")=="VaultDoor" && panel.at("iHackingTime")=="8" && panel.at("UsableBy")=="U_SPAWNOFFENSE","a hacking panel opens its door");
    Check(panel.at("fInteractionHeight")=="120" && panel.at("fInteractionRadius")=="60" && panel.at("UseRelativeLocation")==Json({{"X","39.5"},{"Y","3"},{"Z","0"}}) && panel.at("UseRelativeRotation").at("Yaw")=="32768","SInterrupteur 120/60, panel offset 39.5,3,0, facing back");
    const auto& button=Props(c,"HallDoor_Button");
    Check(button.at("UseRelativeLocation")==Json({{"X","55"},{"Y","0"},{"Z","0"}}) && button.at("UsableBy")=="U_BOTH" && button.at("iHackingTime")=="0","button offset 55,0,0");
    Check(button.at("Location")==Json({{"X","-112"},{"Y","382"},{"Z","100"}}) && button.at("Rotation").at("Yaw")=="49152","a door's button beside the opening, on the wall's open face, facing the room");
    Check(Props(c,"GuardDoor_Sensor").at("DetectedClass")=="Class'SBase.SPawnDefense'" && Props(c,"GuardDoor_Sensor").at("Event")=="GuardDoor","a presence trigger for mercs");
    Check(Props(c,"VaultLightSwitch").at("Event")=="VaultLight","a button fires its one target");
    Check(Props(c,"HallAlarm").at("MoversToLock")==Json::array({{{"ActionBeforeLock","ActionBeforeLock_None"},{"MoverToLock",{{"$ref","VaultDoor"}}}}}),"MoversToLock full structs");
    // Lifts.
    const auto e=Spec::Compile(Example("elevator-3floors"));
    const auto& lift=Props(e,"MainLift");
    Check(Create(e,"MainLift").at("class")=="SBase.SLift" && lift.at("NumKeys")=="3" && lift.at("KeyPos")[2].at("Z")=="768" && lift.at("KeyNum")=="0","an SLift key per storey");
    // scct://wiki/elevators: each storey's door and the switches that work while the car is there.
    const auto& floors=lift.at("DoorsAndSwitches");
    Check(floors.size()==3 && floors[0].at("Doors")==Json::array({{{"$ref","MainLift_Door_Ground"}}}) && floors[2].at("Doors")==Json::array({{{"$ref","MainLift_Door_Top"}}}),"DoorsAndSwitches: each storey's door");
    Check(floors[0].at("Switches")==Json::array({{{"$ref","MainLift_CarSwitch"}},{{"$ref","MainLift_Switch_First"}},{{"$ref","MainLift_Switch_Top"}}}) &&
          floors[1].at("Switches")==Json::array({{{"$ref","MainLift_CarSwitch"}},{{"$ref","MainLift_Switch_Ground"}},{{"$ref","MainLift_Switch_Top"}}}),"the car's switch and the other storeys' call switches");
    for(const char* id:{"MainLift_Switch_Ground","MainLift_Switch_First","MainLift_Switch_Top","MainLift_CarSwitch"})
        Check(Create(e,id).at("class")=="SBase.SInterrupteur" && Props(e,id).at("TriggerMethode")=="TriggerToggle" && Props(e,id).at("Event")=="MainLift","lift switches toggle the lift");
    Check(Props(e,"MainLift_Switch_Ground").at("bInitiallyUsable")=="False" && Props(e,"MainLift_Switch_First").at("bInitiallyUsable")=="True" && Props(e,"MainLift_CarSwitch").at("bInitiallyUsable")=="True",
          "usable at the start: the car's switch and the calls from the other storeys");
    Check(Props(e,"MainLift_CarSwitch").at("AttachTag")=="MainLift","the car's switch rides with the car");
    // External doors: Key 0 open, Key 1 closed; KeyNum 0 on the start storey, 1 elsewhere.
    const auto& ground=Props(e,"MainLift_Door_Ground");
    const auto& first=Props(e,"MainLift_Door_First");
    Check(Create(e,"MainLift_Door_Ground").at("class")=="Engine.Mover" && ground.at("StaticMesh")=="StaticMesh'ToweD_STM.Lift.Door160'" && ground.at("NumKeys")=="2","an external door is a Mover");
    Check(ground.at("KeyNum")=="0" && first.at("KeyNum")=="1" && Props(e,"MainLift_Door_Top").at("KeyNum")=="1","KeyNum 0 on the start storey, 1 on the others");
    Check(ground.at("KeyPos")[0]==Json({{"X","0"},{"Y","0"},{"Z","0"}}) && ground.at("KeyPos")[1]==Json({{"X","0"},{"Y","0"},{"Z","-192"}}) && ground.at("Location").at("Z")=="192" && first.at("Location").at("Z")=="576",
          "Key 0 open (Location, raised), Key 1 closed in the opening");
    Check(ground.at("Location").at("X")=="528" && ground.at("InitialState")=="TriggerControl","the door in the doorway onto the shaft");
    const auto& call=Props(e,"MainLift_Switch_Ground");
    Check(std::stod(call.at("Location").at("X").get<std::string>())<512 && call.at("Location").at("Z")=="100","a call switch outside the shaft, beside its door");
    Check(lift.at("LiftDoors")==Json::array({{{"$ref","MainLift_CarDoor1"}}}) && Create(e,"MainLift_CarDoor1").at("class")=="SBase.SLiftDoor" && Props(e,"MainLift_CarDoor1").at("KeyNum")=="0" &&
          std::stod(Props(e,"MainLift_CarDoor1").at("Location").at("X").get<std::string>())>544,"the car's SLiftDoor, open at the start storey, inside the shaft");
    auto doorless=Example("elevator-3floors");doorless["mechanisms"]["lifts"][0].erase("doorMesh");
    const auto d=Spec::Compile(doorless);
    Check(Diagnostic(d.diagnostics,"elevator.doors") && Diagnostic(d.diagnostics,"elevator.doors")->severity=="warning" && Count(d.diagnostics,"error")==0,"a lift without a door mesh is built doorless, with a warning");
    Check(Props(d,"MainLift").at("DoorsAndSwitches")[0].at("Doors")==Json::array() && !Find(Operations(d),"MainLift_Door_Ground") && !Props(d,"MainLift").contains("LiftDoors") && Find(Operations(d),"MainLift_Switch_Top"),"no doors, still switches");
    auto shut=Example("elevator-3floors");shut["geometry"]["doorways"].erase(1);
    const auto s=Spec::Compile(shut);
    Check(Diagnostic(s.diagnostics,"elevator.opening") && !Find(Operations(s),"MainLift_Door_First") && !Find(Operations(s),"MainLift_Switch_First"),"a storey with no opening onto the shaft gets no door or call switch");
    auto placed=shut;placed["mechanisms"]["lifts"][0]["doors"]=Json::parse(R"([{"storey":"First","at":[544,0,384],"yaw":90}])");
    const auto pl=Spec::Compile(placed);
    Check(Find(Operations(pl),"MainLift_Door_First") && Props(pl,"MainLift_Door_First").at("Rotation").at("Yaw")=="16384" && !Diagnostic(pl.diagnostics,"elevator.opening"),"doors[] places a storey's door");
    Check(Step(e.plan,"lift_ServiceLift").at("args").at("rise")==384 && Step(e.plan,"lift_ServiceLift").at("op")=="design.lift","a lift without a mesh is design.lift");
    Check(e.sidecar.at("actors").at("ServiceLift")==Json({{"$bind",{{"step","lift_ServiceLift"}}}}),"the sidecar binds the lift's identity");
    auto tall=Example("elevator-3floors");tall["mechanisms"]["lifts"][0].erase("mesh");
    Check(Diagnostic(Spec::Compile(tall).diagnostics,"elevator.mesh")->severity=="error","a three-storey lift needs a mesh");
    // Stairs: a stairwell cut to the flight's footprint, with steps a pawn climbs.
    const auto& items=Step(e.plan,"blocks1").at("args").at("items");
    bool stairs=false,well=false;
    for(const auto& item:items)
    {
        if(item.at("spec").at("kind")=="Stairs"){stairs=true;Check(item.at("spec").at("steps")==24 && item.at("spec").at("length")==768 && item.at("spec").at("height")==384,"stairs from storey to storey");}
        if(item.at("spec").at("name")=="Stairwell")well=true;
    }
    Check(stairs && well,"vertical stairs");
}
void Environment()
{
    const auto c=Spec::Compile(Example("security-camnet"));
    Check(Props(c,"HallLight").at("LightEffect")=="LE_ESpotShadow" && Props(c,"HallLight").at("EchelonNormalInfluence")=="0.9","Echelon lights");
    Check(Props(c,"LobbyLight").at("LightEffect")=="LE_Spotlight" && Create(c,"LobbyLight").at("class")=="SBase.STriggerLight" && Props(c,"LobbyLight").at("Rotation").at("Pitch")=="49152","a switchable spot points down");
    Check(Props(c,"AmmoStock").at("fInteractionHeight")=="60" && Props(c,"AmmoStock").at("fInteractionRadius")=="120","SAmmoStock 60/120");
    Check(Props(c,"Infirmary").at("bBlockActors")=="True" && Props(c,"Infirmary").at("bBlockPlayers")=="True" && Props(c,"Infirmary").at("fInteractionHeight")=="120" && Props(c,"Infirmary").at("fInteractionRadius")=="60","SInfirmary blocks, 120/60");
    const auto& volume=Create(c,"VaultName");
    Check(volume.at("geometry")=="polygons" && volume.at("polygons").size()==6 && volume.at("properties").at("LocationName")=="\"Vault floor\"","a sized volume is a polygon brush");
    double hx=0,hz=0;
    for(const auto& p:volume.at("polygons"))for(const auto& v:p.at("vertices")){hx=std::max(hx,v[0].get<double>());hz=std::max(hz,v[2].get<double>());}
    Check(hx==512 && hz==160,"its polygons span the box, not the 256 cube");
    const auto& sensor=Props(c,"VaultMotion");
    Check(sensor.at("Volumes")==Json::array({{{"$ref","VaultMotion_Volume"}}}) && Create(c,"VaultMotion_Volume").at("geometry")=="polygons","a motion sensor watches its own polygon volume");
    size_t outlined=0;
    for(const auto& piece:c.render.at("pieces"))
        if(piece.at("kind")=="volume" && (piece.at("id")=="VaultName" || piece.at("id")=="VaultMotion_Volume"))outlined+=piece.at("outline").size()==4 && piece.at("box").at("max")[0].get<double>()>piece.at("box").at("min")[0].get<double>();
    Check(outlined==2,"plan_render draws sized volumes as outlines");
    auto odd=Level(R"({"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[512,512,256]}]},"environment":{"lights":[{"id":"Disco","in":"Hall","props":{"LightEffect":"LE_Disco"}},{"id":"Sun","preset":"sun","at":{"in":"Hall"}}]}})");
    const auto o=Spec::Compile(odd);
    bool effect=false;
    for(const auto& d:o.diagnostics)effect=effect || (d.wikiPage=="lighting" && d.message.find("LE_Disco")!=std::string::npos);
    Check(effect && Props(o,"Sun").at("LightEffect")=="LE_Sunlight","static lights are LE_None, LE_Spotlight or LE_Sunlight");
    Check(!Find(Operations(o),"Hall_Light"),"a room with a light of its own gets no default light");
    // Events: links, magic events, messages, level events, trigger lights.
    const auto e=Spec::Compile(Example("elevator-3floors"));
    Check(Props(e,"RoofSensor").at("Event")=="UplinkWarning","a link sets the sender's Event to the receiver's Tag");
    const auto& group=Props(e,"UplinkWarning").at("Groups")[0].at("EventGroup");
    Check(group.size()==2 && group[0].at("Event")=="UplinkMessage" && group[0].at("Delay")=="1" && group[1].at("Event")=="RoofLight" && group[1].at("Delay")=="0.5","links from an SMagicEvent append its actions");
    Check(Props(e,"UplinkMessage").at("MessageSpy")=="\"You are near the uplink\"" && Props(e,"UplinkMessage").at("DisplayTime")=="5","SMessageTrigger");
    Check(Props(e,"RoofLight").at("bInitiallyOn")=="True" && Props(e,"RoofConsole").at("ObjectLights")==Json::array({{{"$ref","RoofLight"}}}) && Props(e,"RoofConsole").at("bPickable")=="False","STriggerLight on an SGamePlayObject");
    Check(Props(e,"OfficeGE").at("GEMask")=="2" && Props(e,"OfficeGE").at("bToggle")=="True","SGETrigger");
    Check(Props(e,"LobbyHum").at("AmbientSound")=="Sound'ToweD_Snd.Ambient.Hum'" && Props(e,"LobbyHum").at("SoundRadius")=="600","an AmbientSound");
    const auto& changes=Step(e.plan,"settings_edit").at("args").at("changes");
    Check(changes.at("Events")==Json::array({"UplinkMessage"}) && changes.at("SendAtTime")==Json::array({"-120"}) && changes.at("fDeathPawnHeight")=="-512" && changes.at("GEActivationMask")=="1","level events and properties");
    Check(Step(e.plan,"settings_edit").at("args").at("snapshot")==Json({{"$bind",{{"step","settings"}}}}) && Step(e.plan,"settings").at("op")=="settings.inspect","settings.edit takes settings.inspect's snapshot");
}
void GesAndSettings()
{
    const auto c=Spec::Compile(Example("diskhunt-arena"));
    const auto& ladder=Step(c.plan,"ge3").at("args");
    Check(ladder.at("command")=="LADDER" && ladder.at("points")[0][2]==0 && ladder.at("points")[1][2]==256 && ladder.at("points")[2][2]==0,"a ladder starts at its low end, its side point at that height");
    Check(Step(c.plan,"ge1").at("args").at("command")=="LG" && Step(c.plan,"ge1").at("op")=="ge.add","ledges are GE ADD LG");
    const auto& tweak=Step(c.plan,"settings_edit").at("args").at("changes").at("MotionTrackingTweak");
    Check(tweak==Json({{"MTVOffset","8"},{"MTVSlope","8"}}),"enhanced Motion Tracking Vision is 8/8");
    const auto& map=Step(c.plan,"mapsettings").at("args").at("changes");
    Check(Step(c.plan,"mapsettings").value("optional",false) && map.at("MapName")=="\"DISK ARENA\"" && map.at("Modes").size()==3 && map.at("ModesStrings").size()==3,"SMapSettings in post, optional");
    Check(map.at("Modes")[0].at("GameModeId")=="GM_DiskHunt" && map.at("Modes")[0].at("ScoreLimit")=="8" && map.at("Modes")[0].at("NumDisksMax")=="12" && map.at("ModesStrings")[0].at("LegendModeInBriefSpy").get<std::string>().find("collect")!=std::string::npos,"Modes and ModesStrings index-aligned");
    const auto& post=c.plan.at("post");
    Check(post[0].at("op")=="design.build" && post[1].at("op")=="mapcheck.run" && post.back().at("op")=="view.screenshot","post: build, check, ..., screenshot");
    Spec::CompileOptions quick;quick.build=false;quick.screenshot=false;quick.save="Packages\\MapsEd\\DiskD.sdc";
    const auto q=Spec::Compile(Example("diskhunt-arena"),quick);
    Check(!HasStep(q.plan,"build") && !HasStep(q.plan,"screenshot") && Step(q.plan,"save").at("args").at("path")=="Packages\\MapsEd\\DiskD.sdc","options decide the post steps");
    auto metres=Level(R"({"units":{"unit":"m"},"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[5.12,5.12,2.56],"at":[1,0]}]}})");
    const auto metric=Spec::Compile(metres);
    const auto& item=Step(metric.plan,"blocks1").at("args").at("items")[0];
    Check(std::abs(item.at("spec").at("width").get<double>()-512)<1e-9 && std::abs(item.at("position")[0].get<double>()-100)<1e-9,"lengths in metres become Unreal units");
}
void Geometry()
{
    // Attach on a turned host: a corridor running east-west, a room off its west end.
    auto spec=Level(R"({"geometry":{"pieces":[
        {"id":"Hub","kind":"Room","size":[512,512,256],"at":[0,0]},
        {"id":"Run","kind":"Corridor","size":[192,640,256],"attach":{"to":"Hub","side":"west","offset":64}},
        {"id":"Far","kind":"Room","size":[384,384,256],"attach":{"to":"Run","side":"west"}},
        {"id":"Side","kind":"Room","size":[256,256,256],"attach":{"to":"Hub","side":"north","offset":-100}}],
        "doorways":[{"id":"D1","host":"Hub","side":"north","offset":-100,"size":[96,128]},{"id":"D2","between":["Hub","Side"],"size":[64,128],"portal":false}]}})");
    const auto c=Spec::Compile(spec);
    for(const auto& d:c.diagnostics)Check(!(d.severity=="error" && d.rule.rfind("geometry",0)==0),"attached pieces compile: "+d.message);
    const auto& items=Step(c.plan,"blocks1").at("args").at("items");
    Check(items[1].at("position")==Json({-256-16-320,64,0}) && items[1].at("rotation")==Json({0,16384,0}),"west of the hub, offset north");
    Check(items[2].at("position")==Json({-592-320-16-192,64,0}),"off the corridor's west end");
    Check(items[3].at("position")==Json({-100,256+16+128,0}) && items[3].at("rotation")==Json({0,0,0}),"north of the hub, offset west");
    Check(items[4].at("spec").at("kind")=="Doorway" && items[4].at("position")==Json({-100,256+8,0}) && items[4].at("spec").at("portal")==true,"a doorway on a compass wall, a portal by default");
    Check(items[5].at("position")[0]==-100 && items[5].at("spec").at("length")==16 && items[5].at("spec").at("portal")==false,"a doorway between two pieces finds their shared wall");
    Reject([&]{auto bad=spec;bad["geometry"]["pieces"][1]["attach"]["to"]="Nowhere";Spec::Compile(bad);});
    try{auto bad=spec;bad["geometry"]["pieces"][1]["attach"]["to"]="Nowhere";Spec::Compile(bad);}
    catch(const Spec::SpecError& e){Check(e.pointer=="/geometry/pieces/1/attach/to","attach names a missing piece at its pointer");}
    auto turned=spec;turned["geometry"]["pieces"][0]["yaw"]=30;
    const auto t=Spec::Compile(turned);
    bool compass=false;
    for(const auto& d:t.diagnostics)compass=compass || (d.severity=="error" && d.pointer=="/geometry/doorways/0");
    Check(compass,"a doorway on a host turned off the compass is an error at its pointer");
    auto wide=spec;wide["geometry"]["doorways"][1]["size"]={512,128};
    Check(Diagnostic(Spec::Compile(wide).diagnostics,"geometry.doorway")->pointer=="/geometry/doorways/1/between","a doorway wider than the shared wall");
    try{Spec::Compile(Level(R"({"spawns":{"spy":[{"id":"S","at":["x",0]}]}})"));Check(false,"text x accepted");}
    catch(const Spec::SpecError& e){Check(e.pointer=="/spawns/spy/0/at/0","x and y are numbers: "+e.pointer);}
    // Zones decide portals: a doorway inside one zone has none.
    auto zoned=spec;zoned["zones"]=Json::parse(R"([{"id":"Z1","spaces":["Hub","Run","Far"]},{"id":"Z2","spaces":["Side"]}])");
    zoned["geometry"]["doorways"][1].erase("portal");
    const auto z=Spec::Compile(zoned);
    const auto& zi=Step(z.plan,"blocks1").at("args").at("items");
    Check(zi[4].at("spec").at("portal")==true && zi[5].at("spec").at("portal")==true,"doorways into another zone are portals");
    auto same=zoned;same["zones"][0]["spaces"].push_back("Side");same["zones"].erase(1);
    const auto s=Spec::Compile(same);
    Check(Step(s.plan,"blocks1").at("args").at("items")[5].at("spec").at("portal")==false,"a doorway inside one zone is not a portal");
    // Polygon brushes instead of Map Design pieces.
    Spec::CompileOptions polygons;polygons.brushMode="polygons";
    auto painted=Example("versus-starter");painted["geometry"]["surfaces"]=Json::parse(R"([{"brush":"ObjectiveRoom","faces":["floor"],"texture":"Pkg.Floors.Tile"}])");
    const auto p=Spec::Compile(painted,polygons);
    Check(!HasStep(p.plan,"blocks1") && !HasStep(p.plan,"register"),"no blockouts in polygons mode");
    const auto& room=Create(p,"ObjectiveRoom_Brush0");
    Check(room.at("class")=="Engine.Brush" && room.at("geometry")=="polygons" && room.at("polygons").size()==6 && room.at("properties").at("CsgOper")=="CSG_Subtract","a carved room as a subtractive polygon brush");
    size_t tiled=0;
    for(const auto& polygon:room.at("polygons"))
        if(polygon.value("texture",std::string())=="Pkg.Floors.Tile"){++tiled;for(const auto& v:polygon.at("vertices"))Check(v[2]==0,"the floor face is textured");}
    Check(tiled==1,"one floor face");
    Check(Props(p,"WestCorridor_Portal_Brush1").at("PolyFlags")==std::to_string(Design::kPortalPolyFlags),"portal sheets keep the portal flags");
    ValidDocuments(p.plan);
    Plan::Check(p.plan);
    // In pieces mode the faces are textured after the build, on the piece's brushes.
    const auto m=Spec::Compile(painted);
    const auto& surface=Step(m.plan,"surface1");
    Check(surface.at("op")=="surface.texture" && surface.value("optional",false) && surface.at("args").at("faces")==Json::array({"floor"}) && surface.at("args").at("texture")=="Pkg.Floors.Tile" &&
          surface.at("args").at("brushes")==Json({{"$bind",{{"step","blocks1"},{"ptr","/0/members"}}}}),"surface.texture binds the piece's brushes");
    Check(Step(m.plan,"packages").at("args").at("document").at("operations")[0]==Json({{"op","load"},{"package","Pkg"}}),"the texture's package loads first");
    const auto ran=Execute(m.plan);
    Check(Plan::Bind(surface.at("args"),ran.results).at("brushes")==ran.results.at("blocks1")[0].at("members"),"it resolves to the brushes the build made");
}
// The sections the examples use least: raw brushes and meshes, generic movers
// and actors, interface textures, release notes, raw operations, provenance.
void Rest()
{
    auto spec=Level(R"({"defaults":{"material":"Greybox.Walls.Grey","lights":"none"},
        "geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[1024,1024,320]}],
            "brushes":[{"id":"Pillar","csg":"CSG_Add","polyFlags":32,"at":[0,0,0],"polygons":[
                {"texture":"Greybox.Trim.Dark","vertices":[[-32,-32,0],[-32,32,0],[32,32,0],[32,-32,0]]},
                {"vertices":[[-32,-32,0],[32,-32,0],[0,0,256]]},{"vertices":[[32,-32,0],[32,32,0],[0,0,256]]},
                {"vertices":[[32,32,0],[-32,32,0],[0,0,256]]},{"vertices":[[-32,32,0],[-32,-32,0],[0,0,256]]}]}],
            "meshes":[{"id":"Crate","mesh":"StaticMesh'Crates.Wood.Crate'","at":{"in":"Hall","u":0.3},"yaw":45,"scale":[1,2,1]}]},
        "mechanisms":{"movers":[{"id":"Fan","class":"Engine.Mover","mesh":"Fans.Blades.Fan","at":{"in":"Hall","h":"ceiling-40"},"keys":[{"yaw":180},{"yaw":360}],"initialState":"TriggerToggle","moveTime":2}]},
        "events":{"shakers":[{"id":"Quake","at":{"in":"Hall"},"properties":{"ShakeEvents":[]}}],"noises":[{"id":"Hum","at":{"in":"Hall"},"properties":{"fIntensity":0.5}}]},
        "environment":{"fish":[{"id":"Fish1","at":{"in":"Hall"}}],"animated":[{"id":"Flag1","class":"SBase.SAnimatedMesh","at":{"in":"Hall"}}],"tutorials":[{"id":"Tip1","at":{"in":"Hall"}}],
            "softbody":[{"id":"Chain1","class":"ESBChainActor","mesh":"Ropes.Link","at":{"in":"Hall"}},{"id":"Chain2","class":"ESBChainActor","mesh":"Ropes.Link","at":{"in":"Hall"}},{"id":"Curtain","at":{"in":"Hall","u":0.2}}]},
        "interface":{"menu":"art/menu.dds","loadingDef":"art/def.dds"},
        "release":{"packages":["RestD_TXT"]},
        "extra":{"operations":[{"op":"load","package":"Extra"},{"op":"create","id":"Raw1","class":"Engine.Note","properties":{"Location":{"X":"0","Y":"0","Z":"64"}}}]},
        "provenance":{"source":"test","notes":"kept with the spec"}})");
    const auto c=Spec::Compile(spec);
    const auto& item=Step(c.plan,"blocks1").at("args").at("items")[0];
    Check(item.at("spec").at("material")=="Greybox.Walls.Grey" && item.at("spec").at("construction")=="Carve","defaults reach the pieces");
    std::set<std::string> loads;
    for(const auto& op:Step(c.plan,"packages").at("args").at("document").at("operations"))loads.insert(op.at("package").get<std::string>());
    Check(loads==std::set<std::string>{"Greybox","Crates","Fans","Ropes","Extra"},"every package the spec names loads in pre: "+Json(loads).dump());
    const auto& pillar=Create(c,"Pillar");
    Check(pillar.at("class")=="Engine.Brush" && pillar.at("geometry")=="polygons" && pillar.at("polygons").size()==5 && pillar.at("properties").at("CsgOper")=="CSG_Add" && pillar.at("properties").at("PolyFlags")=="32","a raw polygon brush");
    Check(Props(c,"Crate").at("DrawScale3D")==Json({{"X","1"},{"Y","2"},{"Z","1"}}) && Props(c,"Crate").at("Rotation").at("Yaw")=="8192" && Create(c,"Crate").at("class")=="Engine.StaticMeshActor","a mesh");
    const auto& fan=Props(c,"Fan");
    Check(fan.at("NumKeys")=="3" && fan.at("KeyRot")[1].at("Yaw")=="32768" && fan.at("KeyRot")[2].at("Yaw")=="0" && fan.at("InitialState")=="TriggerToggle" && fan.at("Location").at("Z")=="280","a mover with its keys");
    Check(Create(c,"Quake").at("class")=="SBase.SShaker" && Create(c,"Hum").at("class")=="SBase.SNotifyNoise" && Props(c,"Hum").at("fIntensity")=="0.5","shakers and noises");
    Check(Create(c,"Fish1").at("class")=="SBase.SFish" && Create(c,"Tip1").at("class")=="SBase.SZoneTips" && Create(c,"Flag1").at("class")=="SBase.SAnimatedMesh","generic environment actors");
    Check(Props(c,"Tip1").at("bHidden")=="True" && Props(c,"Tip1").at("DrawType")=="DT_Sprite" && Props(c,"Tip1").at("bShowOnlyWithIngameHelp")=="True" && Props(c,"Tip1").at("bStayPermanently")=="False","tutorial tips as the wiki sets them");
    Check(Diagnostic(c.diagnostics,"softbody.duplicate").has_value() && Diagnostic(c.diagnostics,"softbody.duplicate")->pointer=="/environment/softbody/1","a SoftBody repeated exactly is a warning");
    Check(Create(c,"Curtain").at("class")=="Echelon.ESBPatchActor" && Create(c,"Chain1").at("class")=="Echelon.ESBChainActor","SoftBody: ESBPatchActor by default, short classes completed");
    Check(Props(c,"Chain1").at("LinkMesh")=="StaticMesh'Ropes.Link'" && !Props(c,"Chain1").contains("DrawType"),"a SoftBody's mesh is a chain's links; it keeps DT_SoftBody");
    auto base=spec;base["environment"]["softbody"]=Json::parse(R"([{"id":"Cloth","class":"Echelon.ESoftBodyActor","at":{"in":"Hall"}}])");
    const auto b=Spec::Compile(base);
    Check(Diagnostic(b.diagnostics,"softbody.class") && Diagnostic(b.diagnostics,"softbody.class")->severity=="error" && !Find(Operations(b),"Cloth"),"the SoftBody base class cannot be placed");
    auto animated=spec;animated["environment"]["animated"][0]["mesh"]="Anims.Flag";animated["environment"]["fish"][0]["mesh"]="SkeletalMesh'Fish.Shark'";
    const auto an=Spec::Compile(animated);
    Check(Props(an,"Flag1").at("Mesh")=="Mesh'Anims.Flag'" && Props(an,"Flag1").at("DrawType")=="DT_Mesh" && Props(an,"Fish1").at("Mesh")=="SkeletalMesh'Fish.Shark'" && !Props(an,"Flag1").contains("StaticMesh"),"animated meshes and fish draw a Mesh");
    // Polygon brushes and sized volumes show in plan_render as their outlines.
    const Json* pillarPiece=nullptr;
    for(const auto& piece:c.render.at("pieces"))if(piece.at("id")=="Pillar")pillarPiece=&piece;
    Check(pillarPiece && pillarPiece->at("kind")=="add" && pillarPiece->at("outline").size()==4 && pillarPiece->at("box").at("max")[2]==256,"geometry.brushes in the render scene");
    Check(!Find(Operations(c),"Hall_Light"),"lights: none places no room lights");
    Check(Find(Operations(c),"Raw1") && Step(c.plan,"interface1").at("args")==Json({{"slot","Menu"},{"file","art/menu.dds"}}) && Step(c.plan,"interface2").at("args").at("slot")=="LoadingDEF0" && Step(c.plan,"interface1").value("optional",false),"raw operations and interface textures");
    Check(c.scene.at("release").at("packages")==Json::array({"RestD_TXT"}) && c.scene.at("release").at("interface").at("menu")=="art/menu.dds","release notes reach the rules");
    Check(c.sidecar.at("spec").at("provenance").at("source")=="test","provenance is kept with the spec");
    ValidDocuments(c.plan);
    auto clash=spec;clash["extra"]["operations"][1]["id"]="Crate";
    Check(Diagnostic(Spec::Compile(clash).diagnostics,"extra.id").has_value(),"a raw create may not reuse a spec id");
    auto deleting=spec;deleting["extra"]["operations"].push_back({{"op","delete"},{"actor",{{"path","MyLevel.X"},{"class","Engine.Note"}}},{"before",{{"A","1"}}}});
    Check(Diagnostic(Spec::Compile(deleting).diagnostics,"extra.delete").has_value(),"deletes come from the spec, not raw operations");
}
// Builds again from what the last build stored: only what changed changes.
void Incremental()
{
    const auto first=Spec::Compile(Example("versus-starter"));
    const auto stored=Execute(first.plan).sidecar;
    Spec::CompileOptions again;again.sidecar=stored;
    // The same spec again changes nothing but the sidecar.
    const auto same=Spec::Compile(Example("versus-starter"),again);
    Check(same.plan.at("steps").size()==1 && same.plan.at("steps")[0].at("op")=="mcp.sidecar.set" && same.summary.at("kept")==same.summary.at("pieces").get<size_t>()+same.summary.at("actors").get<size_t>(),"an unchanged spec keeps everything");
    // A fragment merged over the stored spec: a new ammo box, a longer alarm,
    // the laser taken out, the merc spawn resized.
    auto fragment=Json::parse(R"({"environment":{"ingredients":[{"id":"SpyAmmo","kind":"ammo","in":"SpySpawn"}]},
        "security":{"alarms":[{"id":"ServerAlarm","duration":45}],"devices":[{"id":"CorridorLaser","$remove":true}]},
        "geometry":{"pieces":[{"id":"MercSpawn","size":[896,768,256]}]}})");
    const auto merged=Spec::Compile(fragment,again);
    Check(Count(merged.diagnostics,"error")==0,"the merge compiles:"+Errors(merged));
    Plan::Check(merged.plan);
    const auto& deletes=Operations(merged,"deletes");
    std::set<std::string> gone;
    for(const auto& d:deletes){gone.insert(d.at("actor").at("path").get<std::string>());Check(Plan::IsBind(d.at("before")) && d.at("before").at("$bind").at("ptr")=="/actors/0/values","a delete carries the live values");}
    Check(gone.count("MyLevel.CorridorLaser") && gone.count("MyLevel.SecurityOff"),"what the spec no longer has is deleted");
    Check(Step(merged.plan,"deletes").at("args").at("document").at("version")==3 && Step(merged.plan,"deletes").at("args").at("document").at("allowDeletes")==true &&
          Step(merged.plan,"deletes").at("args").at("document").at("map")==Json({{"$bind",{{"step","mapid"},{"ptr","/map"}}}}),"deletes are a v3 document naming the open map");
    const auto& replaced=Step(merged.plan,"blocks_replaced").at("args").at("items");
    Check(replaced.size()==1 && replaced[0].at("spec").at("width")==896 && replaced[0].at("previous")==stored.at("pieces").at("MercSpawn"),"a changed piece replaces its previous record");
    const auto& ops=Operations(merged);
    const auto* ammo=Find(ops,"SpyAmmo");
    Check(ammo && ammo->at("op")=="create","the new actor is created");
    Check(!Find(ops,"SpyStart1") && !Find(ops,"ServerTerminal"),"kept actors are not created again");
    const Json* alarm=nullptr;
    for(const auto& op:ops)if(op.at("op")=="update" && op.at("actor").at("path")=="MyLevel.ServerAlarm")alarm=&op;
    Check(alarm && alarm->at("properties").at("Duration")=="45" && Plan::IsBind(alarm->at("before")),"a changed actor is updated against its live values");
    const auto& deathmatch=*Find(ops,"Deathmatch");
    Check(deathmatch.at("op")=="create" && !deathmatch.at("properties").contains("AtResetEvent") && gone.count("MyLevel.Deathmatch"),"an actor that loses a property is deleted and made again");
    const auto& unregister=Step(merged.plan,"unregister").at("args").at("members");
    Check(unregister==stored.at("pieces").at("MercSpawn").at("members"),"the old piece record leaves the library");
    Check(Step(merged.plan,"register").at("args").at("pieces")==Json::array({{{"$bind",{{"step","blocks_replaced"}}}}}),"the new record joins it");
    // Steps run in the order McpPlanModel requires: reads, deletes, replacements, then the rest.
    const auto& steps=merged.plan.at("steps");
    size_t deletesAt=0,replacedAt=0,actorsAt=0;
    for(size_t i=0;i<steps.size();++i){if(steps[i].at("id")=="deletes")deletesAt=i;if(steps[i].at("id")=="blocks_replaced")replacedAt=i;if(steps[i].at("id")=="actors")actorsAt=i;}
    Check(deletesAt<replacedAt && replacedAt<actorsAt,"deletes, then replacements, then the actors");
    const auto run=Execute(merged.plan);
    Check(run.sidecar.at("actors").at("SpyStart1")==stored.at("actors").at("SpyStart1") && run.sidecar.at("actors").at("SpyAmmo").at("path")=="MyLevel.SpyAmmo","the sidecar keeps kept identities and records new ones");
    Check(run.sidecar.at("pieces").at("ObjectiveRoom")==stored.at("pieces").at("ObjectiveRoom") && run.sidecar.at("pieces").at("MercSpawn")!=stored.at("pieces").at("MercSpawn"),"and the replaced piece's new record");
    Check(!run.sidecar.at("actors").contains("CorridorLaser") && run.sidecar.at("spec").at("security").at("devices").empty(),"the removed id is gone from the sidecar");
    for(const auto& step:merged.plan.at("steps"))
        if(step.at("op")=="authoring.apply")
        {
            auto document=Plan::Bind(step.at("args").at("document"),run.results);
            Workflow::Authoring::Validate(document,"c:\\maps\\test.sdc");
        }
    // Kept actors' references reach recreated ones by $ref again; others by path.
    auto retarget=Json::parse(R"({"security":{"camnet":[{"id":"CamWest","camName":"West end"}]}})");
    const auto r=Spec::Compile(retarget,again);
    const Json* camWest=nullptr;
    for(const auto& op:Operations(r))if(op.at("op")=="update" && op.at("actor").at("path")=="MyLevel.CamWest")camWest=&op;
    Check(camWest && camWest->at("properties")==Json({{"CamName","\"West end\""}}) && !Find(Operations(r),"CamWest"),"a renamed camera is updated in place, with only what changed");
    Check(!HasStep(r.plan,"deletes") && !HasStep(r.plan,"blocks1"),"nothing else is touched");
    // replace: everything the sidecar lists goes, and the spec is built afresh.
    Spec::CompileOptions replace=again;replace.sync="replace";
    const auto fresh=Spec::Compile(Example("versus-starter"),replace);
    Check(Operations(fresh,"deletes").size()>=first.summary.at("actors").get<size_t>() && Step(fresh.plan,"blocks_replaced").at("args").at("items").size()==11 &&
          Step(fresh.plan,"blocks_replaced").at("args").at("items")[0].at("previous")==stored.at("pieces").at("ObjectiveRoom"),"replace deletes every actor and rebuilds every piece in place");
    Plan::Check(fresh.plan);
    // incremental: the spec given is the whole level, so what it leaves out goes.
    Spec::CompileOptions whole=again;whole.sync="incremental";
    auto smaller=Example("versus-starter");smaller["environment"]["ingredients"].erase(0);
    const auto w=Spec::Compile(smaller,whole);
    bool ammoGone=false;
    for(const auto& d:Operations(w,"deletes"))ammoGone=ammoGone || d.at("actor").at("path")=="MyLevel.AmmoStock";
    Check(ammoGone,"incremental deletes what the spec leaves out");
    // A sidecar that is not ours is reported and ignored.
    Spec::CompileOptions wrong;wrong.sidecar={{"format","other"}};
    Check(Diagnostic(Spec::Compile(Example("versus-starter"),wrong).diagnostics,"incremental.sidecar").has_value(),"a foreign sidecar is ignored with a warning");
}
void Pointers()
{
    auto spec=Level(R"({"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[512,512,256]}]},"events":{"links":[{"from":"Missing","to":"Hall2"}]},
        "gameplay":{"playerCounts":[{"objective":"Nobody"}]}})");
    const auto c=Spec::Compile(spec);
    Check(Diagnostic(c.diagnostics,"ref.unresolved")->pointer=="/events/links/0/from" && Diagnostic(c.diagnostics,"ref.unresolved")->wikiPage=="events","an unresolved id is reported at its pointer");
    Check(Diagnostic(c.diagnostics,"players.subject")->pointer=="/gameplay/playerCounts/0/objective","player counts name a spec actor");
    try{Spec::Compile(Level(R"({"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[512,512,256],"colour":1}]}})"));Check(false,"unknown key accepted");}
    catch(const Spec::SpecError& e){Check(e.pointer=="/geometry/pieces/0/colour","Compile refuses a bad shape with its pointer");}
    Spec::CompileOptions bad;bad.sync="sometimes";
    Reject([&]{Spec::Compile(Level("{}"),bad);});
    // Existing actors: referenced by path, sent events by Tag, edited against their live values.
    auto live=Level(R"({"existing":{"refs":{"OldDoor":{"path":"MyLevel.Mover7","class":"Engine.Mover","tag":"OldDoorTag"},"OldLamp":{"tag":"LampTag"}}},
        "geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[512,512,256]}]},
        "security":{"devices":[{"id":"L","kind":"laser","in":"Hall","alarm":"A"}],"alarms":[{"id":"A","outputs":["OldLamp"],"lockMovers":[{"mover":"OldDoor"}]}]},
        "events":{"magic":[{"id":"Seq","document":{"format":"scct.smagic-event","version":1,"properties":{"Groups":[]}}}],"links":[{"from":"OldDoor","to":"Seq"}]},
        "environment":{"vision":[{"target":"OldDoor","heatIntensity":0.5}]}})");
    const auto l=Spec::Compile(live);
    for(const auto& d:l.diagnostics)Check(!(d.severity=="error" && (d.rule.rfind("ref",0)==0 || d.rule.rfind("events",0)==0 || d.rule.rfind("alarm",0)==0)),"existing refs resolve: "+d.message);
    Check(Props(l,"A").at("Events")==Json::array({"LampTag"}) && Props(l,"A").at("MoversToLock")[0].at("MoverToLock")==Json({{"$ref","MyLevel.Mover7"}}),"live actors by Tag and by path");
    const Json* link=nullptr;const Json* update=nullptr;
    for(const auto& op:Operations(l)){if(op.at("op")=="link")link=&op;if(op.at("op")=="update")update=&op;}
    Check(link && link->at("event")=="Seq" && link->at("target")=="MyLevel.Mover7" && link->at("trigger")==true,"a live actor linked to a spec SMagicEvent");
    Check(Step(l.plan,"actors").at("args").at("document").at("expect").contains("MyLevel.Mover7"),"the link expects the live actor's values");
    Check(update && update->at("properties").at("HeatIntensity")=="0.5" && Plan::IsBind(update->at("before")),"vision on a live actor is an update");
    Plan::Check(l.plan);
    // Built again unchanged, the link and the edit are not repeated.
    Spec::CompileOptions again;again.sidecar=Execute(l.plan).sidecar;
    const auto twice=Spec::Compile(live,again);
    Check(!HasStep(twice.plan,"actors") && !HasStep(twice.plan,"q1"),"a link to a live actor is made once");
}
// The starter's plan and plan_render scene, byte for byte: a change to either is
// a change to what level_build does, so it shows in review. "--write-golden"
// rewrites them.
void Golden(bool write)
{
    const std::filesystem::path folder="tests/mcp/golden";
    const auto c=Spec::Compile(Example("versus-starter"));
    for(const auto& [name,value]:std::vector<std::pair<std::string,Json>>{{"versus-starter.plan.json",c.plan},{"versus-starter.render.json",c.render}})
    {
        if(write)
        {
            std::filesystem::create_directories(folder);
            std::ofstream(folder/name)<<value.dump(1)<<"\n";
        }
        std::ifstream file(folder/name);
        Check(file.good(),"golden file "+name+" (run with --write-golden)");
        Check(Json::parse(file)==value,"the starter's "+name+" matches tests/mcp/golden (rerun with --write-golden after a deliberate change)");
    }
}
int main(int argc,char** argv)
{
    try
    {
        Golden(argc>1 && std::string(argv[1])=="--write-golden");
        Starter();
        Examples();
        Quoting();
        Schema();
        CamNet();
        Sectors();
        DiskHuntAndDeathmatch();
        Story();
        Spawns();
        Mechanisms();
        Environment();
        GesAndSettings();
        Geometry();
        Rest();
        Incremental();
        Pointers();
        std::cout<<"LevelSpecCompilerTests passed\n";
        return 0;
    }
    catch(const std::exception& e)
    {
        std::cerr<<"LevelSpecCompilerTests failed: "<<e.what()<<"\n";
        return 1;
    }
}
