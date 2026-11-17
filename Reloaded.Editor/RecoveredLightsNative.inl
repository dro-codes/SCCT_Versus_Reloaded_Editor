// Native reads for the light recovery (RecoveredLightsNative.h). Included in
// WorkflowEditor.cpp after LightShadowNative.inl: the model layout, the light
// flag offsets and the static mesh shapes are the ones it reads. Nothing here
// calls Engine() or Level(): the level is passed in, because a compiled map
// being recovered is not a level the workflow tools accept.
namespace
{
    using LightVec=RecoveredLights::Vec3;
    LightShadow::Point RecoveredPoint(const LightVec& v){return {v.x,v.y,v.z};}

    // Actor (and LevelInfo) fields by name; -1 when this build has none.
    struct RecoveredLightFields
    {
        int brightness=-1,radius=-1,hue=-1,saturation=-1,cone=-1,period=-1,phase=-1;
        int movableAt=-1,staticAt=-1,shadowCastAt=-1,hiddenAt=-1,deleteAt=-1;
        unsigned movableMask=0,staticMask=0,shadowCastMask=0,hiddenMask=0,deleteMask=0;
        int staticMesh=-1,drawType=-1;
    };
    int RecoveredOffset(Address object,const char* wanted,unsigned* mask=nullptr)
    {
        for(auto p:Properties(Read<Address>(object+0x24)))
            if(NameOf(p)==wanted)
            {
                if(mask)*mask=IsA(p,"BoolProperty")?Read<unsigned>(p+0x64):0;
                return Read<int>(p+0x3c);
            }
        return -1;
    }
    RecoveredLightFields ResolveRecoveredLightFields(Address actor)
    {
        RecoveredLightFields f;
        for(auto p:Properties(Read<Address>(actor+0x24)))
        {
            const auto name=NameOf(p);
            const int offset=Read<int>(p+0x3c);
            const unsigned mask=IsA(p,"BoolProperty")?Read<unsigned>(p+0x64):0;
            if(name=="LightBrightness")f.brightness=offset;
            else if(name=="LightRadius")f.radius=offset;
            else if(name=="LightHue")f.hue=offset;
            else if(name=="LightSaturation")f.saturation=offset;
            else if(name=="LightCone")f.cone=offset;
            else if(name=="LightPeriod")f.period=offset;
            else if(name=="LightPhase")f.phase=offset;
            else if(name=="StaticMesh")f.staticMesh=offset;
            else if(name=="DrawType")f.drawType=offset;
            else if(name=="bMovable"){f.movableAt=offset;f.movableMask=mask;}
            else if(name=="bStatic"){f.staticAt=offset;f.staticMask=mask;}
            else if(name=="bShadowCast"){f.shadowCastAt=offset;f.shadowCastMask=mask;}
            else if(name=="bHidden"){f.hiddenAt=offset;f.hiddenMask=mask;}
            else if(name=="bDeleteMe"){f.deleteAt=offset;f.deleteMask=mask;}
        }
        return f;
    }
    std::vector<Address> RecoveredActors(Address level,const RecoveredLightFields* fields=nullptr)
    {
        std::vector<Address> result;
        for(auto at:Array(level+0x2c))
        {
            const auto actor=Read<Address>(at);
            if(!actor)continue;
            if(fields && ShadowFlag(actor,fields->deleteAt,fields->deleteMask))continue;
            result.push_back(actor);
        }
        return result;
    }
    void RecoveredLightOf(Address actor,const RecoveredLightFields& f,RecoveredLights::Light& light)
    {
        light.name=NameOf(actor);
        const auto at=Position(actor);
        light.location={at[0],at[1],at[2]};
        const Pose turn{{0,0,0},RotationOf(actor)};
        const auto forward=TransformPoint({1,0,0},turn);
        light.direction={forward[0],forward[1],forward[2]};
        light.brightness=ShadowField<float>(actor,f.brightness,64.f);
        light.lightRadius=ShadowField<float>(actor,f.radius,64.f);
        light.hue=ShadowField<unsigned char>(actor,f.hue,0);
        light.saturation=ShadowField<unsigned char>(actor,f.saturation,255);
        light.cone=ShadowField<unsigned char>(actor,f.cone,128);
        light.period=ShadowField<unsigned char>(actor,f.period,32);
        light.phase=ShadowField<unsigned char>(actor,f.phase,0);
        light.type=Read<unsigned char>(actor+kLightType);
        light.effect=Read<unsigned char>(actor+kLightEffect);
        const auto a=Read<unsigned>(actor+kLightFlagsA),b=Read<unsigned>(actor+kLightFlagsB);
        light.ownZoneOnly=(a&kZoneLimitedBit)!=0;
        light.dynamic=(a&kDynamicLightBit)!=0 || (ShadowFlag(actor,f.movableAt,f.movableMask) && !ShadowFlag(actor,f.staticAt,f.staticMask));
        light.applyStatic=(b&kStaticBit)!=0;
        light.inGame=(b&kInGameBit)!=0;
        light.heatRadius=Read<float>(actor+kLightAnimA);
        light.heatIntensity=Read<float>(actor+kLightAnimB);
        light.zone=Read<unsigned char>(actor+kActorRegionZoneNumber);
        // Lights the game switches at run time are lit there, not in the bake.
        light.gameplay=IsA(actor,"STriggerLight");
        if(!std::isfinite(light.location.x+light.location.y+light.location.z+light.brightness+light.lightRadius+light.heatRadius+light.heatIntensity))
            throw std::runtime_error("The light "+light.name+" holds a value that is not a number.");
    }

    // The compiled map's own geometry, copied, for shadow rays.
    struct RecoveredOccluders
    {
        std::vector<LocalLightingMatch::BspNode> bsp;
        std::vector<unsigned char> sky;     // per node: its surface is a fake backdrop
        std::vector<unsigned char> zones;   // per node: back zone, front zone
        bool rootOutside=false;
        std::vector<std::shared_ptr<const LightShadow::MeshShape>> shapes;
        std::vector<LightShadow::MeshInstance> instances;
        LightShadow::Bvh bvh;

        bool MeshClear(const LightShadow::Point& a,const LightShadow::Point& b) const
        {
            if(instances.empty())return true;
            return !LightShadow::AnyHit(bvh,a,b-a,[&](uint32_t item){
                const auto& instance=instances[item];
                const auto la=instance.toLocal.Apply(a),lb=instance.toLocal.Apply(b);
                return LightShadow::SegmentHitsShape(*shapes[instance.shape],la,lb-la);
            });
        }
        bool Clear(const LightVec& from,const LightVec& to) const
        {
            const auto a=RecoveredPoint(from),b=RecoveredPoint(to);
            if(!bsp.empty() && !LightShadow::BspClear(bsp,rootOutside,a,b))return false;
            return MeshClear(a,b);
        }
        // The first solid the segment meets, nearest first: where, and the
        // node whose plane it starts at (-1 when the segment starts in solid).
        bool FirstSolid(const LightShadow::Point& a,const LightShadow::Point& b,LightShadow::Point& where,int& via) const
        {
            struct Part { int index; LightShadow::Point a,b; bool outside; int via; };
            thread_local std::vector<Part> pending;
            pending.clear();
            pending.push_back({bsp.empty()?-1:0,a,b,rootOutside,-1});
            size_t visited=0;
            while(!pending.empty())
            {
                const auto part=pending.back();
                pending.pop_back();
                if(++visited>bsp.size()*4+64){where=part.a;via=-1;return true;}
                if(part.index==-1)
                {
                    if(part.outside)continue;
                    where=part.a;via=part.via;
                    return true;
                }
                if(part.index<0 || size_t(part.index)>=bsp.size()){where=part.a;via=-1;return true;}
                const auto& node=bsp[part.index];
                const double da=LocalLightingMatch::Dot(node.normal,part.a)-node.distance,db=LocalLightingMatch::Dot(node.normal,part.b)-node.distance;
                const bool fa=da>=0,fb=db>=0;
                auto next=[&](bool front,const LightShadow::Point& start,const LightShadow::Point& end,int through){
                    pending.push_back({front?node.front:node.back,start,end,front?(part.outside||node.csg):(part.outside&&!node.csg),through});
                };
                if(fa==fb)next(fa,part.a,part.b,part.via);
                else
                {
                    const auto middle=part.a+(part.b-part.a)*(da/(da-db));
                    // The far half is pushed first, so the near half is walked first.
                    next(fb,middle,part.b,part.index);
                    next(fa,part.a,middle,part.via);
                }
            }
            return false;
        }
        // Sunlight reaches a point when the first solid on the way to the sun
        // is sky (a fake backdrop surface) and no static mesh stands before it.
        bool SunClear(const LightVec& from,const LightVec& towardsSun) const
        {
            const auto a=RecoveredPoint(from);
            const LightShadow::Point b{from.x+towardsSun.x*262144.0,from.y+towardsSun.y*262144.0,from.z+towardsSun.z*262144.0};
            LightShadow::Point where=b;int via=-1;
            if(FirstSolid(a,b,where,via) && !(via>=0 && size_t(via)<sky.size() && sky[via]))return false;
            return MeshClear(a,where);
        }
        int ZoneAt(const LightVec& point) const
        {
            const auto p=RecoveredPoint(point);
            int index=bsp.empty()?-1:0,zone=0;
            for(size_t visited=0;index>=0 && size_t(index)<bsp.size() && visited<=bsp.size();++visited)
            {
                const auto& node=bsp[index];
                const bool front=LocalLightingMatch::Dot(node.normal,p)>=node.distance;
                zone=zones[size_t(index)*2+(front?1:0)];
                index=front?node.front:node.back;
            }
            return zone;
        }
    };
}

bool RecoveredLightsReadLights(void* levelPointer,std::vector<RecoveredLightsNative::LevelLight>& lights,std::vector<std::string>& actorNames,std::string& error,
    const std::vector<void*>& outsideLevel)
{
    lights.clear();actorNames.clear();
    try
    {
        const auto level=reinterpret_cast<Address>(levelPointer);
        if(!level)throw std::runtime_error("There is no level to read lights from.");
        auto actors=RecoveredActors(level);
        if(actors.empty())return true;
        const auto fields=ResolveRecoveredLightFields(actors[0]);
        actors=RecoveredActors(level,&fields);
        const size_t inLevel=actors.size();
        for(void* extra:outsideLevel)if(extra)actors.push_back(reinterpret_cast<Address>(extra));
        std::map<Address,bool> lightClasses;
        for(size_t index=0;index<actors.size();++index)
        {
            const auto actor=actors[index];
            actorNames.push_back(NameOf(actor));
            if(!Read<unsigned char>(actor+kLightType))continue;
            RecoveredLightsNative::LevelLight entry;
            entry.outsideLevel=index>=inLevel;
            RecoveredLightOf(actor,fields,entry.light);
            const auto type=Read<Address>(actor+0x24);
            auto known=lightClasses.find(type);
            if(known==lightClasses.end())known=lightClasses.emplace(type,IsA(actor,"Light")).first;
            entry.lightClass=known->second;
            lights.push_back(std::move(entry));
        }
        return true;
    }
    catch(const std::exception& exception){error=exception.what();lights.clear();return false;}
}

std::shared_ptr<RecoveredLights::Scene> RecoveredLightsCapture(void* levelPointer,double thin,std::string& error)
{
    try
    {
        const auto level=reinterpret_cast<Address>(levelPointer);
        const auto model=level?Read<Address>(level+0x13c):0;
        if(!model)throw std::runtime_error("The compiled map has no BSP.");
        const auto nodes=Array(model+0x54,kShadowNodeStride),sections=Array(model+0xD4,kShadowSectionStride);
        const auto textures=Array(model+0xE0,kShadowTextureStride),surfaces=Array(model+0x94,kShadowSurfStride);
        auto scene=std::make_shared<RecoveredLights::Scene>();
        auto occluders=std::make_shared<RecoveredOccluders>();

        // Lightmap texels as samples: every drawn polygon with its lightmap UVs.
        std::vector<RecoveredLights::AtlasImage> images(textures.size());
        for(size_t i=0;i<textures.size();++i)
        {
            const Address compressed=textures[i]+0x14;
            const int w=Read<int>(compressed+0x38),h=Read<int>(compressed+0x3C);
            // Build Lighting writes TEXF_RGBA8 (5); another format is left out.
            if(Read<unsigned char>(compressed+0x34)!=5 || w<=0 || h<=0 || w>2048 || h>2048)continue;
            void* pixels=reinterpret_cast<void*(__thiscall*)(void*,int)>(kShadowLoadMip)(reinterpret_cast<void*>(compressed),0);
            if(!pixels || Read<Address>(compressed+0x10)!=reinterpret_cast<Address>(pixels) || Read<int>(compressed+0x14)!=w*h*4)continue;
            images[i]={w,h,static_cast<const std::uint32_t*>(pixels)};
        }
        RecoveredLights::SampleBuilder builder(thin);
        std::vector<RecoveredLights::LitVertex> polygon;
        for(auto node:nodes)
        {
            const int count=Read<unsigned char>(node+0x5A);
            const int section=Read<short>(node+0x50),start=Read<short>(node+0x52);
            if(count<3 || section<0 || start<0 || section>=static_cast<int>(sections.size()))continue;
            const auto render=sections[section];
            const int texture=Read<int>(render+0x28);
            if(texture<0 || texture>=static_cast<int>(images.size()) || !images[texture].pixels)continue;
            const auto vertexData=Read<Address>(render+4);
            const int vertexCount=Read<int>(render+8);
            const int surface=Read<int>(node+0x2C);
            if(!vertexData || vertexCount<=0 || vertexCount>2000000 || start+count>vertexCount
                || surface<0 || surface>=static_cast<int>(surfaces.size()))continue;
            auto normal=ShadowPoint(node);
            const double length=std::sqrt(LocalLightingMatch::Dot(normal,normal));
            if(!(length>0.5 && length<2))continue;
            normal=normal*(1/length);
            polygon.clear();
            bool finite=true;
            for(int i=0;i<count;++i)
            {
                const Address at=vertexData+static_cast<Address>(start+i)*kShadowVertexStride;
                const auto p=ShadowPoint(at);
                const double u=Read<float>(at+0x20),v=Read<float>(at+0x24);
                if(!std::isfinite(u+v)){finite=false;break;}
                polygon.push_back({{p.x,p.y,p.z},u,v});
            }
            if(!finite)continue;
            const unsigned flags=Read<unsigned>(surfaces[surface]+0x14);
            // The node's plane faces the empty side; its front zone is the one it is drawn in.
            builder.AddPolygon(polygon,{normal.x,normal.y,normal.z},(flags&0x100u)!=0,Read<unsigned char>(node+0x59),images[texture],texture);
        }
        scene->samples=std::move(builder.samples);

        // The BSP, for line of sight, sky and zones.
        occluders->rootOutside=Read<int>(model+0x104)!=0;
        occluders->bsp.reserve(nodes.size());
        for(auto node:nodes)
        {
            occluders->bsp.push_back({ShadowPoint(node),Read<float>(node+0xC),Read<int>(node+0x34),Read<int>(node+0x30),
                Read<unsigned char>(node+0x5A)!=0 && !(Read<unsigned char>(node+0x5B)&0x21)});
            const int surface=Read<int>(node+0x2C);
            occluders->sky.push_back(surface>=0 && surface<static_cast<int>(surfaces.size()) && (Read<unsigned>(surfaces[surface]+0x14)&0x80u) ? 1 : 0);
            occluders->zones.push_back(Read<unsigned char>(node+0x58));
            occluders->zones.push_back(Read<unsigned char>(node+0x59));
        }

        // Static meshes that cast shadows into the lightmaps.
        auto actors=RecoveredActors(level);
        if(actors.empty())throw std::runtime_error("The compiled map has no actors.");
        const auto fields=ResolveRecoveredLightFields(actors[0]);
        actors=RecoveredActors(level,&fields);
        const auto shadowFields=ResolveShadowFields(actors[0]);
        if(fields.staticMesh>=0 && fields.drawType>=0)
        {
            std::unordered_map<Address,size_t> shapeIndex;
            std::unordered_map<Address,bool> meshClasses;
            std::vector<LightShadow::Box> boxes;
            for(auto actor:actors)
            {
                if(Read<unsigned char>(actor+fields.drawType)!=kDrawStaticMeshType)continue;
                if(!ShadowFlag(actor,fields.shadowCastAt,fields.shadowCastMask) || ShadowFlag(actor,fields.hiddenAt,fields.hiddenMask))continue;
                const auto mesh=Read<Address>(actor+fields.staticMesh);
                if(!mesh)continue;
                const auto meshClass=Read<Address>(mesh+0x24);
                auto known=meshClasses.find(meshClass);
                if(known==meshClasses.end())known=meshClasses.emplace(meshClass,IsA(mesh,"StaticMesh")).first;
                if(!known->second)continue;
                auto index=shapeIndex.find(mesh);
                if(index==shapeIndex.end())
                {
                    auto shape=ShadowShape(mesh);
                    if(!shape)continue;
                    index=shapeIndex.emplace(mesh,occluders->shapes.size()).first;
                    occluders->shapes.push_back(std::move(shape));
                }
                LightShadow::MeshInstance instance;
                instance.id=actor;
                instance.shape=index->second;
                const auto& shape=*occluders->shapes[instance.shape];
                if(shape.triangles.empty() || !ShadowPose(actor,shadowFields,instance.toWorld) || !LightShadow::Place(instance,shape) || !instance.box.Valid())continue;
                scene->meshTriangles+=shape.triangles.size();
                boxes.push_back(instance.box);
                occluders->instances.push_back(std::move(instance));
            }
            occluders->bvh=LightShadow::BuildBvh(boxes,2);
        }

        // Zone ambient and the level's brightness.
        const Address levelInfo=actors[0];
        const int ambientBrightness=RecoveredOffset(levelInfo,"AmbientBrightness"),ambientHue=RecoveredOffset(levelInfo,"AmbientHue");
        const int ambientSaturation=RecoveredOffset(levelInfo,"AmbientSaturation"),brightness=RecoveredOffset(levelInfo,"Brightness");
        scene->levelBrightness=IsA(levelInfo,"LevelInfo") && brightness>=0 ? Read<float>(levelInfo+brightness) : 1.0;
        if(!std::isfinite(scene->levelBrightness) || scene->levelBrightness<=0)scene->levelBrightness=1;
        auto ambientOf=[&](Address zone)
        {
            if(!zone || !IsA(zone,"ZoneInfo") || ambientBrightness<0)return LightVec{};
            return RecoveredLights::LightmapAmbientBytes(ambientHue>=0?Read<unsigned char>(zone+ambientHue):0,
                ambientSaturation>=0?Read<unsigned char>(zone+ambientSaturation):255,Read<unsigned char>(zone+ambientBrightness));
        };
        auto ambient=std::make_shared<std::array<LightVec,64>>();
        const LightVec levelAmbient=ambientOf(levelInfo);
        for(int zone=0;zone<64;++zone)
        {
            const auto zoneActor=Read<Address>(model+0x118+static_cast<Address>(zone)*0x10);
            (*ambient)[zone]=zone>0 && zoneActor && IsA(zoneActor,"ZoneInfo") && !IsA(zoneActor,"LevelInfo") ? ambientOf(zoneActor) : levelAmbient;
        }
        scene->ambient=[ambient](int zone){return (*ambient)[zone>=0 && zone<64?zone:0];};
        scene->zoneAt=[occluders](const LightVec& point){return occluders->ZoneAt(point);};
        scene->occluders.clear=[occluders](const LightVec& from,const LightVec& to){return occluders->Clear(from,to);};
        scene->occluders.sunClear=[occluders](const LightVec& from,const LightVec& towardsSun){return occluders->SunClear(from,towardsSun);};
        Logger::log("MapRecovery: light recovery read "+std::to_string(scene->samples.Count())+" lightmap samples, "
            +std::to_string(occluders->instances.size())+" shadow-casting static meshes ("+std::to_string(scene->meshTriangles)+" triangles)");
        return scene;
    }
    catch(const std::exception& exception){error=exception.what();return {};}
}
