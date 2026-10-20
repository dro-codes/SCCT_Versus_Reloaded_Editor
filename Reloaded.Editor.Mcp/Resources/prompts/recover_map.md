---
name: recover_map
title: Recover a compiled map
description: Turn a compiled SCCT Versus map (a .sdc in Packages\Maps) into an editable source map with File > Recover Compiled Map, read its Recovery Report and get it ready for more Versus gameplay.
arguments:
  - name: source
    description: The compiled map, such as "OffsD" (Packages\Maps\OffsD.sdc) or a full path to a .sdc.
    required: true
  - name: name
    description: The editable map's file name in Packages\MapsEd. Default <source>_Recovered, as the editor's menu names it.
  - name: merge
    description: true or false. Merge the compiled BSP's fragments into fewer brushes; the default is the editor's File > Merge Recovered Geometry setting.
  - name: keepXboxOnly
    description: true to keep the Xbox-only actors as common content instead of leaving them out.
  - name: goal
    description: What the user wants to do with the recovered map, such as "add Disk Hunt" or "fix the vents".
wiki: [index, map-packaging]
docs: [tools]
rules: false
---
# Recover a compiled SCCT Versus map

**Compiled map:** {{source}}
{{#name}}**Editable map:** {{name}}
{{/name}}{{#merge}}**Merge geometry:** {{merge}}
{{/merge}}{{#keepXboxOnly}}**Keep Xbox-only actors:** {{keepXboxOnly}}
{{/keepXboxOnly}}{{#goal}}**Goal:** {{goal}}
{{/goal}}
## What recovery does

A compiled map (`Packages\Maps\<name>.sdc`) is what the game plays: its brushes were turned into BSP and its source data stripped. It cannot be opened as a map (`map_file` refuses it, since a plain load loses its geometry's alignment). `map_recover` runs the editor's own **File > Recover Compiled Map**: it rebuilds structural brushes from the compiled BSP's convex cells, restores the actors and level settings, moves embedded meshes into the map's own package, rebuilds geometry and paths, carries the original baked lighting over and saves a new editable map in `Packages\MapsEd`. The compiled file is never changed.

It is not a 1:1 copy. Tell the user so:
- **Brushes are new.** Each is `RecoveredVolume<n>`, made from the BSP's cells (merged into fewer, larger brushes when merging is on). The original brushes, their grouping and their history are gone; the built geometry matches.
- **Xbox-only actors** are left out (the recovered map is for PC) unless `keepXboxOnly` is true; properties that pointed at them are cleared.
- **Lighting** is the compiled bake carried over. Ordinary builds keep it, so new or moved lights do not show until a re-bake. **Build > Recalculate Lighting** re-bakes the whole map and changes the recovered look; for single surfaces and meshes the user selects them and runs **Build > Recalculate Selected Lighting**.

## Recipe

1. **Connect.** Call `editor_status`. Recovery replaces the open map: if it has unsaved changes, ask the user whether to save them (`map_file` with `action:"save"`) or drop them (`discard: true`).

2. **Check first.** Call `map_recover` with `source:"{{source}}"`{{#name}}, `name:"{{name}}"`{{/name}}{{#merge}}, `merge:{{merge}}`{{/merge}}{{#keepXboxOnly}}, `keepXboxOnly:{{keepXboxOnly}}`{{/keepXboxOnly}} and `dryRun:true`. It changes nothing and reports what the real run would do.
   - **Missing packages.** A map made with custom textures, meshes or sounds needs those packages installed, or loading it would stop the editor at a missing-package dialog. The refusal lists them: ask the user to install each into its Packages folder (`.utx` Textures, `.usx` StaticMeshes, `.uax` Sounds, `.ukx` Animations, `.u` into System), from wherever the map came from, such as the SCCT-Maps collection. Then check again.
   - **A taken name.** Recovery never replaces files; pass the `name` the refusal suggests.
   - **A source map.** A `.sdc` in `Packages\MapsEd` is already editable: open it with `map_file` `action:"open"` instead.
   - **Busy.** A recovery started from the menu, or Rebuild All Maps, must finish first.

3. **Recover.** Call `map_recover` again without `dryRun`. It takes minutes, up to an hour or more for a big map on a slow PC, and reports each stage with its percent. If your client stops waiting sooner, the user can raise its tool timeout (Claude Code: the `MCP_TOOL_TIMEOUT` environment variable, in milliseconds); the editor finishes the recovery regardless, and `editor_status` shows the result. Cancelling works until it starts saving, and leaves a new, empty map with no files behind.
   - If it fails because the editor's BSP build did not reproduce the merged brushes, try again with `merge:false`.
   - Otherwise `editor_log` with `contains:"MapRecovery"` shows the stage that failed.

4. **Read the Recovery Report.** The result has the counts (cells from the BSP, structural brushes, polygons, actors), `categories` (each with what it means and what to do) and the first `rows`. `map_query` with `what:"recovery"` returns every row; filter with `category`, page with `limit` and `offset`.
   - **Thin brush**, **Reference cleared**, **Mesh lighting recalculated**: the row's `actor` is in the map. Look at them with `view_screenshot` and `frame:[actor, ...]`, and read them with `map_query` `what:"actor"`.
   - **Actor left out**: Xbox-only actors that were not recreated. Nothing to do unless gameplay used them; then recover again under a new name with `keepXboxOnly:true`.
   - **Asset moved**: the map's own `StaticMeshes\<name>_Assets.usx` now holds them. Ship it with the map.
   - **BSP lighting not preserved**: surfaces that kept a fresh bake. Compare them with `view_screenshot`; the user can re-bake just those with **Recalculate Selected Lighting**.

5. **Look.** Call `view_screenshot` with `preset:"overview"`, `spawn:spy` and `spawn:merc`, and `map_query` with `what:"summary"`: spawns per team, missions, objectives, cameras.

6. **Validate.** Call `map_validate` with `profile:"draft"`. A stock map was made for the game's own needs: read each finding against `scct://wiki/index` and `scct://rules` before changing anything, and report what is missing for each mode.

7. **Make it yours.** {{#goal}}The user wants: {{goal}}. {{/goal}}{{^goal}}Ask the user what to change. {{/goal}}
   - The map has no level spec yet. Add gameplay with the intent tools (`gameplay`, `security`, `environment`, `mechanisms`, `events`, `geometric_events`), naming the recovered actors they build on in `existing.refs`; use `actors_edit` for one-off changes. Each change is one Undo step.
   - Rebuild with `map_build` (geometry) after brush changes; it keeps the recovered lighting.
   - **Never save it under the compiled map's own name**: every save also writes `Packages\Maps\<name>.sdc`, which would replace the original. For a release, pick a new name, four letters plus D that no map in `Packages\Maps` uses (`map_file` with `action:"save_as"`), then `editor_restart` before `map_settings`, as for any new map, and `map_validate` with `profile:"release"`.

8. **Report.** Tell the user the file, the counts, the report's categories and what you did about them, and what is not 1:1 (above).
