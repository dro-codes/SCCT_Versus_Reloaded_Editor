---
name: create_level_from_file
title: Create a level from a file
description: Turn a structured file (ASCII/CSV grid, floor-plan image, SVG, Tiled, GeoJSON, CSV actor table, T3D, Hammer VMF, Quake .map, OBJ, a map export or an existing spec) into a wiki-correct SCCT Versus map.
arguments:
  - name: path
    description: The file to import.
    required: true
  - name: format
    description: The importer to use (grid, image, svg, tiled, geojson, csv, t3d, vmf, quake, obj, ase, map-authoring, map-changes, level-spec, r6v) or auto.
    default: auto
  - name: mapName
    description: The map's display name (CAPS). The file becomes its first 4 letters + D.
  - name: options
    description: Importer options as JSON, such as {"cell":128,"legend":{"#":"wall"}} for a grid or {"scale":2} for a VMF.
wiki: [index, spawn-points, game-modes, level-ingredients]
docs: [tools]
rules: false
---
# Create an SCCT Versus level from a file

**File:** `{{path}}` (format: {{format}})
{{#mapName}}**Map name:** {{mapName}}
{{/mapName}}{{#options}}**Importer options:** {{options}}
{{/options}}
The rule catalogue is not embedded, to keep this recipe short: read `scct://rules` (every check with its draft and release severity and its wiki page) before you design or fix anything.

## Recipe

1. **Connect.** Call `editor_status`. If the open map has unsaved work, ask the user before calling `map_file` with `action:"new"`.

2. **Import.** Call `import_to_spec` with `source` (the file's full path, as above) and `format:"{{format}}"`{{#options}}, and `options` {{options}}{{/options}}.
   - It returns a `scct.level-spec` plus a `report`: what each part became, what it could not map, and the scale it assumed.
   - Read the report.
   - Supported formats:

     | Format | What the importer does |
     |---|---|
     | ASCII/CSV grids | The legend `# . S M O F Z B K D V C L A H N X` marks solid, floor, spy and merc starts, terminals, flags, flag drop zones, bomb targets, disks, doors, vents, cameras, lights, ammo, infirmaries, CamNet cameras and lasers (`options.legend` changes it). `--- storey N ---` starts another floor. Floor cells become rooms and corridors by rectangle decomposition. |
     | Raster floor plans | A colour legend leads into the grid pipeline. |
     | SVG, Tiled `.tmj` / `.tmx`, GeoJSON | Shapes and layers become pieces and actors. |
     | CSV actor tables | One actor per row. |
     | T3D, Hammer `.vmf`, Quake `.map` | Brushes become polygon brushes. SCCT's world is solid, so for Hammer and Quake maps a hull is carved and the brushes are added back. |
     | OBJ | Becomes a static mesh asset list. |
     | Map-authoring export | Becomes spec sections plus `existing` refs. |

3. **Check the scale.** About 100 uu per metre; a standing player is 180 uu; doors are about 96 × 128; stair risers ≤ 35 uu.
   - A grid cell is usually 64-128 uu.
   - Hammer and Quake maps use inch-sized units: a Source player is 72 units tall and a Quake player 56. Scale them by about 2.5 (Source) or 3.2 (Quake), so a player is 180 uu.
   - Check the scale the importer's report says it assumed; adjust `options` and re-import if needed.

4. **Complete the gameplay.** Files rarely carry everything a Versus map needs. Add what is missing, following the embedded wiki:
   - At least 3 starts per team (use 4).
   - An `SMission` per mode: Story `GM_Multi` with `SObjective`s and their triggers, Disk Hunt with `SDisk`s, and Deathmatch `GM_DeathMatch`.
   - A **CamNet loop** (otherwise Mercenaries softlock).
   - `SAmmoStock` and `SInfirmary`.
   - Zones with `locationName`.
   - Map Settings.
   - Record in `provenance.notes` what came from the file and what you added.

5. **Check offline.**
   - Call `level_spec_validate`, and fix every error at its JSON pointer.
   - Call `plan_render` with the spec, and compare the PNG with the source file. For an image, view the original with `media_read_image`.

6. **Build.** Call `level_build` with `mode:"preview"`. Then call it with `mode:"apply"`, `build:"geometry"` and, for brush formats, `brushMode:"polygons"`.
   - Polygon brushes keep the source's faces exactly, but are not editable as Map Design pieces.
   - `pieces` mode turns spaces into editable rooms.

7. **Look and validate.**
   - Call `view_screenshot` with `preset` `overview`, `spawn:spy` and `spawn:merc`.
   - Call `map_validate` with `profile:"draft"`. Fix the findings by editing the spec and running `level_build` with `sync:"incremental"`, or through each finding's `fix`.

8. **Assets.** Any textures, meshes or sounds the file references must exist.
   - Import textures and static meshes with `assets_import` into map-owned packages: `<Map>_TXT`, `<Map>_STM`. Sounds are imported by hand in the Sound Browser, into `Amb_<Map>`; ask the user.
   - Or swap them for loaded stock assets (`map_query` with `what:"assets"`).

9. **Save and finish.**
   - Call `map_file` with `action:"save_as"`{{#mapName}} and `mapName:"{{mapName}}"`{{/mapName}}, then `editor_restart`, then `map_settings` with the spec's `settings`.
   - Then `map_validate` with `profile:"release"`.

10. **Report.** Tell the user what the importer mapped, what you added for gameplay, and what could not be imported.
