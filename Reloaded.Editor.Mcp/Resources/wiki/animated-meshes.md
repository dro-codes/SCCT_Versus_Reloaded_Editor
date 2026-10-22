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
