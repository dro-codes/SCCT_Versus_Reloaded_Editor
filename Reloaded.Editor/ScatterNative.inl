// Native side of Scatter Props (ScatterModel.h holds the sampling and the
// spacing). Included after PlacementNative.inl, ConveyorNative.inl and
// StairSmoothNative.inl, whose traces, mesh loading and collision-hash
// handling it uses. All calls run on the UI thread; placing is one Undo step.
namespace
{
    namespace Scatter = Workflow::Scatter;

    // Prop names: ScatterProp<n>, never one already in the map's package,
    // live or waiting in the Undo buffer.
    constexpr const char* kScatterPrefix="ScatterProp";
    constexpr const char* kScatterGroup="Scatter";

    struct ScatterRay { std::optional<double> distance; Vector normal{}; };
    // PlaceTraceRaw (Drop to Floor's line check) from start along direction for reach units.
    ScatterRay ScatterTrace(Address level,Address source,const Vector& start,const Vector& direction,double reach)
    {
        const Vector end=Place::Add(start,Place::Scale(direction,reach));
        const PlaceFVec s{static_cast<float>(start[0]),static_cast<float>(start[1]),static_cast<float>(start[2])};
        const PlaceFVec e{static_cast<float>(end[0]),static_cast<float>(end[1]),static_cast<float>(end[2])};
        PlaceHit hit{};
        if(!PlaceTraceRaw(level,source,&s,e,&hit))throw std::runtime_error("The editor's line check failed.");
        ScatterRay ray;
        if(!hit.found)return ray;
        ray.distance=Place::Dot(Place::Sub({hit.location[0],hit.location[1],hit.location[2]},start),direction);
        ray.normal={hit.normal[0],hit.normal[1],hit.normal[2]};
        return ray;
    }
    Address ScatterBuilder()
    {
        auto builder=reinterpret_cast<Address>(MapRecovery::ResolveBuilderBrushActor(reinterpret_cast<void*>(Level())));
        if(!builder)throw std::runtime_error("The builder brush is unavailable.");
        return builder;
    }
    // A static mesh's bounds as the mesh holds them.
    Place::Box ScatterMeshBounds(Address mesh)
    {
        const auto box=Read<std::array<float,6>>(mesh+0x28);
        for(int axis=0;axis<3;++axis)
            if(!std::isfinite(box[axis]) || !std::isfinite(box[axis+3]) || box[axis]>box[axis+3] || box[axis+3]-box[axis]>100000)
                throw std::runtime_error(Path(mesh)+" has no usable bounds.");
        return {{box[0],box[1],box[2]},{box[3],box[4],box[5]}};
    }
    Json ScatterBoxJson(const Place::Box& box){return {{"lo",box.lo},{"hi",box.hi}};}
    // The editor hides every actor whose Group is not in LevelInfo.VisibleGroups
    // (a comma-separated list), so a new group is added there, in the same Undo
    // step, before any prop joins it.
    void ScatterShowGroup()
    {
        const auto live=LiveActors();
        if(live.empty() || !IsA(live[0],"LevelInfo"))return;
        const auto info=live[0];
        const auto p=Property(info,"VisibleGroups");
        if(!p || !IsA(p,"StrProperty"))return;
        const auto at=info+Read<int>(p+0x3c);
        const auto data=Read<Address>(at);const int count=Read<int>(at+4);
        const std::string groups=data && count>0 && count<1000000?String(data,static_cast<size_t>(count)):std::string();
        // An empty list is a map that never used groups: left as it is.
        if(groups.empty())return;
        std::stringstream list(groups);
        for(std::string group;std::getline(list,group,',');)if(Fold(group)==Fold(kScatterGroup))return;
        Modify(info);
        const auto next=ConvertText(groups+","+kScatterGroup,CP_UTF8,CP_ACP);
        // FString::operator=(const TCHAR*), as MagicImport assigns a string.
        reinterpret_cast<void*(__thiscall*)(void*,const char*)>(0x10e03770)(reinterpret_cast<void*>(at),next.c_str());
    }
}

std::string ScatterBrowserMesh()
{
    // GUnrealEd's CurrentStaticMesh: the one the Static Mesh Browser shows.
    const auto mesh=Read<Address>(Engine()+0x13c);
    return mesh && IsA(mesh,"StaticMesh")?Path(mesh):std::string();
}
Json ScatterMesh(const std::string& path)
{
    Engine();
    auto mesh=LoadStockMesh(path);
    if(!mesh)throw std::runtime_error(path+" is not a static mesh the editor can load.");
    auto result=ScatterBoxJson(ScatterMeshBounds(mesh));
    result["path"]=Path(mesh);
    return result;
}
// The area as ScatterModel.h's Area wants it, plus how high a drop starts
// ("top") and how far down a floor may be ("bottom"): the builder brush's
// top and bottom, or for the circle 128 units above and below the click, so
// props stay on the clicked floor and do not fall through a grating or a
// stairwell to the floor below.
Json ScatterArea(bool circle,double radius)
{
    Engine();
    if(circle)
    {
        auto unrealEd=Read<Address>(0x117a59b0);if(!unrealEd)throw std::runtime_error("The editor is unavailable.");
        const auto click=Read<std::array<float,3>>(unrealEd+0x1bc);
        auto area=Scatter::CircleArea({click[0],click[1]},radius);
        return {{"circle",true},{"centre",area.centre},{"radius",radius},{"z",click[2]},{"top",click[2]+128.0},{"bottom",click[2]-128.0}};
    }
    const auto builder=ScatterBuilder();
    auto model=Read<Address>(builder+0x238),polys=model?Read<Address>(model+0x50):0;
    if(!polys)throw std::runtime_error("Rebuild the builder brush first.");
    std::array<float,12> coords{};Call<void*>(builder,0xac,coords.data());
    Json faces=Json::array();
    double top=-1e30,bottom=1e30;
    for(auto poly:Array(polys+0x28,0x14c))
    {
        const auto count=Read<unsigned short>(poly+0x148);if(count<3 || count>16)continue;
        Json face=Json::array();
        for(unsigned i=0;i<count;++i)
        {
            auto local=Read<std::array<float,3>>(poly+0x18+i*12);std::array<float,3> world{};
            reinterpret_cast<void*(__thiscall*)(void*,void*,const void*)>(0x10eb2ba0)(local.data(),world.data(),coords.data());
            face.push_back(Scatter::Point{world[0],world[1]});
            top=std::max(top,static_cast<double>(world[2]));bottom=std::min(bottom,static_cast<double>(world[2]));
        }
        faces.push_back(std::move(face));
    }
    std::vector<Scatter::Polygon> polygons;for(const auto& f:faces)polygons.push_back(f.get<Scatter::Polygon>());
    Scatter::PolygonArea(polygons); // throws when it covers nothing
    if(!(top>bottom))throw std::runtime_error("The builder brush is flat: give it some height.");
    return {{"circle",false},{"polygons",faces},{"top",top},{"bottom",bottom}};
}
namespace
{
    Scatter::Area ScatterAreaOf(const Json& area)
    {
        if(area.at("circle").get<bool>())return Scatter::CircleArea(area.at("centre").get<Scatter::Point>(),area.at("radius").get<double>());
        return Scatter::PolygonArea(area.at("polygons").get<std::vector<Scatter::Polygon>>());
    }
    // The small static meshes already near the area: props to keep clear of.
    std::vector<Place::Box> ScatterObstacles(const Scatter::Area& area,double top,double bottom)
    {
        std::vector<Place::Box> result;
        std::map<Address,CheckFields> checks;std::map<Address,FloorFields> cache;std::map<Address,bool> meshClasses;
        auto live=LiveActors();
        for(size_t i=2;i<live.size();++i)
        {
            try
            {
                const auto& fields=FloorFieldsOf(Read<Address>(live[i]+0x24),checks,cache);
                if(!fields.mesh)continue;
                const auto box=FloorMeshBox(live[i],fields,meshClasses);
                if(!box)continue;
                bool prop=true;for(int a=0;a<3;++a)prop=prop && box->hi[a]-box->lo[a]<=Scatter::kPropSize;
                if(!prop)continue;
                if(box->hi[0]<area.lo[0] || box->lo[0]>area.hi[0] || box->hi[1]<area.lo[1] || box->lo[1]>area.hi[1])continue;
                if(box->hi[2]<bottom-64 || box->lo[2]>top+64)continue;
                result.push_back(*box);
            }
            catch(const std::exception&) { /* An actor being edited; it is not in the way. */ }
        }
        return result;
    }
    // The floor under (x, y): a ray down from the top, started lower in 32-unit
    // steps while it begins inside something (a builder brush whose top is in
    // the ceiling), and no floor below the bottom.
    std::optional<std::pair<double,Vector>> ScatterFloor(Address level,Address source,const Scatter::Point& at,double top,double bottom)
    {
        for(double z=top;z>bottom;z-=32)
        {
            const auto ray=ScatterTrace(level,source,{at[0],at[1],z},{0,0,-1},z-bottom);
            if(!ray.distance)return std::nullopt;
            if(*ray.distance>0.5)return std::make_pair(z-*ray.distance,ray.normal);
        }
        return std::nullopt;
    }
}
// request: {items:[{path,weight,lo,hi}], area (ScatterArea's), count, byDensity,
// density, randomYaw, randomTilt, align, scaleMin, scaleMax, spacing, seed}.
// Returns {props:[{item,path,location,rotation,scale,lo,hi}], target, tried,
// noFloor, walls, outside, crowded, blocked, obstacles, rays, ms}.
Json ScatterPlan(const Json& request)
{
    const auto started=std::chrono::steady_clock::now();
    const auto level=Level();
    const auto source=ScatterBuilder();
    std::vector<Scatter::Item> items;std::vector<std::string> paths;
    for(const auto& item:request.at("items"))
    {
        items.push_back({item.at("weight").get<double>(),{item.at("lo").get<Vector>(),item.at("hi").get<Vector>()}});
        paths.push_back(item.at("path").get<std::string>());
    }
    Scatter::Settings settings;
    settings.byDensity=request.value("byDensity",false);
    settings.count=request.value("count",20);
    settings.density=request.value("density",1.0);
    settings.randomYaw=request.value("randomYaw",true);
    settings.randomTilt=request.value("randomTilt",false);
    settings.scaleMin=request.value("scaleMin",1.0);
    settings.scaleMax=request.value("scaleMax",1.0);
    settings.spacing=request.value("spacing",0.0);
    settings.seed=request.value("seed",uint64_t{1});
    const bool align=request.value("align",false) && !settings.randomTilt;
    const auto& areaJson=request.at("area");
    const auto area=ScatterAreaOf(areaJson);
    const double top=areaJson.at("top").get<double>(),bottom=areaJson.at("bottom").get<double>();
    const auto obstacles=ScatterObstacles(area,top,bottom);
    const Vector down{0,0,-1};
    size_t rays=0;
    auto drop=[&](const Scatter::Candidate& c)
    {
        Scatter::Landing landing;
        const auto floor=ScatterFloor(level,source,c.at,top,bottom);++rays;
        if(!floor){landing.miss=Scatter::Miss::NoFloor;return landing;}
        auto rotation=c.rotation;
        const Vector scale{c.scale,c.scale,c.scale};
        if(align && Place::Length(floor->second)>0.5 && floor->second[2]>0.1)rotation=Place::AlignToSurface(rotation,floor->second,Place::Surface::Floor);
        // Its bottom just above the floor found, then Drop to Floor's rays.
        const auto& bounds=items[c.item].bounds;
        auto box=Place::WorldBox(bounds,{c.at[0],c.at[1],0},rotation,scale);
        Vector location{c.at[0],c.at[1],floor->first+2-box.lo[2]};
        box=Place::Moved(box,{0,0,location[2]});
        std::vector<std::optional<double>> hits;Vector centreNormal=floor->second;
        const auto starts=Place::DropStarts(box,down);
        for(size_t i=0;i<starts.size();++i)
        {
            const auto ray=ScatterTrace(level,source,starts[i],down,Place::Support(box,down)+64);++rays;
            hits.push_back(ray.distance);
            if(i==0 && ray.distance)centreNormal=ray.normal;
        }
        const auto move=Place::DropDistance(box,down,hits);
        if(!move){landing.miss=Scatter::Miss::NoFloor;return landing;}
        location[2]-=*move;box=Place::Moved(box,{0,0,-*move});
        // Perched on a rail or hanging off an edge.
        if(!Scatter::Supported(box.lo[2],starts,hits)){landing.miss=Scatter::Miss::NoFloor;return landing;}
        const Vector floorPoint{starts[0][0],starts[0][1],starts[0][2]-*hits[0]};
        for(const auto& probe:Scatter::Probes(box,floorPoint,centreNormal))
        {
            ++rays;
            if(Scatter::Blocked(probe,ScatterTrace(level,source,probe.start,probe.direction,probe.reach+1).distance)){landing.miss=Scatter::Miss::Wall;return landing;}
        }
        landing.prop={c.item,location,rotation,c.scale,box};
        return landing;
    };
    const auto result=Scatter::Run(settings,items,area,obstacles,drop);
    Json props=Json::array();
    for(const auto& p:result.props)
        props.push_back({{"item",p.item},{"path",paths[p.item]},{"location",p.location},{"rotation",p.rotation},{"scale",p.scale},{"lo",p.box.lo},{"hi",p.box.hi}});
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count();
    return {{"props",props},{"target",result.target},{"tried",result.tried},{"noFloor",result.noFloor},{"walls",result.walls},
            {"outside",result.outside},{"crowded",result.crowded},{"blocked",result.blocked},{"obstacles",obstacles.size()},{"rays",rays},{"ms",ms}};
}
// props: ScatterPlan's. Pastes them as StaticMeshActors in one Undo step, in
// the Scatter group, and selects them. Returns their identities.
Json ScatterPlace(const Json& props)
{
    Engine();
    if(!props.is_array() || props.empty())throw std::runtime_error("Nothing to place: no prop found a free spot on the floor.");
    if(props.size()>static_cast<size_t>(Scatter::kMaxProps))throw std::runtime_error("One scatter places at most "+std::to_string(Scatter::kMaxProps)+" props.");
    if(!pasteHookReady)throw std::runtime_error("This editor build does not support verified actor insertion.");
    if(insertionText)throw std::runtime_error("Actor insertion is already running.");
    std::map<std::string,std::string> meshes; // as given -> the mesh's own path
    for(const auto& p:props)
    {
        const auto path=p.at("path").get<std::string>();
        if(meshes.count(path))continue;
        auto mesh=LoadStockMesh(path);
        if(!mesh)throw std::runtime_error(path+" is not a static mesh the editor can load.");
        meshes[path]=Path(mesh);
    }
    std::set<std::string> used;
    const auto package=Read<Address>(Level()+0x18);
    for(auto at:Array(0x11697B70)){auto object=Read<Address>(at);if(object && Read<Address>(object+0x18)==package)used.insert(Fold(NameOf(object)));}
    struct Planned { std::string name; Vector location; Rotation rotation; float scale; };
    std::vector<Planned> planned;
    std::string t3d="Begin Map\n";
    int serial=0;
    for(const auto& p:props)
    {
        std::string name;
        do name=kScatterPrefix+std::to_string(serial++); while(used.count(Fold(name)));
        used.insert(Fold(name));
        Planned item{name,p.at("location").get<Vector>(),p.at("rotation").get<Rotation>(),static_cast<float>(p.at("scale").get<double>())};
        Place::Check(item.location,"A prop would leave the world.");
        if(!std::isfinite(item.scale) || item.scale<=0)throw std::runtime_error("Invalid prop scale.");
        t3d+="Begin Actor Class=Engine.StaticMeshActor Name="+name+"\n"
             "    StaticMesh=StaticMesh'"+meshes[p.at("path").get<std::string>()]+"'\n"
             "    Location="+Workflow::VectorText(item.location)+"\n"
             "    Rotation="+Workflow::RotationText(item.rotation)+"\n"
             "    DrawScale="+std::to_string(item.scale)+"\n"
             "End Actor\n";
        planned.push_back(std::move(item));
    }
    t3d+="End Map\n";
    const auto before=SelectedIdentities();
    const std::string text=ConvertText(t3d,CP_UTF8,CP_ACP);
    Json created=Json::array();
    struct Scope{~Scope(){insertionText=nullptr;}} scope;
    try
    {
        // Static meshes collide: the collision hash is set aside while they
        // are pasted and moved, and every colliding actor is filed again
        // where it now stands afterwards (StairSmoothNative.inl).
        DetachedCollisionHash hash;
        Transaction transaction("Scatter props");
        Select(Json::array());
        insertionText=text.c_str();
        // Verified edactPasteSelected, as PlacementCopy uses it: the pasted
        // actors come in selected and recentred; their poses go back below.
        Call(Engine(),0x26c,reinterpret_cast<void*>(Level()),0);
        insertionText=nullptr;
        std::map<std::string,Address> pasted;
        for(auto actor:LiveActors())if(Read<unsigned>(actor+0x2f4)&0x40)pasted[Fold(NameOf(actor))]=actor;
        if(pasted.size()!=planned.size())throw std::runtime_error("The editor pasted "+std::to_string(pasted.size())+" props instead of "+std::to_string(planned.size())+"; nothing was placed.");
        ScatterShowGroup();
        for(const auto& item:planned)
        {
            auto found=pasted.find(Fold(item.name));
            if(found==pasted.end())throw std::runtime_error("The editor did not paste "+item.name+"; nothing was placed.");
            const auto actor=found->second;
            if(!IsA(actor,"StaticMeshActor"))throw std::runtime_error("The editor pasted "+item.name+" as something else; nothing was placed.");
            Modify(actor);
            SetPosition(actor,item.location);
            Write(Field(actor,"Rotation"),item.rotation);
            Write(Field(actor,"DrawScale"),item.scale);
            if(Property(actor,"Group"))DesignSetGroup(actor,kScatterGroup);
            Call(actor,0x44);
            // Shown, whatever the editor made of the new group.
            if(Read<unsigned>(actor+0x2f4)&0x10u){Logger::log("Scatter Props: "+item.name+" came in hidden; shown");Write(actor+0x2f4,Read<unsigned>(actor+0x2f4)&~0x10u);}
            created.push_back(Identity(actor));
        }
        transaction.Commit();
    }
    catch(...){insertionText=nullptr;try{Select(before);}catch(...){}throw;}
    Select(created);Redraw();
    return created;
}
