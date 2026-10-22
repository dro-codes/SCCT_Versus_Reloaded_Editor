# MCP server

`Reloaded.Editor.Mcp.exe` is an [MCP](https://modelcontextprotocol.io) server
for the RE+ editor. It lets an AI agent such as Claude Code or Claude Desktop
read, edit and build Versus maps in the running editor.

The agent does the thinking: it reads the wiki digests, which carry every rule
of the
[SCCT Versus Reloaded wiki](https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki),
and decides what to change. The server does the exact work: it previews each
change, applies it in one Undo step, builds the geometry, runs Map Check and
takes screenshots.

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
`Reloaded.Editor.Mcp.exe --self-test` checks the tools and resources it serves;
neither needs the editor. Run with no arguments, it waits for an MCP
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

The first two are also in **RE+ Options** (F12) under **MCP Server**, and every
item can be run from **Help > Command Palette** (Ctrl+Shift+P) like any other
menu command. The settings are kept in `System\Reloaded_Editor.ini`:

```ini
[Mcp]
Enabled=0
RequireApproval=0
AllowRawExec=0
AllowPlay=0
HistoryMaxMB=200
WriteRoots=
```

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | 0 | The pipe is open. The menu item sets it. |
| `RequireApproval` | 0 | Each change waits for **Approve** in the editor. The menu item sets it. |
| `AllowRawExec` | 0 | `editor_exec` may run console commands. Only the INI sets it. |
| `AllowPlay` | 0 | The `play.level` op may start Play Level. Only the INI sets it. |
| `HistoryMaxMB` | 200 | The most the call history may take, 1 to 100,000 MB; the oldest files go first. |
| `WriteRoots` | empty | Extra folders, separated by `;`, that may be written besides the work folder: by a release copy (besides `<Game>\Releases`) and by an `editor_exec` command's `FILE=`. |
| `WorkDir` | `%ProgramData%\REPlusMcp` | The server's work folder. The editor allows a release copy and an `editor_exec` `FILE=` there too. Not written by default: add `WorkDir=<folder>` to move it. |

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
server with its 14 tools and its resources.

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
call's time runs out. Two reasons come back to the agent at once instead,
since waiting cannot help and the server runs one request at a time:
`approval_pending` (the agent should call `approval_wait` with the ticket the
error carries) and `another_client`.

| Reason | What is going on |
| --- | --- |
| `modal` | A dialog or message box is open in the editor, whoever opened it; the reply names it when it can. |
| `menu` | A menu is open. |
| `drag` | You are dragging in a viewport. |
| `transaction` | An editor operation is still open. |
| `mover_preview` | The Mover Workbench is playing a preview, which moves the mover outside Undo and puts it back when it ends; a save or an edit now would keep or lose the passing pose. Reads still run. |
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
name). If a part of that step
fails, every map change of the call is rolled back and no Undo step is left; a
failure after it (the build, Map Check, a save, or a dialog after a file was
written) keeps the map changes and says so with `rolledBack: false`.
Saves, builds, package and texture imports, and screenshots are not Undo steps.
To take a call back:

- **Edit > Undo** or Ctrl+Z, as for any change;
- **RE+ Tools > MCP > Undo last MCP call**;
- the agent's `editor_undo`, which with no arguments undoes the server's
  newest call that is still undoable (called again, the one before it). It
  refuses rather than undo something newer that you did, and undoes nothing
  when this server has made no change: it never undoes blindly. `steps`
  undoes the newest Undo steps whatever made them.

An MCP call is undone as a whole only while its steps are the newest in the
Undo buffer; otherwise it is refused (`not_newest`) and Redo is kept.

**Dry runs.** Every tool that changes something takes `dryRun: true`: the editor works out the same change, returns its
preview rows (the Map JSON preview's **Add**, **Modify**, **Move**, **Delete**,
**Link**, **Package** and **Surface** rows) and changes nothing.

**Dialogs.** While a call runs, a dialog it opens is answered: a known one with
its known answer, any other with its most cautious button. That button is
chosen by its control id, in the order Cancel, No, Close, Abort, so that a
translated message box (`&Nein`) is answered the same way; then by the English
labels of a dialog with ids of its own; OK only when it is the only button. A
dialog with no safe button is closed, as Escape closes it; no other button is
ever pressed. The call then stops as `dialog_blocked`: a plan stops at the step
that raised the dialog and runs nothing after it, optional steps included. A
dialog inside the plan's one Undo step (`steps`) rolls that step back, as any
failure there does. Otherwise the call's map changes are undone afterwards,
unless a step had already written a file (a save, an interface texture, a
package import or save, a release copy) or opened a map: then they are kept,
so the map in the editor and the file stay alike, and the reply lists what was
written in `wrote` and says how to take the map changes back (`undo.call`).
Files and packages written stay either way, and geometry a build made stays
until the next build. The reply's message says what happened in words, and
its details in fields:

| Detail | What it says |
| --- | --- |
| `dialogs` | Each dialog: its title, text and the button pressed (`button`, `buttonId`), or `(closed)`. |
| `stoppedAt` | The plan step that raised it: `{step, op, section}` (`pre`, `steps` or `post`). |
| `ran` | The steps that finished, in order; the one that raised the dialog is among them if it finished. |
| `wrote` | The finished steps that wrote a file or package or opened a map: `{step, op, section, file}`. Undo does not reach these. |
| `built` | The build steps that ran. |
| `rolledBack` | `true` when the call's map changes were undone, `false` when they were kept. |
| `undoneSteps`, `undoSteps` | How many Undo steps were undone, and how many of the call's are still in the buffer. |
| `cause` | When the call failed as well: its own error, `{code, message}`. |

Progress windows are left alone. A call that arrives while another program's
or RE+'s own dialog is open waits (`busy` / `modal`) instead of running inside
it.

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
the editor finishes. A tool that waits without the editor (`editor_launch`,
`editor_restart`'s wait for the editor to close, `approval_wait`) stops at its next check or progress report, so the next request
does not queue behind it.

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

14 tools in three groups. Each tool's own description, as the client shows it,
is the full reference; this section gives each one's purpose and its main
arguments.

Every tool that changes something takes `dryRun` and `label`, and returns
`summary`, `created`, `changed`, `warnings`, `undo` (`{call, steps}`;
`editor_undo` takes the `call`) and `next` (suggested follow-up calls).
Pictures (screenshots) come back as MCP image content.
Errors come back as tool results marked `isError`, with `code`, `reason`, a
`hint` and whether retrying can help. An error carries those details as
`structuredContent` only when they fit the tool's `outputSchema`; otherwise
they are in the text block, since
clients validate any `structuredContent` they get, errors included.

To keep `tools/list` small (about 22 KB for the 14 tools), the
descriptions give each argument's shape and point at two documents the
agent reads as resources: `scct://docs/tools` (the conventions every tool
shares) and `scct://docs/ops` (the bridge ops).

### Session

| Tool | Purpose and main arguments |
| --- | --- |
| `editor_status` | Start here. Connection, the open map, unsaved changes, Undo state, approval, the `[Mcp]` settings and every editor with a pipe. `pid` picks an editor when several run. Read-only. |
| `editor_launch` | Starts `Reloaded_Editor.exe -reMcp` and waits (`waitSeconds`, default 180) for its pipe; `map` opens a map once started. Refuses when `Enabled` is 0 unless `force`. With `-reMcp` the editor skips its startup update check. |
| `editor_restart` | Saves (`save`, default true), closes the editor, starts it again and reopens the map. The wiki requires this once a new map is first saved, before Map Settings can be edited. Refuses a map that was never saved. |
| `editor_undo` | No arguments: undoes this server's newest call that is still undoable, and nothing when it has made none. `call` undoes that MCP call; `steps` undoes that many Undo steps whatever made them; `redo` with `steps` redoes (and makes a redone call undoable again). |
| `approval_wait` | Keeps waiting for the review window: `ticket` (required), `timeoutSeconds` (default 50). |
| `editor_log` | The editor's log (last 2,000 lines): `level` (all, warning, error), `contains`, `since`, `newOnly`, `max`. Read-only. |
| `map_file` | `action`: `new`, `open`, `save` or `save_as`. `path` (a bare name such as `AquaD` is looked up in `Packages\MapsEd`), `mapName` (save_as names the file after its first four letters plus `D`), `discardUnsaved`, `overwrite`. |
| `map_build` | `what`: `geometry` (default, as Build Geometry) or `all` (the full build with lighting, without the Lighting Budget question); then Map Check (`check`, default true). |

### Inspect

| Tool | Purpose and main arguments |
| --- | --- |
| `map_query` | Reads the map. `what`: `summary`, `actors`, `actor`, `level`, `schema`, `assets`, `wiring`, `spawns`, `missions`, `security`, `cameras`, `lights`, `stages`, `design` or `ges`. `actors` filters by `classes`, `tags`, `names`, `near {at, radius}`, `box {min, max}`, `selected`, with `fields` (names or `"all"`), `limit` and `cursor`; `actor` takes `actor` (path, name or `{path, class}`) and `includeSchema`; `assets` takes `kind` and `query`. Read-only. |
| `view_screenshot` | A PNG of a viewport: `viewport` (`perspective` or `top`), `maxEdge` (64-2048, default 1024), `camera {location, rotation}`, `frame` (actors), or `preset`: `overview`, `spawn:spy`, `spawn:merc`, `objective:<name>`. The camera is put back afterwards. |

### Escape hatches

| Tool | Purpose and main arguments |
| --- | --- |
| `actors_edit` | Direct edits in one Undo step: `edits` of `{action, actor, ...}` with `action` `set` (`properties` in Map JSON form), `move` (`delta`), `turn` (`rotation`), `drop`, `copy` (`offsets`), `delete` (needs `allowDeletes: true`), `copy_properties` (`from`, `only`) or `rename_tag` (`tag`, renaming the Events that use it too). |
| `map_changes_apply` | A `scct.map-changes` `document` as written by hand ([MapAuthoring.md](MapAuthoring.md)), with `baseDirectory` for relative texture paths. Its `texture` and `save` operations write only the map's own packages, `<Map>_<name>` (or a texture into `MyLevel`): another package is refused as `refused` / `not_map_owned` (or `package_name`, `interface_package`), and any before the map's first save as `unsaved_map`. A `load` alone is allowed. |
| `workflow_request` | One bridge op by name: `op` and `args`. `scct://docs/ops` lists every op with its policy and arguments; see [Bridge ops](#bridge-ops). |
| `editor_exec` | One console `command`. Off unless `AllowRawExec=1` (else `refused` / `raw_exec_off`). Even then it is refused (see [Limits and safety](#limits-and-safety) for the whole list) for commands that quit, crash or relaunch the editor, run a file of commands, replace, save, load, import or export the map, packages or files, undo blindly or reach the sound driver (`denied_command`); a `FILE=` outside the work folder and `WriteRoots` (`outside_write_roots`); a `PACKAGE=` other than the map's own (`not_map_owned`); and more than one command (`invalid`). The description lists the editor's deny list. Console commands skip the stale checks and may not be undoable. |

## Bridge ops

`workflow_request` runs one op of the editor's workflow bridge, the same ops
RE+'s own tests drive. The agent reads the list, with each op's policy and
arguments, as `scct://docs/ops`. Each op has a policy, which the editor
enforces whatever the client asks:

| Policy | Ops |
| --- | --- |
| Read | `editor.state`, `log.tail`, `approval.status`, `blob.read`, `blob.free`, `actors`, `actors.query`, `map`, `map.file`, `settings.inspect`, `authoring.mapid`, `authoring.schema`, `authoring.export`, `authoring.preview`, `assets.search`, `view.screenshot`, `view.capture`, `view.restore`, `mapcheck.run`, `lighting.budget`, `scene.trace`, `ge.list`, `object.inspect`, `mapsettings.get`, `mcp.sidecar.get`, `props.copy`, `design.pieces`, `design.scene`, `design.grid`, `design.view`, `design.bspowners`, `design.builds`, `design.spawns`, `design.clearances`, `placement.selection`, `security.actors`, `light.list`, `objective.actors`, `stage.actors`, `stage.read`, `stage.preview`, `optimise.report`, `magic.classes`, `magic.assets`, `magic.inspect`, `magic.json.export`, `camera.snapshot`, `brush.visibility`, `usages`, `connections`, `surface.brushes`, `mesh.bounds`, `brush.snap.bounds`, `vertex.selection`, `stairs.state`, `tag.preview`, `assembly.capture`, `emitterlib.list`, `emitterlib.packs`, `emitterlib.cache`, `emitterlib.problems`, `emitterlib.categories`, `emitterlib.selected`, `emitterlib.capture`, `skins.settings`, `objectiveplayers.state`, `conveyor.state`, `prefab.instances`, `prefab.counts`, `objimport.preview`, `mover.selected`, `mover.state`, `mover.outline`, `mover.candidates` |
| Undoable | `mcp.plan`, `undo.call`, `undo.steps`, `redo.steps`, `authoring.apply`, `select`, `design.block`, `design.blockbatch`, `design.align`, `design.layer`, `design.polyflags`, `design.repairflags`, `design.sendtolast`, `design.flags`, `design.group`, `design.translate`, `design.lift`, `design.tempstart`, `design.register`, `design.unregister`, `mcp.sidecar.set`, `placement.drop`, `placement.move`, `placement.turn`, `placement.copy`, `props.paste`, `start.create`, `alarm.locks`, `security.create`, `security.sensor`, `security.link`, `security.outputs`, `security.unlink`, `security.delete`, `security.move`, `stage.apply`, `objectiveplayers.set`, `objectiveplayers.playlevel`, `objectiveplayers.targets`, `conveyor.add`, `conveyor.sync`, `camera.order`, `camera.add`, `magic.component`, `magic.key`, `magic.json.import`, `magic.edit`, `magic.create`, `magic.link`, `objective.add`, `replace`, `tag.rename`, `assembly.place`, `emitterlib.place`, `brush.visibility.set`, `builder.fit`, `builder.fit.brush`, `vertex.portal`, `vertex.snap`, `brush.snap`, `stairs.smooth`, `stairs.remove`, `settings.edit`, `ge.add`, `surface.texture`, `mover.path`, `mover.settings`, `mover.wire`, `prefab.selectinstance`, `prefab.selectall`, `prefab.place` |
| File | `map.new`, `map.open`, `map.save`, `asset.texture`, `asset.mesh`, `package.save`, `object.set`, `interface.texture`, `minimap.apply`, `uas.build`, `mapsettings.set`, `optimise.release`, `conveyor.prepare`, `emitterlib.save`, `emitterlib.delete`, `emitterlib.update`, `objimport.import`, `prefab.save`, `prefab.update`, `prefab.break` |
| Build | `design.build`, `build.all` |
| Process | `editor.quit`, `play.level` (needs `AllowPlay=1`) |
| Raw | `exec` (needs `AllowRawExec=1`; use `editor_exec`) |
| Blocked | `light.set` (it writes without a stale check: use `actors_edit`), `design.removestart`, `design.play`, `package.preview`, `map.setfile`, `emitterlib.restore`, `skins.apply`, `skins.remove`, `mover.preview`, `mover.restore` (the Mover Workbench's preview pose, outside Undo), every `preview.*` and `characters.*` op: they open windows or undo blindly. |

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
  Over the bridge, a document's `texture` and `save` operations write only
  the map's own packages, `<Map>_<name>` as for `package.save` (a `texture`
  into `MyLevel` is saved with the map and allowed). Any other is refused
  before anything runs: `refused` / `not_map_owned` for a package that is not
  the map's, `package_name` for `<Map>` or `Amb_<Map>` or a name another kind
  of package already has, `interface_package` for `<Map>-i` (written by
  `interface.texture`), and `unsaved_map` before the map's first save, since
  its packages are named after its file. A `load` alone is allowed: it only
  reads.
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
  `uas.build {packages, quality}`, `optimise.release {packs, destination,
  assetPackage, folder, overwrite}` (the folder only inside `<Game>\Releases`,
  the work folder or `WriteRoots`).
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
| `scct://wiki/index` | Start here: the page map and the minimum viable Versus map checklist. |
| `scct://wiki/{page}` | One digest per wiki page, with class and property names exactly as the wiki writes them: `home`, `spawn-points`, `game-modes`, `map-settings`, `level-properties`, `vision-mode-properties`, `geometric-events`, `events`, `alarms`, `movers`, `elevators`, `lighting`, `material-classes`, `static-meshes`, `sound-actors`, `sound-browser`, `gameplay-objects`, `buttons-and-hacking-panels`, `level-ingredients`, `zones-and-volumes`, `softbody-actors`, `animated-meshes`, `fish`, `map-tutorials`, `map-packaging`. |
| `scct://schema/{name}` | JSON Schemas: `map-changes`, `map-authoring`, `stages`, `smagic-event`, `build-plan`. |
| `scct://docs/{name}` | `tools` (the agent's guide and the conventions every tool shares), `ops` (the bridge ops, their policies and arguments), `map-authoring` and `map-design` (abridged from this folder). |
| `scct://editor/state` | Live: the open map's identity and state. |
| `scct://map/summary` | Live: actor counts, spawns per team, missions, security, cameras. |
| `scct://map/validation` | Live: the editor's Map Check of the map. |
| `scct://map/screenshot/latest` | Live: a PNG of the perspective viewport. |
| `scct://map/actor/{path}` | Live: one actor's editable values, such as `scct://map/actor/MyLevel.Light12`. |

A URI may end in a `#fragment` naming a section, such as
`scct://docs/tools#errors`: it is read, and subscribed to, as the resource
without it.

Clients can subscribe to the live resources; the server announces a change
after each call that changed something in the editor (a bridge call that
edits, saves, builds or undoes, and not a dry run, a preview or a read), so a
preview does not make subscribers re-read `scct://map/validation`.

**Protocol details.** Versions 2025-11-25, 2025-06-18, 2025-03-26 and
2024-11-05 are spoken. A JSON-RPC batch is answered with one array of
replies under 2025-03-26 and older; 2025-06-18 removed batches, so from then
on a batch gets one `-32600` error. A `prompts/get` argument the prompt does
not take, or one that is not a string, is `-32602`; reading
`scct://map/actor/<path>` for an actor the map does not have is `-32002`.

The digests were written from the wiki's raw pages; each names its source page.
`tools/mcp/build_wiki_resources.ps1` fetches the pages again and lists names and
numbers a digest lacks, for when the wiki changes.

## Limits and safety

What the server and the editor never do, whatever the agent asks:

- **Turn themselves on.** Only you can tick **Enable MCP server**, and
  `AllowRawExec` and `AllowPlay` are set only in the INI.
- **Write stock packages.** Package writes are limited to the map's own names:
  `<Map>`, `<Map>-i`, `Amb_<Map>` and `<Map>_<anything>` (`<Map>_TXT`,
  `<Map>_STM`). Texture and mesh imports (`objimport.import`'s OBJ meshes
  and their textures too), package saves and a Map JSON document's `texture`
  and `save` operations go only into `<Map>_<anything>` (`not_map_owned`,
  `package_name`), and the interface textures only into `<Map>-i`. Since
  those are named after the map's file, none of them, nor a `.uas` or a
  release copy, is written before the map's first save (`unsaved_map`). A
  Map JSON texture and an OBJ import may also go into `MyLevel`, which is
  saved with the map. A name that another kind of package already has is
  refused too: packages of one base name are one package to the engine.
- **Delete without asking for it.** A call deletes actors only when it says
  `allowDeletes: true` (`actors_edit`, Map JSON version 3).
- **Discard unsaved work.** `map_file new` and `open` refuse while the map has
  unsaved changes, unless the call says `discardUnsaved`.
- **Overwrite files.** Replacing another map's file, a package file or an
  output image needs `overwrite: true`, and replaced maps and packages are
  backed up first.
- **Write elsewhere.** Maps are saved in `Packages\MapsEd` under ASCII names
  without spaces. The editor writes packages in `Packages\Textures`,
  `Packages\StaticMeshes` and `Packages\Sounds` under the map's names, its
  own files in `System\ReloadedEditor\Mcp`, and release copies in
  `<Game>\Releases\<Map>`, as Optimise Map Assets does. Over the bridge a
  release copy's `folder` must be a full path inside `<Game>\Releases`, the
  work folder or `WriteRoots`, compared after `..` and links are resolved
  (else `refused` / `outside_write_roots`), so its `overwrite` only ever
  replaces files there; the Optimise Map Assets window still takes any folder.
- **Run console commands.** `editor_exec` is off unless `AllowRawExec=1`. Even
  then the editor refuses a command that starts with any of these, read word
  by word as its console reads them (any case, blanks skipped, a word ending
  at anything but a letter or digit, so `QUIT;` and `EXEC"cmds.txt"` count),
  as `refused` / `denied_command`:
  - `QUIT`, `EXIT`, `RELAUNCH` and `DEBUG`, which end or crash the editor, and
    `EXEC`, which runs a file of commands;
  - `MAP NEW`, `MAP LOAD`, `MAP SAVE`, `MAP IMPORT` and `MAP EXPORT`;
  - `OBJ SAVEPACKAGE`, `OBJ EXPORT`, `OBJ IMPORT`, `OBJ LOAD`, `SAVEMAPPROP`
    and `LOADMAPPROP`;
  - `TEXTURE IMPORT`, `TEXTURE EXPORT`, `STATICMESH IMPORT`,
    `STATICMESH EXPORT`, `MESH IMPORT`, `BRUSH IMPORT`, `BRUSH EXPORT` and
    `DUMPINT`;
  - `TRANSACTION` (blind undo and redo) and `AUDIO` (the sound driver).

  It also refuses any command with a `FILE=` (found anywhere, as the engine
  finds it, so `PROFILE=` counts) outside the work folder and `WriteRoots`, a
  relative path being read from `System` (`outside_write_roots`); a `PACKAGE=`
  other than `MyLevel` or the map's own `<Map>_<name>`, `<Map>-i` or
  `Amb_<Map>`, and any but `MyLevel` before the map's first save
  (`not_map_owned`); and anything but one command of up to 4,096 characters on
  one line, without `|` (`invalid`). The map, package and undo tools
  do these things with their checks.
- **Open windows or skip checks.** Ops that open windows, undo blindly or
  write without a stale check are blocked over the pipe.

The pipe accepts connections only from your own Windows account on this
computer, one client at a time.

## Status and known limitations

The server is covered by the model and server test suites (`tools/test_mcp.cmd`,
[tests/README.md](../tests/README.md#mcp-server)). The editor side (the pipe,
the ops and the menu) has been checked against a stubbed editor; **the native
probe and the end-to-end run in a real editor have not passed yet**, so treat
what follows as the current state, not a promise:

- **Geometric events.** GEs can be added (`GE ADD`), not deleted or given GE
  IDs over the bridge: delete them in GE mode and set IDs with **Set GE ID**.
  `ge.list` knows only the GEs Map Design or the server added. Whether a GE
  added inside a plan is undone with the rest of the plan is not yet confirmed.
- **Map Settings.** `mapsettings.set` is not supported: set them by hand
  (right-click empty viewport space > **Map Settings**).
  `mapsettings.get` reads what it can from `<Map>-i.utc` on disk. Level
  Properties and the interface textures go through paths RE+ already uses.
- **Restarting the editor.** `editor_restart` needs the editor to close without
  a question; if one appears (for example about unsaved changes), answer it,
  or restart the editor yourself when told.
- **Meshes.** `asset.mesh` imports ASE files with `STATICMESH IMPORT`;
  collision can only be checked, not set. Mesh packages are saved as
  `Packages\StaticMeshes\<Map>_STM.usx`.
- **Other unsupported ops.** `object.inspect` and `object.set` (non-actor
  objects) refuse with manual steps.
- **Approval** has no time limit in the editor; the window stays until you
  answer or the client goes away.
- The DLL sends no change events of its own yet: live resources are marked
  updated after each change the server makes, not after your own edits.

## Troubleshooting

| Symptom | What to do |
| --- | --- |
| `not_connected`, or "No editor connected" | The editor is not running, or its server is off. Start the editor and tick **RE+ Tools > MCP > Enable MCP server**, or let the agent use `editor_launch` once `Enabled=1`. |
| `disabled` | The server was turned off. Tick **Enable MCP server** again. |
| The client cannot start the server | Check the path in the client's configuration points at `System\Reloaded.Editor.Mcp.exe`, and that the file exists (it comes with RE+ 2.9.0 and later). Run it with `--self-test` in a command prompt. |
| `protocol_mismatch` | The exe and the DLL come from different RE+ versions. Install both from the same release, then restart the MCP client. |
| `busy` with a reason | See [How calls run](#how-calls-run): close the dialog or menu, finish the drag, answer a startup window, restore a minimised editor, let a Mover Workbench preview end. |
| `busy` / `another_client` | Another client holds the editor. Close it, or use **Disconnect client**. |
| `busy` / `approval_pending` | A change waits in the review window. Answer it; the agent continues with `approval_wait`. |
| `refused` / `outside_write_roots` | A release copy's folder, or an `editor_exec` `FILE=`, is outside `<Game>\Releases` (release copies only), the work folder and `WriteRoots`. Leave the folder out, or add it to `[Mcp] WriteRoots`. |
| `invalid` / `several_editors` | Several editors are running; `editor_status` lists them and takes `pid`. |
| `stale` | The map changed since the agent read it. It should read again and retry. |
| `timeout` / `awaiting_approval` | The change is waiting in the review window. Answer it; the agent continues with `approval_wait`. |
| `denied` | You cancelled the review. |
| `dialog_blocked` | A dialog opened during the change. It was answered with its most cautious button (or closed) and the call stopped there; the message says what happened. `details.dialogs` has its title, text and answer; `stoppedAt` is the step it stopped at (no later step ran), `ran` the steps that finished and `wrote` the files and packages they had already written, which stay. `rolledBack` says whether the map changes were undone: a plan that had already saved or written files keeps them (see [Dialogs](#how-calls-run)), so the map and its file stay alike, and then `editor_undo` with the `call` the message names takes them back. Answer the dialog's question yourself in the editor if it needs one, then retry. |
| `refused` / `raw_exec_off` or `play_off` | Set `AllowRawExec=1` or `AllowPlay=1` in the INI with the editor closed, only if you want to allow that. |
| `refused` / `unsaved_changes` | Save the map, or tell the agent it may discard the changes. |
| `refused` / `not_map_owned` | The package is not the map's own (an import, a package save, a Map JSON `texture` or `save`, an `editor_exec` `PACKAGE=`); use `<Map>_TXT`, `<Map>_STM` or another `<Map>_` name. |
| `refused` / `package_name` or `interface_package` | `<Map>` and `Amb_<Map>` are not for imports, and a name another kind of package has would merge with it; `<Map>-i` is written only by the interface textures. Use `<Map>_TXT` or `<Map>_STM`. |
| `refused` / `unsaved_map` | The map has no file yet, and its packages, `.uas` and release copy are named after it. Save it first (`map_file action:"save_as"`). |
| `refused` / `denied_command` | `editor_exec` never runs that command (see [Limits and safety](#limits-and-safety)); use the tool that does it with its checks. |
| A build or `.uas` times out in the client | Raise the client's tool timeout; the editor finishes the build regardless, and `editor_status` shows the result. |
| `editor_launch` waits and fails | A startup window (an update offer, crash recovery, a missing package) is waiting for you; answer it, then `editor_status`. `System\Reloaded_Editor.exe.log` says why the launcher failed. |

The bridge's own messages go to RE+'s log as lines starting `MCP:`;
`editor_log` shows the engine's lines to the agent, and the history folder has
every change's request and reply.

## Verification

`tools/test_mcp.cmd` (MSVC) and `tools/test_mcp.sh` (g++) build and run every
MCP test suite and the server's `--self-test`. The native probe's MCP section
and the end-to-end run against a real editor are described in
[tests/README.md](../tests/README.md#mcp-server) and
[tests/McpBridgeNativeNotes.md](../tests/McpBridgeNativeNotes.md).
