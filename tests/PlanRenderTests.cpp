#include "../Reloaded.Editor.Mcp/PlanRender.h"
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
std::string Read(const std::string& relative)
{
    for(const auto& dir:{std::filesystem::path("tests"),std::filesystem::path("."),std::filesystem::path(__FILE__).parent_path()})
    {
        std::ifstream in(dir/relative,std::ios::binary);
        if(in){std::ostringstream s;s<<in.rdbuf();return s.str();}
    }
    return {};
}
bool Near(const uint8_t* p,std::array<int,3> rgb,int tolerance=6){for(int i=0;i<3;++i)if(std::abs(p[i]-rgb[static_cast<size_t>(i)])>tolerance)return false;return true;}
// World point to pixel, as the renderer lays out a scene with these bounds.
struct Layout
{
    double minX,maxY,scale;int margin;
    std::pair<int,int> At(double x,double y)const{return {static_cast<int>(margin+(x-minX)*scale),static_cast<int>(margin+(maxY-y)*scale)};}
};
Layout LayoutFor(double x0,double y0,double x1,double y1,int maxEdge)
{
    // The renderer pads by max(64, 3%) on each side.
    const double padX=std::max(64.0,(x1-x0)*0.03),padY=std::max(64.0,(y1-y0)*0.03);
    x0-=padX;x1+=padX;y0-=padY;y1+=padY;
    const int margin=std::max(8,maxEdge/64);
    return {x0,y1,(maxEdge-2.0*margin)/std::max(x1-x0,y1-y0),margin};
}
int main()
{
    try
    {
        const Json quiet={{"grid",false},{"labels",false},{"legend",false}};
        // One room with a spy start and a camera: colours where they belong.
        Json scene={{"bounds",{{"min",{0,0,0}},{"max",{1000,500,320}}}},
            {"pieces",{{{"id","Hall"},{"kind","Room"},{"storey","Ground"},{"box",{{"min",{0,0,0}},{"max",{1000,500,320}}}},{"yaw",0}}}},
            {"actors",{{{"id","Spy1"},{"cls","Engine.PlayerStart"},{"kind","spawn-spy"},{"at",{200,250,48}},{"yaw",0}},
                       {{"id","Cam1"},{"cls","SBase.SCamera"},{"kind","camera"},{"at",{900,250,200}},{"yaw",180}}}}};
        auto image=Render::PlanImage(scene,quiet);
        Check(std::max(image.width,image.height)==1024,"the longer side is maxEdge");
        const auto layout=LayoutFor(0,0,1000,500,1024);
        Check(std::abs(image.height-static_cast<int>(std::ceil((500+128)*layout.scale))-2*layout.margin)<=1,"aspect kept");
        auto [rx,ry]=layout.At(100,60);
        Check(Near(image.At(rx,ry),{236,230,214}),"room floor colour");
        auto [ox,oy]=layout.At(-40,540);
        Check(Near(image.At(ox,oy),{44,48,54}),"solid outside the room");
        auto [sx,sy]=layout.At(200,250);
        Check(Near(image.At(sx,sy),{52,152,219},2),"spy start in team blue");
        // The camera's view cone tints the floor in front of it (west).
        auto [cx,cy]=layout.At(700,250);
        Check(!Near(image.At(cx,cy),{236,230,214},2) && image.At(cx,cy)[0]>image.At(cx,cy)[2],"camera cone drawn");
        auto [bx,by]=layout.At(1100,250);
        Check(Near(image.At(bx,by),{44,48,54}),"no cone past the bounds' room");
        // The PNG is valid and decodes to the same pixels.
        const auto png=Render::PlanPng(scene,quiet);
        Check(Image::IsPng(png) && png.size()<60000,"a compact PNG");
        const auto decoded=Image::DecodePng(png);
        Check(decoded.width==image.width && decoded.pixels==image.pixels,"PNG round trip");
        // maxEdge.
        Check(std::max(Render::PlanImage(scene,{{"maxEdge",256}}).width,Render::PlanImage(scene,{{"maxEdge",256}}).height)==256,"maxEdge 256");
        Check(Render::PlanImage(scene,{{"maxEdge",10}}).width==64,"maxEdge clamps to 64");

        // Storeys: the upper storey alone leaves the lower room as an outline.
        Json stacked=scene;
        stacked["pieces"].push_back({{"id","Loft"},{"kind","Room"},{"storey","Upper"},{"box",{{"min",{500,0,384}},{"max",{1000,500,704}}}}});
        stacked["actors"].push_back({{"id","Merc1"},{"kind","spawn_merc"},{"at",{800,100,432}}});
        auto upper=Render::PlanImage(stacked,{{"storey","Upper"},{"grid",false},{"labels",false},{"legend",false}});
        auto [lx,ly]=layout.At(250,100);
        Check(Near(upper.At(lx,ly),{44,48,54}),"lower storey not filled");
        auto [ux,uy]=layout.At(700,400);
        Check(Near(upper.At(ux,uy),{236,230,214}),"upper storey filled");
        Check(Near(upper.At(sx,sy),{44,48,54}),"lower storey actors hidden");
        auto [mx,my]=layout.At(800,100);
        Check(Near(upper.At(mx,my),{231,76,60},2),"merc start on the upper storey ('_' kinds accepted)");
        Check(Render::PlanImage(stacked,{{"storey",1},{"grid",false}}).pixels==Render::PlanImage(stacked,{{"storey","upper"},{"grid",false}}).pixels,"storey by index or id");
        Reject([&]{Render::PlanImage(stacked,{{"storey","Roof"}});},"storeys: ground, upper");
        Reject([&]{Render::PlanImage(stacked,{{"storey",5}});},"2 storeys");

        // Nothing to draw still makes an image.
        const auto blank=Render::PlanImage(Json::object());
        Check(blank.width>=64 && blank.height>=64,"empty scene");
        Check(Image::IsPng(Render::PlanPng(Json::array())),"not even an object");

        // The compiled starter level (the spec agent's golden render scene).
        const auto golden=Read("mcp/golden/versus-starter.render.json");
        if(!golden.empty())
        {
            const auto starter=Json::parse(golden);
            for(int edge:{512,1024,2048})
            {
                const auto plan=Render::PlanImage(starter,{{"maxEdge",edge},{"title","Versus starter"}});
                Check(std::max(plan.width,plan.height)==edge,"starter at maxEdge");
                const auto bytes=Image::EncodePng(plan);
                Check(Image::PngSize(bytes)==std::make_pair(plan.width,plan.height),"starter PNG size");
            }
            const auto lay=LayoutFor(-2080,-512,2080,912,1024);
            const auto plan=Render::PlanImage(starter,quiet);
            // A spy start of the starter: team blue under its icon.
            for(const auto& actor:starter.at("actors"))
                if(actor.at("kind")=="spawn-spy")
                {
                    auto [px,py]=lay.At(actor.at("at")[0].get<double>(),actor.at("at")[1].get<double>());
                    Check(Near(plan.At(px,py),{52,152,219},2),"starter spy start drawn");
                    break;
                }
        }

        // The live map: design.scene items and actors.query.
        auto edges=[](double x0,double y0,double z0,double x1,double y1,double z1)
        {
            Json e=Json::array();
            const std::vector<std::array<double,3>> c={{x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}};
            for(int i=0;i<4;++i){e.push_back({c[static_cast<size_t>(i)],c[static_cast<size_t>((i+1)%4)]});e.push_back({c[static_cast<size_t>(i+4)],c[static_cast<size_t>((i+1)%4+4)]});e.push_back({c[static_cast<size_t>(i)],c[static_cast<size_t>(i+4)]});}
            return e;
        };
        const Json design=Json::array({
            {{"path","MyLevel.Brush1"},{"class","Engine.Brush"},{"csg",2},{"position",{0,0,0}},{"rotation",{0,0,0}},{"edges",edges(-1000,-1000,0,1000,1000,400)}},
            {{"path","MyLevel.Brush2"},{"class","Engine.Brush"},{"csg",1},{"position",{0,0,0}},{"rotation",{0,0,0}},{"edges",edges(-100,-100,0,100,100,400)}},
            {{"path","MyLevel.PlayerStart3"},{"class","Engine.PlayerStart"},{"position",{-600,-600,48}},{"rotation",{0,16384,0}}},
            {{"path","MyLevel.SLaserTrigger0"},{"class","SBase.SLaserTrigger"},{"position",{500,0,48}},{"rotation",{0,0,0}}},
            {{"path","MyLevel.SAlarm0"},{"class","SBase.SAlarm"},{"position",{600,600,48}},{"rotation",{0,0,0}}},
            {{"path","MyLevel.Light4"},{"class","Engine.Light"},{"position",{-500,500,300}},{"rotation",{0,0,0}},{"light",{{"radius",400},{"brightness",64}}}},
            {{"path","MyLevel.ZoneInfo0"},{"class","Engine.ZoneInfo"},{"position",{0,700,100}},{"rotation",{0,0,0}}}});
        const Json query={{"actors",{
            {{"path","MyLevel.PlayerStart3"},{"class","Engine.PlayerStart"},{"tag","PlayerStart"},{"event","None"},{"position",{-600,-600,48}},{"values",{{"TeamNumber","1"}}}},
            {{"path","MyLevel.SLaserTrigger0"},{"class","SBase.SLaserTrigger"},{"tag","Laser1"},{"event","Alarm1"},{"position",{500,0,48}}},
            {{"path","MyLevel.SAlarm0"},{"class","SBase.SAlarm"},{"tag","Alarm1"},{"event","None"},{"position",{600,600,48}}},
            {{"path","MyLevel.ZoneInfo0"},{"class","Engine.ZoneInfo"},{"tag","ZoneInfo"},{"event","None"},{"position",{0,700,100}},{"values",{{"LocationName","\"Vault\""}}}},
            {{"path","MyLevel.SFlag0"},{"class","SBase.SFlag"},{"tag","Flag"},{"event","None"},{"position",{-200,300,64}},{"rotation",{0,0,0}}}}},{"total",5}};
        const auto live=Render::SceneFromLive(design,query);
        Check(live.at("pieces").size()==2 && live.at("pieces")[0].at("kind")=="subtract" && live.at("pieces")[1].at("kind")=="add","brushes become CSG pieces");
        Check(live.at("pieces")[0].at("outline").size()==4,"brush footprint");
        Check(live.at("actors").size()==6,"actors from both sources");
        auto actorKind=[&](const std::string& path){for(const auto& a:live.at("actors"))if(a.at("id")==path)return a.at("kind").get<std::string>();return std::string();};
        Check(actorKind("MyLevel.PlayerStart3")=="spawn-merc","TeamNumber 1 is a merc start");
        Check(actorKind("MyLevel.SFlag0")=="flag" && actorKind("MyLevel.SLaserTrigger0")=="laser" && actorKind("MyLevel.Light4")=="light","kinds from classes");
        Check(live.at("lines").size()==1 && live.at("lines")[0].at("kind")=="alarm","Event to Tag wiring");
        Check(live.at("labels").size()==1 && live.at("labels")[0].at("text")=="Vault","zone label from LocationName");
        for(const auto& a:live.at("actors"))if(a.at("id")=="MyLevel.PlayerStart3")Check(std::abs(a.at("yaw").get<double>()-90)<1e-9,"yaw in degrees");
        const auto liveImage=Render::PlanImage(live,quiet);
        const auto liveLayout=LayoutFor(-1000,-1000,1000,1000,1024);
        auto [ax,ay]=liveLayout.At(0,-50);
        Check(Near(liveImage.At(ax,ay),{82,86,94}),"additive brush drawn over the carved hull");
        auto [fx,fy]=liveLayout.At(300,-700);
        Check(Near(liveImage.At(fx,fy),{236,230,214}),"carved space is floor");
        Check(Render::SceneFromLive(Json::object()).at("actors").empty(),"empty live scene");

        // A spec polygon brush as a render piece.
        const Json brush={{"id","Wedge"},{"csg","CSG_Subtract"},{"at",{100,0,0}},{"polygons",{
            {{"vertices",{{0,0,0},{200,0,0},{0,200,0}}}},{{"vertices",{{0,0,100},{0,200,100},{200,0,100}}}}}}};
        const auto piece=Render::BrushPiece(brush);
        Check(piece.at("kind")=="subtract" && piece.at("outline").size()==3 && piece.at("box").at("min")[0]==100,"brush piece");
        // A spec's brushes join a compiled scene, in the spec's units.
        {
            Json compiled={{"bounds",{{"min",{0,0,0}},{"max",{10,10,10}}}},{"pieces",Json::array()}};
            const Json spec={{"units",{{"unit","m"}}},{"geometry",{{"brushes",{brush}}}}};
            Render::AddBrushes(compiled,spec);
            Check(compiled.at("pieces").size()==1 && !compiled.contains("bounds"),"brush added, bounds dropped");
            Check(compiled.at("pieces")[0].at("box").at("min")[0]==10000 && compiled.at("pieces")[0].at("box").at("max")[1]==20000,"metres scaled to units");
            Check(Image::IsPng(Render::PlanPng(compiled)),"renders");
        }
        std::cout<<"PlanRenderTests passed\n";
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"PlanRenderTests failed: "<<e.what()<<"\n";return 1;}
}
