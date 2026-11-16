#include "../Reloaded.Editor/WorkflowModel.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
using namespace Workflow;
static int checks=0;
void Check(bool condition,const char* what) { ++checks; if(!condition) throw std::runtime_error(what); }
template<class F> void Reject(F f,const char* what) { bool failed=false;try{f();}catch(const std::exception&){failed=true;}Check(failed,what); }
int main()
{
    try
    {
        Json rename={{"changes",Json::array({{{"property","Tag"}},{{"property","Tag"}},{{"property","Event"}},{{"property","Groups[0].EventGroup[0].Event"}}})}};
        Check(SelectedTagChanges(rename).size()==4,"rename includes all assignments by default");
        rename["excluded"]={1,3};auto partial=SelectedTagChanges(rename);
        Check(partial.size()==2 && partial[0]["property"]=="Tag" && partial[1]["property"]=="Event","selectively exclude tags and nested events");
        rename["excluded"]={0,1};Reject([&]{SelectedTagChanges(rename);},"rename requires a target tag");
        for(const auto& bad:{Json::array({-1}),Json::array({4}),Json::array({1,1}),Json::array({1.5}),Json::array({"1"})})
        {rename["excluded"]=bad;Reject([&]{SelectedTagChanges(rename);},"invalid exclusions rejected");}
        std::string text="Begin Map\nBegin Actor Class=Engine.Trigger Name=A\n Tag=OpenDoor\n Event=CloseDoor\n Message=\"Actor'MyLevel.B'\"\n Target=Actor'\"MyLevel.B\"'\n Begin Object Class=Engine.Object Name=Owned\n Target=Actor'MyLevel.B'\n End Object\n Location=(X=0,Y=0,Z=0)\nEnd Actor\nBegin Actor Class=Engine.Mover Name=B\n Tag=CloseDoor\n Event=OpenDoor\nEnd Actor\nEnd Map\n";
        auto actors=ParseActors(text);Check(actors.size()==2,"parse actors with owned subobject");
        Check(References(text).size()==2,"do not treat authored strings as typed references");
        auto rewritten=RewriteReferences(text,{{"mylevel.b","Other.NewB"}});
        Check(rewritten.find("Message=\"Actor'MyLevel.B'\"")!=std::string::npos,"authored message preserved");
        Check(rewritten.find("Target=Actor'\"Other.NewB\"'")!=std::string::npos,"case insensitive path rewrite");
        Check(RewriteReferences("X=Actor'MyLevel.Bar'",{{"MyLevel.B","New.B"}})=="X=Actor'MyLevel.Bar'","reference prefix boundary");
        Check(RewriteReferences("X=Actor'MyLevel.B'",{{"MyLevel.B",""}})=="X=None","explicit unbound");
        auto set=SetProperty(actors[0].text,"Target","None");
        Check(Property(set,"Target")=="None","set top-level property");
        Check(set.find(" Target=Actor'MyLevel.B'")!=std::string::npos,"nested property preserved");
        Check(Property(SetProperty(set,"Location","(X=7,Y=8,Z=9)"),"Location")=="(X=7,Y=8,Z=9)","update existing property");
        const std::string baked="Lighting=StaticMeshInstance'MyLevel.One'\n Begin Object Class=Object Name=Owned\n CustomCache=StaticMeshInstance'MyLevel.Two'\n End Object\nMessage=\"StaticMeshInstance'MyLevel.Keep'\"\nMesh=StaticMesh'Assets.RealMesh'\nReferences=(Cache=StaticMeshInstance'MyLevel.InStruct',Other=None)\n";
        std::vector<std::string> dropped;
        const auto clean=RemoveBakedLighting(baked,&dropped);
        Check(dropped==std::vector<std::string>{"MyLevel.One","MyLevel.Two"},"baked lighting omitted under any field name and at nested object depth");
        Check(clean.find("Lighting=")==std::string::npos && clean.find("CustomCache=")==std::string::npos,"assembly hotfix strips the complete lighting property lines");
        Check(clean.find("Message=\"StaticMeshInstance'MyLevel.Keep'\"")!=std::string::npos && clean.find("StaticMesh'Assets.RealMesh'")!=std::string::npos,"assembly hotfix preserves authored strings and real mesh assets");
        Check(clean.find("References=(Cache=StaticMeshInstance'MyLevel.InStruct',Other=None)")!=std::string::npos,"complex struct syntax is retained for runtime type filtering");
        Reject([]{ParseActors("Begin Actor Class=Engine.Actor Name=A\n");},"reject incomplete text");
        Reject([&]{ParseActors(text+actors[0].text);},"reject duplicate names");
        for(const Rotation r: {Rotation{0,16384,0},Rotation{7000,12000,5000},Rotation{16384,4000,0}})
        {
            Pose frame{{20,-15,40},r};Vector point{70,3,-90};auto back=TransformPoint(TransformPoint(point,frame),frame,true);
            for(int i=0;i<3;++i) Check(std::abs(back[i]-point[i])<1e-8,"point transform roundtrip");
            auto rotated=TransformRotation({4000,9000,-3000},r);auto restored=TransformRotation(rotated,r,true);
            Pose first{{},{4000,9000,-3000}},second{{},restored};
            auto v1=TransformPoint({1,2,3},first),v2=TransformPoint({1,2,3},second);
            for(int i=0;i<3;++i) Check(std::abs(v1[i]-v2[i])<0.002,"rotation composition roundtrip");
        }
        Json definition={{"id","stable"},{"name","Door"},{"actors",Json::array()},{"bindings",Json::array()}};
        for(auto& actor:actors) definition["actors"].push_back({{"name",actor.name},{"path","MyLevel."+actor.name},{"text",actor.text},{"tag",Property(actor.text,"Tag")},{"event",Property(actor.text,"Event")},{"position",Vector{1,2,3}},{"rotation",Rotation{}}});
        auto placement=PreparePlacement(definition,{{100,200,300},{0,16384,0}},"I1_","Other",{});
        std::string placed=placement.at("t3d");
        Check(placed.find("Name=I1_A")!=std::string::npos,"unique actor names");
        Check(placed.find("Event=I1_CloseDoor")!=std::string::npos,"event remap");
        Check(placed.find("Tag=I1_CloseDoor")!=std::string::npos,"tag remap");
        Check(placement.at("tags").value("CloseDoor",std::string())=="I1_CloseDoor","placement reports its tag renames");
        Check(placed.find("Other.I1_B")!=std::string::npos,"internal actor reference remap");
        Check(definition.at("actors")[0].at("name")=="A","placement leaves saved definition unchanged");
        auto canonical=CanonicalizeAssembly(definition,{{"MyLevel.A","SavedA"},{"MyLevel.B","SavedB"}});
        Check(canonical["actors"][0]["name"]=="SavedA" && canonical["actors"][0]["path"]=="Assembly.SavedA","canonical member identity retained during update");
        Check(canonical["actors"][0]["text"].get<std::string>().find("Assembly.SavedB")!=std::string::npos,"canonical internal references remapped");
        auto prefixed=definition;prefixed["actors"][0]["tag"]="RE_0123456789ab_Group";prefixed["actors"][1]["tag"]="RE_abcdef012345_Group";
        prefixed["actors"][0]["event"]="RE_abcdef012345_Group";
        auto groups=CanonicalizeAssembly(prefixed,{});
        Check(groups["actors"][0]["tag"]!=groups["actors"][1]["tag"],"canonicalization preserves distinct instance tag groups");
        Check(groups["actors"][0]["event"]==groups["actors"][1]["tag"],"canonicalization preserves tag-group relationships");
        Check(NormalizeExportNames("Name=Brush\xA7(123)\nMessage=\"Brush\xA7(123)\"\n")=="Name=Brush\nMessage=\"Brush\xA7(123)\"\n","native diagnostic name suffix removed only outside strings");
        auto bound=definition;bound["bindings"].push_back({{"id","external"},{"label","External target"},{"kind","object"},{"path","Elsewhere.Actor"}});
        Reject([&]{PreparePlacement(bound,Pose{},"I_","Map",{});},"external binding must be explicitly resolved or unbound");
        Check(!PreparePlacement(bound,Pose{},"I_","Map",{{"external",""}}).empty(),"explicit optional unbound accepted");
        // Particle emitters: stock sub-emitters are exported under the map
        // package, workbench components under their actor, and paste creates
        // both directly in the map package.
        const auto npos=std::string::npos;
        std::string stock="Begin Actor Class=Emitter Name=RE_0123456789ab_Emitter5\r\n    Begin Object Class=SpriteEmitter Name=RE_0123456789ab_SpriteEmitter6\r\n        Texture=Texture'sfx.Emitter.smoke_grenade'\r\n        Name=\"SpriteEmitter6\"\r\n    End Object\r\n    Emitters(0)=SpriteEmitter'MyLevel.RE_0123456789ab_SpriteEmitter6'\r\n    Tag=\"Emitter\"\r\nEnd Actor\r\n";
        std::string component="Begin Actor Class=Emitter Name=Emitter_Magic_1\r\n    Begin Object Class=SpriteEmitter Name=SpriteEmitter6\r\n        Name=\"SpriteEmitter6\"\r\n    End Object\r\n    Emitters(0)=SpriteEmitter'MyLevel.Emitter_Magic_1.SpriteEmitter6'\r\nEnd Actor\r\n";
        Check(InlineObjectNames(stock)==std::vector<std::string>{"RE_0123456789ab_SpriteEmitter6"} && InlineObjectNames("Begin Actor Class=Brush Name=B\r\nBegin Brush Name=Model1\r\nBegin PolyList\r\nEnd PolyList\r\nEnd Brush\r\nEnd Actor\r\n")==std::vector<std::string>{"Model1"},"inline object names come from nested Begin Object/Brush lines only");
        auto emitterActor=[](const std::string& text,const std::string& name){return Json{{"name",name},{"class","Engine.Emitter"},{"path","MyLevel."+name},{"text",text},{"tag","None"},{"event","None"},{"position",Vector{}},{"rotation",Rotation{}}};};
        auto emitterDefinition=[&](Json actors){return Json{{"id","fx"},{"name","FX"},{"pivot",Vector{}},{"bindings",Json::array()},{"dependencies",Json::array()},{"actors",std::move(actors)}};};
        auto fx=CanonicalizeAssembly(emitterDefinition(Json::array({emitterActor(stock,"RE_0123456789ab_Emitter5"),emitterActor(component,"Emitter_Magic_1")})),{});
        auto fx0=fx["actors"][0]["text"].get<std::string>(),fx1=fx["actors"][1]["text"].get<std::string>();
        Check(fx0.find("Begin Object Class=SpriteEmitter Name=SpriteEmitter6\r\n")!=npos && fx0.find("Emitters(0)=SpriteEmitter'\"Assembly.SpriteEmitter6\"'")!=npos,"canonical sub-object drops its placement prefix with its reference");
        Check(fx1.find("Name=SpriteEmitter6_1\r\n")!=npos && fx1.find("Emitters(0)=SpriteEmitter'\"Assembly.SpriteEmitter6_1\"'")!=npos,"components of different actors sharing a name stay distinct");
        Check(fx0.find("Name=\"SpriteEmitter6\"")!=npos && fx0.find("Texture=Texture'sfx.Emitter.smoke_grenade'")!=npos,"particle emitter Name string and asset references are untouched");
        Check(fx1.find("Name=\"SpriteEmitter6_1\"")!=npos,"a particle emitter Name string spelling its object name follows the rename");
        auto innerNames=[](const Json& definition){std::vector<std::string> names;for(const auto& actor:definition["actors"]){names.push_back(actor["name"]);for(auto& n:InlineObjectNames(actor["text"]))names.push_back(n);}return names;};
        auto cycle=fx;Json first;
        for(int round=0;round<3;++round)
        {
            // Place, read the pasted text back as native copy would, save again.
            auto instance=PreparePlacement(cycle,{{64,0,0},{}},"RE_"+std::string(12,static_cast<char>('a'+round))+"_","MyLevel",{});
            const std::string t3d=instance["t3d"];
            Check(t3d.find("SpriteEmitter'\"MyLevel.RE_"+std::string(12,static_cast<char>('a'+round))+"_SpriteEmitter6_1\"'")!=npos && t3d.find("Emitter_Magic_1.")==npos,"placed references name sub-objects in the map package");
            Json recaptured=Json::array();
            for(const auto& actor:ParseActors(t3d)) recaptured.push_back(emitterActor(actor.text,actor.name));
            cycle=CanonicalizeAssembly(emitterDefinition(recaptured),{});
            Check(innerNames(cycle)==innerNames(fx),"actor and sub-object names do not grow across save and place cycles");
            if(round==0)first=cycle;
            for(size_t i=0;i<2;++i)Check(cycle["actors"][i]["text"]==first["actors"][i]["text"],"saved text is identical after every place and save cycle");
            for(const auto& n:innerNames(cycle)) Check(n.size()+16<64,"names fit the native 64-byte Name= buffer after a prefix");
        }
        auto single=PreparePlacement(emitterDefinition(Json::array({emitterActor(component,"Emitter_Magic_1")})),Pose{},"I_","Other",{});
        const std::string singleText=single["t3d"];
        Check(singleText.find("Begin Object Class=SpriteEmitter Name=I_SpriteEmitter6\r\n")!=npos && singleText.find("Emitters(0)=SpriteEmitter'\"Other.I_SpriteEmitter6\"'")!=npos,"a component owned by its actor resolves where paste creates it");
        auto pair=PreparePlacement(emitterDefinition(Json::array({emitterActor(component,"Emitter_Magic_1"),emitterActor(std::regex_replace(component,std::regex("Emitter_Magic_1"),"Emitter_Magic_2"),"Emitter_Magic_2")})),Pose{},"I_","Other",{});
        const std::string pairText=pair["t3d"];
        Check(pairText.find("Name=I_SpriteEmitter6\r\n")!=npos && pairText.find("Name=I_SpriteEmitter6_1\r\n")!=npos && pairText.find("'\"Other.I_SpriteEmitter6_1\"'")!=npos,"raw components sharing a name get distinct placed names");
        // A definition saved before sub-objects were canonicalized keeps actor
        // paths Assembly.X but package-level references as captured.
        std::string oldBrush="Begin Actor Class=Brush Name=Brush1436\r\n    Begin Brush Name=Model1438\r\n       Begin PolyList\r\n       End PolyList\r\n    End Brush\r\n    Brush=Model'MyLevel.Model1438'\r\nEnd Actor\r\n";
        std::string oldEmitter="Begin Actor Class=Emitter Name=Emitter5\r\n    Begin Object Class=SpriteEmitter Name=SpriteEmitter6\r\n        Name=\"SpriteEmitter6\"\r\n    End Object\r\n    Emitters(0)=SpriteEmitter'MyLevel.SpriteEmitter6'\r\nEnd Actor\r\n";
        auto savedActor=[&](const std::string& text,const std::string& name){auto actor=emitterActor(text,name);actor["path"]="Assembly."+name;actor["sourcePath"]="MyLevel."+name;return actor;};
        auto legacy=emitterDefinition(Json::array({savedActor(oldBrush,"Brush1436"),savedActor(oldEmitter,"Emitter5")}));
        for(const std::string level:{"MyLevel","Other"})
        {
            const std::string t3d=PreparePlacement(legacy,Pose{},"RE_aaaaaaaaaaaa_",level,{})["t3d"];
            Check(t3d.find("Begin Brush Name=RE_aaaaaaaaaaaa_Model1438\r\n")!=npos && t3d.find("Brush=Model'\""+level+".RE_aaaaaaaaaaaa_Model1438\"'")!=npos,"an older saved brush references its own placed model");
            Check(t3d.find("Name=RE_aaaaaaaaaaaa_SpriteEmitter6\r\n")!=npos && t3d.find("Emitters(0)=SpriteEmitter'\""+level+".RE_aaaaaaaaaaaa_SpriteEmitter6\"'")!=npos && t3d.find("'MyLevel.Model1438'")==npos && t3d.find("'MyLevel.SpriteEmitter6'")==npos,"an older saved stock emitter references its own placed sub-emitter");
        }
        const std::string migrated=CanonicalizeAssembly(legacy,{})["actors"][0]["text"];
        Check(migrated.find("Brush=Model'\"Assembly.Model1438\"'")!=npos,"saving an older definition again names its sub-objects under Assembly");
        // Older captures also listed a stock sub-emitter as a dependency.
        legacy["dependencies"]=Json::array({"Engine.Brush","Engine.Emitter","GAR_TXT.BSP.GAR_beton","MyLevel.SpriteEmitter6","MyLevel.Trigger3"});
        Check(PlacementDependencies(legacy)==Json::array({"Engine.Brush","Engine.Emitter","GAR_TXT.BSP.GAR_beton","MyLevel.Trigger3"}),"an older definition's own sub-emitter is not a placement dependency");
        auto current=fx;current["dependencies"]=Json::array({"Engine.Emitter","sfx.Emitter.smoke_grenade"});
        Check(PlacementDependencies(current)==current["dependencies"],"a current definition places with every listed dependency");
        auto longName=emitterDefinition(Json::array({emitterActor("Begin Actor Class=Emitter Name=E\r\nBegin Object Class=SpriteEmitter Name=RE_0123456789ab_"+std::string(55,'x')+"\r\nEnd Object\r\nEnd Actor\r\n","E")}));
        Check(InlineObjectNames(CanonicalizeAssembly(longName,{})["actors"][0]["text"]).at(0).size()==40,"long sub-object names are shortened to leave room for a prefix");
        Json entries=Json::array({{{"id","view1"},{"name","Roof"},{"cameras",Json::array()}}});
        UpdateEntry(entries,"view1",{{"id","wrong"},{"name","wrong"},{"cameras",Json::array({1})}});
        Check(entries.size()==1 && entries[0]["id"]=="view1" && entries[0]["name"]=="Roof" && entries[0]["cameras"].size()==1,"view update retains identity without duplication");
        auto before=entries; Reject([&]{UpdateEntry(entries,"missing",Json{});},"reject stale entry");Check(entries==before,"stale update does not mutate entries");
        Json assemblyEntries=Json::array({definition});auto changed=definition;changed["actors"].erase(0);UpdateEntry(assemblyEntries,"stable",changed);
        Check(assemblyEntries.size()==1 && assemblyEntries[0]["id"]=="stable" && assemblyEntries[0]["actors"].size()==1,"assembly membership update in place");
        // Whole-map actor lookups: one index, the same answers as a search in list order.
        PathIndex<int> paths;paths.Add("MyLevel.light3",1);paths.Add("MyLevel.LIGHT3",2);paths.Add("MyLevel.Brush0",3);
        Check(paths.Find("MyLevel.light3") && *paths.Find("MyLevel.light3")==1,"path index keeps the first of two paths that differ only in case");
        Check(paths.Find("MYLEVEL.Light3") && *paths.Find("MYLEVEL.Light3")==1 && paths.Find("mylevel.brush0") && *paths.Find("mylevel.brush0")==3,"path index folds the query");
        PathIndex<int> guarded;guarded.Add("None",4);guarded.Add("",5);
        Check(guarded.size()==2 && !guarded.Find("None") && !guarded.Find("NONE") && !guarded.Find(""),"path index finds nothing for an empty or None path, even one it holds");
        Check(!paths.Find("MyLevel.light") && !paths.Find("MyLevel.light34") && !paths.Find("Other.light3"),"path index finds nothing for a missing path");
        Check(paths.size()==2,"path index counts distinct folded paths");
        auto dir=std::filesystem::temp_directory_path()/("workflow-test-"+Id());auto path=dir/"library.json";
        Json doc={{"version",1},{"entries",entries}};WriteDocument(path,doc);Check(ReadDocument(path,Json{})==doc,"disk roundtrip");
        doc["entries"]=assemblyEntries;WriteDocument(path,doc);Check(ReadDocument(path,Json{})==doc,"atomic overwrite");
        auto bad=dir/"bad.json";{std::ofstream out(bad);out<<"{broken";}Reject([&]{ReadDocument(bad,Json{});},"corrupt document does not become empty library");
        {std::ofstream out(bad);out<<"{\"version\":2}";}Reject([&]{ReadDocument(bad,Json{});},"future document version rejected");
        auto blocked=path/"child.json";Reject([&]{WriteDocument(blocked,doc);},"failed filesystem write reported");Check(ReadDocument(path,Json{})==doc,"failed write preserves existing document");
        std::filesystem::remove(path);std::filesystem::remove(bad);std::filesystem::remove(dir);
        std::cout<<"PASS "<<checks<<" workflow model checks\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
