// Native reads for the Light and Shadow map (LightShadowModel.h): the walkable
// BSP floor triangles with their lightmap UVs, the lightmap atlases they use,
// the in-game lights, the BSP for line of sight, and the static meshes (as
// blockers, and as floors with their baked vertex lighting). Included in
// WorkflowEditor.cpp after LightingBudgetNative.inl (its light-flag offsets).
// The model layout is the one RecoveredBspLighting reads.
//
// UStaticMesh (editor build, as RenderBudgetNative.inl and the offline notes
// have it): the render vertex stream's TArray at +0x60 (24 bytes a vertex:
// position, normal), the render index buffer's TArray of WORDs at +0xC4
// (a triangle list), the local bounds at +0x28. UStaticMeshInstance (an
// actor's StaticMeshInstance, made by Build Lighting): the static lights at
// +0x28, 0x14 bytes each: the light actor, then a TArray<BYTE> of one bit per
// mesh vertex, set where the light reaches that vertex.
namespace
{
    constexpr size_t kShadowNodeStride=0x5C,kShadowSectionStride=0x38,kShadowTextureStride=0x70,kShadowSurfStride=0x2C,kShadowVertexStride=0x28;
    // Invisible, not solid, fake backdrop, zone portal: no one stands on them.
    constexpr unsigned kShadowSkipFlags=0x00000001u|0x00000008u|0x00000080u|0x04000000u;
    constexpr Address kShadowLoadMip=0x111B2400;
    constexpr size_t kMeshVertices=0x60,kMeshIndices=0xC4,kMeshBounds=0x28,kInstanceLights=0x28,kInstanceLightStride=0x14;
    constexpr unsigned char kDrawStaticMeshType=8;
    bool shadowFieldsLogged=false,shadowMeshLayoutChecked=false,shadowMeshLayoutGood=true;

    float ShadowFloat(Address at)
    {
        const float v=Read<float>(at);
        if(!std::isfinite(v))throw std::runtime_error("The BSP holds a coordinate that is not a number. Rebuild the geometry.");
        return v;
    }
    LightShadow::Point ShadowPoint(Address at){return {ShadowFloat(at),ShadowFloat(at+4),ShadowFloat(at+8)};}

    // Lightmap atlases, kept while their pixels stay the same: copying every
    // atlas on every edit was most of the time on a big map.
    struct ShadowAtlasEntry { Address data=0; int width=0,height=0; uint64_t sum=0; std::shared_ptr<const LightShadow::Image> image; bool used=false; };
    std::map<Address,ShadowAtlasEntry> shadowAtlases;
    // One atlas, or none when its format is not the RGBA8 Build Lighting writes.
    std::shared_ptr<const LightShadow::Image> ShadowAtlas(Address texture)
    {
        const Address compressed=texture+0x14;
        const int w=Read<int>(compressed+0x38),h=Read<int>(compressed+0x3C);
        if(Read<unsigned char>(compressed+0x34)!=5 || w<=0 || h<=0 || w>2048 || h>2048)return nullptr;
        void* pixels=reinterpret_cast<void*(__thiscall*)(void*,int)>(kShadowLoadMip)(reinterpret_cast<void*>(compressed),0);
        const Address data=Read<Address>(compressed+0x10);
        const int bytes=Read<int>(compressed+0x14);
        if(!pixels || data!=reinterpret_cast<Address>(pixels) || bytes!=w*h*4)return nullptr;
        // A sum of a spread of texels tells a rebuilt atlas from the kept one.
        uint64_t sum=1469598103934665603ULL;
        const size_t count=size_t(w)*h;
        for(size_t k=0;k<509;++k)
        {
            uint32_t texel=0;
            if(!CopyMemorySafe(&texel,static_cast<const uint32_t*>(pixels)+(k*2654435761ULL)%count,4))return nullptr;
            sum=LightShadow::Mix(sum,&texel,4);
        }
        auto& entry=shadowAtlases[texture];
        entry.used=true;
        if(entry.image && entry.data==data && entry.width==w && entry.height==h && entry.sum==sum)return entry.image;
        auto image=std::make_shared<LightShadow::Image>();
        image->width=w;image->height=h;image->pixels.resize(count);
        if(!CopyMemorySafe(image->pixels.data(),pixels,size_t(bytes))){entry=ShadowAtlasEntry{};return nullptr;}
        entry={data,w,h,sum,image,true};
        return entry.image;
    }

    // The Actor fields the map reads, by name, once per sample.
    struct ShadowFields
    {
        int staticMesh=-1,drawType=-1,drawScale=-1,drawScale3D=-1,prePivot=-1,rotation=-1,instance=-1;
        int hiddenAt=-1,collideAt=-1,blockPlayersAt=-1;unsigned hiddenMask=0,collideMask=0,blockPlayersMask=0;
        int brightness=-1,hue=-1,saturation=-1,cone=-1,range=-1,width=-1,height=-1,nearRatio=-1;
    };
    ShadowFields ResolveShadowFields(Address actor)
    {
        ShadowFields f;
        std::string text="LightShadowMap: actor fields";
        for(auto p:Properties(Read<Address>(actor+0x24)))
        {
            const auto name=NameOf(p);
            const int offset=Read<int>(p+0x3c);
            const unsigned mask=IsA(p,"BoolProperty")?Read<unsigned>(p+0x64):0;
            int* slot=name=="StaticMesh"?&f.staticMesh:name=="DrawType"?&f.drawType:name=="DrawScale"?&f.drawScale:name=="DrawScale3D"?&f.drawScale3D
                :name=="PrePivot"?&f.prePivot:name=="Rotation"?&f.rotation:name=="StaticMeshInstance"?&f.instance:name=="LightBrightness"?&f.brightness
                :name=="LightHue"?&f.hue:name=="LightSaturation"?&f.saturation:name=="LightCone"?&f.cone:name=="EchelonRange"?&f.range
                :name=="SpotWidth"?&f.width:name=="SpotHeight"?&f.height:name=="NearPlaneRatio"?&f.nearRatio:nullptr;
            if(name=="bHidden"){f.hiddenAt=offset;f.hiddenMask=mask;slot=&f.hiddenAt;}
            else if(name=="bCollideActors"){f.collideAt=offset;f.collideMask=mask;slot=&f.collideAt;}
            else if(name=="bBlockPlayers"){f.blockPlayersAt=offset;f.blockPlayersMask=mask;slot=&f.blockPlayersAt;}
            else if(slot)*slot=offset;
            if(slot && !shadowFieldsLogged)
            {
                char where[32];snprintf(where,sizeof(where),"@+0x%X",offset);
                text+=" "+name+where+(mask?"/"+std::to_string(mask):"");
            }
        }
        if(!shadowFieldsLogged){shadowFieldsLogged=true;Logger::log(text);}
        return f;
    }
    template<class T> T ShadowField(Address actor,int offset,T fallback){return offset>=0?Read<T>(actor+offset):fallback;}
    bool ShadowFlag(Address actor,int offset,unsigned mask){return offset>=0 && mask && (Read<unsigned>(actor+offset)&mask)!=0;}

    // A light as the map uses it: at its Location, reaching its in-game render
    // radius, turned by its Rotation, with its cone and frustum fields.
    bool ShadowLight(Address actor,const ShadowFields& f,LightShadow::Light& light)
    {
        auto render=reinterpret_cast<Address>(reinterpret_cast<void*(__thiscall*)(void*)>(kLightRenderData)(reinterpret_cast<void*>(actor)));
        if(!render)return false;
        const auto at=Position(actor);
        light.at={at[0],at[1],at[2]};
        light.radius=Read<float>(render+0x34);
        light.brightness=ShadowField<float>(actor,f.brightness,64.f)
            *LightShadow::ColourShare(ShadowField<unsigned char>(actor,f.hue,0),ShadowField<unsigned char>(actor,f.saturation,255));
        light.effect=Read<unsigned char>(actor+kLightEffect);
        light.cone=ShadowField<unsigned char>(actor,f.cone,128);
        light.range=ShadowField<float>(actor,f.range,0.f);
        light.spotWidth=ShadowField<float>(actor,f.width,0.f);
        light.spotHeight=ShadowField<float>(actor,f.height,0.f);
        light.nearRatio=ShadowField<float>(actor,f.nearRatio,0.1f);
        const Pose turn{{0,0,0},RotationOf(actor)};
        auto axis=[&](const Vector& v){auto w=TransformPoint(v,turn);return LightShadow::Point{w[0],w[1],w[2]};};
        light.forward=axis({1,0,0});light.right=axis({0,1,0});light.up=axis({0,0,1});
        light.id=actor;
        return std::isfinite(light.at.x+light.at.y+light.at.z+light.radius+light.brightness+light.range+light.spotWidth+light.spotHeight+light.nearRatio);
    }

    // Static mesh shapes, kept while the mesh's buffers stay where they were.
    struct ShadowShapeEntry { uint64_t key=0; std::shared_ptr<const LightShadow::MeshShape> shape; bool used=false; };
    std::unordered_map<Address,ShadowShapeEntry> shadowShapes;
    std::shared_ptr<const LightShadow::MeshShape> ShadowShape(Address mesh)
    {
        const Address vertexData=Read<Address>(mesh+kMeshVertices),indexData=Read<Address>(mesh+kMeshIndices);
        const int vertexCount=Read<int>(mesh+kMeshVertices+4),indexCount=Read<int>(mesh+kMeshIndices+4);
        if(!vertexData || !indexData || vertexCount<3 || vertexCount>65536 || indexCount<3 || indexCount%3 || indexCount>3000000)return nullptr;
        uint64_t key=1469598103934665603ULL;
        for(Address v:{mesh,vertexData,indexData,Address(vertexCount),Address(indexCount)})key=LightShadow::Mix(key,&v,sizeof(v));
        auto& entry=shadowShapes[mesh];
        entry.used=true;
        if(entry.shape && entry.key==key)return entry.shape;
        std::vector<float> stream(size_t(vertexCount)*6);
        std::vector<uint16_t> corners(static_cast<size_t>(indexCount));
        if(!CopyMemorySafe(stream.data(),reinterpret_cast<void*>(vertexData),stream.size()*4) || !CopyMemorySafe(corners.data(),reinterpret_cast<void*>(indexData),corners.size()*2))
        {entry=ShadowShapeEntry{};return nullptr;}
        auto shape=std::make_shared<LightShadow::MeshShape>();
        shape->key=key;
        shape->points.resize(vertexCount);shape->normals.resize(vertexCount);
        for(int i=0;i<vertexCount;++i)
        {
            const float* v=&stream[size_t(i)*6];
            if(!std::isfinite(v[0]+v[1]+v[2]+v[3]+v[4]+v[5])){entry=ShadowShapeEntry{};return nullptr;}
            shape->points[i]={v[0],v[1],v[2]};shape->normals[i]={v[3],v[4],v[5]};
        }
        shape->triangles.reserve(indexCount/3);
        for(int i=0;i+2<indexCount;i+=3)
            if(corners[i]!=corners[i+1] && corners[i+1]!=corners[i+2] && corners[i]!=corners[i+2])
                shape->triangles.push_back({corners[i],corners[i+1],corners[i+2]});
        LightShadow::Finish(*shape);
        // Once a session: the stream's points must lie in the mesh's own
        // bounds, or this is not the layout the map was written for.
        if(!shadowMeshLayoutChecked)
        {
            shadowMeshLayoutChecked=true;
            const auto box=Read<std::array<float,6>>(mesh+kMeshBounds);
            LightShadow::Box bounds;bounds.Add(LightShadow::Point{box[0],box[1],box[2]});bounds.Add(LightShadow::Point{box[3],box[4],box[5]});
            shadowMeshLayoutGood=bounds.Valid() && shape->bounds.Valid() && bounds.Contains(shape->bounds.lo,1+1e-3*std::abs(box[3]-box[0])) && bounds.Contains(shape->bounds.hi,1+1e-3*std::abs(box[3]-box[0]));
            if(!shadowMeshLayoutGood)Logger::log("LightShadowMap: static mesh "+NameOf(mesh)+" vertex stream lies outside its bounds; static meshes are left out of the map");
        }
        if(!shadowMeshLayoutGood){entry=ShadowShapeEntry{};return nullptr;}
        entry={key,shape,true};
        return entry.shape;
    }
    // A static mesh actor's local-to-world: Location, Rotation, DrawScale times
    // DrawScale3D, less PrePivot (UE2's LocalToWorld).
    bool ShadowPose(Address actor,const ShadowFields& f,LightShadow::Affine& toWorld)
    {
        const float scale=ShadowField<float>(actor,f.drawScale,1.f);
        const auto scale3=f.drawScale3D>=0?Read<std::array<float,3>>(actor+f.drawScale3D):std::array<float,3>{1,1,1};
        const auto pivot=f.prePivot>=0?Read<std::array<float,3>>(actor+f.prePivot):std::array<float,3>{0,0,0};
        const Pose pose{Position(actor),RotationOf(actor)};
        auto world=[&](double x,double y,double z){
            const Vector local{(x-pivot[0])*scale*scale3[0],(y-pivot[1])*scale*scale3[1],(z-pivot[2])*scale*scale3[2]};
            auto w=TransformPoint(local,pose);return LightShadow::Point{w[0],w[1],w[2]};};
        const auto origin=world(0,0,0);
        toWorld=LightShadow::FromAxes(origin,world(1,0,0)-origin,world(0,1,0)-origin,world(0,0,1)-origin);
        for(auto& row:toWorld.m)for(double v:row)if(!std::isfinite(v) || std::abs(v)>1e7)return false;
        return true;
    }
    // A mesh floor's baked brightness at each vertex from the static lights its
    // instance lists as reaching it. Empty when lighting was not built.
    std::vector<float> ShadowInstanceShade(Address actor,const ShadowFields& f,const LightShadow::MeshShape& shape,const LightShadow::MeshInstance& placed,
                                           std::unordered_map<Address,std::optional<LightShadow::Light>>& staticLights,bool cones)
    {
        std::vector<float> shade;
        if(f.instance<0)return shade;
        const auto instance=Read<Address>(actor+f.instance);
        if(!instance || NameOf(Read<Address>(instance+0x24))!="StaticMeshInstance")return shade;
        const Address data=Read<Address>(instance+kInstanceLights);
        const int count=Read<int>(instance+kInstanceLights+4);
        if(count<0 || count>256 || (count && !data))return shade;
        const int bytes=(static_cast<int>(shape.points.size())+7)/8;
        struct Reach { const LightShadow::Light* light; std::vector<unsigned char> bits; };
        std::vector<Reach> reaches;
        for(int i=0;i<count;++i)
        {
            const Address entry=data+i*kInstanceLightStride;
            const Address light=Read<Address>(entry);
            const Address bits=Read<Address>(entry+4);
            if(!light || !bits || Read<int>(entry+8)!=bytes)return {};
            auto known=staticLights.find(light);
            if(known==staticLights.end())
            {
                LightShadow::Light made;
                std::optional<LightShadow::Light> value;
                if(Read<unsigned char>(light+kLightType) && ShadowLight(light,f,made))value=made;
                known=staticLights.emplace(light,value).first;
            }
            if(!known->second)continue;
            Reach reach{&*known->second,std::vector<unsigned char>(bytes)};
            if(!CopyMemorySafe(reach.bits.data(),reinterpret_cast<void*>(bits),bytes))return {};
            reaches.push_back(std::move(reach));
        }
        shade.resize(shape.points.size());
        std::vector<const LightShadow::Light*> visible;
        for(size_t v=0;v<shape.points.size();++v)
        {
            visible.clear();
            for(const auto& reach:reaches)if(reach.bits[v/8]&(1u<<(v%8)))visible.push_back(reach.light);
            const auto p=placed.toWorld.Apply(LightShadow::ToPoint(shape.points[v]));
            // Normals go through the inverse transpose.
            const auto& n=shape.normals[v];const auto& m=placed.toLocal.m;
            LightShadow::Point normal{m[0][0]*n.x+m[1][0]*n.y+m[2][0]*n.z,m[0][1]*n.x+m[1][1]*n.y+m[2][1]*n.z,m[0][2]*n.x+m[1][2]*n.y+m[2][2]*n.z};
            const double length=std::sqrt(LocalLightingMatch::Dot(normal,normal));
            normal=length>1e-9?normal*(1/length):LightShadow::Point{0,0,1};
            shade[v]=static_cast<float>(LightShadow::VertexShade(visible,p,normal,cones));
        }
        return shade;
    }
    template<class Map> void ShadowForgetUnused(Map& map)
    {
        for(auto i=map.begin();i!=map.end();)
            if(!i->second.used)i=map.erase(i);
            else{i->second.used=false;++i;}
    }
}
// After a lighting build: the atlases are read again. Mesh shapes stay: a lighting
// build leaves their buffers alone, and a reimport moves them (a new key).
void LightShadowForget()
{
    shadowAtlases.clear();
}
LightShadowSceneStats LightShadowScene(LightShadow::Scene& scene,const LightShadow::Settings& settings)
{
    scene=LightShadow::Scene{};
    LightShadowSceneStats stats;
    Engine();
    const auto level=Level();
    const auto model=Read<Address>(level+0x13c);
    if(!model)throw std::runtime_error("The map has no BSP. Build its geometry first.");
    const auto nodes=Array(model+0x54,kShadowNodeStride),sections=Array(model+0xD4,kShadowSectionStride);
    const auto textures=Array(model+0xE0,kShadowTextureStride),surfaces=Array(model+0x94,kShadowSurfStride);
    const auto verts=Array(model+0x64,8),points=Array(model+0x84,12);
    std::map<int,int> atlasOf;
    auto atlas=[&](int texture)->int
    {
        if(texture<0 || texture>=static_cast<int>(textures.size()))return -1;
        auto known=atlasOf.find(texture);
        if(known!=atlasOf.end())return known->second;
        auto image=ShadowAtlas(textures[texture]);
        int index=-1;
        if(image){index=static_cast<int>(scene.atlases.size());scene.atlases.push_back(std::move(image));}
        atlasOf[texture]=index;
        return index;
    };
    for(auto node:nodes)
    {
        const int count=Read<unsigned char>(node+0x5A);
        if(count<3)continue;
        // The plane faces the empty side: up for a floor, down for a ceiling.
        const auto normal=ShadowPoint(node);
        if(normal.z<LightShadow::kWalkable)continue;
        const int surface=Read<int>(node+0x2C);
        if(surface<0 || surface>=static_cast<int>(surfaces.size()))continue;
        if(Read<unsigned>(surfaces[surface]+0x14)&kShadowSkipFlags)continue;
        const int section=Read<short>(node+0x50),start=Read<short>(node+0x52);
        bool rendered=false;
        if(section>=0 && section<static_cast<int>(sections.size()) && start>=0)
        {
            const auto render=sections[section];
            const auto vertexData=Read<Address>(render+4);
            const int vertexCount=Read<int>(render+8);
            if(vertexData && vertexCount>=0 && vertexCount<=2000000 && start+count<=vertexCount)
            {
                // BuildRenderData's PC binding; the Xbox one is never used here.
                const int map=atlas(Read<int>(render+0x28));
                LightShadow::Point p[3];double u[3],v[3];
                auto vertex=[&](int i,int k)
                {
                    const Address at=vertexData+static_cast<Address>(start+i)*kShadowVertexStride;
                    p[k]=ShadowPoint(at);u[k]=Read<float>(at+0x20);v[k]=Read<float>(at+0x24);
                };
                vertex(0,0);
                for(int j=1;j+1<count;++j)
                {
                    vertex(j,1);vertex(j+1,2);
                    LightShadow::Floor f{p[0],p[1],p[2],{u[0],u[1],u[2]},{v[0],v[1],v[2]},map};
                    if(!std::isfinite(u[0]+u[1]+u[2]+v[0]+v[1]+v[2]))f.atlas=-1;
                    scene.floors.push_back(f);
                }
                rendered=true;
            }
        }
        if(rendered)continue;
        // No render data (geometry not built since a change): the polygon's
        // own points, without baked lighting.
        const int pool=Read<int>(node+0x28);
        if(pool<0 || pool+count>static_cast<int>(verts.size()))continue;
        auto corner=[&](int i)
        {
            const unsigned point=Read<unsigned short>(verts[pool+i]);
            if(point>=points.size())throw std::runtime_error("The BSP has an invalid floor point. Rebuild the geometry.");
            return ShadowPoint(points[point]);
        };
        const auto first=corner(0);
        for(int j=1;j+1<count;++j)scene.floors.push_back(LightShadow::Floor{first,corner(j),corner(j+1)});
    }
    ShadowForgetUnused(shadowAtlases);
    stats.bspFloors=scene.floors.size();

    const auto actors=LiveActors();
    if(actors.empty())throw std::runtime_error("The map has no actors.");
    const auto fields=ResolveShadowFields(actors[0]);
    // In-game and dynamic lights: the Lighting Budget's "in game" set, at the
    // render sphere the editor's own light checks use.
    for(auto actor:actors)
    {
        if(!Read<unsigned char>(actor+kLightType))continue;
        LightingBudget::LightFlags flags;
        const auto a=Read<unsigned>(actor+kLightFlagsA),b=Read<unsigned>(actor+kLightFlagsB);
        flags.type=Read<unsigned char>(actor+kLightType);flags.effect=Read<unsigned char>(actor+kLightEffect);
        flags.staticFlag=(b&kStaticBit)!=0;flags.inGameFlag=(b&kInGameBit)!=0;
        flags.dynamicFlag=(a&kDynamicLightBit)!=0;flags.zoneLimited=(a&kZoneLimitedBit)!=0;
        flags.animA=Read<float>(actor+kLightAnimA);flags.animB=Read<float>(actor+kLightAnimB);
        if(!LightingBudget::CountsInGame(flags))continue;
        LightShadow::Light light;
        if(ShadowLight(actor,fields,light))scene.lights.push_back(light);
    }
    // The BSP's planes and solidity, for line of sight to each light.
    scene.rootOutside=Read<int>(model+0x104)!=0;
    scene.bsp.reserve(nodes.size());
    for(auto node:nodes)
    {
        const auto normal=ShadowPoint(node);
        scene.bsp.push_back({normal,Read<float>(node+0xC),Read<int>(node+0x34),Read<int>(node+0x30),
            Read<unsigned char>(node+0x5A)!=0 && !(Read<unsigned char>(node+0x5B)&0x21)});
    }
    // Static meshes: every actor drawn as one (DT_StaticMesh). Drawn ones block
    // light; ones players collide with can be stood on.
    if((settings.meshShadows || settings.meshFloors) && fields.staticMesh>=0 && fields.drawType>=0)
    {
        std::unordered_map<Address,size_t> shapeIndex;
        std::unordered_map<Address,bool> meshClasses;
        std::unordered_map<Address,std::optional<LightShadow::Light>> staticLights;
        for(auto actor:actors)
        {
            if(Read<unsigned char>(actor+fields.drawType)!=kDrawStaticMeshType)continue;
            const auto mesh=Read<Address>(actor+fields.staticMesh);
            if(!mesh)continue;
            const auto meshClass=Read<Address>(mesh+0x24);
            auto known=meshClasses.find(meshClass);
            if(known==meshClasses.end())known=meshClasses.emplace(meshClass,IsA(mesh,"StaticMesh")).first;
            if(!known->second)continue;
            LightShadow::MeshInstance instance;
            instance.id=actor;
            instance.blocks=settings.meshShadows && !ShadowFlag(actor,fields.hiddenAt,fields.hiddenMask);
            instance.floor=settings.meshFloors && ShadowFlag(actor,fields.collideAt,fields.collideMask) && ShadowFlag(actor,fields.blockPlayersAt,fields.blockPlayersMask);
            if(!instance.blocks && !instance.floor)continue;
            auto index=shapeIndex.find(mesh);
            if(index==shapeIndex.end())
            {
                auto shape=ShadowShape(mesh);
                if(!shape)continue;
                index=shapeIndex.emplace(mesh,scene.shapes.size()).first;
                scene.shapes.push_back(std::move(shape));
            }
            instance.shape=index->second;
            const auto& shape=*scene.shapes[instance.shape];
            if(shape.triangles.empty() || !ShadowPose(actor,fields,instance.toWorld) || !LightShadow::Place(instance,shape))continue;
            if(instance.floor)instance.shade=ShadowInstanceShade(actor,fields,shape,instance,staticLights,settings.spotCones);
            stats.meshTriangles+=shape.triangles.size();
            scene.instances.push_back(std::move(instance));
        }
        ShadowForgetUnused(shadowShapes);
        if(settings.meshFloors)LightShadow::AddMeshFloors(scene);
    }
    stats.meshFloors=scene.floors.size()-stats.bspFloors;
    LightShadow::Prepare(scene);
    return stats;
}
