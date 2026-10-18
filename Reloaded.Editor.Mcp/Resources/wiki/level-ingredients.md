# Level Ingredients

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Level-Ingredients

Level ingredients are predefined, reusable objects found throughout levels.
- Some are **essential for a playable map**: `SCamNetwork` is critical.
- Others add optional interactivity.

The property sheets are image-only; the tables below are the text.

## SAmmoStock

The ammo box Mercenaries use to refill their weapon. Place one and it is ready to use. The only properties you may want to adjust are in the `Interactions` category:

| Property | Description |
|---|---|
| `bInteractInFront` | Restricts interaction to the front of the object. Useful against the prompt showing through a wall when the box stands right against one. |
| `fInteractionHeight` | Height of the prompt. **Recommended for `SAmmoStock`: `60.0`.** Enhanced maps try to stay consistent across levels. |
| `fInteractionRadius` | Distance at which the prompt becomes available. **Recommended for `SAmmoStock`: `120.0`.** Enhanced maps try to stay consistent across levels. |

## SInfirmary

The medical kit that restores health for **both Spies and Mercenaries**. Place one and it is nearly ready to use.

> **By default `SInfirmary` has no collision. Enabling it is strongly recommended:** in the `Collision` category, set `bBlockActors=True` and `bBlockPlayers=True`.

| Property | Description |
|---|---|
| `bInteractInFront` | Recommended. Medkits are typically on walls and should not be usable from behind. |
| `fInteractionHeight` | Height of the prompt. **Recommended for `SInfirmary`: `120.0`.** Enhanced maps try to stay consistent across levels. |
| `fInteractionRadius` | Distance at which the prompt becomes available. **Recommended for `SInfirmary`: `60.0`.** Enhanced maps try to stay consistent across levels. |

## SCamNetwork

Serves the Mercenary team's **Camera Network (Cam. Net.)** gadget, which views a set of cameras around the map.

> **CRITICAL:** include **at least one `SCamNetwork`** in your map. Without any cameras, **Mercenaries who use the gadget are softlocked.**

In the Actor Class Browser it is under:

```
Actor
└─ SBasicGamePlayObject
    └─ SOrientableObject
        └─ SStickyCam
            └─ SCamNetwork
```

> **By default `SCamNetwork` has no collision. Enabling it is strongly recommended:** in the `Collision` category, set `bBlockActors=True` and `bBlockPlayers=True`.

| Property | Description |
|---|---|
| `bFirstCam` | If `True`, this camera is shown first when a Mercenary enters the gadget. **Only one camera on the map should have it set.** |
| `CamName` | Name on the Mercenary HUD for this camera. Usually the room or area it covers. |
| `NextCam` | The `SCamNetwork` that comes next. The "Next Camera" key switches to it. |
| `PrevCam` | The `SCamNetwork` that comes before. The "Previous Camera" key switches to it. |

> **TIP:** make the camera network a **linear loop**. With three cameras `SCamNetwork1`, `SCamNetwork2` and `SCamNetwork3`:
> - `SCamNetwork1`: `NextCam=SCamNetwork2`, `PrevCam=SCamNetwork3`
> - `SCamNetwork2`: `NextCam=SCamNetwork3`, `PrevCam=SCamNetwork1`
> - `SCamNetwork3`: `NextCam=SCamNetwork1`, `PrevCam=SCamNetwork2`

### Adjusting constraints

By default `SCamNetwork` cameras rotate freely, which can allow unrealistic movement for wall-mounted cameras. Limit the rotation with the vectors in the `Constraint` category:

| Constraint | Description |
|---|---|
| `PitchConstraint` | Limits up/down rotation. **`Y` = minimum angle, `Z` = maximum angle.** |
| `RollConstraint` | Usually `0.0`, and can be ignored, since cameras typically do not roll. |
| `YawConstraint` | Limits left/right rotation. **`Y` = minimum angle, `Z` = maximum angle.** |

These keep a camera from full 360° spins or movement beyond its mount. For example, a wall camera can be limited to look up/down and left/right within its range.

## SVitre

`SVitre` (French for "window") is a **breakable glass window**.
- Place it where the window goes, and resize it with `DrawScale` or `DrawScale3D` in the `Display` category if needed.
- Its `StaticMesh` can be replaced. For more customisation, a generic `SGamePlayObject` may be better: `SVitre` always spawns the same destruction emitter, which was designed for its default mesh.

## SExtincteur

A **fire extinguisher**. When damaged, it releases a spray that temporarily obscures the Mercenaries' vision. Place it; **no properties need adjusting**.

## SProjoFixe

A **spotlight** that Mercenaries steer left, right, up or down, like an `SCamNetwork`. In the Actor Class Browser:

```
Actor
└─ SBasicGamePlayObject
    └─ SOrientableObject
        └─ SProjoFixe
```

- Using it spawns an **Echelon Light** that lights the area. Configure that light in the `EchelonLighting` category of `SProjoFixe` (see `scct://wiki/lighting`).
- Constraints work as on `SCamNetwork`: limit rotation to prevent full 360° movement.

### Persistent Light Mode (added by SCCT Versus Reloaded)

`SProjoFixe` can keep its Echelon Light on even when no Mercenary is using it.
- **Enable it with `HitPoints=1` in the `SGPODamages` category.**
- Limitation: when a Mercenary enters the interaction radius, the light must turn off briefly so the interaction prompt can appear.

| Property | Description |
|---|---|
| `CameraOffset` | XYZ offset applied while a Mercenary uses the spotlight. Adjust it so the view feels first-person from the light itself. |

Any `StaticMesh` works. As a placeholder, Ubisoft used **`Missile.Bunker.Bunk_Bunk_Projo`** with **`CameraOffset` `X=35.0, Y=0.0, Z=100.0`**.

## SSprinkler

A sprinkler. It is **off by default**; an event activates it.

> It spawns as a default sprite, which shows in game unless hidden: in the `Display` category, set **`bHidden=True`**.

| Property | Description |
|---|---|
| `Duration` | Seconds the sprinkler stays on when triggered. |
| `WetVolumeHeight` | Height of the cylindrical volume where the puddles and rain effect appear. |
| `WetVolumeRadius` | Radius of that cylinder. |
| `WetVolumeZOffset` | Z offset of the volume from the actor icon. The actor's position is where the sprinkler effect spawns. The volume is where puddles (`SFlaque`) appear, and where Mercenaries standing in it see water droplets. |

By default sprinklers **can be damaged, which also activates them**. To prevent that, set `bDamageable=False` in the `SGamePlayObject` category.

## SFlaque

`SFlaque` (French for "puddle") is a predefined puddle, mainly used by an activated `SSprinkler`.
- It makes splashes, and **electrocutes anyone standing on it when a Spy tases it**.
- Any BSP surface or static mesh material whose `SurfaceType` is `SURFACE_MediumWater` behaves the same way, like an `SFlaque`.
- Place it where the puddle goes, and resize it with `DrawScale` / `DrawScale3D` in `Display` if needed.

## SBallTrap

Probably not something a typical map uses. When an event activates it, it starts throwing a projectile class, such as frag grenades.

| Property | Description |
|---|---|
| `fFireRate` | How often the trap fires while active. |
| `iVelocityMax` | Highest velocity the projectile is thrown at. |
| `iVelocityMin` | Lowest velocity the projectile is thrown at. |
| `ProjectileClass` | The type of projectile thrown. |

## How the MCP tools apply this

**CamNet** (`security` `camnet[{id, camName, at, first, constraints}]`, spec `security.camnet`):
- At least one camera is required.
- Exactly one `bFirstCam True` (the `first` flag, or the first listed).
- `NextCam`/`PrevCam` wired as a **closed loop** in list order.
- `bBlockActors`/`bBlockPlayers` `True`.
- `constraints:{pitch:[min,max], yaw:[min,max]}` written to `PitchConstraint`/`YawConstraint` (`Y` = min, `Z` = max).
- `map_query what:"cameras"` reads the loop back.

**Ingredients** (`environment` `ingredients[{kind, in|at, props?}]`; `props` adds any other property):

| `kind` | Class | Defaults applied |
|---|---|---|
| `ammo` | `SAmmoStock` | interaction 60 high / 120 radius, `bInteractInFront` |
| `infirmary` | `SInfirmary` | `bBlockActors`/`bBlockPlayers` `True`, interaction 120 / 60, `bInteractInFront` |
| `glass` | `SVitre` | |
| `extinguisher` | `SExtincteur` | |
| `projector` | `SProjoFixe` | none; pass `props: {"HitPoints": "1"}` for the Persistent Light Mode |
| `sprinkler` | `SSprinkler` | `bHidden` `True` |
| `puddle` | `SFlaque` | |
| `balltrap` | `SBallTrap` | |

- Rules: `camnet.missing`, `camnet.first-cam`, `camnet.loop`, `ingredients.collision`, `ingredients.interaction`, `ingredients.sprinkler-hidden`
