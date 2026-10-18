// Included last inside Workflow::Editor (WorkflowEditor.cpp): the editor side
// of the MCP bridge's ops (McpEditor.h, McpOps.cpp). UI thread only.

// One real Transaction for a whole plan. The outermost scope owns it; a scope
// opened inside another joins it, as the Transactions of the ops do.
JoinScope::JoinScope(const std::string& label)
{
    if(joinDepth>0){++joinDepth;return;}
    const auto trans=Read<Address>(Engine()+0x148);
    if(!trans || Read<int>(trans+0x44)!=0)throw std::runtime_error("Finish the current editor operation first.");
    Call(trans,0x64,TransactionLabel(label.c_str()));
    buffer=trans;owner=true;joinDepth=1;
}
void JoinScope::Commit()
{
    if(committed)return;
    committed=true;
    if(joinDepth>0)--joinDepth;
    if(owner)Call(buffer,0x68);
}
// Not committed: an op failed, so everything the plan changed is taken back
// in one Undo, with no Redo left to replay it.
JoinScope::~JoinScope()
{
    if(committed)return;
    if(joinDepth>0)--joinDepth;
    if(!owner)return;
    try{RollBack(buffer);}
    catch(const std::exception& e){Logger::log(std::string("MCP: could not roll the plan back: ")+e.what());}
}
bool Joining() { return joinDepth>0; }
void SetTransactionPrefix(const std::string& prefix) { transactionPrefix=prefix; }
std::string AuthoringMap() { return AuthoringMapKey(); }
// The undo buffer as UndoHistory.cpp reads it: FTransaction records (0x28
// each, Begin's stamp at +4) at +0x28, Num +0x2c, the newest UndoCount (+0x34)
// of them undone, ActiveCount +0x44.
UndoState UndoRecords()
{
    UndoState state;
    const auto trans=Read<Address>(Engine()+0x148);
    if(!trans)return state;
    const auto data=Read<Address>(trans+0x28);
    const int count=Read<int>(trans+0x2c),undone=Read<int>(trans+0x34);
    if(count<0 || count>1000000 || undone<0 || undone>count)throw std::runtime_error("The editor's undo buffer cannot be read.");
    state.stamps.reserve(static_cast<size_t>(count));
    for(int i=0;i<count;++i)state.stamps.push_back(Read<unsigned>(data+static_cast<Address>(i)*0x28+4));
    state.undone=undone;state.open=Read<int>(trans+0x44)!=0;
    return state;
}
bool Exec(const std::string& command,std::string& output)
{
    auto e=Engine();TextOutput device;
    const auto text=ConvertText(command,CP_UTF8,CP_ACP);
    const bool ok=Call<int>(e+0x28,0,text.c_str(),static_cast<void*>(&device))!=0;
    output=device.overflow?std::string("(output over 64 MiB left out)"):ConvertText(device.text,CP_ACP,CP_UTF8);
    return ok;
}
namespace
{
    // What actors.query reads of a class, found once: a map holds thousands
    // of actors of a few hundred classes. Properties by folded name, the most
    // derived first, as Property finds them.
    struct McpClass { std::string path; bool skip=false,wanted=true; int tag=-1,event=-1,rotation=-1; std::map<std::string,Address> fields; };
    std::vector<std::string> McpNames(const Json& query,const char* key)
    {
        std::vector<std::string> out;
        if(!query.contains(key) || query.at(key).is_null())return out;
        const auto& list=query.at(key);
        if(list.is_string()){out.push_back(Fold(list.get<std::string>()));return out;}
        if(!list.is_array())throw std::runtime_error(std::string(key)+" is a list of names.");
        for(const auto& item:list)out.push_back(Fold(item.get<std::string>()));
        return out;
    }
    McpClass McpClassOf(Address actor,Address type,const std::vector<std::string>& classes)
    {
        McpClass c;c.path=Path(type);
        c.skip=IsA(actor,"Camera") || IsA(actor,"LevelInfo");
        if(!classes.empty())
        {
            c.wanted=false;
            for(const auto& name:classes)if(IsA(actor,name)){c.wanted=true;break;}
        }
        for(auto p:Properties(type))c.fields.emplace(Fold(NameOf(p)),p);
        auto offset=[&](const char* name,const char* kind)
        {
            const auto found=c.fields.find(Fold(name));
            return found!=c.fields.end() && IsA(found->second,kind)?Read<int>(found->second+0x3c):-1;
        };
        c.tag=offset("Tag","NameProperty");c.event=offset("Event","NameProperty");c.rotation=offset("Rotation","StructProperty");
        return c;
    }
}
// actors.query: the live actors (never the LevelInfo, the builder brush or a
// camera) that pass every filter given, a page at a time, in level order.
Json QueryActors(const Json& query)
{
    const auto classes=McpNames(query,"classes"),tagList=McpNames(query,"tags"),nameList=McpNames(query,"names");
    const std::set<std::string> tags(tagList.begin(),tagList.end()),names(nameList.begin(),nameList.end());
    std::optional<Vector> nearAt;double radius=0;
    if(query.contains("near") && !query.at("near").is_null())
    {
        nearAt=query.at("near").at("at").get<Vector>();radius=query.at("near").at("radius").get<double>();
        Design::CheckVector(*nearAt);
        if(!std::isfinite(radius) || radius<0)throw std::runtime_error("near.radius is a distance of 0 or more.");
    }
    std::optional<std::pair<Vector,Vector>> box;
    if(query.contains("box") && !query.at("box").is_null())
    {
        auto low=query.at("box").at("min").get<Vector>(),high=query.at("box").at("max").get<Vector>();
        for(int k=0;k<3;++k)if(low[k]>high[k])std::swap(low[k],high[k]);
        box.emplace(low,high);
    }
    std::optional<bool> selected;
    if(query.contains("selected") && query.at("selected").is_boolean())selected=query.at("selected").get<bool>();
    bool all=false;std::vector<std::string> fields;
    if(query.contains("fields") && !query.at("fields").is_null())
    {
        const auto& given=query.at("fields");
        if(given.is_string())
        {
            if(given.get<std::string>()!="all")throw std::runtime_error("fields is \"all\" or a list of property names.");
            all=true;
        }
        else if(given.is_array()){for(const auto& field:given)fields.push_back(field.get<std::string>());}
        else throw std::runtime_error("fields is \"all\" or a list of property names.");
    }
    const int limit=query.value("limit",500),cursor=query.value("cursor",0);
    if(limit<1 || limit>5000)throw std::runtime_error("limit is 1 to 5000 actors.");
    if(cursor<0)throw std::runtime_error("cursor is the next value of an earlier page, 0 or more.");
    auto live=LiveActors();std::map<Address,McpClass> types;
    size_t total=0,unreadable=0;Json actors=Json::array();
    for(size_t i=2;i<live.size();++i)
    {
        const auto a=live[i];
        try
        {
            const auto type=Read<Address>(a+0x24);
            auto found=types.find(type);
            if(found==types.end())found=types.emplace(type,McpClassOf(a,type,classes)).first;
            const auto& c=found->second;
            if(c.skip || !c.wanted)continue;
            const auto flags=Read<unsigned>(a+0x2f4);const bool isSelected=(flags&0x40)!=0;
            if(selected && *selected!=isSelected)continue;
            const auto tag=c.tag>=0?Name(Read<int>(a+c.tag)):std::string("None");
            if(!tags.empty() && !tags.count(Fold(tag)))continue;
            const auto name=NameOf(a);std::string path;
            if(!names.empty())
            {
                path=Path(a);
                if(!names.count(Fold(name)) && !names.count(Fold(path)))continue;
            }
            const auto position=Position(a);
            if(nearAt)
            {
                double distance=0;for(int k=0;k<3;++k)distance+=(position[k]-(*nearAt)[k])*(position[k]-(*nearAt)[k]);
                if(distance>radius*radius)continue;
            }
            if(box)
            {
                bool inside=true;for(int k=0;k<3;++k)inside=inside && position[k]>=box->first[k] && position[k]<=box->second[k];
                if(!inside)continue;
            }
            ++total;
            if(total<=static_cast<size_t>(cursor) || actors.size()>=static_cast<size_t>(limit))continue;
            if(path.empty())path=Path(a);
            Json item={{"path",path},{"class",c.path},{"name",name},{"tag",tag},{"event",c.event>=0?Name(Read<int>(a+c.event)):std::string("None")},
                       {"position",position},{"rotation",c.rotation>=0?Read<Rotation>(a+c.rotation):Rotation{}},{"selected",isSelected},{"hidden",(flags&0x10)!=0}};
            if(all || !fields.empty())
            {
                // InspectActor's encoding; a value that cannot be read is left out.
                Json values=Json::object();
                if(all)
                {
                    for(const auto& field:c.fields)
                        if(MagicEditable(field.second,a))try{values[NameOf(field.second)]=MagicValue(field.second,a+Read<int>(field.second+0x3c));}catch(const std::exception&){}
                }
                else for(const auto& field:fields)
                {
                    const auto at=c.fields.find(Fold(field));
                    if(at!=c.fields.end())try{values[field]=MagicValue(at->second,a+Read<int>(at->second+0x3c));}catch(const std::exception&){}
                }
                item["values"]=std::move(values);
            }
            actors.push_back(std::move(item));
        }
        catch(const std::exception&){++unreadable;}
    }
    Json result={{"actors",std::move(actors)},{"total",total}};
    if(static_cast<size_t>(cursor)+result.at("actors").size()<total)result["next"]=static_cast<size_t>(cursor)+result.at("actors").size();
    if(unreadable)result["unreadable"]=unreadable;
    return result;
}
// authoring.schema: each class as authoring.export lists it, every property
// marked writable or not. Map JSON writes the editable properties, Location
// and Rotation, a mover's keys and an actor's look; a player start's team
// goes into the pasted text on create only, and so does a new particle
// component's MaxParticles.
Json AuthoringClasses(const Json& names)
{
    Engine();
    if(!names.is_array() || names.size()>512)throw std::runtime_error("authoring.schema takes classes: a list of up to 512 class names.");
    Json result=Json::object();
    for(const auto& item:names)
    {
        const auto name=item.get<std::string>();
        const auto type=Find(name);
        if(!type || !IsA(type,"Class")){result[name]={{"error","No loaded class "+name+". Name it in full (Package.Class) and load its package first."}};continue;}
        const bool actor=AuthoringSubclass(type,"Actor"),component=AuthoringSubclass(type,"ParticleEmitter"),mover=AuthoringSubclass(type,"Mover");
        Json entry={{"class",Path(type)},{"abstract",(Read<unsigned>(type+0x8c)&1)!=0}};
        if(actor){entry["category"]=Magic::Category(Path(type));entry["brush"]=AuthoringSubclass(type,"Brush");}
        if(component)entry["component"]=true;
        std::map<std::string,Address> properties;for(auto p:Properties(type))properties.emplace(NameOf(p),p);
        auto schema=AuthoringSchema(type);
        for(auto it=schema.begin();it!=schema.end();++it)
        {
            const auto& key=it.key();auto& field=it.value();
            const auto p=properties.count(key)?properties.at(key):0;
            if((p && MagicEditable(p)) || (actor && (key=="Location" || key=="Rotation" || AuthoringAppearance(key))) || (mover && (key=="KeyPos" || key=="KeyRot" || key=="NumKeys")))
            {field["writable"]=true;continue;}
            const bool particles=component && key=="MaxParticles";
            field["writable"]=false;field["create"]=particles;
            field["reason"]=particles?"Set only on a new component (a component operation); an existing emitter keeps its MaxParticles.":"Map JSON does not write this property.";
        }
        if(AuthoringSubclass(type,"PlayerStart") && !schema.contains("TeamNumber") && properties.count("TeamNumber"))
        {
            try
            {
                auto field=MagicSchema(properties.at("TeamNumber"));
                field["writable"]=false;field["create"]=true;
                field["reason"]="Written into a new start when it is created (\"0\" Spy, \"1\" Merc); an existing start keeps its team.";
                schema["TeamNumber"]=std::move(field);
            }
            catch(const std::exception&){}
        }
        entry["schema"]=std::move(schema);
        result[name]=std::move(entry);
    }
    return result;
}
// assets.search: loaded objects of a kind whose path holds the query, sorted
// by path; total counts them all, assets the first limit.
Json SearchAssets(const std::string& kind,const std::string& query,size_t limit)
{
    Engine();
    static const std::map<std::string,std::string> kinds={{"sound","Sound"},{"material","Material"},{"staticmesh","StaticMesh"},{"texture","Texture"},{"class","Class"}};
    const auto found=kinds.find(Fold(kind));
    if(found==kinds.end())throw std::runtime_error("Search for sound, material, staticmesh, texture or class assets.");
    if(limit<1 || limit>5000)throw std::runtime_error("limit is 1 to 5000 assets.");
    const auto wanted=Fold(query);const bool classes=found->first=="class";
    std::map<Address,bool> types;std::vector<std::pair<std::string,Address>> matches;
    for(auto at:Array(0x11697B70))
    {
        const auto object=Read<Address>(at);if(!object)continue;
        const auto type=Read<Address>(object+0x24);
        auto known=types.find(type);
        if(known==types.end())known=types.emplace(type,IsA(object,found->second)).first;
        if(!known->second)continue;
        auto path=Path(object);
        if(!wanted.empty() && Fold(path).find(wanted)==std::string::npos)continue;
        matches.emplace_back(std::move(path),object);
        if(matches.size()>200000)throw std::runtime_error("Too many matching assets. Search with a longer query.");
    }
    std::sort(matches.begin(),matches.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    Json assets=Json::array();
    for(size_t i=0;i<matches.size() && i<limit;++i)
    {
        const auto object=matches[i].second;
        Json entry={{"path",matches[i].first},{"kind",found->first},{"class",Path(Read<Address>(object+0x24))}};
        if(classes)entry["abstract"]=(Read<unsigned>(object+0x8c)&1)!=0;
        if(found->first=="staticmesh")entry["bounds"]=Read<std::array<float,6>>(object+0x28);
        assets.push_back(std::move(entry));
    }
    return {{"assets",assets},{"total",matches.size()}};
}
namespace
{
    // LevelInfo settings the property sheet does not offer that settings.edit
    // may write all the same, through the ImportText the minimap uses for
    // SnapshotCamera.
    bool McpSettingsExtra(const std::string& name)
    {
        for(const char* extra:{"MapSnapshotInfo","MotionTrackingTweak","GEActivationMask","Events","SendAtTime","fDeathPawnHeight","Song"})
            if(Fold(name)==Fold(extra))return true;
        return false;
    }
    Address McpLevelInfo()
    {
        auto live=LiveActors();
        if(live.empty() || !IsA(live[0],"LevelInfo"))throw std::runtime_error("This map has no LevelInfo.");
        return live[0];
    }
}
Json LevelSettings()
{
    const auto info=McpLevelInfo();
    auto result=InspectSettingsActor(Identity(info));
    for(auto p:Properties(Read<Address>(info+0x24)))
    {
        const auto name=NameOf(p);
        if(!McpSettingsExtra(name) || MagicEditable(p,info) || result["values"].contains(name))continue;
        try
        {
            auto schema=MagicSchema(p);schema["category"]=Name(Read<int>(p+0x38));schema["direct"]=true;
            result["values"][name]=MagicValue(p,info+Read<int>(p+0x3c));result["schema"][name]=std::move(schema);
        }
        catch(const std::exception& e){result["unavailable"][name]=e.what();}
    }
    return result;
}
// settings.edit: refused when the LevelInfo changed since the snapshot (as
// MagicCheck refuses a stale actor), then every change in one Undo step.
Json EditSettings(const Json& snapshot,const Json& changes)
{
    if(!changes.is_object() || changes.empty())throw std::runtime_error("settings.edit needs changes: {property: value}.");
    const auto info=McpLevelInfo();
    if(!snapshot.is_object() || !snapshot.contains("values"))throw std::runtime_error("settings.edit needs the snapshot settings.inspect returned.");
    if(snapshot.value("level",uint64_t{0})!=static_cast<uint64_t>(LevelIdentity()) || snapshot.value("generation",0u)!=MapGeneration())
        throw Mcp::Bridge::Error(Mcp::Bridge::Code::Stale,"Another map was opened since settings.inspect. Read the settings again.");
    const auto fresh=LevelSettings();
    if(snapshot.contains("actor") && snapshot.at("actor")!=fresh.at("actor"))throw std::runtime_error("The snapshot is of another actor; settings.edit edits the LevelInfo.");
    if(fresh.at("values")!=snapshot.at("values"))throw Mcp::Bridge::Error(Mcp::Bridge::Code::Stale,"The level settings changed since settings.inspect. Read them again.");
    std::vector<std::pair<Address,const Json*>> writes;
    for(auto it=changes.begin();it!=changes.end();++it)
    {
        const auto p=Property(info,it.key());
        if(!p)throw std::runtime_error("The LevelInfo has no property "+it.key()+".");
        if(!MagicEditable(p,info) && !McpSettingsExtra(it.key()))throw std::runtime_error(it.key()+" is not a level setting RE+ writes.");
        try
        {
            Json schema;try{schema=MagicSchema(p);}catch(const std::exception&){}
            if(!schema.is_null()){Magic::Validate(schema,it.value());MagicReferences(schema,it.value());}
        }
        catch(const std::exception& e){throw std::runtime_error(it.key()+": "+e.what());}
        writes.emplace_back(p,&it.value());
    }
    Transaction transaction("Edit level settings");Modify(info);
    for(const auto& [p,value]:writes)
    {
        try{MagicImport(p,info+Read<int>(p+0x3c),*value);}
        catch(const std::exception& e){throw std::runtime_error(NameOf(p)+": "+e.what());}
    }
    Call(info,0x44);transaction.Commit();Redraw();
    return LevelSettings();
}
// scene.trace: the line check Drop to Floor uses (movers, the BSP and level
// geometry), nearest hit per segment.
Json TraceSegments(const Json& segments)
{
    if(!segments.is_array() || segments.empty() || segments.size()>4096)throw std::runtime_error("Trace 1 to 4096 segments at a time.");
    const auto level=Level();Json hits=Json::array();
    for(const auto& segment:segments)
    {
        const auto from=segment.at("from").get<Vector>(),to=segment.at("to").get<Vector>();
        Place::Check(from);Place::Check(to);
        const PlaceFVec s{static_cast<float>(from[0]),static_cast<float>(from[1]),static_cast<float>(from[2])};
        const PlaceFVec e{static_cast<float>(to[0]),static_cast<float>(to[1]),static_cast<float>(to[2])};
        PlaceHit hit{};
        if(!PlaceTraceRaw(level,0,&s,e,&hit))throw std::runtime_error("The editor's line check failed.");
        Json row={{"hit",hit.found!=0}};
        if(hit.found)
        {
            row["at"]=Vector{hit.location[0],hit.location[1],hit.location[2]};
            row["normal"]=Vector{hit.normal[0],hit.normal[1],hit.normal[2]};
            try{row["actor"]=hit.actor?Identity(hit.actor):Json();}catch(const std::exception&){row["actor"]=Json();}
        }
        hits.push_back(std::move(row));
    }
    return hits;
}
// ge.add: the editor's own GE ADD, with the points ordered as the Map Design
// element tool orders them: a pipe or ladder from its lower end, its side
// point at that height (ElementPanel.inl).
Json AddGeometricEvent(const std::string& command,const Json& points)
{
    static const std::map<std::string,size_t> kinds={{"ZL",2},{"HOH",2},{"PIPE",3},{"LADDER",3},{"FENCE",2},{"POLE",2},{"LG",2}};
    std::string name=command;for(auto& c:name)c=static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto kind=kinds.find(name);
    if(kind==kinds.end())throw std::runtime_error("A geometric event is ZL, HOH, PIPE, LADDER, FENCE, POLE or LG.");
    if(!points.is_array() || points.size()!=kind->second)throw std::runtime_error(name+" takes "+std::to_string(kind->second)+" points.");
    std::vector<Vector> at;
    for(const auto& point:points){auto v=point.get<Vector>();Design::CheckVector(v);at.push_back(v);}
    if(at.size()==3){if(at[0][2]>at[1][2])std::swap(at[0],at[1]);at[2][2]=at[0][2];}
    std::string text="GE ADD "+name;
    for(size_t i=0;i<at.size();++i)
    {
        const auto n=std::to_string(i+1);
        text+=" X"+n+"="+Design::Round(at[i][0],3)+" Y"+n+"="+Design::Round(at[i][1],3)+" Z"+n+"="+Design::Round(at[i][2],3);
    }
    Transaction transaction("Add geometric event");
    if(!Exec(text))throw std::runtime_error("The editor refused: "+text);
    transaction.Commit();Redraw();
    return {{"command",text},{"points",at}};
}
// surface.texture: the stock Apply Texture on exactly the chosen surfaces, as
// a Map JSON surface operation does it. A brush's faces are its built BSP
// surfaces (FBspSurf +0x20 FPoly, whose +0x14c is the brush), named by the
// way their normal (+0x1a, into Vectors +0x74) points.
Json TextureSurfaces(const std::string& texture,const Json& surfaces,const Json& brushes,const Json& faces)
{
    auto e=Engine(),model=Read<Address>(Level()+0x13c);
    if(!model)throw std::runtime_error("The map has no BSP surfaces. Build geometry first.");
    const auto material=Find(texture);
    if(!material || !IsA(material,"Material"))throw std::runtime_error("Load the material first: "+texture);
    auto all=Array(model+0x94,0x2c);auto vectors=Array(model+0x74,12);
    std::set<size_t> wanted;
    if(surfaces.is_array())for(const auto& s:surfaces)
    {
        const auto i=s.get<size_t>();
        if(i>=all.size())throw std::runtime_error("The map has "+std::to_string(all.size())+" BSP surfaces; surface "+std::to_string(i)+" does not exist. Build geometry first.");
        wanted.insert(i);
    }
    if(brushes.is_array() && !brushes.empty())
    {
        std::set<Address> owners;
        for(const auto& identity:brushes)
        {
            const auto a=ResolveIdentity(identity);
            if(!a || !IsA(a,"Brush"))throw std::runtime_error("No such brush in the map: "+identity.value("path",std::string()));
            owners.insert(a);
        }
        std::set<std::string> sides;
        if(faces.is_array())for(const auto& f:faces)
        {
            const auto side=f.get<std::string>();
            if(side!="floor" && side!="ceiling" && side!="+X" && side!="-X" && side!="+Y" && side!="-Y")throw std::runtime_error("Faces are floor, ceiling, +X, -X, +Y or -Y.");
            sides.insert(side);
        }
        size_t owned=0;
        for(size_t i=0;i<all.size();++i)
        {
            const auto poly=Read<Address>(all[i]+0x20),actor=poly?Read<Address>(poly+0x14c):0;
            if(!owners.count(actor))continue;
            ++owned;
            const auto index=Read<unsigned short>(all[i]+0x1a);
            if(index>=vectors.size())continue;
            const auto n=Read<std::array<float,3>>(vectors[index]);
            const char* side=std::abs(n[2])>=0.7f?(n[2]>0?"floor":"ceiling"):std::abs(n[0])>=std::abs(n[1])?(n[0]>0?"+X":"-X"):(n[1]>0?"+Y":"-Y");
            if(sides.empty() || sides.count(side))wanted.insert(i);
        }
        if(!owned)throw std::runtime_error("Those brushes have no built surfaces. Build geometry first (design.build).");
    }
    if(wanted.empty())throw std::runtime_error("No surface faces that way. Name surfaces, or brushes and the faces to texture.");
    std::vector<unsigned> flags;for(auto s:all)flags.push_back(Read<unsigned>(s+0x14));
    struct Restore{Address e,previous;std::vector<Address>& surfaces;std::vector<unsigned>& flags;
        ~Restore(){Write(e+0x138,previous);for(size_t i=0;i<surfaces.size();++i)Write(surfaces[i]+0x14,(Read<unsigned>(surfaces[i]+0x14)&~0x02000000u)|(flags[i]&0x02000000u));}
    } restore{e,Read<Address>(e+0x138),all,flags};
    Transaction transaction("Texture BSP surfaces");
    for(size_t i=0;i<all.size();++i)Write(all[i]+0x14,(flags[i]&~0x02000000u)|(wanted.count(i)?0x02000000u:0));
    Write(e+0x138,material);
    if(!Exec("POLY SETTEXTURE"))throw std::runtime_error("Native surface texturing failed.");
    transaction.Commit();Redraw();
    return {{"surfaces",wanted.size()},{"texture",Path(material)}};
}
// object.inspect / object.set (McpLifecycle.cpp): a loaded object that is not
// an actor (a package's map settings, a material), read and written through
// the same reflection InspectActor and EditActor use. The caller decides
// which packages may be written.
namespace
{
    Address McpObject(const std::string& path)
    {
        const auto object=Find(path);
        if(!object)throw std::runtime_error("No loaded object "+path+". Name it in full (Package.Group.Name) and load its package first.");
        if(IsA(object,"Actor"))throw std::runtime_error(path+" is an actor: read it with actors.query or settings.inspect, change it with Map JSON.");
        if(IsA(object,"Class"))throw std::runtime_error(path+" is a class; authoring.schema describes classes.");
        return object;
    }
}
Json InspectObject(const std::string& path) { return InspectResolved(McpObject(path)); }
Json SetObjectProperties(const std::string& path,const Json& properties,const Json& before)
{
    if(!properties.is_object() || properties.empty())throw std::runtime_error("object.set needs properties: {name: value}.");
    const auto object=McpObject(path);
    const auto snapshot=InspectResolved(object);
    if(before.is_object() && before!=snapshot.at("values"))throw Mcp::Bridge::Error(Mcp::Bridge::Code::Stale,path+" changed since it was read. Read it again.");
    for(auto it=properties.begin();it!=properties.end();++it)
    {
        if(!snapshot.at("schema").contains(it.key()))throw std::runtime_error(path+": "+it.key()+" is not an editable property.");
        try{Magic::Validate(snapshot.at("schema").at(it.key()),it.value());MagicReferences(snapshot.at("schema").at(it.key()),it.value());}
        catch(const std::exception& e){throw std::runtime_error(it.key()+": "+e.what());}
    }
    Transaction transaction("Set object properties");Modify(object);
    for(auto it=properties.begin();it!=properties.end();++it)MagicSet(object,it.key(),it.value());
    Call(object,0x44);transaction.Commit();
    return InspectResolved(object);
}
