# Animated Meshes

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Animated-Meshes

`SAnimatedMesh` plays animations on a mesh.

**Limitation:** Ubisoft did not include the **Animation Browser** in the release version of UnrealEd, which limits importing or managing new animations. You can still **reuse animated meshes that already exist in Ubisoft's packages or maps**. This page covers working with those existing assets.

The property sheet is image-only; the table below is the text.

| Property | Description |
|---|---|
| `ActorsToAttach` | Actors to attach to this animated mesh. |
| `Animations` | List of animation names the mesh plays. |
| `bAutoStart` | If `True`, the animation sequence starts automatically when the level begins. |
| `bChainedAnims` | If `True`, the animations in `Animations` play one after another. |
| `LoopStatus_0forInfinite` | Array matching the `Animations` array: `0` loops that animation forever, `1` plays it once. |

## How the MCP tools apply this

`environment` with `animated[{id, at, mesh?, properties}]` (spec `environment.animated`) places an `SAnimatedMesh`. The class is the default.

The compiler passes `properties` through, so write the values yourself:
- **Mesh:** the animated mesh must come from an existing Ubisoft package, loaded first. Use a map-changes v2 `load` op in the spec's `extra.operations`, or check `map_query what:"assets"`.
- **Animations:** write `Animations` and `LoopStatus_0forInfinite` as matching arrays, one entry per animation.

No tool imports new animations, because the Animation Browser is missing.
