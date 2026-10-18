# SoftBody Actors

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/SoftBody-Actors

SoftBody actors simulate **cloth physics**, such as curtains, flags and hanging fabric.
- They use the **`DT_SoftBody`** draw type instead of rendering with a mesh. It is assigned automatically when the actor is spawned.
- Instead of a mesh, they render a **texture** on the simulated cloth surface. Assign it with the `Texture` property in the `Display` category.
- The base class is **`ESoftBodyActor`**. It is **not placeable** and is only the parent of the specific SoftBody types, such as `ESBPatchActor`.

> **IMPORTANT:** **do not duplicate SoftBody actors; always create a new one.** Duplicating makes several `ESoftBodyActor` instances reference the **same internal `ESoftBody` object**.

The property sheets are image-only; the tables below are the text.

## ESoftBodyActor properties

`Attach` is an array that attaches actors to points of the cloth grid:

| Property | Description |
|---|---|
| `AttachedActor` | The actor attached to the cloth. |
| `Hook` | Index of the cloth point the actor attaches to. The cloth is a grid of `nbU` columns by `nbV` rows. The index starts at the **top-left** point and increases across the row before moving to the next row down. |
| `TargetX` | Offset or orientation modifier for the attached actor along X. Negative values can flip the attachment direction. |
| `TargetY` | Offset or orientation modifier for the attached actor along Y. It can affect how the attached actor rotates relative to the cloth. |

Other properties:

| Property | Description |
|---|---|
| `attachFlipZ` | Flips the vertical orientation used to solve attachment constraints. Can affect how attached actors orient themselves to the cloth. |
| `attachPreferX` | Biases the attachment solver toward X. Can change how attached actors move with the cloth; the effect is usually subtle. |
| `BulletHitFactor` | Strength of the cloth's reaction to hits, such as being shot by a Mercenary or tased by a Spy. |
| `collActor` | If `True`, the SoftBody collides with actors such as players. |
| `collSound` | Played when an actor collides with the SoftBody. |
| `dragForce` | Air resistance in the simulation. Higher values damp motion faster. |
| `EnergyFactor` | How much energy the simulation keeps. Higher values can mean more movement. |
| `friction` | Friction when the cloth touches other surfaces. |
| `Gravity` | Direction and strength of gravity on the cloth. Usually only Z is used (e.g. `Z=-980`). |
| `hitDamping` | Reduces motion caused by impacts such as bullets or explosions. |
| `nbIter` | Physics iterations. **Ubisoft typically used `3`.** Higher values can improve stability but may cost a lot of performance. |
| `nbNormalizeIter` | Extra normalisation passes that stabilise the cloth. **Ubisoft typically used `2`**; changing it is rarely needed. |
| `shotSound` | Played when the SoftBody is hit, such as by a Mercenary's shot or a Spy's taser. |
| `windMax` | Maximum wind force on the cloth. |
| `windMin` | Minimum wind force on the cloth. |
| `windUPan` | Speed of the wind animation along U. |
| `windUScale` | Scale of the wind movement along the texture's U axis. |
| `windVPan` | Speed of the wind animation along V. |
| `windVScale` | Scale of the wind movement along the texture's V axis. |

## ESBPatchActor

**The most commonly used SoftBody actor.** Typically used for curtains or other hanging cloth that reacts to players, wind, weapon impacts and explosions.

| Property | Description |
|---|---|
| `FixedBottom` | Pins the bottom edge so it cannot move. |
| `FixedBottomLeft` | Pins the bottom-left corner. |
| `FixedBottomRight` | Pins the bottom-right corner. |
| `FixedLeft` | Pins the left edge. |
| `FixedRight` | Pins the right edge. |
| `FixedTop` | Pins the top edge. |
| `FixedTopLeft` | Pins the top-left corner. |
| `FixedTopRight` | Pins the top-right corner. |
| `nbU` | Number of cloth segments along the **U axis** (horizontal). |
| `nbV` | Number of cloth segments along the **V axis** (vertical). |
| `scaleU` | Number of texture repeats along U. |
| `scaleV` | Number of texture repeats along V. |
| `ULength` | Length of each U segment, in Unreal units. **Total width = `nbU x ULength`.** |
| `VLength` | Length of each V segment, in Unreal units. **Total height = `nbV x VLength`.** |

## ESBStripeDoorActor

Like `ESBPatchActor`, but it simulates **several vertical cloth strips** instead of one surface. Typically used for hanging strap-style door coverings.
- It adds no properties; it inherits everything from `ESBPatchActor`.
- The difference is that the cloth is simulated as separate vertical strips.

## ESBRopeActor

Simulates a single rope. Ubisoft did not use it in any official map, but it appears to work.

| Property | Description |
|---|---|
| `Length` | Length of each rope segment, in Unreal units. |
| `nbSeg` | Number of rope segments. For example, `nbSeg=8` with `Length=16` makes a rope 128 units long. |
| `Radius` | Thickness of the rope. |

## ESBChainActor

Simulates a chain. It inherits from `ESBRopeActor`, so it has all the rope properties above. Unlike the other SoftBody actors, which use textures, it uses a **Static Mesh for each link**.

| Property | Description |
|---|---|
| `LinkMesh` | Static mesh for the chain links. |

`nbSeg` sets how many links are generated, and `Length` the spacing between them.

## How the MCP tools apply this

`environment` with `softbody[{id, class?, at, mesh?, properties}]` (spec `environment.softbody`) places SoftBody actors.

**`class` defaults to `SoftBody.ESBPatchActor`**, the usual cloth. Set `ESBStripDoorActor`, `ESBRopeActor` or `ESBChainActor` for the others.
- The classes live in the **`SoftBody`** package: a short name gets it (`SoftBody.ESBRopeActor`), and so does an `Echelon.` guess.
- The engine names the strip door **`ESBStripDoorActor`**. The wiki's `ESBStripeDoorActor` is accepted and written as that.
- The base `ESoftBodyActor` is not placeable, and the compiler refuses it (error `softbody.class`).

**`mesh` is for `ESBChainActor` only:** it becomes the chain's `LinkMesh`, its links. The other SoftBody actors have no `LinkMesh` and draw a texture instead, so a `mesh` on them is an error (`softbody.mesh`). The SoftBody still draws itself (`DT_SoftBody`), so `StaticMesh` and `DrawType` are left alone.

The compiler passes `properties` through, so write the wiki values yourself:
- **Texture:** in `Texture`.
- **Patch size:** pick `nbU`/`nbV`, then set `ULength = width / nbU` and `VLength = height / nbV`.
- **Pinned edges:** `FixedTop` and the others.
- **Iterations:** leave `nbIter` 3 and `nbNormalizeIter` 2 unless there is a reason.

Always **create** each SoftBody actor:
- Never use `actors_edit` `copy` or the editor's Duplicate on one.
- The compiler warns (`softbody.duplicate`) when two spec entries are identical apart from their id.
- `map_validate` flags live duplicates sharing one `ESoftBody`.
- Rules: `softbody.duplicated`
