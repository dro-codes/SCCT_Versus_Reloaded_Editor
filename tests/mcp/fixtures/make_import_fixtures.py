"""Writes the importer fixtures that hold computed data (tests/ImportersTests.cpp).

Run from the repository root:
    python3 -I tests/mcp/fixtures/make_import_fixtures.py

plan.tmx carries a base64 + zlib layer; room.t3d and the map-authoring
export carry brushes whose faces wind as the editor's box does (corner bits:
1 = +X, 2 = +Y, 4 = +Z; faces 1375 0462 2673 0154 4576 0231), so
(v1 - v0) x (v2 - v0) points out of the brush.
"""
import base64
import json
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
FACES = [(1, 3, 7, 5), (0, 4, 6, 2), (2, 6, 7, 3), (0, 1, 5, 4), (4, 5, 7, 6), (0, 2, 3, 1)]


def box(lo, hi, indent="          ", texture=None):
    lines = []
    for face in FACES:
        head = indent + "Begin Polygon" + (f" Texture={texture} Flags=0 Link=0" if texture else "")
        lines.append(head)
        for c in face:
            v = [hi[k] if c >> k & 1 else lo[k] for k in range(3)]
            lines.append(indent + "   Vertex   " + ",".join(f"{x:+013.6f}" for x in v))
        lines.append(indent + "End Polygon")
    return "\n".join(lines)


def tmx():
    ground = [2] * 8 + ([2] + [1] * 6 + [2]) * 4 + [2] * 8
    markers = [0] * 48
    for i in (9, 17, 25):
        markers[i] = 3
    for i in (14, 22, 30):
        markers[i] = 4 | 0x80000000  # a flipped tile still means merc
    csv = ",\n".join(",".join(str(v) for v in ground[r * 8:(r + 1) * 8]) for r in range(6))
    packed = base64.b64encode(zlib.compress(b"".join(struct.pack("<I", v) for v in markers))).decode()
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<map version="1.10" tiledversion="1.10.2" orientation="orthogonal" renderorder="right-down" width="8" height="6" tilewidth="32" tileheight="32" infinite="0" nextlayerid="4" nextobjectid="5">
 <tileset firstgid="1" name="plan" tilewidth="32" tileheight="32" tilecount="4" columns="4">
  <image source="plan.png" width="128" height="32"/>
  <tile id="0" type="floor"/>
  <tile id="1"><properties><property name="kind" value="wall"/></properties></tile>
  <tile id="2" class="spy"/>
  <tile id="3" class="merc"/>
 </tileset>
 <group id="3" name="Storey 1">
  <layer id="1" name="Ground" width="8" height="6">
   <data encoding="csv">
{csv}
</data>
  </layer>
  <layer id="2" name="Starts" width="8" height="6">
   <data encoding="base64" compression="zlib">
   {packed}
   </data>
  </layer>
  <objectgroup id="4" name="Objects">
   <object id="1" name="Server" type="terminal" x="112" y="80"><point/></object>
   <object id="2" name="Net" type="camnet" x="176" y="48"><point/></object>
   <object id="3" name="Ledge" type="ledge" x="64" y="40"><polyline points="0,0 96,0"/></object>
   <object id="4" name="Store" type="room" x="224" y="32" width="64" height="96"/>
  </objectgroup>
 </group>
</map>
'''


def t3d():
    return f'''Begin Map
Begin Actor Class=LevelInfo Name=LevelInfo0
    TimeSeconds=0.000000
End Actor
Begin Actor Class=Brush Name=Brush0
    MainScale=(SheerAxis=SHEER_ZX)
    PostScale=(SheerAxis=SHEER_ZX)
    Level=LevelInfo'MyLevel.LevelInfo0'
    Tag=Brush
    Location=(X=0.000000,Y=0.000000,Z=0.000000)
    Begin Brush Name=Brush
       Begin PolyList
{box((-128, -128, -128), (128, 128, 128))}
       End PolyList
    End Brush
    Brush=Model'MyLevel.Brush'
    Name=Brush0
End Actor
Begin Actor Class=Brush Name=Brush1
    CsgOper=CSG_Subtract
    MainScale=(Scale=(X=2.000000,Y=1.000000,Z=1.000000),SheerAxis=SHEER_ZX)
    PostScale=(SheerAxis=SHEER_ZX)
    PrePivot=(X=0.000000,Y=0.000000,Z=-128.000000)
    Location=(X=512.000000,Y=0.000000,Z=128.000000)
    Rotation=(Yaw=16384)
    Begin Brush Name=Model1
       Begin PolyList
{box((-128, -128, -128), (128, 128, 128), texture="Bunker.Walls.Concrete")}
       End PolyList
    End Brush
    Brush=Model'MyLevel.Model1'
    Name=Brush1
End Actor
Begin Actor Class=Brush Name=Brush2
    CsgOper=CSG_Add
    PolyFlags=32
    MainScale=(Scale=(X=-1.000000,Y=1.000000,Z=1.000000),SheerAxis=SHEER_ZX)
    Location=(X=512.000000,Y=0.000000,Z=0.000000)
    Begin Brush Name=Model2
       Begin PolyList
{box((-32, -32, 0), (32, 32, 64))}
       End PolyList
    End Brush
End Actor
Begin Actor Class=PlayerStart Name=PlayerStart0
    TeamNumber=1
    Location=(X=100.000000,Y=50.000000,Z=48.000000)
    Rotation=(Yaw=32768)
End Actor
Begin Actor Class=PlayerStart Name=PlayerStart1
    Location=(X=-100.000000,Y=50.000000,Z=48.000000)
End Actor
Begin Actor Class=Light Name=Light3
    LightEffect=LE_Spotlight
    LightBrightness=180.000000
    Location=(X=0.000000,Y=0.000000,Z=200.000000)
    Rotation=(Pitch=-16384,Yaw=16384)
End Actor
Begin Actor Class=StaticMeshActor Name=StaticMeshActor7
    StaticMesh=StaticMesh'Crates.Wood.Crate01'
    DrawScale3D=(X=2.000000,Y=2.000000,Z=1.000000)
    Location=(X=-64.000000,Y=64.000000,Z=0.000000)
    Rotation=(Yaw=8192)
    Tag=Crate
End Actor
Begin Actor Class=BlockingVolume Name=BlockingVolume0
    Location=(X=256.000000,Y=0.000000,Z=0.000000)
    Begin Brush Name=Model3
       Begin PolyList
{box((0, 0, 0), (64, 64, 32))}
       End PolyList
    End Brush
End Actor
Begin Actor Class=SBase.SMagicEvent Name=SMagicEvent0
    Tag=Fireworks
    Groups(0)=(EventGroup=((Event=Boom,Delay=1.000000)),Sequence=False)
    Groups(1)=(EventGroup=((Event=Bang,Delay=2.000000)))
    Location=(X=0.000000,Y=0.000000,Z=0.000000)
End Actor
Begin Actor Class=Mover Name=Mover0
    Location=(X=0.000000,Y=0.000000,Z=0.000000)
End Actor
Begin Actor Class=Camera Name=Camera0
End Actor
End Map
'''


def authoring():
    actors = [
        ("PlayerStart0", "Engine.PlayerStart", {"TeamNumber": "0", "Tag": "PlayerStart"}, [0, 0, 48], [0, 16384, 0]),
        ("PlayerStart1", "Engine.PlayerStart", {"TeamNumber": "1", "Tag": "MercSpawn"}, [512, 0, 48], [0, 32768, 0]),
        ("SComputerObjectiveTrigger0", "SBase.SComputerObjectiveTrigger", {"Tag": "Server", "TriggerName": "\"Main server\"", "DominationCaptureTime": "12.000000"}, [256, 256, 78], [0, 0, 0]),
        ("SFlag0", "SBase.SFlag", {"Tag": "SFlag", "ResetTime": "30.000000"}, [0, 512, 64], [0, 0, 0]),
        ("SFlagDropZone0", "SBase.SFlagDropZone", {"Tag": "SFlagDropZone"}, [100, 600, 64], [0, 0, 0]),
        ("SCamNetwork0", "SBase.SCamNetwork", {"Tag": "SCamNetwork", "bFirstCam": "True"}, [0, 0, 200], [0, 49152, 0]),
        ("Light0", "Engine.Light", {"Tag": "Light", "LightBrightness": "64.000000", "LightEffect": "LE_None"}, [0, 0, 300], [0, 0, 0]),
        ("StaticMeshActor0", "Engine.StaticMeshActor", {"Tag": "StaticMeshActor", "StaticMesh": "StaticMesh'Crates.Wood.Crate01'"}, [50, 50, 0], [0, 0, 0]),
        ("SMission0", "SBase.SMission", {"Tag": "SMission"}, [0, 0, 0], [0, 0, 0]),
        ("Emitter3", "SBase.SSpawnableEmitter", {"Tag": "Steam", "Emitters": ["SpriteEmitter'MyLevel.SpriteEmitter0'"]}, [10, 10, 10], [0, 0, 0]),
    ]
    out = []
    for name, cls, values, position, rotation in actors:
        out.append({"actor": {"path": "MyLevel." + name, "class": cls}, "values": values, "schema": {}, "unavailable": {},
                    "selected": False, "position": position, "rotation": rotation})
    out.append({"actor": {"path": "MyLevel.SpriteEmitter0", "class": "Engine.SpriteEmitter"}, "values": {}, "schema": {}, "unavailable": {}})
    geometry = f'''Begin Map
Begin Actor Class=Brush Name=Brush5
    CsgOper=CSG_Subtract
    Location=(X=0.000000,Y=0.000000,Z=0.000000)
    Begin Brush Name=Model5
       Begin PolyList
{box((-512, -512, 0), (512, 768, 320))}
       End PolyList
    End Brush
End Actor
End Map
'''
    return {"format": "scct.map-authoring", "version": 1, "map": "c:\\game\\packages\\mapsed\\testd.sdc", "level": "MyLevel",
            "actors": out, "classes": [], "assets": {"Sound": [], "Material": [], "StaticMesh": []}, "geometryT3d": geometry,
            "changes": {"format": "scct.map-changes", "version": 3, "map": "c:\\game\\packages\\mapsed\\testd.sdc", "description": "", "operations": []}}


def main():
    (HERE / "plan.tmx").write_text(tmx())
    (HERE / "room.t3d").write_text(t3d())
    (HERE / "authoring-export.json").write_text(json.dumps(authoring(), indent=1))


if __name__ == "__main__":
    main()
