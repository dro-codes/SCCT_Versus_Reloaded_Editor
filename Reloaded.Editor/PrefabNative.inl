// Native side of Actor Assemblies placement and linked prefabs (PrefabModel.h
// holds the names, records and pose model). All calls run on the UI thread;
// placing and updating are one Undo step each.
namespace
{
    namespace Prefabs = Workflow::Prefab;

    // Loads the packages a definition needs and checks its bindings; returns
    // the bindings with each Event target turned into that actor's Tag.
    std::map<std::string,std::string> PlacementBindings(const Json& definition,const std::map<std::string,std::string>& bindings)
    {
        auto resolved=bindings;
        for(const auto& dependency:PlacementDependencies(definition))
        {
            auto path=dependency.get<std::string>();
            if(!Find(path))
            {
                auto package=path.substr(0,path.find('.'));
                if(package.find_first_of("\"\r\n/\\")!=std::string::npos) throw std::runtime_error("Invalid dependency package.");
                if(Fold(package)==Fold(LevelPath())) throw std::runtime_error(path+" was stored inside the map this entry was saved from, so this map does not have it. Move the asset into a shared package and save the entry again.");
                // Loading the class/object by package leaves existing assets untouched.
                // Ambient sounds are .uax packages; map sound banks are .uas.
                const std::pair<const char*,const char*> folders[]={{"Textures",".utx"},{"StaticMeshes",".usx"},{"Sounds",".uax"},{"Sounds",".uas"},{"Animations",".ukx"}};
                for(const auto& [directory,extension]:folders)
                {
                    auto file=Directory().parent_path().parent_path()/"Packages"/directory/(package+extension);
                    if(std::filesystem::exists(file)) Exec("OBJ LOAD FILE=\""+file.string()+"\"");
                }
                if(!Find(path)) throw std::runtime_error("Load the required package before placing: "+path);
            }
        }
        for(const auto& binding:definition.at("bindings"))
        {
            auto id=binding.at("id").get<std::string>(); auto it=resolved.find(id);
            if(it==resolved.end()) throw std::runtime_error("Missing binding: "+binding.at("label").get<std::string>());
            if(it->second.empty()) continue;
            auto target=Find(it->second,true);
            if(!target || !IsA(target,binding.at("type").get<std::string>())) throw std::runtime_error("Incompatible external actor: "+it->second);
            if(binding.at("kind")=="event")
            {
                it->second=NameField(target,"Tag");
                if(Fold(it->second)=="none") throw std::runtime_error("The bound Event target must have a Tag. Set its Tag before placement.");
            }
        }
        return resolved;
    }
    struct PastedAssembly { Json members=Json::array(),names=Json::object(); };
    // Pastes a prepared placement inside the caller's transaction, puts each
    // actor at its requested pose (native paste recentres them) and, for a
    // linked prefab, adds the instance's Group. Leaves the new actors selected.
    PastedAssembly PasteAssembly(const Json& prepared,const std::string& prefix,const std::string& group)
    {
        if(!pasteHookReady) throw std::runtime_error("This editor build does not support verified assembly insertion.");
        const std::string text=ConvertText(prepared.at("t3d").get<std::string>(),CP_UTF8,CP_ACP);
        struct TextScope { TextScope(const char* value) {if(insertionText) throw std::runtime_error("Assembly insertion is already running.");insertionText=value;} ~TextScope(){insertionText=nullptr;} };
        {
            TextScope source(text.c_str());
            Select(Json::array());
            // Verified UUnrealEdEngine::edactPasteSelected.
            Call(Engine(),0x26c,reinterpret_cast<void*>(Level()),0);
        }
        PastedAssembly result; const ActorIndex index;
        for(const auto& actor:prepared.at("actors"))
        {
            auto a=index.Find(LevelPath()+"."+actor.at("name").get<std::string>());
            if(!a) throw std::runtime_error("Native paste did not create all assembly actors.");
            Modify(a); SetPosition(a,actor.at("position").get<Vector>()); Write(Field(a,"Rotation"),actor.at("rotation").get<Rotation>()); Call(a,0x44);
            if(!group.empty())
            {
                if(!Property(a,"Group")) throw std::runtime_error("This editor's actors do not expose the native Group field.");
                DesignSetGroup(a,Prefabs::WithGroup(NameField(a,"Group"),group));
            }
            result.members.push_back(Identity(a));
            result.names[Path(a)]=actor.at("name").get<std::string>().substr(prefix.size());
        }
        index.Report(prepared.at("actors").size(),"PasteAssembly");
        if(SelectedIdentities().size()!=result.members.size()) throw std::runtime_error("Native paste created an unexpected actor count.");
        return result;
    }
    std::string PlacementPrefix() { return "RE_"+Id().substr(0,12)+"_"; }
}
Json PlaceAssembly(const Json& definition,const Pose& frame,const std::map<std::string,std::string>& bindings,const std::string& group)
{
    if(!group.empty() && !Prefabs::IsInstanceGroup(group)) throw std::runtime_error("Invalid linked prefab group.");
    const auto resolved=PlacementBindings(definition,bindings);
    const auto prefix=PlacementPrefix();
    auto prepared=PreparePlacement(group.empty()?definition:Prefabs::WithoutInstanceGroupsInText(definition),frame,prefix,LevelPath(),resolved);
    auto before=SelectedIdentities(); PastedAssembly pasted;
    try
    {
        Transaction transaction(group.empty()?"Place actor assembly":"Place linked prefab");
        pasted=PasteAssembly(prepared,prefix,group);
        transaction.Commit();
    }
    catch(...) { Select(before); throw; }
    Select(pasted.members);
    return {{"id",Id()},{"assembly",definition.at("id")},{"members",pasted.members},{"names",pasted.names},{"tags",prepared.value("tags",Json::object())},{"position",frame.position},{"rotation",frame.rotation}};
}
Json PrefabActors()
{
    // One pass: every live actor filed in a linked prefab's group.
    // The Group field's offset by class: looking the property up for each of a
    // big map's actors would walk every class chain thousands of times.
    Json result=Json::array(); std::map<Address,int> offsets;
    for(auto a:LiveActors())
    {
        const auto type=Read<Address>(a+0x24); auto found=offsets.find(type);
        if(found==offsets.end()) { auto p=Property(a,"Group"); found=offsets.emplace(type,p && IsA(p,"NameProperty")?Read<int>(p+0x3c):-1).first; }
        if(found->second<0) continue;
        const auto groups=Name(Read<int>(a+found->second));
        if(Fold(groups)=="none") continue;
        if(Prefabs::InstanceGroups(groups).empty()) continue;
        auto j=Identity(a); j["name"]=NameOf(a); j["groups"]=groups; j["selected"]=(Read<unsigned>(a+0x2f4)&0x40)!=0;
        j["position"]=Position(a); j["rotation"]=RotationOf(a);
        result.push_back(std::move(j));
    }
    return result;
}
std::vector<std::string> SelectedGroups()
{
    std::vector<std::string> result;
    for(auto a:LiveActors()) if((Read<unsigned>(a+0x2f4)&0x40) && Property(a,"Group")) result.push_back(NameField(a,"Group"));
    return result;
}
Json RebuildPrefabInstances(const Json& definition,const Json& jobs)
{
    if(!jobs.is_array() || jobs.empty()) return Json::array();
    if(!pasteHookReady || insertionText) throw std::runtime_error("Native actor insertion is unavailable or busy.");
    // Every copy keeps the external actors the edited instance points at: an
    // object binding names that actor, an Event binding its Tag.
    std::map<std::string,std::string> bindings;
    for(const auto& binding:definition.at("bindings")) bindings[binding.at("id").get<std::string>()]=binding.at("kind")=="object" && !Find(binding.at("path").get<std::string>(),true)?std::string{}:binding.at("path").get<std::string>();
    std::map<std::string,std::string> checked;
    for(const auto& [id,value]:bindings) checked[id]=""; // Unbound passes the checks; the values above go straight to PreparePlacement.
    PlacementBindings(definition,checked);
    const auto clean=Prefabs::WithoutInstanceGroupsInText(definition);
    struct Pending { std::string prefix,group; Json prepared,members; Pose frame; };
    std::vector<Pending> pending;
    for(const auto& job:jobs)
    {
        Pending p; p.prefix=PlacementPrefix(); p.group=job.at("group").get<std::string>(); p.members=job.at("members");
        if(!Prefabs::IsInstanceGroup(p.group)) throw std::runtime_error("Invalid linked prefab group.");
        p.frame={job.at("position").get<Vector>(),job.at("rotation").get<Rotation>()};
        p.prepared=PreparePlacement(clean,p.frame,p.prefix,LevelPath(),bindings);
        pending.push_back(std::move(p));
    }
    const auto before=SelectedIdentities();
    Json results=Json::array();
    try
    {
        // As Smooth Staircase does around its deletes and pastes: the collision
        // hash is detached until the copies are in place (StairSmoothNative.inl).
        DetachedCollisionHash hash;
        Transaction transaction("Update linked prefab");
        for(const auto& p:pending)
        {
            const ActorIndex index; std::vector<Address> old;
            for(const auto& id:p.members) if(auto a=index.Resolve(id)) old.push_back(a);
            if(!old.empty())
            {
                Json identities=Json::array();
                for(auto a:old){Modify(a);Write(a+0x2f0,Read<unsigned>(a+0x2f0)&~kCollideActors);identities.push_back(Identity(a));}
                Select(identities);
                if(!Exec("ACTOR DELETE")) throw std::runtime_error("Could not delete a linked instance's actors.");
            }
            auto pasted=PasteAssembly(p.prepared,p.prefix,p.group);
            results.push_back({{"members",pasted.members},{"names",pasted.names},{"tags",p.prepared.value("tags",Json::object())},{"position",p.frame.position},{"rotation",p.frame.rotation},{"removed",old.size()}});
        }
        Select(before);
        transaction.Commit();
    }
    catch(...){insertionText=nullptr;try{Select(before);}catch(...){}throw;}
    Redraw();
    return results;
}
