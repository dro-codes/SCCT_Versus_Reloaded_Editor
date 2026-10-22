# Using the SCCT Reloaded MCP server

`Reloaded.Editor.Mcp.exe` lets an agent inspect, edit, build and validate SCCT Versus maps in the running RE+ editor, and build whole levels from a declarative spec.
- The exe talks to the editor over a named pipe. It lives in the game's `System` folder, beside `Reloaded.Editor.dll`.
- **The user must enable it:** RE+ Tools > MCP > Enable MCP server, or `[Mcp] Enabled=1` in `System\Reloaded_Editor.ini`. An agent cannot turn it on, nor allow raw commands (`AllowRawExec`) or Play Level (`AllowPlay`).
- The server never interprets pictures, video or prose. You look, decide what the place is and write it down as a spec; the server compiles, checks and builds.

## Ground rules

- **The wiki is the source of truth.** Read `scct://wiki/index`, then the pages for what you build. `scct://rules` lists every check with its wiki page.
- **Units:**
  - Unreal units (about 100 uu per metre; a standing player is 180 tall); Z up; east +X, north +Y.
  - Spec yaw is in degrees (east 0, north 90).
  - Engine rotations use 65536 units per turn.
- **Every change is one Undo step**, labelled `MCP <n>: <label>` in Undo History.
  - Mutating tools take `dryRun` (preview only) and `label`.
  - Each result carries `undo.call`; `editor_undo` with that `call` undoes the whole call while it is still the newest. With no arguments `editor_undo` undoes this server's newest call that is still undoable (again: the one before it); when this server has made no change it undoes nothing, rather than something the user did. `steps` undoes the newest Undo steps whoever made them.
  - Saves, builds, package and texture imports are not Undo steps.
- **One call at a time.** The editor runs each call on its own window thread, between the user's clicks.
- **Staleness:** changes carry the map's identity and edits to existing actors their current values, so if the map changed meanwhile the call fails as `stale` and nothing changes. Re-read (`map_query`) and retry.
- **Approval:** when the user turns on *Require approval*, each change opens a review window in the editor. After about 50 s the call fails with code `timeout`, reason `awaiting_approval` and a `ticket`; call `approval_wait` with it.
  - `denied` means the user said no. Do not retry the same change unasked.
- **Busy:** the editor is in a dialog, a menu, a drag or a rebuild. The server retries with backoff. If that keeps failing, ask the user to finish what they are doing in the editor (the reason says what). Two reasons come back at once instead, since waiting cannot help: approval_pending (a change waits in the review window: call `approval_wait` with its ticket, which the error's details carry) and `another_client` (another MCP client holds the editor).
- **Cancelling** a request stops it: the editor is asked to drop the call in flight, and waits (`editor_launch`, `editor_restart`, `approval_wait`) and long tools stop at their next progress report. Check `editor_status` before repeating a cancelled change.
- **Never allowed:**
  - writing stock packages (only the map's own: `<Map>`, `<Map>-i`, `Amb_<Map>`, `<Map>_*`). Imports, package saves and a Map JSON document's `texture` and `save` operations (`map_changes_apply`, `workflow_request` authoring.apply and objimport.import) write only `<Map>_<name>` (a Map JSON texture and an OBJ import may also go into `MyLevel`); another package is refused (reason not_map_owned, package_name or interface_package), and so is any of them before the map's first save (unsaved_map), since those packages are named after its file. A `load` alone is allowed;
  - deleting actors without `allowDeletes`;
  - discarding unsaved work unless the user asked (`discardUnsaved`);
  - overwriting files without `overwrite`;
  - writing outside the work folder and `[Mcp] WriteRoots` (`editor_exec` `FILE=`), or a release copy outside `<Game>\Releases`, the work folder and `WriteRoots`;
  - raw console commands, unless the user set `AllowRawExec=1`. Even then `editor_exec` refuses (reason denied_command) any command that starts with QUIT, EXIT, RELAUNCH, DEBUG, EXEC, MAP NEW/LOAD/SAVE/IMPORT/EXPORT, OBJ SAVEPACKAGE/EXPORT/IMPORT/LOAD, SAVEMAPPROP, LOADMAPPROP, TEXTURE IMPORT/EXPORT, STATICMESH IMPORT/EXPORT, MESH IMPORT, BRUSH IMPORT/EXPORT, DUMPINT, TRANSACTION or AUDIO, read as the console reads it (any case; `QUIT;` and `EXEC"x"` count); a `FILE=` anywhere outside the work folder and WriteRoots (`outside_write_roots`); a `PACKAGE=` other than MyLevel or the map's own (not_map_owned); and more than one command. Use the tool that does the job with its checks.

## Tools (27)

| Group | Tools |
|---|---|
| Session | `editor_status` (start here; `pid` picks one of several editors), `editor_launch` (`waitSeconds`, `map`, `force`), `editor_restart` (`save`), `editor_undo` (`call`, `steps`, `redo`), `approval_wait` (`ticket`), `editor_log` (`level`, `contains`, `since`, `newOnly`), `map_file` (`action`: new, open, save or `save_as`; `path`, `mapName`, `discardUnsaved`, `overwrite`), `map_build` (`what`: geometry or all; `check`) |
| Inspect | `map_query` (`what`: summary, actors, actor, level, schema, assets, wiring, spawns, missions, security, cameras, lights, stages, design, ges, spec; actors filter by `classes`, `tags`, `names`, `near`, `box`, `selected`, with `fields`, `limit`, `cursor`), `view_screenshot` (`viewport`, `maxEdge`, `camera`, `frame`, or `preset`: overview, spawn:spy, spawn:merc, objective:<name>), `plan_render` (`spec` or `path` offline, or the live map; `maxEdge`, `options`: storey, labels, grid, legend, actorLabels, cones, title) |
| Validate | `map_validate` (`profile`: draft or release; `include` groups wiki, design, security, mapcheck, lighting, release; `rules`; `spec` or `path` to check a spec offline; findings cite `wikiPage` and carry a `fix`: a tool call on the live map, an edit of the spec offline) |
| Spec | `level_spec_validate` (`spec` or `path`, offline), `level_build` (`spec` or `path`; `mode`: preview (default) or apply; `sync`: incremental, replace or merge; `brushMode`: pieces or polygons; `build`: geometry, all or none; `screenshot`; `save`; `force`) |
| Intent | `blockout`, `gameplay`, `security`, `mechanisms`, `events`, `environment`, `geometric_events`, `map_settings`: each takes its part of the spec at the top level, lays it over the spec the map was built from (matched by id; `remove` takes ids out) and builds only what changed, as `level_build` with `sync:"incremental"` does, so the wiki defaults always apply. `mode` defaults to apply here; `mode:"preview"` or `dryRun` changes nothing |
| Release | `release` (`action`: check, sounds, optimise, package or interface; a release copy's `folder` must be inside `<Game>\Releases`, the work folder or `[Mcp] WriteRoots`) |
| Escape hatches | `actors_edit` (`edits` of set, move, turn, copy, drop, delete, `copy_properties`, `rename_tag`; deletes need `allowDeletes`), `map_changes_apply` (a raw `scct.map-changes` `document`), `workflow_request` (one bridge `op` with `args`; `scct://docs/ops` lists them), `editor_exec` (one console `command`; off by default) |

Mutating tools return `{summary, created[], changed[], warnings[], validation[], undo, next[]}`.
- `next` suggests the follow-up calls.
- Images come back as MCP image content.
- Errors come back as results marked isError, with `code`, `reason`, a `hint` and whether a retry can help.

## Conventions every spec tool shares

**Units and directions.** Lengths are in the spec's `units.unit`: `uu` (Unreal units, the default), `m` or `ft` (through `uuPerMetre`, default 100). Z is up, east +X, north +Y. Yaw is in degrees (east 0, north 90); engine rotations are 65536 per turn.

**Ids.** Every id matches `[A-Za-z_][A-Za-z0-9_]{0,62}`, is unique across the spec ignoring case, and becomes the actor's create id and its `Tag`, so alarms, targets, links and `{"$ref": id}` name things by id. `existing.refs` names live actors the spec did not make: each id maps to `{path, class}` or `{tag}`.

**Positions**, anywhere a spec takes one:
- `[x, y, z]`, where z may be `"floor+N"` or `"ceiling-N"` of the room under the point, or be left out for the usual height of the thing;
- `{in: pieceId, u, v, h}`: u and v from -0.5 to 0.5 across and along the piece, h `"floor+N"` or `"ceiling-N"`;
- `{near: id, offset: [dx, dy, dz]}`;
- `{doorway: id}`.

**How the intent tools apply their arguments.** `blockout`, `gameplay`, `security`, `mechanisms`, `events`, `environment`, `geometric_events` and `map_settings` each take their part of the spec at the top level:
- The arguments are laid over the spec the map was last built from (`map_query what:"spec"`; a map without one starts a new spec, named by `mapName` or the map's file).
- Objects merge field by field. List items match by `id` (vision by `target`, links by `from` and `to`, level events by `event`, objective player counts by `objective`, Map Settings modes by `mode`): a match is updated field by field, the rest are added. `remove: [ids]` takes items out of the tool's part.
- Only what changed is built, as `level_build sync:"incremental"` does, in ONE Undo step. `mode: "apply"` is the default; `mode: "preview"` (or `dryRun: true`) returns the editor's preview rows and a plan PNG and changes nothing.
- Errors the change brings block it unless `force: true`; errors the spec already had are listed as standing and do not.
- `build`: `auto` (the default: a geometry build when brushes changed), `geometry`, `all` (with lighting) or `none`. `screenshot` (default: when it builds), `save` (a map path to save to afterwards), `label` (the Undo History name).
- The result has `created`, `changed` and `removed` by id (with the actor paths), `validation` (this part's findings: JSON pointer, `wikiPage`, `fix`), `undo` and `next`.

Every field of every tool, and the wiki defaults the compiler applies: `scct://docs/tool-reference`. A tool description's `scct://docs/tool-reference#<tool>` is that tool's section of it (read the URI as given; the server returns the whole document). Every bridge op `workflow_request` can call, with its policy and arguments: `scct://docs/ops`.

**Fixes.** A finding's `fix` takes one of two forms:
- On the live map (`map_validate` without a spec, the intent tools): `{tool, args, note?}`, the call that repairs it.
- For a spec checked as a document (`level_spec_validate`, `level_build` with a spec, `map_validate` with `spec` or `path`): `{spec: [{pointer, merge | change}], note?}`. Merge `merge` into the spec at the JSON `pointer` the way the intent tools merge their arguments; `change` says in words what to write there. Then check the spec again. A fix only the built map can take (`release`, `map_build`, `actors_edit`) keeps its tool call, with `live: true`.

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

`mode:"preview"` (the default for `level_build`, or `dryRun`) changes nothing and returns the preview rows and a plan PNG. When the preview has errors or findings about other parts of the spec, the spec it built is saved in the work folder (`specFile`), and `next` runs `level_spec_validate` on that file.

Sync:
- `incremental` (the default once the map has a stored spec): new ids are created, changed pieces rebuilt, changed actors updated (or recreated when class, team or Tag changes), dropped ids deleted.
- `replace` (the default otherwise): deletes what the last build made and builds afresh.
- `merge`: lays the given spec over the stored one first.
- Edits the user made by hand since the last build are not detected; read the map (`map_query`) before rebuilding over a map the user has worked on.

## The workflow loop

1. **`editor_status`.** Is an editor connected, which map is open, is approval on? With no editor, try `editor_launch`, or ask the user to start RE+ and enable the server.
2. **Read** `scct://wiki/index`, the pages for the features involved, and `scct://rules`.
3. **Start or open a map.** Use `map_file action:"new"` (or `open`). Never discard unsaved work unless asked (`discardUnsaved`).
4. **Write the spec.** Use a prompt (`create_level_from_description`, `versus_starter`).
   - Record where it came from in `provenance`.
   - Calibrate scale: a standing player is 180 uu tall and 96 wide; ≈100 uu/m; doors 96 × 128; stair risers ≤35 uu (the blockout uses ≤24 rise, ~32 run). `uuPerPx` = known size in uu / its size in pixels.
5. **Check offline.** Run `level_spec_validate` (errors at JSON pointers, rule findings with wiki pages, fixes as edits of the spec) and `plan_render`; compare the plan PNG with the source. Both take `spec` or `path` (a spec saved to a file), as does `map_validate`.
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
| `scct://docs/{tools, tool-reference, ops, map-authoring, map-design}` | This guide, every tool field and wiki default, the bridge ops, and the Map JSON and Map Design formats |
| `scct://examples/{versus-starter, diskhunt-arena, elevator-3floors, security-camnet, three-modes-sectors}` | Example specs |
| `scct://editor/state`, `scct://map/summary`, `scct://map/validation`, `scct://map/spec`, `scct://map/screenshot/latest`, `scct://map/actor/{path}` | Live, from the editor |

## Prompts

`create_level_from_description`, `versus_starter`, `add_game_mode`, `design_security`, `ge_pass`, `review_map_against_wiki`, `fix_validation` and `prepare_release`.

Each prompt is a step-by-step recipe that embeds the wiki pages it needs and points at `scct://rules`.

## Errors

| Code | Meaning / what to do |
|---|---|
| `not_connected` / `disabled` | No editor, or the server is off: `editor_status`, `editor_launch`, or ask the user to enable it |
| `busy` | The editor is occupied (reason given: modal, menu, drag, rebuild, mover_preview); the server already retried. Ask the user to close dialogs. approval_pending (call `approval_wait` with the ticket) and `another_client` come back at once |
| `stale` | The map changed since you read it; re-read and retry |
| `invalid` | Bad arguments or document; the message names the field (reason several editors: pass `pid` to `editor_status`) |
| `refused` | A blocked op, or the reason says what: a setting the user has not enabled (`AllowRawExec`, `AllowPlay`), unsaved changes, a package that is not the map's own (not_map_owned, package_name, interface_package), a map not saved yet (unsaved_map), a folder or `FILE=` outside the write roots (`outside_write_roots`), a console command `editor_exec` never runs (denied_command) |
| `denied` | The user declined in the approval window |
| `dialog_blocked` | An unexpected editor dialog appeared. It was answered with its most cautious button (Cancel, No, Close, Abort by control id; OK only when alone) or closed, and the call stopped there. details.dialogs has its title, text and answer; details.stoppedAt the step, details.ran the steps that finished, details.wrote the files and packages already written (they stay). details.rolledBack says whether the map changes were undone: a plan that had already saved or written files keeps them, so map and file stay alike, and then `editor_undo` with the call the message names takes them back |
| `editor_error` | The editor refused the change; the message says why (nothing was changed) |
| `timeout` | Too slow; reason `awaiting_approval` comes with a ticket for `approval_wait` |
| `cancelled` | The client cancelled the request |
| `protocol_mismatch` | The exe and the editor's DLL are different versions; update RE+ so both match |
