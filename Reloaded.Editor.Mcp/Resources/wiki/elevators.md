# Elevators

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Elevators

Elevators are mainly handled by the **`SLift`** actor. It is a specialised `Mover` that manages floor positions, doors and switches automatically.

## SLift

Create an `SLift` like a standard mover:
1. Select a StaticMesh.
2. **Right-click** the `Mover` button in the left toolbar for more options.
3. Choose `SLift`.

`SLift` keeps the doors synchronised with each floor, and activates the correct switches automatically based on the elevator's position.

> **NOTE:** an `SLift` made from the Actor Browser with `Add SLift Here` spawns with the default actor sprite. Fix it in its properties, in the `Display` category:
> - assign a `StaticMesh` for the elevator
> - set `DrawType` to `DT_StaticMesh`

**If your level has an elevator, use `SLift`, not a standard `Mover`.** This is strongly recommended.

### Setting up the elevator

- In the `Mover` category, set a key position **for each floor**, as for a standard mover. Adjust `MoveTime` if needed.
- The `SLift` properties have two main sections, described below. The property screenshot is image-only.

#### DoorsAndSwitches

For **each floor**, add a subsection and assign two things:
- **External doors** (`Mover`).
  - External doors stay on their floor; they do not move with the elevator.
  - **Each door uses two keyframes: `Key 0` = Open, `Key 1` = Closed.** This is the opposite of an ordinary door.
  - **`KeyNum` must match the floor where the elevator starts.** Movers on the starting floor are at `Key 0`; all the others are at `Key 1`.
- **Active switches** (`SInterrupteur`).
  - Usually the switch inside the elevator plus any call switches on other floors.
  - In each `SInterrupteur`'s properties, under the `Interaction` category, set `TriggerMethode` to **`TriggerToggle`**.
  - Configure their initial usability (`bInitiallyUsable`) according to the elevator's starting floor.

| Property | Description |
|---|---|
| `Doors` | Every `Mover` needed on this floor. |
| `Switches` | The `SInterrupteur` actors that control the elevator on this floor: usually the switch inside the elevator and any call switches on other floors. |

#### LiftDoors

Inner elevator doors use the special **`SLiftDoor`** actor.
- Each has two keyframes: **`Key 0` = open, `Key 1` = closed**.
- They are linked to the `SLift`'s movement. **The elevator does not move until every inner door is closed**, which keeps the system synchronised.

## How the MCP tools apply this

`mechanisms` with `lifts[{id, shaft, storeys, start, mesh?, moveTime?, switches?}]` (spec `mechanisms.lifts`):

**With a `mesh`**, it makes an `SLift`:
- Its `StaticMesh` is set from the mesh.
- It has one key per storey (`KeyPos` offsets in Z, the `start` storey first) and `KeyNum` 0.
- `MoveTime` defaults to about rise / 160, clamped to 1.5-8 s.
- Unless `switches: false`, each floor gets one `SInterrupteur` call switch with:
  - `TriggerMethode TriggerToggle`;
  - `bInitiallyUsable` True only on the starting floor;
  - its `Event` set to the lift.
- Each switch is listed in that floor's `DoorsAndSwitches` entry.

**Without a mesh**, a two-storey lift is the Map Design brush platform (bridge op `design.lift`, 192 x 192). It rises when stood on and returns after 3 s.

**Doors are not generated.** Add them as `mechanisms.doors`, then list them in the lift's `DoorsAndSwitches` `Doors` with `actors_edit` `set`:
- External doors need `Key 0` = open and `Key 1` = closed.
- Doors on the start floor are at `Key 0`; doors on the other floors are at `Key 1`.
- Inner doors are `SLiftDoor`s (class `SLiftDoor`, two keys, open = `Key 0`).

`vertical[{kind: "lift"}]` in the geometry section cuts the shaft through each slab.

To read an elevator back, use `map_query what:"actors" classes:["SLift"] fields:"all"`. `scct://examples/elevator-3floors` is a worked spec.
