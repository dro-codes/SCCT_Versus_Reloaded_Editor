# Map JSON: snapshots and change files

Abridged from RE+ `docs/MapAuthoring.md`. These are the formats behind **View > Export Map to JSON...** / **Import Map from JSON...** and the bridge ops `authoring.export`, `authoring.preview` and `authoring.apply`. The MCP tools write them for you: `actors_edit` and `map_changes_apply`.

Schemas:
- `scct://schema/map-changes`
- `scct://schema/map-authoring`

## What a change file can do

| Version | Adds |
|---|---|
| **1** | Create loaded actor classes, set positions/rotations and editable properties, add particle-emitter components, update existing actors (checked against their exported values), link events, resolve forward and cyclic references between new actors, and create **box** brushes/volumes (`geometry:"box"`) |
| **2** | Package loads, texture imports and package saves (run first, not undoable); brushes made of textured **polygons**; BSP **surface** texturing; `"map": "*"` for "the map that is open"; up to 20,000 operations |
| **3** | Explicit actor **deletes**, which need `"allowDeletes": true` |

What import does **not** do:
- It does not rebuild BSP, navigation or lighting. Rebuild geometry after brush changes, and lighting after static-light changes.
- It does not import mesh files.
- It does not run commands or scripts.

The export holds editable data and reference context. It is not a lossless `.sdc` replacement.

## Snapshot (`scct.map-authoring`, version 1)

| Field | Content |
|---|---|
| `map` | Destination identity. Saved maps use their canonical path; unsaved maps use a session identity. Re-export after Save As or opening another map. |
| `level` | The native object-path prefix (e.g. `MyLevel`). |
| `actors[]` | Each actor or component has these fields. Engine infrastructure (LevelInfo, builder brush, cameras) is excluded.<br>- `actor` `{path, class}`<br>- complete exposed `values`<br>- live `schema`<br>- `unavailable` properties with reasons<br>- numeric `position`, `rotation` and `selected` |
| `classes[]` | Loaded, non-abstract actor classes `{class, category, brush}` and particle components `{class, component:true}`, each with a property `schema`. Schemas give types and enum `choices`, **not class defaults**. |
| `assets` | `{Sound, Material, StaticMesh}` lists of loaded asset paths. Static meshes have local `bounds`. Load more packages and export again if an asset is missing. |
| `geometryT3d` / `geometryUnavailable` | Native T3D text of the map's actors and brushes, for spatial context. |
| `changes` | An empty change file with the correct `map` identity (`"version": 3`). |
| `instructions` | A reminder of these rules. |

Size limits:
- Exports are streamed and limited to 256 MiB. ShipD is about 42 MB, so the MCP tools never run a full export routinely.
- Change files are limited to **128 MiB**, **64 nesting levels** and **2,000 operations** (version 1; 20,000 in version 2 and later).

## Change file (`scct.map-changes`)

```json
{"format": "scct.map-changes", "version": 3, "map": "<exported map id, or * (v2+, no deletes)>",
 "description": "...", "expect": {}, "allowDeletes": false, "operations": [ ... ]}
```

- Top-level keys are exactly `format`, `version`, `map`, `description`, `operations`, `expect` and `allowDeletes`.
- **Unknown fields anywhere are rejected.**
- Keep the `map` string exactly as exported.

### Property values (every leaf is a string)

| Kind | Example |
|---|---|
| Number | `"LightBrightness": "96"` |
| Bool | `"bChained": "True"` |
| Enum | `"GameMode": "GM_Multi"` (use the live schema's `choices`) |
| Text | `"Description": "\"Neutralize the server\""`: the JSON value is a **quoted** string |
| Name (`Tag`, `Event`) | `"Tag": "ServerAlarm"`: an unquoted literal Tag |
| Struct | Every field: `"Location": {"X": "128", "Y": "96", "Z": "64"}`, `"Rotation": {"Pitch": "0", "Yaw": "16384", "Roll": "0"}` |
| Array | Replaces the whole array: `"Objectives": [{"$ref": "Obj1"}, {"$ref": "Obj2"}]` |
| Object reference | `{"$ref": "CreateId"}` for a new actor or component, or an existing actor's exact exported path. Assets keep their text form: `"Sound'Package.Group.SoundName'"` (they must be loaded). |

Coordinates and rotation:
- Coordinates are absolute Unreal units.
- Rotations use **65536 units per turn** (16384 = 90°).
- `Location` is mandatory on a create.

### Operations

**create** (`op, id, class, geometry?, polygons?, properties`)
- `id`: up to 63 letters, digits and underscores, not starting with a digit. It is unique in the file, ignoring case.
- The new actor's **Tag defaults to its id**.
- A live actor with that name or Tag blocks creation.
- `geometry`: `"point"` (default) for ordinary actors. Classes marked `brush` take `"box"`, or `"polygons"` (v2).
  - Box brushes are always 256 uu cubes, so use polygons for sized volumes.
- Omitted properties keep native class defaults. New actors may set `StaticMesh`, `DrawType`, `DrawScale` and `DrawScale3D`.

**component** (`op, id, class, owner, properties`)
- A particle emitter appended to its owner's `Emitters`.
- Do not also replace the owner's `Emitters` in the same file.

**update** (`op, actor{path, class}, before, properties`)
- `before` is the object's **complete exported `values`**.
- Any change since export rejects the update.
- Combine updates to one object into one operation.

**link** (`op, event, target, trigger, group?, delay?`)
- `trigger: true` sets the *target's* `Event` to the SMagicEvent's Tag. The target is the trigger actor.
- `trigger: false` appends an action aimed at the target's Tag to the event's group:
  - `group` defaults to 0;
  - `delay` is a **string** of 0-86400 seconds, and only valid on actions.
- An event with no groups gets its first group automatically.
- Missing receiver Tags are allocated.
- Links append after property updates, in file order.
- For Sequence/Repeat/ValidOn logic, set `Groups` explicitly instead. Group shape: `scct://schema/smagic-event`.

**load** (v2: `op, package`)
- Finds `System\<name>.u`, then `Packages\Textures\<name>.utx`, `StaticMeshes\.usx`, `Animations\.ukx`, `Sounds\.uax`/`.uas`.

**texture** (v2: `op, package, group?, name, file, format?, mips?, masked?, alphaTexture?, lodSet?`)
- `file` is `.bmp`, `.tga`, `.pcx` or `.dds`. It is relative to the change file's folder (`baseDirectory`).
- Sides must be **powers of two, ≤ 8192** (4096 is the practical size).
- `format`: `DXT1` (default), `DXT3`, `DXT5` (alpha) or `RGBA8`. **Omit it for `.dds`**, which keeps its own format and mips.
- `mips` defaults to true. `lodSet` is 0-15.
- Importing over an existing texture is rejected.

**save** (v2: `op, package, overwrite?`)
- Saves `Packages\Textures\<package>.utx`. Only a package that an earlier `load`/`texture` in the same file names can be saved.
- An existing file is replaced only with `overwrite: true`, and is backed up first.
- Save the package **before** saving a map that uses it.

**Packages over MCP.** Through `map_changes_apply` and `workflow_request` (authoring.apply or authoring.preview), a `texture` or `save` may only name the map's own `<Map>_<name>` package, such as `<Map>_TXT` (a `texture` may also go into `MyLevel`, which is saved with the map). Any other is refused before anything runs, with code refused and reason not_map_owned (a stock or other map's package), package_name (`<Map>` or `Amb_<Map>`, or a name another kind of package already has) or interface_package (`<Map>-i`, which the interface textures write), and before the map's first save every one but a `texture` into `MyLevel` is refused (unsaved_map), since those packages are named after its file. A `load` alone is always allowed.

**polygon brushes** (v2: create with `geometry:"polygons"`, `polygons:[{vertices, texture?, flags?, origin?, textureU?, textureV?, pan?}]`)
- Vertices are local to `Location`.
- Each polygon has **3-16** vertices, wound as the editor winds brush faces. Split larger faces.
- `flags` are unsigned 32-bit PolyFlags; `pan` is `[U, V]` integers.
- Up to 16,384 polygons per brush.
- CSG brushes take `CsgOper` (default `CSG_Add`) and `PolyFlags` properties.
- Convexity and closure are **not** checked, but CSG needs closed, convex brushes. Rebuild geometry afterwards.

**surface** (v2: `op, surfaces:[indices], texture`)
- Applies a material to BSP surfaces by index, as Apply Texture does.
- The indices are of the **current** BSP, so rebuild geometry first.

**delete** (v3: `op, actor{path, class}, before`)
- Needs `"allowDeletes": true` and the exported `map` (not `*`).
- `before` must equal the current values.
- At most 1,000 per file.
- LevelInfo, the builder brush, cameras and particle components are never deleted.
- An actor deleted by the file cannot be updated, linked, given a component or `$ref`'d by the same file.
- Deletes run first. The preview warns about references left dangling (`still referenced by`, `still the Event of`).

### expect

When a link or a component touches an **existing** actor, add that actor's exported values under `expect["<exact path>"]`. Stale links are then refused. An update's `before` already counts as its expectation.

## Order and failure

- Package loads, imports and saves run first, in file order, and **cannot be undone**.
- The map changes then apply in **one Undo step**, in this order:
  1. deletes
  2. creates
  3. components
  4. properties
  5. links

  Forward references therefore work in any operation order.
- Late errors roll back the whole transaction.
- Results:
  - `authoring.preview` returns `rows` (`change` Add/Modify/Move/Delete/Link/Package/Surface, `target`, `select`, `property`, `before`, `after`) and a `tally`.
  - `authoring.apply` returns `created`, `deleted` and `identities` (create id → actor identity).

## What the preview cannot judge

Placement, audibility, light quality, timing, and whether a trigger works for the intended team are **playtest** checks. Use `view_screenshot`, and play the map in game.
