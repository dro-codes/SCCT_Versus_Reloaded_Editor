# MCP server

`Reloaded.Editor.Mcp.exe` is an [MCP](https://modelcontextprotocol.io) server
for the RE+ editor. It lets an AI agent such as Claude Code or Claude Desktop
read, edit, build and check Versus maps in the running editor, and build a
whole level from a description, a video, a floor plan or another game's map.

The agent does the thinking: it reads the wiki digests, looks at your pictures
and writes the level down. The server does the exact work: it compiles that
description with the wiki's defaults, previews it, applies it in one Undo step,
builds the geometry, runs the checks and takes screenshots. Every rule it
applies or checks comes from the
[SCCT Versus Reloaded wiki](https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki)
and cites the page it comes from.

The server is off until you turn it on, and nothing it does can be turned on by
the agent itself.

```
MCP client (Claude Code, Claude Desktop, ...)
  stdin/stdout, JSON-RPC
Reloaded.Editor.Mcp.exe (System folder)
  named pipe \\.\pipe\ReloadedEditor.Mcp.<editor process id>
Reloaded.Editor.dll inside the running editor (RE+ Tools > MCP)
```

## Install

`Reloaded.Editor.Mcp.exe` comes in the RE+ ZIP. Extracting the ZIP into the
folder that contains `SCCT_Versus.exe` (the game's `System` folder) puts it
beside `Reloaded.Editor.dll`, which is where it must stay: it finds the editor,
its INI and the game's `Packages` folder from there. The self-updater installs
it with each new DLL, and **Help > Roll Back to RE+ x.y.z...** puts the previous
copy back with the DLL.
Restart your MCP client after an update so it starts the new copy.

`Reloaded.Editor.Mcp.exe --version` prints its version and
`Reloaded.Editor.Mcp.exe --self-test` compiles and checks the example levels it
carries; neither needs the editor. Run with no arguments, it waits for an MCP
client on stdin, so double-clicking it does nothing useful.

## Turning it on

Open **RE+ Tools > MCP**:

| Item | What it does |
| --- | --- |
| **Enable MCP server** | Opens the editor's pipe so a client can connect. It reads **(connected)** while one is. Off by default. |
| **Require approval for changes** | Shows every change in a review window before it happens (see [Approval](#approval)). Off by default. |
| **Undo last MCP call** | Undoes the newest MCP change that can still be undone, as a whole. |
| **Disconnect client** | Closes the client's connection; it can reconnect. |
| **Copy Claude Code command** | Copies the `claude mcp add` command for this install to the clipboard. |
| **Copy Claude Desktop config** | Copies the `claude_desktop_config.json` entry for this install. |
| **Open MCP history folder** | Opens `System\ReloadedEditor\Mcp\history`. |

The first two are also in **RE+ Options** under **MCP Server**. The settings are
kept in `System\Reloaded_Editor.ini`:

```ini
[Mcp]
Enabled=0
RequireApproval=0
AllowRawExec=0
AllowPlay=0
HistoryMaxMB=200
Ffmpeg=
WriteRoots=
```

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 0 | The pipe is open. The menu item sets it. |
| `RequireApproval` | 0 | Each change waits for **Approve** in the editor. The menu item sets it. |
| `AllowRawExec` | 0 | `editor_exec` may run console commands. Only the INI sets it. |
| `AllowPlay` | 0 | The `play.level` op may start Play Level. Only the INI sets it. |
| `HistoryMaxMB` | 200 | The most the call history may take, 1 to 100,000 MB; the oldest files go first. |
| `Ffmpeg` | empty | The `ffmpeg.exe` that `media_extract_frames` may run when Media Foundation cannot read a video. Empty: the one on `PATH`, if any. |
| `WriteRoots` | empty | Extra folders, separated by `;`, that the media and import tools may write into besides the work folder. |
| `WorkDir` | `%ProgramData%\REPlusMcp` | The server's work folder: extracted frames, converted images, imported specs. Not written by default: add `WorkDir=<folder>` to move it. |

The editor writes the missing keys (all but `WorkDir`) the first time it starts
with this version. It reads them when it starts, so edit the INI with the editor
closed; the two menu items take effect at once. The server reads `WorkDir` when
it starts and the other keys when a tool needs them.

## Connecting a client

The editor must be running with **Enable MCP server** ticked, or the client
must be allowed to start it (`editor_launch`, which also needs `Enabled=1`).
Replace `C:\Games\SCCT Versus\System` below with your own `System` folder, or
use the **Copy** items in **RE+ Tools > MCP**, which fill it in.

**Claude Code.** In a terminal:

```
claude mcp add scct-reloaded -- "C:\Games\SCCT Versus\System\Reloaded.Editor.Mcp.exe"
```

Add `--scope user` to have it in every project. In Claude Code, `/mcp` lists the
server with its 32 tools, its resources and its prompts; prompts appear as
`/mcp__scct-reloaded__<prompt>` commands.

**Claude Desktop.** **Settings > Developer > Edit Config** opens
`claude_desktop_config.json`. Add the server and restart Claude Desktop:

```json
{
  "mcpServers": {
    "scct-reloaded": {
      "command": "C:\\Games\\SCCT Versus\\System\\Reloaded.Editor.Mcp.exe"
    }
  }
}
```

**MCP Inspector.** To try the tools by hand:

```
npx @modelcontextprotocol/inspector "C:\Games\SCCT Versus\System\Reloaded.Editor.Mcp.exe"
```

**Other clients** run the exe as a stdio server with no arguments. It speaks
MCP protocol versions 2024-11-05, 2025-03-26, 2025-06-18 and 2025-11-25, and
offers 2025-06-18 to a client that asks for another. Tool titles, output
schemas and `structuredContent` are sent from 2025-06-18; tool annotations
(`readOnlyHint`, `destructiveHint`, ...) and progress notifications from
2025-03-26. Logs go to stderr; stdout carries only JSON-RPC.

The server connects to the editor when a tool first needs it. With one editor
running it uses that one; with several, `editor_status` lists them and its
`pid` argument chooses one. Only one client can hold an editor at a time: a
second gets `busy` with reason `another_client`.

Start a session with `editor_status`: it says whether an editor is connected,
which map is open, whether it has unsaved changes and whether approval is on,
and what to do next.

## How calls run

**One at a time, on the editor's own thread.** The server runs tool calls one
after another. The editor runs each change on its window thread, between your
own clicks, exactly as a menu command would. The pipe thread only reads and
writes messages; it never touches the map.

**Busy.** When the editor cannot take a call, the reply is `busy` with a
reason, and the server tries again (after 250 ms, doubling to 4 s) until the
call's time runs out:

| Reason | What is going on |
| --- | --- |
| `modal` | A dialog is open in the editor. |
| `menu` | A menu is open. |
| `drag` | You are dragging in a viewport. |
| `transaction` | An editor operation is still open. |
| `rebuild` | Rebuild All Maps is running. |
| `slow_task` | A long task is running, or the editor did not pick the call up within 10 s (the call is then dropped, not run late). |
| `startup_dialog` | An update offer or the crash recovery window is waiting for you. |
| `minimised` | A screenshot needs the editor window restored. |
| `in_flight` | Another call is still running. |
| `approval_pending` | A change is waiting in the review window. |
| `another_client` | Another MCP client holds this editor. |

While a call runs or waits for approval, the editor still answers
`editor.state` (from its last known state, marked `cached`), `log.tail` and
`approval.status`.

**Stale checks.** Changes carry the map's identity as the server last read it
(level, generation and content revision). If the map changed in between (you
edited it, opened another map, or a review click changed it), the call is
refused as `stale` and nothing changes; the agent reads the map again and
retries. Edits to existing actors also carry their exact current values, as
Map JSON updates do, so an actor you changed is never overwritten blind.

**One Undo step per call.** Every change a tool makes is one Undo step, named
`MCP <n>: <label>` in **Edit > Undo History** (`label` defaults to the tool's
name). A whole level from `level_build` is one step, and if any part of it
fails, every map change of the call is rolled back and no Undo step is left.
Saves, builds, package and texture imports, and screenshots are not Undo steps.
To take a call back:

- **Edit > Undo** or Ctrl+Z, as for any change;
- **RE+ Tools > MCP > Undo last MCP call**;
- the agent's `editor_undo`, which by default undoes the server's last call,
  and refuses rather than undo something newer that you did.

An MCP call is undone as a whole only while its steps are the newest in the
Undo buffer; otherwise it is refused (`not_newest`) and Redo is kept.

**Dry runs.** Every tool that changes something takes `dryRun: true` (the spec
tools also `mode: "preview"`): the editor works out the same change, returns its
preview rows (the Map JSON preview's **Add**, **Modify**, **Move**, **Delete**,
**Link**, **Package** and **Surface** rows) and changes nothing.

**Dialogs.** While a call runs, a dialog it opens is answered: known ones with
their known answer, any other with its most cautious button (Cancel, No, Close
or OK, in that order). The call then fails as `dialog_blocked` with the
dialog's title and text, and its map changes are undone. Progress windows are
left alone.

**History.** Every change, applied or failed, is written to
`System\ReloadedEditor\Mcp\history\<date>-<time>-<call>-<op>.json`
(`scct.mcp-call`: the request, the reply, the label, the client and how long
it took). Reads and dry runs are not recorded. The folder is kept under
`HistoryMaxMB`. Large replies (a whole-map export) travel through files in
`System\ReloadedEditor\Mcp\blobs`, which are deleted once read.

**Long calls.** A geometry build may take up to 15 minutes and a full build
(`map_build what:"all"`) up to an hour before the server gives up; progress is
reported meanwhile. Some MCP clients stop waiting sooner on their own (Claude
Code's limit is set with the `MCP_TOOL_TIMEOUT` environment variable, in
milliseconds). A cancelled request is dropped if it has not started, and taken
out of the review window if it is waiting there; a change already running in
the editor finishes.

### Approval

With **Require approval for changes** ticked, each change (not dry runs, not
reads) first runs as a dry run. If that would fail, the call fails with
`editor_error` / `preview_failed` and nothing is shown. Otherwise the Map JSON
preview window opens as **Approve a change from the MCP client**, listing every
row the change makes:

- **Approve** applies it. The map is checked again first; if it changed while
  the window was open, the call is refused as `stale`.
- **Cancel** refuses it: the agent gets `denied` and is told not to retry the
  same change unasked.

The window waits as long as you need. After about 50 seconds the agent's tool
call returns `timeout` with reason `awaiting_approval` and a `ticket`;
`approval_wait` with that ticket keeps waiting and returns the result once you
answer. While a change waits for approval, other calls get `busy` /
`approval_pending`.

## Tools

32 tools in eight groups. Each tool's own description, as the client shows it,
is the full reference; this section gives each one's purpose and its main
arguments.

Every tool that changes something takes `dryRun` and `label`, and returns
`summary`, `created`, `changed`, `warnings`, `undo` (`{call, steps}`;
`editor_undo` takes the `call`) and `next` (suggested follow-up calls).
Pictures (screenshots, plans, frames) come back as MCP image content.
Errors come back as tool results marked `isError`, with `code`, `reason`, a
`hint` and whether retrying can help.

### Session

| Tool | Purpose and main arguments |
| --- | --- |
| `editor_status` | Start here. Connection, the open map, unsaved changes, Undo state, approval, the `[Mcp]` settings and every editor with a pipe. `pid` picks an editor when several run. Read-only. |
| `editor_launch` | Starts `Reloaded_Editor.exe -reMcp` and waits (`waitSeconds`, default 180) for its pipe; `map` opens a map once started. Refuses when `Enabled` is 0 unless `force`. With `-reMcp` the editor skips its startup update check. |
| `editor_restart` | Saves (`save`, default true), closes the editor, starts it again and reopens the map. The wiki requires this once a new map is first saved, before Map Settings can be edited. Refuses a map that was never saved. |
| `editor_undo` | No arguments: undoes this server's last call. `call` undoes that MCP call; `steps` undoes that many Undo steps whatever made them; `redo` with `steps` redoes. |
| `approval_wait` | Keeps waiting for the review window: `ticket` (required), `timeoutSeconds` (default 50). |
| `editor_log` | The editor's log (last 2,000 lines): `level` (all, warning, error), `contains`, `since`, `newOnly`, `max`. Read-only. |
| `map_file` | `action`: `new`, `open`, `save` or `save_as`. `path` (a bare name such as `AquaD` is looked up in `Packages\MapsEd`), `mapName` (save_as names the file after its first four letters plus `D`), `discardUnsaved`, `overwrite`. |
| `map_build` | `what`: `geometry` (default, as Build Geometry) or `all` (the full build with lighting, without the Lighting Budget question); then Map Check (`check`, default true). |

### Inspect

| Tool | Purpose and main arguments |
| --- | --- |
| `map_query` | Reads the map. `what`: `summary`, `actors`, `actor`, `level`, `schema`, `assets`, `wiring`, `spawns`, `missions`, `security`, `cameras`, `lights`, `stages`, `design`, `ges` or `spec`. `actors` filters by `classes`, `tags`, `names`, `near {at, radius}`, `box {min, max}`, `selected`, with `fields` (names or `"all"`), `limit` and `cursor`; `actor` takes `actor` (path, name or `{path, class}`) and `includeSchema`; `assets` takes `kind` and `query`. Read-only. |
| `view_screenshot` | A PNG of a viewport: `viewport` (`perspective` or `top`), `maxEdge` (64-2048, default 1024), `camera {location, rotation}`, `frame` (actors), or `preset`: `overview`, `spawn:spy`, `spawn:merc`, `objective:<name>`. The camera is put back afterwards. |
| `plan_render` | A top-down plan PNG, north up, of a `spec` (offline) or of the live map (no spec): pieces, brushes, spawns in team colours, objectives, cameras, security, lights, Event links and zones. `maxEdge`, `options` (storey, labels, grid, ...). |

### Validate

| Tool | Purpose and main arguments |
| --- | --- |
| `map_validate` | Checks the open map, or a `spec` offline, against the wiki. `profile` (`draft` or `release`), `include` (groups), `rules` (ids or prefixes), `modes`, `release`, `trace`, `info`, `limit`. See [Validation](#validation). Read-only. |

### Spec

| Tool | Purpose and main arguments |
| --- | --- |
| `level_spec_validate` | Parses and compiles a `scct.level-spec` offline, runs every rule and draws the plan. `spec` or `path` (a file, or `scct://examples/<name>`), `profile`, `render`, `maxEdge`, `options`, `limit`. Returns `valid`, `summary`, `diagnostics` (each with a JSON `pointer` into the spec), the plan's steps and the plan image. |
| `level_build` | Builds a spec in the open map as one Undo step. `spec` or `path`; `mode` `preview` (default) or `apply`; `sync` `incremental`, `replace` or `merge`; `brushMode` `pieces` or `polygons`; `build` `geometry` (default), `all` or `none`; `screenshot` (default true); `save` (a path in `Packages\MapsEd`); `force` (apply despite errors); `profile`. See [The level spec](#the-level-spec). |

### Intent

Each intent tool changes one part of the level. Its arguments are that part of
the spec, at the top level; they are laid over the spec the map was last built
from (a map without one starts a new spec), and only what changed is built, as
`level_build sync:"incremental"` does, in one Undo step. List items match by
`id` and are updated field by field; `remove` takes ids out. Common arguments:
`mode` (`apply`, the default, or `preview`), `dryRun`, `label`, `remove`,
`existing` (live actors to refer to by id), `build` (`auto`, `geometry`, `all`,
`none`), `screenshot`, `save`, `force`, `mapName` (for a map with no spec yet).
Errors the change brings block it unless `force`; errors the spec already had
are listed as standing and do not.

| Tool | Purpose and main arguments |
| --- | --- |
| `blockout` | Rooms, corridors, vents, crawlways, doorways, stairs, ramps and platforms as Map Design pieces: `pieces`, `doorways`, `storeys`, `vertical`, `brushes`, `surfaces`, `meshes`, `defaults`, `brushMode`. |
| `gameplay` | `spawns` (spy, merc, toggles), `modes` and `removeModes`, `story` (mission, objectives with terminal, flag and bomb triggers, sectors), `diskhunt`, `deathmatch`, `playerCounts`, `targets`. |
| `security` | `alarms`, `devices` (laser, camera, motion, presence, mine) and `camnet` (the Mercenaries' camera loop). |
| `mechanisms` | `doors`, other static-mesh `movers`, `lifts` (with `doorMesh`, `doors`, `carSwitch`), `buttons` (buttons and hacking panels). |
| `events` | `links` (Event to Tag, with delay), `magic` (SMagicEvent documents), `messages`, `shakers`, `noises`, `level` (LevelInfo events at a time), `triggerLights`. |
| `environment` | `lights` (presets ceiling, spot, sun, echelon-spot), `sounds`, `zones`, `volumes`, `ingredients`, `objects`, `meshes`, `vision`, `props`, `fish`, `softbody`, `animated`, `tutorials`. |
| `geometric_events` | `ges` (ledge, hoh, pipe, ladder, zipline, fence, pole) and `geTriggers`. With no arguments it lists the GEs it knows. |
| `map_settings` | `level` (Level Properties: song, deathPawnHeight, motionTracking, events, geActivationMask), the Map Settings (`mapName`, `briefSpy`, `briefMerc`, `legend`, `legendInBrief`, `modes`, or all of them as `mapSettings`), `interface` (menu, briefing, loadingAtt, loadingDef images) and `minimap`. |

### Escape hatches

| Tool | Purpose and main arguments |
| --- | --- |
| `actors_edit` | Direct edits in one Undo step: `edits` of `{action, actor, ...}` with `action` `set` (`properties` in Map JSON form), `move` (`delta`), `turn` (`rotation`), `drop`, `copy` (`offsets`), `delete` (needs `allowDeletes: true`), `copy_properties` (`from`, `only`) or `rename_tag` (`tag`, renaming the Events that use it too). |
| `map_changes_apply` | A `scct.map-changes` `document` as written by hand ([MapAuthoring.md](MapAuthoring.md)), with `baseDirectory` for relative texture paths. |
| `workflow_request` | One bridge op by name: `op` and `args`. See [Bridge ops](#bridge-ops). |
| `editor_exec` | One console `command`. Off unless `AllowRawExec=1`; QUIT, EXIT, MAP NEW/LOAD/SAVE/IMPORT, OBJ SAVEPACKAGE, TRANSACTION and AUDIO are always refused. Console commands skip the stale checks and may not be undoable. |

### Import and media

| Tool | Purpose and main arguments |
| --- | --- |
| `import_to_spec` | Turns a file into a level spec: `source` (a full path, or `"live"` for the open map), `format` (default `auto`), `options` for the importer, `plan`, `maxEdge`. Returns the spec (also saved in the work folder), a report of warnings and assumptions, the compiler's diagnostics and a plan image. Read-only. See [Importers](#importers). |
| `assets_import` | Imports textures and static meshes into the map's own packages and saves them: `items` (1-64) of `{kind: texture|staticmesh, file, package, group, name, ...}`, `save`, `overwrite`. OBJ files are converted to ASE first. The map must be saved under its name first. Not undoable. |
| `media_extract_frames` | Still frames from a video as PNGs, attached as images: `video`, `mode` (`interval`, `count` or `scene`), `intervalS`, `count`, `start`, `end`, `threshold`, `maxEdge`, `sheet` (a contact sheet), `attach`, `outDir`, `backend`. |
| `media_read_image` | Opens an image for the agent to look at, with `crop`, `maxEdge`, `enlarge`, a labelled measurement `grid` (`spacingPx`, or `cellUu` with `uuPerPx`, `origin`), its `palette` and occupancy. |
| `image_convert` | Makes a texture-ready image: power-of-two `size`, or an interface `slot` (Menu, Briefing, LoadingATT0, LoadingDEF0); `fit`, `format` (tga, bmp, png), `alpha`, `background`, `out`, `overwrite`. |

### Release

| Tool | Purpose and main arguments |
| --- | --- |
| `release` | `action`: `check` (read-only: file names, the MapsEd and Maps copies, package names, `<Map>-i.utc` and its interface textures, the `.uas`, the packages the map imports; `base` compares stock packages with a clean install), `sounds` (builds `Packages\Sounds\<Map>.uas` with Interface and `Amb_<Map>`, `quality` default 5.0), `optimise` (Optimise Map Assets' report, or a release copy with `packs`/`suggested`), `package` (the archive's file list) or `interface` (`images` into `<Map>-i`). |

## Bridge ops

`workflow_request` runs one op of the editor's workflow bridge, the same ops
RE+'s own tests drive. Each op has a policy, which the editor enforces whatever
the client asks:

| Policy | Ops |
| --- | --- |
| Read | `editor.state`, `log.tail`, `approval.status`, `blob.read`, `blob.free`, `actors`, `actors.query`, `map`, `map.file`, `settings.inspect`, `authoring.mapid`, `authoring.schema`, `authoring.export`, `authoring.preview`, `assets.search`, `view.screenshot`, `view.capture`, `view.restore`, `mapcheck.run`, `lighting.budget`, `scene.trace`, `ge.list`, `object.inspect`, `mapsettings.get`, `mcp.sidecar.get`, `props.copy`, `design.pieces`, `design.scene`, `design.grid`, `design.view`, `design.bspowners`, `design.builds`, `design.spawns`, `design.clearances`, `placement.selection`, `security.actors`, `light.list`, `objective.actors`, `stage.actors`, `stage.read`, `stage.preview`, `optimise.report`, `magic.classes`, `magic.assets`, `magic.inspect`, `magic.json.export`, `camera.snapshot`, `brush.visibility`, `usages`, `connections`, `surface.brushes`, `mesh.bounds`, `brush.snap.bounds`, `vertex.selection`, `stairs.state`, `tag.preview`, `assembly.capture`, `emitterlib.list`, `emitterlib.packs`, `emitterlib.cache`, `emitterlib.problems`, `emitterlib.categories`, `emitterlib.selected`, `emitterlib.capture`, `skins.settings`, `objectiveplayers.state`, `conveyor.state` |
| Undoable | `mcp.plan`, `undo.call`, `undo.steps`, `redo.steps`, `authoring.apply`, `select`, `design.block`, `design.blockbatch`, `design.align`, `design.layer`, `design.polyflags`, `design.repairflags`, `design.sendtolast`, `design.flags`, `design.group`, `design.translate`, `design.lift`, `design.tempstart`, `design.register`, `design.unregister`, `mcp.sidecar.set`, `placement.drop`, `placement.move`, `placement.turn`, `placement.copy`, `props.paste`, `start.create`, `alarm.locks`, `security.create`, `security.sensor`, `security.link`, `security.outputs`, `security.unlink`, `security.delete`, `security.move`, `stage.apply`, `objectiveplayers.set`, `objectiveplayers.playlevel`, `objectiveplayers.targets`, `conveyor.add`, `conveyor.sync`, `camera.order`, `camera.add`, `magic.component`, `magic.key`, `magic.json.import`, `magic.edit`, `magic.create`, `magic.link`, `objective.add`, `replace`, `tag.rename`, `assembly.place`, `emitterlib.place`, `brush.visibility.set`, `builder.fit`, `builder.fit.brush`, `vertex.portal`, `vertex.snap`, `brush.snap`, `stairs.smooth`, `stairs.remove`, `settings.edit`, `ge.add`, `surface.texture` |
| File | `map.new`, `map.open`, `map.save`, `asset.texture`, `asset.mesh`, `package.save`, `object.set`, `interface.texture`, `minimap.apply`, `uas.build`, `mapsettings.set`, `optimise.release`, `conveyor.prepare`, `emitterlib.save`, `emitterlib.delete`, `emitterlib.update` |
| Build | `design.build`, `build.all` |
| Process | `editor.quit`, `play.level` (needs `AllowPlay=1`) |
| Raw | `exec` (needs `AllowRawExec=1`; use `editor_exec`) |
| Blocked | `light.set` (it writes without a stale check: use `actors_edit`), `design.removestart`, `design.play`, `package.preview`, `map.setfile`, `emitterlib.restore`, `skins.apply`, `skins.remove`, every `preview.*` and `characters.*` op: they open windows or undo blindly. |

Every op that changes something runs as a one-step plan, so approval, dry runs,
the history file and the one Undo step apply to it too. The undo ops and `exec`
run on their own and cannot go inside a plan.

Arguments of the ops made for the MCP server (the others take the shapes
`Reloaded.Editor/WorkflowTools.cpp` gives them):

- `editor.state {}`: `{open, level, generation, revision, contentRevision,
  mapKey, mapFile, map, levelPath, dirty, canUndo, canRedo, undoMark,
  geometryBuilds, version}`.
- `actors.query {classes, tags, names, near {at, radius}, box {min, max},
  selected, fields, limit (500, at most 5,000), cursor}`: `{actors:[{path,
  class, name, tag, event, position, rotation, selected, hidden, values}],
  total, next}`. The LevelInfo, the builder brush and cameras are left out.
- `authoring.schema {classes}`, `authoring.export {}` (to a blob: `{blob,
  size}`), `blob.read {blob, offset, size}` (`{data: base64, eof}`),
  `blob.free {blob}`.
- `authoring.preview` / `authoring.apply {document, baseDirectory}`: the Map
  JSON import; the result adds `identities` (create id to `{path, class}`).
- `settings.inspect {}`, `settings.edit {snapshot, changes}`: Level Properties
  against the snapshot just read.
- `design.block` / `design.blockbatch` take `spec.material`;
  `design.register {pieces, elements}`, `design.unregister {members}`,
  `design.pieces {}`; `mcp.sidecar.get {}` / `mcp.sidecar.set {sidecar}`.
- `placement.move {moves:[{actor, delta}]}`, `placement.turn {changes:[{actor,
  rotation}]}`, `placement.copy {members, copies}`, `props.copy {actor}`,
  `props.paste {targets, properties}`.
- `ge.add {command: ZL|HOH|PIPE|LADDER|FENCE|POLE|LG, points}`, `ge.list {}`.
- `surface.texture {texture, surfaces | brushes, faces}`: faces named by where
  their normal points (`floor`, `ceiling`, `+X`, `-X`, `+Y`, `-Y`; a carved
  room's east wall faces west, `-X`).
- `scene.trace {segments:[{from, to}]}`: `{hits:[{hit, at, normal, actor}]}`.
- `map.new {discard}`, `map.open {path, discard}`, `map.save {path,
  overwrite}`, `build.all {}`, `editor.quit {discard}`.
- `view.screenshot {viewport, maxEdge, camera, frame}`: `{png, width,
  height}`; `mapcheck.run {}`: `{rows:[{severity, group, check, message,
  actors, surfaces}], summary}`; `lighting.budget {}`.
- `asset.texture {file, package, group, name, format, mips, masked,
  alphaTexture, lodSet}`, `asset.mesh {file (.ase), package, group, name,
  collision}`, `package.save {package, overwrite}`,
  `interface.texture {slot, file}`, `minimap.apply {...}`,
  `uas.build {packages, quality}`.
- `undo.call {call}`, `undo.steps {count}`, `redo.steps {count}`,
  `log.tail {since}`, `approval.status {ticket}`, `exec {command}`.
- `mcp.plan {label, pre, steps, post, dryRun}`: steps are `{id, op, args,
  optional}`, and `{"$bind": {"step": "s1", "ptr": "/0/members"}}` anywhere in
  an argument takes the value at that JSON pointer in an earlier step's
  result. `pre` runs first (package loads, imports, saves; not undoable);
  `steps` run as one Undo step and roll back together; `post` runs after
  (builds, surface texturing, Map Check, saves, screenshots), each on its own,
  and an `optional` step there may fail without stopping the rest. Every step
  that deletes actors must come before the first that adds any, or the plan is
  refused: undoing a paste followed by a delete crashed the editor.
  `scct://schema/build-plan` describes it.

## Resources

| URI | What |
| --- | --- |
| `scct://wiki/index` | Start here: the page map, the minimum viable Versus map checklist and which tool adds what. |
| `scct://wiki/{page}` | One digest per wiki page, with class and property names exactly as the wiki writes them: `home`, `spawn-points`, `game-modes`, `map-settings`, `level-properties`, `vision-mode-properties`, `geometric-events`, `events`, `alarms`, `movers`, `elevators`, `lighting`, `material-classes`, `static-meshes`, `sound-actors`, `sound-browser`, `gameplay-objects`, `buttons-and-hacking-panels`, `level-ingredients`, `zones-and-volumes`, `softbody-actors`, `animated-meshes`, `fish`, `map-tutorials`, `map-packaging`. |
| `scct://rules` | Every validation rule with its draft and release severity, its wiki page and what it checks, generated from the checks themselves. |
| `scct://schema/{name}` | JSON Schemas: `level-spec`, `map-changes`, `map-authoring`, `stages`, `smagic-event`, `build-plan`. |
| `scct://docs/{name}` | `tools` (the agent's guide), `map-authoring` and `map-design` (abridged from this folder). |
| `scct://examples/{name}` | Example level specs (see [Examples](#examples)). |
| `scct://editor/state` | Live: the open map's identity and state. |
| `scct://map/summary` | Live: actor counts, spawns per team, missions, security, cameras. |
| `scct://map/validation` | Live: the map checked against the rules (draft). |
| `scct://map/spec` | Live: the level spec last applied to the map (its sidecar), or null. |
| `scct://map/screenshot/latest` | Live: a PNG of the perspective viewport. |
| `scct://map/actor/{path}` | Live: one actor's editable values, such as `scct://map/actor/MyLevel.Light12`. |

Clients can subscribe to the live resources; the server announces a change
after each change it makes.

The digests were written from the wiki's raw pages; each names its source page.
`tools/mcp/build_wiki_resources.ps1` fetches the pages again and lists names and
numbers a digest lacks, for when the wiki changes.

## Prompts

Each prompt is a step-by-step recipe that carries the wiki pages and rules it
needs.

| Prompt | Arguments (required in bold) | What it does |
| --- | --- | --- |
| `create_level_from_description` | **description**, mapName, modes, profile | Designs and builds a complete map from a written description. |
| `create_level_from_media` | **media**, mapName, modes, knownSize, notes | Rebuilds a place from a video, screenshots, photos or a plan image. |
| `create_level_from_file` | **path**, format, mapName, options | Imports a structured file and finishes it into a playable map. |
| `versus_starter` | mapName, roomSize, corridorLength, extras | The RE+ Versus starter layout with the wiki's gaps filled. |
| `add_game_mode` | **mode**, details | Adds Story, Story sectors, Disk Hunt or Deathmatch with its Map Settings entry. |
| `design_security` | area, intensity, details | Alarms, detectors, door locks and the CamNet. |
| `ge_pass` | area, focus | The spies' climbing layer: ledges, hand-over-hand, pipes, ladders, ziplines, fences. |
| `review_map_against_wiki` | profile, focus | Audits the open map and reports by wiki page, changing nothing. |
| `fix_validation` | profile, scope, findings | Applies each finding's fix, dry run first, until the map is clean. |
| `prepare_release` | **mapName**, images, briefing | Naming, Map Settings, interface textures, the `.uas`, package checks and the archive list. |

## The level spec

A level spec (`scct.level-spec`, version 1) describes a whole level as data:
the spaces, who spawns where, what each mode plays, the security, the doors,
the events, the lights and the settings. `level_build` compiles it with the
wiki's defaults and applies it in one Undo step; the intent tools edit parts of
it. Its schema is `scct://schema/level-spec`. Unknown fields are refused, and
every problem is reported at its JSON pointer (`/security/devices/2`).

| Section | What it holds |
| --- | --- |
| `format`, `version` | `"scct.level-spec"`, `1` (required). |
| `map` | `name` (capitals, as in Map Settings) and `file` (default: the first four letters of the name plus `D`). |
| `units` | `unit` (`uu`, `m` or `ft`) and `uuPerMetre` (100). |
| `profile` | `draft` or `release`: how strictly it is checked. |
| `defaults` | `construction` (Carve), `thickness` (16), `material`, `lights` (`rooms`: a ceiling light per room, or `none`), `startsPerTeam` (4). |
| `storeys`, `zones` | Floors `{id, z, name}`; zones `{id, locationName, spaces, portals}`, each a ZoneInfo with portals where zones meet. |
| `geometry` | `pieces` (Room, Corridor, Vent, Crawlway, Doorway, Platform, Stairs, Stairs L, Stairs U, Spiral, Ramp; with `size`, `at`, `storey`, `yaw`, `attach`), `doorways`, `vertical` (stairs or lifts between storeys), `brushes` (polygon brushes), `surfaces` (per-face textures), `meshes`. |
| `spawns` | `spy` and `merc` (`{in, count, facing}` or a list of starts), `toggles`. |
| `gameplay` | `modes`, `story` (mission, objectives and their triggers, sectors), `diskhunt`, `deathmatch`, `playerCounts`, `targets`. |
| `security` | `alarms`, `devices`, `camnet`. |
| `mechanisms` | `doors`, `movers`, `lifts`, `buttons`. |
| `events` | `links`, `magic`, `messages`, `shakers`, `noises`, `level`, `triggerLights`. |
| `environment` | `lights`, `sounds`, `volumes`, `ingredients`, `objects`, `vision`, `props`, `fish`, `softbody`, `animated`, `tutorials`. |
| `ges`, `geTriggers` | Geometric events and the triggers that switch them. |
| `settings` | Map Settings: `mapName`, `briefSpy`, `briefMerc`, `legend`, `legendInBrief`, `modes`. |
| `level`, `interface` | Level Properties; the four interface images. |
| `release`, `existing`, `extra`, `provenance` | Release notes; `existing.refs` (live actors the spec did not make); `extra.operations` (raw map-changes operations); where the level came from (`source`, `frames`, `notes`). |

**Conventions.**

- Lengths are in Unreal units unless `units` says otherwise (about 100 to the
  metre; a standing player is 180 tall). Z is up, +X east, +Y north.
- Yaw is in degrees: east 0, north 90. (Engine values use 65,536 per turn.)
- Every `id` is 1 to 63 letters, digits and underscores, not starting with a
  digit, unique in the spec ignoring case. It becomes the actor's Tag and its
  create id, so alarms, links and `{"$ref": id}` name things by id.
- A position is `[x, y, z]` (z may be `"floor+N"` or `"ceiling-N"`, or left
  out for the usual height of that thing), `{in: pieceId, u, v, h}` (u and v
  from -0.5 to 0.5 across and along the piece, h `"floor+N"` or
  `"ceiling-N"`), `{near: id, offset: [dx, dy, dz]}` or `{doorway: id}`.

### A minimal level

Two spawn rooms, a vault with a terminal, two corridors and a two-camera
CamNet:

```json
{
  "format": "scct.level-spec",
  "version": 1,
  "map": {"name": "DEPOT"},
  "geometry": {
    "pieces": [
      {"id": "SpyRoom", "kind": "Room", "size": [768, 768, 256], "at": [-1152, 0]},
      {"id": "WestHall", "kind": "Corridor", "size": [192, 384, 256], "attach": {"to": "SpyRoom", "side": "east"}},
      {"id": "Vault", "kind": "Room", "size": [768, 768, 320], "at": [0, 0]},
      {"id": "EastHall", "kind": "Corridor", "size": [192, 384, 256], "attach": {"to": "Vault", "side": "east"}},
      {"id": "MercRoom", "kind": "Room", "size": [768, 768, 256], "at": [1152, 0]}
    ]
  },
  "spawns": {
    "spy": {"in": "SpyRoom", "count": 4, "facing": {"toward": "Vault"}},
    "merc": {"in": "MercRoom", "count": 4, "facing": {"toward": "Vault"}}
  },
  "gameplay": {
    "modes": ["story"],
    "story": {
      "mission": {"id": "Mission", "description": "Hack the vault terminal", "descriptionDef": "Defend the vault"},
      "objectives": [{
        "id": "VaultObjective", "name": "Vault",
        "triggers": [{"id": "VaultTerminal", "kind": "terminal", "at": {"in": "Vault", "u": 0.3}, "dominationCaptureTime": 10}]
      }]
    }
  },
  "security": {
    "camnet": [
      {"id": "CamVault", "camName": "Vault", "at": {"in": "Vault", "u": -0.45, "v": 0.45, "h": "ceiling-32"}, "first": true},
      {"id": "CamHall", "camName": "East hall", "at": {"in": "EastHall", "h": "ceiling-32"}}
    ]
  }
}
```

From these few lines the compiler makes:

- five carved Map Design pieces, the corridors flush against the rooms' walls,
  and a ceiling light in each room;
- four PlayerStarts a team (TeamNumber 0 for the spies, 1 for the mercs) on a
  128-unit grid at floor+48, facing the vault;
- an SMission with GameMode GM_Multi listing the SObjective by name, and the
  SObjective listing its SComputerObjectiveTrigger, placed at floor+78 with
  DominationCaptureTime 10;
- two SCamNetwork cameras linked NextCam/PrevCam in a closed loop, one
  bFirstCam, collision on;
- the file name `DepoD`.

`level_spec_validate` reports no errors in either profile and two warnings:
the map has no Disk Hunt and no Deathmatch mission. `level_build` with
`mode:"apply"` builds it.

### How it is built

The compiler turns a spec into one plan (`scct://schema/build-plan`) that the
editor runs:

1. **Before** (not undoable): package loads, texture imports and package
   saves.
2. **One Undo step**, in this order, rolled back together if any part fails:
   - the blockout pieces, in batches of up to 64;
   - every actor through one Map JSON document (the same import as **View >
     Import Map from JSON**);
   - objective player counts;
   - geometric events (`GE ADD`);
   - Level Properties;
   - the Map Design records, so the **Map Design** panel can keep editing the
     pieces;
   - the spec itself, stored with the map (the sidecar, `scct://map/spec`).
3. **After**, each on its own: the geometry build, Map Check, Map Settings and
   interface textures (when given; they may be unsupported, see
   [Status](#status-and-known-limitations)), the save and a top-view
   screenshot.

**Sync.** The map remembers the spec it was built from and which actors and
brushes each id made. `sync:"incremental"` (the default once a map has a
stored spec) builds only the difference: new ids are created, changed pieces
rebuilt, changed actors updated (or recreated when their class, team or Tag
changes), and ids the spec dropped are deleted. `replace` (the default
otherwise) deletes what the last build made and builds the spec afresh.
`merge` lays the given spec over the stored one first, so a fragment adds to
the level. Deletes only ever touch what an earlier build made.

**Brush mode.** `pieces` (default) makes Map Design pieces you can keep editing
in the panel. `polygons` makes plain polygon brushes, for per-face textures.

### Examples

| Example | What it shows |
| --- | --- |
| `scct://examples/versus-starter` | The Map Design Versus starter layout with the wiki's gaps filled: 4 starts a team, the terminal at floor+78, a three-camera CamNet loop, zones and all three modes. |
| `scct://examples/three-modes-sectors` | Story in three sectors (zone missions under a chained top mission), doors and alarms. |
| `scct://examples/diskhunt-arena` | An arena built for Disk Hunt: twelve disks on the floor and on two raised platforms reached by ramps, the score limit and handicap in Map Settings. |
| `scct://examples/security-camnet` | Three alarms fed by lasers, a camera, a motion sensor, a presence detector and a mine, locking a door and an objective; a four-camera CamNet. |
| `scct://examples/elevator-3floors` | Three storeys joined by stairs and an SLift with its doors and switches. |

Every example plays all three modes, and all five compile with no errors or
warnings in both profiles. The meshes and sounds in all but `versus-starter`
are placeholders in the map's own packages: import real ones (`assets_import`)
before applying those examples.

## Building a level from any input

**What the agent does, and what the server does.** The server never interprets
pictures, video or prose. The agent looks at the frames and images, reads the
description, decides what the place is (rooms, doors, heights, objectives) and
writes a spec, recording its sources in `provenance`. The server extracts
frames, draws measurement grids, reads exact formats, compiles the spec with
the wiki's defaults, checks it, builds it and shows the result to compare with
the source.

The usual loop:

1. `editor_status`; read `scct://wiki/index` and `scct://rules`.
2. Turn the input into a spec: by hand from a description, from frames and
   images, or with `import_to_spec`.
3. Calibrate the scale (below).
4. `level_spec_validate` and `plan_render`: fix errors, compare the plan with
   the source.
5. `map_file action:"new"`, then `level_build mode:"preview"`, then
   `mode:"apply"`.
6. `view_screenshot` (overview, spawns, objectives) against the source.
7. `map_validate`, fixing with each finding's `fix` or by editing the spec and
   building again (incremental).
8. `map_file action:"save_as"`, `editor_restart`, then `map_settings`,
   `map_validate profile:"release"` and `release`.

The `create_level_from_*` prompts walk an agent through this.

**Video.** `media_extract_frames` takes frames at intervals, a fixed count, or
at each cut (`scene`), and can tile them into a contact sheet. It reads video
through Windows Media Foundation, and falls back to ffmpeg (`[Mcp] Ffmpeg`, or
`PATH`) for files Media Foundation cannot read.

**Images.** `media_read_image` shows an image (or a crop of it) with a labelled
grid in pixels or Unreal units, its main colours, and a cell-by-cell map of
what is drawn, which turns a plan into a grid the agent can transcribe.

**Scale.** Find something of known size and measure it in pixels: a standing
player is about 180 units tall and 96 wide, a door about 96 by 128, a metre
about 100 units, a stair riser at most 35. Then `uuPerPx` = its size in units
divided by its size in pixels. A spec can also be written in metres or feet
(`units`).

### Importers

`import_to_spec` reads these formats, chosen by extension and contents or by
`format`:

| Format | Files | What it becomes |
| --- | --- | --- |
| `level-spec` | `.json` | Checked and returned with its defaults filled in. |
| `map-changes` | `.json` | Its operations become `extra.operations`. |
| `map-authoring` | `.json`, or `"live"` | A map export (**View > Export Map to JSON**, or the open map): `existing.refs` to extend the map in place (`mode: "refs"`, the default) or a spec that rebuilds it (`mode: "recreate"`). |
| `grid` | `.txt`, `.grid`, `.csv` | An ASCII or CSV grid, one character per cell (below). |
| `image` | `.png`, `.bmp`, `.tga` | A raster floor plan read by colour into the grid importer, with an overlay of how each cell was read. JPEG, GIF and TIFF are decoded with Windows' codecs first. |
| `svg` | `.svg` | Rects and polygons are rooms; `data-kind` or `class` marks starts, objectives, cameras, doors and GEs; layers are storeys. |
| `tiled` | `.tmj`, `.tmx` | Tile layers become the grid, object layers rooms and markers. |
| `geojson` | `.geojson`, `.json` | Polygons are rooms (holes are pillars), Points markers, by `properties.kind`. |
| `csv` | `.csv`, `.tsv` | A table of actors (class, x, y, z, yaw, id, tag, event, then any properties). |
| `t3d` | `.t3d` | Unreal T3D: CSG brushes as polygon brushes, volumes, starts, lights, meshes and other actors. |
| `vmf` | `.vmf` | A Hammer map. SCCT's world is solid, so a hull is carved and the brushes are added back; scale 2.5 and Y mirrored by default. |
| `quake` | `.map` | Quake, Half-Life, Quake 3 and Doom 3 maps, the same way; scale 3.2 by default. |
| `obj`, `ase` | `.obj`, `.ase` | A mesh's size and parts, as an asset for `assets_import` (never level geometry). |
| `r6v` | `.json` | R6 Vegas converter placements as StaticMeshActor props. |

The grid legend (`options.legend` changes it):

```
#  solid              .  floor             S  spy start      M  merc start
O  terminal objective F  flag              Z  flag drop zone B  bomb target
K  disk               D  door              V  vent           C  camera
L  light              A  ammo              H  infirmary      N  CamNet camera
X  laser
```

Row 1 is the north edge. Each cell is 128 units (`cell`); `--- storey N ---`
starts another floor 384 units up (`storeyHeight`). Floor cells are cut into
the fewest large rectangles, each a Room 320 tall (`roomHeight`), or a
Corridor when its short side is 256 or less (`corridorMax`); V cells are vents.

A raster plan uses the same meanings by colour: black solid, white floor, blue
spy, red merc, green terminal, magenta door, cyan vent and so on
(`options.colors {"#rrggbb": meaning}` changes them; `media_read_image`'s
palette shows a plan's colours).

An import is a starting point. Read its report's assumptions and warnings,
compare the plan with the source, then check and build as above.

## Validation

`map_validate` checks the open map, or a spec offline, against the wiki rules.
`scct://rules` lists every rule with its severity in each profile,
its wiki page and what it checks.

**Profiles.** `draft` (the default) suits a map in progress. `release` is
stricter: fewer than 3 starts a team, missing Map Settings and interface
textures, isolated rooms and objectives the spies cannot reach become errors.

**Groups.** `include` runs only some, and makes only their editor calls:

| Group | What it covers |
| --- | --- |
| `wiki` | Spawns, Story, Disk Hunt, Deathmatch, objectives, CamNet, actors, Level Properties, GE measurements. |
| `design` | The Map Design pieces: layout, traversal (steps, slopes, clearance) and whether objectives can be reached. |
| `security` | Alarms and detectors and their wiring. |
| `mapcheck` | RE+'s **Build > Map Check**: zone leaks, starts in the void, floating meshes. |
| `lighting` | The Lighting Budget. |
| `release` | Map Settings, the `<Map>-i` interface textures, file and package names, the `.uas`. |

**Findings.** Each has `severity` (error, warning or info), `rule`,
`wikiPage` (read it as `scct://wiki/<page>`), `message`, `actors` (paths, or
spec ids offline) and usually `fix`: the tool call that repairs it. Errors come
first. On the live map, `trace` (on by default) measures the built level: the
floor under terminals, disks and GEs, so heights and clearances are checked
for real.

`level_spec_validate` and `level_build` run the same rules on the compiled spec
before anything reaches the editor.

## Limits and safety

What the server and the editor never do, whatever the agent asks:

- **Turn themselves on.** Only you can tick **Enable MCP server**, and
  `AllowRawExec` and `AllowPlay` are set only in the INI.
- **Write stock packages.** Package writes are limited to the map's own names:
  `<Map>`, `<Map>-i`, `Amb_<Map>` and `<Map>_<anything>` (`<Map>_TXT`,
  `<Map>_STM`).
- **Delete without asking for it.** A call deletes actors only when it says
  `allowDeletes: true` (`actors_edit`, Map JSON version 3), or when an
  incremental build removes actors its own earlier build made.
- **Discard unsaved work.** `map_file new` and `open` refuse while the map has
  unsaved changes, unless the call says `discardUnsaved`.
- **Overwrite files.** Replacing another map's file, a package file or an
  output image needs `overwrite: true`, and replaced maps and packages are
  backed up first.
- **Write elsewhere.** Maps are saved in `Packages\MapsEd` under ASCII names
  without spaces. The editor writes packages in `Packages\Textures`,
  `Packages\StaticMeshes` and `Packages\Sounds` under the map's names, release
  copies in `Releases\<Map>` (as Optimise Map Assets does), and its own files in
  `System\ReloadedEditor\Mcp`. The media and import tools write only in the work
  folder and `WriteRoots`, and run only the ffmpeg you configured.
- **Run console commands.** `editor_exec` is off unless `AllowRawExec=1`, and
  quitting, map loads and saves, package saves, transactions and audio
  commands are refused even then.
- **Open windows or skip checks.** Ops that open windows, undo blindly or
  write without a stale check are blocked over the pipe.

The pipe accepts connections only from your own Windows account on this
computer, one client at a time.

## Status and known limitations

The server, the level spec compiler, the rules, the importers and the media
helpers are covered by the model and server test suites (`tools/test_mcp.cmd`,
[tests/README.md](../tests/README.md#mcp-server)). The editor side (the pipe,
the ops and the menu) has been checked against a stubbed editor; **the native
probe and the end-to-end run in a real editor have not passed yet**, so treat
what follows as the current state, not a promise:

- **Geometric events.** GEs can be added (`GE ADD`), not deleted or given GE
  IDs over the bridge: delete them in GE mode and set IDs with **Set GE ID**.
  `ge.list` knows only the GEs Map Design or the server added. Whether a GE
  added inside a plan is undone with the rest of the plan is not yet confirmed.
- **Map Settings.** `mapsettings.set` is not supported: `map_settings` and
  `level_build` report `applied: false` with a checklist of the values and the
  manual steps (right-click empty viewport space > **Map Settings**).
  `mapsettings.get` reads what it can from `<Map>-i.utc` on disk. Level
  Properties and the interface textures go through paths RE+ already uses.
- **Restarting the editor.** `editor_restart` needs the editor to close without
  a question; if one appears (for example about unsaved changes), answer it,
  or restart the editor yourself when told.
- **Video.** Media Foundation reading has not run on Windows yet; ffmpeg is the
  fallback (`[Mcp] Ffmpeg` or `PATH`). Windows N editions without Media
  Foundation need ffmpeg.
- **Meshes.** `asset.mesh` imports ASE files with `STATICMESH IMPORT`; the
  ASE that OBJ files are converted to has not been tried in the editor's
  importer yet, and collision can only be checked, not set. Mesh packages are
  saved as `Packages\StaticMeshes\<Map>_STM.usx`.
- **Other unsupported ops.** `object.inspect` and `object.set` (non-actor
  objects) refuse with manual steps. Brush movers are not supported; movers
  are static meshes.
- **Engine names not yet confirmed in the editor:** `GM_DiskHunt`,
  `ActionBeforeLock_None` and the shape of `UseRelativeRotation`. When the
  editor's schema differs, `level_build` uses its enum choices.
- **Lifts.** The external door movers' `InitialState TriggerControl`, the car
  switch riding the car (AttachTag) and the SLiftDoors moving with the car
  follow the wiki as far as it goes and need an in-game test.
- **Incremental builds** do not notice edits you made by hand since the last
  build, and an Undo of a build leaves its Map Design records and stored spec
  behind (the next build recreates what is missing).
- **Release.** `release action:"package"` lists the archive but does not write
  it: use **File > Package Map for Sharing**. Whether a `.uas` holds Interface
  and `Amb_<Map>` is read from the build log RE+ writes beside it. A map saved
  under a new name must be reopened before its `.uas` can be built.
- **Approval** has no time limit in the editor; the window stays until you
  answer or the client goes away.
- The DLL sends no change events of its own yet: live resources are marked
  updated after each change the server makes, not after your own edits.

## Troubleshooting

| Symptom | What to do |
| --- | --- |
| `not_connected`, or "No editor connected" | The editor is not running, or its server is off. Start the editor and tick **RE+ Tools > MCP > Enable MCP server**, or let the agent use `editor_launch` once `Enabled=1`. |
| `disabled` | The server was turned off. Tick **Enable MCP server** again. |
| The client cannot start the server | Check the path in the client's configuration points at `System\Reloaded.Editor.Mcp.exe`, and that the file exists (it comes with RE+ 2.7.0 and later). Run it with `--self-test` in a command prompt. |
| `protocol_mismatch` | The exe and the DLL come from different RE+ versions. Install both from the same release, then restart the MCP client. |
| `busy` with a reason | See [How calls run](#how-calls-run): close the dialog or menu, finish the drag, answer a startup window, restore a minimised editor. |
| `busy` / `another_client` | Another client holds the editor. Close it, or use **Disconnect client**. |
| `invalid` / `several_editors` | Several editors are running; `editor_status` lists them and takes `pid`. |
| `stale` | The map changed since the agent read it. It should read again and retry. |
| `timeout` / `awaiting_approval` | The change is waiting in the review window. Answer it; the agent continues with `approval_wait`. |
| `denied` | You cancelled the review. |
| `dialog_blocked` | A dialog opened during the change; it was answered and the change undone. The details name it. |
| `refused` / `raw_exec_off` or `play_off` | Set `AllowRawExec=1` or `AllowPlay=1` in the INI with the editor closed, only if you want to allow that. |
| `refused` / `unsaved_changes` | Save the map, or tell the agent it may discard the changes. |
| `refused` / `not_map_owned` | The package is not the map's own; use `<Map>_TXT`, `<Map>_STM` or another `<Map>_` name. |
| A build or `.uas` times out in the client | Raise the client's tool timeout; the editor finishes the build regardless, and `editor_status` shows the result. |
| `editor_launch` waits and fails | A startup window (an update offer, crash recovery, a missing package) is waiting for you; answer it, then `editor_status`. `System\Reloaded_Editor.exe.log` says why the launcher failed. |
| Video frames fail | Install ffmpeg and set `[Mcp] Ffmpeg` to its `ffmpeg.exe`, or put it on `PATH`. |
| Output paths refused | Leave the path out to write in the work folder, or add the folder to `[Mcp] WriteRoots`. |

The bridge's own messages go to RE+'s log as lines starting `MCP:`;
`editor_log` shows the engine's lines to the agent, and the history folder has
every change's request and reply.

## Verification

`tools/test_mcp.cmd` (MSVC) and `tools/test_mcp.sh` (g++) build and run every
MCP test suite and the server's `--self-test`. The native probe's MCP section
and the end-to-end run against a real editor are described in
[tests/README.md](../tests/README.md#mcp-server) and
[tests/McpBridgeNativeNotes.md](../tests/McpBridgeNativeNotes.md).
