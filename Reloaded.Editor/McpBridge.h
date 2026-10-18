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
    // RE+ Tools > MCP (41460-41479, routed by General.cpp's dispatcher).
    constexpr UINT kMenuFirst = 41460;
    constexpr UINT kEnable = 41460, kRequireApproval = 41461, kUndoLast = 41462, kDisconnect = 41463,
                   kCopyClaudeCode = 41464, kCopyClaudeDesktop = 41465, kOpenHistory = 41466;
    constexpr UINT kMenuLast = 41479;

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
    };
    Settings Current();
    bool Connected();
    // <System>\ReloadedEditor\Mcp: blobs\, history\.
    std::filesystem::path Directory();

    // The bridge calls that changed the map, newest last: undo.call checks a
    // call's transactions are still the newest undoable ones before undoing.
    struct CallRecord { unsigned call = 0; std::string label; unsigned markFrom = 0, markTo = 0; unsigned steps = 0; };
    std::vector<CallRecord> Records();
    // Engine log lines since a sequence number (log.tail), from a 2,000-line ring.
    struct LogLine { uint64_t seq = 0; std::string level, text; };
    std::vector<LogLine> LogSince(uint64_t seq, size_t max = 500);
    uint64_t LogLast();
}
