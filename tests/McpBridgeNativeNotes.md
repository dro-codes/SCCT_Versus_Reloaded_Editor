# MCP bridge: native probe and end-to-end notes

`Reloaded.Editor/McpBridge.cpp` is the named-pipe end of the MCP bridge inside
the editor. Its pure parts (framing, policies, plan checks, busy reasons) are
covered by `McpBridgeModelTests.cpp`, `McpPlanModelTests.cpp` and
`McpOpCoverageTests.cpp`. The last also tests `McpSafetyModel.h`, what the
bridge refuses before an op writes (stock packages in Map JSON `texture` and
`save` operations, release folders and `exec` files outside the write roots,
the commands `exec` never runs) and the button the dialog watchdog presses,
and scans the DLL's sources for `std::min`/`std::max` where MSVC's windows.h
macros are live. What only a running editor can show is checked by
two runs in a disposable copy of the game:

- the **native probe's MCP bridge section** (`tests/WorkflowNativeTests.h`,
  `mcpBridge`), a client on the editor's own pipe inside the editor;
- the **end-to-end run** (`tools/run_mcp_e2e.ps1`), Reloaded.Editor.Mcp.exe
  driven over stdio by the steps in `tests/mcp/e2e/*.jsonl`.

## Running them
From PowerShell 5.1 or later on Windows, with the game's System folder in
`SCCT` (or `-GameSystem`):

```
# The native section: compiles the probe (Visual Studio C++ tools), prepares
# run_native_map_recovery_test.ps1's disposable install, sets [test]
# mcp_bridge_only=1 and [Mcp] Enabled=1, then runs and streams the report.
.\tools\run_mcp_e2e.ps1 -Native -EditorDll .\bin\Reloaded.Editor.dll [-VisibleEditor]

# End to end: copies the DLL, bin\Reloaded_Editor.exe (or the installed
# launcher) and bin\Reloaded.Editor.Mcp.exe into a disposable install, starts
# the editor through the launcher with -reMcp and runs every step file.
.\tools\run_mcp_e2e.ps1 -EditorDll .\bin\Reloaded.Editor.dll -McpExe .\bin\Reloaded.Editor.Mcp.exe

# Anywhere, no game: reads the step files and tests the expectation checker.
pwsh tools/run_mcp_e2e.ps1 -CheckScripts
```

The section is selected the way the other focused sections are: `[test]
mcp_bridge_only=1` in `native_recovery_test.ini` (with `workflow_tools=1`)
makes `RunWorkflowTests` run only it. `run_native_map_recovery_test.ps1` has
no switch for it yet; `-Native` sets the key after that script's
`-PrepareOnly`.

## The native section
The probe runs on the editor's frame thread, and the bridge runs every call
on that thread too, so the probe's client (`tests/mcp/McpPipeClient.h`) works
on a worker thread (`McpPipe::Worker`) while the frame thread pumps messages:
that pumping is what runs each call. The section starts from `map.new`, so it
needs no fixture. If `[Mcp] Enabled` is 0 it turns the server on through RE+
Tools > MCP > Enable MCP server (command 41720), as a user would.

| Case | What the section checks |
|---|---|
| A call before `hello` | `protocol_mismatch` |
| `hello` with protocol major 9 | `protocol_mismatch`; the connection stays open |
| `hello` | `t:"hello"`, the editor's `pid`, `enabled`, `ops` equal to `OpList()` (no Blocked op) |
| A second client | result id 0, `busy` / `another_client` |
| `light.set`, an unknown op | `refused` / `blocked`, `refused` / `unknown_op` |
| `exec`, `play.level` with the INI keys off | `refused` / `raw_exec_off`, `play_off` |
| `editor.state` | `result.open`; `state.expect` = `{level, generation, revision: contentRevision}` |
| `actors.query` | `ok`, `log` is a list, `undo.steps` 0 |
| `map.new {discard}` | an open map with no file and `dirty` false |
| `design.block` with a label | the op's own result (not the plan's); `undo:{call:n, steps:1}`; Undo History's newest title is `MCP n: <label>`; `history\<stamp>-n-design.block.json` exists |
| `dryRun:true` | `result.preview` rows, `undo.steps` 0, actors and ContentRevision unchanged |
| `expect.revision` one less | `stale` / `map_edited`, nothing changed; the current `expect` runs |
| A modal dialog (a MessageBox owned by the frame) | `actors.query` gives `busy` / `modal`; `editor.state` answers with `busy:"modal"`; calls run again after it closes |
| `mcp.plan` whose second step fails (a `$bind` to nothing) | `failedStep` "bad", `rolledBack` true, `section` "steps", the first step's result kept; the actors as before, `canRedo` false, no Undo step |
| A two-step `mcp.plan` | `undo.steps` 1, one Undo History entry; one `TRANSACTION UNDO` takes both pieces back, one `REDO` puts both back |
| `undo.call` | `refused` / `not_newest` after an edit made directly; works once the editor's own Undo took that edit back; `already_undone` after; the newest call undoes at once |
| `spec.material` | a loaded material other than the blockout's default textures every polygon of the piece (assembly.capture text) |
| `build.all` | `ok`; no dialog answered (its progress window is left alone) |
| `mapcheck.run` | `{rows:[{severity, group, check, message, actors, surfaces}], summary:{errors, warnings, info}}` |
| `ge.add LADDER` | `GE ADD LADDER X1=...`; its `undo` block is recorded (spike S1) |
| `view.screenshot` | a PNG whose IHDR matches `width`/`height`, over 256 px; saved as `mcp_screenshot.png` |
| `authoring.export` over 8 MiB | the map is filled with lights (batches of 400) until the blob passes 8 MiB; `blob.read` returns it whole in 4 MiB pieces and it parses as `scct.map-authoring`; `blob.free` deletes it; `..\library` is refused; `undo.steps` takes the fillers back |
| `map.save` / `map.new` / `map.open` | `Packages\MapsEd\McpProbeD.sdc` written, `dirty` false; the new map lacks the piece; the reopened map has it |
| `design.register` (on the saved map) | `registered` 1, listed by `design.pieces` and in `library.json` under the map's key; again 0; `design.unregister` removes it |
| Require approval on, `design.block` | `progress:"awaiting_approval"` with a ticket; once the review window is open: `actors.query` and another change `busy` / `approval_pending`, `editor.state` cached with `busy`, `approval.status` `pending`, `log.tail` answers |
| ...Approve (IDOK posted to the window) | `progress:"approved"`, then `ok`; `approval.status` `done` with `outcome.ok` |
| ...Cancel (IDCANCEL) | `denied` / `user_declined`, nothing changed, ticket `denied` |
| ...`{"t":"cancel","id":n}` | `cancelled`, ticket `cancelled` |
| A plan of reads only, approval on | `ok` with no `awaiting_approval` |
| RE+ Tools > MCP > Disconnect client | the pipe closes; a new client connects |
| The server off, then on | `Enabled=0` and no pipe; `Enabled=1` and a client connects |

Things the section does not cover yet: an op that pops an unknown message box
(the watchdog's `dialog_blocked` and its rollback need an op that opens one),
an edit made while the review window is open (`stale` /
`changed_during_review`), a frame header over 16 MiB, and RE+ Tools > MCP >
Undo last MCP call (its failure path opens a message box nobody would answer).
Worth adding, as the wine harness below runs them only against stubs:
- an ownerless `MessageBox` open on the frame thread (LightmapFix's damage
  notice during File > Open): `actors.query` and `design.block` give `busy` /
  `modal` naming its title, `editor.state` says `busy:"modal"`, nothing runs
  inside the box's loop; and a dialog owned by an RE+ tool window, which
  leaves the frame enabled, does the same;
- RE+'s own prompts, windows of their own classes that disable their tool
  window and run their own `IsDialogMessage` loop: the Magic Event
  Workbench's Create event actor picker (`ReloadedMagicClassPicker`), Undo
  History's Add Checkpoint (`ReloadedUndoCheckpointName`) and a Workflow
  Tools form (`ReloadedWorkflowForm`): `busy` / `modal` naming the prompt;
  with Undo History or the workbench merely open, calls run;
- with approval on, the `busy` / `approval_pending` reply to a second call
  carries the review's ticket in `error.details.ticket`;
- `undo.call` of a call number the editor has no record of: `invalid` /
  `unknown_call`; of a recorded one, `{call, done, steps}`;
- an `mcp.plan` whose post `map.save` meets an unknown question: `dialog_blocked`
  with `stoppedAt`, `ran`, `wrote` (the `.sdc`) and `rolledBack:false`, the steps
  after it not run and the map changes kept (`undo.call` takes them back);
- `authoring.apply` with a `save` of a stock texture package: `refused` /
  `not_map_owned`, the package file untouched; `optimise.release` with a
  `folder` outside `Releases`, the work folder and `WriteRoots`: `refused` /
  `outside_write_roots`;
- with `AllowRawExec=1`: `exec` of `SAVEMAPPROP`, `MAP EXPORT FILE=...` and
  `EXEC <file>` refused (`denied_command`), also when punctuation ends the
  word as the engine's ParseCommand allows (`QUIT;`, `EXEC"x"`,
  `SAVEMAPPROP;MAP=X`), and `FILE=` outside the write roots
  (`outside_write_roots`).

What the section learned about the editor:
- ContentRevision counts every transaction, a rolled-back one too, so it moves
  on after a rollback; the section records the two values
  (`mcp_rollback_content_revision`) and checks the actors instead.
- An unsaved map's Map Design entry is kept in memory only (`library.json`
  leaves out the empty key), so the library check runs on a saved map.
- `view.screenshot` needs a visible viewport; the probe hides the editor, so
  the section shows the frame (without activating it) for the shot.

## End to end
`tools/run_mcp_e2e.ps1` prepares the disposable install as
`run_native_map_recovery_test.ps1` does (copies of System's files, junctions to
the shared asset folders, its own StaticMeshes, MapsEd and Maps), writes
`[Updates] CheckOnStartup=0`, `[CrashRecovery] OfferAtStartup=0` and `[Mcp]
Enabled=1 RequireApproval=0 AllowRawExec=0 AllowPlay=0 WorkDir=<test>\McpWork`,
runs `Reloaded.Editor.Mcp.exe --self-test`, starts the editor through the
launcher with `-reMcp` and waits for its pipe, then serves the steps to the exe
over stdio, one at a time (closing stdin early would make the exe cancel what
is still running). At the end it closes stdin and expects the exe to exit with
code 0. The transcript (`mcp_e2e_transcript.txt`, long strings cut) and the
exe's stderr (`mcp_stderr.log`) stay in the test folder.

`tests/mcp/e2e/versus-starter.jsonl` follows the plan's end-to-end list:
initialize, tools/list (32 tools, schemas, annotations), resources and
prompts, `editor_status`, `map_file new`, `level_build` apply of
`Resources/examples/versus-starter.level.json`, `map_build all`, `map_validate
release` (no errors), `map_query summary`, `map_file save_as` (`VersD`),
`editor_restart`, `map_query summary` again (the same counts), `view_screenshot`
(a PNG over 256 px), then a one-light `map_changes_apply`, `editor_undo` and a
summary showing the map as before.

A step is one JSON line: `{name, request:{method, params}, expect:[...],
save:{variable: path}, files:[{path, exists, minBytes}], timeoutSeconds,
stopOnFail, progress}`. The runner adds `jsonrpc` and `id` (none for
`notifications/...`), and `_meta.progressToken` when `progress` is true.
`${name}` is a value the runner sets (`Repository`, `TestRoot`, `TestSystem`,
`MapsEd`, `MapFile`, `ExampleSpec`, `EditorPid`) or a step saved; a string
that is exactly `${name}` keeps the value's type. An expectation is `{path,
...}` with any of `exists`, `equals`, `notEquals`, `gt`, `ge`, `lt`, `le`,
`matches`, `type`, `oneOf`, `contains`, `notContains`, `length`, `minLength`,
`maxLength` and `png:{minEdge, longestEdgeAtLeast, width, height}`, plus an
optional `message`. Paths are `a.b[0].c`, `list[*]`, `list[?key=value]`,
`map['dotted.key']` and `[-1]`; one with `[*]` or `[?...]` selects many values,
which every comparison must hold for (`contains` needs one, the lengths count
them). `_notifications` lists what the server sent during the step. The
checker is C# 5 inside the script (Windows PowerShell's Add-Type compiles it):
PowerShell 7's `ConvertFrom-Json` turns "2025-06-18" into a date, and 5.1's
refuses keys that differ only in case.

## Off-Windows checks
- The bridge itself was run under wine64 and Xvfb with the engine,
  WorkflowTools, the review window and EngineLog stubbed (84 checks):
  ```
  x86_64-w64-mingw32-g++ -std=c++20 -O1 -Wall -Wextra -Wshadow -I<shim with Windows.h> \
    -IReloaded.Editor -IReloaded.Editor/Include -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
    harness.cpp Reloaded.Editor/McpBridge.cpp -static -lcomctl32 -o harness.exe
  xvfb-run -a wine64 harness.exe
  ```
  It was run again with `McpOps.cpp` linked (`-DDLL_API=` so pch.h defines no
  export twice), so `mcp.plan` is the real executor, and 24 more checks (108):
  - the watchdog on dialogs from in-memory templates: "&Ja"/"&Nein" (IDYES,
    IDNO) is answered IDNO, "OK"/"Annuler" IDCANCEL, and one with only buttons
    of its own ids is closed as Escape closes it (it ends with IDCANCEL), never
    by its first button;
  - an ownerless `MessageBoxW(nullptr, ...)` opened by a frame message, a
    dialog box over the tool window (the tool disabled, the frame not) and a
    modeless ownerless panel: the first two give `busy` / `modal` (nothing
    runs re-entrantly) and calls run again once they close; the panel does
    not make the editor busy;
  - plans an unknown question stops: in post after `map.save` (nothing undone,
    `wrote` lists the file, the screenshot after it never runs), in post with
    nothing written (an optional step too: the plan's Undo step is undone), in
    steps (the JoinScope rolls back, the next step never runs), in pre (the
    steps never run), and after a build (the message says the geometry stays).
  What wine showed: a posted message dispatched by a message box's loop goes
  through the WH_MSGFILTER hook (code MSGF_DIALOGBOX), one from the thread's
  own `GetMessage` loop does not, and the message box's text is the static
  with id 0xFFFF (its icon is 0x440 in wine, 0x14 in Windows).
  A later run added 14 checks (122):
  - a window of its own class (`ReloadedMagicClassPicker`, WS_EX_DLGMODALFRAME)
    owned by the tool window it disables, with its own `GetMessage` /
    `IsDialogMessage` loop as MagicEventWorkbench.cpp's CreateActor has, and a
    tooltip-like window of the tool shown above it: `busy` / `modal` naming the
    prompt, not the tip, no change runs, calls run again once it closes; a
    modeless window of its own class owned by the frame (Undo History) and a
    modeless ownerless dialog made only of a 0xFFFF static and push buttons
    (a message box's shape) do not make the editor busy. With the earlier
    `FindModal` (dialogs only, the shape alone enough) the prompt let calls
    run and the panel made every call busy;
  - the `busy` / `approval_pending` reply carries `details.ticket`;
  - `undo.call` through McpOps.cpp: an unknown call gives `invalid` /
    `unknown_call`, a recorded one `{call, done:1, steps:1}`, then
    `already_undone`.
- MinGW cannot see MSVC's `min`/`max` macros (libstdc++ defines `NOMINMAX`).
  To check a DLL file for them, copy it beside a `pch.h` that includes the
  real one, then every standard header, then `#define min(a,b)` and
  `max(a,b)` as minwindef.h does, and compile that copy with the MinGW
  check: `LevelSnapshot.cpp` before its `#undef` gave the 18 errors clang's
  MSVC mode gives, inside MinimapModel.h, and every DLL file changed since
  ccff94d gives as many errors with the macros as without.
- The probe's client and worker were run the same way, the harness's main
  thread playing the frame thread and running the section's flows through
  `McpPipe::Worker` and `McpPipe::Client` (add `-Itests`): the handshake, a
  second client, the refusals, `busy` / `modal` under a real MessageBox, stale
  `expect`, rollback, the review window (a real window of the probe's class
  and title) with approve, cancel, client cancel and `approval_pending`,
  Disconnect, and the server off and on: 42 checks, three runs, no failure.
- `tools/run_mcp_e2e.ps1 -CheckScripts` runs under pwsh 7 on Linux, and the
  script's own `Invoke-Steps` was run against a Linux build of
  Reloaded.Editor.Mcp (no editor) over stdio: initialize, tools/list,
  resources, prompts, `editor_status` unconnected, `level_build` preview with
  progress and a PNG, saved values, a JSON-RPC error, and a failing check
  reported. The embedded C# compiles as C# 5 with no warnings.
