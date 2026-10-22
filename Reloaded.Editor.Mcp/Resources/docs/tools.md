# Using the SCCT Reloaded MCP server

`Reloaded.Editor.Mcp.exe` lets an agent inspect, edit and build SCCT Versus maps in the running RE+ editor.
- The exe talks to the editor over a named pipe. It lives in the game's `System` folder, beside `Reloaded.Editor.dll`.
- **The user must enable it:** RE+ Tools > MCP > Enable MCP server, or `[Mcp] Enabled=1` in `System\Reloaded_Editor.ini`. An agent cannot turn it on, nor allow raw commands (`AllowRawExec`) or Play Level (`AllowPlay`).

## Ground rules

- **The wiki is the source of truth.** Read `scct://wiki/index`, then the pages for what you build.
- **Units:**
  - Unreal units (about 100 uu per metre; a standing player is 180 tall); Z up; east +X, north +Y.
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

## Tools (14)

| Group | Tools |
|---|---|
| Session | `editor_status` (start here; `pid` picks one of several editors), `editor_launch` (`waitSeconds`, `map`, `force`), `editor_restart` (`save`), `editor_undo` (`call`, `steps`, `redo`), `approval_wait` (`ticket`), `editor_log` (`level`, `contains`, `since`, `newOnly`), `map_file` (`action`: new, open, save or `save_as`; `path`, `mapName`, `discardUnsaved`, `overwrite`), `map_build` (`what`: geometry or all; `check`) |
| Inspect | `map_query` (`what`: summary, actors, actor, level, schema, assets, wiring, spawns, missions, security, cameras, lights, stages, design, ges; actors filter by `classes`, `tags`, `names`, `near`, `box`, `selected`, with `fields`, `limit`, `cursor`), `view_screenshot` (`viewport`, `maxEdge`, `camera`, `frame`, or `preset`: overview, spawn:spy, spawn:merc, objective:<name>) |
| Escape hatches | `actors_edit` (`edits` of set, move, turn, copy, drop, delete, `copy_properties`, `rename_tag`; deletes need `allowDeletes`), `map_changes_apply` (a raw `scct.map-changes` `document`), `workflow_request` (one bridge `op` with `args`; `scct://docs/ops` lists them), `editor_exec` (one console `command`; off by default) |

Mutating tools return `{summary, created[], changed[], warnings[], undo, next[]}`.
- `next` suggests the follow-up calls.
- Images come back as MCP image content.
- Errors come back as results marked isError, with `code`, `reason`, a `hint` and whether a retry can help.

## The workflow loop

1. **`editor_status`.** Is an editor connected, which map is open, is approval on? With no editor, try `editor_launch`, or ask the user to start RE+ and enable the server.
2. **Read** `scct://wiki/index` and the pages for the features involved.
3. **Start or open a map.** Use `map_file action:"new"` (or `open`). Never discard unsaved work unless asked (`discardUnsaved`).
4. **Edit.** `map_query` reads what is there. `actors_edit` changes existing actors; `map_changes_apply` creates actors, links events and imports textures from a `scct.map-changes` document (`scct://docs/map-authoring`). Preview with `dryRun`; `editor_undo` reverts a call.
5. **Build.** `map_build` after brushes change, `what:"all"` when lighting matters (it can take minutes).
6. **Look.** `view_screenshot` (overview, spawns, objectives).
7. **Save, then restart.** `map_file action:"save_as" mapName:"..."` (file = first 4 letters + `D`, in `Packages\MapsEd`), then `editor_restart`. A new map needs save → exit → reopen before Map Settings exist.

Known gaps to work around:
- `editor_restart` needs the editor to close without a question; if it times out, ask the user to answer or restart by hand.

## Resources

| URI | What |
|---|---|
| `scct://wiki/{page}` | Wiki digests (start at `scct://wiki/index`) |
| `scct://schema/{map-changes, map-authoring, stages, smagic-event, build-plan}` | JSON Schemas |
| `scct://docs/{tools, ops, map-authoring, map-design}` | This guide, the bridge ops, and the Map JSON and Map Design formats |
| `scct://editor/state`, `scct://map/summary`, `scct://map/validation`, `scct://map/screenshot/latest`, `scct://map/actor/{path}` | Live, from the editor |

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
