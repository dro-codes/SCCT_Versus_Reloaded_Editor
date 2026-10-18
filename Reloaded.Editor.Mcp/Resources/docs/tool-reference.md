# Tool reference: fields and wiki defaults

The tool descriptions in `tools/list` give each argument's shape. This page gives the rest: what every field does, the wiki defaults the level-spec compiler applies, and what the checks look for. The conventions every spec tool shares (units, ids, positions, how intent tools merge) are in `scct://docs/tools`; every spec field is in `scct://schema/level-spec`; every rule is in `scct://rules`.

## level_build

The compiler turns a `scct.level-spec` into one `mcp.plan` that the editor runs as ONE Undo step: blockout pieces as Map Design pieces, every actor through one Map JSON document, objective player counts, GEs, Level Properties, the Map Design records and the stored spec. It then builds the geometry, runs Map Check and takes a top-view screenshot. If a step inside that Undo step fails, every map change of the plan is rolled back; a failure after it (the build, Map Check, a save, or a dialog after a file was written) keeps the map changes and says so with `rolledBack: false`.

- `spec` or `path`: as `level_spec_validate` (a `.level.json` on this machine, or `scct://examples/<name>`).
- `mode`: `preview` (the default; `dryRun: true` too) asks the editor for its preview rows (what each create, update and delete would do) and returns the plan PNG, changing nothing. Without an editor it is the offline compile. `apply` builds it; errors in the spec block apply unless `force: true`. When a preview has errors or findings about other parts of the spec, the spec it built is saved in the work folder (`specFile`) and `next` points `level_spec_validate` at it.
- `sync`: the map stores the spec it was built from, with the ids' actors and brushes (the sidecar; `map_query what:"spec"`).
  - `incremental` (the default when there is one) builds only the difference by id: new ids are created, changed pieces rebuilt, changed actors updated (or recreated when their class, team or Tag changes), ids the spec dropped deleted, the rest left alone.
  - `replace` (the default otherwise) deletes everything the last build made and builds the spec afresh.
  - `merge` lays the given spec over the stored one first, so a fragment adds to the level.
- `brushMode`: `pieces` (the default; Map Design pieces the panel can keep editing) or `polygons` (plain polygon brushes, for per-face textures).
- `build`: `geometry` (the default: the Map Design geometry build, then Map Check), `all` (the full build with lighting; minutes), `none` (no build, no Map Check).
- `screenshot` (default true): the top view after the build, returned as an image. `save`: `map.save` to this path after the build (`Packages\MapsEd`, ASCII, a stem without spaces: the first four letters of the map name plus D, such as `AquaD.sdc`; `scct://wiki/map-packaging`).
- With Require approval on, the editor shows the change for the user to approve; `approval_wait` follows a long wait.

It returns `created`, `changed` and `removed` by spec id (with the actor path the editor gave each), `validation` (the compiler's and the rules' findings with JSON pointers and wiki pages), `mapCheck`, `mapSettings` (SMapSettings cannot always be written over the bridge: when not, its checklist and manual steps), the step results, `undo` (`editor_undo` with that call reverts the whole build) and `next`: a new map is saved (`map_file save_as`), the editor restarted (`editor_restart`) and only then given Map Settings (`map_settings`), as `scct://wiki/map-settings` requires.

The wiki defaults the compiler applies:
- PlayerStart `TeamNumber` 0 spy / 1 merc, 4 per team at floor+48 (spawn-points).
- SMission `GM_Multi` with objectives and triggers by exact name; terminals at floor+78 using `DominationCaptureTime`; SFlag with 1-5 drop zones; sector missions chained (game-modes).
- SDisk groups cycling 1/2/3; deathmatch `AtResetEvent` switching security off.
- Detectors' `Event` set to their SAlarm's `Tag` (alarms); a closed CamNet loop with one `bFirstCam` (level-ingredients).
- Doors `TriggerOpenTimed` with `ME_WaitAndRetryWhenEncroach` (movers); SInterrupteur interaction 120/60 (buttons-and-hacking-panels).
- Static lights `LE_None`, `LE_Spotlight` or `LE_Sunlight` only (lighting).
- A ZoneInfo with `LocationName` per zone and portals where zones meet (zones-and-volumes).
- `GE ADD` for each geometric event, pipes and ladders low end first (geometric-events).

## blockout

Lays out the level's spaces as Map Design pieces, the RE+ blockout the Map Design panel can keep editing (`scct://docs/map-design`): rooms, corridors, vents, crawlways, doorways, stairs, ramps and platforms, plus polygon brushes, per-face textures and static meshes.

- `pieces [{id, kind: Room|Corridor|Vent|Crawlway|Doorway|Platform|Stairs|Stairs L|Stairs U|Spiral|Ramp, size: [width (local X), length (local Y), height], at: [x, y] (the centre; z from storey), storey, yaw (multiples of 90 keep walls on the compass), name, material (Package.Group.Name), construction, thickness, ceiling, portal, attach: {to, side: north|south|east|west, offset} (flush against another piece's wall, running away from it), steps}]`. The world is solid: rooms are carved out of it (construction Carve, walls `defaults.thickness`, 16), corridors cut through the walls they join, vents are crouch height (125) and crawlways 105. Additive stairs, ramps and platforms belong inside carved space.
- `doorways [{id, between: [a, b] | host + side + offset, size: [width, height], sill, depth, portal, name}]`: openings cut from a wall's inside face; `portal` puts a zone portal in it (default true, or only where two zones meet when zones are given). 96 x 128 fits a standing player (180 tall).
- `storeys [{id, z, name}]` and `vertical [{kind: stairs|lift, from: storeyId, to, at: {in, u, v}, shape, width}]` join floors.
- `brushes [{id, csg: CSG_Add|CSG_Subtract, polyFlags, class, at, polygons: [{vertices: [[x, y, z]...], texture}]}]`: polygon brushes.
- `surfaces [{brush, faces: [floor|ceiling|+X|-X|+Y|-Y], texture}]` texture a piece's faces after the build. A face is named by where its surface normal points, so a carved room's east wall is `-X`; both brush modes texture the same wall.
- `meshes [{id, mesh: StaticMesh'Pkg.Group.Name', at, yaw, scale, fit: floor}]` place StaticMeshActors.
- `defaults {construction, thickness, material, lights: rooms (a ceiling light in each room) | none, startsPerTeam}`.

The Map Design checks run on the result (`validation`): steps at most 35 high, slopes at most 45 degrees, standing clearance 96 x 180, crouch 125, rooms reachable through doorways. The geometry is rebuilt afterwards. Spawns, objectives and the rest go in through gameplay, security, mechanisms, events and environment, which place things `{in: pieceId}`.

## gameplay

Who spawns where and what each mode plays (`scct://wiki/spawn-points`, `scct://wiki/game-modes`).

- `spawns {spy, merc: {in: roomId | [roomIds], count (default defaults.startsPerTeam, 4), facing: {toward: id | position}, spacing} | [{id, at, yaw | facing}], toggles: [{id, team: spy|merc, in | at, enabledBy (an event Tag)}]}`: PlayerStarts with `TeamNumber` 0 (spy) and 1 (merc) on a 128-unit lattice at floor+48; the wiki wants at least 3 per team (an error in release). `toggles` are STriggerPlayerStarts whose Tag is the toggling event.
- `modes [story, diskhunt, deathmatch]` add to the modes the level plays (giving a mode's section adds it too); `removeModes` drops modes together with their sections.
- `story {mission, objectives, sectors}`:
  - `mission {id, name, description (the Spy text), descriptionDef (the Merc text), minimumObjectives, chained, atResetEvent}`: an SMission with GameMode `GM_Multi` listing its SObjectives by exact name.
  - `objectives [{id, name, description, descriptionDef, visibilityZone (a zone id), at, triggers [...]}]`: each SObjective lists its Triggers.
  - `triggers [{id, kind: terminal|flag|bomb, at, triggerName, ...}]`: a terminal is an SComputerObjectiveTrigger at floor+78 using `dominationCaptureTime` (CaptureTime on a terminal warns); a flag is an SFlag with `resetTime` and 1-5 `dropZones`; a bomb target an SBombTargetObjectiveTrigger with `captureTime`.
  - `dropZones [{id, at} | {ref: id}]`: `{id, at}` makes an SFlagDropZone; `{ref}` names one another flag's entry made, so flags share drop zones as the wiki allows.
  - `sectors [{id, name, brief, briefDefend, objectives [ids], required, actions [{kind: open|light|sound|announce|trigger, target, delay (0 to 86400 seconds), ...}]}]`: sectors chain under a `GM_Multi` mission with `bChained True`, each sector mission `GM_Undefined` with `bChained False`, with `Zone<N>_Complete` SMagicEvents and `Zone<N>_Announce` SAlarms. An `open` action on a door that has its own presence or button trigger is an error with `hold`: give the door `trigger {kind: "event"}` or the action `hold: false`.
- `diskhunt {disks: {in: [roomIds], count} | [{id, at, group}], resetTime, mission}`: an SMission `GM_DiskHunt` listing every SDisk, `Group` cycling 1/2/3, disks kept clear of geometry; the Map Settings ScoreLimit (`map_settings`) must not exceed the disks placed.
- `deathmatch {disableSecurity, mission}`: an SMission `GM_DeathMatch` whose `AtResetEvent` fires an SMagicEvent that switches the security off.
- `playerCounts [{objective, min, max, open}]` (matched by objective) and `targets {mission, sizes: [8 counts, 1v1 to 8v8]}`: objective player limits.

Placement is checked too: spy starts not within 512 units of an objective, the teams not within 768 of each other, every objective within 4096 of a merc start, objectives reachable from the spawns.

## security

Alarms, detectors and the camera network (`scct://wiki/alarms`, `scct://wiki/level-ingredients`).

- `alarms [{id, name, description (the Merc HUD text), descriptionSpy, duration, priority 0-5 (5 when it locks something, 2-4 otherwise), outputs [ids or Tags it triggers: doors, lights, events], lockMovers [{mover: a door id, action}], lockObjectives [objective trigger ids], at}]`: SAlarms. `outputs` fill Events, `lockMovers` the complete MoversToLock structs (ActionBeforeLock default `ActionBeforeLock_None`), `lockObjectives` ObjectiveTriggersToLock; a bomb target (SBombTargetObjectiveTrigger) is never locked.
- `devices [{id, kind: laser|camera|motion|presence|mine, at | in | box, facing, alarm (an alarm id), detected: spy|merc|both, length, radius, height, coneAngle, maxDistance, patrolAngle, rotationVelocity, detectionDelay, hideLaser, spawnPercent, props}]`: each detector's Event is its alarm's Tag; `detected` sets DetectedClass (SPawnAttaque, SPawnDefense, SBasePawn). A motion sensor is an SVolumetricSensor plus its detection Volume, a polygon brush sized to `box` (`{min, max}` or `{in: roomId, height}`). Lasers and presence detectors sit 48 units above the floor, cameras at 200.
- `camnet [{id, camName, at, yaw | facing, first, constraints {pitch: [min, max], yaw: [min, max]}}]`: SCamNetwork cameras wired NextCam/PrevCam as one closed loop in list order (new cameras join the end of the loop), exactly one `bFirstCam` (`first`, else the first listed), `bBlockActors` and `bBlockPlayers` True. A Versus map needs at least one, or the Mercs softlock.

Checked: detectors wired to an alarm, alarms fed and doing something, motion sensors with a volume, MessagePriority 0-5, no SAlarmZoneInfo (RE+ never makes one), the CamNet loop. Doors to lock come from mechanisms.

## mechanisms

Doors, movers, elevators, buttons and hacking panels (`scct://wiki/movers`, `scct://wiki/elevators`, `scct://wiki/buttons-and-hacking-panels`).

- `doors [{id, mesh: StaticMesh'Pkg.Group.Name', at: {doorway: id} | position, yaw, open: {offset: [x, y, z]} | {yaw: degrees}, moveTime, stayOpen, trigger: {kind: button|panel|presence|event, hackTime, usableBy: spy|merc|both, at, radius, text}}]`: static-mesh Movers, `Key 0` closed and `Key 1` open (KeyPos/KeyRot), NumKeys 2, InitialState `TriggerOpenTimed`, MoverEncroachType `ME_WaitAndRetryWhenEncroach`, left at Key 0. A presence trigger is an SPresenceTrigger whose Event is the door's Tag; button and panel make an SInterrupteur; event leaves it to a Tag.
- `movers [{id, class, mesh, at, yaw, keys [{offset, yaw, pitch, roll}], moveTime, stayOpen, initialState, encroach}]`: other static-mesh movers (brush movers are not supported).
- `lifts [{id, shaft: pieceId, storeys: [storeyIds], start, mesh, moveTime, doorMesh, carDoorMesh, doorOpen, doorMoveTime, doors, switches, carSwitch}]` (`scct://wiki/elevators` has the whole of it):
  - With a `mesh`, an SLift with one key per storey (the `start` storey first, `KeyNum` 0); `MoveTime` defaults to about rise / 160, 1.5-8 s. Without a mesh, a two-storey lift is the Map Design brush platform (192 x 192), which has no doors.
  - A storey's opening onto the shaft is the spec doorway joining the shaft whose sill is within 128 units above that storey's floor, or its `doors [{storey, doorway | at, yaw, mesh}]` entry.
  - With `doorMesh`, each opening gets an external door: an `Engine.Mover` with `Key 0` = open and `Key 1` = closed, `KeyNum` 0 on the start storey and 1 on the others, InitialState `TriggerControl`. `doorOpen` is how it opens from closed (`{offset}` or `{yaw}`; by default it rises by its height), `doorMoveTime` defaults to 1 s. The car gets an SLiftDoor (mesh `carDoorMesh`, default `doorMesh`), listed in the lift's `LiftDoors`. Without `doorMesh` the lift has no doors (warning `elevator.doors`).
  - Unless `switches: false`, a TriggerToggle call switch beside each storey's opening and, unless `carSwitch: false`, one in the car that rides with it. `DoorsAndSwitches` has one entry per storey: that storey's door, the car switch and the other storeys' call switches.
  - Never model lift doors with `doors`: those are TriggerOpenTimed with their own presence trigger, open whenever someone comes near, even with the car on another floor, and the SLift does not control them.
- `buttons [{id, kind: button|panel, at, yaw | facing, targets: [ids or Tags], hackTime, usableBy, text, mesh}]`: an SInterrupteur with fInteractionHeight 120, fInteractionRadius 60, bInteractInFront, UseRelativeLocation 55,0,0 (button) or 39.5,3,0 (panel) and UseRelativeRotation yaw 32768; its Event is the target's Tag, several targets fan out through an SMagicEvent; `usableBy` spy U_SPAWNOFFENSE, merc U_SPAWNDEFENSE, both U_BOTH (a merc panel with a hack time warns: the wiki calls it unsupported).

Alarms lock doors with security `alarms[].lockMovers`.

## events

Wiring between things, and timed and scripted events (`scct://wiki/events`, `scct://wiki/level-properties`).

- `links [{from, to, delay, untrigger}]` (matched by from and to): from's Event becomes to's Tag; a delay (0 to 86400 seconds, as an SMagicEvent takes it) goes through an SMagicEvent action.
- `magic [{id, document, at}]`: SMagicEvents from `scct.smagic-event` documents (`scct://schema/smagic-event`, the SMagicEvent Workbench format).
- `messages [{id, spy, merc, time, priority, at}]`: SMessageTriggers, HUD text per team.
- `shakers` and `noises [{id, class, at, properties}]`: SShaker and SNotifyNoise.
- `level [{event, sendAtTime}]` (matched by event): events the LevelInfo sends at a time counted back from the end of the match; use a negative time, since 0 or more never fires. The LevelInfo has one `Event` and one `SendAtTime` (whole seconds); several level events go out through one SMagicEvent, `LevelEvents`, that the LevelInfo sends at the earliest time and that delays each of the others by how much later it is due. So they all hang on the earliest: in a match shorter than the earliest `|sendAtTime|` none of them fires. One due more than 86400 seconds after the earliest is an error at its `sendAtTime` (an SMagicEvent delays a day at most).
- `triggerLights [{light (an environment light id), object (an SGamePlayObject id), initiallyOn}]` (matched by light): the light becomes an STriggerLight in the object's ObjectLights.

Ids are Tags, so links stay consistent; `map_query what:"wiring"` lists the live links and the unresolved ones.

## environment

Lights, sound, zones, volumes and what dresses a level (`scct://wiki/lighting`, `sound-actors`, `zones-and-volumes`, `level-ingredients`, `gameplay-objects`, `vision-mode-properties`, `static-meshes`, `fish`, `softbody-actors`, `animated-meshes`, `map-tutorials`).

- `lights [{id, preset: ceiling (LE_None) | spot (LE_Spotlight; cone sets LightCone, else the class default) | sun (LE_Sunlight; needs a Fake Backdrop in the zone) | echelon-spot (LE_ESpotShadow, EchelonNormalInfluence 0.9), in | at, radius, brightness, hue, saturation (0-255), cone, corona, switchable (an STriggerLight events can switch), yaw, pitch}]`. Static lights only use LE_None, LE_Spotlight or LE_Sunlight; they bake with `map_build what:"all"`.
- `sounds [{id, class (SoundTrigger, MusicTrigger, SAmbientSoundTrigger, AmbientSound), sound: Sound'Pkg.Group.Name' (already loaded), at, radius, volume 0-255}]`.
- `zones [{id, locationName, spaces: [pieceIds], portals}]`: a ZoneInfo with LocationName in each zone and zone portals in the doorways where zones meet; put every spawn room in a zone.
- `volumes [{id, class: Volume|SDamageVolume|ERainVolume|BlockingVolume, box: {min, max} | {in, height}, locationName, priority}]`: polygon brushes sized to the box.
- `ingredients [{id, kind: ammo (SAmmoStock, interaction 60 high / 120 radius) | infirmary (SInfirmary, blocking, 120 / 60) | glass (SVitre) | extinguisher | projector | sprinkler (hidden) | puddle | balltrap, in | at, yaw}]`.
- `objects [{id, mesh, at, yaw, damageable, hitPoints}]`: SGamePlayObjects, never bPickable. `meshes [{id, mesh, at, yaw, scale, fit: floor}]`: StaticMeshActors.
- `vision [{target, heatIntensity, heatOpacity, heatRadius, emfIntensity, emfOpacity (0-1), properties}]` (matched by target): thermal and EMF vision values on a spec or existing actor.
- `props`, `fish` (SFish: bPilote on the leader, Pilot on followers), `softbody`, `animated` (SAnimatedMesh) and `tutorials` (SZoneTips, SHelpPath, SEndZoneTip): `[{id, class, mesh, at, yaw, scale, properties}]` with raw property values (leaves are strings, structs objects, `{"$ref": id}` for actors).
- `softbody`: the classes are in the SoftBody package: ESBPatchActor (the usual cloth, the default), ESBStripDoorActor (the wiki writes ESBStripeDoorActor), ESBRopeActor and ESBChainActor; a short name gets `SoftBody.`. Only ESBChainActor takes a `mesh` (its LinkMesh); a patch draws its cloth with the texture in `properties.Texture`. Never copy a SoftBody actor: make each one new.

Zones and volumes change brushes, so the geometry is rebuilt (build auto).

## geometric_events

Geometric events, the ledges, pipes and wires players climb (`scct://wiki/geometric-events`).

- `ges [{id, kind: ledge|hoh|pipe|ladder|zipline|fence|pole, points}]`: each runs the editor's GE ADD (ledge LG, hoh HOH, pipe PIPE, ladder LADDER, zipline ZL, fence FENCE, pole POLE). Two points along the edge for ledge, hoh, zipline, fence and pole; three for pipe and ladder: bottom, top, then the side the player climbs from. Pipes and ladders are put low end first with the side point at that height, as the game needs. New GEs are added to the spec's list (one with an id updates that GE). The bridge cannot delete GEs: one taken out of the spec stays in the map until it is deleted in GE mode.
- `geTriggers [{id, toggle, geMask, at}]`: SGETriggers that switch GE groups; the starting mask is `map_settings level.geActivationMask`. GE IDs are set by hand in the editor (Set GE ID).

The wiki's measurements are checked (`validation`): ledge grab 208-320 standing, 448 running, 512 co-op, obstacles 115, pull-up 64, a ledge at least 32 long; fences 88-128 high (96 best) with a bClimbable texture; hand-over-hand 216 clearance; ladders an even number of bars, at least 8 (32 units a bar); ziplines at most 384 up with 198 clearance; pipes and ladders from the low end. `map_validate` measures them again on the built map.

Called with no `ges`, `geTriggers` or `remove`, it only lists the GEs: those Map Design or this server added (`ge.list`; the editor keeps no list of others) and the spec's. After a change the result has `geList` too.

## map_settings

Level Properties, Map Settings, interface textures and the in-game minimap (`scct://wiki/level-properties`, `scct://wiki/map-settings`).

- `level {song, deathPawnHeight (fDeathPawnHeight: below everything players use, or 0 for off), motionTracking: standard (MTVOffset 52, MTVSlope 85) | enhanced (8/8), events [{event, sendAtTime (negative: seconds before the end)}] (replaces the list; several chain off the earliest through one SMagicEvent, as under events), geActivationMask (the starting GE mask)}`: the LevelInfo, written with `settings.edit` inside the Undo step. The properties are written by their own names (`Song`, `fDeathPawnHeight`, `Event`, `SendAtTime`, `MTVOffset`, `MTVSlope`, `GEActivationMask`); the Level Properties sheet's headings (Events, MotionTrackingTweak, MapSnapshotInfo) are only categories.
- Map Settings (SMapSettings): `mapName` (in capitals), `briefSpy` and `briefMerc` (shared by every mode), `legend` (date, place and a one-line story; `|` breaks lines), `legendInBrief` (the numbered minimap zones), `modes [{mode: story|diskhunt|deathmatch, respawn 5-15, timeLimit 0-20 minutes, spiesLives and sentriesLives 0-10 (0 is unlimited), scoreLimit, handicap, numDisksMax (Disk Hunt; at most the disks placed), strings {explicationSpy, explicationMerc, briefSpy, briefMerc, legendSpy, legendMerc}}]` matched by mode (`removeModes` drops one); or all of it as one object in `mapSettings` (also called `settings`). Modes[] and ModesStrings[] are written index-aligned with GameModeId GM_Multi, GM_DiskHunt or GM_DeathMatch, and LegendModeInBrief* defaults to the Ubisoft text.
  - SMapSettings exist only once a new map is saved, closed and opened again: `map_file action save_as`, `editor_restart`, then this tool. This editor build may not be able to write them over the bridge: then the result's `mapSettings` says `applied: false` and gives the checklist of values and the manual steps (right-click empty viewport space > Map Settings), and the values stay in the spec for the next try.
- `interface {menu, briefing (256 x 256, DXT5 with alpha), loadingAtt, loadingDef (512 x 512, DXT1)}`: image files on this machine (full paths, or relative to the work directory) imported into the map's `<Map>-i` package (`interface.texture`; PNG, BMP, JPEG, GIF and TIFF are resized, TGA must already be the right size; `image_convert` prepares them).
- `minimap true` or `{floors: [z...], framing: gameplay|everything|current, style, objectives, spawns, meshes, briefing, briefingFloor}`: draws the in-game (F7) map from the built geometry into the LevelInfo's `MapFloors` and `SnapshotCamera` (the MapSnapshotInfo category of Level Properties; `minimap.apply`).

Checked: MapName in capitals, the ranges above, one Modes and ModesStrings entry per mode, Disk Hunt ScoreLimit and Handicap against the disks, SendAtTime negative, interface texture formats. Save the map afterwards (`map_file action save`).

## map_validate

What it checks (`scct://rules` lists every rule with its severities):
- **Spawns:** PlayerStarts for both teams (TeamNumber 0 spy, 1 merc), at least 3 each; spy starts not within 512 units of an objective, the teams not within 768 of each other, every objective within 4096 of a merc start; starts out of solid geometry; objectives the spies can reach through the blockout.
- **Story, Disk Hunt, Deathmatch:** an SMission per mode (GameMode GM_Multi, GM_DiskHunt, GM_DeathMatch); Objectives and Triggers naming real actors of the right kind; no SObjective outside a mission; MinimumObjectives reachable; sector chains (top mission bChained True over GM_Undefined sectors); SFlag with 1-5 drop zones; SComputerObjectiveTrigger about 78 units above the floor, clear of collision, using DominationCaptureTime; SDisk Group 1-3, listed in the Disk Hunt mission, enough for ScoreLimit and Handicap; Deathmatch AtResetEvent switching security off; objective player counts.
- **CamNet:** at least one SCamNetwork (mercs softlock without), exactly one bFirstCam, one closed NextCam/PrevCam loop, collision on.
- **Security:** detectors' Event reaching an SAlarm Tag, motion sensors with volumes, alarms fed and doing something, no SBombTargetObjectiveTrigger locked, MessagePriority 0-5, no SAlarmZoneInfo.
- **Other actors:** LightEffect LE_None/LE_Spotlight/LE_Sunlight (LE_ESpotShadow for Echelon), no bPickable, no duplicated SoftBody, movers with a static mesh, hidden tutorial and sprinkler sprites, ingredient collision and interaction sizes, Events that reach a Tag, fDeathPawnHeight and level SendAtTime.
- **Release:** Map Settings (created, MapName in capitals, RespawnTime 5-15, lives 0-10, TimeLimit 0-20, Modes and ModesStrings aligned), the `<Map>-i` interface textures (Menu/Briefing 256 DXT5, LoadingATT0/DEF0 512 DXT1), the map file name, release package names and `.uas` contents.
- **Geometric events** against the wiki's measurements: ledge grab 208-320 standing, 448 running, 512 co-op, 115 obstacle, 64 pull-up, 32 long; fence 88-128 (96 best); hand-over-hand 216 clearance; ladders an even number of bars, at least 8 (32 units a bar); zip line at most 384 up with 198 clearance (161 tucked); pipes and ladders from the lower end.
- **Folded in:** the Map Design check on blockout pieces (layout, traversal), Map Check rows (zone leaks among them) and the lighting budget.

`include` groups: `wiki` (spawns, modes, objectives, CamNet, actors, level, GEs), `design` (Map Design pieces: layout, traversal, reachability), `security` (alarms and detectors), `mapcheck` (the editor's Map Check, zone leaks), `lighting` (the lighting budget), `release` (Map Settings, interface textures, file and package names). `profile release` makes 3 starts per team, Map Settings, interface textures, isolated rooms and unreachable objectives errors.

A finding's `fix`:
- On the live map: `{tool, args, note?}`, the tool call that repairs it (`actors_edit` edits; `gameplay`, `security`, `environment`, `map_settings`, `geometric_events` or `blockout` fragments, which change the map's stored spec and build it; `map_build` or `release`).
- For a spec checked offline (`spec` or `path`, and `level_spec_validate`): `{spec: [{pointer, merge | change}], note?}`, the edit of that spec. `merge` is laid over what is at the JSON pointer as the intent tools merge; `change` says in words what to write there. A fix that only the built map can take (`release`, `map_build`, `actors_edit`) keeps its tool call with `live: true`.

## release

Takes a finished map to a shareable release by the wiki's rules (`scct://wiki/map-packaging`, `scct://wiki/sound-browser`, `scct://wiki/map-settings`).

- **`action: check`** (changes nothing) reads the game folder beside this server and, when the map is open, the editor:
  - the file name (ASCII without spaces; the wiki's convention is the first 4 letters of the map name plus a suffix letter, D for the default version: Aquarius -> AquaD);
  - `Packages\MapsEd\<Map>.sdc` (needed to edit the map ever again) and the playable `Packages\Maps\<Map>.sdc` every save writes, not older than it;
  - unique package base names (the engine ignores extensions, so `Textures\X.utx` and `StaticMeshes\X.usx` are one package; name them `<Map>_TXT.utx`, `<Map>_STM.usx`, `Amb_<Map>.uax`, never `<Map>` alone);
  - `Packages\Textures\<Map>-i.utc` with SMapSettings and the four interface textures (Menu and Briefing 256x256 DXT5, LoadingATT0 and LoadingDEF0 512x512 DXT1), and no stray `<Map>-i.utx` (the game ignores it: delete it);
  - `Packages\Sounds\<Map>.uas`, which the wiki says to always build, with Interface (objective sounds, win/lose music) and `Amb_<Map>` (read from the build log RE+ writes beside it; a `.uas` built elsewhere cannot be verified);
  - the packages the map imports (its own must be saved and shipped; the rest must be stock packages players have, never modified: pass `base`, a clean install, to compare them byte for byte);
  - and Optimise Map Assets' suggestions.

  It returns findings `{severity, rule, wikiPage, message, fix}`, `ready`, the archive layout and a `release` block for `map_validate`.
- **`action: sounds`** builds `Packages\Sounds\<Map>.uas` (replacing the old one) from Interface, `Amb_<Map>` and `packages`, at Ogg Vorbis quality 5.0 (Ubisoft's; lower is not recommended). Streamed sounds must be flagged Stream in the Sound Browser first.
- **`action: optimise`** shows Optimise Map Assets' report, or with `packs` (or `suggested: true`) saves the map and makes its release copy with only the assets it uses from those packs, moved inside the map (`destination: map`, the default) or into a new package of its own (`destination: package`, `assetPackage <Map>_<name>`); the working map and the packs end as they were. The copy goes in `<Game>\Releases\<Map>`, or `folder`: a full path inside `<Game>\Releases`, the work folder or `[Mcp] WriteRoots` (the editor refuses any other folder, since with `overwrite` the copy replaces the map's files there).
- **`action: package`** does the same when packs are given, then lists the archive: `Packages\Maps\<Map>.sdc`, `Packages\MapsEd\<Map>.sdc` (keep it: a Maps file cannot be turned back), `Packages\Textures\<Map>-i.utc`, `Packages\Sounds\<Map>.uas` and the map's own packages, with each file's source, size and whether it is there.
- **`action: interface`** imports `images` (menu, briefing, loadingAtt, loadingDef: full paths; PNG, JPEG and BMP are fitted to the slot, TGA and DDS must already be its size) into `<Map>-i`; give Menu the SCCT fade mask (map-settings).

The map must be saved under its release name first (`map_file save_as`). Stock packages are never written: every package this tool writes is the map's own. Files are not undoable.
