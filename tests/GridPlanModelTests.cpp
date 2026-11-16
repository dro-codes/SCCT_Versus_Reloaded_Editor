// The grid importer and the plan model the 2D importers share. The compiler
// check uses the compiler's own wiki checks (RE_SPEC_NO_RULES), so this suite
// links only WorkflowModel.cpp.
#define RE_SPEC_NO_RULES 1
#include "../Reloaded.Editor.Mcp/GridPlanModel.h"
#include "../Reloaded.Editor.Mcp/LevelSpecModel.h"
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
std::filesystem::path FixtureDir()
{
    for(const auto& dir:{std::filesystem::path("tests/mcp/fixtures"),std::filesystem::path("mcp/fixtures"),std::filesystem::path(__FILE__).parent_path()/"mcp/fixtures"})
        if(std::filesystem::exists(dir/"grid-basic.txt"))return dir;
    throw std::runtime_error("missing fixtures (tests/mcp/fixtures)");
}
std::string Read(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);std::ostringstream s;s<<in.rdbuf();return s.str();}
Json FromGrid(const std::string& text,const Json& options,Json& report){report=Json::object();return Grid::ImportText(text,options,report);}
const Json* ById(const Json& list,const std::string& id){for(const auto& item:list)if(item.value("id",std::string())==id)return &item;return nullptr;}
bool Warned(const Json& report,const std::string& part){for(const auto& w:report.at("warnings"))if(w.get<std::string>().find(part)!=std::string::npos)return true;return false;}
// No error diagnostics from the spec compiler.
void Compiles(const Json& spec,const char* what)
{
    Spec::Parse(spec);
#ifdef RE_TEST_COMPILER
    Spec::CompileOptions options;options.build=false;options.screenshot=false;
    const auto compiled=Spec::Compile(spec,options);
    for(const auto& d:compiled.diagnostics)
        if(d.severity=="error")throw std::runtime_error(std::string(what)+" does not compile: "+d.pointer+" "+d.message);
#else
    (void)what;
#endif
}
int main(int argc,char** argv)
{
    try
    {
        // --update-golden rewrites the golden file after a deliberate change.
        const bool update=argc>1 && std::string(argv[1])=="--update-golden";
        const auto fixtures=FixtureDir();
        // The golden plan: two storeys, every legend character.
        {
            Json report;
            const auto spec=FromGrid(Read(fixtures/"grid-basic.txt"),{{"name","Grid test"}},report);
            Compiles(spec,"grid-basic");
            const auto golden=fixtures/"grid-basic.expected.json";
            if(update){std::ofstream(golden,std::ios::binary)<<spec.dump(1)<<"\n";std::cout<<"wrote "<<golden.string()<<"\n";}
            const auto expected=Json::parse(Read(golden));
            if(spec!=expected)
            {
                std::cerr<<"grid-basic differs from its golden file:\n"<<spec.dump(1)<<"\n";
                throw std::runtime_error("grid golden mismatch (run with --update-golden to rewrite it after checking the change)");
            }
            const auto& counts=report.at("counts");
            Check(counts.at("rooms")==7 && counts.at("corridors")==3 && counts.at("vents")==1,"rectangles");
            Check(counts.at("doorways")==4 && counts.at("joins")==9,"doorways and joins");
            Check(counts.at("spyStarts")==4 && counts.at("mercStarts")==4 && counts.at("objectives")==3 && counts.at("camnet")==2,"markers");
            Check(report.at("warnings").empty(),"no warnings for the golden plan");
            Check(spec.at("storeys")[1].at("z")==384,"storey height");
            Check(spec.at("defaults").at("lights")=="none","own lights");
            // The doors cut the 1-cell walls between the bases and the server room.
            const auto* door=ById(spec.at("geometry").at("doorways"),"Door2");
            Check(door && door->at("host")=="Room1" && door->at("side")=="east" && door->at("depth")==128 && door->at("size")[0]==128,"door through a wall");
            // Each drop zone joins the nearest flag.
            const auto& flag=spec.at("gameplay").at("story").at("objectives")[2].at("triggers")[0];
            Check(flag.at("kind")=="flag" && flag.at("dropZones").size()==1,"flag drop zone");
        }
        // The same plan as CSV, with names and characters mixed.
        {
            const std::string ascii="#####\n#S.M#\n#...#\n#.O.#\n#####\n";
            const std::string csv="#,#,#,#,#\n#,spy,floor,merc,#\n#,Floor,.,open,#\n#,.,terminal,.,#\n#,,,,\n";
            Json a,b;
            auto one=FromGrid(ascii,Json::object(),a),two=FromGrid(csv,Json::object(),b);
            one.erase("provenance");two.erase("provenance");
            Check(one==two,"CSV and ASCII grids agree");
            Check(one.at("geometry").at("pieces").size()==1 && one.at("geometry").at("pieces")[0].at("size")==Json::array({384,384,320}),"one room");
        }
        // Legend overrides, unknown characters, lower case.
        {
            Json report;
            const auto spec=FromGrid("~~~~\n~s?~\n~~~~\n",{{"legend",{{"~","solid"}}}},report);
            Check(spec.at("spawns").at("spy").size()==1,"lower-case legend letter");
            Check(Warned(report,"'?'"),"unknown character reported");
            Reject([&]{Json r;FromGrid("#.#",{{"legend",{{"~","lava"}}}},r);},"unknown");
            Reject([&]{Json r;FromGrid("#.#",{{"legend",{{"ab","floor"}}}},r);},"one character");
            Reject([&]{Json r;FromGrid("####\n#  #\n",Json::object(),r);},"no cells");
            Reject([&]{Json r;FromGrid("--- storey 1 ---\n#.#\n--- storey 1 ---\n#.#\n",Json::object(),r);},"twice");
        }
        // Corridors run along their long side; storeys below ground.
        {
            Json report;
            const auto spec=FromGrid("--- storey -1 ---\n##########\n#........#\n##########\n",Json::object(),report);
            const auto& piece=spec.at("geometry").at("pieces")[0];
            Check(piece.at("kind")=="Corridor" && piece.at("yaw")==90 && piece.at("size")==Json::array({128,1024,320}),"corridor along X");
            Check(piece.at("storey")=="StoreyB1" && spec.at("storeys")[0].at("z")==-384,"basement storey");
            const auto tall=FromGrid("###\n#.#\n#.#\n#.#\n###\n",Json::object(),report);
            Check(tall.at("geometry").at("pieces")[0].contains("yaw")==false,"corridor along Y keeps yaw 0");
            const auto room=FromGrid("#####\n#...#\n#...#\n#...#\n#####\n",{{"corridorMax",0},{"cell",100},{"roomHeight",400}},report);
            Check(room.at("geometry").at("pieces")[0].at("size")==Json::array({300,300,400}),"cell size and room height");
            const auto placed=FromGrid("###\n#.#\n###\n",{{"origin",{1000,2000}}},report);
            Check(placed.at("geometry").at("pieces")[0].at("at")==Json::array({1192,1808}),"origin is the north-west corner");
        }
        // Doors: thick walls, doors that lead nowhere, door movers, no joins.
        {
            Json report;
            const auto thick=FromGrid("#####\n#...#\n##D##\n##D##\n#...#\n#####\n",{{"doorMesh","StaticMesh'Doors.Metal.Slide'"},{"portals",true}},report);
            const auto& d=thick.at("geometry").at("doorways")[0];
            Check(d.at("host")=="Corridor1" && d.at("side")=="south" && d.at("depth")==256 && d.at("portal")==true,"two-cell-deep door");
            Check(thick.at("mechanisms").at("doors")[0].at("at")==Json{{"doorway","Door1"}},"door mover in the doorway");
            Compiles(thick,"thick door");
            const auto loose=FromGrid("#####\n#...#\n#.D.#\n#...#\n#####\n",Json::object(),report);
            Check(Warned(report,"read as floor"),"door in the open read as floor");
            Check(!loose.at("geometry").contains("doorways") || loose.at("geometry").at("doorways").size()==0,"no doorway made");
            const auto apart=FromGrid("######\n#..#.#\n#..#.#\n######\n",{{"autoDoorways",false}},report);
            Check(!apart.at("geometry").contains("doorways"),"autoDoorways false");
            const auto joined=FromGrid("#####\n#...#\n#.###\n#.###\n#####\n",Json::object(),report);
            Check(joined.at("geometry").at("doorways").size()==1 && joined.at("geometry").at("doorways")[0].contains("between"),"touching rectangles joined");
            Compiles(joined,"joined rectangles");
        }
        // A flag without drop zones is reported.
        {
            Json report;
            FromGrid("#####\n#.F.#\n#####\n",Json::object(),report);
            Check(Warned(report,"no drop zone"),"flag needs drop zones");
        }

        // Maximal rectangles.
        {
            auto mask=[](std::vector<std::string> rows){std::vector<std::vector<char>> m;for(auto& r:rows){m.emplace_back();for(char c:r)m.back().push_back(c=='x');}return m;};
            Check(Grid::Decompose(mask({"xxx","xxx"})).size()==1,"one rectangle");
            const auto l=Grid::Decompose(mask({"xxxx","x...","x..."}));
            Check(l.size()==2 && l[0].Area()==4 && l[1].Area()==2,"L shape");
            Check(Grid::Decompose(mask({"x.x",".x.","x.x"})).size()==5,"single cells");
            int total=0;for(const auto& r:Grid::Decompose(mask({"xx.xx","xxxxx","xx.xx","..x.."})))total+=r.Area();
            Check(total==14,"every cell covered once");
        }
        // A very large plan is cut by rows, quickly, and says so.
        {
            std::string big;
            for(int r=0;r<300;++r)big+=std::string(300,'.')+"\n";
            Json report;
            const auto spec=FromGrid(big,{{"cell",16},{"autoDoorways",false}},report);
            Check(spec.at("geometry").at("pieces").size()==1 && Warned(report,"cut into rows"),"scanline fallback");
        }
        // Polygons: cleaning, convexity, convex pieces.
        {
            using P=Grid::P2;
            const std::vector<P> square={{0,0},{0,100},{100,100},{100,0}};
            Check(Grid::SignedArea(Grid::Clean(square))>0,"clean makes it counter-clockwise");
            Check(Grid::Clean({{0,0},{50,0},{100,0},{100,100},{0,100},{0,100}}).size()==4,"collinear and repeated points dropped");
            Check(Grid::ConvexParts(square).size()==1,"convex stays whole");
            const std::vector<P> ell={{0,0},{300,0},{300,100},{100,100},{100,300},{0,300}};
            const auto parts=Grid::ConvexParts(ell);
            double area=0;for(const auto& p:parts){Check(Grid::Convex(p),"piece is convex");area+=Grid::SignedArea(p);}
            Check(parts.size()==2 && std::abs(area-50000)<1e-6,"L split in two convex pieces");
            const std::vector<P> star={{0,100},{30,30},{100,0},{30,-30},{0,-100},{-30,-30},{-100,0},{-30,30}};
            area=0;for(const auto& p:Grid::ConvexParts(star)){Check(Grid::Convex(p),"star piece convex");area+=Grid::SignedArea(p);}
            Check(std::abs(area-std::abs(Grid::SignedArea(star)))<1e-6,"star area kept");
            Reject([&]{Grid::ConvexParts({{0,0},{100,100},{100,0},{0,100},{50,-50}});},"crosses itself");
            Check(Grid::AxisRectangle(square).has_value() && !Grid::AxisRectangle(ell).has_value(),"axis rectangles");
            Check(Grid::Inside(ell,50,250) && !Grid::Inside(ell,200,200),"point in polygon");
        }
        // Plans built directly, as the SVG and GeoJSON importers do.
        {
            Grid::Options o;
            Grid::Plan plan;
            plan.storeys.push_back({"Ground",0,0,""});
            Grid::Space a;a.id="West";a.kind="Room";a.storey="Ground";a.x0=-600;a.x1=-100;a.y0=-200;a.y1=200;
            Grid::Space b=a;b.id="East";b.x0=100;b.x1=600;
            plan.spaces={a,b};
            // A door drawn as a line across the 200-unit wall gap.
            Grid::Door door;door.id="Gate";door.storey="Ground";door.x=0;door.y=0;door.width=120;door.alongX=false;
            plan.doors.push_back(door);
            // A 20-sided pillar, which needs its caps split, and a wedge room.
            Grid::Prism pillar;pillar.id="Pillar";pillar.csg="CSG_Add";pillar.storey="Ground";pillar.height=320;
            for(int i=0;i<20;++i)pillar.points.push_back({-350+60*std::cos(i*Grid::kPi/10),60*std::sin(i*Grid::kPi/10)});
            Grid::Prism wedge;wedge.id="Wedge";wedge.storey="Ground";wedge.points={{0,400},{400,400},{0,800}};wedge.height=320;
            plan.prisms={pillar,wedge};
            Grid::Marker spy;spy.kind="spy";spy.storey="Ground";spy.x=-500;spy.y=0;
            Grid::Marker merc;merc.kind="merc";merc.storey="Ground";merc.x=100;merc.y=600;
            plan.markers={spy,merc};
            Grid::Segment ledge;ledge.kind="ledge";ledge.storey="Ground";ledge.x0=200;ledge.y0=-100;ledge.x1=500;ledge.y1=-100;
            Grid::Segment beam;beam.kind="laser";beam.storey="Ground";beam.x0=-550;beam.y0=-150;beam.x1=-550;beam.y1=150;
            plan.segments={ledge,beam};
            for(const auto& s:plan.spaces)plan.ids.Reserve(s.id);
            plan.format="test";
            Json report=Json::object();
            const auto spec=Grid::ToSpec(plan,o,Json::object(),report);
            const auto& gate=*ById(spec.at("geometry").at("doorways"),"Gate");
            Check(gate.at("host")=="West" && gate.at("side")=="east" && gate.at("depth")==200 && gate.at("size")[0]==120,"door line finds its wall");
            const auto& brushes=spec.at("geometry").at("brushes");
            Check(brushes[0].at("id")=="Wedge" && brushes[0].at("csg")=="CSG_Subtract" && brushes[1].at("csg")=="CSG_Add","carved prisms before solid ones");
            // Every polygon faces out of its brush: (v1-v0)x(v2-v0) points away from the centre.
            for(const auto& brush:brushes)
            {
                double c[3]={0,0,0};size_t n=0;
                for(const auto& p:brush.at("polygons"))for(const auto& v:p.at("vertices")){for(int k=0;k<3;++k)c[k]+=v[k].get<double>();++n;}
                for(double& x:c)x/=static_cast<double>(n);
                size_t caps=0;
                for(const auto& p:brush.at("polygons"))
                {
                    const auto& v=p.at("vertices");
                    Check(v.size()>=3 && v.size()<=16,"3 to 16 vertices");
                    double e1[3],e2[3],m[3]={0,0,0};
                    for(int k=0;k<3;++k){e1[k]=v[1][k].get<double>()-v[0][k].get<double>();e2[k]=v[2][k].get<double>()-v[0][k].get<double>();}
                    const double nrm[3]={e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]};
                    for(const auto& q:v)for(int k=0;k<3;++k)m[k]+=q[k].get<double>()/static_cast<double>(v.size());
                    Check(nrm[0]*(m[0]-c[0])+nrm[1]*(m[1]-c[1])+nrm[2]*(m[2]-c[2])>0,"polygon winds outward");
                    caps+=std::abs(nrm[2])>1e-9;
                }
                if(brush.at("id")=="Pillar")Check(caps==4,"20-gon caps split in two each");
            }
            Check(spec.at("ges")[0].at("points")[0]==Json::array({200,-100,208}),"ledge at its default height");
            const auto& laser=spec.at("security").at("devices")[0];
            Check(laser.at("facing")==90 && laser.at("length")==300 && laser.at("alarm")=="Alarm1","laser line");
            Check(spec.at("spawns").at("merc")[0].at("at").is_array(),"marker outside rooms at an absolute height");
            Check(spec.at("spawns").at("merc")[0].at("at")[2]==48,"usual start height");
            Check(Warned(report,"outside every rectangular room"),"outside marker reported");
            Compiles(spec,"built plan");
        }
        std::cout<<"GridPlanModelTests passed\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"GridPlanModelTests failed: "<<e.what()<<"\n";return 1;}
}
