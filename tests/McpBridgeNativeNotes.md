# MCP bridge: native probe notes

`Reloaded.Editor/McpBridge.cpp` is the named-pipe end of the MCP bridge inside
the editor. Its pure parts (framing, policies, plan checks, busy reasons) are
covered by `McpBridgeModelTests.cpp`, `McpPlanModelTests.cpp` and
`McpOpCoverageTests.cpp`. What only a running editor can show is listed here,
for the bridge section of `tests/WorkflowNativeTests.h`.

## Setting up
- Write `[Mcp] Enabled=1` to the disposable install's `System\Reloaded_Editor.ini`
  before the editor starts. Without it there is no pipe.
- The probe runs inside the editor. Connect from a worker thread, never the
  frame's thread: every call runs on the frame's thread, so a client there
  would wait on itself.
- Open `\\.\pipe\ReloadedEditor.Mcp.<editor pid>` with `CreateFileW`
  (`GENERIC_READ | GENERIC_WRITE`). Send frames with `Mcp::Bridge::EncodeFrame`
  and read them with `FrameReader`.
- A call is answered by zero or more `progress` messages, then one `result`
  with the same `id`.

## Cases and what each should return
| Case | Expect |
|---|---|
| A call before `hello` | `protocol_mismatch` |
| `hello` | `t:"hello"`, `ops` without any Blocked op, `pid` of the editor, `state` once a call has run |
| `hello` with protocol major 9 | `protocol_mismatch`; the connection stays open |
| A second client while one is connected | one `result` with id 0, `busy` / `another_client`; the pipe closes about 2 s later |
| `light.set`, an unknown op | `refused` / `blocked`, `refused` / `unknown_op` |
| `exec`, `play.level` with the INI keys off | `refused` / `raw_exec_off`, `play_off` |
| `editor.state` | `result.open`; `state.expect` = `{level, generation, revision: contentRevision}` |
| `actors.query` | `ok`; `log` lists the engine warnings logged during the call; `undo.steps` 0 |
| `design.block` with a label | `ok`; result is the op's own (not the plan's); `undo:{call:n, steps:1}`; Undo History shows `MCP n: <label>`; `McpBridge::Records()` has the call; `System\ReloadedEditor\Mcp\history\<stamp>-<n>-design.block.json` exists |
| The same with `expect.revision` one less | `stale` / `map_edited`; nothing changed |
| `dryRun:true` | `result.preview`; `undo.steps` 0; the map unchanged |
| A modal dialog open (any stock dialog) | `actors.query` gives `busy` / `modal`; `editor.state` still answers, with `busy:"modal"` |
| An op that pops a message box | the box closes by itself; `dialog_blocked` with `{title, text, button}`; an Undoable op's changes undone (`details.rolledBack`) |
| Build All over the bridge (`build.all`) | its progress window is left alone (the watchdog skips windows with a progress bar and windows that existed before the call) |
| Require approval on, `design.block` | `progress:"awaiting_approval"` with a `ticket`; the "Approve a change from the MCP client" window is open |
| ...while it is open: `actors.query`, `approval.status`, `editor.state`, `log.tail` | `busy` / `approval_pending`; `status:"pending"`; cached state with `busy`; log lines |
| ...Approve | `progress:"approved"`, then `ok`; `approval.status` gives `done` with the outcome |
| ...Cancel in the window | `denied` |
| ...`{"t":"cancel","id":n}` from the client | `cancelled` |
| ...the client closes the pipe | the window closes; after reconnecting, the ticket says `cancelled` |
| ...an edit made in the editor while the window is open | `stale` / `changed_during_review` |
| A plan of reads only, approval on | no window; `ok` |
| RE+ Tools > MCP > Disconnect client | the pipe closes (about 2 s later); the menu shows no "(connected)" |
| RE+ Tools > MCP > Undo last MCP call | the newest call that is not undone yet is undone |
| Turning the server off and on | no pipe while off; a reply sent just before turning it off still arrives; after turning it on, a new client connects |
| A frame header over 16 MiB | `invalid`, then the pipe closes |

## Off-Windows check
These cases, apart from the real dialogs, the build and Undo History, were run
against the real `McpBridge.cpp` under wine64 and Xvfb. The engine,
WorkflowTools, the review window and EngineLog were stubbed (84 checks). The
harness was a scratch file outside the repository; copy it into `tests/` to keep it.
It built with:

```
x86_64-w64-mingw32-g++ -std=c++20 -O1 -Wall -Wextra -Wshadow -I<shim with Windows.h> \
  -IReloaded.Editor -IReloaded.Editor/Include -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
  harness.cpp Reloaded.Editor/McpBridge.cpp -static -lcomctl32 -o harness.exe
xvfb-run -a wine64 harness.exe
```
