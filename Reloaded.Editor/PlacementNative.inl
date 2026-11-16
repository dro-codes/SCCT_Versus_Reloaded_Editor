// Native side of the placement tools (PlacementModel.h holds the maths). All
// calls run on the UI thread; each operation that changes the map is one
// Undo step.
namespace
{
    namespace Place = Workflow::Placement;

    // A static mesh's bounds, turned and scaled as the actor draws it (as
    // SelectedMeshBounds reads them); none for an actor that draws no mesh.
    std::optional<Place::Box> PlaceMeshBox(Address actor,const Rotation& rotation)
    {
        auto property=Property(actor,"StaticMesh");
        auto mesh=property?Read<Address>(actor+Read<int>(property+0x3c)):0;
        if(!mesh || !IsA(mesh,"StaticMesh"))return std::nullopt;
        if(!IsA(actor,"StaticMeshActor"))
        {
            // DT_StaticMesh; an actor that only carries a mesh reference keeps its cylinder.
            auto draw=Property(actor,"DrawType");
            if(draw && Read<unsigned char>(actor+Read<int>(draw+0x3c))!=8)return std::nullopt;
        }
        auto box=Read<std::array<float,6>>(mesh+0x28);
        auto scale=Read<float>(Field(actor,"DrawScale"));
        auto scale3=Read<std::array<float,3>>(Field(actor,"DrawScale3D"));
        auto pivot=Read<std::array<float,3>>(Field(actor,"PrePivot"));
        for(int axis=0;axis<3;++axis)
            if(!std::isfinite(box[axis]) || !std::isfinite(box[axis+3]) || box[axis]>box[axis+3])return std::nullopt;
        Place::Box local{{box[0],box[1],box[2]},{box[3],box[4],box[5]}};
        return Place::WorldBox(local,Position(actor),rotation,{static_cast<double>(scale)*scale3[0],static_cast<double>(scale)*scale3[1],static_cast<double>(scale)*scale3[2]},{pivot[0],pivot[1],pivot[2]});
    }
    std::optional<Place::Box> PlaceBrushBox(Address actor)
    {
        auto model=Read<Address>(actor+0x238),polys=model?Read<Address>(model+0x50):0;
        if(!polys)return std::nullopt;
        std::array<float,12> coords{};Call<void*>(actor,0xac,coords.data());
        std::vector<Vector> points;
        for(auto poly:Array(polys+0x28,0x14c))
        {
            auto count=Read<unsigned short>(poly+0x148);if(count<3 || count>16)continue;
            for(int j=0;j<count;++j)
            {
                auto local=Read<std::array<float,3>>(poly+0x18+j*12);std::array<float,3> world{};
                reinterpret_cast<void*(__thiscall*)(void*,void*,const void*)>(0x10eb2ba0)(local.data(),world.data(),coords.data());
                const Vector v{world[0],world[1],world[2]};
                bool sane=true;for(auto n:v)if(!std::isfinite(n) || std::abs(n)>1000000)sane=false;
                if(sane)points.push_back(v);
            }
        }
        if(points.empty())return std::nullopt;
        return Place::PointsBox(points);
    }
    float PlaceFloat(Address actor,const char* name)
    {
        auto p=Property(actor,name);
        return p && IsA(p,"FloatProperty")?Read<float>(actor+Read<int>(p+0x3c)):0.f;
    }
    enum class PlaceKind{Actor,Mesh,Brush,Csg};
    const char* PlaceKindName(PlaceKind kind){return kind==PlaceKind::Mesh?"mesh":kind==PlaceKind::Brush?"brush":kind==PlaceKind::Csg?"csg":"actor";}
    struct Placed { Address actor{}; PlaceKind kind{}; Place::Box box; bool locked=false; };
    // An actor's box: a mesh's bounds, a brush's polygons, else its
    // collision cylinder (pawns, pickups, lights, sprites). rotation stands in
    // for the actor's own when asked what a turn would do to a mesh's box.
    Placed PlaceInspect(Address actor,const std::optional<Rotation>& rotation=std::nullopt)
    {
        Placed placed;placed.actor=actor;
        placed.locked=Property(actor,"bLockLocation") && DesignBool(actor,"bLockLocation");
        if(IsA(actor,"Brush"))
        {
            placed.kind=IsA(actor,"Volume") || IsA(actor,"Mover")?PlaceKind::Brush:PlaceKind::Csg;
            if(auto box=PlaceBrushBox(actor)){placed.box=*box;return placed;}
            placed.box={Position(actor),Position(actor)};return placed;
        }
        if(auto box=PlaceMeshBox(actor,rotation?*rotation:RotationOf(actor))){placed.kind=PlaceKind::Mesh;placed.box=*box;return placed;}
        placed.kind=PlaceKind::Actor;
        placed.box=Place::CylinderBox(Position(actor),std::max(0.f,PlaceFloat(actor,"CollisionRadius")),std::max(0.f,PlaceFloat(actor,"CollisionHeight")));
        return placed;
    }
    // The selection the tools act on: never the level info, the builder brush
    // or a camera.
    std::vector<Address> PlaceSelectedActors()
    {
        std::vector<Address> result;auto live=LiveActors();
        for(size_t i=2;i<live.size();++i)
            if((Read<unsigned>(live[i]+0x2f4)&0x40) && !IsA(live[i],"Camera"))result.push_back(live[i]);
        return result;
    }

    // ULevel::MultiLineCheck (0x110bc2b0) as ACTOR ALIGN SNAPTOFLOOR calls it
    // (0x10ef16c4): FMemStack& GMem (0x11691d4c), End by value, &Start,
    // &Extent, GetLevelInfo() (0x10e047d3), TraceFlags 0x86 (movers, level,
    // level geometry: what a pawn stands on) and the source actor, which the
    // trace ignores. It returns GMem's FCheckResult list, nearest first:
    // Next +0, Actor +4, Location +8, Normal +0x14. The FMemMark it is taken
    // under is popped with 0x10e03004. A null source ignores no actor
    // (scene.trace); actor is what was hit (the LevelInfo for the BSP).
    struct PlaceFVec { float x,y,z; };
    struct PlaceHit { int found; float location[3],normal[3]; Address actor; };
    bool PlaceTraceRaw(Address level,Address source,const PlaceFVec* start,PlaceFVec end,PlaceHit* out)
    {
        __try
        {
            out->found=0;out->actor=0;
            const Address info=reinterpret_cast<Address(__thiscall*)(void*)>(0x10e047d3)(reinterpret_cast<void*>(level));
            Address mark[3]={0x11691d4c,*reinterpret_cast<Address*>(0x11691d4c),*reinterpret_cast<Address*>(0x11691d58)};
            const PlaceFVec extent{0,0,0};
            using Check=Address(__thiscall*)(void*,Address,PlaceFVec,const PlaceFVec*,const PlaceFVec*,Address,unsigned,Address);
            Address hit=reinterpret_cast<Check>(0x110bc2b0)(reinterpret_cast<void*>(level),0x11691d4c,end,start,&extent,info,0x86u,source);
            for(int i=0;hit && i<256;++i,hit=*reinterpret_cast<Address*>(hit))
            {
                const Address actor=*reinterpret_cast<Address*>(hit+4);
                if(source && actor==source)continue;
                out->actor=actor;
                memcpy(out->location,reinterpret_cast<const void*>(hit+8),12);
                memcpy(out->normal,reinterpret_cast<const void*>(hit+0x14),12);
                out->found=1;break;
            }
            reinterpret_cast<void(__thiscall*)(void*)>(0x10e03004)(mark);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    struct PlaceRay { std::optional<double> distance; Vector normal{}; };
    PlaceRay PlaceTrace(Address level,Address source,const Vector& start,const Vector& direction)
    {
        const Vector end=Place::Add(start,Place::Scale(direction,Place::kReach));
        const PlaceFVec s{static_cast<float>(start[0]),static_cast<float>(start[1]),static_cast<float>(start[2])};
        const PlaceFVec e{static_cast<float>(end[0]),static_cast<float>(end[1]),static_cast<float>(end[2])};
        PlaceHit hit{};
        if(!PlaceTraceRaw(level,source,&s,e,&hit))throw std::runtime_error("The editor's line check failed.");
        PlaceRay ray;
        if(!hit.found)return ray;
        const Vector at{hit.location[0],hit.location[1],hit.location[2]};
        ray.distance=Place::Dot(Place::Sub(at,start),direction);
        ray.normal={hit.normal[0],hit.normal[1],hit.normal[2]};
        return ray;
    }
    // Where one actor would go: its new rotation (aligned to the surface it
    // drops onto, for a mesh when asked) and how far it moves.
    struct PlaceDropPlan { bool found=false; Rotation rotation{}; Vector delta{}; };
    PlaceDropPlan PlanDrop(Address level,Address actor,Place::Surface surface,bool align)
    {
        PlaceDropPlan plan;plan.rotation=RotationOf(actor);
        auto placed=PlaceInspect(actor);
        if(align && placed.kind==PlaceKind::Mesh)
        {
            const auto direction=Place::DropDirection(surface,plan.rotation);
            const auto ray=PlaceTrace(level,actor,Place::Centre(placed.box),direction);
            if(ray.distance && *ray.distance>0.5 && Place::Length(ray.normal)>0.5)
            {
                plan.rotation=Place::AlignToSurface(plan.rotation,ray.normal,surface);
                placed=PlaceInspect(actor,plan.rotation);
            }
        }
        const auto direction=Place::DropDirection(surface,plan.rotation);
        std::vector<std::optional<double>> hits;
        for(const auto& start:Place::DropStarts(placed.box,direction))hits.push_back(PlaceTrace(level,actor,start,direction).distance);
        const auto move=Place::DropDistance(placed.box,direction,hits);
        if(!move)return plan;
        plan.found=true;plan.delta=Place::Scale(direction,*move);
        return plan;
    }
    bool PlaceChanged(const PlaceDropPlan& plan,const Rotation& rotation)
    {
        return plan.found && (Place::Length(plan.delta)>0.01 || plan.rotation!=rotation);
    }
}
Json PlacementSelection()
{
    Json result=Json::array();
    for(auto actor:PlaceSelectedActors())
    {
        auto placed=PlaceInspect(actor);
        auto item=Identity(actor);
        item["address"]=static_cast<uint64_t>(actor);
        item["name"]=NameOf(actor);
        item["position"]=Position(actor);
        item["rotation"]=RotationOf(actor);
        item["lo"]=placed.box.lo;item["hi"]=placed.box.hi;
        item["kind"]=PlaceKindName(placed.kind);
        item["locked"]=placed.locked;
        result.push_back(std::move(item));
    }
    return result;
}
std::vector<uintptr_t> SelectedActorAddresses()
{
    std::vector<uintptr_t> result;auto live=LiveActors();
    for(size_t i=2;i<live.size();++i)if(Read<unsigned>(live[i]+0x2f4)&0x40)result.push_back(live[i]);
    return result;
}
size_t PlacementMove(const Json& moves,const std::string& label)
{
    std::vector<std::pair<Address,Vector>> changes;const ActorIndex index;
    for(const auto& move:moves)
    {
        auto actor=index.Resolve(move);
        if(!actor)throw std::runtime_error("The selection changed. Try again.");
        if(Property(actor,"bLockLocation") && DesignBool(actor,"bLockLocation"))continue;
        const auto delta=move.at("delta").get<Vector>();
        if(Place::Length(delta)<0.001)continue;
        auto target=Place::Add(Position(actor),delta);Place::Check(target);
        changes.push_back({actor,target});
    }
    index.Report(moves.size(),"PlacementMove");
    if(changes.empty())return 0;
    Transaction transaction(label.c_str());
    for(auto& [actor,target]:changes){Modify(actor);SetPosition(actor,target);Call(actor,0x44);}
    transaction.Commit();Redraw();
    return changes.size();
}
size_t PlacementTurn(const Json& changes,const std::string& label)
{
    std::vector<std::pair<Address,Rotation>> turns;const ActorIndex index;
    for(const auto& change:changes)
    {
        auto actor=index.Resolve(change);
        if(!actor)throw std::runtime_error("The selection changed. Try again.");
        if(Property(actor,"bLockLocation") && DesignBool(actor,"bLockLocation"))continue;
        const auto rotation=change.at("rotation").get<Rotation>();
        if(rotation==RotationOf(actor))continue;
        turns.push_back({actor,rotation});
    }
    index.Report(changes.size(),"PlacementTurn");
    if(turns.empty())return 0;
    Transaction transaction(label.c_str());
    for(auto& [actor,rotation]:turns){Modify(actor);Write(Field(actor,"Rotation"),rotation);Call(actor,0x44);}
    transaction.Commit();Redraw();
    return turns.size();
}
Json PlacementDrop(int surfaceIndex,bool align)
{
    if(surfaceIndex<0 || surfaceIndex>6)throw std::runtime_error("Invalid surface.");
    const auto surface=static_cast<Place::Surface>(surfaceIndex);
    const auto level=Level();
    auto actors=PlaceSelectedActors();
    if(actors.empty())throw std::runtime_error("Select the actors to drop first.");
    Json report={{"moved",0},{"missed",0},{"unchanged",0},{"locked",0},{"brushes",0}};
    // The lowest first onto a floor (the highest onto a ceiling), so a stack
    // settles from the bottom up; the nearest first to a wall along X or Y,
    // so a row packs against it.
    std::vector<std::pair<Address,Place::Box>> order;
    for(auto actor:actors)
    {
        auto placed=PlaceInspect(actor);
        if(placed.locked){report["locked"]=report["locked"].get<int>()+1;continue;}
        // A CSG brush is in the BSP it would be dropped onto: it would only
        // find itself.
        if(placed.kind==PlaceKind::Csg){report["brushes"]=report["brushes"].get<int>()+1;continue;}
        order.push_back({actor,placed.box});
    }
    const bool axis=Place::IsWall(surface) && surface!=Place::Surface::Wall;
    const auto push=axis?Place::DropDirection(surface,{}):Vector{};
    std::stable_sort(order.begin(),order.end(),[&](const auto& a,const auto& b)
    {
        if(axis)return Place::Dot(Place::Centre(a.second),push)>Place::Dot(Place::Centre(b.second),push);
        return surface==Place::Surface::Ceiling?a.second.hi[2]>b.second.hi[2]:a.second.lo[2]<b.second.lo[2];
    });
    // A first look decides whether anything moves at all, so a drop that
    // changes nothing leaves no empty Undo step.
    bool any=false;
    for(auto& [actor,box]:order)if(PlaceChanged(PlanDrop(level,actor,surface,align),RotationOf(actor))){any=true;break;}
    if(any)
    {
        Transaction transaction(surface==Place::Surface::Floor?(align?"Drop to floor and align":"Drop to floor"):surface==Place::Surface::Ceiling?"Drop to ceiling":"Push to wall");
        for(auto& [actor,box]:order)
        {
            // Planned again in turn, so an actor can land on one dropped before it.
            const auto rotation=RotationOf(actor);
            const auto plan=PlanDrop(level,actor,surface,align);
            if(!plan.found){report["missed"]=report["missed"].get<int>()+1;continue;}
            if(!PlaceChanged(plan,rotation)){report["unchanged"]=report["unchanged"].get<int>()+1;continue;}
            Modify(actor);
            if(plan.rotation!=rotation)Write(Field(actor,"Rotation"),plan.rotation);
            auto target=Place::Add(Position(actor),plan.delta);Place::Check(target);
            SetPosition(actor,target);Call(actor,0x44);
            report["moved"]=report["moved"].get<int>()+1;
        }
        transaction.Commit();Redraw();
    }
    else for(auto& [actor,box]:order)
    {
        const auto plan=PlanDrop(level,actor,surface,align);
        report[plan.found?"unchanged":"missed"]=report[plan.found?"unchanged":"missed"].get<int>()+1;
    }
    return report;
}
namespace
{
    // A copy's names carry an "A<n>_" prefix; copying a copy starts from the
    // original name again rather than stacking prefixes.
    std::string PlaceStripPrefix(const std::string& name)
    {
        static const std::regex prefix("^(A[0-9]+_)+",std::regex::icase);
        auto stripped=std::regex_replace(name,prefix,"");
        return stripped.empty()?name:stripped;
    }
}
Json PlacementCopy(const Json& members,const Json& copies,const std::string& label)
{
    if(!members.is_array() || !copies.is_array())throw std::runtime_error("Invalid copy request.");
    Place::CheckCount(static_cast<int>(std::min<size_t>(copies.size(),100000)),members.size());
    if(!pasteHookReady)throw std::runtime_error("This editor build does not support verified actor insertion.");
    if(insertionText)throw std::runtime_error("Actor insertion is already running.");
    std::vector<Place::Copy> plan;
    for(const auto& c:copies)
    {
        Place::Copy copy;
        copy.before=c.value("before",Vector{});copy.pivot=c.value("pivot",Vector{});copy.after=c.value("after",Vector{});
        copy.yaw=c.value("yaw",0);copy.turn=c.value("turn",false);
        plan.push_back(copy);
    }
    // The selection as text, in world coordinates; copies place that.
    auto definition=CaptureAssembly(members,Pose{});
    // A Tag stays as it is on every copy (lights down a corridor still answer
    // the same trigger), unless an Event inside the selection names it: then
    // each copy gets its own, so a copied switch works its own door.
    std::set<std::string> events;
    for(const auto& actor:definition.at("actors"))
        if(auto event=Fold(actor.value("event",std::string()));!event.empty() && event!="none")events.insert(event);
    std::set<std::string> stripped;bool strip=true;
    for(const auto& actor:definition.at("actors"))if(!stripped.insert(Fold(PlaceStripPrefix(actor.at("name").get<std::string>()))).second)strip=false;
    const std::regex actorName("(Begin Actor[^\\r\\n]*\\bName=\"?)(A[0-9]+_)+",std::regex::icase);
    for(auto& actor:definition.at("actors"))
    {
        if(!events.count(Fold(actor.value("tag",std::string()))))actor.erase("tag");
        if(strip)
        {
            actor["name"]=PlaceStripPrefix(actor.at("name").get<std::string>());
            actor["text"]=std::regex_replace(actor.at("text").get<std::string>(),actorName,"$1",std::regex_constants::format_first_only);
        }
    }
    // Links out of the selection stay pointed where they were.
    std::map<std::string,std::string> bindings;
    for(const auto& binding:definition.at("bindings"))bindings[binding.at("id").get<std::string>()]=binding.at("path").get<std::string>();
    // Every name already in the map's package, live or waiting in the Undo
    // buffer, so a copy never reuses one.
    std::set<std::string> used;
    const auto package=Read<Address>(Level()+0x18);
    for(auto at:Array(0x11697B70)){auto object=Read<Address>(at);if(object && Read<Address>(object+0x18)==package)used.insert(Fold(NameOf(object)));}
    struct Planned { std::string name; Vector position; Rotation rotation; };
    std::vector<Planned> planned;std::string t3d="Begin Map\n";
    int serial=1;
    for(const auto& copy:plan)
    {
        Json instance=definition;
        for(auto& actor:instance.at("actors"))
        {
            auto position=Place::Apply(copy,actor.at("position").get<Vector>());Place::Check(position,"A copy would leave the world.");
            actor["position"]=position;
            actor["rotation"]=Place::Apply(copy,actor.at("rotation").get<Rotation>());
        }
        std::string prefix;
        for(;;++serial)
        {
            prefix="A"+std::to_string(serial)+"_";bool free=true;
            for(const auto& actor:instance.at("actors"))
            {
                if(used.count(Fold(prefix+actor.at("name").get<std::string>()))){free=false;break;}
                for(const auto& name:InlineObjectNames(actor.at("text").get<std::string>()))if(used.count(Fold(prefix+name))){free=false;break;}
                if(!free)break;
            }
            if(free)break;
        }
        ++serial;
        auto prepared=PreparePlacement(instance,Pose{},prefix,LevelPath(),bindings);
        for(const auto& actor:prepared.at("actors"))
        {
            planned.push_back({actor.at("name").get<std::string>(),actor.at("position").get<Vector>(),actor.at("rotation").get<Rotation>()});
            used.insert(Fold(actor.at("name").get<std::string>()));
            for(const auto& name:InlineObjectNames(actor.at("text").get<std::string>()))used.insert(Fold(name));
            t3d+=actor.at("text").get<std::string>();
        }
    }
    t3d+="End Map\n";
    const auto before=SelectedIdentities();
    const std::string text=ConvertText(t3d,CP_UTF8,CP_ACP);
    struct TextScope { explicit TextScope(const char* value){insertionText=value;} ~TextScope(){insertionText=nullptr;} } source(text.c_str());
    Json created=Json::array();
    try
    {
        Transaction transaction(label.c_str());
        Select(Json::array());
        // Verified UUnrealEdEngine::edactPasteSelected, as PlaceAssembly uses
        // it: the pasted actors come in selected, recentred; their poses are
        // put back before the step ends.
        Call(Engine(),0x26c,reinterpret_cast<void*>(Level()),0);
        std::map<std::string,Address> pasted;
        for(auto actor:LiveActors())if(Read<unsigned>(actor+0x2f4)&0x40)pasted[Fold(NameOf(actor))]=actor;
        if(pasted.size()!=planned.size())throw std::runtime_error("The editor pasted "+std::to_string(pasted.size())+" actors instead of "+std::to_string(planned.size())+"; nothing was copied.");
        for(const auto& item:planned)
        {
            auto found=pasted.find(Fold(item.name));
            if(found==pasted.end())throw std::runtime_error("The editor did not paste "+item.name+"; nothing was copied.");
            auto actor=found->second;
            Modify(actor);SetPosition(actor,item.position);Write(Field(actor,"Rotation"),item.rotation);Call(actor,0x44);
            created.push_back(Identity(actor));
        }
        transaction.Commit();
    }
    catch(...){try{Select(before);}catch(...){}throw;}
    // The originals and their copies stay selected, to move or turn together.
    Json all=members;for(const auto& id:created)all.push_back(id);
    Select(all);Redraw();
    return created;
}
namespace
{
    // What the floating check reads per class, resolved once (Map Check's
    // FieldsOf and the fields PlaceMeshBox looks up by name): a map can hold
    // thousands of static meshes.
    struct FloorFields
    {
        const CheckFields* check=nullptr;
        bool mesh=false;   // a StaticMeshActor with every field below
        int staticMesh=-1,drawScale=-1,drawScale3D=-1,prePivot=-1,rotation=-1;
    };
    const FloorFields& FloorFieldsOf(Address cls,std::map<Address,CheckFields>& checks,std::map<Address,FloorFields>& cache)
    {
        if(auto found=cache.find(cls);found!=cache.end())return found->second;
        FloorFields fields;fields.check=&FieldsOf(cls,checks);
        bool staticMeshActor=false;
        for(const auto& name:fields.check->classes)staticMeshActor=staticMeshActor || Fold(name)=="staticmeshactor";
        for(auto p:Properties(cls))
        {
            const auto name=Fold(NameOf(p));
            int* offset=name=="staticmesh"?&fields.staticMesh:name=="drawscale"?&fields.drawScale:name=="drawscale3d"?&fields.drawScale3D
                :name=="prepivot"?&fields.prePivot:name=="rotation"?&fields.rotation:nullptr;
            if(offset && *offset<0)*offset=Read<int>(p+0x3c);   // the first, as Property finds it
        }
        fields.mesh=staticMeshActor && fields.staticMesh>=0 && fields.drawScale>=0 && fields.drawScale3D>=0 && fields.prePivot>=0 && fields.rotation>=0;
        return cache.emplace(cls,std::move(fields)).first->second;
    }
    // A static mesh actor's box as PlaceMeshBox builds it, from those offsets.
    std::optional<Place::Box> FloorMeshBox(Address actor,const FloorFields& fields,std::map<Address,bool>& meshClasses)
    {
        const auto mesh=Read<Address>(actor+fields.staticMesh);
        if(!mesh)return std::nullopt;
        const auto meshClass=Read<Address>(mesh+0x24);
        auto known=meshClasses.find(meshClass);
        if(known==meshClasses.end())known=meshClasses.emplace(meshClass,IsA(mesh,"StaticMesh")).first;
        if(!known->second)return std::nullopt;
        const auto box=Read<std::array<float,6>>(mesh+0x28);
        const auto scale=Read<float>(actor+fields.drawScale);
        const auto scale3=Read<std::array<float,3>>(actor+fields.drawScale3D);
        const auto pivot=Read<std::array<float,3>>(actor+fields.prePivot);
        for(int axis=0;axis<3;++axis)
            if(!std::isfinite(box[axis]) || !std::isfinite(box[axis+3]) || box[axis]>box[axis+3])return std::nullopt;
        const Place::Box local{{box[0],box[1],box[2]},{box[3],box[4],box[5]}};
        return Place::WorldBox(local,Position(actor),Read<Rotation>(actor+fields.rotation),{static_cast<double>(scale)*scale3[0],static_cast<double>(scale)*scale3[1],static_cast<double>(scale)*scale3[2]},{pivot[0],pivot[1],pivot[2]});
    }
}
// Map Check's floating meshes (MapCheckModel.h CheckProps): Drop to Floor's
// rays for every visible static mesh, each reaching only maxGap past the
// mesh's bottom instead of PlaceTrace's whole world. A line check that
// faults stops the scan, since it leaves the editor's memory mark behind, and
// Map Check says so.
MapCheck::FloorScan MapCheckFloorGaps(double maxGap)
{
    MapCheck::FloorScan scan;scan.ran=true;
    const auto started=std::chrono::steady_clock::now();
    const auto level=Level();
    auto live=LiveActors();
    std::map<Address,CheckFields> checks;
    std::map<Address,FloorFields> cache;
    std::map<Address,bool> meshClasses;
    const Vector down{0,0,-1};
    size_t meshes=0,rays=0;bool failed=false;
    for(size_t i=2;i<live.size() && !scan.stopped;++i)
    {
        const auto actor=live[i];
        try
        {
            if(Read<unsigned>(actor+0x2E8)&0x1000)continue;
            const auto& fields=FloorFieldsOf(Read<Address>(actor+0x24),checks,cache);
            if(!fields.mesh)continue;
            const auto& check=*fields.check;
            const bool soft=!(check.collide>=0 && (Read<unsigned>(actor+check.collide)&check.collideMask))
                || !(check.block>=0 && (Read<unsigned>(actor+check.block)&check.blockMask));
            // A mesh hidden only in the editor is still there to rest on.
            const bool hiddenEd=(Read<unsigned>(actor+0x2f4)&0x10)!=0;
            if(hiddenEd && !soft)continue;
            const auto box=FloorMeshBox(actor,fields,meshClasses);
            if(!box)continue;
            if(soft)scan.soft.push_back({box->lo,box->hi});
            if(hiddenEd)continue;
            ++meshes;
            const double reach=Place::Support(*box,down)+maxGap+2;
            std::vector<std::optional<double>> hits;
            for(const auto& start:Place::DropStarts(*box,down))
            {
                const auto end=Place::Add(start,Place::Scale(down,reach));
                const PlaceFVec s{static_cast<float>(start[0]),static_cast<float>(start[1]),static_cast<float>(start[2])};
                const PlaceFVec e{static_cast<float>(end[0]),static_cast<float>(end[1]),static_cast<float>(end[2])};
                PlaceHit hit{};++rays;
                if(!PlaceTraceRaw(level,actor,&s,e,&hit)){scan.stopped=true;scan.checked=meshes-1;throw std::runtime_error("The editor's line check failed.");}
                if(!hit.found){hits.push_back(std::nullopt);continue;}
                hits.push_back(Place::Dot(Place::Sub({hit.location[0],hit.location[1],hit.location[2]},start),down));
            }
            if(const auto gap=Place::DropDistance(*box,down,hits))scan.gaps.push_back({Path(actor),NameOf(actor),*gap,box->lo,box->hi});
        }
        catch(const std::exception& e)
        {
            if(!failed || scan.stopped)Logger::log(std::string("MapCheck: floating check skipped an actor: ")+e.what()+(scan.stopped?" The check stopped there.":""));
            failed=true;
        }
    }
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    Logger::log("MapCheck: floating check: "+std::to_string(meshes)+" meshes, "+std::to_string(rays)+" rays, "+std::to_string(ms)+" ms");
    return scan;
}
