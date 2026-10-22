# SCCT Versus wiki digests: index

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki (25 pages: Home plus the 24 linked from its table of contents and the Map Packaging page)

These digests are curated, faithful copies of the SCCT Versus Reloaded Editor wiki, which is the source of truth for how a Versus map must be built.
- Class, property and enum names are verbatim, including the wiki's spellings (`TriggerMethode`, `strenght`, `bInitialyOn`).
- Every rule, number and gotcha is kept.
- Values the wiki shows only in screenshots are marked "image-only".

## Pages

| Slug (`scct://wiki/<slug>`) | What it covers |
|---|---|
| `home` | About the wiki; conventions used by these digests |
| `spawn-points` | `PlayerStart` (`TeamNumber` 0 Spy / 1 Merc, ≥3 per team), `STriggerPlayerStart` |
| `game-modes` | Story (`SMission` `GM_Multi`, `SObjective`, `SComputerObjectiveTrigger` at ~78, `SFlag` + 1-5 `SFlagDropZone`, `SBombTargetObjectiveTrigger`), sector maps (`GM_Undefined`, `bChained`), Disk Hunt (`SDisk`, `Group` 1/2/3), Deathmatch (`GM_DeathMatch`) |
| `map-settings` | `SMapSettings`: `MapName` CAPS, briefs, `Modes`/`ModesStrings` index-aligned, ranges, `<Map>-i` textures, save/exit/reopen |
| `level-properties` | LevelInfo (`F6`): `Song`, `fDeathPawnHeight`, `Event`/`SendAtTime`, `SnapshotCamera`/`MapFloors` (the Strategic Map), `MTVOffset`/`MTVSlope` 52/85 or 8/8; the page's headings (`Events`, `MapSnapshotInfo`, `MotionTrackingTweak`, ...) are property-sheet categories |
| `vision-mode-properties` | `bSeeThroughInMotionVision`, `bHideInNormalVision`, EchelonHeat, EchelonEMF |
| `geometric-events` | GE Mode, ledge 208/320/448/512, obstacle 115, pull-up 64, length 32, fence 88-128 (96), hand-over-hand 216, ladder bars even ≥8, zipline 384/198/161, `SGETrigger`, `GEActivationMask` |
| `events` | `Event`/`Tag`, `TriggerMethode` (`TriggerToggle`, `TriggerControl`, `UntriggerOnly`), `SMagicEvent`, `SMessageTrigger`, `SShaker`, `SNotifyNoise` |
| `alarms` | `SAlarm` (`MessagePriority` 0-5, `MoversToLock`, `ObjectiveTriggersToLock`), `SLaserTrigger`, `SCamera`, `SVolumetricSensor` + `Volume` |
| `movers` | Movers, keyframes, `MoverEncroachType`, `InitialState`, sounds and events, `SPresenceTrigger`, Special Behaviors |
| `elevators` | `SLift`, `DoorsAndSwitches` (door `Key 0` open / `Key 1` closed), `SLiftDoor` |
| `lighting` | Static, dynamic and Echelon (`LE_ESpotShadow`) lights; `LightEffect` `LE_None`/`LE_Spotlight`/`LE_Sunlight` only; coronas; lightmaps; light operations |
| `material-classes` | Echelon material flags (`bClimbable`, ...), `LitSpecularMaterial`, heat/EMF modifiers, `EchelonLightingMaterial`, material operations |
| `static-meshes` | ASE import/export, Convert to Static Mesh, collision options |
| `sound-actors` | `SoundTrigger`, `MusicTrigger`, `SAmbientSoundTrigger` |
| `sound-browser` | Sound flags (`Stream`), Random/Sequence/Switch resources, `.uas` packages (`Interface` always, quality 5.0) |
| `gameplay-objects` | `SGamePlayObject` (never `bPickable`), `SGPODamages`, `SGPOSound`, `STriggerLight` |
| `buttons-and-hacking-panels` | `SInterrupteur`: 120/60 interaction, `UseRelativeLocation` 55,0,0 or 39.5,3,0, `UseRelativeRotation` 32768, `UsableBy` |
| `level-ingredients` | `SAmmoStock` (60/120), `SInfirmary` (collision, 120/60), **`SCamNetwork` (required; loop; one `bFirstCam`)**, `SVitre`, `SExtincteur`, `SProjoFixe`, `SSprinkler`, `SFlaque`, `SBallTrap` |
| `zones-and-volumes` | `ZoneInfo` `LocationName`, `Volume`, `SDamageVolume`, `ERainVolume`, `SAlarmZoneInfo` (avoid) |
| `softbody-actors` | `ESoftBodyActor` (never duplicate), `ESBPatchActor`, `ESBStripeDoorActor` (the engine's `ESBStripDoorActor`), `ESBRopeActor`, `ESBChainActor`, all in the `SoftBody` package |
| `animated-meshes` | `SAnimatedMesh` (reuse existing animations only) |
| `fish` | `SFish`, `SFish_Big`, `SFish_Follow` (needs a `bPilote=True` `Pilot`) |
| `map-tutorials` | `SHelpPath`, `SZoneTips`, `SEndZoneTip` (Visit a Map) |
| `map-packaging` | File name (4 letters + `D`), unique package base names, release file list, keep the MapsEd file |

## Minimum viable Versus map checklist

A map that loads and plays in all three modes without softlocks. Each item names its page.

1. **Geometry**
   - Carved, connected spaces. Spies must reach every objective from their starts.
   - Built: `map_build`.
2. **Spawns** (`spawn-points`)
   - At least **3** `PlayerStart`s with `TeamNumber 0` (Spy) and **3** with `TeamNumber 1` (Merc). RE+ places 4 each.
   - Not inside geometry.
   - Spy starts more than 512 uu from objectives, and the teams more than 768 uu apart.
3. **Story** (`game-modes`)
   - An `SMission` with `GameMode GM_Multi`, `Description`/`DescriptionDEF`, a `MinimumObjectives` the map can reach, and `Objectives` naming each `SObjective` exactly.
   - Each `SObjective` lists its triggers in `Triggers`.
   - Each terminal is an `SComputerObjectiveTrigger`: ~78 uu above the floor, clear of collision, with `DominationCaptureTime` set and `TriggerName` filled.
   - Any `SFlag` has 1-5 `SFlagDropZone`s.
   - Any `SBombTargetObjectiveTrigger` uses `CaptureTime`.
4. **Disk Hunt** (`game-modes`, `map-settings`)
   - An `SMission` (`GM_DiskHunt`) whose `Triggers` lists every `SDisk`.
   - Disks clear of geometry, with `Group` 1-3.
   - At least as many disks as `ScoreLimit`.
5. **Deathmatch** (`game-modes`)
   - An `SMission` with `GM_DeathMatch`.
   - Its `AtResetEvent` untriggers the security (SMagicEvent) when the map has security.
6. **Camera Network** (`level-ingredients`)
   - **At least one `SCamNetwork`**, or Mercenaries using Cam. Net. are softlocked.
   - Exactly one `bFirstCam True`.
   - `NextCam`/`PrevCam` forming one closed loop.
   - `bBlockActors`/`bBlockPlayers True`.
7. **Security** (`alarms`) when present
   - Each detector's `Event` equals an `SAlarm` `Tag`.
   - `MessagePriority` 0-5: 5 when it locks things, 2-4 when it locks nothing.
   - Bomb targets are never in `ObjectiveTriggersToLock`.
   - Motion sensors have `Volumes`.
8. **Ingredients** (`level-ingredients`)
   - `SAmmoStock` for Mercenaries (60/120).
   - `SInfirmary` with collision on (120/60).
9. **Lights** (`lighting`)
   - Static lights use only `LE_None`, `LE_Spotlight` or `LE_Sunlight` (sun needs a Fake Backdrop).
   - Echelon lights use `LE_ESpotShadow` with `EchelonNormalInfluence` ≈0.9.
   - Lighting rebuilt.
10. **Zones** (`zones-and-volumes`)
    - A `ZoneInfo` with `LocationName` in each major area, **including every spawn area**.
    - No `SAlarmZoneInfo`.
    - No zone leaks in Map Check.
11. **File** (`map-packaging`)
    - Saved as `Packages\MapsEd\<4 letters>D.sdc` (ASCII, no spaces).
    - Then save → exit → reopen (`editor_restart`) before Map Settings.
12. **Map Settings** (`map-settings`)
    - `MapName` in CAPS.
    - `ExpliMapInBriefSpy`/`ExpliMapInBriefSentry`, and `LegendMap`/`LegendMapInBrief` with `|` line breaks.
    - One `Modes[i]` + `ModesStrings[i]` pair per mode, index-aligned.
    - `RespawnTime` 5-15, lives 0-10, `TimeLimit` 0-20.
13. **Interface textures** (`map-settings`)
    - In `<Map>-i`: `Menu` and `Briefing` (256x256 DXT5, `bAlphaTexture=True`), `LoadingATT0` and `LoadingDEF0` (512x512 DXT1).
    - Then `Packages\Textures\<Map>-i.utc` (delete any `-i.utx`).
14. **Sound** (`sound-browser`)
    - `Sounds\<MapFile>.uas` built with `Interface` (plus `Amb_<Map>`), quality 5.0. This is needed even with no custom audio.
15. **Release** (`map-packaging`)
    - Unique package base names (`<Map>_TXT`, `<Map>_STM`, `Amb_<Map>`); no stock package modified.
    - The archive holds the Maps and MapsEd `.sdc`, `-i.utc`, `.uas` and the new packages.

## Key additions → tool

| You want to add | Wiki page | MCP tool |
|---|---|---|
| Anything / direct property edits | (any) | `actors_edit`, `map_changes_apply` |
