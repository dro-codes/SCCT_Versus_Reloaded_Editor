#pragma once
// When the editor cannot take a bridge call now: the reason a busy reply
// gives, from facts McpBridge.cpp collects on the UI thread. Engine-free.
#include "McpBridgeModel.h"

namespace Mcp::Guard
{
    struct Inputs
    {
        int depth = 0;                 // bridge handlers already running (a nested modal loop re-entered)
        bool approvalPending = false;  // a review window is open
        bool frameDisabled = false;    // a modal dialog owns the frame
        bool menuMode = false;         // a menu or the system menu is open
        bool moveSize = false;         // the frame is being moved or resized
        bool captured = false;         // a viewport drag holds the mouse
        bool transactionOpen = false;  // the undo buffer has an open transaction
        bool moverPreview = false;     // the Mover Workbench is previewing a mover (its pose moved outside Undo)
        bool rebuilding = false;       // Rebuild All Maps is running
        bool slowTask = false;         // the engine's progress bar is up
        bool minimised = false;
        std::string startupDialog;     // title of an updater or crash-recovery window still open
    };
    // The busy reason, or "" when the op may run. Reads only wait for what
    // would make their answer wrong or re-enter the engine.
    inline std::string BusyReason(const Inputs& in, const Bridge::OpPolicy& op, bool screenshot = false)
    {
        if (in.depth > 0) return in.approvalPending ? "approval_pending" : "in_flight";
        if (!in.startupDialog.empty()) return "startup_dialog";
        if (in.frameDisabled) return "modal";
        if (in.rebuilding) return "rebuild";
        if (in.slowTask) return "slow_task";
        if (screenshot && in.minimised) return "minimised";
        if (op.policy == Bridge::Policy::Read) return {};
        if (in.approvalPending) return "approval_pending";
        if (in.menuMode || in.moveSize) return "menu";
        if (in.captured) return "drag";
        if (in.transactionOpen) return "transaction";
        if (in.moverPreview) return "mover_preview";
        return {};
    }
    // What to tell the agent while it waits.
    inline std::string BusyHint(const std::string& reason)
    {
        if (reason == "approval_pending") return "A change is waiting for approval in the editor.";
        if (reason == "in_flight") return "Another bridge call is still running.";
        if (reason == "startup_dialog") return "A window is open in the editor (an update offer or crash recovery); ask the user to answer it.";
        if (reason == "modal") return "A dialog is open in the editor; ask the user to close it.";
        if (reason == "rebuild") return "Rebuild All Maps is running.";
        if (reason == "slow_task") return "The editor is busy with a long task.";
        if (reason == "minimised") return "The editor is minimised; screenshots need it restored.";
        if (reason == "menu") return "A menu is open in the editor.";
        if (reason == "drag") return "The user is dragging in a viewport.";
        if (reason == "transaction") return "An editor operation is still open.";
        if (reason == "mover_preview") return "The Mover Workbench is playing a mover preview; it puts the mover back when the preview ends (a few seconds) or Stop Preview is clicked.";
        return "The editor is busy.";
    }
}
