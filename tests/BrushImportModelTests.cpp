// Hammer and Quake brushes as SCCT polygon brushes. Every polygon is put
// through the editor's own polygon check (Authoring::BrushPolygons), so this
// suite links WorkflowModel.cpp and RecoveredPolygonImport.cpp.
#include "../Reloaded.Editor.Mcp/BrushImportModel.h"
#include "../Reloaded.Editor.Mcp/LevelSpecModel.h"
// The shared models put two statements on one line after an if, which GCC's
// -Wall reads as misleading indentation.
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
#include <source_location>
#include <sstream>
using namespace Mcp;
using Json=Workflow::Json;
using Workflow::Vector;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F> void Reject(F f,const std::string& expected={},std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception& e){caught=true;if(!expected.empty() && std::string(e.what()).find(expected)==std::string::npos)throw std::runtime_error("line "+std::to_string(where.line())+": wrong error: "+e.what());}
    if(!caught)throw std::runtime_error("invalid input accepted at line "+std::to_string(where.line()));
}
std::string Fixture(const std::string& name)
{
    for(const auto& dir:{std::filesystem::path("tests/mcp/fixtures"),std::filesystem::path("mcp/fixtures"),std::filesystem::path(__FILE__).parent_path()/"mcp/fixtures"})
    {
        std::ifstream in(dir/name,std::ios::binary);
        if(in){std::ostringstream s;s<<in.rdbuf();return s.str();}
    }
    throw std::runtime_error("missing fixture "+name);
}
bool Warned(const Json& report,const std::string& part){for(const auto& w:report.at("warnings"))if(w.get<std::string>().find(part)!=std::string::npos)return true;return false;}
// Each polygon of each brush: 3-16 vertices, accepted by the editor's
// importer, wound with its normal out of the brush.
void Sound(const Json& spec)
{
    Spec::Parse(spec);
    for(const auto& brush:spec.at("geometry").at("brushes"))
    {
        Workflow::Authoring::BrushPolygons(brush.at("polygons"));
        double c[3]={0,0,0};size_t n=0;
        for(const auto& p:brush.at("polygons"))for(const auto& v:p.at("vertices")){for(int k=0;k<3;++k)c[k]+=v[k].get<double>();++n;}
        for(double& x:c)x/=static_cast<double>(n);
        for(const auto& p:brush.at("polygons"))
        {
            const auto& v=p.at("vertices");
            Check(v.size()>=3 && v.size()<=16,"3 to 16 vertices");
            double e1[3],e2[3],m[3]={0,0,0};
            for(int k=0;k<3;++k){e1[k]=v[1][k].get<double>()-v[0][k].get<double>();e2[k]=v[2][k].get<double>()-v[0][k].get<double>();}
            const double nrm[3]={e1[1]*e2[2]-e1[2]*e2[1],e1[2]*e2[0]-e1[0]*e2[2],e1[0]*e2[1]-e1[1]*e2[0]};
            for(const auto& q:v)for(int k=0;k<3;++k)m[k]+=q[k].get<double>()/static_cast<double>(v.size());
            if(nrm[0]*(m[0]-c[0])+nrm[1]*(m[1]-c[1])+nrm[2]*(m[2]-c[2])<=0)throw std::runtime_error("a polygon of "+brush.at("id").get<std::string>()+" winds inward");
        }
    }
}
std::vector<Brushes::Plane> Box(Vector lo,Vector hi)
{
    std::vector<Brushes::Plane> planes;
    for(int k=0;k<3;++k)
    {
        Vector n{0,0,0};n[static_cast<size_t>(k)]=1;planes.push_back({n,hi[static_cast<size_t>(k)],"Side"});
        n[static_cast<size_t>(k)]=-1;planes.push_back({n,-lo[static_cast<size_t>(k)],"Side"});
    }
    return planes;
}
int main()
{
    try
    {
        // Planes from points, as Quake and Hammer write them.
        {
            const auto top=Brushes::PlaneFromPoints({-64,64,128},{64,64,128},{64,-64,128});
            Check(top && top->normal==Vector({0,0,1}) && top->dist==128,"Hammer top face points up");
            const auto west=Brushes::PlaneFromPoints({-64,-64,-16},{-64,-63,-16},{-64,-64,-15});
            Check(west && west->normal==Vector({-1,0,0}) && west->dist==64,"Quake west face points west");
            Check(!Brushes::PlaneFromPoints({0,0,0},{1,1,1},{2,2,2}),"collinear points make no plane");
        }
        // Clipping: a box gives six quads, a wedge five faces; repeats and
        // planes that bound nothing are dropped; disjoint planes give nothing.
        {
            auto faces=Brushes::Faces(Box({-64,-64,0},{64,64,128}));
            Check(faces.size()==6,"cube has six faces");
            for(const auto& f:faces)
            {
                Check(f.vertices.size()==4,"each face a quad");
                for(const auto& v:f.vertices)for(double c:v)Check(c==64 || c==-64 || c==0 || c==128,"exact corners");
            }
            auto planes=Box({0,0,0},{100,100,100});
            planes.push_back(planes[0]);
            planes.push_back({{1,0,0},500,"Far"});
            Check(Brushes::Faces(planes).size()==6,"repeated and redundant planes dropped");
            auto wedge=Box({0,0,0},{100,100,100});
            const double r=std::sqrt(0.5);
            wedge.push_back({{r,0,r},100*r,"Slope"});
            const auto cut=Brushes::Faces(wedge);
            Check(cut.size()==5,"wedge has five faces");
            auto empty=Box({0,0,0},{10,10,10});
            empty.push_back({{-1,0,0},-50,"Beyond"});
            Check(Brushes::Faces(empty).empty(),"an empty solid has no faces");
            // A 32-sided column: its caps split into fans of at most 16.
            std::vector<Brushes::Plane> column={{{0,0,1},256,"Top"},{{0,0,-1},0,"Bottom"}};
            for(int i=0;i<32;++i){const double a=i*2*3.14159265358979323846/32;column.push_back({{std::cos(a),std::sin(a),0},100,"Side"});}
            const auto round=Brushes::Faces(column);
            Check(round.size()==34,"column faces");
            const auto split=Brushes::Split16(round);
            size_t caps=0;for(const auto& f:split){Check(f.vertices.size()<=16,"split to 16");caps+=f.texture!="Side";}
            Check(split.size()==38 && caps==6,"each 32-gon cap became three fans");
        }
        // The Hammer cube, kept as authored (scale 1, no mirror).
        {
            Json report=Json::object();
            const auto spec=Brushes::ImportVmf(Fixture("cube.vmf"),{{"scale",1},{"mirrorY",false},{"textures",{{"brick/brickwall001a","Bunker.Walls.Brick"}}}},report);
            Sound(spec);
            const auto& brushes=spec.at("geometry").at("brushes");
            Check(brushes.size()==2,"hull and the cube");
            Check(brushes[0].at("id")=="Hull" && brushes[0].at("csg")=="CSG_Subtract" && brushes[0].at("polygons").size()==6,"subtractive hull first");
            Check(brushes[0].at("at")==Json::array({0,0,0}),"hull at the bounds' floor centre");
            const auto& cube=brushes[1];
            Check(cube.at("csg")=="CSG_Add" && cube.at("polygons").size()==6,"VMF cube becomes 6 polygons, added");
            Check(cube.at("at")==Json::array({0,0,64}),"brush at its centre");
            size_t textured=0;
            for(const auto& p:cube.at("polygons"))
            {
                Check(p.at("vertices").size()==4,"quads");
                if(p.contains("texture")){Check(p.at("texture")=="Bunker.Walls.Brick","mapped texture");++textured;}
                for(const auto& v:p.at("vertices"))Check(std::abs(v[0].get<double>())==64 && std::abs(v[1].get<double>())==64 && std::abs(v[2].get<double>())==64,"local corners");
            }
            Check(textured==4,"walls mapped, floor and ceiling unmapped");
            Check(Warned(report,"displacement"),"displacements reported");
            Check(Warned(report,"func_door"),"door brush left out");
            Check(Warned(report,"tool brushes"),"clip brush left out");
            Check(Warned(report,"DEV/DEV_MEASUREGENERIC01B"),"unmapped texture named");
            Check(Warned(report,"point entities"),"prop left out");
            const auto& spy=spec.at("spawns").at("spy")[0];
            Check(spy.at("at")==Json::array({-32,0,48}) && spy.at("yaw")==90,"terrorist start is a spy start, 48 above its feet");
            Check(spec.at("spawns").at("merc")[0].at("yaw")==270,"counter-terrorist start is a merc start");
            Check(spec.at("environment").at("lights").size()==1 && spec.at("defaults").at("lights")=="none","light imported");
            Check(report.at("counts").at("brushes")==1 && report.at("counts").at("sourceBrushes")==2,"counts");
        }
        // Default scale and mirroring: the level plays as it did in Source.
        {
            Json report=Json::object();
            const auto spec=Brushes::ImportVmf(Fixture("cube.vmf"),{{"name","cube"}},report);
            Sound(spec);
            Check(spec.at("map").at("name")=="CUBE","map name");
            const auto& cube=spec.at("geometry").at("brushes")[1];
            Check(cube.at("at")==Json::array({0,0,160}),"scaled 2.5");
            for(const auto& p:cube.at("polygons"))for(const auto& v:p.at("vertices"))Check(std::abs(v[0].get<double>())==160,"scaled corners");
            Check(spec.at("spawns").at("spy")[0].at("at")==Json::array({-80,0,48}) && spec.at("spawns").at("spy")[0].at("yaw")==270,"mirrored yaw");
            const auto centred=Brushes::ImportVmf(Fixture("cube.vmf"),{{"center",true},{"offset",{1000,0,0}},{"margin",64}},report);
            Check(centred.at("geometry").at("brushes")[1].at("at")==Json::array({1000,0,160}),"centred then offset");
            Check(centred.at("geometry").at("brushes")[0].at("at")==Json::array({1000,0,-64}),"hull margin");
        }
        // Quake: standard, Valve 220 and brushDef faces; water and patches left out.
        {
            Json report=Json::object();
            const auto spec=Brushes::ImportQuake(Fixture("quake-room.map"),{{"scale",1},{"mirrorY",false},{"material","Bunker.Walls.Concrete"}},report);
            Sound(spec);
            const auto& brushes=spec.at("geometry").at("brushes");
            Check(brushes.size()==4,"hull and three blocks");
            Check(brushes[1].at("at")==Json::array({0,0,0}) && brushes[2].at("at")==Json::array({256,0,0}) && brushes[3].at("at")==Json::array({0,256,0}),"block positions");
            Check(brushes[0].at("polygons")[0].at("texture")=="Bunker.Walls.Concrete","material on unmapped faces");
            Check(Warned(report,"liquid"),"water left out");
            Check(Warned(report,"curved patches"),"patch left out");
            Check(spec.at("spawns").at("spy").size()==1 && spec.at("spawns").at("merc").size()==1,"neutral starts alternate");
            Check(spec.at("spawns").at("spy")[0].at("at")==Json::array({0,0,64}),"Quake origin 24 above the feet");
        }
        // Doom 3 plane equations, given facing inward, are turned round.
        {
            std::string text="{\n\"classname\" \"worldspawn\"\n{\nbrushDef3\n{\n";
            for(const auto& [n,d]:std::vector<std::pair<std::string,std::string>>{{"1 0 0","64"},{"-1 0 0","64"},{"0 1 0","64"},{"0 -1 0","64"},{"0 0 1","0"},{"0 0 -1","128"}})
                text+="( "+n+" "+d+" ) ( ( 0.0078125 0 0 ) ( 0 0.0078125 0 ) ) \"textures/common/nodraw\" 0 0 0\n";
            text+="}\n}\n}\n";
            Json report=Json::object();
            const auto spec=Brushes::ImportQuake(text,{{"scale",1},{"mirrorY",false}},report);
            Sound(spec);
            Check(spec.at("geometry").at("brushes").size()==2 && spec.at("geometry").at("brushes")[1].at("polygons").size()==6,"brushDef3 cube");
        }
        Reject([&]{Json r;Brushes::ImportVmf("world\n{\n\"classname\" \"worldspawn\"\nsolid\n{\nside\n{\n\"plane\" \"(0 0 0) (1 0 0)\"\n}\n}\n}\n",Json::object(),r);},"three points");
        Reject([&]{Json r;Brushes::ImportVmf("world\n{\n\"classname\" \"worldspawn\"\n",Json::object(),r);},"ends inside");
        Reject([&]{Json r;Brushes::ImportVmf("versioninfo\n{\n}\n",Json::object(),r);},"not a Hammer VMF");
        Reject([&]{Json r;Brushes::ImportQuake("{\n\"classname\" \"worldspawn\"\n{\n( 0 0 ) ( 1 0 0 ) ( 0 1 0 ) TEX 0 0 0 1 1\n}\n}\n",Json::object(),r);},"x, y and z");
        Reject([&]{Json r;Brushes::ImportQuake("garbage",Json::object(),r);},"expected an entity");
        Reject([&]{Json r;Brushes::ImportVmf(Fixture("cube.vmf"),{{"scale",0}},r);},"scale");
        std::cout<<"BrushImportModelTests passed\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"BrushImportModelTests failed: "<<e.what()<<"\n";return 1;}
}
