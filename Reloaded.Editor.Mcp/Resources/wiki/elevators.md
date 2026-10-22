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
