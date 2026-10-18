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

## How the MCP tools apply this

Placement:
- `gameplay` (spawns), or the `spawns` section of a `scct.level-spec` passed to `level_build`, creates `PlayerStart` actors. `TeamNumber` is `"0"` for spy and `"1"` for merc.
- RE+ places **4 per team** by default (`defaults.startsPerTeam`). That is above the wiki minimum of 3.
- Starts go 48 uu above the floor, on a 128 uu lattice inside the named room, facing `facing.toward`.
- `spawns.toggles[{id, team, in|at, enabledBy}]` creates an `STriggerPlayerStart` whose `Tag` is the toggling event.

Checking:
- `map_query what:"spawns"` lists starts per team.
- `view_screenshot preset:"spawn:spy"` / `"spawn:merc"` shows them.

Placement guidance (RE+ design checks, not wiki rules):
- Spy starts should not be within 512 uu of an objective.
- The two teams' starts should not be within 768 uu of each other.
- Every objective should be within 4096 uu of a merc start.
- Rules: `spawns.team-missing`, `spawns.too-few`, `spawns.in-solid`, `spawns.spy-near-objective`, `spawns.teams-close`, `spawns.objective-far`, `layout.unreachable`, `info.travel`
