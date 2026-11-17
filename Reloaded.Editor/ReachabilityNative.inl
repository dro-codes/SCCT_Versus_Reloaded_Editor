// Native side of Reachability (ReachabilityModel.h): the engine's own
// collision queries on the open level, and the starts, objectives, pickups and
// pawn sizes the fill works from. Included in WorkflowEditor.cpp after
// MapCheckNative.inl (FieldsOf) and MapDesignNative.inl (StartTeam,
// DesignClearances).
namespace
{
    // ULevel::MultiLineCheck (0x110bc2b0), as PlacementNative.inl calls it, with
    // the pawn's box as the extent and TraceFlags 0x84: the level's BSP
    // (TRACE_Level) and world geometry (TRACE_LevelGeometry: static meshes and
    // blocking volumes), but not movers, so doors count as open. The LevelInfo
    // is the source actor: it is in no collision list, so nothing is skipped.
    // The FCheckResult list is nearest first: Location +8, Normal +0x14.
    constexpr Address kReachMultiLineCheck=0x110bc2b0,kReachGetLevelInfo=0x10e047d3,kReachMem=0x11691d4c,kReachMemPop=0x10e03004;
    constexpr unsigned kReachTraceFlags=0x84u;
    struct ReachFVec{float x,y,z;};
    struct ReachRaw{int found;float location[3],normal[3];};
    bool ReachSweepRaw(Address level,Address info,const ReachFVec* start,ReachFVec end,const ReachFVec* extent,ReachRaw* out)
    {
        __try
        {
            out->found=0;
            Address mark[3]={kReachMem,*reinterpret_cast<Address*>(kReachMem),*reinterpret_cast<Address*>(kReachMem+0xc)};
            using Check=Address(__thiscall*)(void*,Address,ReachFVec,const ReachFVec*,const ReachFVec*,Address,unsigned,Address);
            const Address hit=reinterpret_cast<Check>(kReachMultiLineCheck)(reinterpret_cast<void*>(level),kReachMem,end,start,extent,info,kReachTraceFlags,info);
            if(hit)
            {
                memcpy(out->location,reinterpret_cast<const void*>(hit+8),12);
                memcpy(out->normal,reinterpret_cast<const void*>(hit+0x14),12);
                out->found=1;
            }
            reinterpret_cast<void(__thiscall*)(void*)>(kReachMemPop)(mark);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    Address ReachLevelInfo(Address level)
    {
        __try{return reinterpret_cast<Address(__thiscall*)(void*)>(kReachGetLevelInfo)(reinterpret_cast<void*>(level));}
        __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    }
    ReachFVec ReachF(const Reach::Vec3& v){return {static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2])};}

    class EngineReachWorld:public Reach::World
    {
    public:
        EngineReachWorld()
        {
            level_=Level();
            if(!level_)throw std::runtime_error("No map is open.");
            info_=ReachLevelInfo(level_);
            if(!info_)throw std::runtime_error("The map's LevelInfo could not be read.");
        }
        Reach::Hit Sweep(const Reach::Vec3& from,const Reach::Vec3& to,const Reach::Vec3& extent) override
        {
            CheckLevel();
            const auto s=ReachF(from),e=ReachF(to),x=ReachF(extent);
            ReachRaw raw{};
            if(!ReachSweepRaw(level_,info_,&s,e,&x,&raw))throw std::runtime_error("The editor's line check failed.");
            Reach::Hit hit;
            if(!raw.found)return hit;
            hit.hit=true;
            hit.location={raw.location[0],raw.location[1],raw.location[2]};
            hit.normal={raw.normal[0],raw.normal[1],raw.normal[2]};
            for(double v:hit.location)if(!std::isfinite(v))throw std::runtime_error("The editor's line check returned a point that is not a number.");
            return hit;
        }
        bool Fits(const Reach::Vec3& centre,const Reach::Vec3& extent) override
        {
            CheckLevel();
            const float at[3]={static_cast<float>(centre[0]),static_cast<float>(centre[1]),static_cast<float>(centre[2])};
            const float size[3]={static_cast<float>(extent[0]),static_cast<float>(extent[1]),static_cast<float>(extent[2])};
            alignas(8) unsigned char hit[kCheckResultSize]={};
            const int result=EncroachRaw(level_,hit,at,size,info_);
            if(result<0)throw std::runtime_error("The editor's encroachment test failed.");
            return result==0;
        }
    private:
        Address level_=0,info_=0;
        // Another map opened under the fill: stop rather than trace a freed level.
        void CheckLevel()
        {
            if(Level()!=level_)throw std::runtime_error("Another map was opened.");
        }
    };

    // The class's walking sizes from DesignClearances' profiles: the largest
    // standing and crouching cylinder among the team's loaded pawn classes, and
    // the step a class sets.
    void ReachPawnSizes(Reach::Scene& scene)
    {
        Json profiles;
        try{profiles=DesignClearances();}
        catch(const std::exception&){return;}
        bool standing[2]={},crouching[2]={};
        Reach::Pawn read[2]={scene.pawns[0],scene.pawns[1]};
        for(const auto& profile:profiles)
        {
            const int team=profile.value("team",std::string())=="Merc"?Reach::kMerc:Reach::kSpy;
            const auto name=Fold(profile.value("name",std::string()));
            const double width=profile.value("width",0.0),height=profile.value("height",0.0);
            auto& pawn=read[team];
            if(name.find(" standing")!=std::string::npos)
            {
                pawn.radius=standing[team]?std::max(pawn.radius,width/2):width/2;
                pawn.height=standing[team]?std::max(pawn.height,height/2):height/2;
                if(profile.contains("stepHeight") && profile.at("stepHeight").is_number())pawn.step=profile.at("stepHeight").get<double>();
                standing[team]=true;
            }
            else if(name.find(" crouching")!=std::string::npos)
            {
                pawn.crouchHeight=crouching[team]?std::max(pawn.crouchHeight,height/2):height/2;
                crouching[team]=true;
            }
        }
        for(int team:{Reach::kSpy,Reach::kMerc})scene.pawns[team]=Reach::ClampPawn(read[team],team);
    }
}
Reach::Scene ReachabilityScene()
{
    Reach::Scene scene;
    const auto level=Level();
    const auto model=Read<Address>(level+0x13c);
    if(!model || Array(model+0x54,0x5c).empty())throw std::runtime_error("The map's BSP has not been built. Build the geometry first.");
    // The BSP's bounds, a little grown: no step leaves them.
    for(auto at:Array(model+0x84,12))
    {
        const auto p=Read<std::array<float,3>>(at);
        if(!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))continue;
        for(size_t k=0;k<3;++k)
        {
            if(!scene.boundsKnown){scene.lo[k]=scene.hi[k]=p[k];continue;}
            scene.lo[k]=std::min(scene.lo[k],double(p[k]));scene.hi[k]=std::max(scene.hi[k],double(p[k]));
        }
        scene.boundsKnown=true;
    }
    for(size_t k=0;k<3;++k){scene.lo[k]-=256;scene.hi[k]+=256;}
    ReachPawnSizes(scene);
    std::map<Address,CheckFields> cache;
    auto has=[](const CheckFields& fields,const char* name){for(const auto& c:fields.classes)if(Fold(c)==Fold(name))return true;return false;};
    for(auto actor:LiveActors())
    {
        const auto& fields=FieldsOf(Read<Address>(actor+0x24),cache);
        if(has(fields,"PlayerStart"))
        {
            Reach::Start start;
            start.path=Path(actor);start.name=NameOf(actor);start.location=Position(actor);
            try{start.team=std::stoi(StartTeam(actor));}catch(const std::exception&){start.team=-1;}
            scene.starts.push_back(std::move(start));
            continue;
        }
        Reach::Target target;
        if(has(fields,"SMission"))continue;
        if(has(fields,"SObjective"))
        {
            // An objective is done at its triggers when it has any; those are listed themselves.
            if(!NameList(actor,"Triggers").empty())continue;
            target.kind="Objective";
        }
        // The flag and its drop zone are objective triggers too: named first.
        else if(has(fields,"SFlagDropZone"))target.kind="Drop zone";
        else if(has(fields,"SFlag"))target.kind="Flag";
        else if(has(fields,"SComputerObjectiveTrigger"))target.kind="Computer terminal";
        else if(has(fields,"SBombTargetObjectiveTrigger"))target.kind="Bomb target";
        else if(has(fields,"SObjectiveTrigger"))target.kind="Objective trigger";
        else if(has(fields,"Pickup") || has(fields,"Inventory"))
        {
            target.kind="Pickup";target.objective=false;target.team=Reach::PickupTeam(fields.classes);
        }
        else continue;
        target.path=Path(actor);target.name=NameOf(actor);target.location=Position(actor);
        if(fields.radius>=0)target.radius=Read<float>(actor+fields.radius);
        if(fields.height>=0)target.height=Read<float>(actor+fields.height);
        if(!std::isfinite(target.radius) || target.radius<0 || target.radius>4096)target.radius=0;
        if(!std::isfinite(target.height) || target.height<0 || target.height>4096)target.height=0;
        scene.targets.push_back(std::move(target));
    }
    return scene;
}
std::unique_ptr<Reach::World> ReachabilityWorld()
{
    Engine();
    return std::make_unique<EngineReachWorld>();
}
