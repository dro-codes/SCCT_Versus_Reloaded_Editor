# Vision Mode Properties

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Vision-Mode-Properties

Some actors have properties that change how they look in the vision modes: **Thermal Vision**, **EMF Vision** and **Motion Tracking Vision**. Tweak them for specific gameplay or visual effects. Each category's property sheet is image-only on the wiki. The tables below are the text.

## Advanced

| Property | Description |
|---|---|
| `bSeeThroughInMotionVision` | If `True`, Motion Tracking Vision tracks objects passing behind this actor. Typically used for transparent surfaces such as glass. |

## Display

| Property | Description |
|---|---|
| `bHideInNormalVision` | If `True`, the actor is visible only in Thermal Vision or EMF Vision. |

## EchelonHeat (Thermal Vision, Spies)

| Property | Description |
|---|---|
| `bSideFadeEffect` | Enables a side fade effect in Thermal Vision. |
| `HeatIntensity` | The actor's thermal intensity, `0.0` to `1.0`. |
| `HeatOpacity` | Transparency in Thermal Vision, `0.0` to `1.0`. |
| `HeatRadius` | Adds a thermal radius around the actor that affects nearby objects in Thermal Vision. |

## EchelonEMF (EMF Vision, Mercenaries)

| Property | Description |
|---|---|
| `bEMFIsOn` | If `True`, the actor shows the `EMFTexture` defined in any `EMFTextureModifier` or `SpecialVisionTextureModifier`. |
| `EMFIntensity` | The actor's EMF intensity, `0.0` to `1.0`. |
| `EMFOpacity` | Transparency in EMF Vision, `0.0` to `1.0`. |

## Motion Tracking Vision

Motion Tracking Vision is tuned for the whole level in Level Properties, by the LevelInfo's `MTVOffset` and `MTVSlope` (listed under the `MotionTrackingTweak` category of its property sheet). See `scct://wiki/level-properties`.

Material-side vision settings (`bHideInEMFVision`, `bHideInThermalVision`, `HeatTextureModifier`, `EMFTextureModifier`, `SpecialVisionTextureModifier`) are in `scct://wiki/material-classes`.

## Event-gated reveal (and a limit)

Spies' vision **modes** (Thermal, EMF, Night) are the Spy's own goggles — there is **no per-map property or event that turns a vision mode on or off**, and Motion Tracking is only tuned level-wide (`MTVOffset`/`MTVSlope`). So a literal "the Spy's thermal is degraded until a panel is hacked, then restored" is **not** something a map can author; it would need a game-code change.

What a map *can* do, with no engine change, is make **what a vision mode reveals** change on an event — the **event-gated reveal**:
1. Place a marker/clue actor and set `bHideInNormalVision = True`, so it exists only in Thermal/EMF Vision.
2. Also set it `bHidden = True`, so it is not shown at all yet.
3. Wire a hack (an `SInterrupteur` → `SMagicEvent`, see `scct://wiki/events`) to **un-hide** it.

Hacking the panel then reveals a thermal-only trail or marker that was invisible before — you are changing *what Thermal Vision shows*, keyed to match state, which is the achievable form of the idea.

Related levers for perception-on-an-event: an event-triggered `SSprinkler` haze or a damaged `SExtincteur` degrade the **Mercenaries'** view for a time (`scct://wiki/level-ingredients`), and switching Echelon / `STriggerLight` lights on a hack changes what Night Vision and Thermal pay off (`scct://wiki/lighting`).

## How the MCP tools apply this

- `environment` with `vision[{target, ...props}]` (spec `environment.vision`) sets these properties on existing or spec actors.
  - Values are Map JSON strings. For example, `bHideInNormalVision: "True"` and `HeatIntensity: "0.8"`.
  - `hideInNormalVision: true` is a convenience field that writes `bHideInNormalVision=True` (the base of the event-gated reveal above); any other vision property still goes through `properties`.
  - The 0.0-1.0 ranges are checked.
- `actors_edit` `set` is the direct alternative for single actors.
- `map_query what:"schema"` with `classes` (full names, such as `SBase.SVitre`) lists which of these properties a class exposes.
- Glass (`SVitre`, or meshes acting as windows) usually wants `bSeeThroughInMotionVision: "True"`.
