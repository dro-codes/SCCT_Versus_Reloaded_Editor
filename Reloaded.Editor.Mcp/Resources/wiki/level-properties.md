# Level Properties

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Level-Properties

To open the level properties, do either of these:
- Press `F6`.
- Right-click an empty space in the map and choose `Level Properties` from the context menu.

These are the properties of the map's `LevelInfo`. The full property sheet is image-only on the wiki.

## Audio

- `Song`: rarely used in real maps. It plays a song immediately after the level loads, the way the main menu or lobby music is triggered.

## Death

- `fDeathPawnHeight`: the **Z threshold below which a player dies instantly**.
  - For example, the Spy waves his arms and the screen fades to black, which avoids long fall animations. Vertigo Plaza does this.
  - Keep it at `0.0` when the map does not need this.

## Events

- `Event`: the Tag of the event to trigger.
- `SendAtTime`: when the event is triggered, in seconds.
  - **Negative values count back from the end of the match.** For example, `-180` triggers when 3 minutes remain.
  - The event **does not trigger if the match has an infinite time limit**.
  - **Values of 0 or more never activate it.**

## MapSnapshotInfo

Controls the in-game **Strategic Map** view (F7 by default), which shows you and your teammates.

### MapFloors

An array with one entry per floor of the map. Add as many entries as the map needs.
- `FloorMapTexture`: the texture used for this floor on the minimap.
- `FloorZ`: the minimum Z for a position to count as on this floor of the minimap.

### SnapshotCamera

- `FOV`: the camera's field of view, usually left at `45.0`.
- `Position`: the XYZ of the camera that looks down on the map for the minimap. `Z` usually matches `TargetDistance`, which sets the coverage.
- `RotationRow`: rotation arrays for the snapshot camera, usually left at their defaults.
- `TargetDistance`: the minimap's effective coverage. Higher values show a larger area.

## MotionTrackingTweak

`MTVOffset` and `MTVSlope` change how **Motion Tracking Vision** looks:
- Most maps use the defaults **`MTVOffset=52`, `MTVSlope=85`**.
- Ubisoft adjusted them on a few maps, such as Club House.
- **Enhanced maps use `MTVOffset=8` and `MTVSlope=8`.** That gives a darker Motion Tracking Vision image, in which Spies are harder to see in dark areas.

Tip: the UnrealEd viewport can switch to Motion Tracking Vision, so you can preview the in-game look without starting the level each time. The toolbar button is image-only.

## Also in Level Properties (from other pages)

- `GEActivationMask`: which GE mask is active when the level begins. GEs with no ID are always enabled. See `scct://wiki/geometric-events`.

## How the MCP tools apply this

- `map_settings` with `level` (the spec's `level` section: `song`, `deathPawnHeight`, `motionTracking: standard|enhanced`, `events[{event, sendAtTime}]`, `geActivationMask`) writes these through the bridge op `settings.edit`.
  - `settings.edit` checks the LevelInfo snapshot first.
  - **The headings above (`Audio`, `Death`, `Events`, `MapSnapshotInfo`, `MotionTrackingTweak`) are property-sheet categories, not properties.** The properties are written by their own names: `Song`, `fDeathPawnHeight`, `Event`, `SendAtTime`, `SnapshotCamera`, `MapFloors`, `MTVOffset`, `MTVSlope` and `GEActivationMask`, through ImportText where the sheet does not offer one.
- `motionTracking: "standard"` writes `MTVOffset` 52 and `MTVSlope` 85, and `"enhanced"` writes 8 and 8.
- Level events: a LevelInfo has **one** `Event` and one `SendAtTime`, a whole number of seconds (an int property; a fraction is rounded, with a warning).
  - One level event is written as that `Event` and `SendAtTime`.
  - Several go through one `SMagicEvent` (`LevelEvents`): the LevelInfo sends it at the earliest time, and it sends each event delayed by how much later that one is due.
  - **So the later events chain off the earliest one.** If the match's time limit is shorter than the earliest `|sendAtTime|` (for example `-600` in a 5-minute match), the earliest never fires, and none of the others do either. Give the earliest event a time every match reaches, or keep one level event.
  - An `SMagicEvent` delays an event by at most 86400 seconds, so a level event due more than a day after the earliest is an error at its `sendAtTime` and is left out.
- `map_query what:"level"` reads the current values.
- `map_settings` with `minimap` (bridge op `minimap.apply`) fills `SnapshotCamera` and `MapFloors` (the `MapSnapshotInfo` category) from the RE+ minimap tool.
- `map_validate` warns on a `SendAtTime` of 0 or more. It errors when anything players need lies below `fDeathPawnHeight`.
- Rules: `level.death-height`, `level.send-at-time`, `level.motion-tracking`, `events.dangling`
