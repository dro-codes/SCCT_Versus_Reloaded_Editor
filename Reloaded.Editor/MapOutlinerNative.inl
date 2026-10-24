// Native side of the Map Outliner (MapOutlinerModel.h): the actor list read in
// one pass, the cheap key that says when to read it again, and selecting and
// deleting the actors of the list's rows.
namespace
{
    constexpr unsigned kOutlinerHidden=0x10,kOutlinerSelected=0x40;
    // Where a class keeps the outliner's columns: the Tag, Event and Group
    // name properties and bLockLocation's word and bit, found once per class.
    struct OutlinerSlots { int tag=-1,event=-1,group=-1,lock=-1; unsigned lockMask=0; bool camera=false; std::string cls; };
    const OutlinerSlots& OutlinerClassSlots(std::unordered_map<Address,OutlinerSlots>& cache,Address type)
    {
        auto found=cache.find(type);
        if(found!=cache.end())return found->second;
        OutlinerSlots slots;
        slots.cls=Path(type);
        for(auto p:Properties(type))
        {
            const auto name=Fold(NameOf(p));
            const int offset=Read<int>(p+0x3c);
            if(offset<0 || offset>1000000)continue;
            if(name=="tag" || name=="event" || name=="group")
            {
                if(!IsA(p,"NameProperty"))continue;
                (name=="tag"?slots.tag:name=="event"?slots.event:slots.group)=offset;
            }
            else if(name=="blocklocation" && IsA(p,"BoolProperty")){slots.lock=offset;slots.lockMask=Read<unsigned>(p+0x64);}
        }
        for(Address c=type;c && !slots.camera;c=Read<Address>(c+0x28))if(Fold(NameOf(c))=="camera")slots.camera=true;
        return cache.emplace(type,std::move(slots)).first->second;
    }
    std::string OutlinerName(Address actor,int offset)
    {
        if(offset<0)return {};
        try { return Name(Read<int>(actor+offset)); }
        catch(const std::exception&) { return {}; }
    }
}
void OutlinerRows(std::vector<Outliner::Row>& rows)
{
    rows.clear();
    const auto live=LiveActors();
    rows.reserve(live.size());
    std::unordered_map<Address,OutlinerSlots> classes;
    std::unordered_map<Address,std::string> outers;
    size_t unreadable=0;std::string reason;
    for(size_t i=0;i<live.size();++i)
    {
        const auto a=live[i];
        try
        {
            const auto& slots=OutlinerClassSlots(classes,Read<Address>(a+0x24));
            // The viewports' cameras are the editor's, not the map's.
            if(slots.camera)continue;
            Outliner::Row row;
            row.handle=a;
            row.name=NameOf(a);
            const auto outer=Read<Address>(a+0x18);
            auto prefix=outers.find(outer);
            if(prefix==outers.end())prefix=outers.emplace(outer,outer?Path(outer)+".":std::string()).first;
            row.path=prefix->second+row.name;
            row.cls=slots.cls;
            row.tag=OutlinerName(a,slots.tag);
            row.event=OutlinerName(a,slots.event);
            row.group=OutlinerName(a,slots.group);
            const auto at=Read<std::array<float,3>>(a+0x80);
            for(int axis=0;axis<3;++axis)row.location[axis]=at[axis];
            const auto flags=Read<unsigned>(a+0x2f4);
            row.hidden=(flags&kOutlinerHidden)!=0;
            row.selected=(flags&kOutlinerSelected)!=0;
            row.locked=slots.lock>=0 && (Read<unsigned>(a+slots.lock)&slots.lockMask)!=0;
            // The level info and the builder brush, as Actors() marks them.
            row.authorable=i>1;
            rows.push_back(std::move(row));
        }
        catch(const std::exception& e) { if(!unreadable++)reason=e.what(); }
    }
    if(unreadable)Logger::log("Map Outliner: left out "+std::to_string(unreadable)+" actor(s) that could not be read: "+reason);
}
OutlinerKey OutlinerChangeKey()
{
    OutlinerKey key;
    key.generation=MapGeneration();
    key.content=contentRevision;
    const auto level=Level();
    key.actors=Read<int>(level+0x2c+4);
    key.data=Read<Address>(level+0x2c);
    // Stock Undo and Redo move through the buffer without an Exec or a new
    // transaction's End: how many steps wait for Redo says they happened. The
    // step count is left out: a click in a viewport adds a selection step.
    if(auto buffer=Read<Address>(Engine()+0x148))key.undoPosition=Read<int>(buffer+0x34);
    return key;
}
unsigned OutlinerFlags(std::vector<Outliner::Row>& rows)
{
    unsigned changed=0;
    for(auto& row:rows)
    {
        unsigned flags=0;
        if(!row.handle || !CopyMemorySafe(&flags,reinterpret_cast<void*>(row.handle+0x2f4),sizeof(flags)))continue;
        const bool hidden=(flags&kOutlinerHidden)!=0,selected=(flags&kOutlinerSelected)!=0;
        if(selected!=row.selected){row.selected=selected;changed|=1;}
        if(hidden!=row.hidden){row.hidden=hidden;changed|=2;}
    }
    return changed;
}
size_t OutlinerSelect(const std::vector<uintptr_t>& actors,bool focus)
{
    auto e=Engine(),level=Level();
    const std::unordered_set<Address> wanted(actors.begin(),actors.end());
    size_t selected=0;
    // Only actors whose selection changes go through native SelectActor, as
    // Select() does: it calls Modify whatever the actor's state.
    for(auto a:LiveActors())
    {
        const bool want=wanted.count(a)!=0,is=(Read<unsigned>(a+0x2f4)&kOutlinerSelected)!=0;
        selected+=want;
        if(want!=is)reinterpret_cast<void(__thiscall*)(void*,void*,void*,int,int)>(0x10eb9a20)(reinterpret_cast<void*>(e),reinterpret_cast<void*>(level),reinterpret_cast<void*>(a),want?1:0,0);
    }
    if(focus && selected)Exec("CAMERA ALIGN");
    ++revision;
    Call(e,0xe4);Redraw();
    return selected;
}
size_t OutlinerDelete(const std::vector<uintptr_t>& actors)
{
    auto e=Engine(),level=Level();
    const auto live=LiveActors();
    const std::unordered_set<Address> wanted(actors.begin(),actors.end());
    std::vector<Address> doomed;
    for(size_t i=2;i<live.size();++i)if(wanted.count(live[i]) && !IsA(live[i],"Camera"))doomed.push_back(live[i]);
    if(doomed.empty())return 0;
    const std::unordered_set<Address> chosen(doomed.begin(),doomed.end());
    {
        // One Undo step: the selection change and the stock ACTOR DELETE,
        // whose own transaction nests in this one.
        Transaction transaction("Delete actors");
        for(auto a:live)
        {
            const bool want=chosen.count(a)!=0,is=(Read<unsigned>(a+0x2f4)&kOutlinerSelected)!=0;
            if(want!=is)reinterpret_cast<void(__thiscall*)(void*,void*,void*,int,int)>(0x10eb9a20)(reinterpret_cast<void*>(e),reinterpret_cast<void*>(level),reinterpret_cast<void*>(a),want?1:0,0);
        }
        // A hidden actor picked in the list is deleted too; its flag goes in
        // directly in case SelectActor passed it over.
        for(auto a:doomed)
        {
            const auto flags=Read<unsigned>(a+0x2f4);
            if(!(flags&kOutlinerSelected)){Modify(a);Write(a+0x2f4,flags|kOutlinerSelected);}
        }
        // Stock ACTOR DELETE takes colliding actors out of the collision hash
        // without checking there is one (a level opened without the frame).
        if(!Read<Address>(level+0x3a4c))
            for(auto a:doomed)if(Read<unsigned>(a+0x2f0)&0x1000u){Modify(a);Write(a+0x2f0,Read<unsigned>(a+0x2f0)&~0x1000u);}
        if(!Exec("ACTOR DELETE"))throw std::runtime_error("The editor refused to delete the actors.");
        transaction.Commit();
    }
    size_t left=0;
    for(auto a:LiveActors())left+=chosen.count(a);
    Call(e,0xe4);Redraw();
    return doomed.size()-left;
}
