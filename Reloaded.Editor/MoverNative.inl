// Mover Workbench, the editor side (MoverModel.h holds the key list and its
// maths). Included inside Workflow::Editor after MagicEventNative.inl, whose
// property readers and writers it uses. Every edit is one Undo step.
namespace
{
    namespace Mov = Workflow::Movers;

    Address MoverActor(const Json& identity)
    {
        auto actor=ResolveIdentity(identity);
        if(!actor || !IsA(actor,"Mover"))throw std::runtime_error("That mover is no longer in the map. Select a mover.");
        return actor;
    }
    Vector MoverVector(Address actor,const char* name)
    {
        auto v=Read<std::array<float,3>>(Field(actor,name));return {v[0],v[1],v[2]};
    }
    void MoverWriteVector(Address actor,const char* name,const Vector& v)
    {
        for(auto x:v)if(!std::isfinite(x) || std::abs(x)>10000000)throw std::runtime_error("A mover position is out of range.");
        Write(Field(actor,name),std::array<float,3>{static_cast<float>(v[0]),static_cast<float>(v[1]),static_cast<float>(v[2])});
    }
    Json MoverValue(Address actor,const char* name)
    {
        auto p=Property(actor,name);if(!p)return nullptr;
        return MagicValue(p,actor+Read<int>(p+0x3c));
    }
    Mov::Path MoverPath(Address actor)
    {
        Json values={{"KeyPos",MoverValue(actor,"KeyPos")},{"KeyRot",MoverValue(actor,"KeyRot")},{"NumKeys",MoverValue(actor,"NumKeys")},{"KeyNum",MoverValue(actor,"KeyNum")}};
        return Mov::FromValues(values,MoverVector(actor,"BasePos"),Read<Rotation>(Field(actor,"BaseRot")));
    }
    // The settings the workbench edits besides the keys.
    const char* const kMoverSettings[]={"MoveTime","StayOpenTime","DelayTime","DelayTimeLastKey","InitialState","bTriggerOnceOnly",
        "MoverEncroachType","EncroachDamage","MoverGlideType","BumpType","bUseShortestRotation","Tag","Event",
        "OpeningSound","OpenedSound","ClosingSound","ClosedSound","MoveAmbientSound"};
    // The class's states (UState children of each class up to Actor), most derived first.
    Json MoverStates(Address actor)
    {
        Json states=Json::array();std::set<std::string> seen;std::set<Address> classes;
        for(auto c=Read<Address>(actor+0x24);c && classes.insert(c).second && classes.size()<64;c=Read<Address>(c+0x28))
        {
            if(NameOf(c)=="Actor")break;
            std::set<Address> fields;
            for(auto f=Read<Address>(c+0x30);f && fields.insert(f).second && fields.size()<4096;f=Read<Address>(f+0x2c))
                if(NameOf(Read<Address>(f+0x24))=="State" && seen.insert(Fold(NameOf(f))).second)states.push_back(NameOf(f));
        }
        return states;
    }
    // A mover drawn as a static mesh (most stock doors): the mesh's box,
    // turned and scaled as the actor draws it (as SelectedMeshBounds reads it).
    void MoverMeshBox(Address actor,std::vector<Mov::Edge>& edges)
    {
        auto draw=Property(actor,"DrawType");
        if(!draw || Read<unsigned char>(actor+Read<int>(draw+0x3c))!=8)return; // DT_StaticMesh
        auto property=Property(actor,"StaticMesh");
        auto mesh=property?Read<Address>(actor+Read<int>(property+0x3c)):0;
        if(!mesh || !IsA(mesh,"StaticMesh"))return;
        auto box=Read<std::array<float,6>>(mesh+0x28);
        auto scale=Read<float>(Field(actor,"DrawScale"));
        auto scale3=Read<std::array<float,3>>(Field(actor,"DrawScale3D"));
        auto pivot=Read<std::array<float,3>>(Field(actor,"PrePivot"));
        for(int axis=0;axis<3;++axis)
            if(!std::isfinite(box[axis]) || !std::isfinite(box[axis+3]) || box[axis]>box[axis+3] || !std::isfinite(scale) || !std::isfinite(scale3[axis]))return;
        const Pose pose{Position(actor),RotationOf(actor)};
        auto corner=[&](int c){
            Vector local{};
            for(int axis=0;axis<3;++axis)local[axis]=(static_cast<double>(box[axis+((c&(1<<axis))?3:0)])-pivot[axis])*scale*scale3[axis];
            return TransformPoint(local,pose);
        };
        for(int c=0;c<8;++c)
            for(int axis=0;axis<3;++axis)
                if(!(c&(1<<axis)))edges.push_back({corner(c),corner(c|(1<<axis))});
    }
    // The mover's outline: the edges of its brush in the world, each once,
    // and the box of the static mesh it draws, if it draws one.
    std::vector<Mov::Edge> MoverEdges(Address actor)
    {
        std::vector<Mov::Edge> edges;
        MoverMeshBox(actor,edges);
        auto model=Read<Address>(actor+0x238),polys=model?Read<Address>(model+0x50):0;
        if(!polys)return edges;
        std::array<float,12> coords{};Call<void*>(actor,0xac,coords.data());
        std::set<std::array<long long,6>> seen;
        auto world=[&](Address poly,int i){
            auto local=Read<std::array<float,3>>(poly+0x18+i*12);std::array<float,3> w{};
            reinterpret_cast<void*(__thiscall*)(void*,void*,const void*)>(0x10eb2ba0)(local.data(),w.data(),coords.data());
            return Vector{w[0],w[1],w[2]};
        };
        for(auto poly:Array(polys+0x28,0x14c))
        {
            const int count=Read<unsigned short>(poly+0x148);if(count<3 || count>16)continue;
            for(int i=0;i<count;++i)
            {
                auto a=world(poly,i),b=world(poly,(i+1)%count);
                bool sane=true;for(int k=0;k<3;++k)if(!std::isfinite(a[k]) || !std::isfinite(b[k]) || std::abs(a[k])>1e7 || std::abs(b[k])>1e7)sane=false;
                if(!sane)continue;
                std::array<long long,6> key{};
                for(int k=0;k<3;++k){key[k]=std::llround(a[k]*8);key[k+3]=std::llround(b[k]*8);}
                std::array<long long,6> flipped{key[3],key[4],key[5],key[0],key[1],key[2]};
                if(seen.count(key) || seen.count(flipped))continue;
                seen.insert(key);edges.push_back({a,b});
                if(edges.size()>100000)return edges;
            }
        }
        return edges;
    }
    // Writes the path: the key arrays, NumKeys and KeyNum, the base, and the
    // mover on its key, then PostEditChange (which keeps that base: it is
    // worked out as Location - OldPos). Inside the caller's transaction.
    void MoverWritePath(Address actor,const Mov::Path& path)
    {
        Mov::CheckPath(path);
        auto keyPos=Property(actor,"KeyPos");
        if(!keyPos || Read<unsigned short>(keyPos+0x30)!=path.capacity)throw std::runtime_error("This mover's key arrays changed. Refresh the workbench.");
        Modify(actor);
        MagicSet(actor,"KeyPos",Mov::PositionsJson(path));
        MagicSet(actor,"KeyRot",Mov::RotationsJson(path));
        MagicSet(actor,"NumKeys",std::to_string(path.keys.size()));
        MagicSet(actor,"KeyNum",std::to_string(path.current));
        const auto place=Mov::At(path,path.current);
        MoverWriteVector(actor,"BasePos",path.base);
        Write(Field(actor,"BaseRot"),path.baseRotation);
        MoverWriteVector(actor,"OldPos",path.keys[path.current].position);
        Write(Field(actor,"OldRot"),path.keys[path.current].rotation);
        SetPosition(actor,place.position);
        Write(Field(actor,"Rotation"),place.rotation);
        Call(actor,0x44);
    }
    std::string MoverUnusedTag(const std::string& stem)
    {
        std::set<std::string> used;for(const auto& slot:TagSlots())used.insert(Fold(slot.value));
        std::string clean;for(char c:stem)if(std::isalnum(static_cast<unsigned char>(c)) || c=='_')clean+=c;
        if(clean.empty() || std::isdigit(static_cast<unsigned char>(clean[0])))clean="Mover_"+clean;
        if(!used.count(Fold(clean)))return clean;
        for(int i=2;i<1000000;++i){auto tag=clean+"_"+std::to_string(i);if(!used.count(Fold(tag)))return tag;}
        throw std::runtime_error("Could not find an unused Tag.");
    }
    // A Tag nobody else answers to by accident: not None, not empty, not the
    // class name every untagged actor of the class carries.
    bool MoverOwnTag(Address actor,const std::string& tag)
    {
        const auto folded=Fold(tag);
        return !tag.empty() && folded!="none" && folded!=Fold(NameOf(Read<Address>(actor+0x24)));
    }
    bool MoverTriggerLike(Address a)
    {
        if(IsA(a,"Triggers") || IsA(a,"SBase.SMagicEvent"))return true;
        const auto name=Fold(NameOf(Read<Address>(a+0x24)));
        for(const char* word:{"trigger","button","switch","lever","keypad","panel","usable","interact"})
            if(name.find(word)!=std::string::npos)return true;
        return false;
    }
    // The pose and its bytes before a preview moved the mover, to put back.
    struct MoverPreviewState { Address actor=0,level=0; std::array<unsigned char,12> location{},rotation{}; };
    MoverPreviewState moverPreview;
}
Json SelectedMover()
{
    Json found;
    for(auto a:LiveActors())
        if(Read<unsigned>(a+0x2f4)&0x40)
        {
            if(!found.is_null())return nullptr;
            if(!IsA(a,"Mover"))return nullptr;
            found=Identity(a);
        }
    return found;
}
Json MoverState(const Json& identity)
{
    auto actor=MoverActor(identity);
    const auto path=MoverPath(actor);
    Json values=Json::object(),choices=Json::object();
    for(const char* name:kMoverSettings)
    {
        auto p=Property(actor,name);if(!p)continue;
        values[name]=MagicValue(p,actor+Read<int>(p+0x3c));
        auto schema=MagicSchema(p);
        if(schema.contains("choices"))choices[name]=schema.at("choices");
    }
    choices["InitialState"]=MoverStates(actor);
    // Who sets it going, and what it sets going: the Gameplay Connections links.
    Json triggeredBy=Json::array(),triggers=Json::array();
    const auto self=Path(actor);
    for(const auto& link:Connections(self))
    {
        if(link.at("kind").get<std::string>().find("Tag")==std::string::npos)continue;
        if(Fold(link.at("to").get<std::string>())==Fold(self))triggeredBy.push_back({{"actor",link.at("from")},{"property",link.at("property")}});
        else if(Fold(link.at("from").get<std::string>())==Fold(self))triggers.push_back({{"actor",link.at("to")},{"property",link.at("property")},{"resolved",link.at("resolved")}});
    }
    return {{"actor",Identity(actor)},{"name",NameOf(actor)},{"class",Path(Read<Address>(actor+0x24))},{"revision",ContentRevision()},{"level",LevelIdentity()},
            {"path",Mov::PathJson(path)},{"location",Position(actor)},{"rotation",RotationOf(actor)},
            {"values",values},{"choices",choices},{"triggeredBy",triggeredBy},{"triggers",triggers}};
}
// Writes a new key list (Add, Update, Delete, Move, Go to Key) as one Undo step.
Json SetMoverPath(const Json& identity,const Json& path,const std::string& label)
{
    auto actor=MoverActor(identity);
    auto next=Mov::PathFrom(path);
    Transaction transaction(("Mover Workbench: "+label).c_str());
    MoverWritePath(actor,next);
    transaction.Commit();Redraw();
    return MoverState(identity);
}
// Settings by property name, as text (Magic JSON values), in one Undo step.
Json SetMoverSettings(const Json& identity,const Json& changes)
{
    auto actor=MoverActor(identity);
    if(!changes.is_object() || changes.empty())return MoverState(identity);
    const auto states=MoverStates(actor);
    for(auto it=changes.begin();it!=changes.end();++it)
    {
        if(std::none_of(std::begin(kMoverSettings),std::end(kMoverSettings),[&](const char* n){return it.key()==n;}))throw std::runtime_error("The workbench does not edit "+it.key()+".");
        auto p=Property(actor,it.key());if(!p)throw std::runtime_error("This mover has no "+it.key()+".");
        if(it.key()=="Tag")throw std::runtime_error("Use Rename Tag, which keeps the triggers pointing at it.");
        const auto& value=it.value();
        if((it.key()=="Event") && value.is_string() && !value.get<std::string>().empty() && !std::regex_match(value.get<std::string>(),std::regex("[A-Za-z_][A-Za-z0-9_]{0,62}")))
            throw std::runtime_error("Event: use 1-63 letters, digits or underscores, starting with a letter or underscore, or None.");
        if(it.key()=="InitialState" && Fold(value.get<std::string>())!="none"
           && std::none_of(states.begin(),states.end(),[&](const Json& s){return Fold(s.get<std::string>())==Fold(value.get<std::string>());}))
            throw std::runtime_error("InitialState: this mover has no state called "+value.get<std::string>()+".");
        try{auto schema=MagicSchema(p);Magic::Validate(schema,value);MagicReferences(schema,value);}
        catch(const std::exception& e){throw std::runtime_error(it.key()+": "+e.what());}
    }
    Transaction transaction("Mover Workbench: settings");
    Modify(actor);
    for(auto it=changes.begin();it!=changes.end();++it)MagicSet(actor,it.key(),it.value());
    Call(actor,0x44);
    transaction.Commit();Redraw();
    return MoverState(identity);
}
// The outline at the mover's place: world edges and the pose they were read at.
Json MoverOutline(const Json& identity)
{
    auto actor=MoverActor(identity);
    Json edges=Json::array();
    for(const auto& e:MoverEdges(actor))edges.push_back({e.from,e.to});
    return {{"edges",edges},{"location",Position(actor)},{"rotation",RotationOf(actor)}};
}
// Preview: the mover put at a pose for drawing, and put back byte for byte.
// Neither is an edit: nothing is marked changed and no Undo step is made.
void MoverPreviewPose(const Json& identity,const Pose& pose)
{
    auto actor=MoverActor(identity);
    if(moverPreview.actor && moverPreview.actor!=actor)MoverRestorePose();
    if(!moverPreview.actor)
    {
        moverPreview.actor=actor;moverPreview.level=Level();
        moverPreview.location=Read<std::array<unsigned char,12>>(actor+0x80);
        moverPreview.rotation=Read<std::array<unsigned char,12>>(Field(actor,"Rotation"));
    }
    SetPosition(actor,pose.position);
    Write(Field(actor,"Rotation"),pose.rotation);
    Redraw();
}
bool MoverPreviewing() { return moverPreview.actor!=0; }
void MoverRestorePose()
{
    auto state=moverPreview;moverPreview={};
    if(!state.actor)return;
    // Only into the same map, and only while the actor is still in it.
    try
    {
        if(Level()!=state.level)return;
        auto live=LiveActors();if(std::find(live.begin(),live.end(),state.actor)==live.end())return;
        Write(state.actor+0x80,state.location);
        Write(Field(state.actor,"Rotation"),state.rotation);
        Redraw();
    }
    catch(const std::exception& e){Logger::log(std::string("Mover Workbench: preview not put back: ")+e.what());}
}
// The actors a mover can be wired to: triggers, buttons, switches and
// SMagicEvents, and anything that already fires an Event.
Json MoverTriggerCandidates(const Json& identity)
{
    auto mover=MoverActor(identity);
    Json out=Json::array();auto live=LiveActors();
    for(size_t i=2;i<live.size();++i)
    {
        auto a=live[i];if(a==mover || IsA(a,"Camera") || (IsA(a,"Brush") && !IsA(a,"Mover") && !IsA(a,"Volume")))continue;
        const auto event=NameField(a,"Event");
        if(!MoverTriggerLike(a) && (event.empty() || Fold(event)=="none"))continue;
        out.push_back({{"path",Path(a)},{"class",Path(Read<Address>(a+0x24))},{"event",event}});
        if(out.size()>=5000)break;
    }
    std::sort(out.begin(),out.end(),[](const Json& a,const Json& b){return Fold(a.at("path").get<std::string>())<Fold(b.at("path").get<std::string>());});
    return out;
}
// Wires a trigger to the mover in one Undo step: the trigger's Event (or, for
// an SMagicEvent, a new action in its first group) names the mover's Tag. A
// mover without a Tag of its own gets one first, named after it.
Json WireMoverTrigger(const Json& identity,const Json& trigger)
{
    auto mover=MoverActor(identity);
    auto source=ResolveIdentity(trigger);
    if(!source || source==mover)throw std::runtime_error("Choose the trigger or button to wire to this mover.");
    auto tag=NameField(mover,"Tag");
    const bool newTag=!MoverOwnTag(mover,tag);
    if(newTag)tag=MoverUnusedTag(NameOf(mover));
    Transaction transaction("Mover Workbench: wire trigger");
    Modify(mover);if(newTag){MagicSet(mover,"Tag",tag);Call(mover,0x44);}
    Modify(source);
    if(IsA(source,"SBase.SMagicEvent"))MagicLink(source,mover,false,0);
    else MagicSet(source,"Event",tag);
    Call(source,0x44);
    transaction.Commit();Redraw();
    return {{"tag",tag},{"newTag",newTag},{"state",MoverState(identity)}};
}
