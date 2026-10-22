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
