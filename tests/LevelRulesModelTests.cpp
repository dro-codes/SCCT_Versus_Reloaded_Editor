#include "../Reloaded.Editor.Mcp/LevelRulesModel.h"
#include <functional>
#include <iostream>
#include <source_location>
using namespace Mcp;
using Workflow::Json;
using Workflow::Vector;
void Check(bool value,const std::string& message,std::source_location where=std::source_location::current())
{
    if(!value)throw std::runtime_error(message+" (line "+std::to_string(where.line())+")");
}
template<class F> void Reject(F f,std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception&){caught=true;}
    if(!caught)throw std::runtime_error("invalid input accepted at line "+std::to_string(where.line()));
}

// ------------------------------------------------------------- fixtures
Json Ref(const std::string& id){return {{"$ref",id}};}
Json A(const std::string& id,const std::string& cls,Vector at,Json values=Json::object(),const std::string& tag="",const std::string& event="")
{
    Json a{{"path",id},{"class",cls},{"position",at},{"values",values}};
    if(!tag.empty())a["tag"]=tag;
    if(!event.empty())a["event"]=event;
    return a;
}
Json Piece(const std::string& kind,const std::string& name,double w,double l,double h,Vector at)
{
    return {{"spec",{{"kind",kind},{"name",name},{"width",w},{"length",l},{"height",h},{"thickness",16},{"construction","Carve"}}},{"position",at},{"rotation",{0,0,0}}};
}
// A small Versus map that follows every rule: a spy room, an objective room
// and a merc room in a row along +Y, joined by corridors; all three modes;
// a CamNet loop; one alarm; GEs within the wiki's numbers; Map Settings,
// interface textures and release packages in order.
Json Good()
{
    Json actors=Json::array();
    auto add=[&](Json a){actors.push_back(std::move(a));};
    for(int i=0;i<4;++i)add(A("SpyStart"+std::to_string(i+1),"Engine.PlayerStart",{i%2?200.0:-200.0,i/2?200.0:-200.0,48},{{"TeamNumber","0"}}));
    for(int i=0;i<4;++i)add(A("MercStart"+std::to_string(i+1),"Engine.PlayerStart",{i%2?200.0:-200.0,3000+(i/2?200.0:-200.0),48},{{"TeamNumber","1"}}));
    add(A("Mission","SBase.SMission",{0,100,100},{{"GameMode","GM_Multi"},{"bChained","False"},{"MinimumObjectives","1"},{"Objectives",{Ref("Obj1"),Ref("Obj2")}}}));
    add(A("Obj1","SBase.SObjective",{0,1500,100},{{"Triggers",{Ref("Term1")}},{"TriggersERVisibilityZone",Ref("ZoneObj")},{"ObjectiveName","\"Server\""}}));
    add(A("Obj2","SBase.SObjective",{100,1500,100},{{"Triggers",{Ref("Flag1")}}}));
    add(A("Term1","SBase.SComputerObjectiveTrigger",{0,1500,78},{{"DominationCaptureTime","10.000000"}}));
    add(A("Flag1","SBase.SFlag",{200,1500,40},{{"DropZone",{Ref("Drop1")}}}));
    add(A("Drop1","SBase.SFlagDropZone",{-200,1700,0}));
    Json disks=Json::array();
    for(int i=0;i<4;++i)
    {
        const auto id="Disk"+std::to_string(i+1);
        add(A(id,"SBase.SDisk",{-250.0+150*i,1300,40},{{"Group",std::to_string(i%3+1)}}));
        disks.push_back(Ref(id));
    }
    add(A("DiskMission","SBase.SMission",{0,120,100},{{"GameMode","GM_DiskHunt"},{"Triggers",disks}}));
    add(A("DMMission","SBase.SMission",{0,140,100},{{"GameMode","GM_DeathMatch"},{"AtResetEvent","DMReset"}}));
    add(A("DMReset","SBase.SMagicEvent",{0,0,200},Json::object(),"DMReset"));
    const char* cams[]={"Cam1","Cam2","Cam3"};
    for(int i=0;i<3;++i)
        add(A(cams[i],"SBase.SCamNetwork",{0,1500.0*i,200},{{"NextCam",Ref(cams[(i+1)%3])},{"PrevCam",Ref(cams[(i+2)%3])},{"bFirstCam",i==0?"True":"False"},
                                                          {"bBlockActors","True"},{"bBlockPlayers","True"},{"CamName","\"Camera\""}}));
    add(A("Alarm1","SBase.SAlarm",{0,1400,100},{{"Events",{"AlarmDoor"}},{"MessagePriority","5"},{"ObjectiveTriggersToLock",{Ref("Term1")}},
                                                {"MoversToLock",Json::array({{{"Mover",Ref("Door1")},{"Action","ML_Lock"}}})}},"Alarm1"));
    add(A("Laser1","SBase.SLaserTrigger",{0,1200,100},Json::object(),"","Alarm1"));
    add(A("Door1","Engine.Mover",{0,1100,0},{{"StaticMesh","StaticMesh'Pkg.Grp.Door'"}},"AlarmDoor"));
    add(A("ZoneSpy","Engine.ZoneInfo",{0,0,100},{{"LocationName","\"Spy base\""}}));
    add(A("ZoneObj","Engine.ZoneInfo",{0,1500,100},{{"LocationName","\"Server room\""}}));
    add(A("ZoneMerc","Engine.ZoneInfo",{0,3000,100},{{"LocationName","\"Merc base\""}}));
    add(A("Light1","Engine.Light",{0,0,400},{{"LightEffect","LE_None"}}));
    add(A("Light2","Engine.Light",{0,1500,200},{{"LightEffect","LE_ESpotShadow"},{"EchelonNormalInfluence","0.900000"}}));
    add(A("Ammo1","SBase.SAmmoStock",{300,3000,0},{{"fInteractionHeight","60.000000"},{"fInteractionRadius","120.000000"}}));
    add(A("Med1","SBase.SInfirmary",{-300,3000,100},{{"bBlockActors","True"},{"bBlockPlayers","True"},{"fInteractionHeight","120.000000"},{"fInteractionRadius","60.000000"}}));
    add(A("Sprinkler1","SBase.SSprinkler",{0,1500,240},{{"bHidden","True"}}));
    add(A("Tips1","SBase.SZoneTips",{0,0,100},{{"bHidden","True"}}));
    add(A("Fish1","SBase.SFish",{0,0,10},{{"Pilot",Ref("FishLead")}}));
    add(A("FishLead","SBase.SFish_Big",{0,0,10},{{"bPilote","True"}}));
    add(A("Warn3","SBase.SMessageTrigger",{0,0,0},Json::object(),"Warning3min"));
    Json pieces=Json::array({Piece("Room","Spy room",1024,1024,512,{0,0,0}),Piece("Corridor","Corridor A",192,556,192,{0,806,0}),Piece("Room","Server room",800,800,256,{0,1500,0}),
                             Piece("Corridor","Corridor B",192,556,192,{0,2194,0}),Piece("Room","Merc room",1024,1024,256,{0,3000,0})});
    Json ges=Json::array({{{"kind","ladder"},{"points",{{400,400,0},{400,400,256},{370,400,0}}}},
                          {{"kind","ledge"},{"points",{{-300,-400,250},{-172,-400,250}}}},
                          {{"kind","hoh"},{"points",{{-300,300,240},{-44,300,240}}}},
                          {{"kind","zipline"},{"points",{{300,-300,300},{-300,-300,230}}}},
                          {{"kind","pipe"},{"points",{{-400,0,0},{-400,0,300},{-380,0,0}}}}});
    auto texture=[](int size,const char* format){return Json{{"width",size},{"height",size},{"format",format},{"mips",1},{"expected",{{"size",size},{"format",format}}}};};
    Json settings{{"saved",true},{"settings",true},
                  {"textures",{{"Menu",texture(256,"DXT5")},{"Briefing",texture(256,"DXT5")},{"LoadingATT0",texture(512,"DXT1")},{"LoadingDEF0",texture(512,"DXT1")}}},
                  {"values",{{"MapName","\"TESTMAP\""},
                             {"Modes",Json::array({{{"GameModeId","GM_Multi"},{"RespawnTime","10"},{"SpiesLives","0"},{"SentriesLives","0"},{"TimeLimit","10"}},
                                                   {{"GameModeId","GM_DiskHunt"},{"RespawnTime","10"},{"TimeLimit","10"},{"ScoreLimit","3"},{"Handicap","4"},{"NumDisksMax","4"}},
                                                   {{"GameModeId","GM_DeathMatch"},{"RespawnTime","5"},{"TimeLimit","15"}}})},
                             {"ModesStrings",Json::array({Json::object(),Json::object(),Json::object()})}}}};
    Json document{{"format","scct.map-changes"},{"version",2},{"map","*"},{"operations",Json::array({{{"op","create"},{"id","Light1"},{"class","Engine.Light"},{"properties",Json::object()}}})}};
    Json schema{{"classes",{{"SBase.SMission",{{"class","SBase.SMission"},{"schema",{{"GameMode",{{"writable",true},{"choices",{"GM_Undefined","GM_Multi","GM_DiskHunt","GM_DeathMatch"}}}},
                                                                                     {"bChained",{{"writable",true},{"choices",{"False","True"}}}}}}}}}}};
    return {{"actors",actors},{"pieces",pieces},{"ges",ges},{"modes",{"story","diskhunt","deathmatch"}},{"mapName","TESTMAP"},{"mapFile","TestD.sdc"},
            {"level",{{"fDeathPawnHeight","0.000000"},{"Event","Warning3min"},{"SendAtTime","-180"},{"MTVOffset","52"},{"MTVSlope","85"}}},
            {"mapSettings",settings},
            {"release",{{"packages",{"TestD_TXT.utx","TestD_STM.usx","Amb_TestD.uax"}},{"uas",{{"packages",{"Interface","Amb_TestD"}}}}}},
            {"documents",Json::array({document})},{"schema",schema}};
}
Json& Actor(Json& scene,const std::string& id)
{
    for(auto& a:scene.at("actors"))if(a.at("path")==id)return a;
    throw std::runtime_error("fixture has no actor "+id);
}
void Drop(Json& scene,const std::function<bool(const Json&)>& which)
{
    auto& list=scene.at("actors");
    for(auto it=list.begin();it!=list.end();)if(which(*it))it=list.erase(it);else ++it;
}
void Drop(Json& scene,const std::string& id){Drop(scene,[&](const Json& a){return a.at("path")==id;});}
std::vector<Rules::Finding> Run(const Json& scene,const std::string& profile="release"){return Rules::Check(Rules::SceneFromJson(scene),profile);}
const Rules::Finding* Get(const std::vector<Rules::Finding>& findings,const std::string& rule)
{
    for(const auto& f:findings)if(f.rule==rule)return &f;
    return nullptr;
}
std::string Dump(const std::vector<Rules::Finding>& findings,bool all=false)
{
    std::string out;
    for(const auto& f:findings)if(all || f.severity!="info")out+="  "+f.severity+" "+f.rule+": "+f.message+"\n";
    return out;
}

std::set<std::string> failing,passing;
// The rule fires on this scene (at this severity, when given).
Rules::Finding Fails(const std::string& rule,const Json& scene,const std::string& profile="release",const std::string& severity="",std::source_location where=std::source_location::current())
{
    const auto keep=Run(scene,profile);
    const auto* f=Get(keep,rule);
    Check(f!=nullptr,rule+" should fire; got:\n"+Dump(keep,true),where);
    if(!severity.empty())Check(f->severity==severity,rule+" should be "+severity+" in "+profile+", is "+f->severity,where);
    failing.insert(rule);
    return *f;
}
void Passes(const std::string& rule,const Json& scene,const std::string& profile="release",std::source_location where=std::source_location::current())
{
    const auto findings=Run(scene,profile);
    const auto* f=Get(findings,rule);
    Check(f==nullptr,rule+" should not fire: "+(f?f->message:std::string()),where);
    passing.insert(rule);
}

int main()
{
    try
    {
        // ------------------------------------------------------ catalogue
        const auto& pages=Rules::WikiPages();
        const std::set<std::string> known(pages.begin(),pages.end());
        Check(known.size()==25,"25 wiki pages");
        for(const char* page:{"spawn-points","game-modes","alarms","geometric-events","level-ingredients","map-settings","map-packaging","lighting","zones-and-volumes","softbody-actors"})
            Check(known.count(page)==1,std::string("wiki page ")+page);
        std::set<std::string> ids;
        for(const auto& rule:Rules::Catalog())
        {
            Check(ids.insert(rule.id).second,"duplicate rule "+rule.id);
            Check(known.count(rule.wikiPage)==1,rule.id+" cites unknown wiki page "+rule.wikiPage);
            for(const auto& severity:{rule.draft,rule.release})
                Check(severity=="error" || severity=="warning" || severity=="info" || severity=="reported",rule.id+" severity "+severity);
            Check(!rule.description.empty() && rule.description.back()=='.',rule.id+" description");
            Check(Rules::FindRule(rule.id)==&rule,"find "+rule.id);
            const auto group=Rules::Group(rule.id);
            Check(group=="wiki" || group=="design" || group=="security" || group=="mapcheck" || group=="lighting" || group=="release","group of "+rule.id);
        }
        Check(Rules::FindRule("no.such")==nullptr,"unknown rule");
        Check(Rules::Group("security.wiring")=="security" && Rules::Group("layout.unreachable")=="design" && Rules::Group("zones.leak")=="mapcheck" && Rules::Group("lighting.budget")=="lighting" &&
              Rules::Group("lighting.static-effect")=="wiki" && Rules::Group("settings.ranges")=="release" && Rules::Group("spawns.too-few")=="wiki","groups");
        const auto markdown=Rules::CatalogMarkdown();
        for(const auto& id:ids)Check(markdown.find("`"+id+"`")!=std::string::npos,"markdown lists "+id);
        Check(markdown.find("# Validation rules")==0 && markdown.find("208")!=std::string::npos,"markdown header and GE numbers");
        const auto fields=Rules::RuleFields();
        for(const char* field:{"TeamNumber","GameMode","Objectives","Triggers","bChained","NextCam","PrevCam","bFirstCam","Events","MessagePriority","ObjectiveTriggersToLock","LightEffect","bPickable","DropZone","Group","Volumes"})
            Check(std::find(fields.begin(),fields.end(),field)!=fields.end(),std::string("rule field ")+field);
        Reject([]{Rules::Check(Rules::Scene{},"final");});
        Reject([]{Rules::SceneFromJson(Json::array());});

        // ------------------------------------------- a map that follows them
        const auto good=Good();
        for(const char* profile:{"draft","release"})
        {
            const auto findings=Run(good,profile);
            Check(Dump(findings).empty(),std::string("the good map has no errors or warnings in ")+profile+":\n"+Dump(findings));
            for(const auto& f:findings)Check(known.count(f.wikiPage)==1 && ids.count(f.rule)==1,"finding cites a known rule and page");
        }
        {
            const auto findings=Run(good);
            Check(Get(findings,"info.summary")!=nullptr && Get(findings,"info.summary")->message.find("4 spy start(s), 4 merc start(s)")!=std::string::npos,"summary");
            Check(Get(findings,"security.wiring")!=nullptr && Get(findings,"security.wiring")->actors==std::vector<std::string>{"Alarm1"},"wiring summary");
            const auto* travel=Get(findings,"info.travel");
            Check(travel!=nullptr && travel->message.find("spies")!=std::string::npos && travel->message.find(" s)")!=std::string::npos,"travel times");
        }

        // ----------------------------------------------------------- spawns
        {
            auto s=good;Drop(s,[](const Json& a){return a.at("values").value("TeamNumber","")=="1";});
            Fails("spawns.team-missing",s,"draft","error");
            auto t=good;Drop(t,[](const Json& a){return a.at("values").value("TeamNumber","")=="0";});
            Check(Fails("spawns.team-missing",t).message.find("spy")!=std::string::npos,"spy team named");
            // No TeamNumber reads as the engine's default, team 0.
            auto u=good;for(auto& a:u.at("actors"))if(a.at("values").value("TeamNumber","")=="0")a.at("values").erase("TeamNumber");
            Passes("spawns.team-missing",u);
            Passes("spawns.team-missing",good);
        }
        {
            auto s=good;Drop(s,"SpyStart3");Drop(s,"SpyStart4");
            Fails("spawns.too-few",s,"release","error");
            Fails("spawns.too-few",s,"draft","warning");
            auto t=good;Drop(t,"SpyStart4");
            Passes("spawns.too-few",t);
            Passes("spawns.too-few",good);
        }
        {
            auto s=good;Actor(s,"SpyStart1")["position"]={300,520,48}; // in the wall between the spy room and corridor A
            Fails("spawns.in-solid",s);
            auto t=good;Actor(t,"SpyStart1")["values"]["$blocked"]=true;
            Fails("spawns.in-solid",t);
            Passes("spawns.in-solid",good);
        }
        // The Map Design check's distances: spy start WITHIN 512 of an
        // objective, teams WITHIN 768, objective MORE THAN 4096 from every merc.
        {
            Json base{{"actors",Json::array({A("Spy","Engine.PlayerStart",{0,0,48},{{"TeamNumber","0"}}),A("Merc","Engine.PlayerStart",{0,2000,48},{{"TeamNumber","1"}}),
                                             A("Term","SBase.SComputerObjectiveTrigger",{511,0,48})})}};
            Fails("spawns.spy-near-objective",base);
            base["actors"][2]["position"]={512,0,48};
            Passes("spawns.spy-near-objective",base);
            base["actors"][1]["position"]={0,767,48};
            Fails("spawns.teams-close",base);
            base["actors"][1]["position"]={0,768,48};
            Passes("spawns.teams-close",base);
            base["actors"][1]["position"]={512+4097,0,48};
            Fails("spawns.objective-far",base);
            base["actors"][1]["position"]={512+4096,0,48};
            Passes("spawns.objective-far",base);
            Passes("spawns.spy-near-objective",good);Passes("spawns.teams-close",good);Passes("spawns.objective-far",good);
            auto far=good;far["actors"].push_back(A("FarFlag","SBase.SFlag",{0,9000,40},{{"DropZone",{Ref("Drop1")}}}));
            Check(Fails("spawns.objective-far",far).actors==std::vector<std::string>{"FarFlag"},"far objective named");
        }
        {
            auto s=good;
            s["pieces"].push_back(Piece("Room","Vault",512,512,256,{0,6000,0}));
            s["actors"].push_back(A("Bomb1","SBase.SBombTargetObjectiveTrigger",{0,6000,60}));
            Fails("layout.unreachable",s,"release","error");
            Fails("layout.unreachable",s,"draft","warning");
            // A corridor from the merc room (its carve meets both rooms' faces) joins them.
            auto t=s;t["pieces"].push_back(Piece("Corridor","Vault corridor",192,2200,192,{0,4628,0}));
            Passes("layout.unreachable",t);
            Passes("layout.unreachable",good);
        }

        // ------------------------------------------------------------ story
        {
            auto s=good;Drop(s,[](const Json& a){return a.at("class")=="SBase.SMission";});
            Fails("story.no-mission",s,"draft","error");
            Check(Get(Run(s),"modes.missing")==nullptr,"no mission: no-mission says it");
            Passes("story.no-mission",good);
        }
        {
            auto s=good;Actor(s,"Mission")["values"]["GameMode"]="GM_Undefined";
            Fails("story.no-multi",s,"draft","error");
            auto t=s;t["modes"]={"deathmatch"};
            Passes("story.no-multi",t);
            Passes("story.no-multi",good);
        }
        {
            auto s=good;Drop(s,"Obj1");Drop(s,"Obj2");Actor(s,"Mission")["values"]["Objectives"]=Json::array();
            Fails("story.no-objective",s);
            Passes("story.no-objective",good);
        }
        {
            auto s=good;Actor(s,"Mission")["values"]["Objectives"].push_back(Ref("Ghost"));
            Fails("story.unresolved-reference",s,"draft","error");
            auto t=good;Actor(t,"Obj1")["values"]["Triggers"].push_back("SComputerObjectiveTrigger'MyLevel.Missing'");
            Passes("story.unresolved-reference",t); // offline, a typed reference may be an actor already in the map
            t["live"]=true;
            Fails("story.unresolved-reference",t);
            Passes("story.unresolved-reference",good);
        }
        {
            auto s=good;Actor(s,"Obj1")["values"]["Triggers"].push_back(Ref("Light1"));
            Fails("story.trigger-kind",s);
            auto t=good;Actor(t,"Mission")["values"]["Objectives"].push_back(Ref("Term1"));
            Fails("story.trigger-kind",t);
            Passes("story.trigger-kind",good);
        }
        {
            auto s=good;s["actors"].push_back(A("Obj3","SBase.SObjective",{0,1500,100},{{"Triggers",{Ref("Term1")}}}));
            Check(Fails("story.objective-orphan",s,"draft","error").actors==std::vector<std::string>{"Obj3"},"orphan named");
            Passes("story.objective-orphan",good);
        }
        {
            auto s=good;Actor(s,"Obj1")["values"]["Triggers"]=Json::array();
            Fails("story.objective-no-trigger",s);
            Passes("story.objective-no-trigger",good);
        }
        {
            auto s=good;Actor(s,"Mission")["values"]["Objectives"]={"None","None"};
            Fails("story.mission-empty",s,"draft","error");
            Passes("story.mission-empty",good);
        }
        {
            auto s=good;Actor(s,"Mission")["values"]["MinimumObjectives"]="3";
            Fails("story.minimum-objectives",s);
            Passes("story.minimum-objectives",good);
        }
        {
            // Sectors: the GM_Multi mission chained over GM_Undefined sector missions.
            auto s=good;
            Actor(s,"Mission")["values"]["Objectives"]={Ref("Sector1"),Ref("Sector2")};
            s["actors"].push_back(A("Sector1","SBase.SMission",{0,0,0},{{"GameMode","GM_Undefined"},{"bChained","False"},{"Objectives",{Ref("Obj1")}}}));
            s["actors"].push_back(A("Sector2","SBase.SMission",{0,0,0},{{"GameMode","GM_Undefined"},{"bChained","False"},{"Objectives",{Ref("Obj2")}}}));
            Fails("story.sector-chain",s,"draft","error"); // the top is not chained
            Actor(s,"Mission")["values"]["bChained"]="True";
            Passes("story.sector-chain",s);
            Passes("story.objective-orphan",s);
            auto t=s;Actor(t,"Sector2")["values"]["GameMode"]="GM_Multi";
            Fails("story.sector-chain",t);
            auto u=s;Actor(u,"Sector1")["values"]["bChained"]="True";
            Fails("story.sector-chain",u);
            auto v=s;Actor(v,"Mission")["values"]["MinimumObjectives"]="3";
            Fails("story.minimum-objectives",v); // two sectors of one objective each give 2
            Passes("story.sector-chain",good);
        }
        {
            auto s=good;Actor(s,"Obj1")["values"]["TriggersERVisibilityZone"]=Ref("Light1");
            Fails("story.visibility-zone",s);
            auto t=good;Actor(t,"Obj1")["values"]["TriggersERVisibilityZone"]=Ref("NoZone");
            Fails("story.visibility-zone",t);
            Passes("story.visibility-zone",good);
        }
        {
            auto s=good;Drop(s,"DMMission");
            Check(Fails("modes.missing",s).message.find("Deathmatch")!=std::string::npos,"deathmatch missing");
            Passes("modes.missing",good);
        }

        // ------------------------------------------------------- objectives
        {
            auto s=good;Actor(s,"Flag1")["values"]["DropZone"]=Json::array();
            Fails("objectives.flag-dropzones",s,"draft","error");
            auto t=good;for(int i=0;i<6;++i){t["actors"].push_back(A("DZ"+std::to_string(i),"SBase.SFlagDropZone",{0,0,0}));Actor(t,"Flag1")["values"]["DropZone"].push_back(Ref("DZ"+std::to_string(i)));}
            Fails("objectives.flag-dropzones",t);
            auto u=good;Actor(u,"Flag1")["values"]["DropZone"]={Ref("Light1")};
            Fails("objectives.flag-dropzones",u);
            Passes("objectives.flag-dropzones",good);
        }
        {
            auto s=good;s["actors"].push_back(A("Drop2","SBase.SFlagDropZone",{0,1600,0}));
            Fails("objectives.dropzone-unused",s);
            Passes("objectives.dropzone-unused",good);
        }
        {
            auto s=good;Actor(s,"Term1")["position"]={0,1500,150};
            Check(Fails("objectives.terminal-placement",s).message.find("150 units above the floor")!=std::string::npos,"terminal height measured");
            auto t=good;Actor(t,"Term1")["position"]={450,1500,78}; // in the server room's wall
            Check(Fails("objectives.terminal-placement",t).message.find("intersects")!=std::string::npos,"terminal in a wall");
            auto u=good;Actor(u,"Term1")["values"]["$floor"]=-40.0; // traced live: the floor is lower than the blockout says
            Fails("objectives.terminal-placement",u);
            Actor(u,"Term1")["values"]["$floor"]=0.0;
            Actor(u,"Term1")["values"]["$blocked"]=false;
            Passes("objectives.terminal-placement",u);
            Passes("objectives.terminal-placement",good);
        }
        {
            auto s=good;Actor(s,"Term1")["values"]["CaptureTime"]="10.000000";
            Fails("objectives.terminal-capture-time",s);
            // Live, CaptureTime is always read; it matters when DominationCaptureTime is not set.
            auto t=s;t["live"]=true;
            Passes("objectives.terminal-capture-time",t);
            Actor(t,"Term1")["values"]["DominationCaptureTime"]="0.000000";
            Fails("objectives.terminal-capture-time",t);
            Passes("objectives.terminal-capture-time",good);
        }
        {
            auto s=good;s["mapSettings"]["values"]["Modes"][1]["ScoreLimit"]="10";
            Fails("diskhunt.disk-count",s,"draft","error");
            auto t=good;Actor(t,"DiskMission")["values"]["Triggers"]=Json::array();
            Fails("diskhunt.disk-count",t);
            auto u=good;u["mapSettings"]["values"]["Modes"][1]["Handicap"]="2";
            Fails("diskhunt.disk-count",u); // fewer active disks than ScoreLimit
            auto v=good;Drop(v,[](const Json& a){return a.at("class")=="SBase.SDisk";});Actor(v,"DiskMission")["values"]["Triggers"]=Json::array();
            Fails("diskhunt.disk-count",v);
            Passes("diskhunt.disk-count",good);
        }
        {
            auto s=good;Actor(s,"Disk1")["values"]["Group"]="4";
            Fails("diskhunt.disk-group",s);
            auto t=good;Actor(t,"Disk1")["values"]["Group"]="0";
            Fails("diskhunt.disk-group",t);
            Passes("diskhunt.disk-group",good);
        }
        {
            auto s=good;Actor(s,"Disk1")["position"]={-250,1300,4};
            Fails("diskhunt.disk-clearance",s);
            auto t=good;Actor(t,"Disk1")["position"]={-390,1300,40}; // 10 units from the wall
            Fails("diskhunt.disk-clearance",t);
            Passes("diskhunt.disk-clearance",good);
        }
        {
            auto s=good;s["actors"].push_back(A("Disk5","SBase.SDisk",{0,1350,40},{{"Group","1"}}));
            Fails("diskhunt.disk-unlisted",s);
            Passes("diskhunt.disk-unlisted",good);
        }
        {
            auto s=good;Actor(s,"DMMission")["values"]["AtResetEvent"]="None";
            Fails("deathmatch.security",s);
            auto t=s;Drop(t,"Alarm1");Drop(t,"Laser1");
            Passes("deathmatch.security",t);
            Passes("deathmatch.security",good);
        }
        {
            auto s=good;s["playerCounts"]=Json::array({{{"objective","Obj1"},{"min",8},{"max",0}},{{"objective","Obj2"},{"min",8},{"max",0}}});
            Check(Fails("objectiveplayers.problems",s,"draft","error").message.find("no objectives left")!=std::string::npos,"player count problem text");
            auto t=good;t["playerCounts"]=Json::array({{{"objective","Obj2"},{"min",4},{"max",0}}});
            Passes("objectiveplayers.problems",t);
            Passes("objectiveplayers.problems",good);
            auto u=good;u["targets"]=Json::array({{{"mission","Mission"},{"sizes",{5,0,0,0,0,0,0,0}}}});
            Check(Fails("objectiveplayers.targets",u).message.find("1v1")!=std::string::npos,"target warning text");
            auto v=good;v["targets"]=Json::array({{{"mission","Mission"},{"sizes",{2,0,0,0,0,0,0,0}}}});
            Passes("objectiveplayers.targets",v);
            Passes("objectiveplayers.targets",good);
        }

        // ------------------------------------------------------------ CamNet
        {
            auto s=good;Drop(s,[](const Json& a){return a.at("class")=="SBase.SCamNetwork";});
            Fails("camnet.missing",s,"draft","error");
            Passes("camnet.missing",good);
        }
        {
            auto s=good;Actor(s,"Cam2")["values"]["bFirstCam"]="True";
            Fails("camnet.first-cam",s);
            auto t=good;Actor(t,"Cam1")["values"]["bFirstCam"]="False";
            Check(Fails("camnet.first-cam",t).message.find("0 SCamNetwork")!=std::string::npos,"no first camera");
            Passes("camnet.first-cam",good);
        }
        {
            auto s=good;Actor(s,"Cam3")["values"]["NextCam"]="None";
            Fails("camnet.loop",s,"draft","error");
            auto t=good;t["live"]=true;
            for(auto& a:t["actors"])if(a.at("class")=="SBase.SCamNetwork")
                for(const char* field:{"NextCam","PrevCam"})a["values"][field]="SCamNetwork'MyLevel."+a["values"][field].at("$ref").get<std::string>()+"'";
            Passes("camnet.loop",t);
            // Two separate pairs: mercs never cycle from one to the other.
            auto u=t;
            u["actors"].push_back(A("Cam4","SBase.SCamNetwork",{0,0,0},{{"NextCam","SCamNetwork'MyLevel.Cam5'"},{"PrevCam","SCamNetwork'MyLevel.Cam5'"},{"bFirstCam","False"},{"bBlockActors","True"},{"bBlockPlayers","True"}}));
            u["actors"].push_back(A("Cam5","SBase.SCamNetwork",{0,0,0},{{"NextCam","SCamNetwork'MyLevel.Cam4'"},{"PrevCam","SCamNetwork'MyLevel.Cam4'"},{"bFirstCam","False"},{"bBlockActors","True"},{"bBlockPlayers","True"}}));
            const auto loop=Fails("camnet.loop",u);
            Check(loop.message.find("2 separate networks")!=std::string::npos,"separate networks");
            Check(loop.fix.value("tool","")=="actors_edit" && loop.fix.at("args").at("edits").size()==5,"loop fix relinks every camera");
            Passes("camnet.loop",good);
        }
        {
            auto s=good;Actor(s,"Cam1")["values"]["bBlockActors"]="False";
            Fails("ingredients.collision",s);
            auto t=good;Actor(t,"Med1")["values"].erase("bBlockPlayers"); // offline, the default: no collision
            Fails("ingredients.collision",t);
            Passes("ingredients.collision",good);
        }
        {
            auto s=good;Actor(s,"Ammo1")["values"]["fInteractionHeight"]="80.000000";
            Fails("ingredients.interaction",s,"release","info");
            Passes("ingredients.interaction",good);
        }
        {
            auto s=good;Actor(s,"Sprinkler1")["values"]["bHidden"]="False";
            Fails("ingredients.sprinkler-hidden",s);
            Passes("ingredients.sprinkler-hidden",good);
        }

        // ---------------------------------------------------------- security
        {
            auto s=good;Actor(s,"Laser1")["event"]="Nowhere";
            Check(Fails("security.dangling-event",s,"draft","error").actors==std::vector<std::string>{"Laser1"},"dangling detector named");
            Passes("security.dangling-event",good);
        }
        {
            auto s=good;s["actors"].push_back(A("Sensor1","SBase.SVolumetricSensor",{0,1500,100},{{"Volumes",Json::array()}},"","Alarm1"));
            Fails("security.sensor-no-volume",s);
            auto t=good;t["actors"].push_back(A("Sensor1","SBase.SVolumetricSensor",{0,1500,100},{{"Volumes",{"Volume'MyLevel.Volume3'"}}},"","Alarm1"));
            Passes("security.sensor-no-volume",t);
            Passes("security.sensor-no-volume",good);
        }
        {
            auto s=good;s["actors"].push_back(A("Camera1","SBase.SCamera",{0,1500,200}));
            Fails("security.detector-unwired",s);
            Passes("security.detector-unwired",good);
        }
        {
            auto s=good;s["actors"].push_back(A("Alarm2","SBase.SAlarm",{0,0,0},{{"Events",{"AlarmDoor"}}},"Alarm2"));
            Check(Fails("security.alarm-unfed",s).actors==std::vector<std::string>{"Alarm2"},"unfed alarm named");
            Passes("security.alarm-unfed",good);
        }
        {
            auto s=good;auto& v=Actor(s,"Alarm1")["values"];v["Events"]=Json::array();v["ObjectiveTriggersToLock"]=Json::array();v["MoversToLock"]=Json::array();
            Fails("security.alarm-no-output",s);
            // Locks are what an alarm does too.
            auto t=good;Actor(t,"Alarm1")["values"]["Events"]=Json::array();
            Passes("security.alarm-no-output",t);
            Passes("security.alarm-no-output",good);
        }
        {
            auto s=good;s["actors"].push_back(A("Bomb1","SBase.SBombTargetObjectiveTrigger",{0,1600,60}));Actor(s,"Alarm1")["values"]["ObjectiveTriggersToLock"].push_back(Ref("Bomb1"));
            Fails("security.bomb-locked",s,"draft","error");
            Passes("security.bomb-locked",good);
        }
        {
            auto s=good;Actor(s,"Alarm1")["values"]["MoversToLock"].push_back({{"Mover",Ref("GhostDoor")},{"Action","ML_Lock"}});
            Fails("security.lock-unresolved",s);
            auto t=good;Actor(t,"Alarm1")["values"]["ObjectiveTriggersToLock"].push_back(Ref("GhostTerminal"));
            Fails("security.lock-unresolved",t);
            Passes("security.lock-unresolved",good);
        }
        {
            auto s=good;Actor(s,"Alarm1")["values"]["MessagePriority"]="7";
            Fails("security.message-priority",s);
            Passes("security.message-priority",good);
        }
        {
            Fails("security.wiring",good,"release","info");
            auto s=good;Drop(s,"Alarm1");
            Passes("security.wiring",s);
        }

        // ------------------------------------------------------------- zones
        {
            auto s=good;s["actors"].push_back(A("AlarmZone","SBase.SAlarmZoneInfo",{0,0,0},{{"LocationName","\"Vault\""}}));
            Fails("zones.alarm-zone",s);
            Passes("zones.alarm-zone",good);
        }
        {
            auto s=good;Actor(s,"ZoneSpy")["values"].erase("LocationName");
            Fails("zones.location-name",s,"release","info");
            Passes("zones.location-name",good);
        }
        {
            auto s=good;s["mapCheck"]={{"rows",Json::array({{{"severity","error"},{"group","Zones"},{"check","Zone leak"},{"message","Zone 2 leaks into zone 3."},{"actors",{"MyLevel.ZoneInfo2"}}}})}};
            Check(Fails("zones.leak",s).actors==std::vector<std::string>{"MyLevel.ZoneInfo2"},"leak actors");
            Check(Get(Run(s),"mapcheck.row")==nullptr,"a leak is not also a plain row");
            Passes("zones.leak",good);
        }

        // ------------------------------------------------------------ actors
        {
            auto s=good;Actor(s,"Light1")["values"]["LightEffect"]="LE_Disco";
            const auto f=Fails("lighting.static-effect",s,"draft","error");
            Check(f.fix.value("tool","")=="level_build","offline fixes go to the spec");
            auto t=s;t["live"]=true;
            Check(Fails("lighting.static-effect",t).fix.at("args").at("edits")[0].at("properties").at("LightEffect")=="LE_None","live fix sets LE_None");
            Passes("lighting.static-effect",good);
        }
        {
            auto s=good;Actor(s,"Light2")["values"]["EchelonNormalInfluence"]="0.500000";
            Fails("lighting.echelon-normal",s,"release","info");
            Passes("lighting.echelon-normal",good);
        }
        {
            auto s=good;s["actors"].push_back(A("Crate","SBase.SGamePlayObject",{0,0,0},{{"bPickable","True"}}));
            Fails("objects.pickable",s,"draft","error");
            Passes("objects.pickable",good);
        }
        {
            auto s=good;s["actors"].push_back(A("Crate","SBase.SGamePlayObject",{0,0,0},{{"bCanBeChaffed","True"}}));
            Fails("objects.chaff",s,"release","info");
            Passes("objects.chaff",good);
        }
        {
            auto s=good;
            s["actors"].push_back(A("Curtain1","SBase.ESBPatchActor",{0,0,0},{{"SoftBody","ESoftBody'MyLevel.ESoftBody0'"}}));
            s["actors"].push_back(A("Curtain2","SBase.ESBPatchActor",{0,0,0},{{"SoftBody","ESoftBody'MyLevel.ESoftBody0'"}}));
            Check(Fails("softbody.duplicated",s,"draft","error").actors.size()==2,"both copies named");
            auto t=good;
            t["actors"].push_back(A("Curtain1","SBase.ESBPatchActor",{0,0,0},{{"SoftBody","ESoftBody'MyLevel.ESoftBody0'"}}));
            t["actors"].push_back(A("Curtain2","SBase.ESBPatchActor",{0,0,0},{{"SoftBody","ESoftBody'MyLevel.ESoftBody1'"}}));
            Passes("softbody.duplicated",t);
            Passes("softbody.duplicated",good);
        }
        {
            auto s=good;Actor(s,"Door1")["values"]["StaticMesh"]="None";
            Fails("movers.static-mesh",s);
            Passes("movers.static-mesh",good);
        }
        {
            auto s=good;Actor(s,"Fish1")["values"]["Pilot"]=Ref("Light1");
            Fails("fish.pilot",s);
            auto t=good;Actor(t,"FishLead")["values"]["bPilote"]="False";
            Fails("fish.pilot",t);
            Passes("fish.pilot",good);
        }
        {
            auto s=good;Actor(s,"Tips1")["values"]["bHidden"]="False";
            Fails("tutorials.hidden",s);
            Passes("tutorials.hidden",good);
        }
        {
            auto s=good;Actor(s,"Light1")["event"]="Nobody";
            Fails("events.dangling",s);
            auto t=good;Actor(t,"DMMission")["values"]["AtResetEvent"]="NoSuchTag";
            Fails("events.dangling",t);
            auto u=good;Actor(u,"Alarm1")["values"]["Events"]={"AlarmDoor","NoSuchTag"};
            Fails("events.dangling",u);
            Passes("events.dangling",good);
        }

        // ------------------------------------------------------------- level
        {
            auto s=good;s["level"]["fDeathPawnHeight"]="100.000000";
            Fails("level.death-height",s,"draft","error");
            Passes("level.death-height",good);
        }
        {
            // The LevelInfo's one Event and its SendAtTime, an IntProperty
            // (tests/maps/OffsE's LevelInfo has Event=storm, SendAtTime=1), as
            // settings.inspect and the compiler give them.
            auto s=good;s["level"]["SendAtTime"]="30";
            Fails("level.send-at-time",s);
            auto none=s;none["level"]["Event"]="None";
            Passes("level.send-at-time",none);
            // The spec's form reads the same.
            Json t=good;t["level"]={{"events",Json::array({{{"event","Warning3min"},{"sendAtTime",0}}})}};
            Fails("level.send-at-time",t);
            Json u=good;u["level"]={{"events",Json::array({{{"event","Warning3min"},{"sendAtTime",-60}}})}};
            Passes("level.send-at-time",u);
            Passes("level.send-at-time",good);
        }
        {
            // MTVOffset and MTVSlope are LevelInfo properties; MotionTrackingTweak
            // is only the property sheet's category.
            auto s=good;s["level"]["MTVOffset"]="30";
            Fails("level.motion-tracking",s,"release","info");
            auto t=good;t["level"]["MTVOffset"]="8";t["level"]["MTVSlope"]="8";
            Passes("level.motion-tracking",t);
            auto category=good;category["level"].erase("MTVOffset");category["level"].erase("MTVSlope");category["level"]["MotionTrackingTweak"]={{"MTVOffset","30"},{"MTVSlope","85"}};
            Passes("level.motion-tracking",category);
            Passes("level.motion-tracking",good);
        }

        // ------------------------------------------------ packages, documents
        {
            auto s=good;s["release"]["packages"]={"TestD_TXT.utx","testd_txt.usx"};
            Fails("release.package-names",s,"draft","error");
            auto t=good;t["release"]["packages"].push_back({{"name","Bunker.utx"},{"stock",true}});
            Fails("release.package-names",t);
            auto u=good;u["release"]["packages"].push_back("TestD.utx");
            Fails("release.package-names",u);
            Passes("release.package-names",good);
        }
        {
            auto s=good;s["documents"].push_back({{"format","scct.map-changes"},{"version",9},{"map","*"},{"operations",Json::array()}});
            Check(Fails("authoring.validate",s,"draft","error").message.find("document 2")!=std::string::npos,"document numbered");
            Passes("authoring.validate",good);
        }
        {
            auto s=good;s["schema"]["classes"]["SBase.SMission"]["schema"]["GameMode"]["choices"]={"GM_Undefined","GM_Multi"};
            Fails("authoring.unwritable",s);
            auto t=good;t["schema"]["classes"]["SBase.SMission"]["schema"]["bChained"]={{"writable",false},{"reason","Map JSON does not write this property."}};
            Fails("authoring.unwritable",t);
            auto u=t;u["live"]=true; // the live map's values are read, not written
            Passes("authoring.unwritable",u);
            Passes("authoring.unwritable",good);
        }

        // ----------------------------------------- Map Settings and interface
        {
            auto s=good;s["mapSettings"]["settings"]=false;
            Fails("settings.missing",s,"release","warning");
            Fails("settings.missing",s,"draft","info");
            Passes("settings.missing",good);
        }
        {
            auto s=good;s["mapSettings"]["values"]["Modes"][0]["RespawnTime"]="30";
            Check(Fails("settings.ranges",s).message.find("RespawnTime 30")!=std::string::npos,"respawn named");
            auto t=good;t["mapSettings"]["values"]["Modes"][2]["SpiesLives"]="11";t["mapSettings"]["values"]["Modes"][2]["TimeLimit"]="25";
            Fails("settings.ranges",t);
            Passes("settings.ranges",good);
        }
        {
            auto s=good;s["mapSettings"]["values"]["MapName"]="\"TestMap\"";
            Check(Fails("settings.map-name-caps",s).fix.at("args").at("mapName")=="TESTMAP","caps fix");
            Passes("settings.map-name-caps",good);
        }
        {
            auto s=good;s["mapSettings"]["values"]["ModesStrings"].erase(2);
            Fails("settings.mode-parity",s);
            auto t=good;t["mapSettings"]["values"]["Modes"][2]["GameModeId"]="GM_Multi";
            Fails("settings.mode-parity",t);
            Passes("settings.mode-parity",good);
        }
        {
            auto s=good;s["mapSettings"]["values"]["Modes"].erase(2);s["mapSettings"]["values"]["ModesStrings"].erase(2);
            Check(Fails("settings.modes-missing",s).message.find("Deathmatch")!=std::string::npos,"mode named");
            Passes("settings.modes-missing",good);
        }
        {
            // The spec's own settings section reads the same.
            auto s=good;s.erase("mapSettings");
            s["settings"]={{"mapName","Test map"},{"modes",Json::array({{{"mode","story"},{"respawn",20}},{{"mode","diskhunt"},{"scoreLimit",3}},{{"mode","deathmatch"}}})}};
            const auto findings=Run(s);
            Check(Get(findings,"settings.map-name-caps")!=nullptr && Get(findings,"settings.ranges")!=nullptr && Get(findings,"settings.modes-missing")==nullptr,"spec settings");
        }
        {
            auto s=good;s["mapSettings"]["textures"]["Menu"]["format"]="DXT1";
            Fails("interface.textures",s,"release","warning");
            Fails("interface.textures",s,"draft","info");
            auto t=good;t["mapSettings"]["textures"].erase("LoadingDEF0");
            Fails("interface.textures",t);
            auto u=good;u["mapSettings"]["textures"]["Briefing"]["width"]=128;
            Fails("interface.textures",u);
            // A spec names its interface images instead.
            auto v=good;v["mapSettings"].erase("textures");v["release"]["interface"]={{"menu","C:\\art\\menu.png"},{"briefing","b.png"},{"loadingAtt","a.png"}};
            Check(Fails("interface.textures",v).message.find("LoadingDEF0")!=std::string::npos,"missing spec image");
            v["release"]["interface"]["loadingDef"]="d.png";
            Passes("interface.textures",v);
            Passes("interface.textures",good);
        }

        // ----------------------------------------------------------- release
        {
            auto s=good;s["release"]["uas"]={{"packages",{"Amb_TestD"}}};
            Check(Fails("release.uas",s).message.find("Interface")!=std::string::npos,"interface missing from uas");
            auto t=good;t["release"]["uas"]={{"packages",{"Interface"}}};
            Check(Fails("release.uas",t).message.find("Amb_TestD")!=std::string::npos,"ambient package missing from uas");
            Passes("release.uas",good);
        }
        {
            auto s=good;s["mapFile"]="Test Map.sdc";
            Fails("release.file-name",s);
            auto t=good;t["mapFile"]="C:\\Game\\Packages\\MapsEd\\TestD.unr";
            Fails("release.file-name",t);
            auto u=good;u["mapFile"]="C:\\Game\\Packages\\MapsEd\\TestD.sdc";
            Passes("release.file-name",u);
            Passes("release.file-name",good);
        }
        {
            auto s=good;s["mapFile"]="Testing.sdc";
            Fails("release.naming-convention",s,"release","info");
            auto t=good;t["mapFile"]="AquaD.sdc";
            Fails("release.naming-convention",t);
            auto u=good;u["release"]["packages"].push_back("SharedProps.usx");
            Fails("release.naming-convention",u);
            Passes("release.naming-convention",good);
        }
        {
            // The release tool's file check: its rows for what only it can see
            // are folded in at their own severity, the rest are left to the rules.
            auto row=[](const char* severity,const char* rule,const std::string& message){return Json{{"severity",severity},{"rule",rule},{"wikiPage","map-packaging"},{"message",message}};};
            auto s=good;s["release"]["findings"]=Json::array({row("error","release.files","There is no playable Packages\\Maps\\TestD.sdc."),
                                                               row("warning","release.files","Packages\\Maps\\TestD.sdc is older than the MapsEd file.")});
            s["release"]["findings"][0]["fix"]={{"tool","map_file"},{"args",{{"action","save"}}}};
            const auto files=Fails("release.files",s,"draft","error");
            Check(files.message.find("Maps\\TestD.sdc")!=std::string::npos && files.fix.at("tool")=="map_file" && files.wikiPage=="map-packaging","the row's message and fix");
            const auto all=Run(s);
            Check(std::count_if(all.begin(),all.end(),[](const Rules::Finding& f){return f.rule=="release.files";})==2 &&
                  std::any_of(all.begin(),all.end(),[](const Rules::Finding& f){return f.rule=="release.files" && f.severity=="warning";}),"every row, at its severity");
            Passes("release.files",good);
            auto t=good;t["release"]["findings"]=Json::array({row("error","release.dependencies","The map uses Bunker_TXT, but no package of that name is in Packages.")});
            Fails("release.dependencies",t,"release","error");
            t["release"]["findings"][0]["severity"]="warning";
            Fails("release.dependencies",t,"release","warning");
            Passes("release.dependencies",good);
            auto u=good;u["release"]["findings"]=Json::array({row("info","release.optimise","The map uses 2.0 MB of the 40.0 MB in Lab_TXT.")});
            Fails("release.optimise",u,"release","info");
            Passes("release.optimise",good);
            // Rows the rules check for themselves are not repeated.
            auto v=good;v["release"]["findings"]=Json::array({row("error","release.package-names","TestD_TXT.utx and TestD_TXT.usx share a base name."),row("error","release.files","")});
            Passes("release.package-names",v);
            Passes("release.files",v);
        }

        // -------------------------------------------------- geometric events
        {
            auto ge=[&](Json element){auto s=good;s["ges"]=Json::array({element});return s;};
            auto ledge=[](double z,double length=128){return Json{{"kind","ledge"},{"points",{{-300,-400,z},{-300+length,-400,z}}}};};
            Fails("ge.points",ge({{"kind","ladder"},{"points",{{0,0,0},{0,0,256}}}}),"draft","error");
            Fails("ge.points",ge({{"kind","rope"},{"points",{{0,0,0},{0,0,256}}}}));
            Passes("ge.points",good);
            // Heights are measured from the blockout's floor (the spy room, z 0).
            Check(Fails("ge.ledge-height",ge(ledge(470))).message.find("470 units")!=std::string::npos,"ledge measured from the room floor");
            Passes("ge.ledge-height",ge(ledge(300)));
            Fails("ge.ledge-reach",ge(ledge(400)),"release","info");
            Passes("ge.ledge-reach",good);
            Fails("ge.obstacle",ge(ledge(100)));
            Passes("ge.obstacle",good);
            Fails("ge.ledge-length",ge(ledge(250,20)));
            Passes("ge.ledge-length",good);
            auto shallow=ledge(250);shallow["surface"]=40;
            Fails("ge.pull-up",ge(shallow));
            // The platform behind a ledge gives its depth: a 48-unit lip is too shallow.
            auto lip=ge({{"kind","ledge"},{"points",{{-100,-24,256},{100,-24,256}}}});
            lip["pieces"].push_back({{"spec",{{"kind","Platform"},{"width",256},{"length",48},{"height",256},{"construction","Carve"}}},{"position",{0,0,0}},{"rotation",{0,0,0}}});
            Fails("ge.pull-up",lip);
            Passes("ge.pull-up",good);
            auto fence=[](double z){return Json{{"kind","fence"},{"points",{{-300,0,z},{-44,0,z}}}};};
            Fails("ge.fence-height",ge(fence(60)));
            Passes("ge.fence-height",ge(fence(96)));
            Fails("ge.fence-recommended",ge(fence(120)),"release","info");
            Passes("ge.fence-recommended",ge(fence(96)));
            Fails("ge.fence-climbable",ge(fence(96)),"release","info");
            Passes("ge.fence-climbable",good);
            Fails("ge.hoh-clearance",ge({{"kind","hoh"},{"points",{{-300,300,200},{-44,300,200}}}}));
            Passes("ge.hoh-clearance",good);
            Check(Fails("ge.ladder-bars",ge({{"kind","ladder"},{"points",{{400,400,0},{400,400,192},{370,400,0}}}})).fix.at("args").at("ges")[0].at("bars")==8,"ladder fix gives bars");
            Passes("ge.ladder-bars",good);
            Fails("ge.zipline-hang",ge({{"kind","zipline"},{"points",{{300,-300,450},{-300,-300,250}}}}));
            Passes("ge.zipline-hang",good);
            Fails("ge.zipline-clearance",ge({{"kind","zipline"},{"points",{{300,-300,300},{-300,-300,180}}}}));
            Passes("ge.zipline-clearance",good);
            Fails("ge.zipline-slope",ge({{"kind","zipline"},{"points",{{300,-300,300},{-300,-300,300}}}}));
            Passes("ge.zipline-slope",good);
            Check(Fails("ge.point-order",ge({{"kind","ladder"},{"points",{{400,400,256},{400,400,0},{370,400,0}}}})).fix.at("args").at("ges")[0].at("points")[0][2]==0,"order fix");
            Passes("ge.point-order",good);
            // Traced floors (live) win over the blockout.
            auto traced=ge({{"kind","hoh"},{"points",{{-300,300,240},{-44,300,240}}},{"$floors",Json::array({{{"at",{-300,300,238}},{"floor",100}},{{"at",{-172,300,238}},{"floor",100}},{{"at",{-44,300,238}},{"floor",100}}})}});
            Fails("ge.hoh-clearance",traced);
            // A ge.list reply reads the same as the spec's list.
            auto listed=good;listed["ges"]={{"elements",Json::array({{{"kind","Ladder"},{"points",{{400,400,0},{400,400,192},{370,400,0}}},{"command","GE ADD LADDER ..."}}})}};
            Fails("ge.ladder-bars",listed);
        }

        // ----------------------------------------------------- folded checks
        {
            auto s=good;s["pieces"].push_back(Piece("Room","Island",512,512,256,{3000,0,0}));
            Check(Fails("design.layout",s,"release","error").message.find("Island")!=std::string::npos,"isolated room named");
            Fails("design.layout",s,"draft","warning");
            Passes("design.layout",good);
            // A room above the spy room that only a flight of stairs reaches is joined.
            auto t=good;
            t["pieces"].push_back(Piece("Room","Loft",512,512,256,{0,0,528}));
            auto stairs=Piece("Stairs","Loft stairs",128,512,528,{300,0,0});stairs["spec"]["steps"]=16;
            t["pieces"].push_back(stairs);
            Passes("design.layout",t);
            t["pieces"].erase(t["pieces"].size()-1);
            Fails("design.layout",t);
        }
        {
            auto s=good;s["pieces"][1]["spec"]["width"]=64;
            Fails("design.traversal",s);
            Passes("design.traversal",good);
        }
        {
            auto s=good;s["live"]=true;
            Fails("design.stale-geometry",s);
            s["geometryBuilt"]=true;
            Passes("design.stale-geometry",s);
            Passes("design.stale-geometry",good); // offline nothing is built yet
        }
        {
            auto s=good;s["mapCheck"]={{"rows",Json::array({{{"severity","warning"},{"group","Actors"},{"check","Floating"},{"message","Light3 is outside the world."},{"actors",{"MyLevel.Light3"}}},
                                                             {{"severity","error"},{"group","BSP"},{"check","Invalid"},{"message","Brush 4 is not closed."}}})}};
            const auto findings=Run(s);
            std::vector<std::string> severities;
            for(const auto& f:findings)if(f.rule=="mapcheck.row")severities.push_back(f.severity);
            Check(severities==std::vector<std::string>{"error","warning"},"map check rows keep their severity, errors first");
            Fails("mapcheck.row",s);
            Passes("mapcheck.row",good);
        }
        {
            auto s=good;s["lighting"]={{"over",true},{"buildWarning",{{"over",true}}},{"summary","Zone 2: 9 lights reach one leaf."},{"hotspots",Json::array({{{"lights",{"MyLevel.Light5"}}}})}};
            Check(Fails("lighting.budget",s).actors==std::vector<std::string>{"MyLevel.Light5"},"budget hotspot lights");
            auto t=good;t["lighting"]={{"over",true},{"buildWarning",{{"over",false}}}};
            Passes("lighting.budget",t);
            Fails("lighting.guideline",t,"release","info");
            Passes("lighting.guideline",good);
            Passes("lighting.budget",good);
        }
        {
            Fails("info.summary",good,"release","info");
            Fails("info.travel",good,"release","info");
            Json bare{{"actors",Json::array({A("Spy","Engine.PlayerStart",{0,0,48})})}};
            Passes("info.travel",bare);
        }

        // ------------------------------------------- the live map's encoding
        {
            Json query{{"actors",Json::array({
                {{"path","MyLevel.PlayerStart0"},{"class","Engine.PlayerStart"},{"name","PlayerStart0"},{"tag","PlayerStart"},{"event","None"},{"position",{0,0,48}},{"values",Json::object()}},
                {{"path","MyLevel.PlayerStart1"},{"class","Engine.PlayerStart"},{"name","PlayerStart1"},{"tag","PlayerStart"},{"event","None"},{"position",{0,2000,48}},{"values",Json::object()}},
                {{"path","MyLevel.SMission0"},{"class","SBase.SMission"},{"name","SMission0"},{"tag","SMission"},{"event","None"},{"position",{0,0,0}},
                 {"values",{{"GameMode","GM_Multi"},{"Objectives",{"SObjective'MyLevel.SObjective0'","None"}},{"MinimumObjectives","1"},{"bChained","False"}}}},
                {{"path","MyLevel.SObjective0"},{"class","SBase.SObjective"},{"name","SObjective0"},{"tag","SObjective"},{"event","None"},{"position",{0,1000,0}},
                 {"values",{{"Triggers",{"SComputerObjectiveTrigger'SComputerObjectiveTrigger0'","SFlag'MyLevel.SFlag9'"}},{"TriggersERVisibilityZone","None"}}}},
                {{"path","MyLevel.SComputerObjectiveTrigger0"},{"class","SBase.SComputerObjectiveTrigger"},{"name","SComputerObjectiveTrigger0"},{"tag","SComputerObjectiveTrigger"},{"event","None"},
                 {"position",{0,1000,78}},{"values",{{"DominationCaptureTime","10.000000"},{"CaptureTime","5.000000"},{"$floor",0.0},{"$blocked",false}}}}})},{"total",5}};
            Json extra{{"spawns",Json::array({{{"path","MyLevel.PlayerStart0"},{"team","0"}},{{"path","MyLevel.PlayerStart1"},{"team","1"}}})},
                       {"level",{{"actor",{{"path","MyLevel.LevelInfo0"}}},{"values",{{"fDeathPawnHeight","0.000000"}}}}},
                       {"mapSettings",{{"package","TestD-i"},{"saved",false},{"settings",false},{"textures",Json::object()},{"values",nullptr}}},
                       {"ges",{{"elements",Json::array()}}},{"mapFile","C:\\Game\\Packages\\MapsEd\\TestD.sdc"},{"geometryBuilt",true}};
            const auto scene=Rules::SceneFromQuery(query,extra);
            Check(scene.live && scene.actors.size()==5 && scene.mapFile.find("TestD.sdc")!=std::string::npos,"live scene");
            Check(scene.actors[0].values.at("TeamNumber")=="0" && scene.actors[1].values.at("TeamNumber")=="1","teams from design.spawns");
            Check(scene.level.contains("fDeathPawnHeight") && scene.mapSettings.at("settings")==false,"level values and map settings");
            const auto findings=Rules::Check(scene,"release");
            Check(Get(findings,"spawns.team-missing")==nullptr,"teams filled");
            // MyLevel.SObjective0 by its full path, SComputerObjectiveTrigger0 by its last segment, SFlag9 missing.
            const auto* unresolved=Get(findings,"story.unresolved-reference");
            Check(unresolved!=nullptr && unresolved->message.find("SFlag9")!=std::string::npos,"live unresolved trigger");
            size_t count=0;for(const auto& f:findings)count+=f.rule=="story.unresolved-reference";
            Check(count==1,"only the missing reference is unresolved");
            Check(Get(findings,"story.objective-orphan")==nullptr,"objective found by path");
            Check(Get(findings,"objectives.terminal-placement")==nullptr && Get(findings,"objectives.terminal-capture-time")==nullptr,"live terminal at 78 with DominationCaptureTime");
            Check(Get(findings,"settings.missing")!=nullptr && Get(findings,"settings.missing")->severity=="warning","no SMapSettings yet");
            Check(Get(findings,"interface.textures")==nullptr,"textures not reported before Map Settings exist");
            Check(Get(findings,"design.stale-geometry")==nullptr,"built");
            Check(Get(findings,"camnet.missing")!=nullptr,"no cameras");
            for(const auto& f:findings)Check(known.count(f.wikiPage)==1 && ids.count(f.rule)==1,"live finding cites a known rule and page");
            // Live fixes edit the map.
            Check(Get(findings,"camnet.missing")->fix.value("tool","")=="security","camnet fix tool");
            // The page itself, or a bare list of actors.
            Check(Rules::SceneFromQuery(query.at("actors"),Json()).actors.size()==5,"bare list");
        }
        // Spec-shaped pieces ({id, kind, size, at, yaw}) read as Map Design pieces.
        {
            Json scene{{"pieces",Json::array({{{"id","Hall"},{"kind","Room"},{"size",{512,512,256}},{"at",{0,0}},{"yaw",90}},{{"id","Bad"},{"kind","Room"}}})}};
            const auto s=Rules::SceneFromJson(scene);
            Check(s.pieces.size()==1 && s.pieces[0].at("spec").at("width")==512 && s.pieces[0].at("rotation")[1]==16384 && s.pieces[0].at("spec").at("name")=="Hall","spec pieces");
            Json nested{{"pieces",Json::array({Json::array({Piece("Room","A",256,256,256,{0,0,0})}),Piece("Room","B",256,256,256,{0,512,0})})}};
            Check(Rules::SceneFromJson(nested).pieces.size()==2,"design.register's chunk lists");
            Json located{{"actors",Json::array({{{"id","X"},{"class","Engine.Light"},{"values",{{"Location",{{"X","10.0"},{"Y","20.0"},{"Z","30.0"}}}}}}})},{"map",{{"name","TESTMAP"},{"file","TestD.sdc"}}}};
            const auto l=Rules::SceneFromJson(located);
            Check(l.actors[0].position==Vector{10,20,30} && l.actors[0].name=="X" && l.mapName=="TESTMAP" && l.mapFile=="TestD.sdc","location struct and map section");
        }
        // Errors come first, then warnings, then info.
        {
            auto s=good;Drop(s,"Cam1");Actor(s,"Light1")["event"]="Nobody";
            const auto findings=Run(s);
            int last=0;
            for(const auto& f:findings)
            {
                const int rank=f.severity=="error"?0:f.severity=="warning"?1:2;
                Check(rank>=last,"sorted by severity");last=rank;
            }
            const auto summary=Rules::Summary(findings);
            Check(summary.at("errors").get<size_t>()>0 && summary.at("warnings").get<size_t>()>0 && summary.at("info").get<size_t>()>0,"summary counts");
            const auto json=Rules::ToJson(findings.front());
            Check(json.contains("severity") && json.contains("rule") && json.contains("wikiPage") && json.contains("message") && json.contains("actors"),"finding json");
        }

        // What the Map Design check and the security wiring read as intended.
        {
            // A sector's announcement: an SAlarm its SMagicEvent triggers, showing a message.
            auto s=good;
            s["actors"].push_back(A("Zone1_Announce","SBase.SAlarm",{0,0,0},{{"AlarmDescription","\"The vault is open\""},{"Events",Json::array()}},"Zone1_Announce"));
            s["actors"].push_back(A("Zone1_Complete","SBase.SMagicEvent",{0,0,0},{{"Groups",Json::array({{{"EventGroup",Json::array({{{"Event","Zone1_Announce"},{"Delay","0"},{"Type","EVT_Trigger"}}})}}})}},"Zone1_Complete"));
            Passes("security.alarm-unfed",s);Passes("security.alarm-no-output",s);Passes("events.dangling",s);
            auto t=s;Drop(t,"Zone1_Complete");
            Fails("security.alarm-unfed",t);
            // A magic event sending a Tag nothing has.
            auto u=s;Actor(u,"Zone1_Complete")["values"]["Groups"][0]["EventGroup"].push_back({{"Event","Nowhere"},{"Delay","1"}});
            Check(Fails("events.dangling",u).message.find("Nowhere")!=std::string::npos,"magic event target named");
            // A detector-fed alarm in message mode (MessagePriority 0 or 1) needs no outputs.
            auto v=good;auto& alarm=Actor(v,"Alarm1")["values"];alarm["Events"]=Json::array();alarm["ObjectiveTriggersToLock"]=Json::array();alarm["MoversToLock"]=Json::array();alarm["MessagePriority"]="0";
            Passes("security.alarm-no-output",v);
        }
        {
            // A platform above step height is reached by a ramp or a climbing GE.
            auto s=good;
            s["pieces"].push_back({{"spec",{{"kind","Platform"},{"name","Ledge"},{"width",256},{"length",256},{"height",128},{"construction","Carve"}}},{"position",{0,1500,0}},{"rotation",{0,0,0}}});
            Fails("design.traversal",s);
            auto t=s;t["ges"].push_back({{"kind","ledge"},{"points",{{-128,1372,128},{128,1372,128}}}});
            Passes("design.traversal",t);
            auto u=s;u["pieces"].push_back({{"spec",{{"kind","Ramp"},{"name","Ledge ramp"},{"width",128},{"length",512},{"height",128},{"construction","Carve"}}},{"position",{0,1116,0}},{"rotation",{0,0,0}}});
            Passes("design.traversal",u);
        }
        {
            // A doorway into a vent is passed crouched; a compiler-generated portal is meant to cut.
            auto s=good;
            s["pieces"].push_back(Piece("Vent","Duct",96,384,125,{-300,1900+16+192,0}));
            s["pieces"].push_back(Piece("Doorway","Duct opening",96,16,125,{-300,1900+8,0}));
            Passes("design.traversal",s);
            auto t=good;t["pieces"].push_back(Piece("Doorway","Low door",96,16,125,{300,1900+8,0}));
            Fails("design.traversal",t);
            t["pieces"][t["pieces"].size()-1]["generated"]=true;
            Passes("design.traversal",t);
            // Corridors and vents join what they overlap; two rooms merging is still worth a warning.
            auto u=good;u["pieces"].push_back(Piece("Corridor","Spur",192,512,192,{0,500,0}));
            Passes("design.traversal",u);
            auto v=good;v["pieces"].push_back(Piece("Room","Annex",512,512,256,{300,300,0}));
            Check(Fails("design.traversal",v).message.find("carve into each other")!=std::string::npos,"rooms merging");
            // A stairwell or shaft carves through the floors it joins on purpose, as
            // does anything the compiler generates, listed after the room or before it.
            auto w=v;w["pieces"][w["pieces"].size()-1]["spec"]["name"]="Stairwell";
            Passes("design.traversal",w);
            auto x=v;x["pieces"][x["pieces"].size()-1]["generated"]=true;
            Passes("design.traversal",x);
            auto y=good;y["pieces"].insert(y["pieces"].begin(),Piece("Room","Lift shaft",256,256,800,{300,300,0}));
            Passes("design.traversal",y);
            Passes("design.layout",y);
        }

        // Every rule has a failing and a passing fixture.
        for(const auto& rule:Rules::Catalog())
        {
            Check(failing.count(rule.id)==1,"no failing fixture for "+rule.id);
            if(rule.id!="info.summary")Check(passing.count(rule.id)==1,"no passing fixture for "+rule.id);
        }
        std::cout<<"LevelRulesModel tests passed ("<<Rules::Catalog().size()<<" rules)\n";
        return 0;
    }
    catch(const std::exception& e)
    {
        std::cerr<<e.what()<<"\n";
        return 1;
    }
}
