#include "../Reloaded.Editor.Mcp/GeometricEventModel.h"
#include <iostream>
#include <set>
#include <source_location>
#include <tuple>
using namespace Mcp;
using Workflow::Json;
using Workflow::Vector;
void Check(bool value,const char* message,std::source_location where=std::source_location::current())
{
    if(!value)throw std::runtime_error(std::string(message)+" (line "+std::to_string(where.line())+")");
}
template<class F> void Reject(F f,std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception&){caught=true;}
    if(!caught)throw std::runtime_error("invalid input accepted at line "+std::to_string(where.line()));
}
// The rules a measurement found.
std::set<std::string> Rules(const std::vector<Ge::Finding>& findings)
{
    std::set<std::string> out;
    for(const auto& f:findings)out.insert(f.rule);
    return out;
}
const Ge::Finding* Find(const std::vector<Ge::Finding>& findings,const std::string& rule)
{
    for(const auto& f:findings)if(f.rule==rule)return &f;
    return nullptr;
}
Ge::Element Make(const char* kind,std::vector<Vector> points)
{
    Ge::Element e;e.kind=&Ge::Require(kind);e.points=std::move(points);return e;
}
// A flat floor at z=0 everywhere.
std::optional<double> Flat(const Vector&){return 0.0;}

int main()
{
    try
    {
        // The table matches the Map Design element tool (ElementPanel.inl).
        const auto& kinds=Ge::Kinds();
        Check(kinds.size()==7,"seven kinds");
        const std::vector<std::tuple<std::string,std::string,int,std::string>> expected={
            {"Zip line","ZL",2,"zipline"},{"Hand over hand","HOH",2,"hoh"},{"Pipe","PIPE",3,"pipe"},{"Ladder","LADDER",3,"ladder"},
            {"Climbable fence","FENCE",2,"fence"},{"Pole","POLE",2,"pole"},{"Ledge grab","LG",2,"ledge"}};
        for(size_t i=0;i<kinds.size();++i)
        {
            Check(kinds[i].name==std::get<0>(expected[i]) && kinds[i].command==std::get<1>(expected[i]) && kinds[i].points==std::get<2>(expected[i]) && kinds[i].spec==std::get<3>(expected[i]),"kind table");
            Check((kinds[i].third!=nullptr)==(kinds[i].points==3),"third point named for pipes and ladders");
        }
        // Found by panel name, command, spec kind and common spellings.
        Check(Ge::Find("Zip line")->command==std::string("ZL") && Ge::Find("zipline")->command==std::string("ZL") && Ge::Find("ZL")->command==std::string("ZL") && Ge::Find("zip-line")->command==std::string("ZL"),"zip line names");
        Check(Ge::Find("hand-over-hand")->command==std::string("HOH") && Ge::Find("HOH")->points==2,"hand over hand names");
        Check(Ge::Find("ledge")->command==std::string("LG") && Ge::Find("Ledge grab")->command==std::string("LG") && Ge::Find("lg")->command==std::string("LG"),"ledge names");
        Check(Ge::Find("LADDER")->points==3 && Ge::Find("pipe")->points==3 && Ge::Find("Climbable fence")->command==std::string("FENCE") && Ge::Find("pole")->command==std::string("POLE"),"other names");
        Check(Ge::Find("rope")==nullptr && Ge::Find("")==nullptr,"unknown kinds");
        Reject([]{Ge::Require("trampoline");});

        // Pipes and ladders: lower end first, side point at that height.
        auto ordered=Ge::Ordered(Ge::Require("ladder"),{{0,0,300},{0,0,44},{40,0,170}});
        Check(ordered[0]==Vector{0,0,44} && ordered[1]==Vector{0,0,300} && ordered[2]==Vector{40,0,44},"ladder reordered");
        auto kept=Ge::Ordered(Ge::Require("pipe"),{{0,0,10},{0,0,200},{10,0,10}});
        Check(kept[0][2]==10 && kept[1][2]==200 && kept[2][2]==10,"pipe in order kept");
        auto zip=Ge::Ordered(Ge::Require("zipline"),{{0,0,300},{500,0,100}});
        Check(zip[0][2]==300 && zip[1][2]==100,"two-point kinds keep their order");
        Reject([]{Ge::Ordered(Ge::Require("ladder"),{{0,0,0},{0,0,256}});});
        Check(Ge::InOrder(Ge::Require("ladder"),{{0,0,0},{0,0,256},{30,0,0}}) && !Ge::InOrder(Ge::Require("ladder"),{{0,0,256},{0,0,0},{30,0,0}}) && !Ge::InOrder(Ge::Require("pipe"),{{0,0,0},{0,0,256},{30,0,128}}),"in order");

        // The console command, as ElementPanel writes it (three decimals).
        Check(Ge::AddCommand(Ge::Require("ZL"),{{1,2.5,300},{-400,0,96.125}})=="GE ADD ZL X1=1.000 Y1=2.500 Z1=300.000 X2=-400.000 Y2=0.000 Z2=96.125","add command");
        Reject([]{Ge::AddCommand(Ge::Require("LG"),{{0,0,0}});});

        // From spec entries, ge.add requests and ge.list elements.
        auto spec=Ge::FromJson({{"kind","ladder"},{"points",{{0,0,0},{0,0,256},{32,0,0}}},{"bars",10}});
        Check(spec.kind->command==std::string("LADDER") && spec.points.size()==3 && spec.bars==10,"spec ladder");
        auto listed=Ge::FromJson({{"kind","Zip line"},{"points",{{{"X","0"},{"Y","0"},{"Z","300"}},{{"x",500},{"y",0},{"z",100}}}},{"command","GE ADD ZL X1=0 Y1=0 Z1=300"}});
        Check(listed.kind->command==std::string("ZL") && listed.points[0][2]==300 && listed.points[1][0]==500,"ge.list element");
        auto command=Ge::FromJson({{"command","GE ADD LG X1=0 Y1=0 Z1=250 X2=128 Y2=0 Z2=250"},{"points",{{0,0,250},{128,0,250}}},{"surface",80}});
        Check(command.kind->command==std::string("LG") && command.surface==80,"command element");
        Check(Ge::FromJson({{"command","HOH"},{"points",{{0,0,0},{1,0,0}}}}).kind->points==2,"bare command");
        Reject([]{Ge::FromJson({{"kind","rope"},{"points",Json::array()}});});
        Reject([]{Ge::FromJson({{"kind","ledge"}});});
        Reject([]{Ge::FromJson({{"kind","ledge"},{"points",{{0,0},{1,1}}}});});
        Reject([]{Ge::FromJson({{"kind","ledge"},{"points",{{0,0,0},{1e9,0,0}}}});});
        Reject([]{Ge::FromJson(Json::array());});
        auto round=Ge::ToJson(spec);
        Check(round.at("kind")=="ladder" && round.at("command")=="LADDER" && round.at("bars")==10 && round.at("points").size()==3,"to json");

        // Where the floor is read: either side of a ledge or fence, under a bar or a zip line.
        auto ledgeProbes=Ge::Probes(Make("ledge",{{0,0,250},{128,0,250}}));
        Check(ledgeProbes.size()==2 && ledgeProbes[0][1]==Ge::kSideOffset && ledgeProbes[1][1]==-Ge::kSideOffset && ledgeProbes[0][0]==64 && ledgeProbes[0][2]==248,"ledge probes");
        Check(Ge::Probes(Make("hoh",{{0,0,240},{256,0,240}})).size()==3,"hand-over-hand probes");
        auto zipProbes=Ge::Probes(Make("zipline",{{0,0,300},{400,0,220}}));
        Check(zipProbes.size()==5 && zipProbes[0][2]==296 && zipProbes[4][2]==216 && zipProbes[2][0]==200,"zip line probes");
        Check(Ge::Probes(Make("ladder",{{0,0,0},{0,0,256},{32,0,0}})).empty() && Ge::Probes(Make("pole",{{0,0,0},{0,0,256}})).empty(),"no probes for climbs");
        std::vector<Vector> asked;
        Ge::Measure(Make("ledge",{{0,0,250},{128,0,250}}),[&](const Vector& at)->std::optional<double>{asked.push_back(at);return 0.0;});
        Check(asked.size()==2 && asked[0]==ledgeProbes[0],"the floor is asked at the probes");

        // Wrong point counts.
        Check(Rules(Ge::Measure(Make("ladder",{{0,0,0},{0,0,256}})))==std::set<std::string>{"ge.points"},"ladder needs 3 points");
        Check(Rules(Ge::Measure(Ge::Element{}))==std::set<std::string>{"ge.points"},"kind needed");

        // Ledge grab heights: 115 obstacle, 208-320 standing, 448 running, 512 co-op.
        auto ledge=[](double z,double length=128){return Make("ledge",{{0,0,z},{length,0,z}});};
        Check(Rules(Ge::Measure(ledge(250),Flat)).empty(),"a standing grab passes");
        Check(Rules(Ge::Measure(ledge(150),Flat)).empty(),"a climbable obstacle passes");
        Check(Rules(Ge::Measure(ledge(100),Flat))==std::set<std::string>{"ge.obstacle"},"below 115 is jumped");
        Check(Rules(Ge::Measure(ledge(400),Flat))==std::set<std::string>{"ge.ledge-reach"} && Find(Ge::Measure(ledge(400),Flat),"ge.ledge-reach")->severity=="info","running jump only");
        Check(Rules(Ge::Measure(ledge(448),Flat))==std::set<std::string>{"ge.ledge-reach"},"448 is still reachable");
        Check(Rules(Ge::Measure(ledge(470),Flat))==std::set<std::string>{"ge.ledge-height"},"between running and co-op");
        auto coop=Ge::Measure(ledge(512),Flat);
        Check(Rules(coop)==std::set<std::string>{"ge.ledge-reach"} && Find(coop,"ge.ledge-reach")->message.find("144")!=std::string::npos,"co-op height");
        Check(Rules(Ge::Measure(ledge(600),Flat))==std::set<std::string>{"ge.ledge-height"},"too high");
        Check(Find(Ge::Measure(ledge(470),Flat),"ge.ledge-height")->measured==470,"measured height");
        // Measured from the lower side: a probe inside the geometry finds its top.
        auto oneSided=Ge::Measure(ledge(250),[](const Vector& at)->std::optional<double>{return at[1]>0?248.0:0.0;});
        Check(Rules(oneSided).empty(),"the open side's floor counts");
        auto raised=Ge::Measure(ledge(250),[](const Vector& at)->std::optional<double>{return at[1]>0?248.0:150.0;});
        Check(Rules(raised)==std::set<std::string>{"ge.obstacle"},"a raised floor in front makes it an obstacle");
        Check(Rules(Ge::Measure(ledge(470))).empty(),"no floor, no height checks");
        Check(Rules(Ge::Measure(ledge(250,20),Flat))==std::set<std::string>{"ge.ledge-length"},"short ledge");
        Check(Rules(Ge::Measure(ledge(250,32),Flat)).empty(),"32 units is long enough");
        auto shallow=ledge(250);shallow.surface=40;
        Check(Rules(Ge::Measure(shallow,Flat))==std::set<std::string>{"ge.pull-up"},"pull-up surface");
        shallow.surface=64;
        Check(Rules(Ge::Measure(shallow,Flat)).empty(),"64 units to the wall is enough");

        // Fence 88-128, 96 recommended.
        auto fence=[](double z){return Make("fence",{{0,0,z},{256,0,z}});};
        Check(Rules(Ge::Measure(fence(96),Flat)).empty(),"96 fence");
        Check(Rules(Ge::Measure(fence(92),Flat)).empty(),"close to 96");
        Check(Rules(Ge::Measure(fence(120),Flat))==std::set<std::string>{"ge.fence-recommended"},"works but not 96");
        Check(Rules(Ge::Measure(fence(60),Flat))==std::set<std::string>{"ge.fence-height"} && Rules(Ge::Measure(fence(140),Flat))==std::set<std::string>{"ge.fence-height"},"fence out of range");
        Check(Rules(Ge::Measure(fence(88),Flat)).empty() && Rules(Ge::Measure(fence(128),Flat))==std::set<std::string>{"ge.fence-recommended"},"range ends inclusive");
        Check(Rules(Ge::Measure(fence(87),Flat))==std::set<std::string>{"ge.fence-height"} && Rules(Ge::Measure(fence(129),Flat))==std::set<std::string>{"ge.fence-height"},"just outside the range");

        // Hand-over-hand clearance 216.
        Check(Rules(Ge::Measure(Make("hoh",{{0,0,240},{256,0,240}}),Flat)).empty(),"hoh clear");
        Check(Rules(Ge::Measure(Make("hoh",{{0,0,216},{256,0,216}}),Flat)).empty(),"216 exactly");
        auto low=Ge::Measure(Make("hoh",{{0,0,200},{256,0,200}}),Flat);
        Check(Rules(low)==std::set<std::string>{"ge.hoh-clearance"} && Find(low,"ge.hoh-clearance")->measured==200,"hoh too low");
        // The lowest clearance along the bar counts.
        auto step=Ge::Measure(Make("hoh",{{0,0,300},{256,0,300}}),[](const Vector& at)->std::optional<double>{return at[0]>200?120.0:0.0;});
        Check(Rules(step)==std::set<std::string>{"ge.hoh-clearance"},"a step under one end");

        // Ladders: an even number of bars, at least 8 (32 units a bar unless told).
        auto ladder=[](double bottom,double top){return Make("ladder",{{0,0,bottom},{0,0,top},{32,0,bottom}});};
        Check(Rules(Ge::Measure(ladder(0,256))).empty() && Rules(Ge::Measure(ladder(0,320))).empty(),"8 and 10 bars");
        auto six=Ge::Measure(ladder(0,192));
        Check(Rules(six)==std::set<std::string>{"ge.ladder-bars"} && Find(six,"ge.ladder-bars")->suggestion.at("bars")==8 && Find(six,"ge.ladder-bars")->suggestion.at("points")[1][2]==256,"6 bars");
        auto nine=Ge::Measure(ladder(16,304));
        Check(Rules(nine)==std::set<std::string>{"ge.ladder-bars"} && Find(nine,"ge.ladder-bars")->suggestion.at("bars")==10 && Find(nine,"ge.ladder-bars")->suggestion.at("points")[1][2]==336,"9 bars rounds up to 10");
        auto told=ladder(0,192);told.bars=10;
        Check(Rules(Ge::Measure(told)).empty(),"bars given");
        told.bars=7;
        Check(Rules(Ge::Measure(told))==std::set<std::string>{"ge.ladder-bars"},"odd bars given");
        auto spaced=ladder(0,192);spaced.barSpacing=24;
        Check(Rules(Ge::Measure(spaced)).empty(),"24-unit bars: 8 in 192");

        // Zip lines: hang at most 384 up, 198 clearance (161 tucked), and a slope.
        auto line=[](double high,double lowEnd){return Make("zipline",{{0,0,high},{600,0,lowEnd}});};
        Check(Rules(Ge::Measure(line(300,220),Flat)).empty(),"zip line passes");
        Check(Rules(Ge::Measure(line(450,250),Flat))==std::set<std::string>{"ge.zipline-hang"},"too high to reach");
        Check(Rules(Ge::Measure(line(384,250),Flat)).empty(),"384 exactly");
        auto tucked=Ge::Measure(line(300,182),Flat);
        Check(Rules(tucked)==std::set<std::string>{"ge.zipline-clearance"} && Find(tucked,"ge.zipline-clearance")->message.find("tuck")!=std::string::npos,"tucked only");
        auto scraping=Ge::Measure(line(300,150),Flat);
        Check(Rules(scraping)==std::set<std::string>{"ge.zipline-clearance"} && Find(scraping,"ge.zipline-clearance")->message.find("161")!=std::string::npos,"too low even tucked");
        Check(Rules(Ge::Measure(line(300,300),Flat))==std::set<std::string>{"ge.zipline-slope"},"level zip line");
        // The upper end is where the spy grabs, whichever point it is.
        Check(Rules(Ge::Measure(Make("zipline",{{600,0,250},{0,0,450}}),Flat))==std::set<std::string>{"ge.zipline-hang"},"upper end second");

        // Pipes and ladders start at the lower end, side point at that height.
        auto reversed=Ge::Measure(Make("ladder",{{0,0,256},{0,0,0},{32,0,0}}));
        Check(Rules(reversed)==std::set<std::string>{"ge.point-order"} && Find(reversed,"ge.point-order")->suggestion.at("points")[0][2]==0,"ladder reversed");
        Check(Rules(Ge::Measure(Make("pipe",{{0,0,0},{0,0,300},{20,0,150}})))==std::set<std::string>{"ge.point-order"},"pipe side point mid-height");
        Check(Rules(Ge::Measure(Make("pipe",{{0,0,0},{0,0,300},{20,0,0}}))).empty(),"pipe in order");
        Check(Rules(Ge::Measure(Make("pole",{{0,0,0},{0,0,300}}),Flat)).empty(),"poles have no measurements");

        // Every finding's rule is one the rules catalogue knows by prefix.
        for(const auto& r:{"ge.points","ge.obstacle","ge.ledge-reach","ge.ledge-height","ge.ledge-length","ge.pull-up","ge.fence-height","ge.fence-recommended",
                           "ge.hoh-clearance","ge.ladder-bars","ge.zipline-hang","ge.zipline-clearance","ge.zipline-slope","ge.point-order"})
            Check(std::string(r).rfind("ge.",0)==0,"ge rule ids");
        std::cout<<"GeometricEventModel tests passed\n";
        return 0;
    }
    catch(const std::exception& e)
    {
        std::cerr<<e.what()<<"\n";
        return 1;
    }
}
