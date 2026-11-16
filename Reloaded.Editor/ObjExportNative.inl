// File > Export > Map to OBJ (ObjExportModel.h): reads the built level's BSP,
// its static mesh actors and their materials' textures from the editor, and
// writes <Map>.obj, <Map>.mtl and textures/*.png. Nothing in the map changes;
// the only engine calls are the lazy loaders of mesh triangles and texture
// mips that are not loaded yet, which load them from their packages.
namespace
{
    namespace OE = Workflow::ObjExport;

    // UModel arrays (as MinimapNative / RenderBudgetNative read them). A
    // node starts with its FPlane; the surface holds the material, the flags
    // and the indices of its base point, normal and texture U/V vectors.
    constexpr size_t kOeModelNodes=0x54,kOeModelVerts=0x64,kOeModelVectors=0x74,kOeModelPoints=0x84,kOeModelSurfs=0x94,
                     kOeModelNumZones=0x114,kOeModelZones=0x118,kOeZoneStride=0x10;
    constexpr size_t kOeNodeStride=0x5c,kOeNodePool=0x28,kOeNodeSurf=0x2c,kOeNodeZones=0x58,kOeNodeCount=0x5a;
    constexpr size_t kOeSurfStride=0x2c,kOeSurfMaterial=0x10,kOeSurfFlags=0x14,kOeSurfBase=0x18,kOeSurfNormal=0x1a,kOeSurfU=0x1c,kOeSurfV=0x1e;
    // PF_Invisible, PF_FakeBackdrop (the sky shows there), PF_Portal,
    // PF_AntiPortal: not drawn. PF_Selected marks a selected surface.
    constexpr unsigned kOeHiddenSurface=0x1|0x80|0x04000000|0x08000000,kOeSelectedSurface=0x02000000;
    // UStaticMesh, as the stock ASE exporter (0x1105D600) reads it:
    // RawTriangles is a TLazyArray (loader at +0x184, data +0x190, count
    // +0x194) of 0x104-byte FStaticMeshTriangle: three FVector corners, then
    // UVs[corner][8 channels] at +0x24 (0x40 per corner), MaterialIndex
    // +0xF0 and the smoothing-group mask +0xF4.
    constexpr size_t kOeMeshLoader=0x184,kOeMeshTriangles=0x190,kOeMeshTriangleCount=0x194,kOeTriangleStride=0x104,
                     kOeTriangleUV=0x24,kOeCornerUV=0x40,kOeTriangleMaterial=0xf0,kOeTriangleSmoothing=0xf4;
    // FMipmap (40 bytes): USize +4, VSize +8, its data a TLazyArray<BYTE>
    // with the loader at +0x10, data +0x1C, byte count +0x20. A UPalette's
    // Colors (FColor B, G, R, A) are at +0x28.
    constexpr size_t kOeMipStride=40,kOeMipUSize=4,kOeMipVSize=8,kOeMipLoader=0x10,kOeMipData=0x1c,kOeMipBytes=0x20,kOePaletteColors=0x28;
    constexpr unsigned char kOeDrawStaticMesh=8;

    bool OeLoad(Address loader)
    {
        __try
        {
            auto table=*reinterpret_cast<Address*>(loader);
            reinterpret_cast<void(__thiscall*)(void*)>(*reinterpret_cast<Address*>(table))(reinterpret_cast<void*>(loader));
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    std::vector<std::uint8_t> OeBytes(Address at,size_t size)
    {
        std::vector<std::uint8_t> out(size);
        if(size && (!at || !CopyMemorySafe(out.data(),reinterpret_cast<void*>(at),size)))throw std::runtime_error("Editor data could not be read.");
        return out;
    }
    // A TArray's items copied out in one go.
    template<class T> std::vector<T> OeArray(Address at,size_t stride=sizeof(T),int limit=4000000)
    {
        const Address data=Read<Address>(at);const int count=Read<int>(at+4);
        if(count<0 || count>limit || (count && !data))throw std::runtime_error("Invalid editor array.");
        std::vector<T> out(count);
        if(stride==sizeof(T)){if(count && !CopyMemorySafe(out.data(),reinterpret_cast<void*>(data),count*sizeof(T)))throw std::runtime_error("Editor data could not be read.");}
        else{auto raw=OeBytes(data,count*stride);for(int i=0;i<count;++i)memcpy(&out[i],raw.data()+i*stride,sizeof(T));}
        return out;
    }
    int OeOffset(Address object,const char* name)
    {
        auto p=Property(object,name);return p?Read<int>(p+0x3c):-1;
    }
    // A BoolProperty's value (UBoolProperty's bit mask at +0x64); false
    // when the object has no such property.
    bool OeFlag(Address object,const char* name)
    {
        auto p=Property(object,name);
        return p && IsA(p,"BoolProperty") && (Read<unsigned>(object+Read<int>(p+0x3c))&Read<unsigned>(p+0x64));
    }

    // A texture's top mip, copied out of the editor for a worker to decode.
    struct OeTextureData
    {
        int format=-1,width=0,height=0;
        std::vector<std::uint8_t> bytes;
        std::vector<std::array<std::uint8_t,4>> palette;
    };

    struct OeBuilder
    {
        OE::Scene scene;
        std::map<Address,int> materials,textures,meshes;
        std::vector<Address> textureObjects;
        std::map<Address,std::pair<int,int>> sizes; // texture -> USize, VSize
        std::set<std::string> materialNames,textureNames,objectNames;
        bool withTextures=true;

        // The texture a material draws with, as Blender can show it: a
        // Shader's Diffuse, a modifier's or a FinalBlend's Material, a
        // Combiner's first, a MaterialSwitch's current one. `masked` is set
        // when a FinalBlend on the way cuts out by alpha.
        Address BaseTexture(Address m,int depth,bool& masked)
        {
            if(!m || depth>8)return 0;
            if(IsA(m,"Texture"))return m;
            if(!IsA(m,"Material"))return 0;
            if(IsA(m,"FinalBlend"))
                if(auto p=Property(m,"AlphaTest"))if(Read<unsigned>(m+Read<int>(p+0x3c))&Read<unsigned>(p+0x64))masked=true;
            if(IsA(m,"MaterialSwitch"))
            {
                const int at=OeOffset(m,"Materials"),current=OeOffset(m,"Current");
                if(at>=0)
                {
                    auto list=OeArray<Address>(m+at,4,4096);
                    if(!list.empty())
                    {
                        int index=current>=0?Read<int>(m+current):0;
                        index=std::clamp(index,0,static_cast<int>(list.size())-1);
                        if(auto t=BaseTexture(list[index],depth+1,masked))return t;
                    }
                }
            }
            for(const char* field:{"Diffuse","Material","Material1","Material2","SelfIllumination"})
                if(auto p=Property(m,field))
                    if(IsA(p,"ObjectProperty"))
                        if(auto t=BaseTexture(Read<Address>(m+Read<int>(p+0x3c)),depth+1,masked))return t;
            return 0;
        }
        int Texture(Address t,bool masked)
        {
            if(auto found=textures.find(t);found!=textures.end())
            {
                if(masked)scene.textures[found->second].alpha=true;
                return found->second;
            }
            const bool alpha=masked || OeFlag(t,"bMasked") || OeFlag(t,"bAlphaTexture");
            const int index=static_cast<int>(scene.textures.size());
            textures.emplace(t,index);
            textureObjects.push_back(t);
            OE::Texture texture;
            texture.alpha=alpha;
            if(withTextures)texture.file="textures/"+OE::UniqueName(OE::SafeName(Path(t)),textureNames)+".png";
            scene.textures.push_back(texture);
            int u=128,v=128;
            if(const int at=OeOffset(t,"USize");at>=0)u=std::max(1,Read<int>(t+at));
            if(const int at=OeOffset(t,"VSize");at>=0)v=std::max(1,Read<int>(t+at));
            sizes[t]={u,v};
            return index;
        }
        int Material(Address m)
        {
            if(!m)return -1;
            if(auto found=materials.find(m);found!=materials.end())return found->second;
            bool masked=false;
            const auto t=BaseTexture(m,0,masked);
            OE::Material material;
            material.name=OE::UniqueName(OE::SafeName(t==m?Path(m):t?NameOf(m)+"__"+NameOf(t):Path(m)),materialNames);
            material.texture=t?Texture(t,masked):-1;
            const int index=static_cast<int>(scene.materials.size());
            scene.materials.push_back(material);
            materials.emplace(m,index);
            return index;
        }
        std::pair<int,int> Size(Address m)
        {
            bool masked=false;
            auto t=BaseTexture(m,0,masked);
            auto found=t?sizes.find(t):sizes.end();
            return found==sizes.end()?std::pair<int,int>{128,128}:found->second;
        }

        // The built BSP: every drawn polygon, its texture coordinates and its
        // node's plane. Returns the polygons left out as not drawn.
        int Bsp(bool selectedOnly)
        {
            auto model=Read<Address>(Level()+0x13c);
            if(!model)return 0;
            const auto points=OeArray<std::array<float,3>>(model+kOeModelPoints);
            const auto vectors=OeArray<std::array<float,3>>(model+kOeModelVectors);
            const auto verts=OeArray<std::array<std::uint8_t,8>>(model+kOeModelVerts);
            const auto surfs=OeArray<std::array<std::uint8_t,kOeSurfStride>>(model+kOeModelSurfs);
            const auto nodes=OeArray<std::array<std::uint8_t,kOeNodeStride>>(model+kOeModelNodes);
            scene.points.reserve(points.size());
            for(const auto& p:points)scene.points.push_back({p[0],p[1],p[2]});
            // Zones whose ZoneInfo is a SkyZoneInfo: the room the sky is drawn from.
            std::set<int> sky;
            const int zoneCount=std::clamp(Read<int>(model+kOeModelNumZones),0,64);
            for(int z=0;z<zoneCount;++z)
                if(auto info=Read<Address>(model+kOeModelZones+z*kOeZoneStride))if(IsA(info,"SkyZoneInfo"))sky.insert(z);
            auto word=[](const std::uint8_t* at){std::uint16_t v;memcpy(&v,at,2);return v;};
            auto integer=[](const std::uint8_t* at){int v;memcpy(&v,at,4);return v;};
            std::map<Address,std::pair<int,int>> materialSizes;
            int hidden=0;
            for(const auto& node:nodes)
            {
                const int count=node[kOeNodeCount],pool=integer(&node[kOeNodePool]),surface=integer(&node[kOeNodeSurf]);
                if(count<3 || pool<0 || pool+count>static_cast<int>(verts.size()) || surface<0 || surface>=static_cast<int>(surfs.size()))continue;
                const auto& surf=surfs[surface];
                const unsigned flags=static_cast<unsigned>(integer(&surf[kOeSurfFlags]));
                if(flags&kOeHiddenSurface){++hidden;continue;}
                if(selectedOnly && !(flags&kOeSelectedSurface))continue;
                OE::Polygon poly;
                bool sane=true;
                for(int i=0;i<count && sane;++i)
                {
                    const int index=MapCheck::PointIndex(word(verts[pool+i].data()),points.size());
                    if(index<0){sane=false;break;}
                    for(float v:points[index])if(!std::isfinite(v) || std::abs(v)>1000000)sane=false;
                    poly.points.push_back(index);
                }
                if(!sane)continue;
                const Address material=static_cast<Address>(static_cast<unsigned>(integer(&surf[kOeSurfMaterial])));
                poly.material=Material(material);
                auto sized=materialSizes.find(material);
                if(sized==materialSizes.end())sized=materialSizes.emplace(material,material?Size(material):std::pair<int,int>{128,128}).first;
                const int base=word(&surf[kOeSurfBase]),normal=word(&surf[kOeSurfNormal]),tu=word(&surf[kOeSurfU]),tv=word(&surf[kOeSurfV]);
                const bool vectorsOk=base<static_cast<int>(points.size()) && tu<static_cast<int>(vectors.size()) && tv<static_cast<int>(vectors.size());
                for(int p:poly.points)
                {
                    if(!vectorsOk){poly.uvs.push_back({0,0});continue;}
                    const OE::V3 d=OE::Sub(scene.points[p],scene.points[base]);
                    const OE::V3 u{vectors[tu][0],vectors[tu][1],vectors[tu][2]},v{vectors[tv][0],vectors[tv][1],vectors[tv][2]};
                    poly.uvs.push_back({OE::Dot(d,u)/sized->second.first,OE::Dot(d,v)/sized->second.second});
                }
                // The node's own plane says which way the polygon faces; its
                // first three corners often lie in a line. The surface's
                // normal stands in for a plane that is not a unit vector.
                std::array<float,4> plane;memcpy(plane.data(),node.data(),16);
                OE::V3 n{plane[0],plane[1],plane[2]};
                if(!std::isfinite(n[0]) || !std::isfinite(n[1]) || !std::isfinite(n[2]) || std::abs(OE::Length(n)-1)>0.01)
                    n=normal<static_cast<int>(vectors.size())?OE::V3{vectors[normal][0],vectors[normal][1],vectors[normal][2]}:OE::V3{0,0,1};
                poly.normal=n;
                poly.sky=sky.count(node[kOeNodeZones]) || sky.count(node[kOeNodeZones+1]);
                scene.polygons.push_back(std::move(poly));
            }
            return hidden;
        }

        int Mesh(Address mesh)
        {
            if(auto found=meshes.find(mesh);found!=meshes.end())return found->second;
            OE::Mesh out;
            if(Read<int>(mesh+kOeMeshTriangleCount)<=0)OeLoad(mesh+kOeMeshLoader);
            const int count=Read<int>(mesh+kOeMeshTriangleCount);
            if(count>0 && count<2000000)
            {
                const auto raw=OeBytes(Read<Address>(mesh+kOeMeshTriangles),static_cast<size_t>(count)*kOeTriangleStride);
                std::map<std::array<float,3>,int> positions;
                std::map<std::array<float,2>,int> uvs;
                for(int i=0;i<count;++i)
                {
                    const std::uint8_t* t=raw.data()+static_cast<size_t>(i)*kOeTriangleStride;
                    OE::Mesh::Triangle triangle;
                    bool sane=true;
                    for(int c=0;c<3;++c)
                    {
                        std::array<float,3> p;memcpy(p.data(),t+c*12,12);
                        std::array<float,2> uv;memcpy(uv.data(),t+kOeTriangleUV+c*kOeCornerUV,8);
                        for(float v:p)if(!std::isfinite(v) || std::abs(v)>1000000)sane=false;
                        for(float& v:uv)if(!std::isfinite(v))v=0;
                        auto [at,added]=positions.emplace(p,static_cast<int>(out.positions.size()));
                        if(added)out.positions.push_back({p[0],p[1],p[2]});
                        auto [uat,uadded]=uvs.emplace(uv,static_cast<int>(out.uvs.size()));
                        if(uadded)out.uvs.push_back({uv[0],uv[1]});
                        triangle.v[c]=at->second;triangle.t[c]=uat->second;
                    }
                    if(!sane)continue;
                    memcpy(&triangle.slot,t+kOeTriangleMaterial,4);
                    memcpy(&triangle.smoothing,t+kOeTriangleSmoothing,4);
                    out.triangles.push_back(triangle);
                }
            }
            const int index=static_cast<int>(scene.meshes.size());
            scene.meshes.push_back(std::move(out));
            meshes.emplace(mesh,index);
            return index;
        }

        // Static mesh actors drawn in game (DrawType StaticMesh, not bHidden).
        int Actors(bool selectedOnly)
        {
            struct Layout{int drawType=-1,hidden=-1,selected=-1,mesh=-1,skins=-1,rotation=-1,scale=-1,scale3=-1,prePivot=-1;unsigned hiddenMask=0,selectedMask=0;size_t skinStride=4;};
            std::map<Address,Layout> layouts;
            int count=0;
            for(auto actor:LiveActors())
            {
                const auto type=Read<Address>(actor+0x24);
                auto found=layouts.find(type);
                if(found==layouts.end())
                {
                    Layout l;
                    for(auto p:Properties(type))
                    {
                        const auto name=Fold(NameOf(p));const int offset=Read<int>(p+0x3c);
                        if(name=="drawtype")l.drawType=offset;
                        else if(name=="bhidden"){l.hidden=offset;l.hiddenMask=Read<unsigned>(p+0x64);}
                        else if(name=="bselected"){l.selected=offset;l.selectedMask=Read<unsigned>(p+0x64);}
                        else if(name=="staticmesh")l.mesh=offset;
                        else if(name=="skins"){l.skins=offset;if(auto inner=Read<Address>(p+0x64))l.skinStride=std::max<size_t>(4,Read<unsigned short>(inner+0x32));}
                        else if(name=="rotation")l.rotation=offset;
                        else if(name=="drawscale")l.scale=offset;
                        else if(name=="drawscale3d")l.scale3=offset;
                        else if(name=="prepivot")l.prePivot=offset;
                    }
                    found=layouts.emplace(type,l).first;
                }
                const auto& l=found->second;
                if(l.drawType<0 || l.mesh<0 || l.rotation<0 || l.scale<0 || l.scale3<0 || l.prePivot<0)continue;
                if(Read<unsigned char>(actor+l.drawType)!=kOeDrawStaticMesh)continue;
                if(l.hidden>=0 && (Read<unsigned>(actor+l.hidden)&l.hiddenMask))continue;
                if(selectedOnly && (l.selected<0 || !(Read<unsigned>(actor+l.selected)&l.selectedMask)))continue;
                const auto mesh=Read<Address>(actor+l.mesh);
                if(!mesh || !IsA(mesh,"StaticMesh"))continue;
                OE::Instance instance;
                instance.mesh=Mesh(mesh);
                if(scene.meshes[instance.mesh].triangles.empty())continue;
                const auto location=Read<std::array<float,3>>(actor+0x80),scale3=Read<std::array<float,3>>(actor+l.scale3),pivot=Read<std::array<float,3>>(actor+l.prePivot);
                const float scale=Read<float>(actor+l.scale);
                for(int i=0;i<3;++i)
                {
                    instance.placement.location[i]=location[i];
                    instance.placement.scale[i]=static_cast<double>(scale)*scale3[i];
                    instance.placement.prePivot[i]=pivot[i];
                }
                instance.placement.rotation=Read<std::array<int,3>>(actor+l.rotation);
                // Each slot's material: the mesh's own (StaticMeshMaterial.Material
                // first in each entry), or the actor's Skins entry when set.
                std::vector<Address> slots;
                if(auto p=Property(mesh,"Materials"))
                {
                    auto inner=Read<Address>(p+0x64);const size_t stride=inner?Read<unsigned short>(inner+0x32):0;
                    if(stride>=4)for(auto entry:Array(mesh+Read<int>(p+0x3c),stride))slots.push_back(Read<Address>(entry));
                }
                if(l.skins>=0)
                {
                    auto skins=Array(actor+l.skins,l.skinStride);
                    for(size_t i=0;i<skins.size();++i)
                        if(auto skin=Read<Address>(skins[i])){if(i>=slots.size())slots.resize(i+1,0);slots[i]=skin;}
                }
                for(auto m:slots)instance.slots.push_back(Material(m));
                instance.name=OE::UniqueName(OE::SafeName(NameOf(actor)),objectNames);
                scene.instances.push_back(std::move(instance));
                ++count;
            }
            return count;
        }

        // The top mip of a texture, loading it when the editor has not yet.
        OeTextureData TextureData(Address t)
        {
            OeTextureData data;
            const int mips=OeOffset(t,"Mips"),format=OeOffset(t,"Format");
            if(mips<0 || format<0)throw std::runtime_error("no Mips");
            data.format=Read<unsigned char>(t+format);
            if(!OE::Readable(data.format))throw std::runtime_error("format "+std::to_string(data.format));
            auto list=Array(t+mips,kOeMipStride);
            if(list.empty())throw std::runtime_error("no mips");
            const Address mip=list[0];
            data.width=Read<int>(mip+kOeMipUSize);data.height=Read<int>(mip+kOeMipVSize);
            if(!Read<Address>(mip+kOeMipData))OeLoad(mip+kOeMipLoader);
            const Address bytes=Read<Address>(mip+kOeMipData);const int size=Read<int>(mip+kOeMipBytes);
            const size_t wanted=OE::MipBytes(data.format,data.width,data.height);
            if(!bytes || size<0 || static_cast<size_t>(size)<wanted || wanted==0)throw std::runtime_error("no data");
            data.bytes=OeBytes(bytes,wanted);
            if(data.format==OE::kP8)
                if(const int at=OeOffset(t,"Palette");at>=0)
                    if(auto palette=Read<Address>(t+at))data.palette=OeArray<std::array<std::uint8_t,4>>(palette+kOePaletteColors,4,256);
            return data;
        }
    };

    std::string OeDeflate(const std::string& raw)
    {
        uLongf size=compressBound(static_cast<uLong>(raw.size()));
        std::string out(size,'\0');
        if(compress2(reinterpret_cast<Bytef*>(out.data()),&size,reinterpret_cast<const Bytef*>(raw.data()),static_cast<uLong>(raw.size()),6)!=Z_OK)
            throw std::runtime_error("zlib failed");
        out.resize(size);
        return out;
    }
}

Json ExportMapObj(const Json& options)
{
    const auto started=std::chrono::steady_clock::now();
    const std::filesystem::path folder=std::filesystem::path(options.at("folder").get<std::string>());
    const bool withMeshes=options.value("meshes",true),withTextures=options.value("textures",true),selectedOnly=options.value("selectedOnly",false);
    const double centimetres=options.value("centimetres",1.0);
    if(folder.empty())throw std::runtime_error("Choose a folder to export to.");
    if(!std::isfinite(centimetres) || centimetres<=0 || centimetres>100000)throw std::runtime_error("The scale must be a positive number of centimetres per Unreal unit.");
    const auto file=MapFile();
    const std::string stem=OE::SafeName(file.empty()?std::string("Map"):std::filesystem::path(file).stem().string());

    OeBuilder builder;
    builder.withTextures=withTextures;
    const int hidden=builder.Bsp(selectedOnly);
    const int actors=withMeshes?builder.Actors(selectedOnly):0;
    if(builder.scene.polygons.empty() && builder.scene.instances.empty())
        throw std::runtime_error(selectedOnly?"Nothing selected to export: select static meshes or BSP surfaces, or untick Selection only."
                                             :"The map has no built geometry to export. Build it first.");

    std::filesystem::create_directories(folder);
    const std::string header=stem+" from the Chaos Theory editor, RE+ " RE_PLUS_VERSION;
    OE::Totals totals;
    {
        std::ofstream obj(folder/(stem+".obj"),std::ios::binary);
        if(!obj)throw std::runtime_error("Cannot write "+(folder/(stem+".obj")).string());
        totals=OE::WriteObj(obj,builder.scene,centimetres/100.0,stem+".mtl",header);
        if(!obj)throw std::runtime_error("Could not finish "+(folder/(stem+".obj")).string());
    }

    // Textures: read on this thread (the engine is not thread safe), decoded
    // and compressed by workers.
    int written=0;
    std::vector<std::string> failed;
    if(withTextures && !builder.textureObjects.empty())
    {
        std::filesystem::create_directories(folder/"textures");
        const size_t workers=std::max(2u,std::min(8u,std::thread::hardware_concurrency()));
        std::deque<std::pair<int,std::future<std::string>>> running;
        auto finish=[&](std::pair<int,std::future<std::string>>& job){
            auto error=job.second.get();
            if(error.empty())++written;
            else{failed.push_back(Path(builder.textureObjects[job.first])+": "+error);builder.scene.textures[job.first].file.clear();}
        };
        for(size_t i=0;i<builder.textureObjects.size();++i)
        {
            auto& texture=builder.scene.textures[i];
            OeTextureData data;
            try{data=builder.TextureData(builder.textureObjects[i]);}
            catch(const std::exception& e){failed.push_back(Path(builder.textureObjects[i])+": "+e.what());texture.file.clear();continue;}
            const auto path=folder/texture.file;
            const bool alpha=texture.alpha;
            running.emplace_back(static_cast<int>(i),std::async(std::launch::async,[data=std::move(data),path,alpha]()->std::string{
                try
                {
                    auto image=OE::Decode(data.format,data.width,data.height,data.bytes.data(),data.bytes.size(),data.palette);
                    const auto png=OE::Png(image,alpha,OeDeflate);
                    std::ofstream out(path,std::ios::binary);
                    out.write(png.data(),static_cast<std::streamsize>(png.size()));
                    return out?std::string():"cannot write "+path.string();
                }
                catch(const std::exception& e){return e.what();}
            }));
            if(running.size()>=workers){finish(running.front());running.pop_front();}
        }
        while(!running.empty()){finish(running.front());running.pop_front();}
    }
    {
        // Written last, so a texture that failed has no map line.
        std::ofstream mtl(folder/(stem+".mtl"),std::ios::binary);
        OE::WriteMtl(mtl,builder.scene,header);
        if(!mtl)throw std::runtime_error("Cannot write "+(folder/(stem+".mtl")).string());
    }
    for(const auto& line:failed)Logger::log("Export Map to OBJ: texture left out: "+line);
    size_t triangles=0;
    for(const auto& instance:builder.scene.instances)triangles+=builder.scene.meshes[instance.mesh].triangles.size();
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    Logger::log("Export Map to OBJ: "+std::to_string(builder.scene.polygons.size())+" BSP polygons, "+std::to_string(actors)+" static mesh actors ("
        +std::to_string(builder.scene.meshes.size())+" meshes), "+std::to_string(builder.scene.materials.size())+" materials, "
        +std::to_string(written)+" textures in "+std::to_string(seconds)+" s to "+folder.string());
    return {{"obj",(folder/(stem+".obj")).string()},{"mtl",(folder/(stem+".mtl")).string()},
            {"polygons",builder.scene.polygons.size()},{"hiddenPolygons",hidden},{"actors",actors},{"meshes",builder.scene.meshes.size()},
            {"meshTriangles",triangles},{"faces",totals.faces},{"vertices",totals.vertices},{"objects",totals.objects},
            {"materials",builder.scene.materials.size()},{"textures",written},{"texturesFailed",failed.size()},{"seconds",seconds}};
}
