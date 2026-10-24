#pragma once
// The MCP bridge inside the editor: a named pipe (\\.\pipe\ReloadedEditor.Mcp.<pid>)
// that Reloaded.Editor.Mcp.exe connects to, carrying the frames and messages of
// McpBridgeModel.h. The pipe thread only moves bytes; every call runs on the
// editor's UI thread through a message-only window, behind the guards, the op
// policies and (when asked for) the approval preview. Off until RE+ Tools >
// MCP > Enable MCP server, which writes [Mcp] Enabled=1 to Reloaded_Editor.ini.
#include <windows.h>
#include "WorkflowModel.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace McpBridge
{
    // RE+ Tools > MCP (41720-41739, routed by General.cpp's dispatcher).
    constexpr UINT kMenuFirst = 41720;
    constexpr UINT kEnable = 41720, kRequireApproval = 41721, kUndoLast = 41722, kDisconnect = 41723,
                   kCopyClaudeCode = 41724, kCopyClaudeDesktop = 41725, kOpenHistory = 41726;
    constexpr UINT kMenuLast = 41739;

    // From dllmain after WorkflowTools::Initialize: reads [Mcp] only (loader lock).
    void Initialize();
    // On the frame's thread, from EditorExtras' install: the message-only
    // window, and the pipe thread when the server is enabled.
    void AttachUiThread(HWND frame);
    // On the frame's WM_DESTROY: stops the pipe thread without waiting for it.
    void Shutdown();
    // Menu commands kMenuFirst..kMenuLast; false for any other command.
    bool HandleCommand(UINT command);
    // The RE+ Tools > MCP submenu (UI.cpp inserts it), and its check marks and
    // "(connected)" refreshed when a menu opens.
    HMENU BuildMenu();
    void UpdateMenu(HMENU menu);

    struct Settings
    {
        bool enabled = false, requireApproval = false, allowRawExec = false, allowPlay = false;
        unsigned historyMaxMB = 200;
        // The server's work folder ([Mcp] WorkDir, else %ProgramData%\REPlusMcp)
        // and [Mcp] WriteRoots, as Reloaded.Editor.Mcp.exe reads them.
        std::filesystem::path workDir;
        std::vector<std::filesystem::path> writeRoots;
    };
    Settings Current();
    bool Connected();
    // <System>\ReloadedEditor\Mcp: blobs\, history\.
    std::filesystem::path Directory();
    // The folders a bridge call may write in besides the map's own files and
    // packages: the work folder and [Mcp] WriteRoots, canonical.
    std::vector<std::filesystem::path> WriteRoots();
    // Whether the dialog watchdog answered a question it did not know during
    // the call running now: mcp.plan runs no further step once it has.
    bool DialogBlocked();
    // For the handler of the call running now, on the frame's thread: a
    // progress message for that call ({"t": "progress", "stage", ...extra}),
    // and whether its client has asked to cancel it. Only the ops
    // Mcp::Bridge::Cancellable lists ever see a cancel; outside a bridge call
    // the one does nothing and the other is false.
    void OpProgress(const std::string& stage, const Workflow::Json& extra = Workflow::Json::object());
    bool OpCancelRequested();

    // The bridge calls that changed the map, newest last: undo.call checks a
    // call's transactions are still the newest undoable ones before undoing.
    struct CallRecord { unsigned call = 0; std::string label; unsigned markFrom = 0, markTo = 0; unsigned steps = 0; };
    std::vector<CallRecord> Records();
    // Engine log lines since a sequence number (log.tail), from a 2,000-line ring.
    struct LogLine { uint64_t seq = 0; std::string level, text; };
    std::vector<LogLine> LogSince(uint64_t seq, size_t max = 500);
    uint64_t LogLast();
}
