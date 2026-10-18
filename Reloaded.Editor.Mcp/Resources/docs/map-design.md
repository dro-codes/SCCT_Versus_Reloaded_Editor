# Map Design: blockout, layout checks, devices and zones

Abridged from RE+ `docs/MapDesign.md`, the **RE+ Tools > Map Design...** workspace, keeping what an agent driving it through MCP needs. The MCP compiler emits the same parametric pieces (`design.block` / `design.blockbatch`) and registers them, so the human can keep editing them in the panel.

## Piece kinds (`scct.level-spec` `geometry.pieces[].kind`)

| Kind | What it builds |
|---|---|
| `Room` | Carves the interior space; width/length/height are that space. |
| `Corridor` | The same carve, extended by the wall thickness at each end so it cuts through the walls of the rooms it joins. |
| `Vent` | A corridor at **crouch** height (default **125**), checked against crouch clearance. |
| `Crawlway` | Lower still, for a slow walk (crawl clearance, default **105**). Switching a piece to Vent or Crawlway sets its height. |
| `Doorway` | A subtractive opening through a wall, starting at the interior face; length is its depth. It can carry a **zone portal**. |
| `Stairs` | 1-128 solid steps rising along local +Y; height is the total rise. Straight, L and U flights also get an invisible, semi-solid glide ramp over the steps. **Treads are about 32 uu of run and at most 24 uu of rise**; resizing recounts them. |
| `Stairs L` | Two flights with a landing, turning right. Width is one flight; length is the first flight's footprint (at least width + 8). |
| `Stairs U` | Two flights side by side with a half landing. The footprint is 2 × width by length. |
| `Spiral` | Wedge steps round a post, one full turn over the height. Width is the outer diameter (≥64). There is no glide ramp; risers are kept to **16 uu**. From the Floors menu it is 384 across (144 of tread) in a shaft with 24 of clearance. |
| `Ramp` | A solid wedge rising along local +Y. |
| `Platform` | A solid rectangular slab; height is its thickness. |

The defaults:
- **New blockout** is a 1024 × 1024 × 256 carved room.
- Placed pieces take the texture browser's current material on every face. The MCP `spec.material` overrides it.
- A piece placed under its shape's plain name is numbered (Room 1, Room 2, ...).

### Construction

This engine starts from **solid space**:
- **`Carve`** (the default, and what the MCP compiler always uses) builds a room or corridor from a single subtractive brush. It leaves far fewer surfaces to cut, light and break than **`Shell`**.
- **`Shell`** carves the outer volume and adds back a floor, four walls and an optional ceiling. The ceiling option applies to shell only.
- **Wall thickness** sets the gap a carved room leaves between itself and its neighbours, and the depth its doorways cut. The spec default is 16.

Further rules:
- **Rotation is built into the brush shape. The brush actors stay unrotated:** the editor's wireframes apply actor Rotation, but its geometry build does not.
- Additive stairs, ramps and platforms belong **inside** carved space.
- **Rebuild geometry** (`map_build`) after placing, resizing or moving brushes. The design view previews source brushes, not the CSG result.

### Doorways and zone portals

A doorway is cut from the wall's interior face. Two kinds of doorway are rejected:
- an offset that runs past the wall;
- an opening that reaches above the ceiling.

A **zone portal** is an invisible, non-solid sheet in the middle of the opening, flagged as the editor's Add Special zone portal. Zoning keeps visibility cheap in these maps. Rebuild, then check the zones in game.

Editing:
- The toolkit refuses to overwrite manual edits to a generated piece's brushes.
- **Detach** turns a piece back into ordinary brushes.

## Versus starter layout (and why the MCP example differs)

The panel's **Versus starter layout** places:
- an objective room in the middle;
- a spawn room per team at the end of a corridor either side;
- a crouch vent from the spy room into the objective room's side wall;
- **two starts per team**, a mission with an objective and a terminal, and ceiling lights.

That breaks wiki rules: it has 2 starts instead of ≥3, the terminal at floor+64 instead of ~78, and no CamNet. The MCP `versus_starter` prompt and `scct://examples/versus-starter` therefore reuse **only its geometry**, and supply:
- 4 starts per team;
- the terminal at floor+78;
- a 3-camera CamNet loop;
- zones and all three modes.

## Movement limits (defaults)

| Limit | Default |
|---|---|
| Step-up height | **35 uu** |
| Walkable slope | **45°** |
| Standing clearance | **96 wide × 180 tall** |
| Crouch clearance | 96 wide × **125** |
| Crawl clearance | **105** |
| Crouch speed factor | 0.5 |
| Climb speed (ladders, drops) | 120 uu/s |
| Spy / Merc speed | 300 / 300 uu/s |

Blockout previews warn in these cases:
- stairs rise faster than a pawn can step;
- a ramp is steeper than the walkable slope;
- a platform is out of step reach;
- an opening is narrower or lower than the clearance.

These warnings are planning aids; confirm real limits in game.

## Check design (folded into `map_validate`)

It reports the following:
- **Layout:**
  - a room no doorway, corridor or vent reaches;
  - a doorway cutting into nothing;
  - two carved spaces that merge;
  - vents a standing pawn walks through;
  - the traversal warnings.
- **Playability:** a PlayerStart per team, an SMission, an SObjective and a camera network.
- **Objective flow:**
  - a mission with no objectives;
  - an objective no mission lists, or with no terminal, bomb target or trigger;
  - a flag without a drop zone.
- **Spawn distances:**
  - spy starts **within 512** of an objective;
  - the two teams' starts **within 768**;
  - objectives **more than 4096** from every merc start.

It also lists each objective's straight-line distance and travel time from the nearest spy and merc start.

## Floors, lifts, ladders

- A **floor step** is the room height plus the wall thickness at that point, or **272** where there is no room.
- **Stairs up/down** come with a stairwell carved to exactly their footprint: 32-unit treads, with the rise from the floor spacing.
- **Lift up/down** places an `SLift` mover:
  - a **192 × 192** platform in a shaft through the slab;
  - it rises by the floor spacing when a pawn stands on it and returns after 3 s;
  - it takes two Undo steps.
- **Ladder up** / **Pipe up** cut a **96 × 96** shaft and place the GE with its visual brush.
- **Opening through the floor above** cuts a **128 × 128** hole.

What the right-click menu places goes **on the floor of the room** clicked in:
- a light at the room's ceiling;
- otherwise a light 160 above the clicked height, and a player start 48 above it.

## Gameplay elements (GEs)

GEs are placed in stages:
1. start;
2. end;
3. the start's height;
4. the end's height;
5. for a pipe or ladder, the side the player climbs from.

Each element is built with the editor's `GE ADD` command. Pipes and ladders run from their **lower end**, with the side point at that end's height; the game ignores them otherwise. A visual brush is created with each element, so Undo twice removes the brush and the GE.

## Security devices

- New detectors are wired as placed: a detector's `Event` names the alarm's `Tag`, and the alarm's `Events` names what it triggers.
- Placement heights:
  - lasers and presence detectors sit **48** uu above the floor;
  - cameras sit at **200** uu.
- A **motion sensor** builds the `SVolumetricSensor` with its `Volume`, sized to standing clearance.
- Detection classes default to spies (`SPawnAttaque`).
- An alarm's right-click **lock** adds the selected movers to `MoversToLock` and objective triggers to `ObjectiveTriggersToLock`.

## Objectives by lobby size

Objectives and zone missions take **Minimum/Maximum players**, counting both teams; movers take the same rule. A mission can also ask for a different number of **objectives to win by match size**: 1v1 … 8v8, where an uneven lobby counts as its bigger team.

These rules compile to an `SMatchSizeRules0` script actor; the bridge ops are `objectiveplayers.set` and `objectiveplayers.targets`. The design check errors when a lobby size would leave nothing to play.

## Lights

The plan shows each light's reach.
- Presets: ceiling, small lamp, floodlight, night light, alarm pulse, flicker, and **switchable** (`STriggerLight`).
- Reach sets both `LightRadius` and `EchelonRange`.
- A ceiling light 224 up with a 400 reach lights a 331-unit circle on the floor. That figure is reach, not shadow.

## Zones (`scct.stages` version 2)

A multi-zone ("sector") map is one chained mission holding zone missions:

```
SMission (the map's, bChained True, GameMode GM_Multi)
  MinimumObjectives = how many objectives win the match (0 in the plan = every zone's threshold added up)
  Objectives = Zone1Mission, Zone2Mission, ...
    Zone<N>Mission (SMission, bChained False, GameMode GM_Undefined)
      Objectives = the zone's SObjectives, MinimumObjectives = how many end it
```

Rules:
- Every objective belongs to **exactly one** zone.
- A zone's completion fires one `SMagicEvent` with Tag `Zone<N>_Complete`, which holds its actions:
  - `open` a door: a timed door becomes `TriggerToggle`;
  - `light`: an `STriggerLight`;
  - `sound`: a `SoundTrigger`, made as `Zone<N>_Sound<i>`;
  - `announce`: an `SAlarm` `Zone<N>_Announce` that locks nothing;
  - `trigger`: any Tag.
- Zone name, Spy briefing and Merc briefing are the mission's `ObjectiveName`, `Description` and `DescriptionDEF`.
- Plans are exported and imported as `scct.stages` v2 (`scct://schema/stages`), and name actors by path.

## Persistence

Workspace data (pieces, references, annotations, movement limits) lives in `System/ReloadedEditor/library.json`, keyed by map path. It is copied to `System/ReloadedEditor/Workspaces/<map>.json` on each save.
- Unsaved maps keep it in memory until their first save.
- Group membership travels in each actor's native `Group` field.
- The MCP bridge's `design.register` adds compiled pieces to this library, so the panel can edit them, and `mcp.sidecar.set` keeps the applied spec beside them.
