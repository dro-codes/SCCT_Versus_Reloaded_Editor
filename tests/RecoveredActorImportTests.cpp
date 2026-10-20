#include "../Reloaded.Editor/RecoveredActorImport.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
    const char* kRecovered = R"(Begin Map
Begin Actor Name="OldLevel" Class="Engine.LevelInfo"
    Title="Recovered test"
    Author="Mapper"
    Summary=LevelSummary'MyLevel.LevelSummary'
    TimeSeconds=100
    DefaultGameType=Class'Engine.GameInfo'
    NavigationPointList=PlayerStart'MyLevel.Start0'
    Name="OldLevel"
End Actor
Begin Actor Class=Brush Name=OriginalBrush
    Begin Brush Name=StrippedModel
       Begin PolyList
       End PolyList
    End Brush
End Actor
Begin Actor Class=Engine.BlockingVolume Name=Volume0
    Level=LevelInfo'MyLevel.OldLevel'
    Region=(Zone=LevelInfo'MyLevel.OldLevel',iLeaf=-1)
    Owner=Trigger'MyLevel.Trigger0'
    Begin Brush Name=VolumeModel
       Begin PolyList
          Begin Polygon Texture=External.Wall
             Vertex +0,+0,+0
             Vertex +10,+0,+0
             Vertex +0,+10,+0
          End Polygon
       End PolyList
    End Brush
    Brush=Model'MyLevel.VolumeModel'
End Actor
Begin Actor Class=Trigger Name=Trigger0
    Location=(X=12.500000,Y=-8,Z=9)
    DrawScale3D=(Z=1.000000)
    m_Platform=1
    Platform=PLF_PC_Only
    Event="Trigger event"
    Base=BlockingVolume'MyLevel.Volume0'
    Instigator=Trigger'MyLevel.Trigger0'
    Target=LevelInfo'MyLevel.OldLevel'
    Description="LevelInfo'MyLevel.OldLevel' is literal text"
    StaticMesh=StaticMesh'External.Mesh'
    StaticMeshInstance=StaticMeshInstance'MyLevel.TransientInstance'
    StaticMeshInstanceOnXBOX=StaticMeshInstance'MyLevel.TransientXboxInstance'
    Begin Object Class=SpriteEmitter Name=Sprite0
        Level=7
        Owner=42
        Texture=Texture'External.Sprite'
    End Object
    Emitters(0)=SpriteEmitter'MyLevel.Trigger0.Sprite0'
End Actor
Begin Surface
End Surface
Begin GE
    GE ADD LG X1=1 Y1=2 Z1=3 X2=4 Y2=5 Z2=6
End GE
End Map
)";
    const char* kFresh = R"(Begin Map
Begin Actor Class=LevelInfo Name=FreshLevel
    Title="Untitled"
End Actor
Begin Actor Class=Brush Name=Builder0
    Begin Brush Name=FreshBuilderModel
       Begin PolyList
       End PolyList
    End Brush
    Brush=Model'MyLevel.FreshBuilderModel'
    Name="Builder0"
End Actor
End Map
)";
    const char* kGeometry = R"(Begin Map
Begin Actor Class=Brush Name=RecoveredBrush000001
    Begin Brush Name=RecoveredModel000001
       Begin PolyList
       End PolyList
    End Brush
    Brush=Model'MyLevel.RecoveredModel000001'
End Actor
End Map
)";

    std::string Replace(std::string text, const std::string& from, const std::string& to)
    {
        const auto pos = text.find(from);
        assert(pos != std::string::npos);
        text.replace(pos, from.size(), to);
        return text;
    }

    std::string ReplaceAll(std::string text, const std::string& from, const std::string& to)
    {
        std::size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos)
        {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
        return text;
    }
}

int main(int argc, char** argv)
{
    using namespace RecoveredActorImport;
    PreparedMap prepared;
    std::string error, composed;
    assert(Prepare(kRecovered, prepared, error));
    assert(error.empty());
    assert(prepared.actorCount == 2);
    assert(prepared.mapSectionsT3d.find("GE ADD LG X1=1") != std::string::npos);
    assert(prepared.removedBrushCount == 1);
    assert(prepared.levelInfoName == "OldLevel");
    assert(prepared.levelInfoActor.find("Title=\"Recovered test\"") != std::string::npos);
    assert(prepared.levelInfoActor.find("Summary=") == std::string::npos);
    assert(prepared.levelInfoActor.find("TimeSeconds=") == std::string::npos);
    assert(prepared.actorsT3d.find("OriginalBrush") == std::string::npos);
    assert(prepared.actorsT3d.find("Region=") == std::string::npos);
    assert(prepared.actorsT3d.find("StaticMeshInstance=") == std::string::npos);
    assert(prepared.actorsT3d.find("StaticMeshInstanceOnXBOX=") == std::string::npos);
    assert(prepared.actorsT3d.find("Begin Brush Name=VolumeModel") != std::string::npos);
    assert(prepared.actorsT3d.find("Level=7") != std::string::npos);
    assert(prepared.actorsT3d.find("Owner=42") != std::string::npos);
    assert(prepared.actorsT3d.find("Owner=Trigger'MyLevel.Trigger0'") != std::string::npos);
    assert(prepared.actorsT3d.find("Base=BlockingVolume'MyLevel.Volume0'") != std::string::npos);
    assert(prepared.actorsT3d.find("Instigator=Trigger'MyLevel.Trigger0'") != std::string::npos);

    assert(ComposeSourceMap(prepared, kFresh, kGeometry, composed, error));
    assert(composed.find("Begin Actor Class=LevelInfo Name=LevelInfo0") != std::string::npos);
    assert(composed.find("Title=\"Recovered test\"") != std::string::npos);
    assert(composed.find("Title=\"Untitled\"") == std::string::npos);
    assert(composed.find("Target=LevelInfo'MyLevel.LevelInfo0'") != std::string::npos);
    assert(composed.find("GE ADD LG X1=1") > composed.find("Name=RecoveredBrush000001"));
    assert(composed.find("Description=\"LevelInfo'MyLevel.OldLevel' is literal text\"")
        != std::string::npos);
    assert(composed.find("Begin Actor Class=Brush Name=Builder0")
        < composed.find("Begin Actor Class=Engine.BlockingVolume"));
    assert(VerifySourceMap(prepared, composed, error));
    assert(VerifySourceMap(prepared,
        Replace(composed, "GE ADD LG X1=1 Y1=2", "GE ADD LG X1=+1.000000 Y1=2.000000"), error));
    assert(!VerifySourceMap(prepared, Replace(composed, "Name=Trigger0", "Name=MissingActor"), error));
    assert(error.find("Trigger0") != std::string::npos);
    assert(!VerifySourceMap(prepared, Replace(composed, "Class=Trigger Name=Trigger0",
                                                         "Class=Light Name=Trigger0"), error));
    assert(!VerifySourceMap(prepared, Replace(composed, "Title=\"Recovered test\"",
                                                         "Title=\"Wrong title\""), error));
    assert(error.find("title") != std::string::npos);
    assert(!VerifySourceMap(prepared, Replace(composed, "Author=\"Mapper\"", ""), error));
    assert(error.find("author") != std::string::npos);
    assert(!VerifySourceMap(prepared, Replace(composed, "External.Mesh", "Other.Mesh"), error));
    assert(error.find("staticmesh") != std::string::npos);
    assert(!VerifySourceMap(prepared, Replace(composed, "Location=(X=12.500000", "Location=(X=15"), error));
    assert(error.find("location") != std::string::npos);
    // A sub-precision default component re-imports as the default and is omitted.
    assert(VerifySourceMap(prepared, Replace(composed, "DrawScale3D=(Z=1.000000)", ""), error));
    assert(!VerifySourceMap(prepared, Replace(composed, "DrawScale3D=(Z=1.000000)", "DrawScale3D=(Z=2)"), error));
    assert(error.find("drawscale3d") != std::string::npos);
    {
        PreparedMap scaled;
        std::string scaledMap;
        assert(Prepare(Replace(kRecovered, "DrawScale3D=(Z=1.000000)", "DrawScale3D=(Z=2.000000)"), scaled, error));
        assert(ComposeSourceMap(scaled, kFresh, kGeometry, scaledMap, error));
        assert(!VerifySourceMap(scaled, Replace(scaledMap, "DrawScale3D=(Z=2.000000)", ""), error));
        assert(error.find("drawscale3d") != std::string::npos);
    }
    assert(!VerifySourceMap(prepared, Replace(composed, "m_Platform=1", "m_Platform=0"), error));
    assert(error.find("m_platform") != std::string::npos);
    assert(!VerifySourceMap(prepared, Replace(composed, "m_Platform=1", ""), error));
    assert(error.find("m_platform") != std::string::npos);
    assert(!VerifySourceMap(prepared, Replace(composed, "Platform=PLF_PC_Only", "Platform=PLF_XBOX_Only"), error));
    assert(error.find("platform") != std::string::npos);
    assert(!VerifySourceMap(prepared, Replace(composed,
        "GE ADD LG X1=1 Y1=2 Z1=3 X2=4 Y2=5 Z2=6", ""), error));
    assert(error.find("ledge/pipe") != std::string::npos);
    assert(!VerifySourceMap(prepared, Replace(composed, "GE ADD LG X1=1", "GE ADD LG X1=2"), error));
    assert(error.find("ledge/pipe") != std::string::npos);

    PreparedMap bad;
    assert(!Prepare(Replace(kRecovered, "External.Mesh", "MyLevel.EmbeddedMesh"), bad, error));
    assert(bad.unsupportedReferences.size() == 1);
    assert(error.find("MyLevel.EmbeddedMesh") != std::string::npos);
    assert(!Prepare(Replace(kRecovered, "Texture=External.Wall", "Texture=MyLevel.EmbeddedWall"),
                    bad, error));
    assert(error.find("MyLevel.EmbeddedWall") != std::string::npos);
    assert(!Prepare(Replace(kRecovered, "Base=BlockingVolume'MyLevel.Volume0'",
                             "Base=Brush'MyLevel.OriginalBrush'"), bad, error));
    assert(error.find("MyLevel.OriginalBrush") != std::string::npos);
    assert(Prepare(Replace(kRecovered, "External.Mesh", "MyLevel.HUD"), bad, error,
                   "RecoveryAssets_Lobby"));
    assert(bad.externalizedAssets.size() == 1);
    assert(bad.externalizedAssets[0].className == "StaticMesh");
    assert(bad.externalizedAssets[0].originalPath == "MyLevel.HUD");
    assert(bad.externalizedAssets[0].externalPath == "RecoveryAssets_Lobby.HUD");
    assert(bad.actorsT3d.find("StaticMesh'RecoveryAssets_Lobby.HUD'") != std::string::npos);
    assert(bad.actorsT3d.find("SpriteEmitter'MyLevel.Sprite0'") != std::string::npos);
    assert(Prepare(Replace(kRecovered, "Texture=External.Wall", "Texture=MyLevel.EmbeddedWall"),
                   bad, error, "RecoveryAssets_Test"));
    assert(bad.externalizedAssets.size() == 1);
    assert(bad.externalizedAssets[0].className == "Material");
    assert(bad.actorsT3d.find("Texture=RecoveryAssets_Test.EmbeddedWall") != std::string::npos);
    const auto antiportals=Replace(kRecovered,"End Map",
        "Begin Actor Class=AntiPortalActor Name=Occluder0\n"
        "AntiPortal=ConvexVolume'MyLevel.SharedOccluder'\nEnd Actor\n"
        "Begin Actor Class=StaticMeshActor Name=Occluder1\n"
        "AntiPortal=ConvexVolume'MyLevel.SharedOccluder'\nEnd Actor\nEnd Map");
    assert(!Prepare(antiportals,bad,error));
    assert(Prepare(antiportals,bad,error,"RecoveredAssets"));
    assert(bad.externalizedAssets.size()==1);
    assert(bad.externalizedAssets[0].className=="ConvexVolume");
    assert(bad.externalizedAssets[0].externalPath=="RecoveredAssets.SharedOccluder");
    assert(ComposeSourceMap(bad,kFresh,kGeometry,composed,error));
    assert(VerifySourceMap(bad,composed,error));
    assert(!VerifySourceMap(bad,Replace(composed,
        "AntiPortal=ConvexVolume'RecoveredAssets.SharedOccluder'","AntiPortal=None"),error));
    const auto stripDoor=Replace(kRecovered,"End Map",
        "Begin Actor Class=SoftBody.ESBStripDoorActor Name=Door0\n"
        "ULength=24\nVLength=48\nnbU=6\nnbV=12\nprevTopFixed=True\n"
        "windMin=(Y=-100)\nTexture=Shader'External.Glass'\n"
        "SoftBody=ESBStripDoor'MyLevel.MyLevel.OriginalDoor'\nEnd Actor\nEnd Map");
    assert(Prepare(stripDoor,bad,error));
    assert(bad.regeneratedSoftBodies==std::vector<std::string>{"Door0"});
    assert(bad.actorsT3d.find("OriginalDoor")==std::string::npos);
    assert(bad.actorsT3d.find("prevTopFixed=True")!=std::string::npos);
    assert(ComposeSourceMap(bad,kFresh,kGeometry,composed,error));
    assert(!VerifySourceMap(bad,composed,error)); // A null simulation is never successful recovery.
    const auto regenerated=Replace(composed,"SoftBody=None","SoftBody=ESBStripDoor'MyLevel.MyLevel.NewDoor'");
    assert(VerifySourceMap(bad,regenerated,error));
    assert(!VerifySourceMap(bad,Replace(regenerated,"ULength=24","ULength=25"),error));
    assert(!VerifySourceMap(bad,Replace(regenerated,"windMin=(Y=-100)","windMin=(Y=-200)"),error));
    assert(!VerifySourceMap(bad,Replace(regenerated,"ESBStripDoor'MyLevel.MyLevel.NewDoor'",
        "ESBPatch'MyLevel.MyLevel.NewDoor'"),error));
    assert(!Prepare(Replace(stripDoor,"Class=SoftBody.ESBStripDoorActor","Class=Trigger"),bad,error));
    assert(!Prepare(Replace(stripDoor,"ESBStripDoor'MyLevel.MyLevel.OriginalDoor'",
        "ESBPatch'MyLevel.MyLevel.OriginalDoor'"),bad,error));
    const auto patch = ReplaceAll(stripDoor, "ESBStripDoor", "ESBPatch");
    assert(Prepare(patch,bad,error));
    assert(bad.regeneratedSoftBodies==std::vector<std::string>{"Door0"});
    assert(ComposeSourceMap(bad,kFresh,kGeometry,composed,error));
    assert(!VerifySourceMap(bad,composed,error));
    const auto regeneratedPatch = Replace(composed,"SoftBody=None","SoftBody=ESBPatch'MyLevel.MyLevel.NewPatch'");
    assert(VerifySourceMap(bad,regeneratedPatch,error));
    assert(!VerifySourceMap(bad,Replace(regeneratedPatch,"ULength=24","ULength=25"),error));
    assert(!VerifySourceMap(bad,Replace(regeneratedPatch,"ESBPatch'MyLevel.MyLevel.NewPatch'",
        "ESBStripDoor'MyLevel.MyLevel.NewPatch'"),error));
    assert(!Prepare(Replace(patch,"Class=SoftBody.ESBPatchActor","Class=Trigger"),bad,error));
    assert(!Prepare(Replace(kRecovered, "Owner=Trigger'MyLevel.Trigger0'",
                                        "Owner=Trigger'MyLevel.MissingTrigger'"),
                    bad, error, "RecoveryAssets_Test"));
    assert(error.find("MyLevel.MissingTrigger") != std::string::npos);
    assert(bad.externalizedAssets.empty());
    assert(!Prepare(Replace(kRecovered, "Brush=Model'MyLevel.VolumeModel'",
                                        "Brush=Model'MyLevel.MyLevel.Brush'"),
                    bad, error, "RecoveryAssets_Test"));
    assert(error.find("MyLevel.MyLevel.Brush") != std::string::npos);
    assert(!Prepare(kRecovered, bad, error, "Bad.Package"));

    assert(Prepare(Replace(kRecovered, "MyLevel.Trigger0.Sprite0", "MyLevel.Sprite0"),
                   bad, error, "RecoveryAssets_Test"));
    assert(bad.externalizedAssets.empty());
    assert(bad.actorsT3d.find("Emitters(0)=SpriteEmitter'MyLevel.Sprite0'") != std::string::npos);
    assert(bad.actorsT3d.find("Begin Object Class=SpriteEmitter Name=Sprite0") != std::string::npos);
    assert(bad.actorsT3d.find("Texture=Texture'External.Sprite'") != std::string::npos);
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    assert(Prepare(ReplaceAll(Replace(kRecovered, "MyLevel.Trigger0.Sprite0", "MyLevel.Sprite0"),
                             "SpriteEmitter", "MeshEmitter"), bad, error, "RecoveryAssets_Test"));
    assert(bad.externalizedAssets.empty());
    assert(bad.actorsT3d.find("Emitters(0)=MeshEmitter'MyLevel.Sprite0'") != std::string::npos);
    assert(!Prepare(Replace(Replace(kRecovered, "MyLevel.Trigger0.Sprite0", "MyLevel.Sprite0"),
                           "Emitters(0)=SpriteEmitter", "Emitters(0)=MeshEmitter"),
                    bad, error, "RecoveryAssets_Test"));
    const auto ambiguousEmitter = Replace(Replace(kRecovered, "MyLevel.Trigger0.Sprite0", "MyLevel.Sprite0"),
        "End Map", "Begin Actor Class=Emitter Name=Emitter1\n"
                   "Begin Object Class=SpriteEmitter Name=Sprite0\nEnd Object\nEnd Actor\nEnd Map");
    assert(!Prepare(ambiguousEmitter, bad, error, "RecoveryAssets_Test"));
    assert(error.find("more than one exported owner") != std::string::npos);
    const auto duplicateParticleNames = Replace(kRecovered, "End Map",
        "Begin Actor Class=Emitter Name=Emitter1\n"
        "Begin Object Class=SpriteEmitter Name=Sprite0\nMaxParticles=17\nName=\"Sprite0\"\nEnd Object\n"
        "Emitters(0)=SpriteEmitter'MyLevel.Emitter1.Sprite0'\nEnd Actor\nEnd Map");
    assert(Prepare(duplicateParticleNames, bad, error));
    assert(bad.actorsT3d.find("Begin Object Class=SpriteEmitter Name=Sprite0_Recovery1") != std::string::npos);
    assert(bad.actorsT3d.find("MaxParticles=17\nName=\"Sprite0_Recovery1\"") != std::string::npos);
    assert(bad.actorsT3d.find("Emitters(0)=SpriteEmitter'MyLevel.Sprite0'") != std::string::npos);
    assert(bad.actorsT3d.find("Emitters(0)=SpriteEmitter'MyLevel.Sprite0_Recovery1'") != std::string::npos);
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    assert(!VerifySourceMap(bad, Replace(composed, "MyLevel.Sprite0_Recovery1", "MyLevel.Sprite0"), error));
    const auto particleSettings = Replace(kRecovered, "Owner=42",
        "Owner=42\n        MaxParticles=17\n        SizeScaleRepeats=0.000000\n"
        "        SizeScale(0)=(RelativeTime=1.0,RelativeSize=0.5)");
    assert(Prepare(particleSettings, bad, error));
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    assert(VerifySourceMap(bad, Replace(composed, "SizeScaleRepeats=0.000000", ""), error));
    assert(!VerifySourceMap(bad, Replace(composed, "MaxParticles=17", ""), error));
    assert(error.find("maxparticles") != std::string::npos && error.find("inline particle") != std::string::npos);
    assert(!VerifySourceMap(bad, Replace(composed, "MaxParticles=17", "MaxParticles=18"), error));
    assert(!VerifySourceMap(bad, Replace(composed, "RelativeSize=0.5", "RelativeSize=0.7"), error));
    assert(!VerifySourceMap(bad, Replace(composed, "Texture=Texture'External.Sprite'", ""), error));
    assert(!VerifySourceMap(bad, Replace(composed, "External.Sprite", "Other.Sprite"), error));
    assert(!VerifySourceMap(bad, Replace(composed, "SizeScaleRepeats=0.000000", "SizeScaleRepeats=1.0"), error));
    assert(!VerifySourceMap(bad, Replace(composed, "Begin Object Class=SpriteEmitter Name=Sprite0",
                                                    "Begin Object Class=MeshEmitter Name=Sprite0"), error));
    assert(Prepare(Replace(particleSettings, "SizeScaleRepeats=0.000000", "SizeScaleRepeats=2.0"), bad, error));
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(!VerifySourceMap(bad, Replace(composed, "SizeScaleRepeats=2.0", ""), error));
    const auto particleActorCollision = Replace(ReplaceAll(kRecovered, "Sprite0", "Trigger0"),
        "SpriteEmitter'MyLevel.Trigger0.Trigger0'", "SpriteEmitter'MyLevel.Trigger0'");
    assert(Prepare(particleActorCollision, bad, error));
    assert(bad.actorsT3d.find("Owner=Trigger'MyLevel.Trigger0'") != std::string::npos);
    assert(bad.actorsT3d.find("Instigator=Trigger'MyLevel.Trigger0'") != std::string::npos);
    assert(bad.actorsT3d.find("Name=Trigger0_Recovery1") != std::string::npos);
    assert(bad.actorsT3d.find("Emitters(0)=SpriteEmitter'MyLevel.Trigger0_Recovery1'") != std::string::npos);
    assert(Prepare(ReplaceAll(kRecovered, "Sprite0", "VolumeModel"), bad, error));
    assert(bad.actorsT3d.find("Name=VolumeModel_Recovery1") != std::string::npos);
    assert(bad.actorsT3d.find("Brush=Model'MyLevel.VolumeModel'") != std::string::npos);
    assert(Prepare(ReplaceAll(kRecovered, "Sprite0", "RecoveredModel000001"), bad, error));
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(composed.find("Begin Brush Name=RecoveredModel000001_Recovery1") != std::string::npos);
    assert(composed.find("Emitters(0)=SpriteEmitter'MyLevel.RecoveredModel000001'") != std::string::npos);
    assert(VerifySourceMap(bad, composed, error));

    const auto spacedMesh = Replace(Replace(kRecovered, "External.Mesh", "Oilrig_SM.Third Floor.topcatwalk"),
        "Description=\"LevelInfo'MyLevel.OldLevel' is literal text\"",
        "Description=\"StaticMesh'Oilrig_SM.Third Floor.topcatwalk' is literal text\"");
    assert(Prepare(spacedMesh, bad, error));
    assert(bad.actorsT3d.find("StaticMesh=StaticMesh'\"Oilrig_SM.Third Floor.topcatwalk\"'") != std::string::npos);
    assert(bad.actorsT3d.find("Description=\"StaticMesh'Oilrig_SM.Third Floor.topcatwalk' is literal text\"")
        != std::string::npos);
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    assert(VerifySourceMap(bad, Replace(composed, "'\"Oilrig_SM.Third Floor.topcatwalk\"'",
                                                   "'Oilrig_SM.Third Floor.topcatwalk'"), error));
    assert(!VerifySourceMap(bad, Replace(composed, "'\"Oilrig_SM.Third Floor.topcatwalk\"'",
                                                    "'Oilrig_SM.ThirdFloor.topcatwalk'"), error));
    assert(Prepare(Replace(spacedMesh, "StaticMesh'Oilrig_SM.Third Floor.topcatwalk'",
        "StaticMesh'\"Oilrig_SM.Third Floor.topcatwalk\"'"), bad, error));
    assert(bad.actorsT3d.find("'\"\"Oilrig_SM") == std::string::npos);
    assert(Prepare(Replace(kRecovered, "External.Mesh", "\"MyLevel.Local Group.Mesh\""),
                   bad, error, "RecoveryAssets_Test"));
    assert(bad.externalizedAssets.size() == 1);
    assert(bad.externalizedAssets[0].originalPath == "MyLevel.Local Group.Mesh");
    assert(bad.actorsT3d.find("StaticMesh'\"RecoveryAssets_Test.Local Group.Mesh\"'") != std::string::npos);
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    const auto zoneEffect = Replace(kRecovered, "End Map", "Begin Actor Class=ZoneInfo Name=Zone0\n"
        "ZoneEffect=EFFECT_Hangar'MyLevel.EFFECT_Hangar536'\nEnd Actor\nEnd Map");
    assert(Prepare(zoneEffect, bad, error, "RecoveryAssets_Test"));
    assert(bad.externalizedAssets.size() == 1);
    assert(bad.externalizedAssets.front().className == "EFFECT_Hangar");
    assert(bad.actorsT3d.find("ZoneEffect=EFFECT_Hangar'RecoveryAssets_Test.EFFECT_Hangar536'")
        != std::string::npos);
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    assert(!Prepare(Replace(zoneEffect, "ZoneEffect=EFFECT_Hangar", "ZoneEffect=EFFECT_Unknown"),
                    bad, error, "RecoveryAssets_Test"));
    // Every I3DL2 reverb preset, so a map that sounds like a city or a plain
    // recovers as readily as one that sounds like a hangar.
    for (const auto* effect : {"EFFECT_Generic", "EFFECT_PaddedCell", "EFFECT_Room", "EFFECT_Bathroom",
                              "EFFECT_Livingroom", "EFFECT_StoneRoom", "EFFECT_Auditorium",
                              "EFFECT_ConcertHall", "EFFECT_Cave", "EFFECT_Arena",
                              "EFFECT_CarpetedHallway", "EFFECT_Hallway", "EFFECT_StoneCorridor",
                              "EFFECT_Alley", "EFFECT_Forest", "EFFECT_City", "EFFECT_Mountains",
                              "EFFECT_Quarry", "EFFECT_Plain", "EFFECT_ParkingLot", "EFFECT_SewerPipe",
                              "EFFECT_Underwater", "EFFECT_Drugged", "EFFECT_Dizzy", "EFFECT_Psychotic"})
    {
        assert(Prepare(ReplaceAll(zoneEffect, "EFFECT_Hangar", effect), bad, error, "RecoveryAssets_Test"));
        assert(bad.externalizedAssets.size() == 1);
        assert(bad.externalizedAssets.front().className == effect);
    }
    assert(!Prepare(Replace(zoneEffect, "ZoneEffect=EFFECT_Hangar'MyLevel.EFFECT_Hangar536'",
        "SoftBody=ESBPatch'MyLevel.MyLevel.ESBPatch293'"), bad, error, "RecoveryAssets_Test"));
    assert(error.find("ESBPatch293") != std::string::npos);

    const auto xboxMap = Replace(Replace(kRecovered,
        "Owner=Trigger'MyLevel.Trigger0'", "Owner=Trigger'MyLevel.Xbox0'"),
        "End Map", "Begin Actor Class=Trigger Name=Xbox0\n"
                   "Platform=PLF_XBOX_Only\n"
                   "SoftBody=ESBPatch'MyLevel.MyLevel.UnexportedXboxCloth'\nEnd Actor\nEnd Map");
    assert(Prepare(xboxMap, bad, error));
    assert(bad.actorCount == 2);
    assert(bad.skippedXboxActorCount == 1);
    assert(bad.clearedXboxActorReferenceCount == 1);
    assert(bad.clearedDeletedActorReferenceCount == 0);
    // The recovery report names the left-out actor and the one that lost a reference.
    assert(bad.skippedXboxActorNames == std::vector<std::string>{"Xbox0"});
    assert(bad.clearedReferenceActorNames == std::vector<std::string>{"Volume0"});
    assert(bad.actorsT3d.find("Owner=None") != std::string::npos);
    assert(bad.actorsT3d.find("Xbox0") == std::string::npos);
    assert(bad.actorsT3d.find("Platform=PLF_PC_Only") != std::string::npos);
    assert(bad.actorsT3d.find("Base=BlockingVolume'MyLevel.Volume0'") != std::string::npos);
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    assert(VerifySourceMap(bad, Replace(composed, "Owner=None", ""), error));
    assert(!VerifySourceMap(bad,
        Replace(composed, "Owner=None", "Owner=Trigger'MyLevel.Trigger0'"), error));
    assert(!Prepare(Replace(xboxMap, "Platform=PLF_XBOX_Only", "Platform=PLF_COMMON"), bad, error));
    assert(error.find("UnexportedXboxCloth") != std::string::npos);
    assert(!Prepare(Replace(xboxMap, "Platform=PLF_XBOX_Only", "m_Platform=2"), bad, error));
    assert(!Prepare(Replace(xboxMap, "Owner=Trigger'MyLevel.Xbox0'",
                           "Owner=Trigger'MyLevel.Xbox0.Child'"), bad, error));
    assert(!Prepare(Replace(kRecovered, "Title=\"Recovered test\"",
        "Title=\"Recovered test\"\nPlatform=PLF_XBOX_Only"), bad, error));
    const auto nestedXboxProperty = Replace(kRecovered, "Owner=42", "Owner=42\nPlatform=PLF_XBOX_Only");
    assert(Prepare(nestedXboxProperty, bad, error));
    assert(bad.skippedXboxActorCount == 0);

    // Cooked PC membership wins over stale Xbox labels, for every actor
    // class. Preserve its references and nested effect, and make later
    // ordinary saves keep it in the PC map. Unconfirmed actors stay excluded.
    const auto mislabeledPc = Replace(Replace(xboxMap,
        "SoftBody=ESBPatch'MyLevel.MyLevel.UnexportedXboxCloth'",
        "Begin Object Class=SpriteEmitter Name=Flame0\nMaxParticles=30\n"
        "Platform=PLF_XBOX_Only\nEnd Object\nEmitters(0)=SpriteEmitter'MyLevel.Flame0'"),
        "End Map", "Begin Actor Class=Emitter Name=ConsoleEffect\n"
        "Platform=PLF_XBOX_Only\nEnd Actor\nEnd Map");
    assert(Prepare(mislabeledPc, bad, error, "", {}, {"MyLevel.Xbox0"}));
    assert(bad.correctedPcActorPlatformCount == 1 && bad.skippedXboxActorCount == 1);
    assert(bad.clearedXboxActorReferenceCount == 0);
    assert(bad.actorsT3d.find("Owner=Trigger'MyLevel.Xbox0'") != std::string::npos);
    assert(bad.actorsT3d.find("MaxParticles=30") != std::string::npos);
    assert(bad.actorsT3d.find("ConsoleEffect") == std::string::npos);
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    assert(!VerifySourceMap(bad, Replace(composed, "MaxParticles=30", "MaxParticles=0"), error));
    assert(!Prepare(mislabeledPc, bad, error, "", {}, {"External.Xbox0"}));
    assert(!Prepare(mislabeledPc, bad, error, "", {"MyLevel.Xbox0"}, {"mylevel.xbox0"}));
    // Actual PC content with unsupported data must fail explicitly, rather
    // than be silently lost because of an old label.
    assert(!Prepare(xboxMap, bad, error, "", {}, {"MyLevel.Xbox0"}));
    assert(error.find("UnexportedXboxCloth") != std::string::npos);

    // Asked to keep Xbox-only actors (map.recover keepXboxOnly), every one
    // comes back as common content: its root label goes, its nested
    // components keep theirs, and references to it stay. Nothing is left out.
    assert(Prepare(mislabeledPc, bad, error, "", {}, {}, true));
    assert(bad.keptXboxActorCount == 2 && bad.skippedXboxActorCount == 0 && bad.correctedPcActorPlatformCount == 0);
    assert((bad.keptXboxActorNames == std::vector<std::string>{"Xbox0", "ConsoleEffect"}));
    assert(bad.skippedXboxActorNames.empty() && bad.clearedXboxActorReferenceCount == 0 && bad.clearedReferenceActorNames.empty());
    assert(bad.actorsT3d.find("Owner=Trigger'MyLevel.Xbox0'") != std::string::npos);
    assert(bad.actorsT3d.find("Name=ConsoleEffect") != std::string::npos);
    assert(bad.actorsT3d.find("MaxParticles=30") != std::string::npos);
    {
        // Only Flame0's own label is left.
        std::size_t labels = 0;
        for (auto at = bad.actorsT3d.find("Platform=PLF_XBOX_Only"); at != std::string::npos;
             at = bad.actorsT3d.find("Platform=PLF_XBOX_Only", at + 1)) ++labels;
        assert(labels == 1);
    }
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    // With PC evidence for one, that one is corrected and the other kept.
    assert(Prepare(mislabeledPc, bad, error, "", {}, {"MyLevel.Xbox0"}, true));
    assert(bad.correctedPcActorPlatformCount == 1 && bad.keptXboxActorCount == 1 && bad.skippedXboxActorCount == 0);
    assert((bad.keptXboxActorNames == std::vector<std::string>{"ConsoleEffect"}));
    // Kept content is checked as PC content is: unsupported data still fails,
    // and an Xbox-only LevelInfo still cannot define the map.
    assert(!Prepare(xboxMap, bad, error, "", {}, {}, true));
    assert(error.find("UnexportedXboxCloth") != std::string::npos);
    assert(!Prepare(Replace(kRecovered, "Title=\"Recovered test\"",
        "Title=\"Recovered test\"\nPlatform=PLF_XBOX_Only"), bad, error, "", {}, {}, true));

    const auto deletedMap = Replace(Replace(kRecovered,
        "Owner=Trigger'MyLevel.Trigger0'", "Owner=Trigger'MyLevel.Deleted0'"),
        "Event=\"Trigger event\"", "Event=\"Trigger event\"\n"
        "SpecialBehaviors(0)=(EventFromActor=Trigger'MyLevel.Deleted0',MoveTime=1.0)\n"
        "Description2=\"Trigger'MyLevel.Deleted0' stays literal\"");
    assert(!Prepare(deletedMap, bad, error));
    assert(Prepare(deletedMap, bad, error, "", {"MyLevel.Deleted0"}));
    assert(bad.actorCount == 2);
    assert(bad.skippedXboxActorCount == 0);
    assert(bad.clearedXboxActorReferenceCount == 0);
    assert(bad.clearedDeletedActorReferenceCount == 2);
    assert(bad.skippedXboxActorNames.empty() && !bad.clearedReferenceActorNames.empty()
           && bad.clearedReferenceActorNames.front() == "Volume0");
    assert(bad.actorsT3d.find("Owner=None") != std::string::npos);
    assert(bad.actorsT3d.find("(EventFromActor=None,MoveTime=1.0)") != std::string::npos);
    assert(bad.actorsT3d.find("Description2=\"Trigger'MyLevel.Deleted0' stays literal\"") != std::string::npos);
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    assert(VerifySourceMap(bad, composed, error));
    assert(!VerifySourceMap(bad, Replace(composed,
        "EventFromActor=None", "EventFromActor=Trigger'MyLevel.Trigger0'"), error));
    assert(VerifySourceMap(bad, Replace(composed, "EventFromActor=None,", ""), error));
    assert(!VerifySourceMap(bad, Replace(Replace(composed, "EventFromActor=None,", ""),
                                      "MoveTime=1.0", "MoveTime=2.0"), error));
    const auto nestedNull = Replace(deletedMap,
        "SpecialBehaviors(0)=(EventFromActor=Trigger'MyLevel.Deleted0',MoveTime=1.0)",
        "SpecialBehaviors(0)=(MoveTime=1.0,Conditions=(Other=None,Target=Trigger'MyLevel.Deleted0',Count=2),Last=None)");
    assert(Prepare(nestedNull, bad, error, "", {"MyLevel.Deleted0"}));
    assert(ComposeSourceMap(bad, kFresh, kGeometry, composed, error));
    const auto omittedNestedNull = Replace(composed,
        "(MoveTime=1.0,Conditions=(Other=None,Target=None,Count=2),Last=None)",
        "(MoveTime=1.0,Conditions=(Count=2))");
    assert(VerifySourceMap(bad, omittedNestedNull, error));
    assert(!VerifySourceMap(bad, Replace(omittedNestedNull, "Conditions=(Count=2)", "Conditions=()"), error));
    assert(!VerifySourceMap(bad, Replace(omittedNestedNull,
        "Conditions=(Count=2)", "Conditions=(Count=2,Target=Trigger'MyLevel.Trigger0')"), error));
    assert(!VerifySourceMap(bad, Replace(omittedNestedNull,
        "Conditions=(Count=2)", "Conditions=(Count=2,Target=\"None\")"), error));
    // HELI02 omits complete MoversToLock entries after references to deleted
    // movers become null. Missing live bindings or other fields remain errors.
    for (const auto& value : {"(MoverToLock=None)", "(MoverToLock=None,Other=None)",
                              "(MoverToLock=Trigger'MyLevel.Trigger0')",
                              "(MoverToLock=None,Count=2)", "(MoverToLock=\"None\",Other=None)"})
    {
        const std::string assignment=std::string("MoversToLock(10)=")+value;
        const auto alarm=Replace(kRecovered,"Event=\"Trigger event\"",
            std::string("Event=\"Trigger event\"\n")+assignment);
        assert(Prepare(alarm,bad,error));
        assert(ComposeSourceMap(bad,kFresh,kGeometry,composed,error));
        const bool nullOnly=std::string(value)=="(MoverToLock=None)"
            || std::string(value)=="(MoverToLock=None,Other=None)";
        assert(VerifySourceMap(bad,Replace(composed,assignment,""),error)==nullOnly);
        assert(!VerifySourceMap(bad,Replace(composed,assignment,
            "MoversToLock(10)=(MoverToLock=Trigger'MyLevel.Trigger0',Count=9)"),error));
    }
    assert(!Prepare(deletedMap, bad, error, "", {"MyLevel.OtherDeleted0"}));
    assert(!Prepare(kRecovered, bad, error, "", {"MyLevel.Trigger0"}));
    assert(error.find("present in the exported actor snapshot") != std::string::npos);
    assert(!Prepare(deletedMap, bad, error, "", {"External.Deleted0"}));
    assert(!Prepare(Replace(deletedMap, "Owner=Trigger'MyLevel.Deleted0'",
                           "Owner=Trigger'MyLevel.Deleted0.Child'"),
                    bad, error, "", {"MyLevel.Deleted0"}));
    assert(Prepare(deletedMap, bad, error, "", {"mylevel.deleted0", "MyLevel.Deleted0"}));
    assert(bad.clearedDeletedActorReferenceCount == 2);

    assert(!Prepare(Replace(kRecovered, "    End Object", "    End Brush"), bad, error));
    assert(!Prepare(std::string(kRecovered) + "trailing garbage", bad, error));
    assert(!Prepare(Replace(kRecovered, "End Map\n", ""), bad, error));
    assert(!Prepare(Replace(kRecovered, "Name=Trigger0", "Name=Volume0"), bad, error));
    assert(!Prepare(Replace(kRecovered, "Class=Trigger", "Class=LevelInfo"), bad, error));
    assert(!Prepare("Begin Map\nBegin Actor Class=Trigger Name=T\nEnd Actor\nEnd Map\n",
                    bad, error));

    assert(!ComposeSourceMap(prepared, Replace(kFresh, "Begin Brush Name=FreshBuilderModel",
                                               "Begin Object Name=FreshBuilderModel"),
                             kGeometry, composed, error));
    assert(composed.empty());
    assert(ComposeSourceMap(prepared, kFresh,
                            Replace(kGeometry, "Name=RecoveredBrush000001", "Name=Trigger0"),
                            composed, error));
    assert(composed.find("Class=Brush Name=Trigger0_Recovery1") != std::string::npos);
    assert(composed.find("Class=Trigger Name=Trigger0") != std::string::npos);
    assert(composed.find("Instigator=Trigger'MyLevel.Trigger0'") != std::string::npos);
    assert(VerifySourceMap(prepared, composed, error));

    PreparedMap conflictingModels;
    assert(Prepare(ReplaceAll(kRecovered, "VolumeModel", "RecoveredModel000001"),
                   conflictingModels, error));
    const auto quotedGeometry = Replace(kGeometry, "Name=RecoveredModel000001",
                                                   "Name=\"RecoveredModel000001\"");
    assert(ComposeSourceMap(conflictingModels, kFresh, quotedGeometry, composed, error));
    assert(composed.find("Begin Brush Name=RecoveredModel000001\n") != std::string::npos);
    assert(composed.find("Begin Brush Name=\"RecoveredModel000001_Recovery1\"") != std::string::npos);
    assert(composed.find("Brush=Model'MyLevel.RecoveredModel000001'\n") != std::string::npos);
    assert(composed.find("Brush=Model'MyLevel.RecoveredModel000001_Recovery1'\n") != std::string::npos);
    assert(VerifySourceMap(conflictingModels, composed, error));
    std::string repeatedComposition;
    assert(ComposeSourceMap(conflictingModels, kFresh, quotedGeometry, repeatedComposition, error));
    assert(repeatedComposition == composed);

    const auto occupiedSuffix = Replace(ReplaceAll(kRecovered, "VolumeModel", "RecoveredModel000001"),
        "End Map", "Begin Actor Class=Trigger Name=RecoveredModel000001_Recovery1\nEnd Actor\nEnd Map");
    assert(Prepare(occupiedSuffix, conflictingModels, error));
    assert(ComposeSourceMap(conflictingModels, kFresh, kGeometry, composed, error));
    assert(composed.find("Begin Brush Name=RecoveredModel000001_Recovery2") != std::string::npos);
    assert(VerifySourceMap(conflictingModels, composed, error));

    assert(ComposeSourceMap(prepared, kFresh,
        ReplaceAll(kGeometry, "RecoveredModel000001", "Trigger0"), composed, error));
    assert(composed.find("Begin Brush Name=Trigger0_Recovery1") != std::string::npos);
    assert(composed.find("Brush=Model'MyLevel.Trigger0_Recovery1'") != std::string::npos);
    assert(VerifySourceMap(prepared, composed, error));

    assert(Prepare(ReplaceAll(kRecovered, "VolumeModel", "FreshBuilderModel"),
                   conflictingModels, error));
    assert(ComposeSourceMap(conflictingModels,
        Replace(kFresh, "Name=Builder0", "Name='Trigger0'"), kGeometry, composed, error));
    assert(composed.find("Class=Brush Name='Trigger0_Recovery1'") != std::string::npos);
    assert(composed.find("Begin Brush Name=FreshBuilderModel_Recovery1") != std::string::npos);
    assert(composed.find("Brush=Model'MyLevel.FreshBuilderModel_Recovery1'") != std::string::npos);
    assert(composed.find("Brush=Model'MyLevel.FreshBuilderModel'") != std::string::npos);
    assert(VerifySourceMap(conflictingModels, composed, error));

    assert(Prepare(ReplaceAll(kRecovered, "VolumeModel", "LevelInfo0"), conflictingModels, error));
    assert(!ComposeSourceMap(conflictingModels, kFresh, kGeometry, composed, error));
    assert(error.find("LevelInfo0") != std::string::npos);

    const std::string suffix = std::string(1, static_cast<char>(0xA7)) + "(123)";
    const std::string normalized = Replace(kRecovered,
        "Target=LevelInfo'MyLevel.OldLevel'",
        "Target=LevelInfo" + suffix + "'MyLevel" + suffix + ".OldLevel" + suffix + "'");
    assert(Prepare(normalized, bad, error));
    assert(bad.actorsT3d.find("Target=LevelInfo'MyLevel.OldLevel'") != std::string::npos);
    assert(Prepare(Replace(kRecovered, "Title=\"Recovered test\"",
                            "Title=\"Keep " + suffix + " literally\""), bad, error));
    assert(bad.levelInfoActor.find("Keep " + suffix + " literally") != std::string::npos);

    assert(Prepare("Begin Map\nBegin Actor Class=LevelInfo Name=L\nEnd Actor\nEnd Map\n",
                    bad, error));
    assert(bad.actorCount == 0);
    assert(ComposeSourceMap(bad, kFresh, "Begin Map\nEnd Map\n", composed, error));

    std::cout << "RecoveredActorImport tests passed.\n";
    if (argc == 4 && std::string(argv[1]) == "--verify")
    {
        std::ifstream source(argv[2], std::ios::binary), actual(argv[3], std::ios::binary);
        assert(source && actual);
        const std::string sourceText((std::istreambuf_iterator<char>(source)), {});
        const std::string actualText((std::istreambuf_iterator<char>(actual)), {});
        const bool passed = Prepare(sourceText, bad, error) && VerifySourceMap(bad, actualText, error);
        std::cout << "Full source verification=" << passed << ' ' << error << '\n';
        return passed ? 0 : 1;
    }
    if (argc > 1)
    {
        std::ifstream input(argv[1], std::ios::binary);
        assert(input);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        std::vector<std::string> confirmedDeleted;
        for (int i = 3; i < argc; ++i) confirmedDeleted.emplace_back(argv[i]);
        const bool accepted = Prepare(text, bad, error, argc > 2 ? argv[2] : "", confirmedDeleted);
        std::cout << "Sample accepted=" << accepted << " actors=" << bad.actorCount
                  << " structural brushes=" << bad.removedBrushCount
                  << " skipped Xbox actors=" << bad.skippedXboxActorCount
                  << " cleared Xbox references=" << bad.clearedXboxActorReferenceCount
                  << " cleared deleted references=" << bad.clearedDeletedActorReferenceCount
                  << " unsupported references=" << bad.unsupportedReferences.size()
                  << " externalized assets=" << bad.externalizedAssets.size() << '\n';
        if (!accepted)
            std::cout << error << '\n';
        else if (bad.externalizedAssets.empty() && bad.clearedXboxActorReferenceCount == 0
            && bad.clearedDeletedActorReferenceCount == 0)
            assert(VerifySourceMap(bad, text, error));
    }
}
