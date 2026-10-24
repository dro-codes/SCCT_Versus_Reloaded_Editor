// Standalone: cl /std:c++20 /W4 /WX /EHsc tests\ScatterModelTests.cpp
#include "../Reloaded.Editor/ScatterModel.h"
#include <iostream>
#include <map>
#include <source_location>
using namespace Workflow;
using namespace Workflow::Scatter;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool Near(double a,double b,double tolerance=1e-6){return std::abs(a-b)<=tolerance;}
template<class F> void Reject(F f,std::source_location where=std::source_location::current())
{
    bool caught=false;
    try{f();}catch(const std::exception&){caught=true;}
    if(!caught)throw std::runtime_error("invalid input accepted at line "+std::to_string(where.line()));
}
// A level floor at z: every candidate lands with its box's bottom on it.
Drop FlatFloor(const std::vector<Item>& items,double z=0,int* calls=nullptr)
{
    return [&items,z,calls](const Candidate& c)
    {
        if(calls)++*calls;
        Landing landing;
        auto box=CandidateBox(c,items);
        const double lift=z-box.lo[2];
        landing.prop={c.item,{c.at[0],c.at[1],lift},c.rotation,c.scale,Place::Moved(box,{0,0,lift})};
        return landing;
    };
}
int main()
{
  try
  {
    // The generator is fixed: these numbers are what every build makes from seed 1.
    {
        Random a(1),b(1),c(2);
        const auto first=a.Next();
        Check(first==b.Next(),"same seed, same numbers");
        Check(first!=c.Next(),"another seed, other numbers");
        Check(Random(0).Next()==0xE220A8397B1DCDAFull,"SplitMix64 from 0");
        Random u(7);
        for(int i=0;i<1000;++i){const double x=u.Unit();Check(x>=0 && x<1,"unit in [0, 1)");}
    }

    // A circle: inside within the radius, outside beyond it.
    auto circle=CircleArea({100,200},50);
    Check(Inside(circle,{100,200}) && Inside(circle,{149,200}) && !Inside(circle,{140,240}) && !Inside(circle,{151,200}),"circle inside");
    Check(Near(Size(circle),Place::kPi*2500),"circle size");
    Reject([]{CircleArea({0,0},0);});
    Reject([]{CircleArea({0,0},1e9);});
    Reject([]{CircleArea({std::nan(""),0},10);});

    // A builder box seen from above: the top and bottom faces are its
    // footprint, the sides cover nothing and are left out.
    Polygon top{{-100,-50},{100,-50},{100,50},{-100,50}},bottom=top,side{{-100,-50},{100,-50},{100,-50},{-100,-50}};
    auto box=PolygonArea({top,side,bottom});
    Check(box.polygons.size()==2,"edge-on faces left out");
    Check(Inside(box,{0,0}) && Inside(box,{99,49}) && !Inside(box,{101,0}) && !Inside(box,{0,-51}),"box footprint");
    Check(Near(Size(box),200*100,1),"footprint size");
    Reject([&]{PolygonArea({side});});
    // An L-shaped face: the notch is outside.
    Polygon ell{{0,0},{200,0},{200,100},{100,100},{100,200},{0,200}};
    auto l=PolygonArea({ell});
    Check(Inside(l,{50,150}) && Inside(l,{150,50}) && !Inside(l,{150,150}),"concave footprint");
    Check(Near(Size(l),30000,300),"concave size");

    // Weights: picked in proportion; a weight of 0 never.
    std::vector<Item> items{{3,{{-8,-8,0},{8,8,16}}},{1,{{-16,-16,0},{16,16,32}}},{0,{{-4,-4,0},{4,4,4}}}};
    {
        Random r(42);std::map<size_t,int> picks;
        for(int i=0;i<20000;++i)++picks[Pick(items,r.Unit())];
        Check(picks[2]==0,"zero weight never picked");
        Check(std::abs(picks[0]/20000.0-0.75)<0.02,"weights pick in proportion");
        Check(Pick(items,0.999999)==1,"the top of the range picks the last weighted item");
    }

    // Validation.
    Settings s;s.count=10;
    Reject([&]{Validate(s,{});});
    Reject([&]{Validate(s,{{0,{{0,0,0},{1,1,1}}}});});
    {Settings t=s;t.count=0;Reject([&]{Validate(t,items);});}
    {Settings t=s;t.count=kMaxProps+1;Reject([&]{Validate(t,items);});}
    {Settings t=s;t.scaleMin=2;t.scaleMax=1;Reject([&]{Validate(t,items);});}
    {Settings t=s;t.spacing=-1;Reject([&]{Validate(t,items);});}
    {Settings t=s;t.byDensity=true;t.density=0;Reject([&]{Validate(t,items);});}

    // Density: props per 256 x 256 units of the area.
    {
        Settings d;d.byDensity=true;d.density=2;
        auto square=PolygonArea({{{0,0},{1024,0},{1024,1024},{0,1024}}});
        Check(Target(d,square)==32,"density over a 1024 square");
        d.density=0.001;Reject([&]{Target(d,square);});
        d.density=1000;Reject([&]{Target(d,square);});
    }

    // Gap: the distance between footprints, negative when they overlap; Z is ignored.
    Place::Box a{{0,0,0},{10,10,10}},b{{13,14,50},{20,20,60}},c{{5,5,0},{15,15,10}};
    Check(Near(Gap(a,b),5),"gap at a corner");
    Check(Near(Gap(a,{{12,0,0},{20,10,10}}),2),"gap along X");
    Check(Gap(a,c)<0,"overlap is negative");
    // Overlapping in space, not resting on top.
    Check(Overlaps(a,c),"boxes overlap");
    Check(!Overlaps(a,{{0,0,10},{10,10,20}}),"resting on top is not overlapping");

    // Sampling: inside the area, the same run from the same seed, another from another.
    s=Settings{};s.count=40;s.seed=99;s.scaleMin=0.5;s.scaleMax=2;
    auto room=PolygonArea({{{-500,-500},{500,-500},{500,500},{-500,500}}});
    {
        Random r1(5),r2(5);
        for(int i=0;i<200;++i)
        {
            auto x=Sample(r1,s,items,room),y=Sample(r2,s,items,room);
            Check(x && y && x->at==y->at && x->rotation==y->rotation && x->scale==y->scale && x->item==y->item,"sampling repeats from a seed");
            Check(Inside(room,x->at),"samples inside the area");
            Check(x->scale>=0.5 && x->scale<=2,"scale in range");
            Check(x->rotation[0]==0 && x->rotation[2]==0 && x->rotation[1]>=0 && x->rotation[1]<65536,"yaw only");
            Check(x->item!=2,"zero-weight mesh not sampled");
        }
        Settings fixed=s;fixed.randomYaw=false;
        Random r3(5);auto z=Sample(r3,fixed,items,room);
        Check(z && z->rotation==Rotation{},"no random yaw: unturned");
        Settings tilt=s;tilt.randomTilt=true;
        Random r4(5);bool tilted=false;
        for(int i=0;i<20;++i){auto t=Sample(r4,tilt,items,room);tilted=tilted || t->rotation[0]!=0 || t->rotation[2]!=0;}
        Check(tilted,"full random tilts");
    }

    // A run on a flat floor: every prop rests on it, inside the room, apart by the spacing.
    s.spacing=24;
    auto run=Run(s,items,room,{},FlatFloor(items,100));
    Check(run.target==40 && run.props.size()==40,"the count is reached");
    for(size_t i=0;i<run.props.size();++i)
    {
        const auto& p=run.props[i];
        Check(Near(p.box.lo[2],100,1e-6),"resting on the floor");
        Check(Fits(room,p.box),"inside the room");
        for(size_t j=0;j<i;++j)Check(Gap(p.box,run.props[j].box)>=24-1e-6,"spaced by bounds");
    }
    // Same seed, same props; Re-roll's next seed, others.
    auto again=Run(s,items,room,{},FlatFloor(items,100));
    Check(again.props.size()==run.props.size() && again.props[7].location==run.props[7].location,"a seed repeats its run");
    Settings rerolled=s;rerolled.seed=NextSeed(s.seed);
    Check(rerolled.seed!=s.seed && rerolled.seed!=0,"re-roll picks another seed");
    auto other=Run(rerolled,items,room,{},FlatFloor(items,100));
    Check(other.props.front().location!=run.props.front().location,"another seed, other props");

    // Too many for the room: the tries run out, short of the count, still spaced.
    {
        Settings full;full.count=500;full.spacing=40;full.seed=3;
        auto small=PolygonArea({{{0,0},{200,0},{200,200},{0,200}}});
        auto packed=Run(full,items,small,{},FlatFloor(items));
        Check(packed.props.size()<500 && packed.props.size()>=4 && packed.tried==500*kTriesPerProp,"a full room stops at its tries");
        Check(packed.crowded>0,"crowded candidates counted");
    }

    // Candidates that clash before the drop are turned away without tracing.
    {
        int calls=0;
        Settings one;one.count=60;one.spacing=200;one.seed=11;
        auto r=Run(one,items,room,{},FlatFloor(items,0,&calls));
        Check(calls<r.tried,"crowded candidates are not dropped");
    }

    // An obstacle (a crate already there) keeps the props off it; one resting on top is fine.
    {
        Settings few;few.count=200;few.seed=8;
        Place::Box crate{{-200,-200,0},{200,200,64}};
        auto r=Run(few,items,room,{crate},FlatFloor(items,0));
        for(const auto& p:r.props)Check(!Overlaps(p.box,crate),"props kept off an existing mesh");
        Check(r.blocked>0,"the obstacle turned candidates away");
        auto onTop=Run(few,items,room,{crate},FlatFloor(items,64));
        Check(onTop.props.size()==200,"props resting on top of a mesh are not overlapping it");
    }

    // Misses from the drop: no floor, and walls.
    {
        Settings few;few.count=10;few.seed=4;
        auto holes=[&](const Candidate& c){Landing l=FlatFloor(items)(c);if(c.at[0]<0)l.miss=Miss::NoFloor;else if(c.at[1]<0)l.miss=Miss::Wall;return l;};
        auto r=Run(few,items,room,{},holes);
        Check(r.props.size()==10 && r.noFloor>0 && r.walls>0,"misses counted");
        for(const auto& p:r.props)Check(p.location[0]>=0 && p.location[1]>=0,"missed candidates not placed");
        auto none=[&](const Candidate& c){Landing l=FlatFloor(items)(c);l.miss=Miss::NoFloor;return l;};
        auto empty=Run(few,items,room,{},none);
        Check(empty.props.empty() && empty.noFloor>0 && empty.noFloor+empty.outside==10*kTriesPerProp,"no floor at all places nothing");
    }

    // A circle: every footprint inside it.
    {
        Settings few;few.count=30;few.seed=21;
        auto r=Run(few,items,CircleArea({1000,1000},300),{},FlatFloor(items));
        Check(r.props.size()==30,"circle filled");
        for(const auto& p:r.props)
            for(double x:{p.box.lo[0],p.box.hi[0]})for(double y:{p.box.lo[1],p.box.hi[1]})
                Check(std::hypot(x-1000,y-1000)<=300+1e-6,"footprint inside the circle");
    }

    // Probes: eight level rays at half height and eight just above the floor, one up.
    {
        Place::Box prop{{-20,-10,100},{20,10,140}};
        auto level=Probes(prop,{0,0,100},{0,0,1});
        Check(level.size()==17,"seventeen probes on a level floor");
        Check(Near(level[0].start[2],120) && Near(level[1].start[2],104) && Near(level[0].reach,18),"mid and low rays");
        Check(Near(level.back().direction[2],1) && Near(level.back().reach,36),"the ray up");
        // On a slope rising towards +X the low ray that way starts higher.
        const double slope=std::sin(Place::kPi/12);
        auto tilted=Probes(prop,{0,0,100},{-slope,0,std::cos(Place::kPi/12)});
        Check(tilted[1].start[2]>104+4 && Near(tilted[1].start[2],104+18*std::tan(Place::kPi/12),1e-6),"low ray raised uphill");
        // A flat prop has only its level rays at half height.
        auto flat=Probes({{-20,-20,0},{20,20,4}},{0,0,0},{0,0,1});
        for(const auto& p:flat)Check(Near(p.start[2],2),"a flat prop's rays");
        Check(Blocked(level[0],5.0) && !Blocked(level[0],18.5) && !Blocked(level[0],std::nullopt),"a probe is blocked by a hit before its reach");
    }
    // Support: Drop to Floor's five rays from a prop resting with its bottom at 100.
    {
        Place::Box prop{{-20,-20,100},{20,20,140}};
        const auto starts=Place::DropStarts(prop,{0,0,-1});
        Check(starts.size()==5 && Near(starts[0][2],120),"five rays from the centre's height");
        using Hits=std::vector<std::optional<double>>;
        Check(Supported(100,starts,Hits(5,20.0)),"level floor under every ray");
        Check(Supported(100,starts,Hits{20.0,20.0,20.0,std::nullopt,300.0}),"hanging over an edge a little");
        Check(!Supported(100,starts,Hits{20.0,20.0,300.0,std::nullopt,300.0}),"hanging off an edge");
        Check(!Supported(100,starts,Hits{20.0,200.0,200.0,200.0,200.0}),"perched on a rail");
        Check(!Supported(100,starts,Hits{std::nullopt,20.0,20.0,20.0,20.0}),"nothing under the middle");
        Check(Supported(100,starts,Hits{14.0,10.0,20.0,26.0,20.0}),"flush on a slope: the floor at or above the bottom");
        Check(!Supported(100,starts,Hits{0.2,20.0,20.0,20.0,20.0}),"a ray that started inside something says nothing");
    }
    std::cout<<"ScatterModelTests passed\n";
    return 0;
  }
  catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<"\n";return 1;}
}
