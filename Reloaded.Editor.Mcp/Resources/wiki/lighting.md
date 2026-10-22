# Lighting

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Lighting

SCCT Versus has three lighting systems:
- **Static Lights**
- **Dynamic Lights**
- shadow-casting **Echelon Lights**

Each behaves differently and costs a different amount of performance. Light icons are image-only on the wiki.

## Static Lights

Static Lights are the primary lighting tool in most levels.
- They give high-quality **baked** lighting: lightmaps for BSP, and vertex lighting for static meshes.
- The lighting is pre-calculated, so it costs very little performance.
- They **cannot be turned on or off during play**, and **do not update when geometry or actors move**.

To add one, right-click in the viewport and choose `Add Light Here`. The property sheet is image-only.

### LightColor properties

| Property | Description |
|---|---|
| `LightBrightness` | Overall intensity of the light. |
| `LightHue` | Colour. Hue cycles through the spectrum: **0 = red, 80 = green, 160 = blue, 255 = red again**. |
| `LightSaturation` | Strength of the colour. **Lower values give stronger tinting.** |

### Lighting properties

| Property | Description |
|---|---|
| `bActorShadows` | Enables the projector shadow cast by the Spy and the Mercenary. |
| `bAffectOwnZoneOnly` | The light affects only actors in its own zone. |
| `bApplyToInGameLighting` | The light affects gameplay actors: anything other than `StaticMeshActors`, which use baked lighting by default through `bStaticLighting=True`. |
| `bApplyToStaticLighting` | The light contributes to baked lighting for BSP and `StaticMeshActors`. |
| `bCorona` | Shows a glowing corona, using the light's skin texture. |
| `bCoronaUsesOrientation` | The corona shows only from one direction, not from every angle. |
| `bDynamicLight` | Makes the light dynamic, so it updates during play. |
| `bSpecialLit` | The light affects only surfaces or meshes marked Special Lit. |
| `LightCone` | Cone angle for `LE_Spotlight`. Larger values widen the beam. |
| `LightEffect` | The light's spatial effect (below). |
| `LightRadius` | How far the light reaches. Larger values light a bigger area. |

### Light icons

The icons are image-only.
- **S**: static lighting (`bApplyToStaticLighting=True`).
- **G**: in-game lighting (`bApplyToInGameLighting=True`).
- **SG**: both.

### Light Effect

These are the only `LightEffect` values to use. The others are legacy effects from earlier Unreal Engine builds that **no longer work correctly**; avoid them.

| `LightEffect` | Description |
|---|---|
| `LE_None` | Standard omni-directional light. |
| `LE_Spotlight` | Directional projector-style light. |
| `LE_Sunlight` | Parallel directional light, used for sunlight. |

> **NOTE:** `LE_Sunlight` is a parallel directional light that originates from a **Fake Backdrop** material. **In a zone without a Fake Backdrop surface, sunlight does not work.**

## Dynamic Lights

SCCT Versus supports two kinds of dynamic lighting:
- **Standard Dynamic Lights**: basic dynamic lights that do not cast shadows.
- **Echelon Lights**: a custom Ubisoft system with dynamic shadow casting.

### Standard Dynamic Lights

- Make a light dynamic with `bDynamicLight=True` in the `Lighting` category.
- They update in real time and **do not cast shadows**.
- They **pass through walls and floors** up to their `LightRadius`.
- Many of them cost performance.

> **TIP:** to keep a dynamic light out of other spaces, set `bAffectOwnZoneOnly=True` and **split the level into zones**. The light then cannot pass through walls or floors into other zones.

### Echelon Lights

The signature shadow-casting lights of SCCT Versus.
- Create one with **`LightEffect=LE_ESpotShadow`** in the `Lighting` category.
  - They are inherently dynamic, so `bDynamicLight` can be ignored.
  - With this effect the standard `Lighting` properties are **no longer used**. Everything is set in the **`EchelonLighting`** category.

#### EchelonLighting properties

| Property | Description |
|---|---|
| `EchelonBias` | Z-bias that corrects shadow artefacts. Usually found by trial and error. |
| `EchelonLightMaterial` | An `EchelonLightingMaterial` that sets how this light behaves and lets you assign projected textures. The Material Classes page calls the light's property `EchelonLightingMaterial`. |
| `EchelonNormalInfluence` | How strongly surface normals affect the lighting. `1` makes orientation very important; lower values give more even lighting. **`0.9` is a common setting.** |
| `EchelonRange` | The farthest distance the light reaches. **Larger ranges cost much more performance.** |
| `NearPlaneRatio` | Ratio between the light source's size and the projection area. `1` works well for sunlight through windows; `0.05` is typical for small projector-style lights. |
| `ShadowZBufferHeightType` | Shadow map resolution for height. Higher values improve shadows but cost memory and performance. |
| `ShadowZBufferWidthType` | Shadow map resolution for width. Behaves like the height setting. |
| `SpotHeight` | Height of the lit area, in centimetres. |
| `SpotWidth` | Width of the lit area, in centimetres. |

> **IMPORTANT:** Echelon Lights do **not** affect objects whose textures have an **alpha channel**.

> **TIP:** to list every Echelon Light, open `Search for Actors` from the top toolbar and tick `See Echelon Lights Only`.

#### Lightbeam (Echelon Lights only; `LightBeam` category)

| Property | Description |
|---|---|
| `UsesLightBeam` | Enables lightbeam projection. Set to `True` to activate. |
| `LightBeamMesh` | Static mesh used for the beam. The default is `TestMapStaticM.Accessories.LightBeam`. |
| `LightBeamInitialAlpha` | Starting transparency of the beam: `0` invisible, `255` fully opaque. |
| `LightBeamFalloffScale` | How fast the beam fades with distance. `0.5` means it has faded out at 50% of `EchelonRange`. |

## Unlit

Unlit is not a light type. It is a property of a surface or actor. An Unlit surface or actor appears **fully bright** and ignores all lighting.
- On BSP surfaces: `Unlit`, under the **Flags** tab of `Surface Properties`.
- On actors such as `StaticMeshActor`: `bUnlit`, in the `Display` category.

## Coronas

A corona is a 2D texture placed on the light source. A regular sprite shrinks with distance; a corona **stays the same size on screen**. Use coronas for subtle lighting effects and atmosphere.

To give a light a corona:
1. In the light's `Lighting` category, set `bCorona=True`.
2. In the `Display` category, under `Skins`, press `Add` to create `[0]`.
3. With `[0]` selected, choose a texture in the Texture Browser.
4. Back in Light Properties, make sure `[0]` is selected and press `Use` to assign the texture.

| Property | Description |
|---|---|
| `CoronaRotation` | How fast the corona rotates as the camera moves toward or away from it. Above `1.0` is faster; below `1.0` is subtler. |
| `CoronaRotationOffset` | The corona's initial rotation, in Unreal rotation units: `65536` is a full circle, so `180° = 32768` and `45° = 16384`. The wiki prints this pair as written; 45° is in fact 8192, and 16384 is 90°. |
| `MaxCoronaSize` | Largest size the corona reaches as the camera moves away. |
| `MinCoronaSize` | Smallest size the corona shrinks to as the camera approaches. |
| `UseOwnFinalBlend` | Lets the corona use a custom FinalBlend material: set it to `True` and assign the FinalBlend to `Skins[0]`. |

## Lightmaps

- Open the surface's settings: right-click it → `Surface Properties`, or press `F5` with the surface selected. Go to the **`Pan/Rot/Scale`** tab.
- The Lightmaps section has a **PS2** value and an **Xbox** value. **Ignore PS2: the PC version uses the Xbox value.**
- **Lower Xbox values give higher-resolution lightmaps** after a lighting rebuild.
  - The default **`32.0`** is enough for most surfaces.
  - Lower it for more detail. The highest resolution is **`1.0`**.
  - Preview by rebuilding the lighting.
- `Select Matching Surfs` selects every surface with the same lightmap value.

## Light operations (Ubisoft UnrealEd additions)

- **Select Lights** (`Edit` → `Select Lights...`): lists every light, with flags to filter by type.
  - `None` lists lights with no lighting enabled. These are invalid and should be removed or fixed.
  - Tick lights and press `Select` to select them.
- **Modify Selected Lights** (`Tools` → `Modify Selected Lights`): adds to or multiplies the selected lights' brightness, saturation or radius.
- **Duplicate Selected Lights as In-Game Lights** (`Tools` → `Duplicate Selected Lights as In-Game Lights`): duplicates the lights as In-Game Lights, applying the same add or multiply adjustments.
