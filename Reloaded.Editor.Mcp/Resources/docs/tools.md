# Using the SCCT Reloaded MCP server

`Reloaded.Editor.Mcp.exe` lets an agent inspect, edit, build and validate SCCT Versus maps in the running RE+ editor, and build whole levels from a declarative spec.
- The exe talks to the editor over a named pipe. It lives in the game's `System` folder, beside `Reloaded.Editor.dll`.
- **The user must enable it:** RE+ Tools > MCP > Enable MCP server, or `[Mcp] Enabled=1` in `System\Reloaded_Editor.ini`. An agent cannot turn it on, nor allow raw commands (`AllowRawExec`) or Play Level (`AllowPlay`).
- The server never interprets pictures, video or prose. You look, decide what the place is and write it down as a spec; the server extracts, measures, compiles, checks and builds.

## Ground rules

- **The wiki is the source of truth.** Read `scct://wiki/index`, then the pages for what you build. `scct://rules` lists every check with its wiki page.
- **Units:**
  - Unreal units (about 100 uu per metre; a standing player is 180 tall); Z up; east +X, north +Y.
  - Spec yaw is in degrees (east 0, north 90).
  - Engine rotations use 65536 units per turn.
- **Every change is one Undo step**, labelled `MCP <n>: <label>` in Undo History.
  - Mutating tools take `dryRun` (preview only) and `label`.
  - Each result carries `undo.call`; `editor_undo` with that `call` undoes the whole call while it is still the newest. With no arguments `editor_undo` undoes this server's last call.
  - Saves, builds, package and texture imports are not Undo steps.
- **One call at a time.** The editor runs each call on its own window thread, between the user's clicks.
- **Staleness:** changes carry the map's identity and edits to existing actors their current values, so if the map changed meanwhile the call fails as `stale` and nothing changes. Re-read (`map_query`) and retry.
- **Approval:** when the user turns on *Require approval*, each change opens a review window in the editor. After about 50 s the call fails with code `timeout`, reason `awaiting_approval` and a `ticket`; call `approval_wait` with it.
  - `denied` means the user said no. Do not retry the same change unasked.
- **Busy:** the editor is in a dialog, a menu, a drag, a rebuild or a review. The server retries with backoff. If that keeps failing, ask the user to finish what they are doing in the editor (the reason says what).
- **Never allowed:**
  - writing stock packages (only the map's own: `<Map>`, `<Map>-i`, `Amb_<Map>`, `<Map>_*`);
  - deleting actors without `allowDeletes`;
  - discarding unsaved work unless the user asked (`discardUnsaved`);
  - overwriting files without `overwrite`;
  - writing outside the work folder and `[Mcp] WriteRoots` (media and import tools);
  - raw console commands, unless the user set `AllowRawExec=1`.

## Tools (32)

| Group | Tools |
|---|---|
| Session | `editor_status` (start here; `pid` picks one of several editors), `editor_launch` (`waitSeconds`, `map`, `force`), `editor_restart` (`save`), `editor_undo` (`call`, `steps`, `redo`), `approval_wait` (`ticket`), `editor_log` (`level`, `contains`, `since`, `newOnly`), `map_file` (`action`: new, open, save or `save_as`; `path`, `mapName`, `discardUnsaved`, `overwrite`), `map_build` (`what`: geometry or all; `check`) |
| Inspect | `map_query` (`what`: summary, actors, actor, level, schema, assets, wiring, spawns, missions, security, cameras, lights, stages, design, ges, spec; actors filter by `classes`, `tags`, `names`, `near`, `box`, `selected`, with `fields`, `limit`, `cursor`), `view_screenshot` (`viewport`, `maxEdge`, `camera`, `frame`, or `preset`: overview, spawn:spy, spawn:merc, objective:<name>), `plan_render` (`spec` offline, or the live map; `maxEdge`, `options`) |
| Validate | `map_validate` (`profile`: draft or release; `include` groups wiki, design, security, mapcheck, lighting, release; `rules`; `spec` to check offline; findings cite `wikiPage` and carry a `fix` tool call) |
| Spec | `level_spec_validate` (`spec` or `path`, offline), `level_build` (`spec` or `path`; `mode`: preview (default) or apply; `sync`: incremental, replace or merge; `brushMode`: pieces or polygons; `build`: geometry, all or none; `screenshot`; `save`; `force`) |
| Intent | `blockout`, `gameplay`, `security`, `mechanisms`, `events`, `environment`, `geometric_events`, `map_settings`: each takes its part of the spec at the top level, lays it over the spec the map was built from (matched by id; `remove` takes ids out) and builds only what changed, as `level_build` with `sync:"incremental"` does, so the wiki defaults always apply. `mode` defaults to apply here; `mode:"preview"` or `dryRun` changes nothing |
| Release | `release` (`action`: check, sounds, optimise, package or interface) |
| Escape hatches | `actors_edit` (`edits` of set, move, turn, copy, drop, delete, `copy_properties`, `rename_tag`; deletes need `allowDeletes`), `map_changes_apply` (a raw `scct.map-changes` `document`), `workflow_request` (one bridge `op` with `args`), `editor_exec` (one console `command`; off by default) |
| Import / media | `import_to_spec` (`source`: a full path or "live"; `format`: auto, level-spec, map-changes, map-authoring, grid, image, svg, tiled, geojson, csv, t3d, vmf, quake, obj, ase or r6v; `options`), `media_extract_frames` (`video`; `mode`: interval, count or scene), `media_read_image` (`path`; `grid`, `crop`, `palette`), `image_convert` (`file`; `size` or `slot`), `assets_import` (`items` of textures and static meshes into `<Map>_TXT` / `<Map>_STM`) |

Mutating tools return `{summary, created[], changed[], warnings[], validation[], undo, next[]}`.
- `next` suggests the follow-up calls.
- Images come back as MCP image content.
- Errors come back as results marked isError, with `code`, `reason`, a `hint` and whether a retry can help.

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
- Map Settings, Level Properties, interface textures and release info;
- `provenance`.

Rules:
- Ids (`[A-Za-z_][A-Za-z0-9_]{0,62}`, unique ignoring case) become both the create id and the actor's `Tag`.
- Positions may be:
  - absolute `[x, y, z]`, where z may be `"floor+N"` or `"ceiling-N"`, or left out for the usual height;
  - room-relative `{in, u, v, h}` (u, v in -0.5..0.5; h `"floor+N"`/`"ceiling-N"`);
  - `{near, offset}`;
  - `{doorway}`.
- Unknown fields are errors, reported at their JSON pointer.

`level_build` compiles the spec into one `mcp.plan` (`scct://schema/build-plan`):
- **pre:** package loads, texture imports and saves.
- **steps:** one Undo step, rolled back together if any part fails, in this order:
  1. blockout batches (≤64 pieces, Carve);
  2. one actors document;
  3. objective player counts;
  4. GEs;
  5. Level Properties;
  6. Map Design registration;
  7. the spec sidecar.
- **post:** build → Map Check → Map Settings and interface textures (when given) → save → screenshot.

`mode:"preview"` (the default for `level_build`, or `dryRun`) changes nothing and returns the preview rows and a plan PNG.

Sync:
- `incremental` (the default once the map has a stored spec): new ids are created, changed pieces rebuilt, changed actors updated (or recreated when class, team or Tag changes), dropped ids deleted.
- `replace` (the default otherwise): deletes what the last build made and builds afresh.
- `merge`: lays the given spec over the stored one first.
- Edits the user made by hand since the last build are not detected; read the map (`map_query`) before rebuilding over a map the user has worked on.

## The workflow loop

1. **`editor_status`.** Is an editor connected, which map is open, is approval on? With no editor, try `editor_launch`, or ask the user to start RE+ and enable the server.
2. **Read** `scct://wiki/index`, the pages for the features involved, and `scct://rules`.
3. **Start or open a map.** Use `map_file action:"new"` (or `open`). Never discard unsaved work unless asked (`discardUnsaved`).
4. **Write the spec.** Use a prompt (`create_level_from_description`, `create_level_from_media`, `create_level_from_file`, `versus_starter`) or `import_to_spec`.
   - Record where it came from in `provenance`.
   - Calibrate scale: a standing player is 180 uu tall and 96 wide; ≈100 uu/m; doors 96 × 128; stair risers ≤35 uu (the blockout uses ≤24 rise, ~32 run). `uuPerPx` = known size in uu / its size in pixels.
5. **Check offline.** Run `level_spec_validate` (errors at JSON pointers, rule findings with wiki pages) and `plan_render`; compare the plan PNG with the source.
6. **Preview.** `level_build mode:"preview"`: read the rows and the plan image.
7. **Apply.** `level_build mode:"apply"` makes one Undo step and builds the geometry; `editor_undo` reverts it.
8. **Build.** `map_build what:"all"` when lighting matters (it can take minutes).
9. **Look.** `view_screenshot` (overview, spawns, objectives), compared with the source.
10. **Validate.** `map_validate profile:"draft"`.
    - Fix errors with each finding's `fix` call, or by editing the spec and running `level_build sync:"incremental"`.
    - Repeat until clean.
11. **Save, then restart.** `map_file action:"save_as" mapName:"..."` (file = first 4 letters + `D`, in `Packages\MapsEd`), then `editor_restart`. A new map needs save → exit → reopen before Map Settings exist.
12. **Map Settings.** Run `map_settings` (Map Settings, `<Map>-i` interface textures, minimap, Level Properties), then `map_file action:"save"`. When the result says Map Settings were not applied, give the user its checklist and manual steps.
13. **Release.** `map_validate profile:"release"` must come back with no errors. Then `release` (`action`: check, sounds for the `.uas` with Interface, package for the archive list).

Incremental editing:
- Edit the spec and call `level_build sync:"incremental"`, or call an intent tool with just the part that changes. The stored spec is at `scct://map/spec`.
- For one-off changes to existing actors, use the intent tools (with `existing.refs`) or `actors_edit`.

Known gaps to work around:
- GEs can be added, not deleted or given GE IDs: tell the user to delete them in GE mode and set IDs by hand.
- Map Settings may not be writable over the bridge: the result then carries the values and manual steps for the user.
- `editor_restart` needs the editor to close without a question; if it times out, ask the user to answer or restart by hand.

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
| `busy` | The editor is occupied (reason given: modal, menu, drag, rebuild, approval pending, another client); the server already retried. Ask the user to close dialogs |
| `stale` | The map changed since you read it; re-read and retry |
| `invalid` | Bad arguments or document; the message names the field (reason several editors: pass `pid` to `editor_status`) |
| `refused` | Blocked op, or one that needs a setting the user has not enabled (`AllowRawExec`, `AllowPlay`), unsaved changes, or a package that is not the map's own |
| `denied` | The user declined in the approval window |
| `dialog_blocked` | An unexpected editor dialog appeared; the call was rolled back. Its title and text are in the details |
| `editor_error` | The editor refused the change; the message says why (nothing was changed) |
| `timeout` | Too slow; reason `awaiting_approval` comes with a ticket for `approval_wait` |
| `cancelled` | The client cancelled the request |
| `protocol_mismatch` | The exe and the editor's DLL are different versions; update RE+ so both match |
