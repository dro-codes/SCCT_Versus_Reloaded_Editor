# Using the SCCT Reloaded MCP server

`Reloaded.Editor.Mcp.exe` lets an agent inspect, edit, build and validate SCCT Versus maps in the running RE+ editor, and build whole levels from a declarative spec.
- The exe talks to the editor over a named pipe.
- **The user must enable it once:** RE+ Tools > MCP > Enable MCP server, or `[Mcp] Enabled=1` in `System\Reloaded_Editor.ini`. An agent cannot turn it on.

## Ground rules

- **The wiki is the source of truth.** Read `scct://wiki/index`, then the pages for what you build. `scct://rules` lists every check.
- **Units:**
  - Unreal units (≈100 uu per metre; 1 cm = 1 uu); Z up; east +X, north +Y.
  - Spec yaw is in degrees (east 0, north 90).
  - Engine rotations use 65536 units per turn.
- **Every change is one Undo step**, labelled `MCP <n>: <label>` in Undo History.
  - Mutating tools take `dryRun` (preview only) and `label`.
  - Each result carries `undo.call`; `editor_undo` with that `call` undoes the whole call.
- **Staleness:** edits to existing actors check their current values first, so if the map changed meanwhile the call fails as `stale`. Re-read (`map_query`) and retry.
- **Approval:** when the user turns on *Require approval*, each change opens a review window in the editor. After about 50 s the call fails with `awaiting_approval` and a `ticket`; call `approval_wait` with it.
  - `denied` means the user said no. Do not retry the same change unasked.
- **Busy:** the editor is in a dialog, a menu, a drag or a rebuild. The server retries with backoff. If that keeps failing, ask the user to finish what they are doing in the editor.
- **Never allowed:**
  - writing stock packages;
  - deleting without `allowDeletes`;
  - discarding unsaved work unless the user asked;
  - overwriting files without `overwrite`;
  - raw console commands, unless the user set `AllowRawExec=1`.

## Tools (32)

| Group | Tools |
|---|---|
| Session | `editor_status` (start here), `editor_launch`, `editor_restart`, `editor_undo`, `approval_wait`, `editor_log`, `map_file` (new/open/save/save_as), `map_build` (geometry or all, plus Map Check) |
| Inspect | `map_query` (summary, actors, actor, level, schema, assets, wiring, spawns, missions, security, cameras, lights, stages, design, ges, spec), `view_screenshot` (presets overview, spawn:spy, spawn:merc, objective:<name>), `plan_render` (top-down PNG of a spec or the live map; offline) |
| Validate | `map_validate` (`profile: draft\|release`; findings cite `wikiPage` and carry a `fix` tool call) |
| Spec | `level_spec_validate`, `level_build` (`spec`, `mode: preview\|apply`, `sync: merge\|replace\|incremental`, `brushMode: pieces\|polygons`, `build`, `screenshot`, `save`) |
| Intent | `blockout`, `gameplay`, `security`, `mechanisms`, `events`, `environment`, `geometric_events`, `map_settings`, `release`: each builds a spec fragment and goes through `level_build` with `sync:"merge"`, so the wiki defaults always apply |
| Escape hatches | `actors_edit` (set, move, turn, copy, drop, delete, copy_properties, rename_tag), `map_changes_apply` (a raw `scct.map-changes` document), `workflow_request` (one bridge op), `editor_exec` (off by default) |
| Import / media | `import_to_spec` (grid, image plan, SVG, Tiled, GeoJSON, CSV, T3D, VMF, Quake .map, OBJ, map export → spec), `media_extract_frames`, `media_read_image`, `image_convert`, `assets_import` |

Mutating tools return `{summary, created[], changed[], warnings[], validation[], undo, next[]}`.
- `next` suggests the follow-up calls.
- Images come back as MCP image content.

## The level spec

`scct.level-spec` v1 (`scct://schema/level-spec`; examples at `scct://examples/*`) describes a whole level declaratively:
- map name and file;
- storeys and zones;
- blockout pieces with `attach`, doorways and vertical links;
- spawns and the game modes;
- security and CamNet;
- doors, lifts and buttons;
- events and environment;
- GEs;
- Map Settings, interface textures and release info;
- `provenance`.

Rules:
- Ids (`[A-Za-z_][A-Za-z0-9_]{0,62}`, unique ignoring case) become both the create id and the actor's `Tag`.
- Positions may be:
  - absolute `[x, y, z]`, where z may be `"floor+N"`;
  - room-relative `{in, u, v, h}` (u, v in -0.5..0.5; h `"floor+N"`/`"ceiling-N"`);
  - `{near, offset}`;
  - `{doorway}`.

`level_build` compiles the spec into one `mcp.plan` (`scct://schema/build-plan`):
- **pre:** package loads, texture imports and saves.
- **steps:** one Undo step, in this order:
  1. blockout batches (≤64 pieces, Carve);
  2. one actors document;
  3. objective player counts;
  4. GEs;
  5. Level Properties;
  6. Map Design registration;
  7. the spec sidecar.
- **post:** build → Map Check → save → screenshot.

`mode:"preview"` (or `dryRun`) changes nothing and returns the preview rows and a plan PNG.

## The workflow loop

1. **`editor_status`.** Is an editor connected, which map is open, is approval on? With no editor, try `editor_launch`, or ask the user to start RE+ and enable the server.
2. **Read** `scct://wiki/index`, the pages for the features involved, and `scct://rules`.
3. **Start or open a map.** Use `map_file action:"new"` (or `open`). Never discard unsaved work unless asked (`discardUnsaved`).
4. **Write the spec.** Use a prompt (`create_level_from_description`, `create_level_from_media`, `create_level_from_file`, `versus_starter`) or `import_to_spec`.
   - Record where it came from in `provenance`.
   - Calibrate scale: a standing player is 180 uu tall and 96 wide; ≈100 uu/m; doors 96 × 128; stair risers ≤35 uu (the blockout uses ≤24 rise, ~32 run).
5. **Check offline.** Run `level_spec_validate` (errors at JSON pointers, rule findings with wiki pages) and `plan_render`; compare the plan PNG with the source.
6. **Preview.** `level_build mode:"preview"`: read the rows and the plan image.
7. **Apply.** `level_build mode:"apply"` makes one Undo step; `editor_undo` reverts it.
8. **Build.** `map_build what:"geometry"` (or `"all"` for lighting).
9. **Look.** `view_screenshot` (overview, spawns, objectives), compared with the source.
10. **Validate.** `map_validate profile:"draft"`.
    - Fix errors with each finding's `fix` call, or by editing the spec and running `level_build sync:"incremental"`.
    - Repeat until clean.
11. **Save, then restart.** `map_file action:"save_as" mapName:"..."` (file = first 4 letters + `D`, in `Packages\MapsEd`), then `editor_restart`. A new map needs save → exit → reopen before Map Settings works.
12. **Map Settings.** Run `map_settings` (SMapSettings, `<Map>-i` interface textures, minimap, Level Properties), then `map_file action:"save"`.
13. **Release.** `map_validate profile:"release"` must come back with no errors. Then `release` (`.uas` with `Interface`, package names, archive list).

Incremental editing:
- Edit the spec and call `level_build sync:"incremental"`. The server diffs it against the applied spec (`scct://map/spec`), and reports user edits it would overwrite.
- For one-off changes to existing actors, use the intent tools or `actors_edit`.

## Resources

| URI | What |
|---|---|
| `scct://wiki/{page}` | Wiki digests (start at `scct://wiki/index`) |
| `scct://rules` | Every validation rule with its severities and wiki page |
| `scct://schema/{level-spec, map-changes, map-authoring, stages, smagic-event, build-plan}` | JSON Schemas |
| `scct://docs/{tools, map-authoring, map-design}` | These documents |
| `scct://examples/{versus-starter, diskhunt-arena, elevator-3floors, security-camnet, three-modes-sectors}` | Example specs |
| `scct://editor/state`, `scct://map/summary`, `scct://map/validation`, `scct://map/spec`, `scct://map/screenshot/latest`, `scct://map/actor/{path}` | Live, from the editor |

## Prompts

`create_level_from_description`, `create_level_from_media`, `create_level_from_file`, `versus_starter`, `add_game_mode`, `design_security`, `ge_pass`, `review_map_against_wiki`, `fix_validation` and `prepare_release`.

Each prompt is a step-by-step recipe that embeds the wiki pages and the rules it needs.

## Errors

| Code | Meaning / what to do |
|---|---|
| `not_connected` / `disabled` | No editor, or the server is off: `editor_status`, `editor_launch`, or ask the user to enable it |
| `busy` | The editor is occupied (reason given); the server already retried. Ask the user to close dialogs |
| `stale` | The map changed since you read it; re-read and retry |
| `invalid` | Bad arguments or document; the message names the field |
| `refused` | Blocked op, or one that needs a setting the user has not enabled (`AllowRawExec`, `AllowPlay`) |
| `denied` | The user declined in the approval window |
| `dialog_blocked` | An unexpected editor dialog appeared; the call was rolled back. Its title and text are in the details |
| `editor_error` | The editor refused the change; the message says why (nothing was changed) |
| `timeout` | Too slow; `awaiting_approval` comes with a ticket for `approval_wait` |
| `cancelled` | The client cancelled the request |
| `protocol_mismatch` | The exe and the editor's DLL are different versions; update RE+ so both match |
