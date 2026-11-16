# Geometric Event (GE)

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Geometric-Event-(GE)

Geometric Events (GEs) let **Spies** perform special moves and let **Mercenaries** use ladders.

## GE Mode

- To add or edit GEs, enter **GE Mode** with the GE button in the left toolbar. The button is image-only. Leave GE Mode by switching back to Camera Movement mode.
- In GE Mode, Unreal's functions are limited: you can only view and edit GEs and move the camera.

## Creating GEs

Hold `Shift` with the event's key, then left-click the **edge of a brush or StaticMesh** where the GE goes. Wireframe mode makes it easier to place GEs exactly on edges.

| Geometric Event | Key shortcut |
|---|---|
| Ledge Grab | `Shift + L` |
| Hand-over-hand | `Shift + H` |
| Pipe | `Shift + P` |
| Ladder | `Shift + E` |
| Zipline | `Shift + Z` |
| Fence | `Shift + F` |

## Editing GEs

- Select GEs by clicking them or dragging a selection box around them. Edit them by selecting and moving their vertices.
- Tip: the `G` key toggles GE visibility, even outside GE Mode. That is useful for modifying existing GEs.
  - Outside GE Mode you can select, delete or duplicate existing GEs.
  - You cannot **add** new GEs without entering GE Mode.

## Ledge Grab

Lets the Spy grab and hold ledges, or climb obstacles, depending on their height. The Spy can move along a ledge (corners included), pull up onto the next horizontal surface, and jump over lower obstacles. Add it in GE Mode with `Shift + L` + left-click on the edge.

### Grab heights

| Measurement | Description |
|---|---|
| **208 units** | Minimum height for the Spy to grab a ledge from below |
| **320 units** | Maximum ledge height the Spy can grab from a stationary position |
| **448 units** | Maximum ledge height the Spy can grab with a running jump. The move may be less smooth, so smaller heights are preferable. |
| **512 units** | Required height for a **co-op move**: two Spies cooperate to reach a high platform. The second Spy needs an empty space **144 units deep** on the platform. |

### Climbable obstacles and ledge surfaces

| Measurement | Description |
|---|---|
| **115 units** | Minimum height of a climbable obstacle. Below 115 the Spy simply jumps it. |
| **64 units** | Minimum surface size, from a ledge to a wall, for the Spy to pull up and stand on a platform that touches a wall |
| **32 units** | Minimum ledge length |

The "minimum sizes" diagram is image-only.

### Fence Jump

| Measurement | Description |
|---|---|
| **88-128 units** | Fence height that allows the "fence jump" move. **Recommended: 96 units.** |

## Hand-over-hand

Lets the Spy hang from horizontal pipes and move along them. Add it in GE Mode with `Shift + H` + left-click on the edge.

| Measurement | Description |
|---|---|
| **216 units** | Minimum clearance for the Spy to perform a Hand-over-Hand move |

## Pipe

Lets the Spy climb **vertical** pipes. Add it in GE Mode with `Shift + P` + left-click on the edge.

- **Rule:** move the GE vertex of the **horizontal segment** so that it points in the direction the Spy approaches from to grab the pipe.
- The example geometry is image-only (three screenshots).

### Connecting Hand-over-Hand and Pipe GEs

Horizontal and vertical pipes can be connected. **Snap the vertices to the same position** where a Pipe GE meets a Hand-over-Hand GE. The example is image-only.

## Ladder

Lets **Spies and Mercenaries** climb ladders. Add it in GE Mode with `Shift + E` + left-click on the edge.

- **Rule:** Spies climb the bars two by two, so the **number of bars must be a multiple of 2, with a minimum of 8 bars**.
- Three example screenshots are image-only.

## Zipline

Lets Spies slide down sloping cables while hanging on. Add it in GE Mode with `Shift + Z` + left-click on the edge. **Place the GE on the upper edge or face of the zipline mesh.**

| Measurement | Description |
|---|---|
| **2 units** | Zipline radius |
| **384 units** | Maximum height above the floor at which a Spy can hang on the zipline |
| **198 units** | Minimum clearance below the zipline for a Spy to hang |
| **161 units** | Minimum clearance below the zipline when the Spy is tucked |

### Connecting Pipe and Zipline GEs

Vertical pipes connect to ziplines by **snapping vertices to the same position** where the Pipe GE meets the Zipline GE.

## Fence

Lets Spies climb across the top of a fence.

### Making a fence climbable

**The texture's flag makes a surface climbable, not the GE itself.**
1. Right-click the texture and choose `Properties`.
2. In the `Echelon` category, set `bClimbable` to `1`.
3. Re-save the texture package.

### Placing a Fence GE

Enter GE Mode, then `Shift + F` + left-click on the edge **at the top of the fence**.

## GE Activation Masks

Activation masks enable or disable GEs during play. Use them when the level's geometry changes: an object is destroyed or moved, or becomes climbable later in the match.

- A dynamic GE needs a **GE ID**: right-click the GE and hover over `Set GE ID` in the context menu.
- Example: give the initial set of GEs ID `1`. When the object is destroyed, disable those GEs and enable another set with a different ID, such as `2`.

### SGETrigger

An actor that enables or disables GE activation masks during play.
- `bToggle`:
  - `True` toggles the GE mask on and off each time the trigger fires.
  - `False` always **enables** the GE mask when it fires.
- `GEMask`: the GE activation mask the trigger affects. It **must match the GE ID** assigned with `Set GE ID`. The trigger affects every GE using that mask.

The property sheet is image-only.

### Level Properties

Open the level properties (`F6`, or right-click empty space → `Level Properties`).
- `GEActivationMask`: which GE mask is active when the level begins. **GEs with no ID assigned are always enabled.**

## How the MCP tools apply this

`geometric_events` (spec `ges[{kind, points}]`, built by `level_build`) runs the editor's own `GE ADD <command> X1=.. Y1=.. Z1=..` through the bridge op `ge.add`.

Kinds and the `GE ADD` command each uses:

| Spec kind | `GE ADD` command | Points |
|---|---|---|
| `ledge` | `LG` | 2 (along the edge) |
| `hoh` | `HOH` | 2 |
| `pipe` | `PIPE` | 3 (the third is the side the player climbs from) |
| `ladder` | `LADDER` | 3 (the third is the side the player climbs from) |
| `zipline` | `ZL` | 2 |
| `fence` | `FENCE` | 2 |
| `pole` | `POLE` | 2 (RE+ Map Design element; not covered by the wiki) |

Point ordering:
- The tool puts pipes and ladders **lower end first**, with the side point at that end's height, as the stock maps do.
- The game ignores a pipe or ladder whose side point is anywhere else.

Masks:
- `geTriggers[{id, toggle, geMask}]` creates `SGETrigger`s.
- The starting mask is the Level Properties `GEActivationMask`, written by `map_settings`.
- **Deleting is backed:** `ge.delete` removes the geometric event currently **selected in GE mode** (select it there first) in one Undo step — call it via `workflow_request` (op `ge.delete`). The stock editor exposes no console command to select a specific GE or to list all of them, so which GE gets deleted stays a GE-mode choice.
- Assigning GE IDs is not backed: it is the GE-mode / right-click `Set GE ID` action only; do it by hand.

Fences:
- A fence also needs `bClimbable` on its texture. Set it with an `object.set` on the texture through `workflow_request`, or by hand, then save that (map-owned) package.

Checking:
- `map_query what:"ges"` lists GEs where the editor can report them.
- `map_validate` checks every GE against the measurements above. It uses floors traced live, or read from the blockout pieces offline.
- Rules: `ge.points`, `ge.ledge-height`, `ge.ledge-reach`, `ge.obstacle`, `ge.pull-up`, `ge.ledge-length`, `ge.fence-height`, `ge.fence-recommended`, `ge.fence-climbable`, `ge.hoh-clearance`, `ge.ladder-bars`, `ge.zipline-hang`, `ge.zipline-clearance`, `ge.zipline-slope`, `ge.point-order`
