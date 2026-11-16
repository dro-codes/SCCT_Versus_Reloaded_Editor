# Zones and Volumes

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Zones-and-Volumes

Zones and Volumes define areas of a map that affect gameplay or trigger special behaviour, such as:
- showing location names
- applying damage
- controlling ambient effects

The property sheets are image-only; the tables below are the text.

## ZoneInfo

`ZoneInfo` works as in standard Unreal Engine 2, with no major changes. SCCT Versus Reloaded adds the **`LocationName`** property in the `ZoneInfo` category.
- The label shows on the Spy and Mercenary HUD when the player has **`Show location on HUD`** enabled in their Reloaded Settings.

General UE2 background (not on this wiki page): zone portals seal areas, and each zone gets a `ZoneInfo`.
- Zones also confine `bAffectOwnZoneOnly` dynamic lights (see `scct://wiki/lighting`).
- An `SShaker`'s `Zone` and an `SObjective`'s `TriggersERVisibilityZone` also name a `ZoneInfo`.

## Volume

`Volume` works as in standard Unreal Engine 2, like `ZoneInfo`.
- For more precise HUD location names, build volumes **inside a single zone** to show several location names in it.
- To create one, shape and place the builder brush, then click the `Volume` button on the left toolbar and choose `Volume`.

| Property | Description |
|---|---|
| `LocationName` | Text shown on the HUD as the location name. |
| `LocationPriority` | When two volumes overlap, the one with the **higher** priority is shown on the HUD. |

Rules:
- **A volume always overrides the `ZoneInfo`'s location name while the player is inside it.**
- **Tip (gotcha):** assign `LocationName` to every spawn area **through the `ZoneInfo`**. A player who spawns inside a volume does not get the volume's `LocationName` straight away: **only entering or leaving a volume updates the HUD**. Relying on volumes alone gives wrong location names at spawn.

## SDamageVolume

A volume that damages players. It sets which team it affects, the damage type and the hit points.

| Property | Description |
|---|---|
| `bActive` | Whether the volume is active. When it is not, an event must trigger it. |
| `bToggle` | With `True`, each trigger toggles the volume ON/OFF. With `False`, it stays active once triggered. |
| `DamagedClass` | Who is affected: everyone (`SBasePawn`), Spies (`SPawnAttaque`) or Mercenaries (`SPawnDefense`). |
| `DamageType` | The damage applied: generic `SDTBase`, knockout `SDamage_Charged`, taser `SDTStickyShocker`, or fall `SFell`. |
| `HitPoints` | Damage dealt to a player entering the volume. |
| `Period` | How often the damage repeats while the player stays inside, in seconds. A `Period` of 1.0 damages every second. |

## ERainVolume

A volume that makes rain in an area.
- **The rain does not collide with geometry**, so size and place the volume carefully. Where the volume intersects a surface, the rain passes straight through it.

| Property | Description |
|---|---|
| `m_DistanceToCamera` | Distance from the camera at which the rain starts to render. A value of 200 starts it 200 units away. |
| `m_maxDistFromCamera` | Farthest distance at which the rain volume renders. |
| `m_RainFadeScale` | Visual fading of the rain as it falls. Lower values look fainter or darker; higher values are more visible. |
| `m_RainFalloff` | Falloff of the rain intensity across the volume. |
| `m_RainSize` | XYZ size of the rain volume. **`Z` is 1:1 with Unreal units, but `X`/`Y` are halved.** To fill a 512x512x512 BSP brush, use `X=256`, `Y=256`, `Z=512`. |
| `m_RainMaterial` | Material used to render the rain. |
| `m_RainMatScale` | Scale of the rain material. |
| `m_RainThickness` | Visual thickness of the rain. |

## SAlarmZoneInfo

A modified `ZoneInfo` that automatically detects intruding Spies entering the zone.

- **Ubisoft never used it in their final maps, and it is generally not recommended.** A `Volume` with an `SVolumetricSensor` is better and more flexible (see `scct://wiki/alarms`).
- **Odd behaviour:** if a Mercenary is in the zone when a Spy enters, the alarm does not fully activate:
  - the alarm message still appears;
  - linked events do not trigger;
  - no objective triggers or movers lock.
- Detection is based on the **player camera** entering the zone, not the Spy character, which can mislead players.

| Property | Description |
|---|---|
| `bDetectIntruders` | If `True`, the zone detects Spies entering it and sends a trigger for an alarm. |

## How the MCP tools apply this

Zones (spec `zones[{id, locationName, spaces}]`):
- The compiler adds **zone portals** in the doorways where zones meet (Map Design doorway `portal`).
- It places one `ZoneInfo` per zone, with `LocationName`, inside the zone's spaces.
- **Put every spawn room in a zone**, so the players get a `ZoneInfo` `LocationName` at spawn, as the tip above requires. `map_validate` notes a `ZoneInfo` without one.
- Run `map_build` and then `map_validate`: Map Check reports zone leaks.

Volumes (`environment` `volumes[{id, class: Volume|SDamageVolume|ERainVolume|BlockingVolume, box:{min,max} | {in}, locationName, props}]`):
- Volumes are made as **polygon brushes** sized to the box. The map-changes `geometry:"box"` is always a 256 uu cube.
- `ERainVolume` `m_RainSize` is computed as half X/Y, full Z.

RE+ never creates `SAlarmZoneInfo`. Motion detection is an `SVolumetricSensor` plus a `Volume` (`security` `devices kind:"motion"`).
- Rules: `zones.alarm-zone`, `zones.location-name`, `zones.leak`, `story.visibility-zone`
