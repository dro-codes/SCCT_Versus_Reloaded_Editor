# Alarms

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Alarms

An alarm system needs two kinds of actor:
- `SAlarm`: the alarm controller.
- **Detectors**: actors that detect players and trigger the alarm.

A detector sends a **Trigger** event when it detects a player. An `SAlarm` whose `Tag` matches that event activates. In other words, the detector's `Event` must equal the SAlarm's `Tag`.

## SAlarm

An **invisible** actor that controls how the alarm behaves. When triggered it can:
- show messages
- lock doors or objectives
- trigger more events

| Property | Description |
|---|---|
| `AlarmDescription` | HUD message for **Mercenaries** while the alarm is active. |
| `AlarmDescriptionSpy` | HUD message for **Spies** while the alarm is active. |
| `Duration` | How long the alarm lasts once triggered, in seconds. |
| `Events` | Events triggered when the alarm activates. They are **untriggered when the alarm ends**. |
| `MessagePriority` | How the alarm message behaves (table below). |
| `MoversToLock` | Movers, such as doors, locked while the alarm is active. |
| `ObjectiveTriggersToLock` | Objective triggers locked while the alarm is active, such as `SComputerObjectiveTrigger` or `SFlag`. **`SBombTargetObjectiveTrigger` cannot be locked.** |

The property sheet is image-only.

### MessagePriority values

| Value | Behaviour |
|---|---|
| `0` | Message mode: a general HUD message such as "Doors Unlocked". |
| `1` | As `0`, plus a single beep when the message appears. |
| `2-4` | Recommended for alarms that **lock nothing**. No timer is shown, and Spies only see "You've been detected". |
| `5` | Full alarm mode. Shows "You've been detected - Objectives locked", plays a beeping sound for both teams, and shows the alarm duration on the HUD. |

## Detectors

Detectors are the sensors of the alarm system. When one fires, it sends an event that triggers the `SAlarm`.

Detectors can be **temporarily disabled** in two ways:
- **By Spies**, with chaff or sticky shockers. The duration is the `StickyNeutralizationTime` property, in the `SGamePlayObject` category.
- **By an event** that matches the detector's own `Tag`.

### SLaserTrigger

Projects a laser beam forward from whatever surface it is placed on. A player crossing the beam makes the laser send its event and trigger the alarm.

| Property | Description |
|---|---|
| `bHideLaserInNormalVision` | If `True`, the beam is hidden in normal vision. Spies still see it in Thermal Vision, and Mercenaries in EMF Vision. |
| `DetectedClass` | What the laser detects: the default is Spy (`SPawnAttaque`), Mercenary is `SPawnDefense`, and both teams is `SBasePawn`. |
| `LaserLength` | Beam length in centimetres (**1 cm = 1 Unreal Unit**). |
| `SpawnPerCent` | Chance that the laser spawns when the map initialises: `100` always, `50` half the time. |

### SCamera

Detects players inside a **cone of vision**. The player must be **visible** to the camera to trigger it.

| Property | Description |
|---|---|
| `bAutoWallSupport` | Automatically spawns a wall support and attaches the camera to the wall. |
| `DetectionDelay` | How long a player must stay detected before the camera fires. With a higher value, a player can briefly cross the view without being detected. |
| `PatrolAngle` | Patrol sweep in degrees. For example, `90` sweeps a 90° range. |
| `RotationVelocity` | How fast the camera turns while patrolling. |
| `VisibilityConeAngle` | Width of the detection cone in degrees. |
| `VisibilityMaxDistance` | The farthest distance at which it detects players. |

### SVolumetricSensor

Detects players inside **one or more Volumes**. A player entering a volume makes the sensor send its event.

| Property | Description |
|---|---|
| `DetectedClass` | Spy (`SPawnAttaque`, the default), Mercenary (`SPawnDefense`) or both (`SBasePawn`). |
| `DetectionDelay` | How long a player must stay inside before it fires. With a higher value, a player can briefly pass through without triggering the alarm. |
| `MinDetectedVelocity` | The minimum movement speed that is detected inside the volume. |
| `Volumes` | The `Volume` actors used as detection areas. |

To create a `Volume`:
1. Size and place the builder brush.
2. Click the `Volume` button in the left toolbar and choose `Volume`. The volume matches the builder brush's size and location.

Tip: `O` toggles volume visibility in the editor.

## How the MCP tools apply this

`security` (spec `security`, built by `level_build`) writes:
- `alarms[{id, description, descriptionSpy, duration, priority (0-5), outputs, lockMovers, lockObjectives}]`
  - The output is an `SAlarm`.
  - `outputs` fill `Events`.
  - `lockMovers[{mover, action}]` fill `MoversToLock`. Each entry is written as the complete struct `{ActionBeforeLock, MoverToLock}`, as the RE+ Security window writes it. `ActionBeforeLock` defaults to `ActionBeforeLock_None`; the wiki does not document its other values, so read them from `map_query what:"schema" classes:["SBase.SAlarm"]` (the schema takes full class names).
  - `lockObjectives` fill `ObjectiveTriggersToLock`. Bomb targets are refused.
- `devices[{id, kind: laser|camera|motion|presence|mine, at|in|box, facing, alarm, detected: spy|merc|both, length, ...}]`
  - Each device's `Event` is set to its alarm's Tag.
  - `detected` maps to `DetectedClass`: `SPawnAttaque` / `SPawnDefense` / `SBasePawn`.
  - A `motion` device builds the `SVolumetricSensor` **and** its detection `Volume`, as a polygon brush sized to the box. `geometry:"box"` would always be a 256 uu cube.
  - RE+ places lasers and presence detectors 48 uu above the floor, and cameras at 200 uu.
- `camnet[...]`: see `scct://wiki/level-ingredients`.

Priority guidance:
- Use `priority` 5 when the alarm locks something.
- Use 2-4 when it locks nothing.

Inspection:
- `map_query what:"security"` lists alarms with the detectors feeding them and what they trigger.
- Rules: `security.dangling-event`, `security.sensor-no-volume`, `security.detector-unwired`, `security.alarm-unfed`, `security.alarm-no-output`, `security.bomb-locked`, `security.lock-unresolved`, `security.message-priority`, `security.wiring`
