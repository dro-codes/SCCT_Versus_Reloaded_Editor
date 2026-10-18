# Movers

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Movers

A `Mover` is a special `StaticMeshActor` that can **move during gameplay**. Movers are used for:
- doors
- moving platforms
- rotating fans
- other animated level elements

A mover follows a path of **keyframes**. Each keyframe stores the mover's position and rotation. When triggered during play, the mover travels between these keyframes.

Making a mover takes three steps:
1. Create it from a static mesh.
2. Define its keyframes.
3. Configure how it is triggered.

## Creating a mover

Movers **must use a static mesh** for their look.
1. In the **Static Mesh Browser**, select the mesh.
2. Place the Builder Brush where the mover should appear.
3. Click `Add Mover` in the left toolbar.

In wireframe view a mover's outline is **purple**; regular static meshes are **cyan**.

> **NOTE:** right-click the `Add Mover` button to list all the types and choose one: `Mover`, `MoverRotating`, `SLift` or `SLiftDoor`. The menu screenshot is image-only.

## Setting keyframes

Every mover starts with **`Key 0`**, its default position. A door mover usually uses:
- `Key 0`: door closed
- `Key 1`: door open

Creating keys:
1. Put the mover at its starting position. This is `Key 0`.
2. Right-click the mover in the viewport, hover over the **Mover** submenu, and choose `Key 1`.
3. Move or rotate the mover to the `Key 1` position.
4. **Set the mover back to `Key 0` when you are done.** Do not forget this step.

The editor records each keyframe's exact position and **total** rotation. If you rotate the mover several times while positioning it, the full rotation is kept. Create further keys the same way: select another key and move the actor.

## Common mover properties

| Property | Description |
|---|---|
| `KeyNum` | The keyframe being viewed or edited. Changing it moves the mover to that stored position, the same as picking the key from the right-click menu. |
| `NumKeys` | The total number of keyframes the mover uses. Keys beyond this number are ignored. |
| `MoveTime` | Seconds to travel between keyframes. |
| `StayOpenTime` | How long the mover stays open before returning to its previous position. |
| `bTriggerOnceOnly` | If `True`, the mover activates only once. |
| `MoverEncroachType` | What the mover does when something blocks its movement (below). |

### MoverEncroachType

| Value | Behaviour |
|---|---|
| `ME_StopWhenEncroach` | Stops moving when something blocks it. |
| `ME_ReturnWhenEncroach` | When it hits something, returns to the previous keyframe. |
| `ME_CrushWhenEncroach` | Keeps moving and crushes whatever blocks it, **instantly killing players**. |
| `ME_WaitAndRetryWhenEncroach` | When it hits something, returns to the previous keyframe and tries again until the obstruction clears. |

> **TIP:** for doors, `ME_WaitAndRetryWhenEncroach` is usually the best default. It stops a player bumping a door so that it stays open until someone re-triggers it. Instead, the door tries to close again on its own.

### Mover sounds

| Property | Description |
|---|---|
| `ClosedSound` | Played when the mover finishes closing and arrives at its last keyframe. |
| `ClosingSound` | Played when the mover starts closing toward its last keyframe. |
| `LoopSound` | A sound that loops continuously. |
| `MoveAmbientSound` | Optional ambient sound while the mover moves. |
| `OpenedSound` | Played when the mover finishes opening and arrives at its first keyframe. |
| `OpeningSound` | Played when the mover starts opening from its first keyframe. |

### Mover events

- **Closing** means moving toward `Key 0`.
- **Opening** means moving from `Key 0` to another keyframe.
- `Closing`/`Closed` and `Opening`/`Opened` differ only in the moment the event fires.

| Property | Description |
|---|---|
| `ClosedEvent` | Sent when the mover is fully closed. |
| `ClosingEvent` | Sent when the mover starts closing. |
| `LoopEvent` | Sent when the mover loops. |
| `OpenedEvent` | Sent when the mover is fully open. |
| `OpeningEvent` | Sent when the mover starts opening. |

## Trigger behaviour: `InitialState`

`InitialState`, in the **Object** category, sets how the mover reacts to trigger events.

| Value | Behaviour |
|---|---|
| `TriggerControl` | Trigger opens the mover, Untrigger closes it. |
| `TriggerToggle` | Each Trigger swaps the mover between open and closed. |
| `TriggerOpenTimed` | A Trigger opens the mover, which closes by itself after `StayOpenTime`. |
| `TriggerCloseTimed` | A Trigger closes the mover, which opens by itself after `StayOpenTime`. |
| `TriggerOpenOnceOnly` | A Trigger opens the mover once, and it stays open for the rest of the game. |
| `TriggerCloseOnceOnly` | A Trigger closes the mover once, and it stays closed for the rest of the game. |

> **TIP:** for doors, `TriggerOpenTimed` is usually the best default. The door opens when triggered and closes after the `StayOpenTime` delay.

## SPresenceTrigger

A set-up mover still needs something to trigger it. A common choice is an **`SPresenceTrigger`**: an invisible actor with a **cylindrical** detection area. When a player enters the area, it sends an event, which can open the door.

| Property | Description |
|---|---|
| `bDetectedGrab` | Applies when the trigger detects only Mercenaries. Normally a Spy can grab a Mercenary and drag them into the trigger to activate it. With `False`, grabbed Mercenaries do not activate it. |
| `bUntrigger` | If `True`, the trigger sends an **Untrigger** instead of a Trigger. |
| `DetectedClass` | Who activates it: Spies (`SPawnAttaque`), Mercenaries (`SPawnDefense`) or both (`SBasePawn`). |

Collision:

| Property | Description |
|---|---|
| `CollisionRadius` | Radius of the trigger's collision area. |
| `CollisionHeight` | Height of the trigger's collision area. |

> **TIP:** to see the radius and height, right-click the top of the viewport, hover over `Actors` and choose `Radii View`. A selected `SPresenceTrigger` then shows its collision as a red outline.

## Special Behaviors

Special Behaviors let a mover respond differently depending on **which actor triggered it**, without changing its default behaviour.

| Property | Description |
|---|---|
| `EventFromActor` | When this actor triggers the mover, the mover uses the associated Special Behavior settings instead of the defaults. |

Example: a hacking panel or button (`SInterrupteur`) could keep a door open longer than the same door opened by an `SPresenceTrigger`. The rest of the Special Behaviors struct is image-only.

## How the MCP tools apply this

`mechanisms` (spec `mechanisms.doors` / `movers` / `buttons`, built by `level_build`) creates movers from a static mesh.

Doors:
- `Key 0` is closed. `Key 1` comes from `open:{offset}` or `{yaw}`, written as `KeyPos`/`KeyRot`.
- `NumKeys` is 2.
- `MoveTime` and `StayOpenTime` come from the spec.
- RE+ uses `InitialState TriggerOpenTimed` and `MoverEncroachType ME_WaitAndRetryWhenEncroach` by default, and leaves the mover at `Key 0`.

Triggers (`trigger:{kind}`):
- `presence`: an `SPresenceTrigger` whose `Event` is the door's Tag, with `CollisionRadius`/`CollisionHeight`.
- `button` or `panel`: an `SInterrupteur`. See `scct://wiki/buttons-and-hacking-panels`.
- `event`: any Tag.

Doors a story sector opens (`gameplay.story.sectors[].actions` `{kind: "open", target, hold}`):
- With `hold` (the default) the door becomes `TriggerToggle`, so the sector's one Trigger leaves it open.
- **Every other Trigger would toggle it shut again.** So a held door that something else also opens is an error (`story.action-door-trigger`): its own `presence`, `button` or `panel` trigger, a button's `targets`, an `events.links` link, an alarm's `outputs`, or another sector that holds it open.
  - Give that door `trigger:{kind:"event"}`, so only the sector opens it, and the other team a door of its own (`three-modes-sectors` puts a merc hatch beside each shutter).
  - Or set `hold: false`: the door then opens for its `stayOpen` time and stays `TriggerOpenTimed`.

Locks and lifts:
- An alarm's `lockMovers` puts a door into `MoversToLock`.
- Elevators use `lifts` (see `scct://wiki/elevators`).

Static-mesh movers only: brush movers are out of scope (spike S10).

- Rules: `movers.static-mesh`, `events.dangling`
