# Material Classes

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Material-Classes

SCCT Versus adds several material classes and custom material properties that stock Unreal Engine 2 does not have. The property sheets on this page are image-only; the tables below are the text.

## Echelon material properties

These sit in the **`Echelon`** category of Textures, and also appear in other material classes.

| Property | Description |
|---|---|
| `bClimbable` | Marks the surface as climbable, such as a fence or lattice. Climbing over the top **also needs a Geometric Event (GE)**. |
| `bEchelonLightThrough` | If `True`, Echelon lights shine through this material. |
| `bHideInEMFVision` | If `True`, the material is hidden in EMF Vision. |
| `bHideInThermalVision` | If `True`, the material is hidden in Thermal Vision. |
| `bNoSplitJump` | Set to `1` to stop Spies split-jumping on this surface. |
| `bNoStickyCam` | Set to `1` to stop Sticky Cams being placed on this surface. |
| `OcclusionType` | The surface's sound occlusion behaviour. |
| `SurfaceType` | The sound made when the surface is walked on or hit. |

## LitSpecularMaterial

Adds specular highlights, for a shiny or reflective surface.

| Property | Description |
|---|---|
| `Diffuse` | The standard diffuse texture for the surface. |
| `Specular` | The specular texture, usually a `TexEnvMap`, which makes the highlights. |
| `SpecularityMask` | Mask of which areas get highlights. Often the same as the diffuse texture. |
| `TwoSided` | If `True`, the material renders on both sides of a surface. |

## HeatTextureModifier

Gives a material a separate texture for **Thermal Vision**, keeping a standard material for normal vision.

| Property | Description |
|---|---|
| `HeatTexture` | The texture shown in Thermal Vision. |
| `bUseTextureAsHeat` | Leave it `True` to enable the heat texture in Thermal Vision. |
| `HeatTextureType` | `HEATTEX_Cool` or `HEATTEX_Heat`. Cool surfaces show the generic thermal base; heat surfaces show the assigned `HeatTexture`. |
| `Material` | The standard material for every other vision mode. |

## EMFTextureModifier

Gives a material a separate texture for **EMF Vision**, keeping a standard material for normal vision.

| Property | Description |
|---|---|
| `EMFIsOn` | Leave it `True` to enable the EMF texture in EMF Vision. |
| `EMFTexture` | The texture shown in EMF Vision. |
| `Material` | The standard material for every other vision mode. |

## SpecialVisionTextureModifier

Combines Thermal and EMF textures in one material: the features of `HeatTextureModifier` and `EMFTextureModifier` together.

| Property | Description |
|---|---|
| `bUseTextureAsHeat` | Leave it `True` to enable the heat texture in Thermal Vision. |
| `HeatTextureType` | `HEATTEX_Cool` or `HEATTEX_Heat`, as above. |
| `HeatTexture` | The texture shown in Thermal Vision. |
| `EMFIsOn` | Leave it `True` to enable the EMF texture in EMF Vision. |
| `bInvertEMFTexture` | Usually left `True`, so the texture's black pixels do not glow in EMF Vision. |
| `EMFTexture` | The texture shown in EMF Vision. |
| `Material` | The standard material for every other vision mode. |

## EchelonLightingMaterial

Used with Echelon Lights (see `scct://wiki/lighting`). When it is assigned, the light can project textures and animated effects.

| Property | Description |
|---|---|
| `AttenuationType` | How the projected light fades with distance: `EchAtten_None`, `EchAtten_Linear` or `EchAtten_Late`. |
| `ProjectedLightingTexture` | The texture the Echelon Light projects. It also sets the shape of the lightbeam. |
| `ProjectedEffectTexture` | An animated effect texture projected inside the lightbeam, for moving light patterns. Visible only when the light has `UsesLightBeam=True`. |

Assign it on the Light actor, in the `EchelonLighting` category, through the `EchelonLightingMaterial` property. The Lighting page's table lists this property as `EchelonLightMaterial`; use whichever name the live schema shows.

## Material operations (Texture Browser, right-click)

| Operation | Description |
|---|---|
| **Select surfaces or SM with this material** | Highlights every surface or Static Mesh in the level that uses the selected material. |
| **See materials that use this material** | Lists every material that references the selected material, to find dependencies or uses. |
| **Replace Material** | Replaces the selected material with another on every surface that uses it. |
| **Replace NULL References** | Finds surfaces or meshes that reference a missing (NULL) material and replaces those references. |

### Find and Replace Material

Replaces a material on **BSP** or on **StaticMeshes**, depending on the option chosen.
1. Select the material to replace.
2. Choose the replacement.

**Gotcha:** replacing a material on StaticMeshes **modifies the StaticMesh package, not your map**.
- Avoid doing this on original game packages.
- In one of your own packages, **re-save the StaticMesh package** afterwards.

> **NOTE:** exporting DirectDraw (`.dds`) textures from the Texture Browser **does not work** in this editor; Ubisoft broke it.

## How the MCP tools apply this

Texturing:
- Materials for blockout pieces come from the spec's `material` / `defaults.material`, which maps to `design.block` `spec.material`.
- Per-face textures come from `geometry.surfaces`, or map-changes `surface` ops after a geometry build.

Creating and setting materials:
- New textures come in with map-changes v2 `texture` + `save` ops.
- Package names must be map-owned (`<Map>_TXT`); stock packages are never written.
- Setting `bClimbable` or other Echelon flags on a texture is an object edit (bridge `object.set`, via `workflow_request`), followed by saving that map-owned package.

Constraints:
- `map_validate` reminds you that a fence GE also needs `bClimbable`.
- `.dds` export is broken in the editor, so the tools never rely on exporting textures.
- Rules: `ge.fence-climbable`, `release.package-names`
