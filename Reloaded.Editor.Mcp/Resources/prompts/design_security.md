---
name: design_security
title: Design the security layer
description: Plan and build alarms, lasers, cameras, motion sensors, presence detectors, door locks and the Mercenaries' CamNet for the open map, wired the way the wiki requires.
arguments:
  - name: area
    description: Where to focus (room names, zone names or "objectives"); default the whole map.
    default: the whole map, around each objective
  - name: intensity
    description: light, medium or heavy.
    default: medium
  - name: details
    description: Specific requests, such as "a laser grid in the vault" or "lock the server door when the alarm goes".
wiki: [alarms, level-ingredients, events, movers, zones-and-volumes, game-modes]
rules: false
---
# Design the security layer

**Focus:** {{area}}. **Intensity:** {{intensity}}.
{{#details}}**Requests:** {{details}}
{{/details}}
The rule catalogue is not embedded, to keep this recipe short: read `scct://rules` (every check with its draft and release severity and its wiki page) before you design or fix anything.

## Recipe

1. **Read the map.**
   - Call `editor_status`, then `map_query` with `what:"summary"`, then with `what:"security"` (existing alarms, detectors and their wiring), `what:"cameras"` (the CamNet) and `what:"spawns"`.
   - Call `plan_render` (no spec) for a top-down view of the live map.
2. **CamNet first.** Mercenaries are softlocked without it.
   - Use 3 or more `SCamNetwork` cameras covering the objectives and the choke points, with `CamName` set to the room each one covers.
   - Exactly one has `bFirstCam True`.
   - `NextCam`/`PrevCam` form one closed loop.
   - Set `bBlockActors`/`bBlockPlayers` `True`.
   - Limit `PitchConstraint`/`YawConstraint` for wall mounts (`Y` = min, `Z` = max).
   - Keep the existing cameras and extend the loop.
3. **Alarms.** Use one `SAlarm` per protected area:
   - `AlarmDescription` (Merc HUD), `AlarmDescriptionSpy` (Spy HUD) and `Duration`.
   - `MessagePriority`: **5** when it locks something (shows the timer and beeps), **2-4** when it locks nothing.
   - `Events` (outputs, untriggered when the alarm ends), `MoversToLock` (doors) and `ObjectiveTriggersToLock` (terminals, flags).
   - **Never** list an `SBombTargetObjectiveTrigger` in `ObjectiveTriggersToLock`; it cannot be locked.
4. **Detectors.** Each detector's `Event` equals its alarm's `Tag`.
   - `SLaserTrigger`: `LaserLength` spans the opening (1 cm = 1 uu). `bHideLaserInNormalVision` for harder lasers. `SpawnPerCent` below 100 for randomness.
   - `SCamera`: `VisibilityConeAngle`, `VisibilityMaxDistance`, `PatrolAngle`, `RotationVelocity` and a `DetectionDelay` so a quick dash can get through.
   - `SVolumetricSensor`: always with its `Volumes`. The tool builds the `Volume` as a sized polygon brush. Use `MinDetectedVelocity` so slow, sneaky movement passes.
   - Presence detectors and mines, where the intensity calls for them.
   - `DetectedClass`: `SPawnAttaque` (spies, the default), `SPawnDefense` (mercs) or `SBasePawn` (both).
5. **Balance by intensity.**
   - light: one alarm and 2-3 detectors on the main route.
   - medium: an alarm per objective, with a mix of lasers, cameras and a motion sensor, and at least one quiet alternative route (vent, GE climb).
   - heavy: layered detectors and locked doors. Still leave a route chaff or a sticky shocker can open (detectors are disabled by `StickyNeutralizationTime`).
6. **Modes.**
   - Deathmatch and Disk Hunt usually switch security off: their `SMission` `AtResetEvent` fires an `SMagicEvent` that untriggers the detectors and alarms.
   - Use `gameplay` with `deathmatch:{disableSecurity:true}` (or `events`).
7. **Apply.** Call `security` with `alarms`, `devices` and `camnet` (spec `security` section), with `dryRun:true` first and then for real. Door locks go through each alarm's `lockMovers`; button-opened doors through `mechanisms`.
8. **Check.**
   - Call `map_validate` with `include:["security","wiki"]`, and fix every `security.*` and `camnet.*` finding.
   - Call `view_screenshot` with `preset:"objective:<name>"`.
   - Call `map_query` with `what:"wiring"` to confirm that no Event dangles.
9. **Report.** List each alarm with its detectors, outputs and locks, plus the CamNet order, and the spy routes that avoid detection.
