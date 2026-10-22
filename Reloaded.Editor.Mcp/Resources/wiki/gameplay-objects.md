# Gameplay Objects

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Gameplay-Objects

Gameplay objects are interactive actors used throughout maps. Sub-pages:
- `scct://wiki/buttons-and-hacking-panels`
- `scct://wiki/level-ingredients`

The property sheets on this page are image-only; the tables below are the text.

## SGamePlayObject

`SGamePlayObject` is the base actor for most interactive objects in levels.
- Most of its properties are optional. A simple one may need only a Static Mesh and a few properties; advanced ones use more features.
- Common uses: controlling lights, triggering events, or changing an object's look when it is triggered or damaged.

### Setting a Static Mesh

A newly added `SGamePlayObject` shows only as the actor sprite icon. To make it a Static Mesh:
1. Open its properties and go to the `Display` category.
2. Change `DrawType` from `DT_Sprite` to **`DT_StaticMesh`**.
3. Assign a `StaticMesh` in the same category. The easiest way is to select a mesh in the Static Mesh Browser and press `Use` on the `StaticMesh` field. You can also paste a mesh reference into the field.

### SGamePlayObject properties

| Property | Description |
|---|---|
| `bCanBeChaffed` | **Keep this disabled.** Enabling it lets Chaff Grenades put a security failure message on the Mercenary HUD, even on objects where chaff has no real effect. You never need to enable it by hand: if any `STriggerLight` actors are listed in `ObjectLights`, the game enables chaffing automatically at level start, so players can chaff those lights. |
| `bDamageable` | Whether the object can take damage and be destroyed. When disabled, it ignores all incoming damage. |
| `bPickable` | Lets a Spy pick up and throw the object. **The feature exists in the code but is unfinished, and shipped maps never used it. Strongly recommended not to use it.** |
| `bPushable` | Lets players push or kick the object with physics. Common for small props such as cans or other loose objects on the floor. |
| `bStickyDamageIsLikeChaff` | If enabled, Sticky Shocker damage disables the object for `StickyNeutralizationTime`. |
| `ObjectLights` | The `STriggerLight` actors this object controls. They can toggle with triggers, go out when the object is damaged, or go off during a chaff malfunction. |
| `StickyNeutralizationTime` | Seconds the object stays malfunctioning after a shock, when `bStickyDamageIsLikeChaff=True`. |
| `TriggeredMesh` | Static Mesh used when the object is triggered or malfunctions (such as from chaff). |
| `TriggeredSkin` | Material applied to the Static Mesh when the object is triggered. It replaces the material in slot `0`. |

### SGPODamages properties

The `SGPODamages` category holds the damage settings of an `SGamePlayObject`.

| Property | Description |
|---|---|
| `bCanDamageActor` | Not to be confused with `bDamageable`. When enabled, the object damages other actors when it is destroyed. |
| `DamageActorsPoints` | Damage dealt to other actors when `bCanDamageActor=True`. |
| `DamageActorRange` | Radius within which other actors take damage when the object is destroyed. |
| `DamageActorType` | Type of damage applied to those actors, chosen from a list of `SDamageType`. |
| `DamagedMeshes` | Meshes that replace the original as damage builds up (table below). |
| `DestroyOnEventFrom` | When the object receives an event from this actor, it is destroyed. |
| `HitPoints` | The object's health when it spawns. It goes down as the object takes damage (with `bDamageable=True`). |
| `OnResetEvent` | Event sent when the object resets and its damage is restored. |
| `OnResetEventType` | Type of event sent on reset. |
| `PercentDamageRefillable` | Share of the damage that can regenerate over time. For example, `0.7` means the object can recover up to 70% of the damage taken. |
| `ResetDamageTime` | Seconds for the object's damage to reset. |
| `ShakeRange` | Radius within which players feel a camera shake when the object is destroyed. |
| `SpawnableObjects` | Classes spawned when the object is destroyed, such as `SGrenadeExplosion` or particle emitters. Emitters may be shared between actors, so triggering an instanced emitter is often better. |
| `TakeDamageTypes` | When any types are listed, only those damage types hurt the object. Common examples: `SDTBase` (Mercenary weapon), `SDTSnipeShot` (Mercenary weapon while sniping), `SDTStickyShocker`, `SDamage_Explosion`, `SDamage_Charged`. |

#### DamagedMeshes (one entry per damage stage)

| Property | Description |
|---|---|
| `bDisableCollision` | If `True`, the object loses its collision when this mesh is applied. |
| `Event` | Event sent when the `Percent` condition is reached. |
| `EventType` | Type of event: `Trigger`, `Untrigger`, `TriggerUntrigger` (alternates, starting with Trigger) or `UntriggerTrigger` (alternates, starting with Untrigger). |
| `Percent` | Damage taken before this mesh shows: 50 is half damaged, 100 is destroyed. |
| `Skin` | Material applied to slot `0` while this damaged mesh shows. |
| `StaticMesh` | Static Mesh that replaces the original while this damaged mesh shows. |

### SGPOSound properties

| Property | Description |
|---|---|
| `BounceSound` | Played when the object bounces. |
| `DestroySound` | Played when the object is destroyed. |
| `TakeDamageSound` | Played when the object takes damage. |

## STriggerLight

`STriggerLight` is a **dynamic light that can be turned on or off during play**.
- It is commonly used with `SGamePlayObject` actors, for lights that can be switched, damaged or disabled.
- Unlike the standard `Light`, it is designed to respond to events and gameplay interactions.
- For how dynamic lighting works, see `scct://wiki/lighting`.

In the **Actor Class Browser** it is under:

```
Actor
└─ Light
    └─ STriggerLight
```

> **TIP:** a newly spawned STriggerLight is already a Dynamic Light. Its icon may still show **S** (Static Light) instead of **D** (Dynamic Light); the icon just has not updated. Refresh the icons with `Tools` → `Update All Light Symbols`.

| Property | Description |
|---|---|
| `AssociatedShadowActors` | Rarely used. Static Mesh actors that simulate a baked shadow while the light is on. Mainly used with UE2's Standard Dynamic Lights, which cast no shadows. |
| `bInitiallyOn` | Whether the light is on when the level begins. |
| `DistAlwaysVisibleIn` | Distance at which the light becomes always visible. |
| `DistAlwaysVisibleOut` | Distance at which the light stops being always visible. |
| `GzzzitSound` | Played when the light is disabled by damage or chaff. It does **not** play when a trigger event simply turns the light off. |

- Like `SGamePlayObject`, `STriggerLight` has a **`TriggerMethode`** property in the `Interactions` category. It sets which trigger method the light sends when triggered. See `scct://wiki/events`.
- To attach an `STriggerLight` to an `SGamePlayObject`, list it in that object's `ObjectLights`. When the object is triggered or damaged, every light in `ObjectLights` toggles with it.
