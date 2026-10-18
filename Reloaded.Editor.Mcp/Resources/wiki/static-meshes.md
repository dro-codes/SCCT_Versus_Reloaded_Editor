# Static Meshes

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Static-Meshes

Static Meshes are 3D models, usually made in external modelling tools such as 3ds Max or Blender and then imported into the Unreal Editor.
- They are stored in video memory, which makes them very efficient.
- They can be moved, rotated and scaled.
- They support more complex geometry than other types of object.

## Static Mesh Browser

- The **Static Mesh Browser** imports Static Meshes in the **ASCII Scene Export (ASE)** format.
- To export an imported mesh, select it in its package and choose `Export ASE`.
- ASE is an older standard, so some software does not export it by default:
  - Blender needs an add-on: DarklightGames `io_scene_ase`, https://github.com/DarklightGames/io_scene_ase.
  - Newer versions of 3ds Max still support ASE export.

## Convert to Static Mesh

`Convert to Static Mesh` turns **additive** BSP geometry into Static Meshes.
- **Subtractive brushes cannot be converted.**
- Several selected Static Meshes can also be combined into a single mesh.

Steps:
1. Select one or more brushes or Static Meshes in the map.
2. Right-click a selected item, hover over `Convert` and choose `To Static Mesh`.
3. The **Convert to Static Mesh** dialog appears. It is image-only; its fields are described below.

**New Static Mesh**

| Field | Description |
|---|---|
| `Package` | The package the new Static Mesh is saved in. |
| `Group` | Optional group name. Leave it blank for no group. |
| `Name` | The new Static Mesh's name. It **must be unique across objects, packages and groups**. |

**Prefab**

| Option | Description |
|---|---|
| `Create Prefab for Selection` | Makes a Prefab from the selected objects, so the operation can be undone later. The Prefab needs its own unique `Package`, `Group` and `Name`. |

**Collision Objects**

| Option | Description |
|---|---|
| `Group collision objects from selection` | Groups the collision objects of all selected meshes into a single Collision Object. Avoid self-intersecting meshes, which cause collision problems. |
| `Set 1 selected actor as the collision object` | One mesh acts as the collision object; the others are ignored. |
| `Set 1st face of selection as the collision object` | The first selected face is the collision object. |
| `Set grouped bounding box as the collision object` | A bounding box around all selected meshes is the collision object. |
| `No collision object` | Every mesh is used as collision, which is **worse for performance**. |

> **TIP:** keeping geometry as BSP can be the better choice: BSP gets better **lightmaps** than Static Mesh vertex lighting.

## Select Static Meshes

`Edit` → `Select Static Meshes...` opens a dialog listing every Static Mesh in the map. The search bar filters by Static Mesh name.

## How the MCP tools apply this

Placing meshes:
- `environment` / spec `geometry.meshes[{id, mesh: "StaticMesh'Pkg.Grp.Name'", at, yaw, scale, fit: "floor"}]` places `StaticMeshActor`s.
- The mesh's local bounds, from `map_query what:"assets" kind:"staticmesh"`, are used to sit it on the floor.
- Movers, `SGamePlayObject`s and tutorial paths take a `StaticMesh` the same way.

Importing:
- `assets_import` brings OBJ/ASE models in as static mesh assets in a map-owned `<Map>_STM` package.
- Converting brushes to static meshes (Convert to Static Mesh) is a manual editor step (spike S7). The tools report the steps instead.

Caution: replacing materials on stock StaticMesh packages modifies those packages. The release checks refuse stock package writes.
- Rules: `release.package-names`, `release.naming-convention`
