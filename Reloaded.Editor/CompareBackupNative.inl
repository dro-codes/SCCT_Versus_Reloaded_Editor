// Included after MapAuthoringNative.inl; File > Compare with Backup (CompareBackupModel.h). Runs on the editor UI thread.
namespace
{
    // UObject::LoadPackage (PackageLoadFixModel.h): a package by file, every export loaded, named by the file when
    // no outer is given. UObject::ResetLoaders(Package, DynamicOnly, ForceLazyLoad): MAP LOAD calls it at
    // 0x1100d74c, (MyLevel, 0, 0), before it loads a map over MyLevel; it frees the package's linker (and its file)
    // and clears other linkers' imports of it.
    using CompareLoadPackage=Address(__cdecl*)(Address,const char*,unsigned);
    using CompareResetLoaders=void(__cdecl*)(Address,int,int);
    constexpr Address kCompareLoadPackage=0x10FB1EB0,kCompareResetLoaders=0x10FA9F50;
    constexpr unsigned kCompareNoWarn=2; // LOAD_NoWarn, as the Sound Browser's refresh loads packages
    struct CompareLevelScope
    {
        explicit CompareLevelScope(Address level){levelOverride=level;}
        ~CompareLevelScope(){levelOverride=0;}
    };
    // One actor or particle component, as CompareBackupModel.h describes the entries.
    Json CompareEntry(Address a,const std::string& package,const std::string& owner)
    {
        auto identity=Identity(a);
        Json entry={{"key",Compare::Key(identity.at("path").get<std::string>(),package)},{"actor",identity},{"values",InspectResolved(a).at("values")}};
        if(!owner.empty())entry["owner"]=owner;
        if(!IsA(a,"Actor"))return entry;
        entry["tag"]=NameField(a,"Tag");
        if(IsA(a,"Brush"))entry["brush"]=true;
        else
        {
            Json look=Json::object();
            for(const char* name:{"StaticMesh","DrawType","DrawScale","DrawScale3D"})
                if(auto p=Property(a,name))try{look[name]=MagicValue(p,a+Read<int>(p+0x3c));}catch(const std::exception&){}
            if(!look.empty())entry["appearance"]=std::move(look);
        }
        if(IsA(a,"PlayerStart"))
            if(auto p=Property(a,"TeamNumber"))try{entry["team"]=MagicValue(p,a+Read<int>(p+0x3c));}catch(const std::exception&){}
        return entry;
    }
    // Every actor Map JSON export lists (not the LevelInfo, the builder brush or cameras) and the particle
    // components of each emitter, of whichever level Level() is. The stock maps' emitters list components that
    // belong to the map's package rather than to the emitter (MyLevel.SpriteEmitter12): they are read too, marked
    // shared, as Map JSON cannot set one back on its own.
    Json CompareEntries()
    {
        const auto package=LevelPath();const auto packageObject=Read<Address>(Level()+0x18);Json out=Json::array();
        const auto live=LiveActors();std::set<Address> seen;
        for(size_t i=0;i<live.size();++i)
        {
            const auto a=live[i];
            if(i<2 || IsA(a,"Camera"))continue;
            Json entry;
            try{entry=CompareEntry(a,package,{});}
            catch(const std::exception& e){Logger::log("Compare with Backup: left out "+Path(a)+": "+e.what());continue;}
            const auto key=entry.at("key").get<std::string>();out.push_back(std::move(entry));
            if(!IsA(a,"Emitter"))continue;
            for(const auto& ref:ReferencesOf(a,false))
            {
                if(!ref.target || !IsA(ref.target,"ParticleEmitter"))continue;
                const auto outer=Read<Address>(ref.target+0x18);
                if((outer!=a && outer!=packageObject) || !seen.insert(ref.target).second)continue;
                try{auto part=CompareEntry(ref.target,package,key);if(outer!=a)part["shared"]=true;out.push_back(std::move(part));}catch(const std::exception&){}
            }
        }
        return out;
    }
    std::filesystem::path CompareFolder(){return Directory()/"compare";}
    // The objects inside a package (by their outermost outer). With release, RF_Standalone (0x80000) comes off them,
    // which the editor's garbage collection otherwise keeps (EmitterPreview.cpp): nothing outside the copy uses them.
    size_t CompareInside(Address package,bool release)
    {
        size_t count=0;
        for(auto at:Array(0x11697B70))
        {
            auto object=Read<Address>(at);if(!object || object==package)continue;
            auto outer=object;for(int depth=0;outer && Read<Address>(outer+0x18) && depth<64;++depth)outer=Read<Address>(outer+0x18);
            if(outer!=package)continue;
            ++count;if(release)Write(object+0x1c,Read<unsigned>(object+0x1c)&~0x80000u);
        }
        return count;
    }
}
Json CompareWithMapFile(const std::filesystem::path& file)
{
    const auto e=Engine();
    const auto buffer=Read<Address>(e+0x148);
    if(!buffer || Read<int>(buffer+0x44)!=0)throw std::runtime_error("Finish the current editor operation first.");
    std::error_code error;
    if(!std::filesystem::is_regular_file(file,error))throw std::runtime_error("Cannot find "+file.string());
    const auto open=LevelPath();
    // The copy is loaded from a file of its own, under a package name nothing else uses: never over MyLevel, a
    // loaded package or the chosen file (which then stays free to open, save over or remove).
    const auto folder=CompareFolder();std::filesystem::create_directories(folder,error);
    for(std::filesystem::directory_iterator it(folder,error),end;!error && it!=end;it.increment(error))
    {std::error_code ignored;std::filesystem::remove(it->path(),ignored);} // A copy an earlier comparison could not free stays until next time.
    std::string package;std::filesystem::path copy;
    for(unsigned n=0;;++n)
    {
        package="CompareCopy"+std::to_string(GetTickCount()%100000)+"_"+std::to_string(n);copy=folder/(package+".sdc");
        if(!Find(package) && !std::filesystem::exists(copy,error))break;
        if(n>50)throw std::runtime_error("Could not choose a name for the copy.");
    }
    if(!CopyFileW(file.c_str(),copy.c_str(),TRUE))throw std::runtime_error("Could not copy "+file.string()+" (error "+std::to_string(GetLastError())+").");
    struct Unload
    {
        std::filesystem::path copy;std::string name;Address package=0;
        ~Unload()
        {
            try
            {
                size_t loaded=0,left=0;
                if(package)
                {
                    loaded=CompareInside(package,true);
                    Write(package+0x1c,Read<unsigned>(package+0x1c)&~0x80000u);
                    reinterpret_cast<CompareResetLoaders>(kCompareResetLoaders)(package,0,0);
                }
                Exec("OBJ GARBAGE");
                if(package)
                {
                    // A freed package leaves no object of that name; one still there holds what is left.
                    const auto still=Find(name);left=still?CompareInside(still,false):0;
                    Logger::log("Compare with Backup: unloaded the copy ("+std::to_string(loaded)+" objects)"+(still?"; "+std::to_string(left)+" are still loaded":std::string{}));
                }
            }
            catch(...){}
            std::error_code ignored;std::filesystem::remove(copy,ignored);
        }
    } unload{copy,package};
    const auto started=std::chrono::steady_clock::now();
    const auto loaded=reinterpret_cast<CompareLoadPackage>(kCompareLoadPackage)(0,copy.string().c_str(),kCompareNoWarn);
    if(!loaded)throw std::runtime_error("The editor could not load "+file.string()+".");
    unload.package=loaded;
    Address level=0;
    for(auto at:Array(0x11697B70))
        if(auto object=Read<Address>(at);object && Read<Address>(object+0x18)==loaded && IsA(object,"Level")){level=object;break;}
    if(!level || Array(level+0x2c).size()<2)throw std::runtime_error(file.string()+" holds no map.");
    const auto creatable=EventClasses(),parts=EventAssets("Engine.ParticleEmitter",true);
    std::set<std::string> classes,components;
    for(const auto& c:creatable)if(!c.at("brush").get<bool>())classes.insert(c.at("class").get<std::string>());
    for(const auto& c:parts)if(c.at("path")!="Engine.ParticleEmitter")components.insert(c.at("path").get<std::string>());
    Json backup;
    {
        CompareLevelScope scope(level);
        backup=CompareEntries();
    }
    for(auto& entry:backup)
    {
        entry=Compare::Rename(entry,package,open);
        const auto type=entry.at("actor").at("class").get<std::string>();
        entry["creatable"]=entry.contains("owner")?components.count(type)>0:classes.count(type)>0;
    }
    const auto read=std::chrono::steady_clock::now();
    auto current=CompareEntries();
    auto rows=Compare::Diff(current,backup);
    Logger::log("Compare with Backup: "+file.string()+" ("+std::to_string(backup.size())+" objects) against the open map ("+std::to_string(current.size())+") in "
                +std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(read-started).count())+" ms + "
                +std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-read).count())+" ms: "+Compare::Tally(rows));
    auto tally=Compare::Tally(rows);
    return {{"rows",std::move(rows)},{"tally",tally},{"current",std::move(current)},{"backup",std::move(backup)},{"level",open},
            {"file",Authoring::Utf8Text(file)},{"map",AuthoringMapKey()},{"identity",LevelIdentity()},{"generation",MapGeneration()},{"revision",ContentRevision()}};
}
Json BringBackFromComparison(const Json& comparison,const std::vector<size_t>& rows)
{
    if(comparison.at("identity").get<uintptr_t>()!=LevelIdentity() || comparison.at("generation").get<unsigned>()!=MapGeneration() || comparison.at("revision").get<unsigned>()!=ContentRevision()
       || comparison.at("map").get<std::string>()!=AuthoringMapKey())
        throw std::runtime_error("The map changed since the comparison. Compare again to bring back what differs now.");
    const auto name=Authoring::Utf8Path(comparison.at("file").get<std::string>()).filename();
    auto document=Compare::Changes(comparison,rows,AuthoringMapKey(),"Bring back from "+Authoring::Utf8Text(name));
    auto result=ApplyMapAuthoring(document,"Bring back from backup");
    result["document"]=std::move(document);
    return result;
}
