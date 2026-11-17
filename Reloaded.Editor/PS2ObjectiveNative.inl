// The original PS2 ND133 console is a native SComputerObjectiveTrigger child.
// Resources travel inside the DLL so the existing DLL-only updater supplies
// the same packages as a manual install. Stock SBase and nd133 are never changed.
namespace
{
    using PS2Bytes=std::vector<unsigned char>;
    std::string PS2Sha256(const PS2Bytes& data)
    {
        BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
        unsigned char digest[32]{};
        const bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0
            && BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0
            && BCryptHashData(hash,const_cast<PUCHAR>(data.data()),static_cast<ULONG>(data.size()),0)>=0
            && BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
        if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
        if(!ok)throw std::runtime_error("Cannot verify the PS2 objective resources.");
        const char hex[]="0123456789abcdef";std::string out;
        for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;
    }
    PS2Bytes PS2Read(const std::filesystem::path& path)
    {
        std::ifstream input(path,std::ios::binary);
        if(!input)throw std::runtime_error("Cannot read "+path.string());
        return PS2Bytes(std::istreambuf_iterator<char>(input),{});
    }
    PS2Bytes PS2Resource(int id,const char* expected)
    {
        HMODULE module=nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&PS2Resource),&module);
        auto resource=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);
        auto loaded=resource?LoadResource(module,resource):nullptr;
        auto data=loaded?static_cast<const unsigned char*>(LockResource(loaded)):nullptr;
        const auto size=resource?SizeofResource(module,resource):0;
        if(!data || !size)throw std::runtime_error("This RE+ build has no PS2 objective resources.");
        PS2Bytes bytes(data,data+size);
        if(PS2Sha256(bytes)!=expected)throw std::runtime_error("The bundled PS2 objective resource failed verification.");
        return bytes;
    }
    std::filesystem::path PS2BoundedPath(const std::filesystem::path& root,const std::filesystem::path& relative)
    {
        const auto target=std::filesystem::weakly_canonical(root/relative);
        const auto inside=target.lexically_relative(root);
        if(inside.empty() || inside.is_absolute())throw std::runtime_error("PS2 objective resource path is outside the game folder.");
        for(const auto& part:inside)if(part=="..")throw std::runtime_error("PS2 objective resource path is outside the game folder.");
        return target;
    }
    void PS2Install(const std::filesystem::path& target,const PS2Bytes& bytes)
    {
        if(std::filesystem::exists(target))
        {
            auto previous=PS2Read(target);if(previous==bytes)return;
            if(Find(target.stem().string()))throw std::runtime_error("A different "+target.filename().string()+" is already loaded. Restart RE+ before placing the PS2 Vial Case.");
            const auto backup=std::filesystem::path(target.wstring()+L".replus-"+std::filesystem::path(PS2Sha256(previous).substr(0,12)).wstring()+L".bak");
            if(!std::filesystem::exists(backup))std::filesystem::copy_file(target,backup);
            if(PS2Read(backup)!=previous)throw std::runtime_error("Cannot verify the resource backup "+backup.string());
        }
        std::filesystem::create_directories(target.parent_path());
        const auto temporary=std::filesystem::path(target.wstring()+L".replus-new");
        if(std::filesystem::exists(temporary))throw std::runtime_error("A PS2 objective installation is already pending: "+temporary.string());
        {
            std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
            if(!output)throw std::runtime_error("Cannot write "+temporary.string());
        }
        if(PS2Read(temporary)!=bytes)throw std::runtime_error("The PS2 objective resource was not written intact.");
        if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot install "+target.string());
        if(PS2Read(target)!=bytes)throw std::runtime_error("The installed PS2 objective resource failed verification.");
        Logger::log("PS2 objective: installed "+target.string());
    }
    void EnsurePS2ObjectiveResources()
    {
        const auto root=std::filesystem::weakly_canonical(Directory().parent_path().parent_path());
        const auto animation=PS2BoundedPath(root,"Packages/Animations/nd133_1mesh.ukx");
        if(!std::filesystem::is_regular_file(animation) || PS2Sha256(PS2Read(animation))!="2394167a4c1149d0503103a5575147aeedcad5e69c9c40f0ca39c1eeda148928")
            throw std::runtime_error("PS2 Vial Case needs the original Chaos Theory Versus nd133_1mesh.ukx. This installation's animation package is missing or differs from the verified asset.");
        const auto materials=PS2BoundedPath(root,"Packages/Textures/P2_nd133.utx");
        const auto script=PS2BoundedPath(root,"Packages/StaticMeshes/P2_PS2Objectives.usx");
        PS2Install(materials,PS2Resource(IDR_PS2_OBJECTIVE_MATERIALS,"8cc92fd095a703c8c75ec8815982d6075c4266f832e484088297cbdd0cc30654"));
        PS2Install(script,PS2Resource(IDR_PS2_OBJECTIVE_SCRIPT,"d65019c7b514701dc908c0ace29460118eb9bdabf056d766e1b35df06668c9c3"));
        for(const auto& package:{animation,materials,script})
            if(!Exec("OBJ LOAD FILE=\""+package.string()+"\""))throw std::runtime_error("Cannot load "+package.filename().string());
        auto cls=Find("P2_PS2Objectives.PS2Computer");
        if(!cls || !IsA(cls,"Class"))throw std::runtime_error("Cannot load the PS2 Vial Case objective class.");
    }
}
