// scct.level-spec shape checks (Reloaded.Editor.Mcp/LevelSpecModel.h). Run from
// the repository root; "--write-schema" rewrites the published JSON Schema from
// Mcp::Spec::Schema() instead of checking it.
#include "../Reloaded.Editor.Mcp/LevelSpecModel.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
using namespace Mcp;
using Workflow::Json;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void Reject(F f,std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception&){caught=true;}
    if(!caught)throw std::runtime_error("invalid spec accepted at line "+std::to_string(where.line()));
}
// The pointer a bad spec is refused at.
std::string Pointer(const Json& spec)
{
    try{Spec::Parse(spec);}catch(const Spec::SpecError& e){return e.pointer;}
    return "(accepted)";
}
const std::filesystem::path kResources="Reloaded.Editor.Mcp/Resources";
Json Read(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if(!file)throw std::runtime_error("cannot open "+path.string()+"; run from the repository root");
    return Json::parse(file);
}
Json Base(){return {{"format","scct.level-spec"},{"version",1}};}
Json With(const char* section,Json value){auto s=Base();s[section]=std::move(value);return s;}
Json Room(const char* id,double x=0){return {{"id",id},{"kind","Room"},{"size",{512,512,256}},{"at",{x,0}}};}

void Published(bool write)
{
    const auto path=kResources/"schemas"/"level-spec.schema.json";
    if(write)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path)<<Spec::Schema().dump(2)<<"\n";
        std::cout<<"wrote "<<path.string()<<"\n";
    }
    Check(Read(path)==Spec::Schema(),"the published level-spec schema is Mcp::Spec::Schema() (rerun with --write-schema)");
    const auto& schema=Spec::Schema();
    Check(schema.at("properties").at("format").at("const")=="scct.level-spec" && schema.at("additionalProperties")==false,"top level is closed");
    for(const char* section:{"map","units","profile","defaults","storeys","zones","geometry","spawns","gameplay","security","mechanisms","events","environment","ges","geTriggers","settings","level","interface","release","existing","extra","provenance"})
        Check(schema.at("properties").contains(section),"every contract section is in the schema");
    Check(schema.at("$defs").at("id").at("pattern")=="^[A-Za-z_][A-Za-z0-9_]{0,62}$","id pattern");
    const auto& piece=schema.at("properties").at("geometry").at("properties").at("pieces").at("items");
    Check(piece.at("properties").at("kind").at("enum").size()==11,"eleven piece kinds");
}
void Parsing()
{
    auto spec=Spec::Parse(Base());
    Check(spec.at("map").at("name")=="UNTITLED" && spec.at("map").at("file")=="UntiD","map defaults");
    Check(spec.at("units").at("unit")=="uu" && spec.at("units").at("uuPerMetre")==100 && spec.at("profile")=="draft","units and profile defaults");
    Check(spec.at("defaults").at("construction")=="Carve" && spec.at("defaults").at("thickness")==16 && spec.at("defaults").at("lights")=="rooms" && spec.at("defaults").at("startsPerTeam")==4,"build defaults");
    auto named=Base();named["map"]={{"name","AQUARIUM"}};
    Check(Spec::Parse(named).at("map").at("file")=="AquaD","file stem from the name: four letters and D");
    Check(Spec::DefaultFile("ICE STATION")=="IcesD" && Spec::DefaultFile("42")=="MapD","default stems");
    // Every example is a valid spec.
    size_t examples=0;
    for(const auto& entry:std::filesystem::directory_iterator(kResources/"examples"))
        if(entry.path().extension()==".json"){Spec::Parse(Read(entry.path()));++examples;}
    Check(examples>=5,"five example specs");
}
void Refusals()
{
    Check(Pointer(Json::array())=="","not an object");
    Check(Pointer({{"format","scct.map-changes"},{"version",1}})=="/format","wrong format");
    Check(Pointer({{"format","scct.level-spec"},{"version",2}})=="/version","wrong version");
    Check(Pointer(With("geometry",{{"rooms",Json::array()}}))=="/geometry/rooms","unknown section field");
    auto unknown=With("geometry",{{"pieces",{Room("A")}}});unknown["geometry"]["pieces"][0]["colour"]="red";
    Check(Pointer(unknown)=="/geometry/pieces/0/colour","unknown item field, pointed at");
    auto kind=With("geometry",{{"pieces",{Room("A")}}});kind["geometry"]["pieces"][0]["kind"]="Rom";
    Check(Pointer(kind)=="/geometry/pieces/0/kind","enum");
    try{Spec::Parse(kind);}catch(const Spec::SpecError& e){Check(std::string(e.what()).find("Room, Corridor")!=std::string::npos,"the choices are named");}
    auto size=With("geometry",{{"pieces",{Room("A")}}});size["geometry"]["pieces"][0]["size"]={512,512};
    Check(Pointer(size)=="/geometry/pieces/0/size","size has three numbers");
    Check(Pointer(With("geometry",{{"pieces",{{{"id","1A"},{"kind","Room"},{"size",{1,1,1}}}}}}))=="/geometry/pieces/0/id","id pattern");
    Check(Pointer(With("geometry",{{"pieces",{{{"kind","Room"},{"size",{1,1,1}}}}}}))=="/geometry/pieces/0","missing id");
    // Ids are unique ignoring case, across sections.
    auto twice=With("geometry",{{"pieces",Json::array({Room("Hall"),Room("HALL",600)})}});
    Check(Pointer(twice)=="/geometry/pieces/1/id","duplicate id ignoring case");
    auto across=With("geometry",{{"pieces",{Room("Hall")}}});across["security"]={{"alarms",{{{"id","hall"}}}}};
    Check(Pointer(across)=="/security/alarms/0/id","an id is unique across sections");
    auto refs=With("existing",{{"refs",{{"Door1",{{"path","MyLevel.Mover3"}}}}}});refs["security"]={{"alarms",{{{"id","door1"}}}}};
    Check(Pointer(refs)=="/security/alarms/0/id","existing refs take ids too");
    Check(Pointer(With("existing",{{"refs",{{"9bad",{{"path","MyLevel.Mover3"}}}}}}))=="/existing/refs/9bad","existing ref names are ids");
    // Positions: each form, and the furthest error of the form it nearly is.
    auto at=[](Json position){return With("spawns",{{"spy",{{{"id","S1"},{"at",position}}}}});};
    Spec::Parse(at({0,0}));Spec::Parse(at({0,0,"floor+48"}));Spec::Parse(at({{"in","Hall"},{"u",0.25},{"h","ceiling-32"}}));Spec::Parse(at({{"near","S2"},{"offset",{0,64}}}));Spec::Parse(at({{"doorway","D1"}}));
    Check(Pointer(at({{"in","Hall"},{"u",0.75}}))=="/spawns/spy/0/at/u","u runs -0.5 to 0.5");
    Check(Pointer(at({{"in","Hall"},{"w",1}}))=="/spawns/spy/0/at/w","unknown position field");
    Check(Pointer(at({0,0,"roof+8"}))=="/spawns/spy/0/at/2","heights are floor+N or ceiling-N");
    Check(Pointer(at("hall"))=="/spawns/spy/0/at","a position is not text");
    try{Spec::Parse(at(true));}catch(const Spec::SpecError& e){Check(std::string(e.what()).find("{in, u, v, h}")!=std::string::npos,"the position forms are listed");}
    // Ranges and closed enums elsewhere.
    Check(Pointer(With("security",{{"alarms",{{{"id","A"},{"priority",6}}}}}))=="/security/alarms/0/priority","MessagePriority 0-5");
    Check(Pointer(With("gameplay",{{"modes",{"story","story"}}}))=="/gameplay/modes/1","modes are unique");
    Check(Pointer(With("gameplay",{{"modes",{"ctf"}}}))=="/gameplay/modes/0","modes are story, diskhunt, deathmatch");
    auto disks=Base();disks["gameplay"]=Json::parse(R"({"diskhunt":{"disks":[{"id":"D1","at":[0,0],"group":4}]}})");
    Check(Pointer(disks)=="/gameplay/diskhunt/disks/0/group","disk groups 1-3");
    auto zones=Base();zones["gameplay"]=Json::parse(R"({"story":{"objectives":[{"id":"O","triggers":[{"id":"F","kind":"flag","dropZones":[{"at":[0,0]},{"at":[0,0]},{"at":[0,0]},{"at":[0,0]},{"at":[0,0]},{"at":[0,0]}]}]}]}})");
    Check(Pointer(zones)=="/gameplay/story/objectives/0/triggers/0/dropZones","1 to 5 drop zones");
    Check(Pointer(With("gameplay",{{"targets",{{"mission","M"},{"sizes",{1,2,3}}}}}))=="/gameplay/targets/sizes","targets take eight sizes");
    Check(Pointer(With("map",{{"file","Bad Name"}}))=="/map/file","file stems have no spaces");
    Check(Pointer(With("units",{{"unit","yd"}}))=="/units/unit","units");
    // Lifts: the doors' mesh and how they open, and a storey's door placed by hand.
    auto lift=With("mechanisms",Json::parse(R"({"lifts":[{"id":"L","shaft":"S","storeys":["A","B"],"mesh":"P.G.Car","doorMesh":"P.G.Door","carDoorMesh":"P.G.Inner",
        "doorOpen":{"yaw":90},"doorMoveTime":1.5,"carSwitch":false,"doors":[{"storey":"B","doorway":"D"},{"storey":"A","at":[0,0,0],"yaw":90,"mesh":"P.G.Other"}]}]})"));
    Spec::Parse(lift);
    auto badDoor=lift;badDoor["mechanisms"]["lifts"][0]["doors"][0]["side"]="east";
    Check(Pointer(badDoor)=="/mechanisms/lifts/0/doors/0/side","a lift door entry is closed");
    auto noStorey=lift;noStorey["mechanisms"]["lifts"][0]["doors"][0].erase("storey");
    Check(Pointer(noStorey)=="/mechanisms/lifts/0/doors/0","a lift door entry names its storey");
    // Raw bags are not looked into, so their keys are free.
    auto bag=With("security",{{"alarms",{{{"id","A"},{"props",{{"Anything",{{"id","A"}}}}}}}}});
    Spec::Parse(bag);
}
void Merging()
{
    Json base=With("geometry",{{"pieces",Json::array({Room("A"),Room("B",600)})}});
    base["security"]={{"alarms",{{{"id","Alarm1"},{"duration",60}}}}};
    Json fragment=Json::parse(R"({"geometry":{"pieces":[{"id":"b","size":[256,256,256]},{"id":"C","kind":"Room","size":[512,512,256],"at":[1200,0]}]},"security":{"alarms":[{"id":"Alarm1","priority":3}]},"profile":"release"})");
    auto merged=Spec::Merge(base,fragment);
    const auto& pieces=merged.at("geometry").at("pieces");
    Check(pieces.size()==3 && pieces[1].at("id")=="B" && pieces[1].at("size")==Json({256,256,256}) && pieces[1].at("kind")=="Room","items merge by id ignoring case, fragment fields win");
    Check(pieces[2].at("id")=="C","new ids are appended");
    Check(merged.at("security").at("alarms")[0].at("duration")==60 && merged.at("security").at("alarms")[0].at("priority")==3,"fields of an item merge");
    Check(merged.at("profile")=="release" && merged.at("format")=="scct.level-spec","other values are replaced, the rest kept");
    auto removed=Spec::Merge(base,{{"geometry",{{"pieces",{{{"id","A"},{"$remove",true}}}}}}});
    Check(removed.at("geometry").at("pieces").size()==1 && removed.at("geometry").at("pieces")[0].at("id")=="B","$remove takes an item out");
    auto fresh=Spec::Merge(Base(),Json::parse(R"({"security":{"alarms":[{"id":"Gone","$remove":true},{"id":"Kept"}]}})"));
    Check(fresh.at("security").at("alarms")==Json::parse(R"([{"id":"Kept"}])"),"a list the base lacks takes the fragment's items, not its removals");
    auto nothing=Spec::Merge(base,{{"geometry",{{"pieces",{{{"id","Z"},{"$remove",true}}}}}}});
    Check(nothing.at("geometry").at("pieces").size()==2,"removing an unknown id changes nothing");
    auto plain=Spec::Merge(With("gameplay",{{"modes",{"story","deathmatch"}}}),With("gameplay",{{"modes",{"diskhunt"}}}));
    Check(plain.at("gameplay").at("modes")==Json({"diskhunt"}),"lists without ids are replaced");
    auto props=Spec::Merge(With("security",{{"alarms",{{{"id","A"},{"props",{{"X","1"},{"Y","2"}}}}}}}),With("security",{{"alarms",{{{"id","A"},{"props",{{"Z","3"}}}}}}}));
    Check(props.at("security").at("alarms")[0].at("props")==Json({{"Z","3"}}),"a raw property bag is replaced whole");
    Spec::Parse(merged);
}
void Hashing()
{
    const auto a=Spec::Parse(Base()),b=Spec::Parse(With("profile","release"));
    Check(Spec::Hash(a)==Spec::Hash(Spec::Parse(Base())) && Spec::Hash(a)!=Spec::Hash(b) && Spec::Hash(a).size()==16,"hash");
    Check(Spec::Child("/a","b/c~d")=="/a/b~1c~0d" && Spec::Child("/a",3)=="/a/3","pointer escaping");
    Check(Spec::ValidId("Zone1_Complete") && !Spec::ValidId("1Zone") && !Spec::ValidId("a-b") && !Spec::ValidId(std::string(64,'a')) && Spec::ValidId(std::string(63,'a')),"ids");
    Check(Spec::Scale(With("units",{{"unit","m"}}))==100 && std::abs(Spec::Scale(With("units",{{"unit","ft"},{"uuPerMetre",50}}))-15.24)<1e-9 && Spec::Scale(Base())==1,"unit scale");
}
int main(int argc,char** argv)
{
    try
    {
        Published(argc>1 && std::string(argv[1])=="--write-schema");
        Parsing();
        Refusals();
        Merging();
        Hashing();
        std::cout<<"LevelSpecModelTests passed\n";
        return 0;
    }
    catch(const std::exception& e)
    {
        std::cerr<<"LevelSpecModelTests failed: "<<e.what()<<"\n";
        return 1;
    }
}
