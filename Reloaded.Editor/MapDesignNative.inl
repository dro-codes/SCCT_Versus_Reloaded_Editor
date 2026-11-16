// Native operations for the Map Design workspace. All calls run on the UI thread.
namespace
{
    bool DesignBool(Address actor,const char* name)
    {
        auto p=Property(actor,name);return p && IsA(p,"BoolProperty") && (Read<unsigned>(actor+Read<int>(p+0x3c))&Read<unsigned>(p+0x64));
    }
    void DesignSetBool(Address actor,const char* name,bool value)
    {
        auto p=Property(actor,name);if(!p || !IsA(p,"BoolProperty"))throw std::runtime_error(std::string("Unavailable editor flag: ")+name);
        auto slot=actor+Read<int>(p+0x3c),mask=Read<unsigned>(p+0x64);auto flags=Read<unsigned>(slot);Write(slot,value?flags|mask:flags&~mask);
    }
    // Verified FName::FName(const char*, EFindName), NAME_Add=1, as used by
    // Tag renaming. Layer names reach the native Group field this way because
    // a group list separates its names with commas.
    int DesignName(const std::string& value)
    {
        int name=0;
        reinterpret_cast<void*(__thiscall*)(void*,const char*,int)>(0x10fb9610)(&name,value.c_str(),1);
        if(Fold(Name(name))!=Fold(value))throw std::runtime_error("The editor could not create the name: "+value);
        return name;
    }
    void DesignSetGroup(Address actor,const std::string& groups)
    {
        auto p=Property(actor,"Group");
        if(!p || !IsA(p,"NameProperty"))throw std::runtime_error("This editor's actors do not expose the native Group field.");
        Write(actor+Read<int>(p+0x3c),DesignName(groups));
    }
    // A fingerprint is what the toolkit decided about a brush and nothing the
    // editor changes on its own: the CSG operation, polygon flags, scale and
    // rotation lines, and the set of polygon vertices to a tenth of a unit.
    // Everything else is left out: Location and PrePivot (rounded and
    // rebalanced by the editor; moves are followed elsewhere), Group, hidden
    // and lock flags, polygon normals, texture axes, links and order (a
    // geometry build recomputes them). Normalizing twice gives the same text.
    std::string NormalizeBrushText(const std::string& text)
    {
        std::istringstream in(text);
        std::string line,properties;
        std::set<std::string> vertices;
        bool polygons=false,listed=false;
        while(std::getline(in,line))
        {
            while(!line.empty() && (line.back()=='\r' || line.back()==' '))line.pop_back();
            const auto start=line.find_first_not_of(" \t");
            const std::string trimmed=start==std::string::npos?std::string():line.substr(start);
            if(trimmed.rfind("Begin PolyList",0)==0){polygons=true;continue;}
            if(trimmed.rfind("End PolyList",0)==0){polygons=false;continue;}
            if(trimmed=="Vertices"){listed=true;continue;}
            if(listed){if(!trimmed.empty())vertices.insert(trimmed);continue;}
            if(polygons)
            {
                if(trimmed.rfind("Vertex",0)!=0)continue;
                std::string numbers=trimmed.substr(6),rounded;
                std::istringstream parts(numbers);
                std::string part;
                while(std::getline(parts,part,','))
                {
                    try
                    {
                        const double value=std::stod(part);
                        char buffer[32];
                        snprintf(buffer,sizeof(buffer),"%.1f",std::abs(value)<.05?0.0:value);
                        rounded+=std::string(rounded.empty()?"":",")+buffer;
                    }
                    catch(const std::exception&){rounded+=std::string(rounded.empty()?"":",")+part;}
                }
                vertices.insert(rounded);
                continue;
            }
            for(const char* keep:{"Begin Actor","CsgOper=","PolyFlags=","MainScale=","PostScale=","Rotation=","Begin Brush","End Brush","Brush=","End Actor"})
                if(trimmed.rfind(keep,0)==0){properties+=trimmed+"\r\n";break;}
        }
        std::string result=properties+"Vertices\r\n";
        for(const auto& v:vertices)result+=v+"\r\n";
        return result;
    }
    Json NormalizeFingerprint(const Json& fingerprint)
    {
        Json result={{"actors",Json::array()}};
        if(fingerprint.is_object() && fingerprint.contains("actors"))
            for(const auto& a:fingerprint["actors"])
                if(a.is_object() && a.contains("text"))
                    result["actors"].push_back({{"name",a.value("name",std::string())},{"class",a.value("class",std::string())},{"text",NormalizeBrushText(a.at("text").get<std::string>())}});
        return result;
    }
    // Why two fingerprints differ, for the error that says a piece was edited
    // outside the toolkit.
    std::string FingerprintDifference(const Json& stored,const Json& fresh)
    {
        const auto& a=stored.at("actors");const auto& b=fresh.at("actors");
        if(a.size()!=b.size())return " ("+std::to_string(a.size())+" brushes recorded, "+std::to_string(b.size())+" found)";
        for(size_t i=0;i<a.size();++i)
        {
            if(a[i].value("class",std::string())!=b[i].value("class",std::string()))return " (brush "+std::to_string(i+1)+" changed class)";
            std::istringstream was(a[i].value("text",std::string())),now(b[i].value("text",std::string()));
            std::string wasLine,nowLine;
            while(true)
            {
                const bool moreWas=static_cast<bool>(std::getline(was,wasLine)),moreNow=static_cast<bool>(std::getline(now,nowLine));
                if(!moreWas && !moreNow)break;
                if(!moreWas)wasLine.clear();
                if(!moreNow)nowLine.clear();
                while(!wasLine.empty() && wasLine.back()=='\r')wasLine.pop_back();
                while(!nowLine.empty() && nowLine.back()=='\r')nowLine.pop_back();
                if(wasLine!=nowLine)return " (brush "+std::to_string(i+1)+": recorded '"+wasLine+"', found '"+nowLine+"')";
            }
        }
        return "";
    }
    Json DesignFingerprint(const Json& members)
    {
        const ActorIndex index;
        for(auto& id:members)if(!index.Resolve(id))throw std::runtime_error("A blockout brush is missing. Undo/redo or restore it before editing this piece.");
        index.Report(members.size(),"DesignFingerprint");
        return NormalizeFingerprint(CaptureAssembly(members,{}));
    }
}
Vector DesignGrid()
{
    auto grid=Read<std::array<float,3>>(Engine()+0x200);
    Vector result{grid[0],grid[1],grid[2]};
    for(auto& spacing:result)if(!std::isfinite(spacing) || spacing<0 || spacing>65536)spacing=0;
    return result;
}
namespace
{
    // What the design views know about one actor, apart from its polygons.
    Json DesignActorItem(Address actor)
    {
        auto item=Identity(actor);item["level"]=LevelIdentity();item["generation"]=MapGeneration();item["position"]=Position(actor);item["selected"]=(Read<unsigned>(actor+0x2f4)&0x40)!=0;
        item["hidden"]=(Read<unsigned>(actor+0x2f4)&0x10)!=0;item["locked"]=DesignBool(actor,"bLockLocation");
        item["group"]=Property(actor,"Group")?NameField(actor,"Group"):std::string("None");
        // Carved space, solid space and portals read differently in a plan.
        const bool brush=IsA(actor,"Brush");
        item["csg"]=brush?Read<unsigned char>(actor+0x34c):0;
        item["portal"]=brush && (Read<unsigned>(actor+0x344)&0x04000000u)!=0;
        item["volume"]=IsA(actor,"Volume") || IsA(actor,"ZoneInfo");
        item["mover"]=IsA(actor,"Mover");
        item["rotation"]=RotationOf(actor);
        // Lights and cameras are drawn as reach and view on the plan.
        if(IsA(actor,"Light"))
        {
            auto radius=Property(actor,"LightRadius"),brightness=Property(actor,"LightBrightness");
            auto number=[&](Address p)->double
            {
                if(IsA(p,"FloatProperty"))return Read<float>(actor+Read<int>(p+0x3c));
                return Read<unsigned char>(actor+Read<int>(p+0x3c));
            };
            if(radius && brightness)
                item["light"]={{"radius",number(radius)*25.0},{"brightness",static_cast<int>(std::clamp(number(brightness),0.0,255.0))}};
        }
        if(IsA(actor,"SBase.SCamNetwork"))
        {
            auto fov=Property(actor,"FOV");
            if(!fov)fov=Property(actor,"CamFOV");
            item["camera"]={{"fov",fov && IsA(fov,"FloatProperty")?static_cast<double>(Read<float>(actor+Read<int>(fov+0x3c))):60.0}};
        }
        return item;
    }
    // Each polygon of a brush in world space, handed to visit with its vertex
    // count. A polygon whose vertices are out of range is passed with sane
    // false, so the caller decides whether that fails the scene.
    template<class Visit> void DesignPolygons(Address actor,Json& item,Visit visit)
    {
        auto model=Read<Address>(actor+0x238),polys=model?Read<Address>(model+0x50):0;
        if(!polys)return;
        std::array<float,12> coords{};Call<void*>(actor,0xac,coords.data());
        std::array<Vector,16> vertices{};
        for(auto poly:Array(polys+0x28,0x14c))
        {
            auto count=Read<unsigned short>(poly+0x148);if(count<3 || count>16)continue;
            // Portals can also be flagged on individual polygons.
            if(Read<unsigned>(poly+0x140)&0x04000000u)item["portal"]=true;
            bool sane=true;
            for(int j=0;j<count;++j)
            {
                auto local=Read<std::array<float,3>>(poly+0x18+j*12);std::array<float,3> world{};
                reinterpret_cast<void*(__thiscall*)(void*,void*,const void*)>(0x10eb2ba0)(local.data(),world.data(),coords.data());
                vertices[j]={world[0],world[1],world[2]};
                for(auto n:vertices[j])if(!std::isfinite(n) || std::abs(n)>1000000)sane=false;
            }
            visit(vertices.data(),static_cast<int>(count),sane);
        }
    }
}
Json DesignScene()
{
    Json result=Json::array();size_t edges=0;auto live=LiveActors();
    for(size_t i=2;i<live.size();++i)
    {
        auto actor=live[i];if(IsA(actor,"Camera"))continue;
        auto item=DesignActorItem(actor);item["edges"]=Json::array();
        if(IsA(actor,"Brush"))
            DesignPolygons(actor,item,[&](const Vector* vertices,int count,bool)
            {
                for(int j=0;j<count;++j)Design::CheckVector(vertices[j]);
                for(int j=0;j<count;++j){if(++edges>150000)throw std::runtime_error("Design view exceeds 150,000 brush edges. Hide detail or use a smaller map.");item["edges"].push_back({vertices[j],vertices[(j+1)%count]});}
            });
        result.push_back(std::move(item));
    }
    return result;
}
Json DesignScene(std::vector<DesignWire>& wires,size_t& dropped)
{
    // About 25 MB of edges: far more than any map the stock editor can build,
    // and still a small part of the editor's address space.
    constexpr size_t budget=1000000;
    Json result=Json::array();
    std::vector<DesignWire> built;
    size_t edges=0;
    dropped=0;
    auto live=LiveActors();
    built.reserve(live.size());
    for(size_t i=2;i<live.size();++i)
    {
        auto actor=live[i];if(IsA(actor,"Camera"))continue;
        auto item=DesignActorItem(actor);
        DesignWire wire;
        if(IsA(actor,"Brush"))
        {
            DesignPolygons(actor,item,[&](const Vector* vertices,int count,bool sane)
            {
                if(!sane)return;
                for(int j=0;j<count;++j)
                {
                    const auto& v=vertices[j];
                    if(!wire.measured){wire.low=wire.high=v;wire.measured=true;}
                    else for(int axis=0;axis<3;++axis){wire.low[axis]=std::min(wire.low[axis],v[axis]);wire.high[axis]=std::max(wire.high[axis],v[axis]);}
                    const auto& w=vertices[(j+1)%count];
                    std::array<float,6> edge{static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2]),
                                             static_cast<float>(w[0]),static_cast<float>(w[1]),static_cast<float>(w[2])};
                    // One direction for both polygons that share an edge, so
                    // the copy can be found and dropped.
                    if(std::lexicographical_compare(edge.begin()+3,edge.end(),edge.begin(),edge.begin()+3))
                        for(int k=0;k<3;++k)std::swap(edge[k],edge[k+3]);
                    wire.edges.push_back(edge);
                }
            });
            std::sort(wire.edges.begin(),wire.edges.end());
            wire.edges.erase(std::unique(wire.edges.begin(),wire.edges.end()),wire.edges.end());
            if(edges+wire.edges.size()>budget){dropped+=wire.edges.size();wire.edges.clear();wire.edges.shrink_to_fit();}
            else{edges+=wire.edges.size();wire.edges.shrink_to_fit();}
        }
        item["brush"]=wire.measured;
        item["index"]=built.size();
        built.push_back(std::move(wire));
        result.push_back(std::move(item));
    }
    wires=std::move(built);
    return result;
}
namespace
{
    // One piece replaced or created inside an open transaction. The rotation
    // goes into the brush shape and the actor stays unrotated, so the geometry
    // build agrees with the wireframes and the design views. New pieces take
    // the texture browser's current material, like a native builder-brush
    // addition would, unless the spec names one (spec.material, a loaded
    // material's path), which then stays with the piece when it is rebuilt.
    std::string BlockoutMaterial(const Json& spec)
    {
        std::string material;
        if(spec.contains("material") && !spec.at("material").is_null())
        {
            if(!spec.at("material").is_string())throw std::runtime_error("A blockout material is a material path such as Package.Group.Name.");
            material=spec.at("material").get<std::string>();
            if(material.empty() || material.find_first_of("\"\r\n '")!=std::string::npos)throw std::runtime_error("A blockout material is a material path such as Package.Group.Name, without spaces or quotes.");
            const auto found=Find(material);
            if(!found || !IsA(found,"Material"))throw std::runtime_error("Load the blockout material first: "+material);
            material=Path(found);
            if(material.find_first_of("\"\r\n ")!=std::string::npos)throw std::runtime_error("The blockout material's path has spaces or quotes: "+material);
            return material;
        }
        try{material=CurrentAsset(false);}catch(const std::exception&){}
        if(material.find_first_of("\"\r\n ")!=std::string::npos)material.clear();
        return material;
    }
    Json PlaceBlockout(const Json& spec,const Pose& frame,const Json& previous)
    {
        const auto material=BlockoutMaterial(spec);
        Design::CheckVector(frame.position);auto definition=Design::Definition(spec,frame.rotation,material);
        auto prepared=PreparePlacement(definition,{frame.position,{}},"Design_"+Id().substr(0,12)+"_",LevelPath(),{});
        if(!pasteHookReady || insertionText)throw std::runtime_error("Native brush insertion is unavailable or busy.");
        if(!previous.is_null())
        {
            if(previous.at("map")!=AuthoringMapKey())throw std::runtime_error("This piece belongs to another map.");
            {
                const auto stored=NormalizeFingerprint(previous.at("fingerprint")),fresh=DesignFingerprint(previous.at("members"));
                if(fresh!=stored)throw std::runtime_error("This piece was edited outside the toolkit. Detach it for manual editing or undo those edits first."+FingerprintDifference(stored,fresh));
            }
            for(auto& id:previous.at("members"))if(DesignBool(ResolveIdentity(id),"bLockLocation"))throw std::runtime_error("Unlock this blockout layer first.");
        }
        // Regenerated brushes keep the group the old ones were in.
        std::string group;
        if(!previous.is_null())
            for(auto& id:previous.at("members"))
                if(auto a=ResolveIdentity(id);a && Property(a,"Group")){group=NameField(a,"Group");break;}
        Json members=Json::array();
        const auto text=prepared.at("t3d").get<std::string>();
        struct Scope{~Scope(){insertionText=nullptr;}} scope;
        if(!previous.is_null()) {Select(previous.at("members"));if(!Exec("ACTOR DELETE"))throw std::runtime_error("Could not replace the old blockout brushes.");}
        insertionText=text.c_str();Select(Json::array());Call(Engine(),0x26c,reinterpret_cast<void*>(Level()),0);
        for(auto& item:prepared.at("actors"))
        {
            auto a=Find(LevelPath()+"."+item.at("name").get<std::string>(),true);if(!a)throw std::runtime_error("Blockout creation failed.");
            Modify(a);
            // A pasted brush can inherit a stray pivot from the editor, which
            // shifts its polygons by that much in the world. The piece's own
            // frame is the pivot, so it starts from zero; the editor may then
            // round Location and park the remainder here, which keeps the
            // world position exact.
            try{Write(Field(a,"PrePivot"),std::array<float,3>{0,0,0});}catch(const std::exception&){}
            // The brush actor's own PolyFlags decide whether the BSP builder
            // treats it as semi-solid (a glide ramp) rather than cutting the
            // steps beneath it away; the pasted text's line does not take, so
            // the value is written here.
            {
                const auto& source=definition.at("actors")[members.size()].at("text").get<std::string>();
                if(const auto flagsAt=source.find("PolyFlags=");flagsAt!=std::string::npos)
                {
                    const unsigned flags=static_cast<unsigned>(std::strtoul(source.c_str()+flagsAt+10,nullptr,10));
                    try{Write(Field(a,"PolyFlags"),flags);}catch(const std::exception&){}
                }
            }
            SetPosition(a,item.at("position").get<Vector>());Write(Field(a,"Rotation"),item.at("rotation").get<Rotation>());Call(a,0x44);members.push_back(Identity(a));
            if(!group.empty() && Fold(group)!="none" && Property(a,"Group"))DesignSetGroup(a,group);
        }
        if(SelectedIdentities().size()!=members.size())throw std::runtime_error("Unexpected number of blockout brushes.");
        return {{"map",AuthoringMapKey()},{"spec",spec},{"position",frame.position},{"rotation",frame.rotation},{"members",members},{"fingerprint",DesignFingerprint(members)}};
    }
}
std::vector<Address> SensorVolumes(Address actor);
// Moves actors by an offset inside the caller's transaction: a group following
// one of its members. Locked actors stay; a motion sensor's volumes come along.
void FollowNow(const Json& members,const Vector& delta)
{
    if(!members.is_array() || members.empty())return;
    if(std::abs(delta[0])<1e-9 && std::abs(delta[1])<1e-9 && std::abs(delta[2])<1e-9)return;
    std::vector<Address> actors;const ActorIndex index;
    auto add=[&](Address a){if(std::find(actors.begin(),actors.end(),a)==actors.end())actors.push_back(a);};
    for(auto& id:members)
    {
        auto a=index.Resolve(id);
        if(!a || (Property(a,"bLockLocation") && DesignBool(a,"bLockLocation")))continue;
        add(a);
        for(auto volume:SensorVolumes(a))add(volume);
    }
    index.Report(members.size(),"FollowNow");
    for(auto a:actors){Modify(a);auto p=Position(a);for(int i=0;i<3;++i)p[i]+=delta[i];SetPosition(a,p);Call(a,0x44);}
}
Json DesignBlockout(const Json& spec,const Pose& frame,const Json& previous,const Json& followers,const Vector& delta)
{
    auto selection=SelectedIdentities();Json piece;
    try
    {
        Transaction transaction("Create or resize blockout");
        piece=PlaceBlockout(spec,frame,previous);
        FollowNow(followers,delta);
        transaction.Commit();
    }
    catch(...){Select(selection);throw;}
    Select(piece.at("members"));Redraw();
    return piece;
}
// Several pieces in one Undo step: a moved room takes its doorways with it.
Json DesignBlockoutBatch(const Json& items,const Json& followers,const Vector& delta)
{
    if(!items.is_array() || items.empty() || items.size()>64)throw std::runtime_error("Move between one and 64 pieces at a time.");
    auto selection=SelectedIdentities();Json pieces=Json::array();
    try
    {
        Transaction transaction("Move blockout pieces");
        for(auto& item:items)
            pieces.push_back(PlaceBlockout(item.at("spec"),{item.at("position").get<Vector>(),item.at("rotation").get<Rotation>()},item.value("previous",Json{})));
        FollowNow(followers,delta);
        transaction.Commit();
    }
    catch(...){Select(selection);throw;}
    Json members=Json::array();
    for(auto& piece:pieces)for(auto& member:piece.at("members"))members.push_back(member);
    Select(members);Redraw();
    return pieces;
}
void DesignAlign(const Json& scene,int axis,const std::string& mode,double spacing)
{
    std::vector<Vector> positions;std::vector<Address> actors;const ActorIndex index;
    for(auto& item:scene)
    {
        if(item.at("level").get<uintptr_t>()!=LevelIdentity() || item.at("generation").get<unsigned>()!=MapGeneration())throw std::runtime_error("The map changed. Refresh the design view.");
        auto a=index.Resolve(item);if(!a || Position(a)!=item.at("position").get<Vector>())throw std::runtime_error("Selection moved. Refresh the design view.");
        if(DesignBool(a,"bLockLocation"))throw std::runtime_error("Unlock selected actors before aligning.");
        actors.push_back(a);positions.push_back(Position(a));
    }
    index.Report(scene.size(),"DesignAlign");
    auto after=Design::Align(positions,axis,mode,spacing);Transaction transaction("Align or distribute actors");
    for(size_t i=0;i<actors.size();++i){Modify(actors[i]);SetPosition(actors[i],after[i]);Call(actors[i],0x44);}transaction.Commit();Redraw();
}
void DesignLayer(const Json& members,bool hidden,bool locked,const std::string& group,const std::string& groupAction)
{
    std::vector<Address> actors;auto live=LiveActors();const ActorIndex index;
    for(auto& id:members)
    {
        auto a=index.Resolve(id);if(!a)continue;
        if(a==live[0] || a==live[1] || IsA(a,"Camera"))throw std::runtime_error("Layers cannot contain editor infrastructure.");
        if(!Property(a,"bLockLocation"))throw std::runtime_error("Actor does not expose location locking.");actors.push_back(a);
    }
    index.Report(members.size(),"DesignLayer");
    if(groupAction!="none" && groupAction!="add" && groupAction!="remove")throw std::runtime_error("Invalid layer group action.");
    // Membership is written into the map's own Group field, so the layer
    // survives sharing the .sdc without the workspace file.
    // An editor build without the Group field still gets workspace layers.
    std::vector<std::string> groups;
    if(groupAction!="none")for(auto a:actors)
    {
        if(!Property(a,"Group")){groups.push_back({});continue;}
        auto current=NameField(a,"Group");
        groups.push_back(groupAction=="add"?Design::AddGroup(current,group):Design::RemoveGroup(current,group));
    }
    Transaction transaction("Change design layer membership, visibility and lock");
    for(size_t i=0;i<actors.size();++i)
    {
        auto a=actors[i];Modify(a);
        if(groupAction!="none" && !groups[i].empty())DesignSetGroup(a,groups[i]);
        DesignSetBool(a,"bLockLocation",locked);auto flags=Read<unsigned>(a+0x2f4);Write(a+0x2f4,(flags&~0x10u)|(hidden?0x10u:0));if(hidden||locked)Write(a+0x2f4,Read<unsigned>(a+0x2f4)&~0x40u);
    }
    transaction.Commit();Redraw();
}
// Where every actor stands, without the design scene's polygon edges: the
// storey filter needs one box per actor and nothing else, and a map far too
// heavy to draw as a plan is still light enough to sort into floors.
Json DesignActorSpans()
{
    Json result=Json::array();auto live=LiveActors();
    for(size_t i=2;i<live.size();++i)
    {
        auto actor=live[i];if(IsA(actor,"Camera"))continue;
        auto item=Identity(actor);
        const auto position=Position(actor);
        Vector low=position,high=position;
        bool measured=false;
        const bool brush=IsA(actor,"Brush");
        if(brush)
        {
            auto model=Read<Address>(actor+0x238),polys=model?Read<Address>(model+0x50):0;
            if(polys)
            {
                std::array<float,12> coords{};Call<void*>(actor,0xac,coords.data());
                for(auto poly:Array(polys+0x28,0x14c))
                {
                    auto count=Read<unsigned short>(poly+0x148);if(count<3 || count>16)continue;
                    for(int j=0;j<count;++j)
                    {
                        auto local=Read<std::array<float,3>>(poly+0x18+j*12);std::array<float,3> world{};
                        reinterpret_cast<void*(__thiscall*)(void*,void*,const void*)>(0x10eb2ba0)(local.data(),world.data(),coords.data());
                        const Vector v{world[0],world[1],world[2]};
                        bool sane=true;
                        for(auto n:v)if(!std::isfinite(n) || std::abs(n)>1000000)sane=false;
                        if(!sane)continue;
                        if(!measured){low=high=v;measured=true;continue;}
                        for(int axis=0;axis<3;++axis){low[axis]=std::min(low[axis],v[axis]);high[axis]=std::max(high[axis],v[axis]);}
                    }
                }
            }
        }
        item["low"]=low;item["high"]=high;item["box"]=measured;
        item["csg"]=brush?Read<unsigned char>(actor+0x34c):0;
        item["portal"]=brush && (Read<unsigned>(actor+0x344)&0x04000000u)!=0;
        item["hidden"]=(Read<unsigned>(actor+0x2f4)&0x10)!=0;
        result.push_back(item);
    }
    return result;
}
// Hides one set of actors and shows another, which is what changing storey
// does: the floor being left goes, the floor arrived at comes back. This is a
// view, like a geometry build putting hidden actors back (RestoreHiddenActors),
// so it is not an Undo step: the Storeys palette re-applies its floor whenever
// the map changes, and as a transaction that re-apply after an Undo dropped the
// step just undone, so Undo could never get past it and Redo never worked.
size_t DesignSetHidden(const Json& hide,const Json& show)
{
    std::vector<Address> hiding,showing;
    auto live=LiveActors();
    // Storeys re-applies its floor on every map change, often with nothing to do.
    if(hide.empty() && show.empty())return 0;
    const ActorIndex index;
    auto gather=[&](const Json& members,std::vector<Address>& into)
    {
        for(const auto& id:members)
        {
            auto a=index.Resolve(id);
            if(!a || a==live[0] || a==live[1] || IsA(a,"Camera"))continue;
            into.push_back(a);
        }
    };
    gather(hide,hiding);gather(show,showing);
    index.Report(hide.size()+show.size(),"DesignSetHidden");
    if(hiding.empty() && showing.empty())return 0;
    size_t changed=0,deselected=0;
    for(auto a:hiding)
    {
        const auto flags=Read<unsigned>(a+0x2f4);
        if(flags&0x10u)continue;
        if(flags&0x40u)++deselected;
        Write(a+0x2f4,(flags|0x10u)&~0x40u);++changed;
    }
    for(auto a:showing)
    {
        const auto flags=Read<unsigned>(a+0x2f4);
        if(!(flags&0x10u))continue;
        Write(a+0x2f4,flags&~0x10u);++changed;
    }
    if(deselected)Call(Engine(),0xe4);
    if(changed)Redraw();
    return changed;
}
// Turns actors about a point, in one Undo step: each one's yaw moves by the
// same angle and its position swings round the pivot, so a selection keeps its
// shape. A brush turns with its actor, which is what the engine builds from,
// so the geometry follows on the next build.
size_t DesignTurnActors(const Json& members,double degrees,const Vector& pivot)
{
    if(!std::isfinite(degrees))throw std::runtime_error("Enter a finite angle.");
    Design::CheckVector(pivot);
    std::vector<Address> actors;auto live=LiveActors();const ActorIndex index;
    for(auto& id:members)
    {
        auto a=index.Resolve(id);
        if(!a || a==live[0] || a==live[1] || IsA(a,"Camera"))continue;
        if(DesignBool(a,"bLockLocation"))continue;
        actors.push_back(a);
    }
    index.Report(members.size(),"DesignTurnActors");
    if(actors.empty())return 0;
    const double radians=degrees*3.14159265358979323846/180,cosine=std::cos(radians),sine=std::sin(radians);
    const int step=static_cast<int>(std::lround(degrees*65536/360));
    Transaction transaction("Turn actors");
    for(auto a:actors)
    {
        Modify(a);
        auto at=Position(a);
        const double dx=at[0]-pivot[0],dy=at[1]-pivot[1];
        at[0]=pivot[0]+dx*cosine-dy*sine;
        at[1]=pivot[1]+dx*sine+dy*cosine;
        Design::CheckVector(at);
        SetPosition(a,at);
        if(auto field=Field(a,"Rotation"))
        {
            auto rotation=Read<Rotation>(field);
            rotation[1]=((rotation[1]+step)%65536+65536)%65536;
            Write(field,rotation);
        }
        Call(a,0x44);
    }
    transaction.Commit();Redraw();
    return actors.size();
}
// Hides/shows and locks/unlocks actors; -1 leaves a flag as it is. Hidden or
// locked actors leave the selection so nothing moves them by accident.
void DesignSetFlags(const Json& members,int hidden,int locked)
{
    std::vector<Address> actors;auto live=LiveActors();const ActorIndex index;
    // Actors already as asked are left out, so isolating a map that is mostly
    // hidden already puts only what changes into the Undo buffer, and asking
    // again for what is already so leaves no Undo step at all. The flag bits
    // go first; the lock walks the class's properties, so it is read once.
    auto settled=[&](Address a)
    {
        const auto flags=Read<unsigned>(a+0x2f4);
        if((hidden>0 || locked>0) && (flags&0x40u))return false;
        if(hidden>=0 && ((flags&0x10u)!=0)!=(hidden!=0))return false;
        if(locked<0)return true;
        auto p=Property(a,"bLockLocation");
        return !p || (IsA(p,"BoolProperty") && (Read<unsigned>(a+Read<int>(p+0x3c))&Read<unsigned>(p+0x64)))==(locked!=0);
    };
    for(auto& id:members){auto a=index.Resolve(id);if(!a || a==live[0] || a==live[1] || settled(a) || IsA(a,"Camera"))continue;actors.push_back(a);}
    index.Report(members.size(),"DesignSetFlags");
    if(actors.empty())return;
    Transaction transaction(locked>0?"Lock actors":locked==0?"Unlock actors":hidden>0?"Hide actors":"Show actors");
    for(auto a:actors)
    {
        Modify(a);
        if(locked>=0 && Property(a,"bLockLocation"))DesignSetBool(a,"bLockLocation",locked!=0);
        if(hidden>=0){auto flags=Read<unsigned>(a+0x2f4);Write(a+0x2f4,(flags&~0x10u)|(hidden?0x10u:0));}
        if(hidden>0 || locked>0)Write(a+0x2f4,Read<unsigned>(a+0x2f4)&~0x40u);
    }
    transaction.Commit();Redraw();
}
// Adds actors to, or removes them from, a named group in the map's own Group
// field, so the group travels with the .sdc.
void DesignGroupMembers(const Json& members,const std::string& group,const std::string& action)
{
    if(action!="add" && action!="remove")throw std::runtime_error("Invalid group action.");
    if(action=="add" && !Design::ValidGroupName(group))throw std::runtime_error("Use 1-62 letters, digits or underscores for a group that travels with the map.");
    std::vector<std::pair<Address,std::string>> changes;const ActorIndex index;
    for(auto& id:members)
    {
        auto a=index.Resolve(id);if(!a || !Property(a,"Group"))continue;
        auto current=NameField(a,"Group");
        auto next=action=="add"?Design::AddGroup(current,group):Design::RemoveGroup(current,group);
        if(next!=current)changes.push_back({a,next});
    }
    index.Report(members.size(),"DesignGroupMembers");
    if(changes.empty())return;
    Transaction transaction(action=="add"?"Add actors to group":"Remove actors from group");
    for(auto& [a,next]:changes){Modify(a);DesignSetGroup(a,next);}
    transaction.Commit();Redraw();
}
// Moves actors by an offset in one Undo step: a group following one of its
// members. Locked actors stay.
void DesignTranslate(const Json& members,const Vector& delta)
{
    if(!members.is_array() || members.empty())return;
    // No empty Undo step when everything asked for is locked or gone.
    bool movable=false;const ActorIndex index;
    for(auto& id:members){auto a=index.Resolve(id);if(a && !(Property(a,"bLockLocation") && DesignBool(a,"bLockLocation")))movable=true;}
    index.Report(members.size(),"DesignTranslate");
    if(!movable)return;
    Transaction transaction("Move group");
    FollowNow(members,delta);
    transaction.Commit();Redraw();
}
// A lift: an SLift mover whose platform brush rises by `rise` when a pawn
// stands on it and comes back after a pause. One Undo step; the new actor is
// selected. The platform is centred on `position`.
Json CreateLift(const Vector& position,double width,double length,double thickness,double rise,double moveTime)
{
    Design::CheckVector(position);
    if(width<32 || width>2048 || length<32 || length>2048 || thickness<4 || thickness>256)throw std::runtime_error("Lift platforms are 32 to 2048 units wide and long and 4 to 256 thick.");
    if(!std::isfinite(rise) || std::abs(rise)<8 || std::abs(rise)>8192)throw std::runtime_error("A lift rises between 8 and 8192 units.");
    if(!std::isfinite(moveTime) || moveTime<.1 || moveTime>60)throw std::runtime_error("Lift travel takes 0.1 to 60 seconds.");
    if(!pasteHookReady || insertionText)throw std::runtime_error("Native actor insertion is unavailable or busy.");
    std::string name="Design_Lift";
    for(int suffix=2;Find(LevelPath()+"."+name);++suffix)name="Design_Lift_"+std::to_string(suffix);
    std::ostringstream numbers;numbers<<std::fixed<<std::setprecision(6);
    numbers<<"MoveTime="<<moveTime<<"\r\nStayOpenTime=3.000000\r\nNumKeys=2\r\nKeyPos(1)=(X=0.000000,Y=0.000000,Z="<<rise<<")\r\n";
    // DrawType and Physics are stated because the class default draws the
    // actor as a sprite, which shows in game.
    const std::string text="Begin Map\r\nBegin Actor Class=SBase.SLift Name="+name+"\r\nLocation="+VectorText(position)+"\r\nDrawType=DT_Brush\r\nPhysics=PHYS_MovingBrush\r\nInitialState=StandOpenTimed\r\n"+numbers.str()
        +"Begin Brush Name="+name+"Model\r\n"+Magic::BoxPolygons(width/2,length/2,thickness/2)+"End Brush\r\nBrush=Model'"+LevelPath()+"."+name+"Model'\r\nEnd Actor\r\nEnd Map\r\n";
    struct Scope{~Scope(){insertionText=nullptr;}} scope;
    auto selection=SelectedIdentities();
    Address actor=0;
    try
    {
        Transaction transaction("Create lift");
        insertionText=text.c_str();Select(Json::array());Call(Engine(),0x26c,reinterpret_cast<void*>(Level()),0);
        insertionText=nullptr;
        actor=Find(LevelPath()+"."+name,true);
        if(!actor || SelectedIdentities().size()!=1)throw std::runtime_error("Lift creation failed.");
        Modify(actor);
        try{Write(Field(actor,"PrePivot"),std::array<float,3>{0,0,0});}catch(const std::exception&){}
        SetPosition(actor,position);Call(actor,0x44);
        transaction.Commit();
    }
    catch(...){Select(selection);throw;}
    Redraw();
    return Identity(actor);
}
// Brush actors' own PolyFlags, which decide semi-solid and invisible for the
// BSP build; a missing actor reads as null.
Json DesignPolyFlags(const Json& members)
{
    Json result=Json::array();const ActorIndex index;
    for(auto& id:members)
    {
        auto a=index.Resolve(id);
        if(!a){result.push_back(nullptr);continue;}
        try{result.push_back(Read<unsigned>(Field(a,"PolyFlags")));}
        catch(const std::exception&){result.push_back(nullptr);}
    }
    index.Report(members.size(),"DesignPolyFlags");
    return result;
}
// Sets brush actors' PolyFlags in one Undo step; null leaves one alone.
size_t DesignSetPolyFlags(const Json& members,const Json& flags)
{
    if(!members.is_array() || !flags.is_array() || members.size()!=flags.size())throw std::runtime_error("One flags value per brush.");
    std::vector<std::pair<Address,unsigned>> changes;const ActorIndex index;
    for(size_t i=0;i<members.size();++i)
    {
        if(flags[i].is_null())continue;
        auto a=index.Resolve(members[i]);
        if(!a)continue;
        try{if(Read<unsigned>(Field(a,"PolyFlags"))!=flags[i].get<unsigned>())changes.push_back({a,flags[i].get<unsigned>()});}
        catch(const std::exception&){}
    }
    index.Report(members.size(),"DesignSetPolyFlags");
    if(changes.empty())return 0;
    Transaction transaction("Repair brush flags");
    for(auto& [a,value]:changes){Modify(a);Write(Field(a,"PolyFlags"),value);Call(a,0x44);}
    transaction.Commit();Redraw();
    return changes.size();
}
// Moves brushes to the end of the level's actor list, so the CSG build adds
// them after every carve, including one placed later over them. One Undo step;
// nothing happens when they are already last in this order.
size_t DesignSendToLast(const Json& members)
{
    std::vector<Address> actors;const ActorIndex lookup;
    for(auto& id:members)if(auto a=lookup.Resolve(id))actors.push_back(a);
    lookup.Report(members.size(),"DesignSendToLast");
    if(actors.empty())return 0;
    auto level=Level();
    auto data=Read<Address>(level+0x2c);const int count=Read<int>(level+0x30);
    if(!data || count<static_cast<int>(actors.size()))return 0;
    bool ordered=true;
    for(size_t i=0;i<actors.size();++i)if(Read<Address>(data+(count-actors.size()+i)*4)!=actors[i])ordered=false;
    if(ordered)return 0;
    Transaction transaction("Send brushes to last");
    Modify(level);
    for(auto actor:actors)
    {
        int index=-1;
        for(int i=0;i<count;++i)if(Read<Address>(data+i*4)==actor){index=i;break;}
        if(index<0)continue;
        for(int i=index;i+1<count;++i)Write(data+i*4,Read<Address>(data+(i+1)*4));
        Write(data+(count-1)*4,actor);
    }
    transaction.Commit();Redraw();
    return actors.size();
}
std::string StartTeam(Address actor);
Json DesignSpawns()
{
    Json result=Json::array();for(auto a:LiveActors())if(IsA(a,"PlayerStart"))
    {
        auto item=Identity(a);item["position"]=Position(a);item["rotation"]=RotationOf(a);
        item["team"]=StartTeam(a);result.push_back(item);
    }
    return result;
}
Json DesignClearances()
{
    Json profiles=Json::array();
    for(const auto& entry:EventAssets("Engine.Pawn",true))
    {
        auto type=entry.at("path").get<std::string>();auto folded=Fold(type);
        if(folded.find("spy")==std::string::npos && folded.find("merc")==std::string::npos)continue;
        auto value=[&](const char* property)->double
        {
            TextOutput output;auto command="GET "+type+" "+property;
            if(!Call<int>(Engine()+0x28,0,command.c_str(),&output))throw std::runtime_error("Class default lookup unavailable.");
            auto text=output.text;auto end=text.find_last_not_of(" \r\n\t");if(end==std::string::npos)throw std::runtime_error("Empty class default.");text.resize(end+1);
            return Design::Number(text,1,10000);
        };
        auto optional=[&](const char* property)->Json
        {
            try{return value(property);}catch(const std::exception&){return Json{};}
        };
        auto team=folded.find("spy")!=std::string::npos?"Spy":"Merc";
        try
        {
            double radius=value("CollisionRadius"),height=value("CollisionHeight");
            // Movement limits are reported when a class exposes them; the
            // workspace keeps editable values either way.
            Json speed=optional("GroundSpeed"),step=optional("MaxStepHeight");
            Json standing={{"name",type+" standing"},{"width",radius*2},{"height",height*2},{"team",team}};
            if(!speed.is_null())standing["speed"]=speed;
            if(!step.is_null())standing["stepHeight"]=step;
            profiles.push_back(standing);
            try
            {
                Json crouching={{"name",type+" crouching"},{"width",value("CrouchRadius")*2},{"height",value("CrouchHeight")*2},{"team",team}};
                if(!speed.is_null())crouching["speed"]=speed;
                profiles.push_back(crouching);
            }
            catch(const std::exception&){}
        }
        catch(const std::exception&) { /* No guessed collision presets. */ }
    }
    return profiles;
}
// A map without a start for the chosen team gets one for the playtest only.
// The creation is a single transaction that is undone as soon as Play Level
// returns, so the saved map keeps exactly the spawns the author placed.
// TeamNumber is not reachable through reflection on this editor's PlayerStart,
// though the pasted text sets it and the exporter writes it. When reflection
// does find it, it is written directly as well.
void WriteTeamNumber(Address actor,const std::string& team)
{
    if(team.empty())return;
    auto p=Property(actor,"TeamNumber");
    if(!p)return;
    const int value=std::stoi(team);
    const auto at=actor+Read<int>(p+0x3c);
    if(IsA(p,"ByteProperty"))Write(at,static_cast<unsigned char>(value));
    else Write(at,value);
}
// The team of a start: from reflection when it exposes TeamNumber, otherwise
// from the actor's own exported text, where the engine writes it.
std::string StartTeam(Address actor)
{
    if(auto p=Property(actor,"TeamNumber"))
    {
        auto value=MagicValue(p,actor+Read<int>(p+0x3c));
        return value.is_string()?value.get<std::string>():value.dump();
    }
    try
    {
        auto captured=CaptureAssembly(Json::array({Identity(actor)}),{});
        for(const auto& entry:captured.at("actors"))
        {
            const auto text=entry.value("text",std::string());
            std::smatch match;
            if(std::regex_search(text,match,std::regex("(?:^|\\n)\\s*TeamNumber=([0-9]+)")))return match[1].str();
        }
    }
    catch(const std::exception&) { /* Fall back to the default team. */ }
    return "0";
}
Json DesignTemporaryStart(const std::string& type,const std::string& team,const Pose& pose)
{
    Design::CheckVector(pose.position);
    if(!std::regex_match(type,std::regex("[A-Za-z_][A-Za-z0-9_]{0,62}\\.[A-Za-z_][A-Za-z0-9_]{0,62}")))throw std::runtime_error("Choose a PlayerStart class.");
    if(!team.empty() && !std::regex_match(team,std::regex("[A-Za-z0-9_]{1,63}")))throw std::runtime_error("Invalid team value.");
    if(!pasteHookReady || insertionText)throw std::runtime_error("Native actor insertion is unavailable or busy.");
    std::string name="DesignPlaytestStart";
    for(int suffix=2;Find(LevelPath()+"."+name);++suffix)name="DesignPlaytestStart_"+std::to_string(suffix);
    auto text="Begin Map\r\nBegin Actor Class="+type+" Name="+name+"\r\nLocation="+VectorText(pose.position)+"\r\nRotation="+RotationText(pose.rotation)+"\r\n"
        +(team.empty()?"":"TeamNumber="+team+"\r\n")+"End Actor\r\nEnd Map\r\n";
    auto before=SelectedIdentities();
    struct Scope{Scope(const char* value){if(insertionText)throw std::runtime_error("Actor insertion is busy.");insertionText=value;}~Scope(){insertionText=nullptr;}} scope(text.c_str());
    Json identity;
    try
    {
        Transaction transaction("Temporary playtest start");
        Select(Json::array());Call(Engine(),0x26c,reinterpret_cast<void*>(Level()),0);
        auto actor=Find(LevelPath()+"."+name,true);
        if(!actor || SelectedIdentities().size()!=1)throw std::runtime_error("Could not create a temporary team spawn.");
        if(!IsA(actor,"PlayerStart"))throw std::runtime_error("That class is not a PlayerStart.");
        Modify(actor);SetPosition(actor,pose.position);Write(Field(actor,"Rotation"),pose.rotation);WriteTeamNumber(actor,team);Call(actor,0x44);
        identity=Identity(actor);
        transaction.Commit();
    }
    catch(...){Select(before);throw;}
    Select(before);
    // Return the spawn entry itself, so playtesting can use it like any other.
    for(auto& spawn:DesignSpawns())if(spawn.at("path")==identity.at("path"))return spawn;
    throw std::runtime_error("The temporary team spawn did not register as a player start.");
}
void DesignRemoveTemporaryStart(const Json& identity)
{
    if(!ResolveIdentity(identity))return;
    Exec("TRANSACTION UNDO");
    if(!ResolveIdentity(identity))return;
    // Undo could not reach the creation; delete the actor directly instead.
    auto previous=SelectedIdentities();
    Select(Json::array({identity}));
    if(!Exec("ACTOR DELETE"))throw std::runtime_error("Remove the temporary playtest spawn manually: "+identity.at("path").get<std::string>());
    Select(previous);
}
void DesignPlay(const Json& start,const Pose& pose,bool launch)
{
    Design::CheckVector(pose.position);auto available=DesignSpawns();
    if(std::find(available.begin(),available.end(),start)==available.end())throw std::runtime_error("Spawn changed. Select a current team spawn.");
    // Native Play Level serializes a temporary map synchronously. Move only the
    // chosen team's starts during serialization, then restore exact live values.
    std::vector<std::pair<Address,Pose>> originals;
    for(auto& s:available)if(s.at("team")==start.at("team"))originals.push_back({ResolveIdentity(s),{s.at("position").get<Vector>(),s.at("rotation").get<Rotation>()}});
    struct Restore{std::vector<std::pair<Address,Pose>>& values;~Restore(){for(auto& [a,p]:values){try{SetPosition(a,p.position);Write(Field(a,"Rotation"),p.rotation);}catch(...){}}}} restore{originals};
    for(auto& [a,p]:originals){SetPosition(a,pose.position);Write(Field(a,"Rotation"),pose.rotation);}
    if(launch)PlayLevel();
}
