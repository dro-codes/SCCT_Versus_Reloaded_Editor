// Conveyor belts (ConveyorModel.h): imports the belt's texture and meshes into the open
// map's package, compiles ConveyorRoller, ConveyorItem, ConveyorMotor and ConveyorBelt
// into it, and places belts. Included by WorkflowEditor.cpp after
// CharacterSkinsNative.inl, whose map class compiler it uses.
namespace
{
    std::string ConveyorPath(const char* name) { return LevelPath()+"."+Conveyor::Group+"."+name; }
    Address ConveyorBeltClass() { return Find(LevelPath()+"."+Conveyor::BeltClass); }
    bool ClassCompiled(Address c) { return c && (Read<unsigned>(c+0x8c)&2); }
    // The version a compiled ConveyorBelt class was made from, or 0.
    int ConveyorClassVersion(Address c)
    {
        if(!ClassCompiled(c))return 0;
        auto p=StructField(c,"Version");auto defaults=Read<Address>(c+0xb0);
        if(!p || !defaults || Read<int>(p+0x3c)+4>Read<int>(c+0xb4))return 0;
        return Read<int>(defaults+Read<int>(p+0x3c));
    }
    bool ConveyorFields(Address c)
    {
        for(const char* name:{"Length","Width","Speed","NextBelt","ItemSpacing","FadeDistance","ItemScale","bCarryPlayers","bShowItems","bShowRollers","bLeftWall","bRightWall","ItemMeshes","ItemOffsets"})
            if(!StructField(c,name))return false;
        return true;
    }
    bool ConveyorCurrent(Address c) { return ConveyorClassVersion(c)==Conveyor::Version && ConveyorFields(c); }
    // Whether the sync timer may keep a map's belts in line: this version's class
    // or an older one with the same settings. A newer RE+'s is left as it is.
    bool ConveyorSyncable(Address c)
    {
        const int version=ConveyorClassVersion(c);
        return version>=1 && version<=Conveyor::Version && ConveyorFields(c);
    }
    // A property of a belt's class or its parents. A class compiled in this session
    // has its floats and bools only in its field list, not the editor's cached
    // property chain that Property reads, until the map is loaded again.
    Address BeltProperty(Address actor,const std::string& name)
    {
        std::set<Address> seen;
        for(Address c=Read<Address>(actor+0x24);c && seen.insert(c).second && seen.size()<64;c=Read<Address>(c+0x28))
            if(auto p=StructField(c,name))return p;
        return Property(actor,name);
    }
    Address BeltField(Address actor,const std::string& name)
    {
        auto p=BeltProperty(actor,name);
        if(!p)throw std::runtime_error("The map's ConveyorBelt class has no "+name+".");
        return actor+Read<int>(p+0x3c);
    }
    std::filesystem::path ConveyorTempFile(const std::string& name)
    {
        char folder[MAX_PATH]{};GetTempPathA(MAX_PATH,folder);
        return std::filesystem::path(folder)/("RE_"+name);
    }
    void WriteConveyorFile(const std::filesystem::path& file,const void* data,size_t size)
    {
        std::ofstream out(file,std::ios::binary);out.write(static_cast<const char*>(data),static_cast<std::streamsize>(size));
        if(!out)throw std::runtime_error("Cannot write "+file.string());
    }
    void ImportConveyorTexture(const std::string& package)
    {
        const auto tga=Conveyor::TextureTga();
        const auto file=ConveyorTempFile(std::string(Conveyor::Texture)+".tga");
        WriteConveyorFile(file,tga.data(),tga.size());
        Exec("TEXTURE IMPORT FILE=\""+file.string()+"\" NAME=\""+Conveyor::Texture+"\" PACKAGE=\""+package+"\" GROUP=\""+Conveyor::Group+"\" MIPS=1");
        std::error_code ignored;std::filesystem::remove(file,ignored);
        auto texture=Find(ConveyorPath(Conveyor::Texture));
        if(!texture || !IsA(texture,"Texture"))throw std::runtime_error("The editor did not import the conveyor texture.");
    }
    void ImportConveyorMesh(const std::string& package,const Conveyor::Mesh& mesh)
    {
        const auto text=Conveyor::Ase(mesh);
        const auto file=ConveyorTempFile(mesh.name+".ase");
        WriteConveyorFile(file,text.data(),text.size());
        Exec("STATICMESH IMPORT FILE=\""+file.string()+"\" NAME=\""+mesh.name+"\" PACKAGE=\""+package+"\" GROUP=\""+Conveyor::Group+"\"");
        std::error_code ignored;std::filesystem::remove(file,ignored);
        auto found=Find(ConveyorPath(mesh.name.c_str()));
        if(!found || !IsA(found,"StaticMesh"))throw std::runtime_error("The editor did not import the conveyor mesh "+mesh.name+".");
        const auto expected=Conveyor::Bounds(mesh);
        const auto box=Read<std::array<float,6>>(found+0x28);
        for(int i=0;i<6;++i)if(std::abs(box[i]-expected[i])>0.5)throw std::runtime_error("The conveyor mesh "+mesh.name+" imported with unexpected bounds.");
    }
    // A property value as the editor's text import takes it: numbers and booleans
    // become text, arrays and structs keep their shape.
    Json ImportValue(const Json& value)
    {
        if(value.is_boolean())return value.get<bool>()?"True":"False";
        if(value.is_number())return value.dump();
        if(value.is_array() || value.is_object())
        {
            Json out=value;
            for(auto it=out.begin();it!=out.end();++it)*it=ImportValue(*it);
            return out;
        }
        return value;
    }
    // Loads a stock static mesh by path from Packages\StaticMeshes if need be.
    Address LoadStockMesh(const std::string& path)
    {
        if(auto found=Find(path))return found;
        const auto package=path.substr(0,path.find('.'));
        const auto file=Directory().parent_path().parent_path()/"Packages"/"StaticMeshes"/(package+".usx");
        if(std::filesystem::exists(file))Exec("OBJ LOAD FILE=\""+file.string()+"\"");
        auto found=Find(path);
        return found && IsA(found,"StaticMesh")?found:0;
    }
    Address BeltArray(Address actor,const char* name,int stride)
    {
        auto p=BeltProperty(actor,name);
        if(!p)throw std::runtime_error(std::string("The map's ConveyorBelt class has no ")+name+".");
        const auto dims=Read<unsigned>(p+0x30);
        if(static_cast<int>(dims&0xffff)!=Conveyor::ItemSlots || static_cast<int>(dims>>16)!=stride)
            throw std::runtime_error(std::string("Unexpected ConveyorBelt.")+name+" layout.");
        return actor+Read<int>(p+0x3c);
    }
    // The belt class and frame meshes of the open map for the sync timer: finding an
    // object by name reads every loaded object's name, too slow to do twice a second.
    // Looked up again when another map is opened or one stops being what it was (an
    // older version made again). A map with no belt placed has nothing to sync: its
    // placed actors' classes are looked at instead, which is quick.
    struct ConveyorCache { Address level=0,package=0,belt=0; unsigned generation=0; bool syncable=false; std::array<Address,4> frames{}; };
    ConveyorCache conveyorCache;
    bool Still(Address object,const std::string& name,Address outer)
    {
        try{return object && NameOf(object)==name && Read<Address>(object+0x18)==outer;}catch(const std::exception&){return false;}
    }
    // A frame mesh is in the map package's Conveyor group.
    bool StillFrame(Address frame,const char* name,Address package)
    {
        try{return frame && Still(frame,name,Read<Address>(frame+0x18)) && Still(Read<Address>(frame+0x18),Conveyor::Group,package);}catch(const std::exception&){return false;}
    }
    // The map's own ConveyorBelt class, when a belt of it is placed.
    Address PlacedBeltClass(Address package)
    {
        std::set<Address> classes;
        for(auto a:LiveActors())
        {
            const auto c=Read<Address>(a+0x24);
            if(c && classes.insert(c).second && Read<Address>(c+0x18)==package && NameOf(c)==Conveyor::BeltClass)return c;
        }
        return 0;
    }
    const ConveyorCache& ConveyorObjects()
    {
        auto& cache=conveyorCache;
        const auto level=Level();const auto package=Read<Address>(level+0x18);const auto generation=MapGeneration();
        bool valid=cache.level==level && cache.package==package && cache.generation==generation;
        if(valid && cache.belt)
        {
            valid=Still(cache.belt,Conveyor::BeltClass,package);
            for(int i=0;valid && i<4;++i)
                valid=cache.frames[i]==0 || StillFrame(cache.frames[i],Conveyor::FrameMeshName(i&1,i&2),package);
        }
        if(valid && cache.belt)return cache;
        const auto belt=PlacedBeltClass(package);
        if(valid && !belt)return cache;
        cache=ConveyorCache{level,package,belt,generation};
        if(belt)
        {
            cache.syncable=ConveyorSyncable(belt);
            for(int i=0;i<4;++i)cache.frames[i]=Find(ConveyorPath(Conveyor::FrameMeshName(i&1,i&2)));
        }
        return cache;
    }
    // Brings a belt's frame and item offsets in line with its settings. Returns
    // whether anything changed.
    bool SyncBelt(Address actor,const std::array<Address,4>& frames)
    {
        bool changed=false;
        const auto scale=Conveyor::FrameScale(Read<float>(BeltField(actor,"Length")),Read<float>(BeltField(actor,"Width")));
        const auto at=Field(actor,"DrawScale3D");
        if(Read<std::array<float,3>>(at)!=scale){Write(at,scale);changed=true;}
        if(Read<float>(Field(actor,"DrawScale"))!=1.0f){Write(Field(actor,"DrawScale"),1.0f);changed=true;}
        // The frame with the channels its wall options keep.
        auto bit=[&](const char* name){auto p=BeltProperty(actor,name);return p && (Read<unsigned>(actor+Read<int>(p+0x3c))&Read<unsigned>(p+0x64))!=0;};
        if(auto frame=frames[(bit("bLeftWall")?1:0)|(bit("bRightWall")?2:0)])
        {
            const auto slot=Field(actor,"StaticMesh");
            if(Read<Address>(slot)!=frame){Write(slot,frame);changed=true;}
        }
        const auto meshes=BeltArray(actor,"ItemMeshes",4),offsets=BeltArray(actor,"ItemOffsets",12);
        for(int i=0;i<Conveyor::ItemSlots;++i)
        {
            auto mesh=Read<Address>(meshes+i*4);
            std::array<float,3> offset{};
            if(mesh && IsA(mesh,"StaticMesh"))offset=Conveyor::ItemOffset(Read<std::array<float,6>>(mesh+0x28));
            if(Read<std::array<float,3>>(offsets+i*12)!=offset){Write(offsets+i*12,offset);changed=true;}
        }
        return changed;
    }
}

Json ConveyorState()
{
    auto c=ConveyorBeltClass();
    Json belts=Json::array();
    if(c)for(auto a:ActorsOf(c))belts.push_back(Identity(a));
    return {{"compiled",ConveyorCurrent(c)},{"version",ConveyorClassVersion(c)},{"belts",belts}};
}

// Imports the texture and meshes and compiles the classes (the rollers', items' and
// motor's before the belt's, which spawns them) into the open map, unless
// they are there at this version already. A map's older versions are made again over
// themselves, so its belts keep their settings.
void PrepareConveyors()
{
    const auto package=LevelPath();
    const auto existing=ConveyorBeltClass();
    // Made again over a newer RE+'s classes, its belts would lose what that version added.
    if(const int version=ConveyorClassVersion(existing);version>Conveyor::Version)
        throw std::runtime_error("This map's conveyor belts were made by a newer version of RE+ (conveyor version "+std::to_string(version)+"). Update RE+ to add belts to it.");
    const bool current=ConveyorCurrent(existing);
    if(!current || !Find(ConveyorPath(Conveyor::Texture)))ImportConveyorTexture(package);
    for(const auto& mesh:{Conveyor::Frame(),Conveyor::Frame(false,true),Conveyor::Frame(true,false),Conveyor::Frame(false,false),Conveyor::Roller()})
        if(!current || !Find(ConveyorPath(mesh.name.c_str())))ImportConveyorMesh(package,mesh);
    auto compiled=[](Address c){return ClassCompiled(c);};
    CompileMapClass(Conveyor::RollerClass,Conveyor::RollerScript(package),"Actor",compiled,!current);
    CompileMapClass(Conveyor::ItemClass,Conveyor::ItemScript(),"Actor",compiled,!current);
    CompileMapClass(Conveyor::MotorClass,Conveyor::MotorScript(),"Info",compiled,!current);
    CompileMapClass(Conveyor::BeltClass,Conveyor::BeltScript(package),"Actor",ConveyorCurrent,!current);
    conveyorCache=ConveyorCache{};
}

// The one selected actor when it is a conveyor belt, else null.
// The selected actor's own class is looked at, not every loaded object: this runs on
// every right-click in a viewport.
Json SelectedConveyorBelt()
{
    Address found=0;size_t selected=0;
    for(auto a:LiveActors())
        if(Read<unsigned>(a+0x2f4)&0x40)
        {
            if(++selected>1)return nullptr;
            found=a;
        }
    if(!found)return nullptr;
    const auto c=Read<Address>(found+0x24);
    return c && Read<Address>(c+0x18)==Read<Address>(Level()+0x18) && NameOf(c)==Conveyor::BeltClass?Identity(found):Json(nullptr);
}

namespace
{
    // Places a belt as AddConveyorBelt describes. It begins the transaction before
    // ACTOR ADD, whose own "Add Actor" then nests in it, so the belt and everything
    // set on it are one Undo step; a turn's corner piece and the belt after it share it.
    Address PlaceConveyorBelt(const Json& spec,std::optional<Transaction>& transaction)
    {
        auto c=ConveyorBeltClass();
        Address previous=0;
        if(spec.contains("after"))
        {
            auto name=spec.at("after").get<std::string>();
            if(name.find('.')==std::string::npos)name=LevelPath()+"."+name;
            previous=Find(name,true);
            if(!previous || Read<Address>(previous+0x24)!=c)throw std::runtime_error(spec.at("after").get<std::string>()+" is not a conveyor belt in this map.");
        }
        if(spec.contains("turn"))
        {
            const auto turn=spec.at("turn").get<std::string>();
            if(!previous || (turn!="left" && turn!="right"))throw std::runtime_error("A turn is left or right, after a belt.");
            const bool right=turn=="right";
            const double width=Read<float>(BeltField(previous,"Width")),length=Read<float>(BeltField(previous,"Length"));
            const auto rotation=RotationOf(previous);
            // Unreal's Y is to the right of X: a right turn heads along the belt's Y.
            const Rotation level{0,rotation[1],0},corner{0,(rotation[1]+(right?16384:-16384))&0xffff,0};
            const auto end=TransformPoint({length,0,0},{Position(previous),rotation});
            const auto start=TransformPoint({width/2,right?-width/2:width/2,0},{end,level});
            Json cornerSpec={{"after",Path(previous)},{"location",start},{"rotation",corner},
                             {"properties",{{"Length",width},{right?"bRightWall":"bLeftWall",false}}}};
            const auto piece=PlaceConveyorBelt(cornerSpec,transaction);
            Json next=spec;next.erase("turn");next.erase("location");next.erase("rotation");next["after"]=Path(piece);
            return PlaceConveyorBelt(next,transaction);
        }
        const auto properties=spec.value("properties",Json::object());
        for(auto it=properties.begin();it!=properties.end();++it)
        {
            bool found=false;std::set<Address> seen;
            for(Address k=c;k && !found && seen.insert(k).second && seen.size()<64;k=Read<Address>(k+0x28))found=StructField(k,it.key())!=0;
            if(!found)throw std::runtime_error("ConveyorBelt has no property "+it.key()+".");
        }
        if(!transaction)transaction.emplace("Add conveyor belt");
        std::set<Address> before;for(auto a:ActorsOf(c))before.insert(a);
        if(!Exec(std::string("ACTOR ADD CLASS=")+Conveyor::BeltClass))throw std::runtime_error("The editor refused to place a conveyor belt.");
        Address actor=0;
        for(auto a:ActorsOf(c))if(!before.count(a))actor=a;
        if(!actor)throw std::runtime_error("The editor did not place a conveyor belt.");
        Vector at{};
        if(spec.contains("location"))at=spec.at("location").get<Vector>();
        else if(previous)at=TransformPoint({Read<float>(BeltField(previous,"Length")),0,0},{Position(previous),RotationOf(previous)});
        else
        {
            auto unrealEd=Read<Address>(0x117a59b0);if(!unrealEd)throw std::runtime_error("The editor is unavailable.");
            const auto click=Read<std::array<float,3>>(unrealEd+0x1bc);
            const auto plane=Read<std::array<float,3>>(unrealEd+0x1d4);
            for(int axis=0;axis<3;++axis)at[axis]=click[axis];
            if(spec.value("surface",true) && std::isfinite(plane[2]) && plane[2]>0.7f)at[2]+=Conveyor::FrameDepth;
        }
        Modify(actor);
        SetPosition(actor,at);
        if(spec.contains("rotation"))Write(Field(actor,"Rotation"),spec.at("rotation").get<Rotation>());
        else if(previous)Write(Field(actor,"Rotation"),RotationOf(previous));
        if(previous)
        {
            // Its look, speed, options and items (a line's items are its first belt's,
            // and in a loop that can be any of them); the walls are the new belt's own.
            for(const char* name:{"Width","Speed","ItemScale","ItemSpacing","FadeDistance"})
                Write(BeltField(actor,name),Read<float>(BeltField(previous,name)));
            for(const char* name:{"bCarryPlayers","bShowRollers","bShowItems"})
            {
                auto p=BeltProperty(actor,name);const auto mask=Read<unsigned>(p+0x64);const auto at=actor+Read<int>(p+0x3c);
                const bool on=(Read<unsigned>(previous+Read<int>(p+0x3c))&mask)!=0;
                Write(at,on?(Read<unsigned>(at)|mask):(Read<unsigned>(at)&~mask));
            }
            Modify(previous);
            Write(BeltField(actor,"NextBelt"),Read<Address>(BeltField(previous,"NextBelt")));
            Write(BeltField(previous,"NextBelt"),actor);
            Call(previous,0x44);
        }
        // The previous belt's items, or the stock cases and boxes, until the properties
        // say otherwise.
        const auto meshes=BeltArray(actor,"ItemMeshes",4);
        if(previous)
        {
            const auto from=BeltArray(previous,"ItemMeshes",4);
            for(int i=0;i<Conveyor::ItemSlots;++i)Write(meshes+4*i,Read<Address>(from+4*i));
        }
        else
        {
            int slot=0;
            for(const auto* path:Conveyor::DefaultItems)
                if(auto mesh=LoadStockMesh(path);mesh && slot<Conveyor::ItemSlots)Write(meshes+4*slot++,mesh);
        }
        for(auto it=properties.begin();it!=properties.end();++it)
        {
            const auto& key=it.key();const auto& value=it.value();
            auto p=BeltProperty(actor,key);
            if(!p)throw std::runtime_error("ConveyorBelt has no property "+key+".");
            if(key=="ItemMeshes" && value.is_array())
            {
                for(int i=0;i<Conveyor::ItemSlots;++i)
                {
                    Address mesh=0;
                    if(i<static_cast<int>(value.size()) && value[i].is_string() && !value[i].get<std::string>().empty() && Fold(value[i].get<std::string>())!="none")
                    {
                        auto path=value[i].get<std::string>();
                        if(auto name=ReferencedName(path);!name.empty())path=name;
                        mesh=LoadStockMesh(path);
                        if(!mesh)throw std::runtime_error("ItemMeshes: "+path+" is not a static mesh the editor can load.");
                    }
                    Write(meshes+4*i,mesh);
                }
                continue;
            }
            try{MagicImport(p,actor+Read<int>(p+0x3c),ImportValue(value));}catch(const std::exception& e){throw std::runtime_error(key+": "+e.what());}
        }
        conveyorCache=ConveyorCache{};
        SyncBelt(actor,ConveyorObjects().frames);
        Call(actor,0x44);
        return actor;
    }
    // The belts of the line through this one: back along the belts leading onto it
    // and on along NextBelt, each once (all of a closed loop's).
    size_t ConveyorLineLength(Address belt)
    {
        const auto next=BeltField(belt,"NextBelt")-belt;
        const auto belts=ActorsOf(Read<Address>(belt+0x24));
        std::set<Address> line{belt};
        for(Address b=Read<Address>(belt+next);b && line.insert(b).second;b=Read<Address>(b+next)){}
        for(Address b=belt;;)
        {
            Address before=0;
            for(auto a:belts)if(Read<Address>(a+next)==b){before=a;break;}
            if(!before || !line.insert(before).second)break;
            b=before;
        }
        return line.size();
    }
}

// Places a belt. Without a location it starts where the viewport was last clicked,
// standing on the floor when that was a floor. after (a belt's name or path) puts it
// at that belt's end, turned as it is, with its width, speed, options and items, and links
// it in: the belt carries its items on onto the new one (and the new one onto the
// belt that came next, if any). turn ("left" or "right") with after first adds a
// level corner piece, a square of that belt's width beyond its end, open on the
// side the belt comes in, and the new belt carries on from the corner. properties
// sets its own (Length, Speed, ItemMeshes, ...) as the property window would.
Json AddConveyorBelt(const Json& spec)
{
    PrepareConveyors();
    std::optional<Transaction> transaction;
    const auto actor=PlaceConveyorBelt(spec,transaction);
    transaction->Commit();
    Select(Json::array({Identity(actor)}));
    Redraw();
    auto added=Identity(actor);
    size_t belts=0;
    try{belts=ConveyorLineLength(actor);}catch(const std::exception&){/* The belt is placed; only the warning is lost. */}
    if(belts>static_cast<size_t>(Conveyor::MaxChain))
        added["warning"]="This line now has "+std::to_string(belts)+" belts. Items ride at most "+std::to_string(Conveyor::MaxChain)
            +" belts of one line: the belts after that carry none, and a closed loop this long may carry none at all. "
             "Make it two lines: clear NextBelt on the belt where the first one should end.";
    return added;
}

// Keeps every belt's frame sized to its Length and Width, its channels to its wall
// options and its item offsets to its meshes as they are edited.
size_t SyncConveyorBelts()
{
    // A load pumps messages while its level is only partly there (UObject::GObjBeginLoadCount).
    if(Read<int>(0x11697B10)>0)return 0;
    const auto& cache=ConveyorObjects();
    if(!cache.belt || !cache.syncable)return 0;
    size_t changed=0;
    for(auto a:ActorsOf(cache.belt))if(SyncBelt(a,cache.frames)){Call(a,0x44);++changed;}
    if(changed)Redraw();
    return changed;
}
