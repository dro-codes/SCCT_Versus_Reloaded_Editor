// The level-spec tools and the intent tools (Reloaded.Editor.Mcp/Tools/
// SpecTools.cpp and IntentTools.cpp) against a FakeBridge that runs mcp.plan
// the way McpOps.cpp does: every plan checked with Plan::Check, binds resolved
// against stand-in results, optional steps allowed to fail, and the sidecar
// kept, so a build and the intent tools after it see what an editor would
// show them. map_validate (ValidateTools.cpp) shares the compiler for its
// offline path, so its output schema and offline fixes are checked here too.
// Run from the repository root:
//   g++ -std=c++20 -Wall -Wextra -Wshadow -Werror -O1 tests/McpSpecToolsTests.cpp Reloaded.Editor.Mcp/Tools/SpecTools.cpp
//       Reloaded.Editor.Mcp/Tools/IntentTools.cpp Reloaded.Editor.Mcp/Tools/ValidateTools.cpp Reloaded.Editor/WorkflowModel.cpp
//       Reloaded.Editor/RecoveredPolygonImport.cpp -pthread
#include "../Reloaded.Editor.Mcp/FakeBridge.h"
#include "../Reloaded.Editor.Mcp/McpProtocolModel.h"
#include "../Reloaded.Editor.Mcp/Tools/Common.h"
#include "../Reloaded.Editor.Mcp/LevelSpecModel.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
using namespace Mcp;
using Workflow::Json;
void Check(bool value,const std::string& message,std::source_location where=std::source_location::current())
{
    if(!value)throw std::runtime_error(message+" (line "+std::to_string(where.line())+")");
}
const std::filesystem::path kRoot=std::filesystem::path(__FILE__).parent_path().parent_path();
const std::vector<std::string> kExamples{"versus-starter","three-modes-sectors","diskhunt-arena","security-camnet","elevator-3floors"};
Json Example(const std::string& name)
{
    std::ifstream file(kRoot/"Reloaded.Editor.Mcp"/"Resources"/"examples"/(name+".level.json"));
    if(!file)throw std::runtime_error("cannot open example "+name+"; run from the repository root");
    return Json::parse(file);
}
Json Level(const char* text){auto s=Json::parse(text);s["format"]="scct.level-spec";s["version"]=1;return s;}

// An editor good enough for plans: it answers each op the compiler emits with
// the shape the real op returns, keeps the actors it made and the sidecar.
struct Editor
{
    FakeBridge fake;
    Json state{{"open",true},{"level",7},{"generation",2},{"revision",10},{"contentRevision",10},{"mapFile",""},{"dirty",false},{"map","unsaved:1"}};
    Json sidecar,ges=Json::array();
    std::vector<Json> plans;std::vector<bool> dry;
    std::set<std::string> live;
    int brushes=0,lifts=0;
    bool mapSettingsWork=false;
    Editor()
    {
        fake.On("editor.state",[this](const Json&,const CallOptions&){return state;});
        fake.On("mcp.sidecar.get",[this](const Json&,const CallOptions&){return Json{{"sidecar",sidecar}};});
        fake.On("authoring.schema",Json{{"classes",Json::object()}});
        fake.On("actors.query",[this](const Json& args,const CallOptions&){return Query(args);});
        fake.On("ge.list",[this](const Json&,const CallOptions&){return Json{{"elements",ges},{"note","Only the geometric events Map Design or the bridge added are listed."}};});
        fake.On("mcp.plan",[this](const Json& args,const CallOptions& options){return Run(args,options.dryRun || args.value("dryRun",false));});
    }
    Json Query(const Json& args)
    {
        Json actors=Json::array();
        for(const auto& name:args.value("names",Json::array()))
            if(live.count(Workflow::Fold(name.get<std::string>())))actors.push_back({{"path",name},{"class","Engine.Actor"},{"values",{{"Tag","\"Bound\""}}}});
        return {{"actors",actors},{"total",actors.size()}};
    }
    Json Result(const std::string& op,const Json& args)
    {
        if(op=="design.blockbatch")
        {
            Json out=Json::array();
            for(const auto& item:args.at("items"))
            {
                Json members=Json::array();
                for(int k=0;k<2;++k){const auto path="MyLevel.Brush"+std::to_string(++brushes);members.push_back({{"path",path},{"class","Engine.Brush"}});live.insert(Workflow::Fold(path));}
                out.push_back({{"map","c:\\maps\\test.sdc"},{"spec",item.at("spec")},{"position",item.at("position")},{"rotation",item.at("rotation")},{"members",members}});
            }
            return out;
        }
        if(op=="authoring.apply" || op=="authoring.preview")
        {
            Json identities=Json::object();
            for(const auto& o:args.at("document").at("operations"))
            {
                if(o.at("op")=="create")
                {
                    const auto path="MyLevel."+o.at("id").get<std::string>();
                    identities[o.at("id").get<std::string>()]={{"path",path},{"class",o.at("class")}};
                    if(op=="authoring.apply")live.insert(Workflow::Fold(path));
                }
                if(o.at("op")=="delete" && op=="authoring.apply")live.erase(Workflow::Fold(o.at("actor").at("path").get<std::string>()));
            }
            return {{"identities",identities},{"rows",Json::array()}};
        }
        if(op=="design.lift"){const auto path="MyLevel.Design_Lift_"+std::to_string(++lifts);live.insert(Workflow::Fold(path));return {{"path",path},{"class","SBase.SLift"}};}
        if(op=="settings.inspect")return {{"actor",{{"path","MyLevel.LevelInfo0"},{"class","Engine.LevelInfo"}}},{"values",Json::object()}};
        if(op=="settings.edit")return {{"actor",{{"path","MyLevel.LevelInfo0"},{"class","Engine.LevelInfo"}}},{"values",args.at("changes")}};
        if(op=="actors.query")
        {
            const auto name=args.at("names")[0].get<std::string>();
            if(!live.count(Workflow::Fold(name)))return {{"actors",Json::array()},{"total",0}};
            return {{"actors",Json::array({{{"path",name},{"values",{{"Tag","\"Bound\""}}}}})},{"total",1}};
        }
        if(op=="authoring.mapid")return {{"map","c:\\maps\\test.sdc"}};
        if(op=="mcp.sidecar.set"){sidecar=args.at("sidecar");return true;}
        if(op=="ge.add"){ges.push_back(args);return {{"command",args.at("command")}};}
        if(op=="design.build")return {{"built",true}};
        if(op=="build.all")return {{"ok",true},{"log",Json::array()}};
        if(op=="mapcheck.run")return {{"rows",Json::array({{{"severity","warning"},{"group","Zones"},{"check","Leak"},{"message","Zone 2 leaks."},{"actors",Json::array()},{"surfaces",Json::array()}}})},
                                      {"summary",{{"errors",0},{"warnings",1},{"info",0},{"bspBuilt",true}}}};
        if(op=="map.save")return {{"file",args.at("path")}};
        if(op=="view.screenshot")return {{"png","iVBORw0KGgo="},{"mimeType","image/png"},{"width",1024},{"height",768},{"viewport","top"}};
        if(op=="mapsettings.set")
        {
            if(!mapSettingsWork)throw Bridge::Error(Bridge::Code::Refused,"mapsettings.set cannot write SMapSettings yet. Do it in the editor: 1. Save. 2. Map Settings.","unsupported");
            return {{"written",true}};
        }
        if(op=="interface.texture")return {{"slot",args.at("slot")},{"file",args.at("file")},{"size",256}};
        if(op=="minimap.apply")return {{"floors",Json::array({{{"texture","Minimap0"}}})},{"unused",Json::array()}};
        return true;
    }
    // McpOps.cpp's mcp.plan: pre, steps (one Undo step), post; a dry run
    // runs only the reads and previews the rest.
    Json Run(const Json& plan,bool dryRun)
    {
        Plan::Check(plan);
        plans.push_back(plan);dry.push_back(dryRun);
        std::map<std::string,Json> results;Json errors=Json::object(),preview=Json::array();
        for(const char* section:{"pre","steps","post"})
        {
            if(!plan.contains(section))continue;
            for(const auto& step:plan.at(section))
            {
                const auto id=step.at("id").get<std::string>(),op=step.at("op").get<std::string>();
                Json args;
                try{args=Plan::Bind(step.at("args"),results);}
                catch(const std::exception& e)
                {
                    if(!dryRun)throw Bridge::Error(Bridge::Code::EditorError,"Plan step \""+id+"\" ("+op+") failed: "+e.what(),"",{{"failedStep",id},{"rolledBack",true}});
                    preview.push_back({{"step",id},{"section",section},{"change",op},{"note","runs on apply, with arguments from steps that only run then"}});continue;
                }
                const bool read=Bridge::FindPolicy(op)->policy==Bridge::Policy::Read;
                if(dryRun && !read)
                {
                    if(op=="authoring.apply")
                    {
                        results[id]=Result("authoring.preview",args);
                        for(const auto& o:args.at("document").at("operations"))preview.push_back({{"step",id},{"section",section},{"change",o.at("op")},{"target",o.value("id",std::string())}});
                    }
                    else preview.push_back({{"step",id},{"section",section},{"change",op},{"note","runs on apply"}});
                    continue;
                }
                try{results[id]=Result(op,args);}
                catch(const std::exception& e)
                {
                    if(step.value("optional",false)){errors[id]=e.what();continue;}
                    throw Bridge::Error(Bridge::Code::EditorError,"Plan step \""+id+"\" ("+op+") failed: "+e.what(),"",{{"failedStep",id},{"rolledBack",std::string(section)=="steps"}});
                }
            }
        }
        if(dryRun)return {{"preview",preview},{"plan",plan},{"label",plan.value("label",std::string())},{"dryRun",true},{"ok",true}};
        Json out{{"steps",Json(results)},{"undo",{{"steps",1}}},{"label",plan.value("label",std::string())}};
        if(!errors.empty())out["errors"]=errors;
        return out;
    }
    const Json& LastPlan(){Check(!plans.empty(),"a plan was sent");return plans.back();}
};

struct Harness
{
    Editor editor;
    Registry registry;
    std::filesystem::path dir;
    ToolContext context;
    Harness():dir(std::filesystem::temp_directory_path()/("mcp-spec-tools-"+std::to_string(reinterpret_cast<uintptr_t>(this)))),context{editor.fake,dir,dir}
    {
        std::filesystem::create_directories(dir);
        RegisterSpecTools(registry);RegisterIntentTools(registry);
    }
    ~Harness(){std::error_code e;std::filesystem::remove_all(dir,e);}
    // As the server does: the arguments are checked against the tool's schema first.
    ToolResult Call(const std::string& name,const Json& args)
    {
        const auto* tool=registry.FindTool(name);
        Check(tool!=nullptr,"tool "+name+" is registered");
        const auto problem=Protocol::CheckArguments(tool->inputSchema,args);
        Check(problem.empty(),name+" arguments fit its schema: "+problem);
        return tool->run(args,context);
    }
    ToolResult Ok(const std::string& name,const Json& args)
    {
        auto r=Call(name,args);
        Check(!r.isError,name+" succeeds: "+(r.content.empty()?std::string():r.content.front().text.substr(0,1500)));
        return r;
    }
};

bool HasImage(const ToolResult& r)
{
    for(const auto& c:r.content)if(c.type=="image" && c.mimeType=="image/png" && !c.data.empty())return true;
    return false;
}
bool IsPng(const ToolResult& r)
{
    for(const auto& c:r.content)if(c.type=="image"){const auto bytes=Bridge::Unbase64(c.data);return bytes.size()>8 && bytes.compare(0,8,"\x89PNG\r\n\x1a\n")==0;}
    return false;
}
const Json* FindStep(const Json& plan,const std::string& id)
{
    for(const char* section:{"pre","steps","post"})if(plan.contains(section))for(const auto& s:plan.at(section))if(s.at("id")==id)return &s;
    return nullptr;
}
const Json* FindOp(const Json& plan,const std::string& op,const std::string& id)
{
    const auto* actors=FindStep(plan,"actors");
    if(!actors)return nullptr;
    for(const auto& o:actors->at("args").at("document").at("operations"))if(o.at("op")==op && (o.value("id",std::string())==id || (o.contains("actor") && o.at("actor").value("path",std::string())==id)))return &o;
    return nullptr;
}
bool Listed(const Json& list,const std::string& id)
{
    for(const auto& e:list)if(e.value("id",std::string())==id)return true;
    return false;
}
bool NextHas(const Json& structured,const std::string& tool)
{
    for(const auto& n:structured.value("next",Json::array()))if(n.value("tool",std::string())==tool)return true;
    return false;
}
bool WarningHas(const Json& structured,const std::string& text)
{
    for(const auto& w:structured.value("warnings",Json::array()))if(w.is_string() && w.get<std::string>().find(text)!=std::string::npos)return true;
    return false;
}

void Registration()
{
    Harness h;
    const std::vector<std::string> names{"level_spec_validate","level_build","blockout","gameplay","security","mechanisms","events","environment","geometric_events","map_settings"};
    Check(h.registry.tools.size()==names.size(),"ten tools");
    for(const auto& name:names)
    {
        const auto* t=h.registry.FindTool(name);
        Check(t!=nullptr,name+" registered");
        Check(t->description.size()>600,name+" has a full description");
        Check(t->description.find("scct://wiki/")!=std::string::npos,name+" cites the wiki");
        Check(t->inputSchema.value("additionalProperties",true)==false,name+" has a closed schema");
        Check(!Protocol::ToolJson(*t,"2025-06-18").dump().empty(),name+" lists");
        if(name!="level_spec_validate")Check(t->inputSchema.at("properties").contains("dryRun") && t->inputSchema.at("properties").contains("label"),name+" takes dryRun and label");
        if(name!="level_spec_validate" && name!="level_build")Check(t->inputSchema.dump().find("\"$ref\":")==std::string::npos && t->inputSchema.dump().size()<16000,name+" has a self-contained, compact schema");
    }
    Check(h.registry.FindTool("level_spec_validate")->readOnly,"validate is read-only");
    // A fragment may name an item by id alone: required fields are loosened to the id.
    const auto& device=h.registry.FindTool("security")->inputSchema.at("properties").at("devices").at("items");
    Check(device.at("required")==Json::array({"id"}),"devices need only their id in a fragment");
    // The outline leaves the item's fields to the spec check, which names the pointer.
    auto bogus=h.Call("security",{{"devices",Json::array({{{"id","X"},{"kind","laser"},{"bogus",1}}})}});
    Check(bogus.isError && bogus.structured.at("details").at("pointer").get<std::string>().find("/security/devices")==0 &&
          bogus.content.front().text.find("bogus")!=std::string::npos,"unknown fields are still refused, at their pointer");
    // Lifts make their own doors; the description says so (and never to model them as doors).
    const auto& mechanisms=h.registry.FindTool("mechanisms")->description;
    Check(mechanisms.find("doorMesh")!=std::string::npos && mechanisms.find("SLiftDoor")!=std::string::npos && mechanisms.find("carSwitch")!=std::string::npos &&
          mechanisms.find("Lift doors are not made")==std::string::npos,"mechanisms describes the lift doors it makes");
    // The descriptions name what the compiler takes: the engine's softbody
    // class names with a mesh for ESBChainActor only, shared flag drop zones,
    // and a spot light's cone.
    const auto& environment=h.registry.FindTool("environment")->description;
    Check(environment.find("ESBPatchActor, ESBStripDoorActor")!=std::string::npos && environment.find("ESBRopeActor or ESBChainActor, mesh only for ESBChainActor")!=std::string::npos,
          "environment names the softbody classes and where a mesh goes");
    Check(environment.find("cone (spot preset")!=std::string::npos,"environment lights take a cone for the spot preset");
    Check(h.registry.FindTool("gameplay")->description.find("dropZones (1-5) [{id, at} | {ref: id}]")!=std::string::npos,"gameplay's flag drop zones can be shared");
    // tools/list stays small: the shared paragraphs live in scct://docs/tools.
    size_t total=0;
    for(const auto& t:h.registry.tools)
    {
        const auto size=Protocol::ToolJson(t,"2025-06-18").dump().size();
        total+=size;
        Check(size<4500,t.name+" is "+std::to_string(size)+" bytes in tools/list");
        Check(t.description.find("scct://docs/tool")!=std::string::npos,t.name+" points at the shared docs");
        Check(t.description.find("How it works: the arguments are laid over")==std::string::npos,t.name+" does not repeat the shared paragraph");
    }
    Check(total<32000,"the spec and intent tools take "+std::to_string(total)+" bytes");
    // Every intent argument has its place in the spec (Tools::SpecPointer, which offline fixes use).
    const std::set<std::string> common{"mode","dryRun","label","remove","existing","build","screenshot","save","force","mapName","brushMode","removeModes","minimap"};
    for(const auto& name:names)
    {
        if(name=="level_spec_validate" || name=="level_build")continue;
        for(const auto& [property,schema]:h.registry.FindTool(name)->inputSchema.at("properties").items())
            if(!common.count(property) || (name=="map_settings" && property=="mapName"))
                Check(!Tools::SpecPointer(name,property).empty(),name+"."+property+" has a spec pointer");
    }
}

void ValidateExamples()
{
    Harness h;
    for(const auto& name:kExamples)
    {
        const auto r=h.Ok("level_spec_validate",{{"spec",Example(name)}});
        Check(r.structured.at("valid")==true,name+" is valid: "+r.content.front().text.substr(0,800));
        Check(r.structured.at("summary").at("errors")==0,name+" has no errors");
        Check(HasImage(r) && IsPng(r),name+" comes with a plan PNG");
        Check(r.structured.at("plan").at("steps").get<int>()>0,name+" has plan steps");
        // From a file on disk, with the release profile.
        const auto file=(kRoot/"Reloaded.Editor.Mcp"/"Resources"/"examples"/(name+".level.json")).string();
        const auto byPath=h.Ok("level_spec_validate",{{"path",file},{"profile","release"},{"render",false}});
        Check(byPath.structured.at("summary").at("errors")==0 && byPath.structured.at("summary").at("profile")=="release",name+" passes release from its file");
        Check(!HasImage(byPath),"render false leaves the picture out");
    }
    const auto embedded=h.Ok("level_spec_validate",{{"path","scct://examples/versus-starter"}});
    Check(embedded.structured.at("valid")==true && embedded.structured.at("source")=="scct://examples/versus-starter","the embedded example validates");
    // A shape error is a finding at its pointer, not a failure.
    auto bad=Example("versus-starter");bad["geometry"]["pieces"][0]["colour"]="red";
    const auto shape=h.Ok("level_spec_validate",{{"spec",bad}});
    Check(shape.structured.at("valid")==false && shape.structured.at("diagnostics")[0].at("pointer")=="/geometry/pieces/0/colour","a shape error names its pointer");
    // A layout error carries the pointer and the id of the item at fault.
    auto lost=Example("versus-starter");lost["spawns"]["spy"]["in"]="Nowhere";
    const auto ref=h.Ok("level_spec_validate",{{"spec",lost}});
    Check(ref.structured.at("valid")==false && ref.structured.at("summary").at("errors").get<int>()>0,"an unknown room is an error");
    bool pointed=false;
    for(const auto& d:ref.structured.at("diagnostics"))pointed=pointed || (d.at("severity")=="error" && d.value("pointer",std::string()).rfind("/spawns",0)==0);
    Check(pointed,"the error points into spawns");
    Check(h.Call("level_spec_validate",{{"path","scct://examples/none"}}).isError,"an unknown example fails");
    Check(h.Call("level_spec_validate",Json::object()).isError,"spec or path is needed");
    Check(h.editor.fake.calls.empty(),"level_spec_validate never calls the editor");
}

void PreviewAndApply()
{
    Harness h;
    const auto spec=Example("versus-starter");
    // Offline: no editor, still the compile and the picture.
    h.editor.fake.connected=false;
    auto offline=h.Ok("level_build",{{"spec",spec}});
    Check(HasImage(offline) && WarningHas(offline.structured,"No editor answered"),"an offline preview warns and draws");
    Check(h.editor.plans.empty(),"no plan offline");
    Check(h.Call("level_build",{{"spec",spec},{"mode","apply"}}).isError,"apply needs the editor");
    h.editor.fake.connected=true;
    // Preview: the dry run of the plan, nothing stored.
    auto preview=h.Ok("level_build",{{"spec",spec}});
    Check(h.editor.plans.size()==1 && h.editor.dry.back(),"a preview sends a dry-run plan");
    const auto* received=h.editor.fake.Last("mcp.plan");
    Check(received->dryRun && received->args.value("dryRun",false) && !received->expect,"the dry run is flagged and carries no expect");
    Check(preview.structured.at("mode")=="preview" && preview.structured.at("preview").size()>30,"preview rows come back");
    Check(HasImage(preview) && IsPng(preview),"the plan PNG comes back");
    Check(Listed(preview.structured.at("created"),"ObjectiveRoom") && Listed(preview.structured.at("created"),"ServerTerminal"),"the preview lists what it would create");
    Check(preview.structured.at("undo").is_null() && h.editor.sidecar.is_null(),"a preview changes nothing");
    Check(preview.structured.at("mapSettings").at("applied")==false,"Map Settings are only tried on apply");
    Check(h.editor.fake.Count("authoring.schema")==1,"the live schema is read for the classes");
    // Apply.
    auto applied=h.Ok("level_build",{{"spec",spec},{"mode","apply"}});
    const auto& plan=h.editor.LastPlan();
    received=h.editor.fake.Last("mcp.plan");
    Check(!h.editor.dry.back() && received->expect && received->timeoutMs==900000,"apply carries expect and the long timeout");
    Check(received->label=="level_build VERSUS STARTER" && plan.at("label")=="level_build VERSUS STARTER","the Undo label names the map");
    Check(FindStep(plan,"blocks1") && FindStep(plan,"actors") && FindStep(plan,"sidecar") && FindStep(plan,"build") && FindStep(plan,"screenshot"),"the plan has blockout, actors, sidecar, build and screenshot");
    Check(h.editor.sidecar.value("format",std::string())=="scct.level-sidecar","the sidecar was stored");
    const auto& created=applied.structured.at("created");
    Check(Listed(created,"ObjectiveRoom") && Listed(created,"ServerTerminal") && Listed(created,"CamServer"),"created lists pieces and actors");
    for(const auto& e:created)if(e.value("id",std::string())=="ServerTerminal")Check(e.value("path",std::string())=="MyLevel.ServerTerminal","created carries the editor's path");
    Check(applied.structured.at("undo").is_object() && applied.structured.at("undo").value("call",0)>0,"apply reports its undo");
    Check(HasImage(applied) && applied.structured.contains("screenshot"),"the screenshot comes back as an image");
    Check(applied.structured.at("mapCheck").at("summary").at("warnings")==1,"Map Check rows come back");
    const auto& settings=applied.structured.at("mapSettings");
    Check(settings.at("applied")==false && settings.at("steps").size()==4 && settings.at("checklist").size()>3,"unsupported Map Settings give the checklist and steps");
    bool name=false;for(const auto& line:settings.at("checklist"))name=name || line=="MapName = VERSUS STARTER";
    Check(name,"the checklist reads as typed: MapName = VERSUS STARTER");
    Check(WarningHas(applied.structured,"Map Settings were NOT written"),"the summary warns Map Settings were not written");
    Check(NextHas(applied.structured,"map_file") && NextHas(applied.structured,"editor_restart") && NextHas(applied.structured,"map_settings"),"a new map is saved, restarted, then given Map Settings");
    // Map Settings that the editor can write are reported as written.
    h.editor.mapSettingsWork=true;
    auto again=h.Ok("level_build",{{"spec",spec},{"mode","apply"},{"build","all"},{"save","Packages\\MapsEd\\VersD.sdc"}});
    const auto& rebuilt=h.editor.LastPlan();
    Check(FindStep(rebuilt,"build")->at("op")=="build.all" && FindStep(rebuilt,"save"),"build all and save reach the plan");
    Check(h.editor.fake.Last("mcp.plan")->timeoutMs==3600000,"a full build waits longer");
    Check(again.structured.at("mapSettings").at("applied")==true,"written Map Settings are reported");
    Check(again.structured.at("created").empty() && again.structured.value("counts",Json::object()).value("kept",0)>30,"an unchanged spec keeps everything");
    Check(!NextHas(again.structured,"editor_restart"),"a saved map is not restarted");
}

void ErrorsBlockApply()
{
    Harness h;
    auto spec=Example("versus-starter");spec["spawns"]["spy"]["in"]="Nowhere";
    auto refused=h.Call("level_build",{{"spec",spec},{"mode","apply"}});
    Check(refused.isError && refused.structured.at("validation").size()>0,"errors block apply and are listed");
    Check(h.editor.plans.empty(),"nothing was sent");
    auto forced=h.Ok("level_build",{{"spec",spec},{"mode","apply"},{"force",true}});
    Check(h.editor.plans.size()==1,"force builds anyway");
    auto shape=Example("versus-starter");shape["security"]["devices"][0]["kind"]="toaster";
    auto bad=h.Call("level_build",{{"spec",shape}});
    Check(bad.isError && bad.structured.at("details").at("pointer")=="/security/devices/0/kind","a shape error names its pointer");
    // A failing plan comes back as the editor's error, with its step.
    h.editor.fake.Clear("mcp.plan");
    h.editor.fake.Fail("mcp.plan",Bridge::Code::EditorError,"Plan step \"actors\" (authoring.apply) failed: boom. The plan's map changes were undone.","",{{"failedStep","actors"},{"rolledBack",true}});
    auto failed=h.Call("level_build",{{"spec",Example("versus-starter")},{"mode","apply"}});
    Check(failed.isError && failed.structured.at("details").at("failedStep")=="actors","a plan failure names its step");
}

void Unchanged()
{
    Harness h;
    h.editor.state["mapFile"]="C:\\Games\\SCCT\\Packages\\MapsEd\\SmalD.sdc";
    const auto spec=Level(R"({"map":{"name":"SMALL"},"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[1024,1024,256],"at":[0,0]}]},
        "spawns":{"spy":{"in":"Hall","count":3},"merc":[{"id":"M1","at":{"in":"Hall","u":0.4,"v":0.4}},{"id":"M2","at":{"in":"Hall","u":0.4,"v":-0.4}},{"id":"M3","at":{"in":"Hall","u":0.3,"v":0}}]},
        "gameplay":{"modes":["deathmatch"],"deathmatch":{"mission":{"id":"Match"}}},"security":{"camnet":[{"id":"Cam1","at":{"in":"Hall","h":"ceiling-16"}}]}})");
    const auto check=h.Ok("level_spec_validate",{{"spec",spec}});
    Check(check.structured.at("valid")==true,"the small spec is valid: "+check.content.front().text.substr(0,1500));
    auto first=h.Ok("level_build",{{"spec",spec},{"mode","apply"}});
    Check(NextHas(first.structured,"map_file") && !NextHas(first.structured,"editor_restart"),"a map with a file is only saved");
    auto second=h.Ok("level_build",{{"spec",spec},{"mode","apply"}});
    Check(h.editor.plans.size()==1,"the same spec again sends nothing");
    Check(second.structured.at("summary").get<std::string>().rfind("Nothing to change",0)==0 && second.structured.at("undo").is_null(),"and says so");
    // Undone in the editor: the sidecar's actors are gone, so they are made again.
    h.editor.live.clear();
    auto redo=h.Ok("level_build",{{"spec",spec},{"mode","apply"}});
    Check(h.editor.plans.size()==2 && Listed(redo.structured.at("created"),"M1") && Listed(redo.structured.at("created"),"Hall"),"what the map lost is built again");
    Check(WarningHas(redo.structured,"no longer in the map"),"and the loss is reported");
}

void Intents()
{
    Harness h;
    h.Ok("level_build",{{"spec",Example("versus-starter")},{"mode","apply"}});
    const auto builtPlans=h.editor.plans.size();
    // security: one more laser and a longer alarm, built incrementally.
    auto security=h.Ok("security",{{"devices",Json::array({{{"id","EastLaser"},{"kind","laser"},{"at",{{"in","EastCorridor"},{"v",0.25},{"u",-0.5}}},{"facing",90},{"length",184},{"alarm","ServerAlarm"},{"detected","spy"}}})},
                                    {"alarms",Json::array({{{"id","ServerAlarm"},{"duration",45}}})}});
    const auto& plan=h.editor.LastPlan();
    Check(h.editor.plans.size()==builtPlans+1 && FindOp(plan,"create","EastLaser"),"the new laser is created");
    Check(!FindOp(plan,"create","ServerTerminal") && !FindStep(plan,"blocks1"),"what did not change is left alone");
    Check(!FindStep(plan,"build") && !FindStep(plan,"screenshot"),"no brushes changed, so no build (build auto)");
    Check(Listed(security.structured.at("created"),"EastLaser") && !Listed(security.structured.at("created"),"ServerTerminal"),"created is just the laser");
    Check(Listed(security.structured.at("changed"),"ServerAlarm"),"the alarm is changed");
    const auto& stored=h.editor.sidecar.at("spec").at("security");
    Check(stored.at("devices").size()==2 && stored.at("alarms")[0].at("duration")==45 && stored.at("alarms")[0].contains("lockObjectives"),"the fragment merged by id into the spec");
    for(const auto& v:security.structured.at("validation"))Check(v.at("severity")=="error" || v.value("pointer",std::string()).rfind("/security",0)==0 || v.value("rule",std::string()).rfind("security",0)==0 ||
                                                              v.value("rule",std::string()).rfind("camnet",0)==0,"validation is this part's findings");
    // The same again changes nothing, and Map Settings are not retried by another part's tool.
    Check(!FindStep(plan,"mapsettings"),"unchanged Map Settings are left alone by security");
    const auto sent=h.editor.plans.size();
    auto same=h.Ok("security",{{"alarms",Json::array({{{"id","ServerAlarm"},{"duration",45}}})}});
    Check(h.editor.plans.size()==sent && same.structured.at("summary").get<std::string>().rfind("Nothing to change",0)==0,"an unchanged part sends nothing");
    // dryRun previews and stores nothing.
    const auto before=h.editor.sidecar;
    auto dry=h.Ok("security",{{"camnet",Json::array({{{"id","CamExtra"},{"at",{{"in","SpySpawn"},{"h","ceiling-16"}}}}})},{"dryRun",true}});
    Check(h.editor.dry.back() && h.editor.sidecar==before && Listed(dry.structured.at("created"),"CamExtra") && HasImage(dry),"dryRun is a preview");
    // remove takes an item out, and the map loses it.
    auto removed=h.Ok("security",{{"remove",Json::array({"EastLaser"})}});
    Check(FindStep(h.editor.LastPlan(),"deletes") && Listed(removed.structured.at("removed"),"EastLaser"),"remove deletes what the item built");
    Check(h.Call("security",{{"remove",Json::array({"NoSuchThing"})}}).isError,"removing an unknown id fails");
    Check(h.Call("security",Json::object()).isError,"an empty fragment fails");
    // gameplay: modes add up; removeModes drops a mode with its section.
    h.Ok("gameplay",{{"modes",Json::array({"story"})},{"diskhunt",{{"resetTime",20}}}});
    Check(h.editor.sidecar.at("spec").at("gameplay").at("modes").size()==3 && h.editor.sidecar.at("spec").at("gameplay").at("diskhunt").at("resetTime")==20,"modes are a union and sections merge");
    auto dropped=h.Ok("gameplay",{{"removeModes",Json::array({"deathmatch"})}});
    Check(h.editor.sidecar.at("spec").at("gameplay").at("modes")==Json::array({"story","diskhunt"}) && !h.editor.sidecar.at("spec").at("gameplay").contains("deathmatch"),"removeModes drops the mode and its section");
    Check(Listed(dropped.structured.at("removed"),"Deathmatch"),"its mission is removed from the map");
    // events and mechanisms land in their sections.
    h.Ok("events",{{"messages",Json::array({{{"id","Hello"},{"spy","Go"},{"merc","Hold"},{"at",{{"in","ObjectiveRoom"}}}}})},{"level",Json::array({{{"event","Hello"},{"sendAtTime",-60}}})}});
    Check(h.editor.sidecar.at("spec").at("events").at("messages").size()==1 && h.editor.sidecar.at("spec").at("events").at("level")[0].at("sendAtTime")==-60,"events merge into the events section");
    h.Ok("events",{{"level",Json::array({{{"event","Hello"},{"sendAtTime",-90}}})}});
    Check(h.editor.sidecar.at("spec").at("events").at("level").size()==1 && h.editor.sidecar.at("spec").at("events").at("level")[0].at("sendAtTime")==-90,"level events match by event");
    auto lights=h.Ok("environment",{{"lights",Json::array({{{"id","SpawnLamp"},{"preset","ceiling"},{"in","SpySpawn"}}})},{"ingredients",Json::array({{{"kind","ammo"},{"in","SpySpawn"}},{{"kind","ammo"},{"in","MercSpawn"}}})}});
    Check(Listed(lights.structured.at("created"),"SpawnLamp") && h.editor.sidecar.at("spec").at("environment").at("ingredients").size()==4,"ingredients without ids are added (an identical one is not repeated), not replaced");
}

void FreshMap()
{
    Harness h;
    h.editor.state["mapFile"]="C:\\Games\\SCCT\\Packages\\MapsEd\\TestD.sdc";
    auto blockout=h.Ok("blockout",{{"pieces",Json::array({{{"id","Hall"},{"kind","Room"},{"size",{1024,1024,256}},{"at",{0,0}}}})}});
    const auto& spec=h.editor.sidecar.at("spec");
    Check(spec.at("map").at("name")=="TESTD" && spec.at("map").at("file")=="TestD","a map without a spec starts one named after its file");
    Check(FindStep(h.editor.LastPlan(),"blocks1") && FindStep(h.editor.LastPlan(),"build") && HasImage(blockout),"a blockout builds the geometry and shows it");
    Check(WarningHas(blockout.structured,"no level spec from an earlier build"),"starting a spec is noted");
    auto spawns=h.Ok("gameplay",{{"spawns",{{"spy",{{"in","Hall"},{"count",3}}},{"merc",{{"in","Hall"},{"count",3}}}}}});
    Check(!FindStep(h.editor.LastPlan(),"blocks1") && spawns.structured.at("created").size()==6,"spawns go into the existing hall");
    Check(NextHas(spawns.structured,"map_file") && !NextHas(spawns.structured,"editor_restart"),"a saved map only needs saving again");
}

void MapSettingsTool()
{
    Harness h;
    h.Ok("level_build",{{"spec",Example("versus-starter")},{"mode","apply"}});
    auto r=h.Ok("map_settings",{{"mapName","VERSUS STARTER"},{"modes",Json::array({{{"mode","diskhunt"},{"scoreLimit",5}}})},{"level",{{"deathPawnHeight",-500}}},
                                 {"interface",{{"menu","C:/Images/menu.png"}}},{"minimap",true}});
    const auto& plan=h.editor.LastPlan();
    Check(FindStep(plan,"mapsettings") && FindStep(plan,"mapsettings")->value("optional",false),"Map Settings go in as an optional post step");
    Check(FindStep(plan,"interface1") && FindStep(plan,"interface1")->at("args").at("slot")=="Menu","the interface image is imported into its slot");
    Check(FindStep(plan,"minimap") && FindStep(plan,"minimap")->at("op")=="minimap.apply","the minimap is set");
    Check(FindStep(plan,"settings_edit") && FindStep(plan,"settings_edit")->at("args").at("changes").contains("fDeathPawnHeight"),"Level Properties go through settings.edit");
    const auto& modes=h.editor.sidecar.at("spec").at("settings").at("modes");
    Check(modes.size()==3,"modes merge by mode");
    for(const auto& m:modes)if(m.at("mode")=="diskhunt")Check(m.at("scoreLimit")==5,"the disk hunt mode changed");
    const auto& settings=r.structured.at("mapSettings");
    Check(settings.at("applied")==false && settings.at("steps").size()==4 && settings.at("error").get<std::string>().find("cannot write")!=std::string::npos,"unsupported Map Settings: not applied, with the steps");
    Check(settings.at("values").at("MapName")=="\"VERSUS STARTER\"","the values are listed");
    Check(r.structured.at("interface")[0].at("applied")==true && r.structured.at("minimap").at("applied")==true,"the interface and minimap steps are reported");
    Check(WarningHas(r.structured,"Map Settings were NOT written") && WarningHas(r.structured,"not a file on this machine"),"the warnings say what did not happen");
    // mapName alone is a Map Settings change (and names a new spec too).
    Check(h.registry.FindTool("map_settings")->inputSchema.at("properties").at("mapName").value("description",std::string()).find("Map Settings")!=std::string::npos,"map_settings mapName is the Map Settings name");
    h.Ok("map_settings",{{"mapName","STARTER"}});
    Check(h.editor.sidecar.at("spec").at("settings").at("mapName")=="STARTER","mapName alone changes MapName");
    // Settings given as one object, and a mode dropped.
    h.Ok("map_settings",{{"mapSettings",{{"legend","STARTER|Three rooms"}}},{"removeModes",Json::array({"deathmatch"})}});
    Check(h.editor.sidecar.at("spec").at("settings").at("modes").size()==2 && h.editor.sidecar.at("spec").at("settings").at("legend")=="STARTER|Three rooms","mapSettings and removeModes apply");
}

void GeometricEvents()
{
    Harness h;
    h.Ok("level_build",{{"spec",Example("versus-starter")},{"mode","apply"}});
    const auto plans=h.editor.plans.size();
    auto list=h.Ok("geometric_events",Json::object());
    Check(h.editor.plans.size()==plans && h.editor.fake.Count("ge.list")==1 && list.structured.contains("listed"),"no arguments lists the GEs");
    auto ladder=h.Ok("geometric_events",{{"ges",Json::array({{{"kind","ladder"},{"points",Json::array({Json::array({0,300,320}),Json::array({0,300,0}),Json::array({0,332,0})})}}})}});
    const auto* step=FindStep(h.editor.LastPlan(),"ge1");
    Check(step && step->at("op")=="ge.add" && step->at("args").at("command")=="LADDER","a GE becomes GE ADD");
    Check(step->at("args").at("points")[0][2]==0,"a ladder goes low end first");
    Check(ladder.structured.contains("geList") && ladder.structured.at("geList").at("elements").size()==1,"the result lists the GEs");
    h.Ok("geometric_events",{{"ges",Json::array({{{"kind","ledge"},{"points",Json::array({Json::array({-256,512,208}),Json::array({256,512,208})})}}})}});
    Check(h.editor.sidecar.at("spec").at("ges").size()==2 && FindStep(h.editor.LastPlan(),"ge1") && !FindStep(h.editor.LastPlan(),"ge2"),"GEs add up and only the new one is made");
}

// Every result keeps to its tool's outputSchema; an error carries
// structuredContent only when it fits (clients validate it even with isError).
void OutputSchemas()
{
    Harness h;
    RegisterValidateTools(h.registry);
    h.Ok("level_build",{{"spec",Example("versus-starter")},{"mode","apply"}});
    auto sent=[&](const Tool& tool,const Json& args,const std::string& what,int expectError)
    {
        const auto r=tool.run(args,h.context);
        if(expectError>=0)Check(r.isError==(expectError==1),tool.name+" "+what+(r.isError?" failed":" worked")+": "+(r.content.empty()?std::string():r.content.front().text.substr(0,600)));
        const auto reply=Protocol::ToolResultJson(r,"2025-06-18",tool.outputSchema);
        if(!r.isError)Check(reply.contains("structuredContent"),tool.name+" "+what+": a result has structured content");
        if(reply.contains("structuredContent"))
        {
            const auto problem=Protocol::CheckStructured(tool.outputSchema,reply.at("structuredContent"));
            Check(problem.empty(),tool.name+" "+what+": "+problem);
        }
        return reply;
    };
    for(const auto& tool:h.registry.tools)
    {
        if(!tool.outputSchema.is_object())continue;
        Json working=Json{{"remove",Json::array()},{"mode","preview"}},broken=Json::object();
        if(tool.name=="level_spec_validate")working={{"path","scct://examples/versus-starter"},{"render",false}};
        if(tool.name=="level_build")working={{"spec",Example("versus-starter")}};
        if(tool.name=="map_validate")working={{"spec",Example("versus-starter")}};
        if(tool.name=="geometric_events")broken={{"remove",Json::array({"NoSuchGe"})}};
        sent(tool,working,"working",0);
        sent(tool,broken,"with nothing to do",tool.name=="map_validate"?-1:1);
        sent(tool,{{"path","C:/no/such/spec.level.json"}},"with a missing file",tool.name=="level_spec_validate" || tool.name=="level_build" || tool.name=="map_validate"?1:-1);
        h.editor.fake.connected=false;
        sent(tool,working,"without an editor",-1);
        h.editor.fake.connected=true;
    }
    // The two with required fields leave structuredContent out of their errors.
    const auto* validate=h.registry.FindTool("map_validate");
    h.editor.state["open"]=false;
    const auto closed=sent(*validate,Json::object(),"with no map open",1);
    Check(!closed.contains("structuredContent") && closed.at("content")[0].at("text").get<std::string>().find("No map is open")!=std::string::npos,"map_validate's error is text only");
    h.editor.state["open"]=true;
    const auto missing=sent(*h.registry.FindTool("level_spec_validate"),{{"path","scct://map/spec"}},"with the live spec's uri",1);
    Check(!missing.contains("structuredContent") && missing.at("content")[0].at("text").get<std::string>().find("map_query")!=std::string::npos,"level_spec_validate's error is text only, and points at map_query");
}
// A spec checked offline is fixed by editing it, not by the intent tools,
// which would change the open map instead.
void OfflineFixes()
{
    Harness h;
    RegisterValidateTools(h.registry);
    const auto bare=Level(R"({"map":{"name":"SMALL"},"geometry":{"pieces":[{"id":"Hall","kind":"Room","size":[1024,1024,256],"at":[0,0]}]},
        "gameplay":{"modes":["deathmatch"],"deathmatch":{"mission":{"id":"Match"}}}})");
    const std::set<std::string> intents{"blockout","gameplay","security","mechanisms","events","environment","geometric_events","map_settings"};
    auto offline=[&](const Json& list,const std::string& what)
    {
        size_t edits=0;
        std::set<std::string> pointers;
        for(const auto& d:list)
        {
            if(!d.contains("fix"))continue;
            const auto& fix=d.at("fix");
            Check(!intents.count(fix.value("tool",std::string())),what+": "+d.value("rule",std::string())+" is fixed in the spec, not with "+fix.value("tool",std::string()));
            if(fix.contains("spec"))for(const auto& e:fix.at("spec")){++edits;pointers.insert(e.at("pointer").get<std::string>());Check(e.contains("merge") || e.contains("change"),what+": an edit says what to write");}
        }
        Check(edits>0 && pointers.count("/spawns") && pointers.count("/security/camnet"),what+": the spawn and CamNet fixes are spec edits");
    };
    const auto checked=h.Ok("level_spec_validate",{{"spec",bare},{"render",false}});
    Check(checked.structured.at("valid")==false,"the bare spec has errors");
    offline(checked.structured.at("diagnostics"),"level_spec_validate");
    for(const auto& n:checked.structured.at("next"))Check(!intents.count(n.value("tool",std::string())),"level_spec_validate suggests no intent tool");
    const auto validated=h.Ok("map_validate",{{"spec",bare}});
    offline(validated.structured.at("findings"),"map_validate spec");
    const auto& said=validated.content.front().text;
    Check(said.find("fix.spec lists the edits")!=std::string::npos && said.find("names the tool and arguments")==std::string::npos,"the offline summary says the fixes edit the spec: "+said.substr(0,400));
    for(const auto& n:validated.structured.at("next"))Check(!intents.count(n.value("tool",std::string())),"map_validate offline suggests no intent tool");
    // From a file, the next check is of that file.
    const auto file=h.dir/"bare.level.json";
    {std::ofstream(file)<<bare.dump();}
    const auto byPath=h.Ok("map_validate",{{"path",file.string()}});
    Check(byPath.structured.at("summary").at("source")=="spec" && byPath.structured.at("next").at(0).at("args").at("path")==file.string(),"map_validate takes a path and checks it again");
    for(const auto& op:h.editor.fake.Ops())Check(op=="authoring.schema","offline, the editor is only asked for its schema, not "+op);
    // The edit means what it says: merged at its pointer, the error goes.
    auto fixed=bare;
    for(const auto& d:checked.structured.at("diagnostics"))
        if(d.value("rule",std::string()).rfind("spawns.team",0)==0)
            for(const auto& e:d.at("fix").at("spec"))
                if(e.contains("merge"))fixed[Json::json_pointer(e.at("pointer").get<std::string>())]=Spec::Merge(fixed.value(Json::json_pointer(e.at("pointer").get<std::string>()),Json::object()),e.at("merge"));
    fixed["spawns"]["spy"]["in"]="Hall";fixed["spawns"]["merc"]["in"]="Hall";
    const auto again=h.Ok("level_spec_validate",{{"spec",fixed},{"render",false}});
    for(const auto& d:again.structured.at("diagnostics"))Check(d.value("rule",std::string()).rfind("spawns.team-missing",0)!=0,"the spawn fix took");
}
// A preview with errors saves the spec it built and points
// level_spec_validate at that file (scct://map/spec is no file).
void PreviewSpecFile()
{
    Harness h;
    h.Ok("level_build",{{"spec",Example("versus-starter")},{"mode","apply"}});
    auto preview=h.Ok("gameplay",{{"spawns",{{"spy",{{"in","Nowhere"}}}}},{"mode","preview"}});
    const auto& next=preview.structured.at("next");
    size_t checks=0;
    for(const auto& n:next)
    {
        Check(n.dump().find("scct://map/spec")==std::string::npos,"no next step names scct://map/spec");
        checks+=n.value("tool",std::string())=="level_spec_validate";
    }
    Check(checks==1 && next.at(0).at("tool")=="level_spec_validate","one level_spec_validate step, first: "+next.dump());
    const auto file=next.at(0).at("args").at("path").get<std::string>();
    Check(preview.structured.at("specFile")==file && std::filesystem::exists(std::filesystem::path(file)),"the built spec is saved in the work folder");
    const auto checked=h.Ok("level_spec_validate",{{"path",file},{"render",false}});
    Check(checked.structured.at("valid")==false && checked.structured.at("summary").at("errors").get<int>()>0,"and level_spec_validate reads it and shows the errors");
    // An apply blocked by errors points at the file too.
    auto blocked=h.Call("gameplay",{{"spawns",{{"spy",{{"in","Nowhere"}}}}}});
    Check(blocked.isError && blocked.structured.at("next").at(0).at("args").contains("path"),"a blocked apply points level_spec_validate at the saved spec");
    // level_build sync merge saves the merged candidate the findings are
    // about, not the fragment it was given.
    auto merged=h.Ok("level_build",{{"spec",{{"spawns",{{"spy",{{"in","Nowhere"}}}}}}},{"sync","merge"}});
    const auto mergedFile=merged.structured.at("specFile").get<std::string>();
    Json candidate;
    {std::ifstream in{std::filesystem::path(mergedFile)};candidate=Json::parse(in);}
    Check(candidate.contains("geometry") && candidate.at("map").at("name")=="VERSUS STARTER" && candidate.at("spawns").at("spy").at("in")=="Nowhere","the merge's file is the stored spec with the fragment laid over it");
    const auto again=h.Ok("level_spec_validate",{{"path",mergedFile},{"render",false}});
    bool nowhere=false;
    for(const auto& d:again.structured.at("diagnostics"))nowhere=nowhere || (d.at("severity")=="error" && d.value("pointer",std::string()).rfind("/spawns",0)==0);
    Check(nowhere,"and checking it finds the error the preview reported");
    // <work>/specs keeps the newest 20.
    const auto specs=h.dir/"specs";
    for(int i=0;i<25;++i)
    {
        const auto old=specs/("old-"+std::to_string(i)+".level.json");
        {std::ofstream(old)<<"{}";}
        std::filesystem::last_write_time(old,std::filesystem::file_time_type::clock::now()-std::chrono::hours(1+i));
    }
    const auto latest=h.Ok("gameplay",{{"spawns",{{"spy",{{"in","Nowhere"}}}}},{"mode","preview"}}).structured.at("specFile").get<std::string>();
    size_t kept=0;
    for(const auto& e:std::filesystem::directory_iterator(specs))kept+=e.path().extension()==".json";
    Check(kept==20 && std::filesystem::exists(std::filesystem::path(latest)) && !std::filesystem::exists(specs/"old-24.level.json") && std::filesystem::exists(specs/"old-0.level.json"),
          "the oldest saved specs are deleted: "+std::to_string(kept)+" kept");
}

int main()
{
    try
    {
        Registration();
        ValidateExamples();
        PreviewAndApply();
        ErrorsBlockApply();
        Unchanged();
        Intents();
        FreshMap();
        MapSettingsTool();
        GeometricEvents();
        OutputSchemas();
        OfflineFixes();
        PreviewSpecFile();
    }
    catch(const std::exception& e){std::cerr<<"McpSpecToolsTests failed: "<<e.what()<<"\n";return 1;}
    std::cout<<"McpSpecToolsTests passed\n";
    return 0;
}
