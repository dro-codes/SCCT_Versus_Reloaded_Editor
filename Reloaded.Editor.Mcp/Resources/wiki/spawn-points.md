# Spawn Points

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Spawn-Points

Spawn points set where players start when a level begins. Two actors configure them: `PlayerStart` and `STriggerPlayerStart`.

## PlayerStart

Versus maps hold several players, so place several `PlayerStart` actors throughout the level.

- **Rule (wiki tip):** create **at least 3 spawn points for both the Spy and the Mercenary teams**. With fewer, the game cannot assign spawn points correctly in **2v2** and **3v1** matches.
- The team is set by the `TeamNumber` field:

| `TeamNumber` | Team |
|---|---|
| `0` | Spy |
| `1` | Mercenary |

The full PlayerStart property sheet is image-only on the wiki. Only `TeamNumber` is described in text.

## STriggerPlayerStart

`STriggerPlayerStart` works like `PlayerStart`, but events can enable or disable it during play.

- **Sector maps:** used in linear maps where play moves through several areas, such as Orphanage, Station or Missile Strike.
- **Deathmatch:** some maps use it for separate Deathmatch spawn points.
- **Control:** give it a `Tag`. Each time an event calls that Tag, the spawn point toggles between enabled and disabled.

The STriggerPlayerStart property sheet is image-only on the wiki.

## Related

- `scct://wiki/events` (how a `Tag` receives an event)
- `scct://wiki/game-modes` (sector maps and Deathmatch)
- `scct://wiki/zones-and-volumes` (give spawn areas a `ZoneInfo` `LocationName`)
