// Injected only by run_native_map_recovery_test.ps1 into its isolated editor.
// Run engine commands on the editor's UI thread, never on the injection thread.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <share.h>
#include <sstream>
#include <iomanip>
#include <vector>
#include <fstream>
#include <map>
#include <set>

namespace {
HHOOK hook;
HWND frameWindow;
std::filesystem::path directory;
FILE* report;
constexpr UINT kRun = WM_APP + 0x319;
constexpr uintptr_t kEditor = 0x1165DFA0;
constexpr uintptr_t kWarn = 0x115BEFB0;
using ExecFn = int(__thiscall*)(void*, const char*, void*);
ExecFn originalExec;
using CsgFn = int(__thiscall*)(void*,void*,void*,unsigned int,int,int,int);
CsgFn originalCsg;
using BspBuildFn = void(__thiscall*)(void*,void*,int,int,int,int,int);
BspBuildFn originalBspBuild;
using BspRefreshFn = void(__thiscall*)(void*,void*,int);
BspRefreshFn originalBspRefresh;
unsigned buildTraceSequence;
int maximumPointSlots;
int compactionCount;
bool startupFailed;
bool compactionEnabled;
bool traceSoftBodies;
bool stepCookedSoftBodies;
bool traceLeafLights;
bool traceLighting;
bool relightCookedMeshes;
bool allLeafLightCandidates;
bool skipMeshShadowOcclusion;
bool inspectCookedOnly;
bool inspectedCooked;
void Snapshot(const char* stage);
struct ProbePoint { float x{}, y{}, z{}; };
bool EmptyAt(unsigned char* model, ProbePoint point);
ProbePoint tracedPoint;
bool tracePointEnabled;
std::string tracedActor;

void Record(const char* stage, const char* detail = "") {
    fprintf(report, "%s %s\n", stage, detail);
    fflush(report);
}

const char* NameText(int index) {
    auto names = *reinterpret_cast<unsigned char***>(0x1169CFBC);
    int count = *reinterpret_cast<int*>(0x1169CFC0);
    return names && index >= 0 && index < count && names[index]
        ? reinterpret_cast<const char*>(names[index]+12) : "<invalid>";
}

const char* ObjectName(void* object) {
    return object ? NameText(*reinterpret_cast<int*>(static_cast<unsigned char*>(object)+0x20)) : "<null>";
}

unsigned char* FindProperty(unsigned char* actor, const char* name) {
    auto type = *reinterpret_cast<unsigned char**>(actor+0x24);
    for (auto property = *reinterpret_cast<unsigned char**>(type+0x58); property;
         property = *reinterpret_cast<unsigned char**>(property+0x40))
        if (!strcmp(ObjectName(property),name)) return property;
    return nullptr;
}

void DumpNativeBsp(const char* filename) {
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    auto level = editor ? *reinterpret_cast<unsigned char**>(editor+0x130) : nullptr;
    auto model = level ? *reinterpret_cast<unsigned char**>(level+0x13C) : nullptr;
    if (!model) return;
    auto nodes = *reinterpret_cast<unsigned char**>(model+0x54);
    int count = *reinterpret_cast<int*>(model+0x58);
    std::ofstream output(directory/filename);
    output << std::setprecision(9) << "{\"rootOutside\":"
           << *reinterpret_cast<int*>(model+0x104) << ",\"nodes\":[";
    for (int i=0; i<count; ++i) {
        auto node=nodes+i*0x5C;
        auto plane=reinterpret_cast<float*>(node);
        if(i) output << ',';
        output << "{\"plane\":[" << plane[0] << ',' << plane[1] << ',' << plane[2] << ',' << plane[3]
               << "],\"back\":" << *reinterpret_cast<int*>(node+0x30)
               << ",\"front\":" << *reinterpret_cast<int*>(node+0x34)
               << ",\"coplanar\":" << *reinterpret_cast<int*>(node+0x38)
               << ",\"surface\":" << *reinterpret_cast<int*>(node+0x2C)
               << ",\"vertexCount\":" << static_cast<unsigned>(node[0x5A])
               << ",\"flags\":" << static_cast<unsigned>(node[0x5B]) << '}';
    }
    output << "]}";
}

void DumpLeafLights(const char* stage) {
    if (!traceLeafLights) return;
    auto editor=*reinterpret_cast<unsigned char**>(kEditor);
    auto level=editor ? *reinterpret_cast<unsigned char**>(editor+0x130) : nullptr;
    auto model=level ? *reinterpret_cast<unsigned char**>(level+0x13C) : nullptr;
    if (!model) return;
    auto leaves=*reinterpret_cast<unsigned short**>(model+0xBC);
    int leafCount=*reinterpret_cast<int*>(model+0xC0);
    auto lights=*reinterpret_cast<uintptr_t**>(model+0xC8);
    int lightCount=*reinterpret_cast<int*>(model+0xCC);
    fprintf(report,"leaf_lights stage=%s model=%p leaves=%d lights=%d\n",stage,model,leafCount,lightCount);
    fflush(report);
    if (leafCount<0 || leafCount>1000000 || lightCount<0 || lightCount>10000000) return;
    static unsigned sequence=0;
    std::ofstream output(directory/("LeafLights_"+std::to_string(++sequence)+".json"));
    output << "{\"stage\":\"" << stage << "\",\"leaves\":[";
    for(int i=0;i<leafCount;++i) {
        if(i) output << ',';
        output << '[' << leaves[i*2] << ',' << leaves[i*2+1] << ']';
    }
    output << "],\"lights\":[";
    for(int i=0;i<lightCount;++i) {if(i) output << ','; output << lights[i];}
    output << "]}";
}

bool IsTracedModel(void* editor,void* model) {
    auto level=*reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(editor)+0x130);
    return tracePointEnabled && level && *reinterpret_cast<void**>(level+0x13C)==model;
}

void __fastcall TraceBspBuild(void* editor,void*,void* rawModel,int quality,int balance,
                              int portalBias,int rebuildSimplePolys,int rootNode) {
    auto model=static_cast<unsigned char*>(rawModel);
    bool tracing=IsTracedModel(editor,model);
    unsigned sequence=tracing ? ++buildTraceSequence : 0;
    if(tracing) {
        fprintf(report,"trace_build_begin sequence=%u empty=%d nodes=%d args=%d,%d,%d,%d,%d\n",
            sequence,EmptyAt(model,tracedPoint),*reinterpret_cast<int*>(model+0x58),
            quality,balance,portalBias,rebuildSimplePolys,rootNode);
        fflush(report);
        DumpNativeBsp(("TraceBuildBefore_"+std::to_string(sequence)+".json").c_str());
    }
    originalBspBuild(editor,model,quality,balance,portalBias,rebuildSimplePolys,rootNode);
    if(tracing) {
        fprintf(report,"trace_build_end sequence=%u empty=%d nodes=%d\n",sequence,
            EmptyAt(model,tracedPoint),*reinterpret_cast<int*>(model+0x58));
        fflush(report);
        DumpNativeBsp(("TraceBuildAfter_"+std::to_string(sequence)+".json").c_str());
    }
}

void __fastcall TraceBspRefresh(void* editor,void*,void* rawModel,int noRemapSurfs) {
    auto model=static_cast<unsigned char*>(rawModel);
    bool tracing=IsTracedModel(editor,model);
    bool before=tracing && EmptyAt(model,tracedPoint);
    originalBspRefresh(editor,model,noRemapSurfs);
    if(tracing && before!=EmptyAt(model,tracedPoint)) {
        fprintf(report,"trace_refresh_changed before_empty=%d after_empty=%d nodes=%d\n",
            before,EmptyAt(model,tracedPoint),*reinterpret_cast<int*>(model+0x58));
        fflush(report);
        DumpNativeBsp("TraceRefreshChanged.json");
    }
}

void ProbeNativeNormalThreshold() {
    unsigned short controlWord=0;
    __asm fnstcw controlWord
    fprintf(report,"native_fpu_control word=%04X\n",controlWord);
    for(float height : {0.009999f,0.01f,0.010001f}) {
        alignas(4) unsigned char polygon[0x14C]={};
        auto vertices=reinterpret_cast<float*>(polygon+0x18);
        vertices[3]=1.0f; vertices[7]=height;
        *reinterpret_cast<unsigned short*>(polygon+0x148)=3;
        using CalcNormal=int(__thiscall*)(void*,int);
        int result=reinterpret_cast<CalcNormal>(0x110C0970)(polygon,1);
        fprintf(report,"native_calc_normal height=%.9g result=%d normal_z=%.9g\n",
            height,result,*reinterpret_cast<float*>(polygon+0x14));
    }
    fflush(report);
}

int ReferencedPointCount(unsigned char* model) {
    int pointCount = *reinterpret_cast<int*>(model+0x88);
    int nodeCount = *reinterpret_cast<int*>(model+0x58);
    int poolCount = *reinterpret_cast<int*>(model+0x68);
    int surfaceCount = *reinterpret_cast<int*>(model+0x98);
    if (pointCount<0 || pointCount>=65535 || nodeCount<0 || poolCount<0 || surfaceCount<0) return -1;
    std::vector<unsigned char> used(pointCount);
    auto nodes = *reinterpret_cast<unsigned char**>(model+0x54);
    auto pool = *reinterpret_cast<unsigned char**>(model+0x64);
    auto surfaces = *reinterpret_cast<unsigned char**>(model+0x94);
    int result=0;
    for (int i=0; i<nodeCount; ++i) {
        auto node = nodes+i*0x5C;
        int start = *reinterpret_cast<int*>(node+0x28), count=node[0x5A];
        if (count && (start<0 || start>poolCount-count)) return -1;
        for (int j=0; j<count; ++j) {
            unsigned index = *reinterpret_cast<unsigned short*>(pool+(start+j)*8);
            if (index>=static_cast<unsigned>(pointCount)) return -1;
            if (!used[index]) {used[index]=1;++result;}
        }
    }
    for (int i=0; i<surfaceCount; ++i) {
        unsigned index = *reinterpret_cast<unsigned short*>(surfaces+i*0x2C+0x18);
        if (index>=static_cast<unsigned>(pointCount)) return -1;
        if (!used[index]) {used[index]=1;++result;}
    }
    return result;
}

int __fastcall CompactBeforeCsg(void* editor, void*, void* actor, void* rawModel,
                               unsigned int flags,int operation,int rebuildBounds,int mergePolys) {
    auto model = static_cast<unsigned char*>(rawModel);
    auto level=*reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(editor)+0x130);
    bool traceThis=tracePointEnabled && level && *reinterpret_cast<void**>(level+0x13C)==model;
    bool emptyBefore=traceThis && EmptyAt(model,tracedPoint);
    int before = *reinterpret_cast<int*>(model+0x88);
    if (before>maximumPointSlots) maximumPointSlots=before;
    if (compactionEnabled && before>=32768) {
        int referenced = ReferencedPointCount(model);
        if (referenced<0) {
            Record("FAIL","Test compaction rejected out-of-range point references before a CSG operation.");
            TerminateProcess(GetCurrentProcess(),41);
            return 0;
        }
        // Test-only wrapper runs before the existing CSG entry/diagnostics
        // hooks, after the previous CSG call has returned completely.
        using Refresh = void(__thiscall*)(void*,void*,int);
        reinterpret_cast<Refresh>(0x11086810)(editor,model,1);
        int after = *reinterpret_cast<int*>(model+0x88);
        ++compactionCount;
        fprintf(report,"point_compaction count=%d brush=%s before=%d referenced=%d after=%d max=%d nodes=%d\n",
            compactionCount,ObjectName(actor),before,referenced,after,maximumPointSlots,
            *reinterpret_cast<int*>(model+0x58));
        fflush(report);
    }
    int result=originalCsg(editor,actor,model,flags,operation,rebuildBounds,mergePolys);
    if (traceThis) {
        bool emptyAfter=EmptyAt(model,tracedPoint);
        if (emptyBefore!=emptyAfter || tracedActor==ObjectName(actor)) {
            fprintf(report,"trace_csg actor=%s operation=%d before_empty=%d after_empty=%d nodes=%d\n",
                ObjectName(actor),operation,emptyBefore,emptyAfter,*reinterpret_cast<int*>(model+0x58));
            fflush(report);
            DumpNativeBsp(("TraceAfter_"+std::string(ObjectName(actor))+".json").c_str());
        }
    }
    return result;
}

void ObserveCsgCompaction() {
    auto editor = *reinterpret_cast<void**>(kEditor);
    auto slot = *reinterpret_cast<void***>(editor)+0x1DC/4;
    DWORD protection;
    if (!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)) {
        Record("FAIL","Could not install the isolated CSG compaction experiment."); return;
    }
    originalCsg = reinterpret_cast<CsgFn>(*slot);
    *slot = reinterpret_cast<void*>(&CompactBeforeCsg);
    DWORD ignored;
    VirtualProtect(slot,sizeof(void*),protection,&ignored);
    if(compactionEnabled) Record("test_compaction","Native bspRefresh enabled before CSG when point slots reach32768.");
    if(tracePointEnabled) {
        auto vtable=*reinterpret_cast<void***>(editor);
        auto buildSlot=vtable+0x1C4/4;
        auto refreshSlot=vtable+0x1C8/4;
        if(VirtualProtect(buildSlot,sizeof(void*)*2,PAGE_READWRITE,&protection)) {
            originalBspBuild=reinterpret_cast<BspBuildFn>(*buildSlot);
            originalBspRefresh=reinterpret_cast<BspRefreshFn>(*refreshSlot);
            *buildSlot=reinterpret_cast<void*>(&TraceBspBuild);
            *refreshSlot=reinterpret_cast<void*>(&TraceBspRefresh);
            VirtualProtect(buildSlot,sizeof(void*)*2,protection,&ignored);
        }
    }
}

unsigned char* FindStaticMeshActor(unsigned char* level, const char* name=nullptr) {
    auto data = *reinterpret_cast<unsigned char***>(level+0x2C);
    int count = *reinterpret_cast<int*>(level+0x30);
    for (int i=2; data && i<count; ++i)
        if (data[i] && !strcmp(ObjectName(*reinterpret_cast<void**>(data[i]+0x24)),"StaticMeshActor")
            && (!name || !strcmp(ObjectName(data[i]),name))) return data[i];
    return nullptr;
}

bool HasEditedTag(unsigned char* actor) {
    auto property = actor ? FindProperty(actor,"Tag") : nullptr;
    if (!property || strcmp(ObjectName(*reinterpret_cast<void**>(property+0x24)),"NameProperty")) return false;
    auto offset = *reinterpret_cast<unsigned int*>(property+0x3C);
    return !strcmp(NameText(*reinterpret_cast<int*>(actor+offset)),"RecoveredSourceEditProbe");
}

bool EditActorTag(unsigned char* level, std::string& name) {
    auto actor = FindStaticMeshActor(level);
    auto property = actor ? FindProperty(actor,"Tag") : nullptr;
    if (!property || strcmp(ObjectName(*reinterpret_cast<void**>(property+0x24)),"NameProperty")) return false;
    name = ObjectName(actor);
    auto offset = *reinterpret_cast<unsigned int*>(property+0x3C);
    // Same reflected ImportText virtual used by native property editing.
    using Import = const char*(__thiscall*)(void*,const char*,void*,unsigned int);
    auto importer = (*reinterpret_cast<void***>(property))[0x94/4];
    auto result = reinterpret_cast<Import>(importer)(property,"\"RecoveredSourceEditProbe\"",actor+offset,0);
    Record("actor_tag_edit",name.c_str());
    return result && HasEditedTag(actor);
}

bool ContainsActor(unsigned char* level, size_t offset, void* actor) {
    auto data = *reinterpret_cast<void***>(level+offset);
    int count = *reinterpret_cast<int*>(level+offset+4);
    for (int i=0; data && i<count; ++i) if (data[i] == actor) return true;
    return false;
}

void DescribeActorFields(unsigned char* actor) {
    auto type = *reinterpret_cast<unsigned char**>(actor+0x24);
    auto property = *reinterpret_cast<unsigned char**>(type+0x58);
    for (int depth=0; property && depth<4096; ++depth) {
        const char* name = ObjectName(property);
        if (!strcmp(name, "bDeleteMe") || !strcmp(name, "bHidden") || !strcmp(name, "Platform")
            || !strcmp(name, "m_Platform") || !strcmp(name, "Level") || !strcmp(name, "Owner")) {
            unsigned offset = *reinterpret_cast<unsigned int*>(property+0x3C);
            const char* propertyClass = ObjectName(*reinterpret_cast<void**>(property+0x24));
            if (!strcmp(propertyClass,"BoolProperty") || !strcmp(propertyClass,"ByteProperty")) {
                char value[128] = {};
                using ExportItem = void(__thiscall*)(void*, char*, const void*, const void*, unsigned int);
                auto exportItem = (*reinterpret_cast<void***>(property))[0x90/4];
                reinterpret_cast<ExportItem>(exportItem)(property,value,actor+offset,nullptr,0);
                fprintf(report,"actor_property name=%s class=%s offset=%x native_value=%s raw=%08x extra64=%08x\n",
                    name,propertyClass,offset,value,*reinterpret_cast<unsigned int*>(actor+offset),
                    *reinterpret_cast<unsigned int*>(property+0x64));
            } else {
                auto referenced = *reinterpret_cast<unsigned char**>(actor+offset);
                fprintf(report,"actor_property name=%s class=%s offset=%x pointer=%p referenced_name=%s referenced_class=%s\n",
                    name,propertyClass,offset,referenced,ObjectName(referenced),
                    referenced ? ObjectName(*reinterpret_cast<void**>(referenced+0x24)) : "<null>");
            }
            fflush(report);
        }
        property = *reinterpret_cast<unsigned char**>(property+0x40);
    }
}

void DescribeLighting(const char* stage) {
    if (!traceLighting) return;
    const auto folder = directory / (std::string("lighting_") + stage);
    std::filesystem::create_directories(folder);
    std::ofstream text(folder / "actors.txt");
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    auto level = *reinterpret_cast<unsigned char**>(editor + 0x130);
    auto actors = *reinterpret_cast<unsigned char***>(level + 0x2C);
    const int count = *reinterpret_cast<int*>(level + 0x30);
    auto objects = *reinterpret_cast<unsigned char***>(0x11697B70);
    const int objectCount = *reinterpret_cast<int*>(0x11697B74);
    auto package = *reinterpret_cast<void**>(level + 0x18);
    for (int i = 0; objects && i < objectCount; ++i) {
        auto object = objects[i];
        if (!object || *reinterpret_cast<void**>(object + 0x18) != package) continue;
        const std::string className = ObjectName(*reinterpret_cast<void**>(object + 0x24));
        if (className != "Light" && className != "STriggerLight") continue;
        text << "package_light " << ObjectName(object) << " class=" << className
             << " active=" << ContainsActor(level, 0x2C, object)
             << " deleted=" << ((*reinterpret_cast<unsigned*>(object + 0x2E8) & 0x8000u) != 0) << '\n';
    }
    auto dump = [&](const std::string& name, const void* pointer, size_t size) {
        std::vector<char> bytes(size);
        SIZE_T read = 0;
        if (pointer && ReadProcessMemory(GetCurrentProcess(), pointer, bytes.data(), size, &read) && read == size)
            std::ofstream(folder / name, std::ios::binary).write(bytes.data(), size);
    };
    for (int i = 0; actors && i < count; ++i) {
        auto actor = actors[i];
        if (!actor) continue;
        auto type = *reinterpret_cast<unsigned char**>(actor + 0x24);
        const std::string actorName = ObjectName(actor), className = ObjectName(type);
        const bool light = className.find("Light") != std::string::npos
            || className.find("ZoneInfo") != std::string::npos || className == "LevelInfo";
        auto mesh = *reinterpret_cast<void**>(actor + 0x100);
        if (!light && !mesh) continue;
        text << "actor " << actorName << " class=" << className << '\n';
        for (auto property = *reinterpret_cast<unsigned char**>(type + 0x58); property;
             property = *reinterpret_cast<unsigned char**>(property + 0x40)) {
            const std::string name = ObjectName(property);
            const std::string kind = ObjectName(*reinterpret_cast<void**>(property + 0x24));
            if (kind != "BoolProperty" && kind != "ByteProperty" && kind != "FloatProperty"
                && kind != "IntProperty" && kind != "NameProperty") continue;
            if (!light && name.find("Light") == std::string::npos && name.find("Ambient") == std::string::npos
                && name != "bStatic" && name != "bHidden") continue;
            char value[4096]{};
            const unsigned offset = *reinterpret_cast<unsigned*>(property + 0x3C);
            using ExportItem = void(__thiscall*)(void*, char*, const void*, const void*, unsigned int);
            auto exporter = (*reinterpret_cast<void***>(property))[0x90 / 4];
            reinterpret_cast<ExportItem>(exporter)(property, value, actor + offset, nullptr, 0);
            text << "  " << name << '=' << value << '\n';
        }
        if (mesh) {
            auto model = *reinterpret_cast<unsigned char**>(level + 0x13C);
            auto leaves = *reinterpret_cast<unsigned short**>(model + 0xBC);
            const int leafCount = *reinterpret_cast<int*>(model + 0xC0);
            auto lights = *reinterpret_cast<unsigned char***>(model + 0xC8);
            const int lightCount = *reinterpret_cast<int*>(model + 0xCC);
            auto actorLeaves = *reinterpret_cast<int**>(actor + 0x1C4);
            const int actorLeafCount = *reinterpret_cast<int*>(actor + 0x1C8);
            std::set<std::string> affectingLights;
            if (actorLeafCount >= 0 && actorLeafCount < 100000)
                for (int leaf = 0; actorLeaves && leaf < actorLeafCount; ++leaf) {
                    const int index = actorLeaves[leaf];
                    if (index < 0 || index >= leafCount) continue;
                    const unsigned start = leaves[index * 2 + 1];
                    if (start == 0xFFFF) continue;
                    for (unsigned j = start; j < static_cast<unsigned>(lightCount) && lights[j]; ++j)
                        affectingLights.insert(ObjectName(lights[j]));
                }
            text << "  leaf_count=" << actorLeafCount << " lights=";
            for (const auto& name : affectingLights) text << name << ',';
            text << '\n';
            auto instance = *reinterpret_cast<unsigned char**>(actor + 0x22C);
            text << "  mesh=" << ObjectName(mesh) << " instance=" << ObjectName(instance) << '\n';
            auto meshBytes=static_cast<unsigned char*>(mesh);
            const int meshColors=*reinterpret_cast<int*>(meshBytes+0x80);
            text << "  asset_vertex_colors=" << meshColors << " multiply_vertex_colors=" << *reinterpret_cast<int*>(meshBytes+0x180) << '\n';
            if(meshColors>=0 && meshColors<=1000000)
                dump(std::string("asset_")+ObjectName(mesh)+"_colors.bin",*reinterpret_cast<void**>(meshBytes+0x7C),static_cast<size_t>(meshColors)*4);
            dump(actorName + "_instance.bin", instance, 0x50);
            if (instance) {
                text << "  scale_glow=" << *reinterpret_cast<float*>(actor+0x258) << '\n';
                auto shadows=*reinterpret_cast<unsigned char**>(instance+0x28);
                const int shadowCount=*reinterpret_cast<int*>(instance+0x2C);
                if(shadowCount>=0 && shadowCount<10000)for(int shadow=0;shadows && shadow<shadowCount;++shadow)
                {
                    auto entry=shadows+shadow*0x14;auto lightActor=*reinterpret_cast<unsigned char**>(entry);
                    auto mask=*reinterpret_cast<unsigned char**>(entry+4);int bytes=*reinterpret_cast<int*>(entry+8);
                    unsigned visible=0;
                    if(bytes>=0 && bytes<1000000)for(int byte=0;mask && byte<bytes;++byte)for(int bit=0;bit<8;++bit)visible+=(mask[byte]>>bit)&1;
                    text << "  shadow_light=" << ObjectName(lightActor) << " bytes=" << bytes << " visible=" << visible << " baked=" << *reinterpret_cast<int*>(entry+0x10);
                    if(lightActor)
                    {
                        using LightData=unsigned char*(__thiscall*)(void*);
                        auto data=reinterpret_cast<LightData>(0x110B1510)(lightActor);
                        if(data)text << " render_flags=" << static_cast<unsigned>(data[0x38])
                            << " rgb=" << reinterpret_cast<float*>(data+0xC)[0] << ',' << reinterpret_cast<float*>(data+0xC)[1] << ',' << reinterpret_cast<float*>(data+0xC)[2];
                    }
                    text << '\n';
                }
                const int colorCount = *reinterpret_cast<int*>(instance + 0x3C);
                if (colorCount >= 0 && colorCount <= 1000000)
                    dump(actorName + "_colors.bin", *reinterpret_cast<void**>(instance + 0x38),
                         static_cast<size_t>(colorCount) * 4);
                dump("instance_class.bin", *reinterpret_cast<void**>(instance + 0x24), 0x100);
                dump("instance_vtable.bin", *reinterpret_cast<void**>(instance), 0x100);
            }
        }
    }
}

void DescribeBspLighting(const char* stage) {
    if (!traceLighting) return;
    auto folder=directory/(std::string("lighting_")+stage);
    std::filesystem::create_directories(folder);
    auto level=*reinterpret_cast<unsigned char**>(*reinterpret_cast<unsigned char**>(kEditor)+0x130);
    auto model=*reinterpret_cast<unsigned char**>(level+0x13c);
    auto textures=*reinterpret_cast<unsigned char**>(model+0xe0);
    const int count=*reinterpret_cast<int*>(model+0xe4);
    for(int i=0;i<count && i<4096;++i) {
        auto compressed=textures+i*0x70+0x14;
        using Load=void*(__thiscall*)(void*,int);
        auto pixels=reinterpret_cast<Load>(0x111b2400)(compressed,0);
        const int size=*reinterpret_cast<int*>(compressed+0x14);
        if(pixels && size>0 && size<=2048*2048*4)
            std::ofstream(folder/("bsp_atlas_"+std::to_string(i)+".bin"),std::ios::binary).write(static_cast<char*>(pixels),size);
    }
}

void DescribeSoftBodies(const char* stage) {
    if (!traceSoftBodies) return;
    auto editor=*reinterpret_cast<unsigned char**>(kEditor);
    auto level=editor ? *reinterpret_cast<unsigned char**>(editor+0x130) : nullptr;
    auto actors=level ? *reinterpret_cast<unsigned char***>(level+0x2C) : nullptr;
    int count=level ? *reinterpret_cast<int*>(level+0x30) : 0;
    for(int i=0;actors && i<count;++i) {
        auto actor=actors[i];
        auto binding=actor ? FindProperty(actor,"SoftBody") : nullptr;
        if(!binding) continue;
        auto body=*reinterpret_cast<unsigned char**>(actor+*reinterpret_cast<unsigned*>(binding+0x3C));
        fprintf(report,"soft_body stage=%s actor=%s class=%s body=%s body_class=%s vtable=%p\n",
            stage,ObjectName(actor),ObjectName(*reinterpret_cast<void**>(actor+0x24)),ObjectName(body),
            body ? ObjectName(*reinterpret_cast<void**>(body+0x24)) : "<null>",
            body ? *reinterpret_cast<void**>(body) : nullptr);
        if(body && (!strcmp(ObjectName(*reinterpret_cast<void**>(body+0x24)),"ESBStripDoor")
            || !strcmp(ObjectName(*reinterpret_cast<void**>(body+0x24)),"ESBPatch"))) {
            if (stepCookedSoftBodies && !strcmp(stage,"cooked")) {
                using Update = void(__thiscall*)(void*, float);
                reinterpret_cast<Update>(0x110D4610)(body, 1.0f / 60.0f);
                fprintf(report,"stepped_cooked_soft_body actor=%s\n",ObjectName(actor));
            }
            const auto stem=std::string(stage)+"_"+ObjectName(actor);
            std::ofstream raw(directory/(stem+"_body.bin"),std::ios::binary);
            raw.write(reinterpret_cast<char*>(body),0x1A4);
            std::ofstream actorRaw(directory/(stem+"_actor.bin"),std::ios::binary);
            actorRaw.write(reinterpret_cast<char*>(actor),0x3D0);
            for(const auto entry : {std::pair<unsigned,unsigned>{0x70,0x48},{0x7C,0xC}}) {
                const auto data=*reinterpret_cast<char**>(body+entry.first);
                const auto size=*reinterpret_cast<int*>(body+entry.first+4);
                if(size>=0 && size<=100000 && (!size || data)) {
                    std::ofstream array(directory/(stem+"_array_"+std::to_string(entry.first)+".bin"),std::ios::binary);
                    if(size) array.write(data,static_cast<std::streamsize>(size)*entry.second);
                }
            }
        }
        for(auto object : {actor,body}) {
            if(!object) continue;
            auto type=*reinterpret_cast<unsigned char**>(object+0x24);
            for(auto property=*reinterpret_cast<unsigned char**>(type+0x58); property;
                property=*reinterpret_cast<unsigned char**>(property+0x40)) {
                const auto offset=*reinterpret_cast<unsigned*>(property+0x3C);
                const auto propertyClass=ObjectName(*reinterpret_cast<void**>(property+0x24));
                fprintf(report,"soft_property object=%s name=%s class=%s offset=%x",ObjectName(object),ObjectName(property),propertyClass,offset);
                if(!strcmp(propertyClass,"BoolProperty") || !strcmp(propertyClass,"IntProperty")
                    || !strcmp(propertyClass,"FloatProperty") || !strcmp(propertyClass,"ByteProperty")) {
                    char value[512]={};
                    using ExportItem=void(__thiscall*)(void*,char*,const void*,const void*,unsigned);
                    reinterpret_cast<ExportItem>((*reinterpret_cast<void***>(property))[0x90/4])(property,value,object+offset,nullptr,0);
                    fprintf(report," value=%s",value);
                }
                fprintf(report,"\n");
            }
        }
    }
    fflush(report);
}

int __fastcall ObserveExec(void* exec, void*, const char* command, void* output) {
    if (command && strstr(command,"MAP EXPORT") && strstr(command,"Imported.t3d"))
        DescribeSoftBodies("imported");
    if (command && strstr(command,"MAP EXPORT")
        && (strstr(command,"Actors.t3d") || strstr(command,"Imported.t3d"))) {
        auto objects = *reinterpret_cast<unsigned char***>(0x11697B70);
        int count = *reinterpret_cast<int*>(0x11697B74);
        for (int i=0; objects && i<count; ++i) {
            auto object = objects[i];
            if (!object || (strcmp(ObjectName(object),"SpriteEmitter300")
                && strcmp(ObjectName(object),"SpriteEmitter617"))) continue;
            auto property = FindProperty(object,"SizeScaleRepeats");
            if (!property || strcmp(ObjectName(*reinterpret_cast<void**>(property+0x24)),"FloatProperty")) continue;
            auto offset = *reinterpret_cast<unsigned int*>(property+0x3C);
            fprintf(report,"particle_scalar stage=%s object=%s offset=%x value=%.12g bits=%08x outer=%s\n",
                strstr(command,"Actors.t3d") ? "cooked" : "imported",ObjectName(object),offset,
                *reinterpret_cast<float*>(object+offset),*reinterpret_cast<unsigned int*>(object+offset),
                ObjectName(*reinterpret_cast<void**>(object+0x18)));
            fflush(report);
        }
    }
    if (command && strstr(command, "MAP EXPORT") && strstr(command, "Actors.t3d")) {
        if (inspectCookedOnly) {
            Snapshot("inspected_cooked");
            auto editor = *reinterpret_cast<unsigned char**>(kEditor);
            auto level = *reinterpret_cast<unsigned char**>(editor+0x130);
            auto model = *reinterpret_cast<unsigned char**>(level+0x13C);
            std::ofstream(directory/"CookedModel.bin",std::ios::binary).write(reinterpret_cast<char*>(model),0x518);
            struct Field {const char* name;int offset,stride;};
            for (auto field : {Field{"Nodes",0x54,0x5C},Field{"Verts",0x64,8},Field{"Points",0x84,12},
                               Field{"Surfs",0x94,0x2C},Field{"Hulls",0xB0,4},Field{"Leaves",0xBC,4}}) {
                auto data=*reinterpret_cast<char**>(model+field.offset);
                int count=*reinterpret_cast<int*>(model+field.offset+4);
                fprintf(report,"cooked_array name=%s count=%d\n",field.name,count);
                if(count>=0 && count<=1000000 && (!count || data))
                    std::ofstream(directory/(std::string("Cooked")+field.name+".bin"),std::ios::binary).write(data,static_cast<std::streamsize>(count)*field.stride);
            }
            auto actors=*reinterpret_cast<unsigned char***>(level+0x2C);
            int count=*reinterpret_cast<int*>(level+0x30);
            for(int i=0;actors && i<count;++i) if(actors[i]) {
                auto actor=actors[i];
                if(strcmp(ObjectName(*reinterpret_cast<void**>(actor+0x24)),"PlayerStart")) continue;
                auto property=FindProperty(actor,"Location");
                if(!property) continue;
                auto location=reinterpret_cast<float*>(actor+*reinterpret_cast<unsigned*>(property+0x3C));
                fprintf(report,"cooked_spawn name=%s location=%.9g,%.9g,%.9g empty=%d\n",ObjectName(actor),
                    location[0],location[1],location[2],EmptyAt(model,{location[0],location[1],location[2]}));
            }
            fflush(report);
        }
        DescribeSoftBodies("cooked");
        DescribeLighting("cooked");
        if (relightCookedMeshes) {
            auto editor = *reinterpret_cast<unsigned char**>(kEditor);
            auto level = *reinterpret_cast<unsigned char**>(editor + 0x130);
            auto actors = *reinterpret_cast<unsigned char***>(level + 0x2C);
            const int count = *reinterpret_cast<int*>(level + 0x30);
            Record("relighting_cooked_meshes", "Native mesh illumination only; BSP is unchanged and no map is saved.");
            using Illuminate = void(__thiscall*)(void*, void*, int);
            for (int i = 0; actors && i < count; ++i) {
                auto actor = actors[i];
                auto mesh = actor ? *reinterpret_cast<void**>(actor + 0x100) : nullptr;
                if (mesh && *reinterpret_cast<void**>(actor + 0x22C)) {
                    // A cooked load has no editor leaf-membership cache yet.
                    // Refresh it before Illuminate gathers candidate lights.
                    using GetRenderData = void*(__thiscall*)(void*);
                    using UpdateRenderData = void(__thiscall*)(void*);
                    reinterpret_cast<GetRenderData>(0x110B2690)(actor);
                    reinterpret_cast<UpdateRenderData>(0x110B15A0)(actor);
                    struct Leaves {int* data;int count,capacity;};
                    auto& leafArray=*reinterpret_cast<Leaves*>(actor+0x1C4);auto savedLeaves=leafArray;
                    std::vector<int> allLeaves;
                    if(allLeafLightCandidates)
                    {
                        auto model=*reinterpret_cast<unsigned char**>(level+0x13C);int leaves=*reinterpret_cast<int*>(model+0xC0);
                        for(int leaf=0;leaf<leaves;++leaf)allLeaves.push_back(leaf);
                        leafArray={allLeaves.data(),leaves,leaves};
                    }
                    auto& shadowFlags=*reinterpret_cast<unsigned*>(actor+0x2EC);auto originalShadowFlags=shadowFlags;
                    if(skipMeshShadowOcclusion)shadowFlags&=~0x08000000u;
                    reinterpret_cast<Illuminate>(0x110E8190)(mesh, actor, 0);
                    shadowFlags=originalShadowFlags;
                    leafArray=savedLeaves;
                    // Same actor finalization immediately following Illuminate
                    // in the native LIGHT APPLY loop (1108122F).
                    using FinishActorLighting = void(__thiscall*)(void*);
                    reinterpret_cast<FinishActorLighting>((*reinterpret_cast<void***>(actor))[0x8C / 4])(actor);
                    reinterpret_cast<UpdateRenderData>(0x110B15A0)(actor);
                    // Illuminate builds shadow masks; LIGHT APPLY then bakes
                    // the final colour stream separately at 110087B2.
                    using BakeColors = void(__cdecl*)(void*, void*, void*);
                    reinterpret_cast<BakeColors>(0x11195230)(mesh,
                        *reinterpret_cast<void**>(actor + 0x22C),
                        reinterpret_cast<GetRenderData>(0x110B2690)(actor));
                }
            }
            DescribeLighting("relit_cooked_meshes");
        }
        DumpNativeBsp("NativeCookedBsp.json");
        auto editor = *reinterpret_cast<unsigned char**>(kEditor);
        auto level = *reinterpret_cast<unsigned char**>(editor+0x130);
        auto objects = *reinterpret_cast<unsigned char***>(0x11697B70);
        int count = *reinterpret_cast<int*>(0x11697B74);
        auto active = *reinterpret_cast<unsigned char***>(level+0x2C);
        int activeCount = *reinterpret_cast<int*>(level+0x30);
        for (int i=0; active && i<activeCount; ++i) if (active[i] && active[i][0x2D0] == 2) {
            fprintf(report,"platform_sample name=%s byte2d0=%u\n",ObjectName(active[i]),active[i][0x2D0]);
            fflush(report);
            DescribeActorFields(active[i]);
        }
        for (int i=0; objects && i<count; ++i) {
            auto object = objects[i];
            if (!object) continue;
            const char* name = ObjectName(object);
            if (!strcmp(name, "SPresenceTrigger2071")) {
                auto outer = *reinterpret_cast<unsigned char**>(object+0x18);
                auto package = *reinterpret_cast<void**>(level+0x18);
                fprintf(report, "actor_membership name=%s address=%p class=%s outer=%s outer_ptr=%p outer_is_level=%d outer_is_package=%d platform=%u flags=%08x active=%d pc=%d xbox=%d\n",
                    name, object, ObjectName(*reinterpret_cast<void**>(object+0x24)),
                    ObjectName(outer),outer,outer==level,outer==package,object[0x2D0],
                    *reinterpret_cast<unsigned int*>(object+0x1C), ContainsActor(level,0x2C,object),
                    ContainsActor(level,0x3C,object), ContainsActor(level,0x4C,object));
                fflush(report);
                DescribeActorFields(object);
            }
        }
    }
    int result=originalExec(exec, command, output);
    if (result && command && strstr(command, "MAP EXPORT")) {
        if (strstr(command, "Imported.t3d")) DescribeLighting("imported");
        if (strstr(command, "Built.t3d")) DescribeLighting("built");
        if (strstr(command, "Reopened.t3d")) DescribeLighting("reopened");
    }
    if (inspectCookedOnly && result && command && strstr(command,"MAP EXPORT") && strstr(command,"Actors.t3d")) {
        // Keep the native cooked export, then deliberately stop conversion
        // before reconstruction. Production cleanup disposes of the level.
        inspectedCooked=true;
        return 0;
    }
    if (command && (!strcmp(command,"MAP REBUILD") || !strcmp(command,"BSP REBUILD") || !strcmp(command,"LIGHT APPLY")))
        DumpLeafLights(command);
    if (tracePointEnabled && command && (!strcmp(command,"MAP REBUILD") || !strcmp(command,"BSP REBUILD"))) {
        auto editor=*reinterpret_cast<unsigned char**>(kEditor);
        auto level=*reinterpret_cast<unsigned char**>(editor+0x130);
        auto model=*reinterpret_cast<unsigned char**>(level+0x13C);
        fprintf(report,"trace_build command=%s empty=%d\n",command,EmptyAt(model,tracedPoint));
        fflush(report);
        DumpNativeBsp(!strcmp(command,"MAP REBUILD") ? "TraceAfterMapRebuild.json" : "TraceAfterBspRebuild.json");
    }
    return result;
}

void ObserveEditorExec() {
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    auto vtable = *reinterpret_cast<void***>(editor+0x28);
    DWORD protection;
    if (VirtualProtect(vtable, sizeof(void*), PAGE_READWRITE, &protection)) {
        originalExec = reinterpret_cast<ExecFn>(vtable[0]);
        vtable[0] = reinterpret_cast<void*>(&ObserveExec);
        DWORD ignored;
        VirtualProtect(vtable, sizeof(void*), protection, &ignored);
    }
}

void Snapshot(const char* stage) {
    DumpLeafLights(stage);
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    auto level = editor ? *reinterpret_cast<unsigned char**>(editor + 0x130) : nullptr;
    auto model = level ? *reinterpret_cast<unsigned char**>(level + 0x13C) : nullptr;
    int brushes = 0;
    int polys = 0;
    int actors = level ? *reinterpret_cast<int*>(level + 0x30) : 0;
    auto data = level ? *reinterpret_cast<unsigned char***>(level + 0x2C) : nullptr;
    for (int i = 1; data && i < actors; ++i) {
        auto actor = data[i];
        if (!actor || *reinterpret_cast<void**>(actor + 0x24)
                      != reinterpret_cast<void*>(0x1181B048)) continue;
        auto brush = *reinterpret_cast<unsigned char**>(actor + 0x238);
        auto brushPolys = brush ? *reinterpret_cast<unsigned char**>(brush + 0x50) : nullptr;
        if (brushPolys) {
            ++brushes;
            polys += *reinterpret_cast<int*>(brushPolys + 0x2C);
        }
    }
    fprintf(report, "%s level=%p model=%p actors=%d brushes=%d brush_polys=%d nodes=%d surfaces=%d root_outside=%d\n",
        stage, level, model, actors, brushes, polys,
        model ? *reinterpret_cast<int*>(model + 0x58) : 0,
        model ? *reinterpret_cast<int*>(model + 0x98) : 0,
        model ? *reinterpret_cast<int*>(model + 0x104) : -1);
    if (data && data[0]) fprintf(report, "actor0=%p vtable=%p class=%p flags=%08x deleted_list=%p\n",
        data[0], *reinterpret_cast<void**>(data[0]), *reinterpret_cast<void**>(data[0]+0x24),
        *reinterpret_cast<unsigned int*>(data[0]+0x1C), *reinterpret_cast<void**>(level+0x3A50));
    fflush(report);
}

bool RestoreSourceNormals(const char* source) {
    std::ifstream input(source);
    std::map<std::string,std::vector<ProbePoint>> normals;
    std::string line,actor;
    while(std::getline(input,line)) {
        const auto first=line.find_first_not_of(" \t\r");
        if(first==std::string::npos) continue;
        line.erase(0,first);
        if(line.rfind("Begin Actor Class=Brush Name=RecoveredVolume",0)==0) {
            actor=line.substr(line.find("Name=")+5);
            actor.erase(actor.find_last_not_of(" \r\t")+1);
        } else if(line=="End Actor" || line=="End Actor\r") actor.clear();
        else if(!actor.empty() && line.rfind("Normal ",0)==0) {
            ProbePoint value;
            if(sscanf_s(line.c_str()+6,"%f,%f,%f",&value.x,&value.y,&value.z)!=3) return false;
            normals[actor].push_back(value);
        }
    }
    auto editor=*reinterpret_cast<unsigned char**>(kEditor);
    auto level=*reinterpret_cast<unsigned char**>(editor+0x130);
    auto actors=*reinterpret_cast<unsigned char***>(level+0x2C);
    const int count=*reinterpret_cast<int*>(level+0x30);
    std::size_t changed=0,matched=0;
    for(int i=0;i<count;++i) {
        auto actorObject=actors[i];
        if(!actorObject) continue;
        const auto found=normals.find(ObjectName(actorObject));
        if(found==normals.end()) continue;
        auto brush=*reinterpret_cast<unsigned char**>(actorObject+0x238);
        auto polys=brush ? *reinterpret_cast<unsigned char**>(brush+0x50) : nullptr;
        if(!polys || *reinterpret_cast<int*>(polys+0x2C)!=static_cast<int>(found->second.size())) return false;
        auto data=*reinterpret_cast<unsigned char**>(polys+0x28);
        for(std::size_t p=0;p<found->second.size();++p) {
            memcpy(data+p*0x14C+0x0C,&found->second[p],sizeof(ProbePoint));
            ++changed;
        }
        ++matched;
    }
    fprintf(report,"restored_source_normals brushes=%zu polygons=%zu expected_brushes=%zu\n",matched,changed,normals.size());
    fflush(report);
    return matched==normals.size() && matched!=0;
}

int Exec(const std::string& command) {
    Record("exec", command.substr(0, command.find_first_of("\r\n")).c_str());
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    void* output = *reinterpret_cast<void**>(kWarn);
    if (!editor || !output) return 0;
    void* exec = editor + 0x28;
    using Fn = int(__thiscall*)(void*, const char*, void*);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(exec))[0])(
        exec, command.c_str(), output);
}

bool HasGeometry() {
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    auto level = editor ? *reinterpret_cast<unsigned char**>(editor + 0x130) : nullptr;
    auto model = level ? *reinterpret_cast<unsigned char**>(level + 0x13C) : nullptr;
    if (!model || *reinterpret_cast<int*>(model + 0x58) <= 0
        || *reinterpret_cast<int*>(model + 0x98) <= 0) return false;
    auto nodes = *reinterpret_cast<unsigned char**>(model + 0x54);
    int count = *reinterpret_cast<int*>(model + 0x58);
    int hullSlots = *reinterpret_cast<int*>(model + 0xB4);
    int invalid = 0, highest = -1;
    for (int i = 0; i < count; ++i) {
        unsigned index = *reinterpret_cast<unsigned short*>(nodes + size_t(i)*0x5C + 0x54);
        if (index == 0xFFFF) continue;
        highest = (std::max)(highest, static_cast<int>(index));
        if (index >= 0x8000 || index >= static_cast<unsigned>(hullSlots)) ++invalid;
    }
    fprintf(report, "collision_bounds slots=%d highest_start=%d invalid=%d\n", hullSlots, highest, invalid);
    fflush(report);
    return hullSlots >= 0 && hullSlots <= 65535 && invalid == 0;
}

using CollisionBoxConstructor = void*(__thiscall*)(void*, const float*, const float*);

int ConstructBoxWithFullX87(CollisionBoxConstructor constructor, float* box) {
    const unsigned words[6] = {0xC31DA05B,0xC2AC8914,0x42A7FE1F,0x3C0AC10D,0x42F5FAF4,0x42A8007C};
    float endpoints[6];
    memcpy(endpoints, words, sizeof(words));
    unsigned char saved[108];
    int result = 1;
    __asm {
        fnsave saved
        fldcw saved
        fld1
        fld1
        fld1
        fld1
        fld1
        fld1
        fld1
        fld1
    }
    __try { constructor(box, endpoints, endpoints + 3); }
    __except (GetExceptionCode() == EXCEPTION_BREAKPOINT ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        result = 0;
    }
    __asm { frstor saved }
    return result;
}

bool CheckCollisionBoxConstructor(bool stress) {
    constexpr uintptr_t call = 0x11172427;
    if (*reinterpret_cast<unsigned char*>(call) != 0xE8) return false;
    auto target = call + 5 + *reinterpret_cast<int*>(call + 1);
    using Constructor = void*(__thiscall*)(void*, const float*, const float*);
    auto native = reinterpret_cast<Constructor>(0x10EC26E0);
    auto replacement = reinterpret_cast<Constructor>(target);
    if (target == 0x10E03E7D) { Record("collision_box_constructor", "stock"); return !stress; }
    unsigned seed = 0xC1A4D;
    for (int trial = 0; trial < 10000; ++trial) {
        float endpoints[6], expected[6], actual[6];
        for (auto& value : endpoints) {
            seed = seed * 1664525u + 1013904223u;
            value = (static_cast<int>(seed >> 8) - 8388608) / 32.0f;
        }
        native(expected, endpoints, endpoints + 3);
        if (replacement(actual, endpoints, endpoints + 3) != actual
            || memcmp(actual, expected, sizeof(actual))) return false;
    }
    Record("collision_box_constructor", "10000 native comparisons matched");
    if (stress) {
        float nativeBox[6] = {}, replacementBox[6] = {};
        int stockResult = ConstructBoxWithFullX87(native, nativeBox);
        int replacementResult = ConstructBoxWithFullX87(replacement, replacementBox);
        const unsigned expected[6] = {0xC31DA05B,0xC2AC8914,0x42A7FE1F,0x3C0AC10D,0x42F5FAF4,0x42A8007C};
        fprintf(report, "full_x87_stack native_returned=%d native_exact=%d replacement_returned=%d replacement_exact=%d\n",
            stockResult, !memcmp(nativeBox, expected, sizeof(expected)), replacementResult,
            !memcmp(replacementBox, expected, sizeof(expected)));
        fflush(report);
        if (!replacementResult || memcmp(replacementBox, expected, sizeof(expected))) return false;
    }
    return true;
}

bool CheckCollisionAccessors()
{
    const uint32_t words[]={0xc0a2680a,0xc1a5a33a,0xc11cdb23,0x4096e7d5,0x41a558ae,0x411d1d15};
    float box[6];memcpy(box,words,sizeof(box));
    const uintptr_t sites[]={0x111722DB,0x111722E8,0x11172346,0x11172354,0x111723B2,0x111723C0};
    for(int i=0;i<6;++i) {
        const auto site=sites[i];
        if(*reinterpret_cast<unsigned char*>(site)!=0xE8)return false;
        auto getter=reinterpret_cast<void*(__thiscall*)(void*)>(site+5+*reinterpret_cast<int*>(site+1));
        unsigned char saved[108],before[108],after[108];
        void* result=nullptr;
        __asm {
            fnsave saved
            fldcw saved
            fld1
            fld1
            fld1
            fld1
            fld1
            fld1
            fld1
            fld1
            fnsave before
            frstor before
        }
        __try { result=getter(box); }
        __except(GetExceptionCode()==EXCEPTION_BREAKPOINT ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {}
        __asm { fnsave after }
        __asm { frstor saved }
        if(result!=box+(i%2?3:0) || memcmp(before,after,sizeof(before))) {
            fprintf(report,"collision_accessor_failure site=%08lX pointer_correct=%d x87_unchanged=%d\n",
                static_cast<unsigned long>(site),result==box+(i%2?3:0),!memcmp(before,after,sizeof(before)));
            Record("FAIL","Collision bounds accessor changed live x87 state or returned incorrect bounds.");return false;
        }
    }
    Record("collision_bounds_accessors","All six decoder calls returned crash-dump bounds with an unchanged full x87 stack.");
    return true;
}

bool Rebuild() {
    DescribeLighting("before_normal_build");
    DescribeBspLighting("before_normal_build");
    struct Array { void* data{}; int count{}; int capacity{}; } first, second;
    void* editor = *reinterpret_cast<void**>(kEditor);
    using Bracket = void(__thiscall*)(void*, void*, void*);
    reinterpret_cast<Bracket>(0x10E06A1A)(editor, &first, &second);
    const auto config=directory/"native_recovery_test.ini";
    const bool geometryOnly=GetPrivateProfileIntA("test","geometry_only_build",0,config.string().c_str())!=0;
    bool ok = Exec("MAP REBUILD");
    if(ok && !geometryOnly)ok=Exec("BSP REBUILD") && Exec("LIGHT APPLY");
    using Paths = void(__thiscall*)(void*);
    if (ok && !geometryOnly) { Record("rebuild_paths"); reinterpret_cast<Paths>(0x10E06399)(editor); }
    reinterpret_cast<Bracket>(0x10E02EEC)(editor, &first, &second);
    // Finish lighting exactly as the ordinary Build UI does. Save switches
    // platforms and restores mesh instances from this cache.
    auto context = *reinterpret_cast<unsigned char**>(0x11691D7C);
    using CacheMeshLighting = void(__thiscall*)(void*, int);
    if (ok) reinterpret_cast<CacheMeshLighting>(0x10E06605)(editor, *reinterpret_cast<int*>(context + 0x78));
    DescribeLighting("after_normal_build");
    DescribeBspLighting("after_normal_build");
    return ok;
}

bool SetCubeBrush(int halfExtent, float x=0, float y=0, float z=0) {
    // Six outward-facing quads form one ordinary subtractive room brush.
    const int vertices[6][4][3] = {
        {{512,-512,-512},{512,512,-512},{512,512,512},{512,-512,512}},
        {{-512,-512,-512},{-512,-512,512},{-512,512,512},{-512,512,-512}},
        {{-512,512,-512},{-512,512,512},{512,512,512},{512,512,-512}},
        {{-512,-512,-512},{512,-512,-512},{512,-512,512},{-512,-512,512}},
        {{-512,-512,512},{512,-512,512},{512,512,512},{-512,512,512}},
        {{-512,-512,-512},{-512,512,-512},{512,512,-512},{512,-512,-512}}
    };
    std::ostringstream command;
    command << std::setprecision(9);
    command << "BRUSH SET\nBegin PolyList\n";
    for (const auto& face : vertices) {
        command << "Begin Polygon Texture=TXT_INI.BSP.BETON_Lit_Spec Flags=0\n";
        for (const auto& point : face)
            command << "Vertex " << x+point[0]*halfExtent/512 << ',' << y+point[1]*halfExtent/512
                << ',' << z+point[2]*halfExtent/512 << "\n";
        command << "End Polygon\n";
    }
    command << "End PolyList\n";
    return Exec(command.str()) != 0;
}

bool EmptyAt(unsigned char* model, ProbePoint point) {
    auto nodes = *reinterpret_cast<unsigned char**>(model+0x54);
    int count = *reinterpret_cast<int*>(model+0x58);
    bool outside = *reinterpret_cast<int*>(model+0x104) != 0;
    for (int index=0, step=0; index>=0 && index<count && step<=count; ++step) {
        auto node = nodes + index*0x5C;
        auto plane = reinterpret_cast<float*>(node);
        bool front = plane[0]*point.x + plane[1]*point.y + plane[2]*point.z - plane[3] >= 0;
        bool csg = node[0x5A] != 0 && !(node[0x5B]&0x21);
        outside = front ? outside || csg : outside && !csg;
        index = *reinterpret_cast<int*>(node+(front ? 0x34 : 0x30));
    }
    return outside;
}

bool FindEmptyEditSpace(unsigned char* model, ProbePoint& point) {
    auto nodes = *reinterpret_cast<unsigned char**>(model+0x54);
    int count = *reinterpret_cast<int*>(model+0x58);
    auto verts = *reinterpret_cast<unsigned char**>(model+0x64);
    auto points = *reinterpret_cast<ProbePoint**>(model+0x84);
    for (int index=0; index<count; ++index) {
        auto node = nodes + index*0x5C;
        int vertexCount = node[0x5A];
        if (vertexCount<3 || (node[0x5B]&0x21)) continue;
        int pool = *reinterpret_cast<int*>(node+0x28);
        ProbePoint center;
        for (int i=0; i<vertexCount; ++i) {
            auto vertex = points[*reinterpret_cast<unsigned short*>(verts+(pool+i)*8)];
            center.x += vertex.x/vertexCount;
            center.y += vertex.y/vertexCount;
            center.z += vertex.z/vertexCount;
        }
        auto normal = reinterpret_cast<float*>(node);
        for (float offset : {-32.0f,32.0f}) {
            point = {center.x+normal[0]*offset,center.y+normal[1]*offset,center.z+normal[2]*offset};
            bool fits = EmptyAt(model,point);
            for (int x : {-8,8}) for (int y : {-8,8}) for (int z : {-8,8})
                fits = fits && EmptyAt(model,{point.x+x,point.y+y,point.z+z});
            if (fits) return true;
        }
    }
    return false;
}

bool MakeFixture(const char* runtimePath, bool rootOutside) {
    if (!Exec("MAP NEW")) return false;
    auto editor = *reinterpret_cast<unsigned char**>(kEditor);
    auto level = *reinterpret_cast<unsigned char**>(editor + 0x130);
    auto model = *reinterpret_cast<unsigned char**>(level + 0x13C);
    *reinterpret_cast<int*>(model + 0x104) = rootOutside ? 1 : 0;
    if (!SetCubeBrush(512) || !Exec(rootOutside ? "BRUSH ADD" : "BRUSH SUBTRACT") || !Rebuild()) return false;
    Snapshot("authored_fixture");
    if (!HasGeometry()) return false;
    auto sourcePath = std::filesystem::path(runtimePath).parent_path().parent_path()
        / "MapsEd" / std::filesystem::path(runtimePath).filename();
    using Save = int(__thiscall*)(void*, const char*);
    Record("saving_authored_fixture", sourcePath.string().c_str());
    if (!reinterpret_cast<Save>(0x10E0416B)(*reinterpret_cast<void**>(kEditor), sourcePath.string().c_str())) return false;
    if (!std::filesystem::exists(runtimePath)) {
        Record("FAIL", "Normal fixture save did not emit the expected runtime sibling.");
        return false;
    }
    return true;
}

} // namespace (keep standard/JSON headers outside the anonymous namespace)
#include "WorkflowNativeTests.h"

using LightingBytes=std::map<std::string,std::vector<unsigned char>>;
LightingBytes ReadLightingBytes(bool bsp) {
    LightingBytes result;
    auto level=*reinterpret_cast<unsigned char**>(*reinterpret_cast<unsigned char**>(kEditor)+0x130);
    auto actors=*reinterpret_cast<unsigned char***>(level+0x2c);
    const int count=*reinterpret_cast<int*>(level+0x30);
    for(int i=0;i<count;++i) {
        auto actor=actors[i];if(!actor || !*reinterpret_cast<void**>(actor+0x100))continue;
        auto instance=*reinterpret_cast<unsigned char**>(actor+0x22c);if(!instance)continue;
        auto colors=*reinterpret_cast<unsigned char**>(instance+0x38);
        int size=*reinterpret_cast<int*>(instance+0x3c)*4;
        if(colors && size>0 && size<4000000)result[ObjectName(actor)]={colors,colors+size};
    }
    if(bsp) {
        auto model=*reinterpret_cast<unsigned char**>(level+0x13c);
        auto textures=*reinterpret_cast<unsigned char**>(model+0xe0);
        for(int i=0;i<*reinterpret_cast<int*>(model+0xe4);++i) {
            auto compressed=textures+i*0x70+0x14;
            using Load=unsigned char*(__thiscall*)(void*,int);
            auto pixels=reinterpret_cast<Load>(0x111b2400)(compressed,0);
            int size=*reinterpret_cast<int*>(compressed+0x14);
            if(pixels && size>0 && size<=2048*2048*4)result["bsp_"+std::to_string(i)]={pixels,pixels+size};
        }
    }
    return result;
}
int lightingAnswer=IDNO;
bool lightingAnswered=false;
bool selectedLightingDialog=false;
bool matchingLightingDialog=false;
bool matchingSucceeded=false;
std::string matchingBrush;
void CALLBACK AnswerLighting(HWND,UINT,UINT_PTR timer,DWORD) {
    auto dialog=WorkflowProbe::FindDialog(matchingLightingDialog ? "Match Selected BSP Lighting" : (selectedLightingDialog ? "Recalculate Selected Lighting" : "Recalculate Lighting"));
    if(dialog) {
        if(!GetDlgItem(dialog,IDYES)) {
            KillTimer(nullptr,timer);
            char text[2048]{};GetDlgItemTextA(dialog,65535,text,sizeof(text));
            if(matchingLightingDialog && !strncmp(text,"Matched ",8)) {matchingSucceeded=true;Record("local_match",text);}
            else Record("FAIL",text);
            // The editor's message-box hook labels IDCANCEL as OK on notices.
            const int button=GetDlgItem(dialog,IDOK) ? IDOK : IDCANCEL;
            SendMessage(dialog,WM_COMMAND,MAKEWPARAM(button,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(dialog,button)));return;
        }
        lightingAnswered=true;SendMessage(GetDlgItem(dialog,lightingAnswer),BM_CLICK,0,0);
    }
}
bool ChooseLighting(int answer,bool selected=false,bool matching=false) {
    selectedLightingDialog=selected;
    matchingLightingDialog=matching;matchingSucceeded=false;
    lightingAnswer=answer;lightingAnswered=false;
    auto timer=SetTimer(nullptr,0,50,AnswerLighting);
    SendMessage(frameWindow,WM_COMMAND,matching ? 40931 : (selected ? 40930 : 40929),0);
    KillTimer(nullptr,timer);
    return lightingAnswered && (!matching || answer==IDNO || matchingSucceeded);
}

LightingBytes ReadChartLighting() {
    auto result=ReadLightingBytes(false);
    auto level=*reinterpret_cast<unsigned char**>(*reinterpret_cast<unsigned char**>(kEditor)+0x130);
    auto model=*reinterpret_cast<unsigned char**>(level+0x13c);
    auto charts=*reinterpret_cast<unsigned char**>(model+0xec);
    auto textures=*reinterpret_cast<unsigned char**>(model+0xe0);
    for(int i=0;i<*reinterpret_cast<int*>(model+0xf0);++i) {
        auto chart=charts+i*0xa4;
        int atlas=*reinterpret_cast<int*>(chart+8);
        if(atlas<0 || atlas>=*reinterpret_cast<int*>(model+0xe4))continue;
        int x=*reinterpret_cast<int*>(chart+0x14),y=*reinterpret_cast<int*>(chart+0x18);
        int w=*reinterpret_cast<int*>(chart+0x1c),h=*reinterpret_cast<int*>(chart+0x20);
        auto compressed=textures+atlas*0x70+0x14;
        using Load=unsigned char*(__thiscall*)(void*,int);
        auto pixels=reinterpret_cast<Load>(0x111b2400)(compressed,0);
        auto nodes=*reinterpret_cast<unsigned char**>(model+0x54);
        std::string key="chart";
        for(int n=0;n<*reinterpret_cast<int*>(model+0x58);++n)
            if(nodes[n*0x5c+0x5a]>=3 && *reinterpret_cast<int*>(nodes+n*0x5c+0x3c)==i)key+="_"+std::to_string(n);
        if(key=="chart")continue;
        auto& bytes=result[key];
        for(int row=0;row<h;++row)bytes.insert(bytes.end(),pixels+((y+row)*512+x)*4,pixels+((y+row)*512+x+w)*4);
    }
    return result;
}
bool VerifySelectedLighting() {
    auto level=*reinterpret_cast<unsigned char**>(*reinterpret_cast<unsigned char**>(kEditor)+0x130);
    auto actors=*reinterpret_cast<unsigned char***>(level+0x2c);
    const int count=*reinterpret_cast<int*>(level+0x30);
    unsigned char* chosen=nullptr;
    for(int i=0;i<count;++i) if(auto actor=actors[i]) {
        *reinterpret_cast<unsigned*>(actor+0x2f4)&=~0x40u;
        auto instance=*reinterpret_cast<unsigned char**>(actor+0x22c);
        if(!chosen && *reinterpret_cast<void**>(actor+0x100) && instance
            && *reinterpret_cast<int*>(instance+0x3c)>0) chosen=actor;
    }
    if(!chosen) {Record("FAIL","Selective fixture has no lit static mesh.");return false;}
    auto model=*reinterpret_cast<unsigned char**>(level+0x13c);
    auto surfaces=*reinterpret_cast<unsigned char**>(model+0x94);
    const int surfaceCount=*reinterpret_cast<int*>(model+0x98);
    for(int i=0;i<surfaceCount;++i)*reinterpret_cast<unsigned*>(surfaces+i*0x2c+0x14)&=~0x02000000u;
    *reinterpret_cast<unsigned*>(chosen+0x2f4)|=0x40;
    auto instance=*reinterpret_cast<unsigned char**>(chosen+0x22c);
    // A distinctive previous bake proves that the chosen mesh is recalculated.
    memset(*reinterpret_cast<void**>(instance+0x38),0x7f,*reinterpret_cast<int*>(instance+0x3c)*4);
    auto before=ReadChartLighting();
    if(!ChooseLighting(IDNO,true) || before!=ReadChartLighting()) {Record("FAIL","Selective cancellation changed lighting.");return false;}
    if(!ChooseLighting(IDYES,true)) {Record("FAIL","Selective mesh confirmation missing.");return false;}
    auto after=ReadChartLighting();
    const auto name=ObjectName(chosen);
    if(before[name]==after[name]) {Record("FAIL","Selected mesh was not recalculated.");return false;}
    before.erase(name);after.erase(name);
    if(before!=after) {Record("FAIL","Selective mesh bake changed unselected mesh or BSP bytes.");return false;}
    *reinterpret_cast<unsigned*>(chosen+0x2f4)&=~0x40u;
    // Select one chart's faces, expanding only shared-chart ownership. Prove
    // that the remaining BSP and every mesh keep exactly their previous bake.
    auto nodes=*reinterpret_cast<unsigned char**>(model+0x54);
    const int nodeCount=*reinterpret_cast<int*>(model+0x58);
    auto charts=*reinterpret_cast<unsigned char**>(model+0xec);
    std::set<int> faces,selectedCharts;
    if(!matchingBrush.empty()) for(int i=0;i<surfaceCount;++i) {
        auto poly=*reinterpret_cast<unsigned char**>(surfaces+i*0x2c+0x20);
        auto brush=poly ? *reinterpret_cast<unsigned char**>(poly+0x14c) : nullptr;
        if(brush && (","+matchingBrush+",").find(std::string(",")+ObjectName(brush)+",")!=std::string::npos) faces.insert(i);
    }
    if(!matchingBrush.empty() && faces.empty()) {Record("FAIL","Requested lighting brush was not found.");return false;}
    for(int n=0;n<nodeCount && faces.empty();++n) {
        auto node=nodes+n*0x5c;int chart=*reinterpret_cast<int*>(node+0x3c);
        if(node[0x5a]>=3 && chart>=0 && *reinterpret_cast<int*>(charts+chart*0xa4+8)>=0)
            faces.insert(*reinterpret_cast<int*>(node+0x2c));
    }
    bool expanded=true;
    while(expanded) {
        expanded=false;
        for(int n=0;n<nodeCount;++n) {
            auto node=nodes+n*0x5c;if(node[0x5a]<3)continue;
            int chart=*reinterpret_cast<int*>(node+0x3c),face=*reinterpret_cast<int*>(node+0x2c);
            if(chart<0)continue;
            if(faces.count(face))expanded=selectedCharts.insert(chart).second || expanded;
            if(selectedCharts.count(chart))expanded=faces.insert(face).second || expanded;
        }
    }
    if(faces.empty() || faces.size()>=size_t(surfaceCount)) {Record("FAIL","No independent BSP selection.");return false;}
    for(int face:faces)*reinterpret_cast<unsigned*>(surfaces+face*0x2c+0x14)|=0x02000000u;
    auto textures=*reinterpret_cast<unsigned char**>(model+0xe0);
    std::set<std::string> selectedKeys;
    for(int i:selectedCharts) {
        auto chart=charts+i*0xa4;int atlas=*reinterpret_cast<int*>(chart+8);
        if(atlas<0)continue;
        std::string key="chart";
        for(int n=0;n<nodeCount;++n)if(nodes[n*0x5c+0x5a]>=3 && *reinterpret_cast<int*>(nodes+n*0x5c+0x3c)==i)key+="_"+std::to_string(n);
        selectedKeys.insert(key);
        auto compressed=textures+atlas*0x70+0x14;
        using Load=unsigned char*(__thiscall*)(void*,int);
        auto pixels=reinterpret_cast<Load>(0x111b2400)(compressed,0);
        int x=*reinterpret_cast<int*>(chart+0x14),y=*reinterpret_cast<int*>(chart+0x18);
        int w=*reinterpret_cast<int*>(chart+0x1c),h=*reinterpret_cast<int*>(chart+0x20);
        for(int row=0;row<h;++row)memset(pixels+((y+row)*512+x)*4,0x7f,w*4);
    }
    before=ReadChartLighting();
    if(!ChooseLighting(IDYES,true))return false;
    after=ReadChartLighting();
    bool changed=false;
    for(const auto& key:selectedKeys){changed=before[key]!=after[key] || changed;before.erase(key);after.erase(key);}
    if(!changed || before!=after) {
        Record("FAIL","Selected BSP did not change independently of unselected BSP and mesh lighting.");return false;
    }
    before=ReadChartLighting();
    if(!ChooseLighting(IDNO,true,true) || before!=ReadChartLighting()) {Record("FAIL","Matching cancellation changed lighting.");return false;}
    if(!ChooseLighting(IDYES,true,true)) {Record("FAIL","Local lighting matching did not complete.");return false;}
    after=ReadChartLighting();
    bool corrected=false;
    uint64_t nativeSum=0,matchedSum=0,sampleCount=0;
    for(const auto& key:selectedKeys){
        corrected=before[key]!=after[key] || corrected;
        for(size_t i=0;i<before[key].size() && i<after[key].size();++i) if(i%4!=3) {
            nativeSum+=before[key][i];matchedSum+=after[key][i];++sampleCount;
        }
        before.erase(key);after.erase(key);
    }
    if(before!=after) {Record("FAIL","Local matching changed unselected BSP or meshes.");return false;}
    if(!corrected) {Record("FAIL","Matching fixture did not exercise a colour correction.");return false;}
    Record("local_match_values", ("native RGB sum="+std::to_string(nativeSum)+" matched="+std::to_string(matchedSum)
        +" components="+std::to_string(sampleCount)).c_str());
    for(int i=0;i<surfaceCount;++i)*reinterpret_cast<unsigned*>(surfaces+i*0x2c+0x14)&=~0x02000000u;
    auto combined=ReadLightingBytes(true);
    if(!Exec("LIGHT APPLY") || combined!=ReadLightingBytes(true)) {Record("FAIL","Combined bake was not protected.");return false;}
    Record("selected_lighting","Cancellation, selected mesh bake, exact unselected mesh/BSP bytes, selected BSP bake and resumed preservation passed.");
    return true;
}
namespace {
#include "NativeRecoveryMenuProbe.h"

void RunTest() {
    auto configuration = (directory / "native_recovery_test.ini").string();
    char matchingBrushName[256]{};
    GetPrivateProfileStringA("test","lighting_brush","",matchingBrushName,sizeof(matchingBrushName),configuration.c_str());
    matchingBrush=matchingBrushName;
    char source[MAX_PATH] = {}, destination[MAX_PATH] = {}, dll[MAX_PATH] = {};
    GetPrivateProfileStringA("test", "source", "", source, MAX_PATH, configuration.c_str());
    GetPrivateProfileStringA("test", "destination", "", destination, MAX_PATH, configuration.c_str());
    GetPrivateProfileStringA("test", "editor_dll", "", dll, MAX_PATH, configuration.c_str());
    if (report) fclose(report);
    report = _fsopen((directory / "native_recovery_report.txt").string().c_str(), "w", _SH_DENYNO);
    if (!report) return;
    Record("started");
    bool boundsProbe = GetPrivateProfileIntA("test", "fpu_bounds_probe", 0, configuration.c_str()) != 0;
    if (!CheckCollisionBoxConstructor(boundsProbe)) { Record("FAIL", "Collision box constructor differs from native bounds."); return; }
    if(!CheckCollisionAccessors())return;
    if (boundsProbe && !GetPrivateProfileIntA("test", "recovery_menu", 0, configuration.c_str())) {
        Record("PASS", "Native collision bounds comparison and full x87 stack probe completed."); return;
    }
    ProbeNativeNormalThreshold();
    ObserveEditorExec();
    traceSoftBodies=GetPrivateProfileIntA("test","trace_soft_bodies",0,configuration.c_str())!=0;
    stepCookedSoftBodies=GetPrivateProfileIntA("test","step_cooked_soft_bodies",0,configuration.c_str())!=0;
    traceLeafLights=GetPrivateProfileIntA("test","trace_leaf_lights",0,configuration.c_str())!=0;
    traceLighting=GetPrivateProfileIntA("test","trace_lighting",0,configuration.c_str())!=0;
    relightCookedMeshes=GetPrivateProfileIntA("test","relight_cooked_meshes",0,configuration.c_str())!=0;
    allLeafLightCandidates=GetPrivateProfileIntA("test","all_leaf_light_candidates",0,configuration.c_str())!=0;
    skipMeshShadowOcclusion=GetPrivateProfileIntA("test","skip_mesh_shadow_occlusion",0,configuration.c_str())!=0;
    inspectCookedOnly=GetPrivateProfileIntA("test","inspect_cooked_only",0,configuration.c_str())!=0;
    compactionEnabled=GetPrivateProfileIntA("test","compact_points",0,configuration.c_str())!=0;
    char pointText[128]={},actorText[128]={};
    GetPrivateProfileStringA("test","trace_point","",pointText,sizeof(pointText),configuration.c_str());
    GetPrivateProfileStringA("test","trace_actor","",actorText,sizeof(actorText),configuration.c_str());
    tracePointEnabled=sscanf_s(pointText,"%f,%f,%f",&tracedPoint.x,&tracedPoint.y,&tracedPoint.z)==3;
    tracedActor=actorText;
    if(compactionEnabled || tracePointEnabled) ObserveCsgCompaction();
    Snapshot("initial");
    bool rootOutsideFixture = GetPrivateProfileIntA("test", "root_outside", 0, configuration.c_str()) != 0;
    if (GetPrivateProfileIntA("test", "import_text_only", 0, configuration.c_str())) {
        if (!Exec("MAP NEW") || !Exec(std::string("MAP IMPORT FILE=\"") + source + "\"")) {
            Record("FAIL", "Native text import probe failed."); return;
        }
        auto editor = *reinterpret_cast<unsigned char**>(kEditor);
        auto level = *reinterpret_cast<void**>(editor+0x130);
        using Finalize = void(__thiscall*)(void*, void*);
        reinterpret_cast<Finalize>((*reinterpret_cast<void***>(editor))[0xE0/4])(editor,level);
        if(GetPrivateProfileIntA("test","restore_text_normals",0,configuration.c_str())
            && !RestoreSourceNormals(source)) {
            Record("FAIL","Text normal restoration did not match the imported brush polygons."); return;
        }
        Snapshot("text_imported");
        DescribeSoftBodies("text_imported");
        if (GetPrivateProfileIntA("test","build_imported_text",0,configuration.c_str())) {
            struct Array {void* data{};int count{};int capacity{};} first,second;
            using Bracket=void(__thiscall*)(void*,void*,void*);
            reinterpret_cast<Bracket>(0x10E06A1A)(editor,&first,&second);
            bool built=Exec("MAP REBUILD") && Exec("BSP REBUILD");
            reinterpret_cast<Bracket>(0x10E02EEC)(editor,&first,&second);
            Snapshot("text_built");
            DumpNativeBsp("NativeImportedBsp.json");
            if (!built) {Record("FAIL","Native imported geometry build failed.");return;}
        }
        if (!Exec("MAP EXPORT FILE=\"" + (directory / "import_only.t3d").string() + "\"")) {
            Record("FAIL", "Native text import probe could not export."); return;
        }
        Record("PASS", "Native text import/export probe completed; compare import_only.t3d for semantic verification.");
        return;
    }
    if (GetPrivateProfileIntA("test", "reopen_only", 0, configuration.c_str())) {
        if (!Exec(std::string("MAP LOAD FILE=\"") + source + "\"") || !HasGeometry()) {
            Record("FAIL", "Fresh-process ordinary source load failed."); return;
        }
        Snapshot("fresh_normal_reopen");
        char authoringFile[32768]{};
        if(GetEnvironmentVariableA("SCCT_AUTHORING_VERIFY",authoringFile,sizeof(authoringFile)))
        {
            try
            {
                using J=nlohmann::json;using Request=int(__cdecl*)(const char*,char*,unsigned);
                auto module=GetModuleHandleA(std::filesystem::path(dll).filename().string().c_str());
                auto request=reinterpret_cast<Request>(GetProcAddress(module,"ReloadedWorkflowRequest"));
                if(!request)request=reinterpret_cast<Request>(GetProcAddress(module,"_ReloadedWorkflowRequest"));
                if(!request)throw std::runtime_error("Authoring API missing.");
                auto call=[&](const J& q){std::vector<char> buffer(64*1024*1024);auto text=q.dump();auto ok=request(text.c_str(),buffer.data(),static_cast<unsigned>(buffer.size()));if(ok!=1)throw std::runtime_error(buffer.data());return J::parse(buffer.data()).at("result");};
                auto snapshot=call({{"op","authoring.export"}});std::ofstream(directory/"authoring-live-snapshot.json")<<snapshot.dump(2);
                if(strcmp(authoringFile,"snapshot"))
                {
                    J patch;std::ifstream(authoringFile)>>patch;patch["map"]=snapshot.at("map");
                    auto preview=call({{"op","authoring.preview"},{"document",patch}});std::ofstream(directory/"authoring-file-preview.json")<<preview.dump(2);
                    auto result=call({{"op","authoring.apply"},{"document",patch}});J states=J::array();
                    for(const auto& id:result.at("created"))states.push_back(call({{"op","magic.inspect"},{"actor",id}}));
                    for(const auto& op:patch.at("operations"))if(op.at("op")=="update")states.push_back(call({{"op","magic.inspect"},{"actor",op.at("actor")}}));
                    std::ofstream(directory/"authoring-file-expected.json")<<states.dump(2);
                    J bounds=J::array();
                    for(const auto& id:result.at("created"))
                    {
                        auto op=std::find_if(patch.at("operations").begin(),patch.at("operations").end(),[&](const J& o){return o.at("op")=="create" && id.at("path").get<std::string>().ends_with("."+o.at("id").get<std::string>());});
                        if(op!=patch.at("operations").end() && op->at("properties").contains("StaticMesh")){call({{"op","select"},{"actors",J::array({id})}});bounds.push_back({{"actor",id},{"bounds",call({{"op","mesh.bounds"}})}});}
                    }
                    std::ofstream(directory/"authoring-file-bounds.json")<<bounds.dump(2);
                    Exec("TRANSACTION UNDO");Exec("TRANSACTION REDO");
                    for(const auto& a:states)if(call({{"op","magic.inspect"},{"actor",a.at("actor")}}).at("values")!=a.at("values"))throw std::runtime_error("Authoring redo mismatch.");
                    using Save=int(__thiscall*)(void*,const char*);
                    if(!reinterpret_cast<Save>(0x10E0416B)(*reinterpret_cast<void**>(kEditor),destination))throw std::runtime_error("Authoring save failed.");
                    if(!Exec(std::string("MAP LOAD FILE=\"")+destination+"\""))throw std::runtime_error("Authoring reopen failed.");
                    J mismatches=J::array();
                    for(const auto& a:states){auto current=call({{"op","magic.inspect"},{"actor",a.at("actor")}});if(current.at("values")!=a.at("values"))mismatches.push_back({{"actor",a.at("actor")},{"diff",J::diff(a.at("values"),current.at("values"))}});}
                    std::ofstream(directory/"authoring-file-reopen-diff.json")<<mismatches.dump(2);
                    if(!mismatches.empty())throw std::runtime_error("Authoring reopen mismatch; see authoring-file-reopen-diff.json.");
                    for(const auto& b:bounds){call({{"op","select"},{"actors",J::array({b.at("actor")})}});if(call({{"op","mesh.bounds"}})!=b.at("bounds"))throw std::runtime_error("Mesh appearance bounds changed on save/reopen.");}
                    std::ofstream(directory/"authoring-file-result.json")<<states.dump(2);
                }
                Record("PASS","Authoring file snapshot/validation, import, undo/redo and save/reopen probe completed.");
            }
            catch(const std::exception& e){Record("FAIL",e.what());}
            return;
        }

        if(GetPrivateProfileIntA("test","match_lighting_after_load",0,configuration.c_str())) {
            auto level=*reinterpret_cast<unsigned char**>(*reinterpret_cast<unsigned char**>(kEditor)+0x130);
            auto actors=*reinterpret_cast<unsigned char***>(level+0x2c);
            int selected=0;
            for(int i=0;i<*reinterpret_cast<int*>(level+0x30);++i) if(auto actor=actors[i]) {
                bool choose=(","+matchingBrush+",").find(std::string(",")+ObjectName(actor)+",")!=std::string::npos;
                auto& flags=*reinterpret_cast<unsigned*>(actor+0x2f4);
                flags=(flags&~0x40u)|(choose?0x40u:0);
                selected+=choose;
            }
            auto model=*reinterpret_cast<unsigned char**>(level+0x13c);
            auto surfaces=*reinterpret_cast<unsigned char**>(model+0x94);
            for(int i=0;i<*reinterpret_cast<int*>(model+0x98);++i)*reinterpret_cast<unsigned*>(surfaces+i*0x2c+0x14)&=~0x02000000u;
            if(!selected || !ChooseLighting(IDYES,true,true)) {Record("FAIL","Match immediately after load failed.");return;}
            Record("PASS","Match immediately after loading source completed without a prior geometry or lighting build.");
            return;
        }
        if(GetPrivateProfileIntA("test","verify_play_map_save",0,configuration.c_str())) {
            constexpr uintptr_t call=0x10e212aa;
            if(*reinterpret_cast<unsigned char*>(call)!=0xe8) {Record("FAIL","Play Map save call missing.");return;}
            auto target=call+5+*reinterpret_cast<int*>(call+1);
            using SavePlayMap=int(__thiscall*)(void*);
            if(!reinterpret_cast<SavePlayMap>(target)(*reinterpret_cast<void**>(kEditor))) {
                Record("FAIL","Play Map temporary save failed.");return;
            }
            auto temporary=directory.parent_path()/"Packages"/"Maps"/"Autoplay.sdc";
            if(!std::filesystem::is_regular_file(temporary) || !std::filesystem::file_size(temporary)) {
                Record("FAIL","Play Map temporary runtime file is missing.");return;
            }
            Record("PASS","Native Play Map save call finalized Autoplay. Runtime copy retained for compiled-map inspection; no game launched.");
            return;
        }
        const bool verifyLighting=GetPrivateProfileIntA("test","verify_preserved_lighting",0,configuration.c_str())!=0;
        auto originalLighting=verifyLighting ? ReadLightingBytes(false) : LightingBytes{};
        if (!Rebuild() || !HasGeometry()) {
            Record("FAIL", "Fresh-process source rebuild failed."); return;
        }
        Snapshot("fresh_normal_rebuild");
        auto rebuiltLighting=verifyLighting ? ReadLightingBytes(true) : LightingBytes{};
        if(verifyLighting && (originalLighting.empty() || originalLighting!=ReadLightingBytes(false)
            || !Exec("LIGHT APPLY") || rebuiltLighting!=ReadLightingBytes(true))) {
            Record("FAIL","Default rebuild or ordinary lighting command changed protected mesh lighting.");return;
        }
        if(GetPrivateProfileIntA("test","verify_selected_lighting",0,configuration.c_str())) {
            if(!VerifySelectedLighting())return;
            rebuiltLighting=ReadLightingBytes(true);
        }
        const auto saved = directory.parent_path()/"Packages"/"MapsEd"/std::filesystem::path(source).filename();
        auto mainFrame=*reinterpret_cast<unsigned char**>(0x1165E80C);
        using SetFilename=void(__thiscall*)(void*,const char*);
        if (!mainFrame) { Record("FAIL", "Native level window was not available for File Save."); return; }
        reinterpret_cast<SetFilename>(0x10E05E1C)(mainFrame,saved.string().c_str());
        SendMessageA(frameWindow,WM_COMMAND,40007,0);
        if (!Exec("MAP NEW") || !Exec("MAP LOAD FILE=\""+saved.string()+"\"") || !HasGeometry()) {
            Record("FAIL", "Fresh-process source save/reopen failed."); return;
        }
        DescribeLighting("after_saved_reopen");
        DescribeBspLighting("after_saved_reopen");
        if(verifyLighting) {
            if(rebuiltLighting!=ReadLightingBytes(true)) {Record("FAIL","Preserved mesh/BSP lighting changed during File Save/reopen.");return;}
            if(!ChooseLighting(IDNO) || rebuiltLighting!=ReadLightingBytes(true)) {Record("FAIL","Cancelling recalculation changed lighting.");return;}
            if(!ChooseLighting(IDYES) || rebuiltLighting==ReadLightingBytes(true)) {Record("FAIL","Explicit lighting recalculation did not replace the bake.");return;}
            auto recalculated=ReadLightingBytes(true);
            if(!Exec("LIGHT APPLY") || recalculated!=ReadLightingBytes(true)) {Record("FAIL","New bake was not protected after explicit recalculation.");return;}
            Record("preserved_lighting","Exact mesh preservation, BSP/mesh save roundtrip, cancellation, explicit recalculation and resumed protection verified.");
        }
        if (!Exec("MAP EXPORT FILE=\"" + (directory / "fresh_source.t3d").string() + "\"")) {
            Record("FAIL", "Fresh-process source export failed."); return;
        }
        Record("PASS", "Fresh editor ordinary source load/rebuild/File Save/reopen/export completed without invoking recovery.");
        return;
    }
    if (GetPrivateProfileIntA("test", "generate_fixture", 0, configuration.c_str())
        && !MakeFixture(source, rootOutsideFixture)) {
        Record("FAIL", "Could not build and cook the isolated source-room fixture."); return;
    }
    if (GetPrivateProfileIntA("test", "import_baseline", 0, configuration.c_str())) {
        auto t3d = (directory / "authored_baseline.t3d").string();
        if (!Exec("MAP EXPORT FILE=\"" + t3d + "\"") || !Exec("MAP NEW")
            || !Exec("MAP IMPORT FILE=\"" + t3d + "\"")) {
            Record("FAIL", "Baseline source export/new/import failed."); return;
        }
        Snapshot("baseline_imported");
        auto editor = *reinterpret_cast<unsigned char**>(kEditor);
        auto level = *reinterpret_cast<void**>(editor + 0x130);
        auto finalize = (*reinterpret_cast<void***>(editor))[0xE0 / 4];
        fprintf(report, "native_import_finalize=%p\n", finalize); fflush(report);
        using FinalizeImport = void(__thiscall*)(void*, void*);
        reinterpret_cast<FinalizeImport>(finalize)(editor, level);
        Snapshot("baseline_finalized");
        auto sourceLevel = reinterpret_cast<unsigned char*>(level);
        auto activeActors = *reinterpret_cast<unsigned char***>(sourceLevel + 0x2C);
        int activeCount = *reinterpret_cast<int*>(sourceLevel + 0x30);
        *reinterpret_cast<int*>(sourceLevel + 0x40) = 0;
        *reinterpret_cast<int*>(sourceLevel + 0x50) = 0;
        using AddUnique = int(__thiscall*)(void*, void*);
        for (int i=0;i<activeCount;++i) if (activeActors[i]) {
            auto actor = activeActors[i];
            if (actor[0x2D0] != 2) reinterpret_cast<AddUnique>(0x10E025CD)(sourceLevel+0x3C, &actor);
            if (actor[0x2D0] != 1) reinterpret_cast<AddUnique>(0x10E025CD)(sourceLevel+0x4C, &actor);
        }
        Snapshot("baseline_platforms_synchronized");
        if (!Rebuild()) { Record("FAIL", "Baseline rebuild failed."); return; }
        Snapshot("baseline_built");
        using Save = int(__thiscall*)(void*, const char*);
        Record("saving_baseline", destination);
        if (!reinterpret_cast<Save>(0x10E0416B)(*reinterpret_cast<void**>(kEditor), destination)) {
            Record("FAIL", "Baseline source save rejected."); return;
        }
        Snapshot("baseline_saved");
        Record("PASS", "Normal T3D export/new/import/build/save baseline completed without recovery.");
        return;
    }
    HMODULE editorDll = GetModuleHandleA(dll);
    if (!editorDll) editorDll = GetModuleHandleA(std::filesystem::path(dll).filename().string().c_str());
    if (GetPrivateProfileIntA("test", "workflow_tools", 0, configuration.c_str())) {
        RunWorkflowTests(editorDll, destination, GetPrivateProfileIntA("test", "workflow_restart", 0, configuration.c_str()) != 0,
            GetPrivateProfileIntA("test", "brush_grid_snap_only", 0, configuration.c_str()) != 0,
            GetPrivateProfileIntA("test", "character_preview_only", 0, configuration.c_str()) != 0,
            GetPrivateProfileIntA("test", "stair_smooth_only", 0, configuration.c_str()) != 0,
            GetPrivateProfileIntA("test", "optimise_only", 0, configuration.c_str()) != 0,
            GetPrivateProfileIntA("test", "mcp_bridge_only", 0, configuration.c_str()) != 0);
        return;
    }
    using RecoverFn = int(__cdecl*)(const char*, const char*, char*, unsigned int);
    auto recover = reinterpret_cast<RecoverFn>(GetProcAddress(editorDll, "ReloadedRecoverMapToSource"));
    if (!recover) recover = reinterpret_cast<RecoverFn>(GetProcAddress(editorDll, "_ReloadedRecoverMapToSource"));
    if (!recover) { Record("FAIL", "Recovery export was not found."); return; }
    char error[8192] = {};
    bool expectFailure = GetPrivateProfileIntA("test", "expect_recovery_failure", 0, configuration.c_str()) != 0;
    if (expectFailure) {
        auto previousFilename = std::filesystem::path(source);
        if (GetPrivateProfileIntA("test", "generate_fixture", 0, configuration.c_str()))
            previousFilename = previousFilename.parent_path().parent_path() / "MapsEd" / previousFilename.filename();
        using SetFilename = void(__thiscall*)(void*, const char*);
        reinterpret_cast<SetFilename>(0x10E05E1C)(*reinterpret_cast<void**>(0x1165E80C), previousFilename.string().c_str());
        Record("previous_map_filename", previousFilename.string().c_str());
    }
    Record("recovering", source);
    bool menuRecovery = GetPrivateProfileIntA("test", "recovery_menu", 0, configuration.c_str()) != 0;
    // Merge option and cancellation go through the options export; its
    // report (counts and rows) is recorded line by line.
    using RecoverWithOptionsFn = int(__cdecl*)(const char*, const char*, int, int, char*, unsigned int, char*, unsigned int);
    auto recoverWithOptions = reinterpret_cast<RecoverWithOptionsFn>(GetProcAddress(editorDll, "ReloadedRecoverMapToSourceWithOptions"));
    if (!recoverWithOptions) recoverWithOptions = reinterpret_cast<RecoverWithOptionsFn>(GetProcAddress(editorDll, "_ReloadedRecoverMapToSourceWithOptions"));
    const int mergeOption = static_cast<int>(GetPrivateProfileIntA("test", "recovery_merge", 1, configuration.c_str()));
    char cancelText[32] = {};
    GetPrivateProfileStringA("test", "recovery_cancel_stage", "", cancelText, sizeof(cancelText), configuration.c_str());
    const int cancelStage = cancelText[0] ? atoi(cancelText) : -1;
    auto recordReport = [](const char* text) {
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);) Record("recovery_report", line.c_str());
    };
    if (cancelStage >= 0) {
        static char cancelReport[65536] = {};
        if (!recoverWithOptions) { Record("FAIL", "Recovery options export was not found."); return; }
        const auto outputs = std::filesystem::path(destination);
        const auto runtime = outputs.parent_path().parent_path() / "Maps" / outputs.filename();
        const auto scratch = outputs.parent_path() / "Recovery" / outputs.stem();
        const auto assets = outputs.parent_path().parent_path() / "StaticMeshes" / (outputs.stem().string() + "_Assets.usx");
        const int result = recoverWithOptions(source, destination, mergeOption, cancelStage, error, sizeof(error), cancelReport, sizeof(cancelReport));
        recordReport(cancelReport);
        Record("cancel_result", error);
        auto window = *reinterpret_cast<unsigned char**>(0x1165E80C);
        auto filename = window ? reinterpret_cast<const char*>(window + 0x58) : "";
        Snapshot("cancelled_recovery");
        if (result || !strstr(error, "cancelled")) { Record("FAIL", "Recovery did not stop at the requested cancel point."); return; }
        if (!strstr(cancelReport, "cancelled=1")) { Record("FAIL", "The outcome was not marked cancelled."); return; }
        if (!window || filename[0]) { Record("FAIL", "Cancelled recovery left a File Save target."); return; }
        for (const auto& path : {outputs, runtime, scratch, assets})
            if (std::filesystem::exists(path)) { Record("FAIL", ("Cancelled recovery left " + path.string()).c_str()); return; }
        auto editor = *reinterpret_cast<unsigned char**>(kEditor);
        auto level = *reinterpret_cast<unsigned char**>(editor + 0x130);
        auto model = *reinterpret_cast<unsigned char**>(level + 0x13C);
        if (*reinterpret_cast<int*>(model + 0x98) != 0) { Record("FAIL", "Cancelled recovery left BSP surfaces in the open map."); return; }
        // A new map is solid: carving a room proves geometry builds again.
        if (!SetCubeBrush(128) || !Exec("BRUSH SUBTRACT") || !Rebuild() || !HasGeometry()) {
            Record("FAIL", "Normal editing failed after a cancelled recovery."); return;
        }
        Snapshot("cancel_followup_normal_edit");
        Record("PASS", ("Cancel at stage " + std::to_string(cancelStage) + " reset the editor to a new map, removed its files and allowed normal editing.").c_str());
        return;
    }
    static char recoveryReport[65536] = {};
    auto recoverSelected = [&]() -> int {
        if (!recoverWithOptions) return recover(source, destination, error, sizeof(error));
        const int result = recoverWithOptions(source, destination, mergeOption, -1, error, sizeof(error), recoveryReport, sizeof(recoveryReport));
        recordReport(recoveryReport);
        return result;
    };
    if (!(menuRecovery ? RecoverThroughMenu(editorDll, source, destination, sizeof(destination))
                       : recoverSelected())) {
        if (inspectCookedOnly && inspectedCooked) {
            Record("PASS", "Cooked-file inspection completed; no source reconstruction or gameplay validation was performed.");
            return;
        }
        if (expectFailure) {
            auto window = *reinterpret_cast<unsigned char**>(0x1165E80C);
            auto filename = window ? reinterpret_cast<const char*>(window + 0x58) : "";
            Record("expected_recovery_error", error);
            Record("failure_map_filename", filename);
            if (!window || filename[0]) {
                Record("FAIL", "Failed recovery retained the previous normal File Save target."); return;
            }
            auto previousSource = std::filesystem::path(source).parent_path().parent_path()
                / "MapsEd" / std::filesystem::path(source).filename();
            if (!Exec("MAP NEW") || !Exec("MAP LOAD FILE=\"" + previousSource.string() + "\"")
                || !SetCubeBrush(128) || !Exec(rootOutsideFixture ? "BRUSH SUBTRACT" : "BRUSH ADD")
                || !Rebuild() || !HasGeometry()) {
                Record("FAIL", "Normal source work failed after rejected recovery."); return;
            }
            Snapshot("failure_followup_normal_edit");
            auto editor = *reinterpret_cast<unsigned char**>(kEditor);
            auto level = *reinterpret_cast<unsigned char**>(editor+0x130);
            auto model = *reinterpret_cast<unsigned char**>(level+0x13C);
            if (*reinterpret_cast<int*>(model+0x98) <= 6) {
                Record("FAIL", "Normal geometry rebuild stayed blocked after rejected recovery."); return;
            }
            Record("PASS", "Expected geometry-write failure cleared the prior File Save target and allowed ordinary source load/edit/rebuild.");
            return;
        }
        DumpNativeBsp("NativeFailedBsp.json");
        Snapshot("failed_recovery");
        Record("FAIL", error);
        return;
    }
    if (expectFailure) { Record("FAIL", "Recovery unexpectedly accepted the blocked geometry write."); return; }
    Snapshot("recovered_source");
    auto levelWindow = *reinterpret_cast<unsigned char**>(0x1165E80C);
    const char* currentFilename = levelWindow ? reinterpret_cast<const char*>(levelWindow + 0x58) : "";
    Record("current_map_filename", currentFilename);
    if (_stricmp(currentFilename, destination) != 0) {
        Record("FAIL", "The normal File Save target was not updated to the recovered source."); return;
    }
    if (!HasGeometry()) { Record("FAIL", "Recovered source contains no BSP."); return; }
    if (!Exec("MAP NEW")) { Record("FAIL", "MAP NEW was not accepted."); return; }
    Snapshot("blank");
    if (!Exec(std::string("MAP LOAD FILE=\"") + destination + "\"")) {
        Record("FAIL", "Normal MAP LOAD was not accepted."); return;
    }
    Snapshot("normal_reopen");
    if (!HasGeometry()) { Record("FAIL", "Normal reopen contains no BSP."); return; }
    if (!Rebuild()) {
        Record("FAIL", "A normal build command was not accepted."); return;
    }
    Snapshot("normal_rebuild");
    if (!HasGeometry()) { Record("FAIL", "Normal rebuild removed the BSP."); return; }
    bool generatedFixture = GetPrivateProfileIntA("test", "generate_fixture", 0, configuration.c_str()) != 0;
    if (generatedFixture || GetPrivateProfileIntA("test", "edit_geometry", 0, configuration.c_str())) {
        // Add a solid brush in verified empty space, or carve a cavity from
        // the additive fixture. Exercise a real edit and ordinary save/reopen.
        auto editor = *reinterpret_cast<unsigned char**>(kEditor);
        auto level = *reinterpret_cast<unsigned char**>(editor + 0x130);
        auto model = *reinterpret_cast<unsigned char**>(level + 0x13C);
        int originalRootOutside = *reinterpret_cast<int*>(model+0x104);
        ProbePoint editPoint;
        if (!generatedFixture && !FindEmptyEditSpace(model,editPoint)) {
            Record("FAIL", "No safe empty BSP space was found for the isolated geometry edit."); return;
        }
        fprintf(report,"geometry_edit_point %.9g,%.9g,%.9g initial_empty=%d\n",editPoint.x,editPoint.y,editPoint.z,EmptyAt(model,editPoint));
        fflush(report);
        bool subtract = generatedFixture && rootOutsideFixture;
        std::string editedActor;
        if (!generatedFixture && !EditActorTag(level,editedActor)) {
            Record("FAIL", "A retained actor could not accept an ordinary Tag property edit."); return;
        }
        if (!SetCubeBrush(generatedFixture ? 128 : 4,editPoint.x,editPoint.y,editPoint.z)
            || !Exec(subtract ? "BRUSH SUBTRACT" : "BRUSH ADD") || !Rebuild()) {
            Record("FAIL", "Recovered source could not rebuild the geometry edit."); return;
        }
        Snapshot("edited_geometry");
        model = *reinterpret_cast<unsigned char**>(level + 0x13C);
        int editedSurfaces = *reinterpret_cast<int*>(model + 0x98);
        if (EmptyAt(model,editPoint) != subtract) { Record("FAIL", "Geometry edit did not change BSP space as expected."); return; }
        // Exercise File > Save, including its normal post-save SDC repair.
        // Calling the lower-level serializer directly bypasses that handler
        // and is not the save path an editor user actually uses.
        Record("save_normal_command",destination);
        SendMessageA(frameWindow,WM_COMMAND,40007,0);
        if (!Exec("MAP NEW") || !Exec(std::string("MAP LOAD FILE=\"") + destination + "\"")) {
            Record("FAIL", "Edited source could not save and reopen normally."); return;
        }
        Snapshot("edited_normal_reopen");
        level = *reinterpret_cast<unsigned char**>(editor + 0x130);
        model = *reinterpret_cast<unsigned char**>(level + 0x13C);
        if (*reinterpret_cast<int*>(model + 0x98) != editedSurfaces) {
            Record("FAIL", "Edited source lost BSP surfaces after save/reopen."); return;
        }
        if (*reinterpret_cast<int*>(model + 0x104) != originalRootOutside || EmptyAt(model,editPoint) != subtract) {
            Record("FAIL", "Edited source changed its root outside/solid state."); return;
        }
        if (!generatedFixture && !HasEditedTag(FindStaticMeshActor(level,editedActor.c_str()))) {
            Record("FAIL", "The actor Tag property edit did not survive ordinary save/reopen."); return;
        }
    }
    auto exported = std::filesystem::path(destination).replace_extension(".roundtrip.t3d");
    if (!Exec("MAP EXPORT FILE=\"" + exported.string() + "\"")) {
        Record("FAIL", "Normal MAP EXPORT was not accepted."); return;
    }
    Record("PASS", "Recovery, ordinary reopen, geometry/BSP/lighting/path rebuild, requested geometry edit roundtrip, and T3D export completed.");
}

LRESULT CALLBACK OnMessage(int code, WPARAM removed, LPARAM parameter) {
    if (code >= 0) {
        auto message = reinterpret_cast<CWPSTRUCT*>(parameter);
        if (message->message == kRun) {
            UnhookWindowsHookEx(hook);
            hook = nullptr;
            RunTest();
        }
    }
    return CallNextHookEx(hook, code, removed, parameter);
}

BOOL CALLBACK RecordStartupErrorControl(HWND window, LPARAM) {
    char text[4096] = {};
    GetWindowTextA(window,text,sizeof(text));
    if (report && text[0]) Record("startup_error_text",text);
    return TRUE;
}

BOOL CALLBACK FindEditorWindow(HWND window, LPARAM threadAddress) {
    DWORD process = 0;
    DWORD thread = GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId()) return TRUE;
    if (!GetPrivateProfileIntA("test", "visible_editor", 0,
        (directory / "native_recovery_test.ini").string().c_str())) ShowWindow(window, SW_HIDE);
    char title[256] = {};
    GetWindowTextA(window, title, sizeof(title));
    char klass[128] = {};
    GetClassNameA(window, klass, sizeof(klass));
    if (report) {
        fprintf(report, "window=%p thread=%lu class=%s title=%s\n", window, thread, klass, title);
        fflush(report);
    }
    if (!strcmp(klass,"#32770") && !strcmp(title,"Critical Error")) {
        EnumChildWindows(window,RecordStartupErrorControl,0);
        FILE* failure = _fsopen((directory/"native_recovery_report.txt").string().c_str(),"w",_SH_DENYNO);
        if (failure) {
            fputs("FAIL Native editor startup reached a Critical Error; see native_recovery_boot.txt.\n",failure);
            fclose(failure);
        }
        startupFailed=true;
        return FALSE;
    }
    if (strstr(klass, "WEditorFrame")) {
        *reinterpret_cast<DWORD*>(threadAddress) = thread;
        frameWindow = window;
    }
    return TRUE;
}

DWORD WINAPI WaitUntilReady(void*) {
    report = _fsopen((directory / "native_recovery_boot.txt").string().c_str(), "w", _SH_DENYNO);
    if (report) Record("waiting_for_editor");
    for (int attempt = 0; attempt < 240; ++attempt) {
        DWORD uiThread = 0;
        EnumWindows(FindEditorWindow, reinterpret_cast<LPARAM>(&uiThread));
        if (startupFailed) return 1;
        auto editor = *reinterpret_cast<unsigned char**>(kEditor);
        if (report) {
            fprintf(report, "attempt=%d editor=%p uiThread=%lu\n", attempt, editor, uiThread);
            fflush(report);
        }
        if (uiThread && editor && *reinterpret_cast<void**>(editor + 0x130)) {
            // Initial windows exist before startup completes. Wait briefly,
            // then execute through the frame's actual UI-thread message hook.
            Sleep(5000);
            // Both threads are in this process; no cross-process DLL load is needed.
            hook = SetWindowsHookExA(WH_CALLWNDPROC, OnMessage, nullptr, uiThread);
            if (report) { fprintf(report, "hook=%p error=%lu\n", hook, GetLastError()); fflush(report); }
            DWORD_PTR result = 0;
            if (hook && SendMessageTimeoutA(frameWindow, kRun, 0, 0,
                SMTO_ABORTIFHUNG, 300000, &result)) return 0;
            return 1;
        }
        Sleep(250);
    }
    return 1;
}
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        directory = std::filesystem::path(path).parent_path();
        if (HANDLE worker = CreateThread(nullptr, 0, WaitUntilReady, nullptr, 0, nullptr)) CloseHandle(worker);
    }
    return TRUE;
}
