// The MCP server end to end over strings: the handshake, lists, tool calls
// against a FakeBridge, resources, prompts, progress, cancellation and errors.
// Builds without the other agents' tools (only the session, query and escape
// tools and the resources are registered), so it needs no stubs.
#include "../Reloaded.Editor.Mcp/McpServer.h"
#include "../Reloaded.Editor.Mcp/FakeBridge.h"
#include "../Reloaded.Editor.Mcp/BridgeClient.h"
#include "../Reloaded.Editor.Mcp/Tools/Common.h"
#include <atomic>
#include <fstream>
#include <iostream>
#include <source_location>
#include <sstream>
using namespace Mcp;
void Check(bool value,const char* message,std::source_location where=std::source_location::current())
{
    if(!value)throw std::runtime_error(std::string(message)+" (line "+std::to_string(where.line())+")");
}

// A test tool that blocks until released or cancelled.
struct Gate
{
    std::mutex m;std::condition_variable cv;bool open=false;std::atomic<bool> started{false};std::atomic<int> runs{0};
    void Open(){{std::lock_guard<std::mutex> g(m);open=true;}cv.notify_all();}
    void Wait(){++runs;started=true;std::unique_lock<std::mutex> g(m);cv.wait(g,[&]{return open;});}
};

struct Harness
{
    FakeBridge fake;
    Registry registry;
    Gate gate;
    std::atomic<int> cancels{0},resumes{0};
    std::mutex m;
    std::vector<Json> out;
    std::filesystem::path dir;
    std::unique_ptr<Server> server;
    int next=1;
    explicit Harness(const std::string& version="2025-06-18",bool initialise=true)
    {
        dir=std::filesystem::temp_directory_path()/("mcp-server-tests-"+std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir);
        RegisterSessionTools(registry);RegisterQueryTools(registry);RegisterEscapeTools(registry);RegisterResources(registry);
        Tool block;block.name="test_block";block.description="Blocks until the test releases it.";block.readOnly=true;
        block.run=[this](const Json&,ToolContext& c){c.progress(1,2,"waiting");gate.Wait();return Ok({{"released",true}});};
        registry.Add(block);
        Tool fail;fail.name="test_throw";fail.description="Throws a bridge error.";fail.readOnly=true;
        fail.run=[](const Json&,ToolContext&)->ToolResult{throw Bridge::Error(Bridge::Code::Busy,"The editor is busy.","modal");};
        registry.Add(fail);
        Prompt p;p.name="versus_starter";p.title="Starter";p.description="A starter level.";p.arguments={{"theme","What it looks like.",true}};
        p.messages=[](const Json& a,ToolContext&){return Json::array({{{"role","user"},{"content",{{"type","text"},{"text","Build a "+a.at("theme").get<std::string>()+" map."}}}}});};
        registry.Add(p);
        ServerOptions o;o.systemDir=dir;o.workDir=dir/"work";o.version="test-1";
        o.cancel=[this]{++cancels;gate.Open();};o.resume=[this]{++resumes;};
        server=std::make_unique<Server>(registry,fake,o,[this](const std::string& line)
        {
            Check(line.find('\n')==std::string::npos,"one message per line");
            std::lock_guard<std::mutex> g(m);out.push_back(Json::parse(line));
        });
        if(initialise)
        {
            auto init=Request("initialize",{{"protocolVersion",version},{"capabilities",Json::object()},{"clientInfo",{{"name","tests"},{"version","1"}}}});
            Check(init.contains("result"),"initialize answers");
            Send({{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        }
    }
    ~Harness(){gate.Open();server.reset();std::error_code e;std::filesystem::remove_all(dir,e);}
    void Send(const Json& message){server->Handle(message.dump());}
    Json Find(const Json& id)
    {
        std::lock_guard<std::mutex> g(m);
        for(const auto& message:out)if(message.contains("id") && message.at("id")==id && (message.contains("result") || message.contains("error")))return message;
        return Json();
    }
    Json Request(const std::string& method,const Json& params=Json::object(),Json id=Json())
    {
        if(id.is_null())id=next++;
        Send({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}});
        server->Wait();
        return Find(id);
    }
    Json Result(const std::string& method,const Json& params=Json::object())
    {
        auto reply=Request(method,params);
        if(!reply.contains("result"))throw std::runtime_error(method+" failed: "+reply.dump());
        return reply.at("result");
    }
    Json Call(const std::string& tool,const Json& arguments=Json::object(),Json meta=Json())
    {
        Json params{{"name",tool},{"arguments",arguments}};
        if(!meta.is_null())params["_meta"]=meta;
        return Result("tools/call",params);
    }
    std::vector<Json> Notes(const std::string& method)
    {
        std::lock_guard<std::mutex> g(m);std::vector<Json> notes;
        for(const auto& message:out)if(message.value("method",std::string())==method)notes.push_back(message);
        return notes;
    }
    void Clear(){std::lock_guard<std::mutex> g(m);out.clear();}
};
std::string Text(const Json& result){return result.at("content")[0].at("text").get<std::string>();}
Json OpenState(){return {{"open",true},{"level",7},{"generation",2},{"revision",11},{"mapFile",""},{"mapKey","unsaved:7:2"},{"map","unsaved:7:2"},{"dirty",false},{"canUndo",true},{"canRedo",false}};}

void Handshake()
{
    Harness h("2025-06-18",false);
    auto init=h.Result("initialize",{{"protocolVersion","2025-06-18"},{"capabilities",Json::object()},{"clientInfo",{{"name","x"}}}});
    Check(init.at("protocolVersion")=="2025-06-18","version kept");
    Check(init.at("serverInfo").at("name")=="scct-reloaded" && init.at("serverInfo").at("title")=="SCCT Versus Reloaded Editor (RE+)" && init.at("serverInfo").at("version")=="test-1","server info");
    Check(init.at("capabilities").at("tools").at("listChanged")==true && init.at("capabilities").at("resources").at("subscribe")==true && init.at("capabilities").contains("logging"),"capabilities");
    const auto instructions=init.at("instructions").get<std::string>();
    for(const char* step:{"editor_status","scct://wiki/index","scct://rules","level_spec_validate","level_build","map_validate","map_file"})
        Check(instructions.find(step)!=std::string::npos,"instructions walk through the workflow");
    Check(h.Result("initialize",{{"protocolVersion","1999-01-01"}}).at("protocolVersion")=="2025-06-18","unknown versions get 2025-06-18");
    Check(h.Result("initialize",{{"protocolVersion","2024-11-05"}}).at("protocolVersion")=="2024-11-05","old versions kept");
    Check(h.Result("ping").empty(),"ping");
    h.Send({{"jsonrpc","2.0"},{"method","notifications/initialized"}});
    Check(h.out.size()==4,"notifications get no reply");
}
void Errors()
{
    Harness h;
    h.server->Handle("{not json");
    auto parse=h.Find(nullptr);
    Check(parse.at("error").at("code")==-32700,"parse error with a null id");
    h.Clear();
    h.Send({{"id","no-version"},{"method","ping"}});
    Check(h.Find("no-version").at("error").at("code")==-32600,"invalid request");
    Check(h.Request("no/such").at("error").at("code")==-32601,"unknown method");
    Check(h.Request("tools/call",{{"name","no_such_tool"}}).at("error").at("code")==-32602,"unknown tool is a protocol error");
    Check(h.Request("tools/call",{{"arguments",Json::object()}}).at("error").at("code")==-32602,"tool name required");
    Check(h.Request("logging/setLevel",{{"level","loud"}}).at("error").at("code")==-32602,"bad log level");
    Check(h.Request("logging/setLevel",{{"level","warning"}}).contains("result"),"log level set");
    Check(h.Request("resources/read",{{"uri","scct://nothing"}}).at("error").at("code")==-32002,"unknown resource");
    Check(h.Request("prompts/get",{{"name","versus_starter"}}).at("error").at("code")==-32602,"missing prompt argument");
    // Tool failures are results the agent reads, never protocol errors.
    auto missing=h.Call("map_query",Json::object());
    Check(missing.at("isError")==true && Text(missing).find("arguments.what is required")!=std::string::npos,"schema checked before the tool runs");
    auto unknown=h.Call("map_query",{{"what","summary"},{"colour","red"}});
    Check(unknown.at("isError")==true && Text(unknown).find("colour is not a known field")!=std::string::npos,"unknown argument named");
    auto busy=h.Call("test_throw");
    Check(busy.at("isError")==true && busy.at("structuredContent").at("code")=="busy" && busy.at("structuredContent").at("retryable")==true,"bridge errors keep their code");
    Check(Text(busy).find("A dialog is open in the editor")!=std::string::npos,"busy reasons explained");
    // A batch is answered message by message.
    h.server->Handle(R"([{"jsonrpc":"2.0","id":"b1","method":"ping"},{"jsonrpc":"2.0","id":"b2","method":"ping"}])");
    Check(h.Find("b1").contains("result") && h.Find("b2").contains("result"),"batch");
}
void Lists()
{
    Harness h;
    auto tools=h.Result("tools/list").at("tools");
    std::set<std::string> names;
    for(const auto& t:tools)
    {
        names.insert(t.at("name").get<std::string>());
        Check(t.at("inputSchema").at("type")=="object","object input schemas");
        Check(t.at("description").get<std::string>().size()>20,"every tool explains itself");
        Check(t.contains("annotations") && t.at("annotations").contains("readOnlyHint"),"annotations");
    }
    for(const char* name:{"editor_status","editor_launch","editor_restart","editor_undo","approval_wait","editor_log","map_file","map_build",
                          "map_query","view_screenshot","plan_render","actors_edit","map_changes_apply","workflow_request","editor_exec"})
        Check(names.count(name)!=0,"tool listed");
    for(const auto& t:tools)
    {
        if(t.at("name")=="map_query")Check(t.at("annotations").at("readOnlyHint")==true && t.at("title")=="Read the map","map_query is read-only");
        if(t.at("name")=="actors_edit")Check(t.at("annotations").at("destructiveHint")==true && t.contains("outputSchema"),"actors_edit is destructive and has an output schema");
        if(t.at("name")=="map_file")Check(t.at("inputSchema").at("properties").contains("dryRun") && t.at("inputSchema").at("properties").contains("label"),"mutating tools take dryRun and label");
    }
    Harness old("2024-11-05");
    const auto oldTools=old.Result("tools/list").at("tools");
    for(const auto& t:oldTools)Check(!t.contains("outputSchema") && !t.contains("annotations") && !t.contains("title"),"2024-11-05 shape");

    auto resources=h.Result("resources/list").at("resources");
    std::set<std::string> uris;
    for(const auto& r:resources)uris.insert(r.at("uri").get<std::string>());
    for(const char* uri:{"scct://rules","scct://editor/state","scct://map/summary","scct://map/validation","scct://map/spec","scct://map/screenshot/latest"})
        Check(uris.count(uri)!=0,"resource listed");
    auto templates=h.Result("resources/templates/list").at("resourceTemplates");
    std::set<std::string> patterns;
    for(const auto& r:templates)patterns.insert(r.at("uriTemplate").get<std::string>());
    Check(patterns.count("scct://wiki/{page}") && patterns.count("scct://schema/{name}") && patterns.count("scct://examples/{name}") && patterns.count("scct://map/actor/{path}"),"templates listed");

    auto prompts=h.Result("prompts/list").at("prompts");
    Check(prompts.size()==1 && prompts[0].at("name")=="versus_starter" && prompts[0].at("arguments")[0].at("required")==true,"prompts listed");
    auto prompt=h.Result("prompts/get",{{"name","versus_starter"},{"arguments",{{"theme","harbour"}}}});
    Check(prompt.at("messages")[0].at("content").at("text")=="Build a harbour map." && prompt.at("description")=="A starter level.","prompt messages");
}
void Status()
{
    Harness h;
    h.fake.On("editor.state",OpenState());
    auto status=h.Call("editor_status");
    Check(status.at("isError")==false && status.at("structuredContent").at("connected")==true,"connected");
    Check(status.at("structuredContent").at("bridge").at("pid")==4242 && status.at("structuredContent").at("state").at("revision")==11,"bridge and state");
    Check(Text(status).find("Connected to RE+ test")!=std::string::npos && Text(status).find("an unsaved map")!=std::string::npos,"summary line");
    Check(status.at("structuredContent").at("settings").at("enabled")==false,"INI read (absent here)");
    h.fake.connected=false;
    auto offline=h.Call("editor_status");
    Check(offline.at("isError")==false && offline.at("structuredContent").at("connected")==false,"not connected is a status, not an error");
    Check(offline.at("structuredContent").at("error").at("code")=="not_connected","why");
    h.fake.connected=true;
    auto chosen=h.Call("editor_status",{{"pid",9999}});
    Check(chosen.at("isError")==true,"an unknown pid is refused");
    h.Call("editor_status",{{"pid",4242}});
    Check(h.fake.selected==4242,"pid selects the editor");
    // Structured content is only for clients that speak 2025-06-18.
    Harness legacy("2025-03-26");
    legacy.fake.On("editor.state",OpenState());
    auto old=legacy.Call("editor_status");
    Check(!old.contains("structuredContent") && Text(old).find("\"connected\": true")!=std::string::npos,"text carries the JSON for old clients");
}
void Queries()
{
    Harness h;
    h.fake.On("editor.state",OpenState());
    Json actors=Json::array();
    for(int i=0;i<3;++i)actors.push_back({{"path","MyLevel.PlayerStart"+std::to_string(i)},{"class","Engine.PlayerStart"},{"name","PlayerStart"+std::to_string(i)}});
    actors.push_back({{"path","MyLevel.Light1"},{"class","Engine.Light"},{"name","Light1"}});
    h.fake.On("actors.query",Json{{"actors",actors},{"total",4}});
    Json objectives=Json::array();
    for(int i=0;i<3;++i)objectives.push_back({{"path","MyLevel.PlayerStart"+std::to_string(i)},{"class","Engine.PlayerStart"},{"kind","Player start"},{"team",i==2?"1":"0"}});
    objectives.push_back({{"path","MyLevel.SMission0"},{"class","SBase.SMission"},{"kind","Mission"},{"name","Main"},{"objectives",{"MyLevel.SObjective0"}},{"minimum",1}});
    objectives.push_back({{"path","MyLevel.SObjective0"},{"class","SBase.SObjective"},{"kind","Objective"},{"name","Hack"},{"tag","Hack"}});
    objectives.push_back({{"path","MyLevel.SComputerObjectiveTrigger0"},{"class","SBase.SComputerObjectiveTrigger"},{"kind","Computer terminal"},{"tag","Terminal"}});
    h.fake.On("objective.actors",objectives);
    h.fake.On("security.actors",Json::array({{{"path","MyLevel.SAlarm0"},{"kind","Alarm"}}}));
    h.fake.On("camera.snapshot",Json::array({{{"actor",{{"path","MyLevel.SCamNetwork0"}}},{"values",{{"bFirstCam","True"}}},{"schema",Json::object()}}}));
    auto summary=h.Call("map_query",{{"what","summary"}});
    const auto& s=summary.at("structuredContent");
    Check(summary.at("isError")==false && s.at("actors").at("total")==4 && s.at("actors").at("classes").at("Engine.PlayerStart")==3,"actor counts");
    Check(s.at("spawns").at("spy")==2 && s.at("spawns").at("merc")==1,"spawns per team");
    Check(s.at("story").at("missions").size()==1 && s.at("story").at("objectives")==1 && s.at("story").at("devices").at("Computer terminal")==1,"story");
    Check(s.at("cameras").at("count")==1 && s.at("cameras").at("firstCams")==1 && s.at("security").at("Alarm")==1,"cameras and security");
    Check(!s.at("notes").empty() && Text(summary).find("spawns 2 spy / 1 merc")!=std::string::npos,"too few spawns noted");

    auto listed=h.Call("map_query",{{"what","actors"},{"classes",{"Engine.Light"}},{"fields","all"},{"limit",50}});
    const auto* query=h.fake.Last("actors.query");
    Check(listed.at("isError")==false && query->args.at("classes")[0]=="Engine.Light" && query->args.at("fields")=="all" && query->args.at("limit")==50,"filters passed through");

    h.fake.On("magic.inspect",Json{{"actor",{{"path","MyLevel.Light1"},{"class","Engine.Light"}}},{"values",{{"LightBrightness","64"}}},{"schema",{{"LightBrightness",Json::object()}}}});
    h.fake.On("connections",Json::array({{{"from","MyLevel.Light1"},{"to","Lamp"},{"resolved",false}}}));
    auto actor=h.Call("map_query",{{"what","actor"},{"actor","light1"}});
    Check(actor.at("isError")==false && actor.at("structuredContent").at("values").at("LightBrightness")=="64","actor by name");
    Check(!actor.at("structuredContent").contains("schema") && actor.at("structuredContent").at("links").size()==1,"schema left out unless asked");
    Check(h.fake.Last("magic.inspect")->args.at("actor").at("class")=="Engine.Light","resolved identity used");
    auto none=h.Call("map_query",{{"what","actor"},{"actor","Nothing9"}});
    Check(none.at("isError")==true && Text(none).find("No actor named")!=std::string::npos,"missing actor explained");

    h.fake.On("design.scene",Json::array({{{"path","MyLevel.Brush3"},{"class","Engine.Brush"},{"csg",2},{"edges",{{{0,0,0},{256,0,0}},{{256,0,0},{256,128,64}}}}},
                                           {{"path","MyLevel.Light1"},{"class","Engine.Light"},{"position",{10,20,30}}}}));
    auto design=h.Call("map_query",{{"what","design"}});
    const auto& item=design.at("structuredContent").at("items")[0];
    Check(design.at("structuredContent").at("brushes")==1 && item.at("bounds").at("max")[1]==128 && !item.contains("edges") && item.at("edgeCount")==2,"design bounds instead of edges");
    auto plan=h.Call("plan_render");
    Check(plan.at("isError")==false && plan.at("structuredContent").at("pieces")==1 && plan.at("structuredContent").at("actors")==1,"live plan scene");
}
void Screenshot()
{
    Harness h;
    h.fake.On("view.screenshot",Json{{"png","iVBORw0KGgo="},{"width",640},{"height",480},{"viewport","perspective"}});
    h.fake.On("objective.actors",Json::array({{{"path","MyLevel.PlayerStart0"},{"class","Engine.PlayerStart"},{"kind","Player start"},{"team","0"}},
                                               {{"path","MyLevel.PlayerStart1"},{"class","Engine.PlayerStart"},{"kind","Player start"},{"team","1"}}}));
    auto shot=h.Call("view_screenshot",{{"preset","spawn:spy"},{"maxEdge",640}});
    Check(shot.at("isError")==false && shot.at("content")[1].at("type")=="image" && shot.at("content")[1].at("data")=="iVBORw0KGgo=","image content");
    const auto* call=h.fake.Last("view.screenshot");
    Check(call->args.at("frame").size()==1 && call->args.at("frame")[0].at("path")=="MyLevel.PlayerStart0" && call->args.at("maxEdge")==640,"preset frames the spy starts");
    Check(!shot.at("structuredContent").contains("png"),"no base64 in structured content");
}
void Mutations()
{
    Harness h;
    h.fake.On("editor.state",OpenState());
    h.Result("resources/subscribe",{{"uri","scct://editor/state"}});
    h.Result("resources/subscribe",{{"uri","scct://rules"}});
    h.fake.Progress("design.build",Json::array({{{"t","progress"},{"stage","building"}}}));
    h.fake.On("design.build",Json(true));
    h.fake.On("mapcheck.run",Json{{"rows",Json::array({{{"severity","warning"},{"message","Brush is not closed"}},{{"severity","error"},{"message","No PlayerStart"}}})},{"summary","1 error"}});
    h.Clear();
    auto built=h.Call("map_build",Json::object(),{{"progressToken","p1"}});
    Check(built.at("isError")==false && Text(built).find("Map Check: 1 error, 1 warning")!=std::string::npos,"build then Map Check");
    Check(built.at("structuredContent").at("mapcheck").at("rows")[0].at("severity")=="error","errors first");
    auto progress=h.Notes("notifications/progress");
    Check(progress.size()>=2 && progress[0].at("params").at("progressToken")=="p1","progress notifications carry the token");
    for(size_t i=1;i<progress.size();++i)Check(progress[i].at("params").at("progress").get<double>()>progress[i-1].at("params").at("progress").get<double>(),"progress rises");
    auto updates=h.Notes("notifications/resources/updated");
    Check(updates.size()==1 && updates[0].at("params").at("uri")=="scct://editor/state","subscribers of live resources hear about changes");
    h.Clear();
    h.Call("map_build",{{"dryRun",true}});
    Check(h.Notes("notifications/resources/updated").empty(),"a dry run changes nothing");
    Check(h.fake.Last("design.build")->dryRun,"dryRun reaches the bridge");
    h.server->OnBridgeEvent({{"t","event"},{"kind","map_changed"}});
    Check(h.Notes("notifications/resources/updated").size()==1,"bridge events notify too");
    h.Result("resources/unsubscribe",{{"uri","scct://editor/state"}});
    h.Clear();
    h.server->OnBridgeEvent({{"t","event"},{"kind","map_changed"}});
    Check(h.Notes("notifications/resources/updated").empty(),"unsubscribed");

    // Undo: the bridge's undo record comes back in the result and is editor_undo's default.
    auto result=h.Call("map_file",{{"action","save_as"},{"mapName","AQUARIUM"}});
    Check(result.at("isError")==true || h.fake.Count("map.save")==0,"no script for map.save yet");
    h.fake.On("map.save",Json{{"file","C:/Game/Packages/MapsEd/AquaD.sdc"},{"saved",true}});
    result=h.Call("map_file",{{"action","save_as"},{"mapName","AQUARIUM"}});
    Check(result.at("isError")==false && h.fake.Last("map.save")->args.at("path")=="AquaD.sdc","the wiki's four letters plus D");
    Check(result.at("structuredContent").at("next")[0].at("tool")=="editor_restart","a first save suggests the restart Map Settings needs");
    const auto saves=h.fake.Count("map.save");
    auto badName=h.Call("map_file",{{"action","save_as"},{"path","Aqua D"}});
    Check(badName.at("isError")==true && h.fake.Count("map.save")==saves,"names with spaces refused before the bridge");
    auto dirty=OpenState();dirty["dirty"]=true;
    h.fake.Clear("editor.state");h.fake.On("editor.state",dirty);
    auto refused=h.Call("map_file",{{"action","new"}});
    Check(refused.at("isError")==true && Text(refused).find("unsaved changes")!=std::string::npos && h.fake.Count("map.new")==0,"unsaved changes are not discarded unless asked");
    h.fake.On("map.new",OpenState());
    Check(h.Call("map_file",{{"action","new"},{"discardUnsaved",true}}).at("isError")==false && h.fake.Last("map.new")->args.at("discard")==true,"discard when asked");

    h.fake.On("authoring.preview",Json{{"rows",Json::array({{{"change","Add"},{"target","Lamp"}}})},{"tally","1 added"}});
    Json document{{"format","scct.map-changes"},{"version",2},{"map","*"},{"operations",Json::array({{{"op","create"},{"id","Lamp"},{"class","Engine.Light"},{"properties",{{"Location",{{"X","0"},{"Y","0"},{"Z","64"}}}}}}})}};
    auto preview=h.Call("map_changes_apply",{{"document",document},{"dryRun",true}});
    Check(preview.at("isError")==false && h.fake.Count("authoring.apply")==0 && Text(preview).find("1 added")!=std::string::npos,"dry run previews");
    h.fake.On("authoring.apply",Json{{"identities",{{"Lamp",{{"path","MyLevel.Light5"},{"class","Engine.Light"}}}}},{"deleted",Json::array()}});
    auto applied=h.Call("map_changes_apply",{{"document",document},{"label","Add a lamp"}});
    const auto& r=applied.at("structuredContent");
    Check(applied.at("isError")==false && r.at("created")[0].at("identity").at("path")=="MyLevel.Light5","created identities");
    Check(r.at("undo").at("call").get<unsigned>()>0 && h.fake.Last("authoring.apply")->label=="Add a lamp","undo block and label");
    const auto call=r.at("undo").at("call").get<unsigned>();
    h.fake.On("undo.call",Json{{"steps",1}});
    auto undone=h.Call("editor_undo");
    Check(undone.at("isError")==false && h.fake.Last("undo.call")->args.at("call")==call,"editor_undo undoes the last MCP call");
    Check(h.Call("map_changes_apply",{{"document",{{"format","scct.level-spec"}}}}).at("isError")==true,"a spec is not a change file");
}
void ActorsEdit()
{
    Harness h;
    h.fake.On("actors.query",Json{{"actors",Json::array({{{"path","MyLevel.Light1"},{"class","Engine.Light"},{"name","Light1"}},
                                                          {{"path","MyLevel.Door2"},{"class","Engine.Mover"},{"name","Door2"}}})},{"total",2}});
    h.fake.On("magic.inspect",[](const Json& a,const CallOptions&){return Json{{"actor",a.at("actor")},{"values",{{"Tag","\""+a.at("actor").at("path").get<std::string>()+"\""}}}};});
    h.fake.On("authoring.mapid",Json{{"map","c:\\game\\packages\\mapsed\\aquad.sdc"}});
    h.fake.On("mcp.plan",Json{{"steps",{{"e1",{{"deleted",Json::array()}}},{"e2",{{"moved",1}}}}},{"undo",{{"steps",1}}}});
    auto refused=h.Call("actors_edit",{{"edits",Json::array({{{"action","delete"},{"actor","MyLevel.Door2"}}})}});
    Check(refused.at("isError")==true && Text(refused).find("allowDeletes")!=std::string::npos && h.fake.Count("mcp.plan")==0,"deletes need allowDeletes");
    auto edited=h.Call("actors_edit",{{"allowDeletes",true},{"label","Tidy"},{"edits",Json::array({
        {{"action","set"},{"actor","Light1"},{"properties",{{"LightBrightness","96"}}}},
        {{"action","move"},{"actor","MyLevel.Light1"},{"delta",{0,0,64}}},
        {{"action","delete"},{"actor","Door2"}},
        {{"action","set"},{"actor","Light1"},{"properties",{{"LightHue","20"}}}}})}});
    Check(edited.at("isError")==false,"edit runs");
    const auto plan=h.fake.Last("mcp.plan")->args;
    Check(plan.at("label")=="Tidy" && plan.at("steps").size()==2,"one plan: the document, then the moves");
    const auto& doc=plan.at("steps")[0].at("args").at("document");
    Check(plan.at("steps")[0].at("op")=="authoring.apply" && doc.at("version")==3 && doc.at("allowDeletes")==true && doc.at("map")=="c:\\game\\packages\\mapsed\\aquad.sdc","v3 document with the exported map id");
    Check(doc.at("operations")[0].at("op")=="delete" && doc.at("operations")[0].at("before").at("Tag")=="\"MyLevel.Door2\"","delete first, with its before");
    Check(doc.at("operations")[1].at("op")=="update" && doc.at("operations")[1].at("properties").size()==2,"sets on one actor merged into one update");
    Check(plan.at("steps")[1].at("op")=="placement.move" && plan.at("steps")[1].at("args").at("moves")[0].at("delta")[2]==64,"move step");
    Check(edited.at("structuredContent").at("undo").at("steps")==1,"undo block");
    Check(Text(h.Call("actors_edit",{{"edits",Json::array({{{"action","explode"},{"actor","Light1"}}})}})).find("edits[0].action must be one of set")!=std::string::npos,"unknown action explained");
}
void Escape()
{
    Harness h;
    Check(Text(h.Call("workflow_request",{{"op","light.set"}})).find("not available over MCP")!=std::string::npos,"blocked ops refused");
    Check(Text(h.Call("workflow_request",{{"op","exec"}})).find("editor_exec")!=std::string::npos,"raw ops go through editor_exec");
    Check(Text(h.Call("workflow_request",{{"op","no.such"}})).find("no op")!=std::string::npos,"unknown op");
    h.fake.On("security.actors",Json::array());
    auto read=h.Call("workflow_request",{{"op","security.actors"}});
    Check(read.at("isError")==false && read.at("structuredContent").at("result").is_array(),"reads pass through");
    // A misordered plan never reaches the editor.
    Json plan{{"steps",Json::array({{{"op","placement.copy"},{"args",{{"members",Json::array()},{"copies",Json::array()}}}},
                                    {{"op","security.delete"},{"args",{{"actor",{{"path","a"},{"class","b"}}}}}}})}};
    auto misordered=h.Call("workflow_request",{{"op","mcp.plan"},{"args",plan}});
    Check(misordered.at("isError")==true && Text(misordered).find("deletes or replaces actors after")!=std::string::npos && h.fake.Count("mcp.plan")==0,"plans checked locally");
    h.fake.Fail("exec",Bridge::Code::Refused,"Raw editor commands are off.","raw_exec_off");
    auto exec=h.Call("editor_exec",{{"command","STAT FPS"}});
    Check(exec.at("isError")==true && Text(exec).find("AllowRawExec=1")!=std::string::npos,"raw exec off explained");
}
void Resources()
{
    Harness h;
    auto rules=h.Result("resources/read",{{"uri","scct://rules"}});
    Check(rules.at("contents")[0].at("mimeType")=="text/markdown" && !rules.at("contents")[0].at("text").get<std::string>().empty(),"rules");
    h.fake.On("editor.state",OpenState());
    auto state=h.Result("resources/read",{{"uri","scct://editor/state"}});
    Check(Json::parse(state.at("contents")[0].at("text").get<std::string>()).at("revision")==11,"live state");
    h.fake.On("view.screenshot",Json{{"png","AAAA"},{"width",4},{"height",4}});
    auto shot=h.Result("resources/read",{{"uri","scct://map/screenshot/latest"}});
    Check(shot.at("contents")[0].at("blob")=="AAAA" && shot.at("contents")[0].at("mimeType")=="image/png","screenshot blob");
    h.fake.On("mcp.sidecar.get",Json{{"sidecar",nullptr}});
    Check(h.Result("resources/read",{{"uri","scct://map/spec"}}).at("contents")[0].at("text")=="null","no spec yet");
    h.fake.On("mapcheck.run",Json{{"rows",Json::array()}});
    Check(h.Request("resources/read",{{"uri","scct://map/validation"}}).contains("result"),"validation falls back to Map Check without map_validate");
    h.fake.connected=false;
    auto offline=h.Request("resources/read",{{"uri","scct://map/summary"}});
    Check(offline.at("error").at("code")==-32603 && offline.at("error").at("message").get<std::string>().find("editor_launch")!=std::string::npos,"live reads explain a missing editor");
    Check(h.Request("resources/read",{{"uri","scct://wiki/no-such-page"}}).at("error").at("code")==-32002,"template miss is not found");
}
void Approval()
{
    Harness h;
    h.fake.On("approval.status",Json{{"ticket","t5-1"},{"status","pending"}});
    h.fake.On("approval.status",Json{{"ticket","t5-1"},{"status","done"},{"outcome",{{"ok",true},{"result",{{"x",1}}},{"undo",{{"call",12},{"steps",1}}}}}});
    auto approved=h.Call("approval_wait",{{"ticket","t5-1"},{"timeoutSeconds",10}},{{"progressToken","a"}});
    Check(approved.at("isError")==false && approved.at("structuredContent").at("result").at("x")==1 && approved.at("structuredContent").at("undo").at("call")==12,"approved result and its undo");
    Check(h.fake.Count("approval.status")==2 && h.fake.Last("approval.status")->args.at("ticket")=="t5-1","polled until done");
    Check(!h.Notes("notifications/progress").empty(),"waiting reports progress");
    h.fake.On("undo.call",Json{{"steps",1}});
    h.fake.On("editor.state",OpenState());
    h.Call("editor_undo");
    Check(h.fake.Last("undo.call")->args.at("call")==12,"the approved call is what editor_undo reverts");
    h.fake.Clear("approval.status");
    h.fake.On("approval.status",Json{{"ticket","t6-2"},{"status","denied"}});
    auto denied=h.Call("approval_wait",{{"ticket","t6-2"}});
    Check(denied.at("isError")==true && denied.at("structuredContent").at("code")=="denied" && Text(denied).find("declined")!=std::string::npos,"a declined change is an error with advice");
    h.fake.Clear("approval.status");
    h.fake.On("approval.status",Json{{"pending",true}});
    auto waiting=h.Call("approval_wait",{{"ticket","t7"},{"timeoutSeconds",1}});
    Check(waiting.at("isError")==false && waiting.at("structuredContent").at("pending")==true,"still pending after the wait");
}
Json Message(const Json& id,const std::string& method,const Json& params)
{
    Json m{{"jsonrpc","2.0"},{"method",method},{"params",params}};
    if(!id.is_null())m["id"]=id;
    return m;
}
void Cancellation()
{
    Harness h;
    h.Send(Message(99,"tools/call",{{"name","test_block"},{"arguments",Json::object()},{"_meta",{{"progressToken",7}}}}));
    while(!h.gate.started)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    h.Send(Message(100,"tools/call",{{"name","editor_log"},{"arguments",Json::object()}}));
    h.Send(Message(101,"ping",Json::object()));
    Check(h.Find(101).contains("result"),"ping is answered while a tool runs");
    h.Send(Message(nullptr,"notifications/cancelled",{{"requestId",100},{"reason","changed my mind"}}));
    Check(h.cancels==0,"a queued request is dropped without touching the bridge");
    h.Send(Message(nullptr,"notifications/cancelled",{{"requestId",99}}));
    h.server->Wait();
    Check(h.cancels==1,"the running request asks the bridge to stop");
    Check(h.Find(99).is_null() && h.Find(100).is_null(),"cancelled requests get no reply");
    Check(h.fake.Count("log.tail")==0,"the dropped request never ran");
    const auto progress=h.Notes("notifications/progress");
    Check(progress.size()==1 && progress[0].at("params").at("progressToken")==7,"numeric progress tokens");
    Check(h.resumes>=1,"the bridge is resumed before each request");
}
void Launch()
{
    Harness h;
    {
        std::ofstream(h.dir/"Reloaded_Editor.exe")<<"MZ";
        std::ofstream(h.dir/"Reloaded_Editor.ini")<<"[General]\r\nEnabled=0\r\n[Mcp]\r\nEnabled=1\r\nRequireApproval=1\r\n";
    }
    auto& process=Process();
    const auto saved=process;
    std::vector<std::string> launched;
    process.launch=[&](const std::filesystem::path& exe,const std::string& arguments,const std::filesystem::path& folder)
    {
        launched.push_back(exe.filename().string()+" "+arguments+" in "+folder.filename().string());
        h.fake.editors=Json::array({{{"pid",5151}}});
        return 5151ul;
    };
    process.running=[](unsigned long){return true;};
    process.waitExit=[](unsigned long,unsigned){return true;};
    h.fake.On("editor.state",OpenState());
    auto already=h.Call("editor_launch");
    Check(already.at("structuredContent").at("alreadyRunning")==true && launched.empty(),"a running editor is reused");
    h.fake.editors=Json::array();
    auto status=h.Call("editor_status");
    Check(status.at("structuredContent").at("settings").at("enabled")==true && status.at("structuredContent").at("settings").at("requireApproval")==true,"INI [Mcp] read");
    auto started=h.Call("editor_launch",{{"waitSeconds",10}});
    Check(started.at("isError")==false && launched.size()==1 && launched[0]=="Reloaded_Editor.exe -reMcp in "+h.dir.filename().string(),"started with -reMcp in System");
    Check(h.fake.selected==5151 && started.at("structuredContent").at("pid")==5151,"selects the new editor");
    // Restart: save, quit, wait, start again, reopen.
    auto saved_state=OpenState();saved_state["mapFile"]="C:/Game/Packages/MapsEd/AquaD.sdc";saved_state["dirty"]=true;
    h.fake.Clear("editor.state");h.fake.On("editor.state",saved_state);
    h.fake.On("map.save",Json{{"file","C:/Game/Packages/MapsEd/AquaD.sdc"}});
    h.fake.On("editor.quit",[&](const Json&,const CallOptions&){h.fake.editors=Json::array();return Json(true);});
    h.fake.On("map.open",saved_state);
    auto restart=h.Call("editor_restart");
    Check(restart.at("isError")==false && launched.size()==2,"restarted");
    const auto ops=h.fake.Ops();
    auto index=[&](const char* op){return std::find(ops.begin(),ops.end(),op)-ops.begin();};
    Check(index("map.save")<index("editor.quit") && index("editor.quit")<static_cast<long>(ops.size()) && h.fake.Last("map.open")->args.at("path")=="C:/Game/Packages/MapsEd/AquaD.sdc","save, quit, reopen");
    Check(h.fake.Last("editor.quit")->args.at("discard")==false && h.fake.disconnects==1,"nothing discarded; pipe dropped");
    {std::ofstream(h.dir/"Reloaded_Editor.ini")<<"[Mcp]\r\nEnabled=0\r\n";}
    h.fake.editors=Json::array();
    auto off=h.Call("editor_launch");
    Check(off.at("isError")==true && Text(off).find("Enable MCP server")!=std::string::npos && launched.size()==2,"an editor with MCP off is not started");
    process=saved;
}
void Stub()
{
    BridgeClient client({});
#ifndef _WIN32
    bool refused=false;
    try{client.Call("editor.state",Json::object());}catch(const Bridge::Error& e){refused=e.code==Bridge::Code::NotConnected;}
    Check(refused && !client.Connected() && client.Editors().empty(),"no bridge off Windows");
#endif
    std::istringstream input("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\r\n\n{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
                             "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"editor_status\"}}");
    Registry registry;RegisterSessionTools(registry);
    std::vector<std::string> lines;
    Server server(registry,client,{},[&](const std::string& line){lines.push_back(line);});
    Check(server.Run(input)==0 && lines.size()==3 && Json::parse(lines[1]).at("result").at("tools").size()==8,"Run serves a stream to its end");
    Check(Json::parse(lines[2]).at("result").at("structuredContent").at("connected")==false,"a request queued before the input ended is still answered");
}
int main()
{
    try{Handshake();Errors();Lists();Status();Queries();Screenshot();Mutations();ActorsEdit();Escape();Resources();Approval();Cancellation();Launch();Stub();}
    catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
    std::cout<<"McpServer passed\n";
    return 0;
}
