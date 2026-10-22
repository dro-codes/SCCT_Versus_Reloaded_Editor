#pragma once
#include <windows.h>
#include <string>

// Updates the editor from the GitHub releases. A short while after start-up
// (unless turned off, or the MCP server started the editor with -reMcp) and
// from Help > Check for RE+ Updates, a worker thread asks GitHub for the
// newest release; when it is newer than this DLL it offers to install it in
// a window with the release's notes and download size: Install, Remind me
// later (at the next start or after a chosen delay, [Updates] RemindVersion /
// RemindAfter) or Skip this version ([Updates] SkippedVersion). Installing
// downloads the release archive, checks it, and swaps Reloaded.Editor.dll
// (and the launcher and Reloaded.Editor.Mcp.exe) in place: a loaded DLL or a
// running program cannot be overwritten but can be renamed, so the running
// one becomes Reloaded.Editor.dll.old. The new version runs after the editor
// is restarted (and the MCP client, for the server); that start keeps the
// .old files as Reloaded.Editor.previous.dll (and Reloaded_Editor.previous.exe,
// Reloaded.Editor.Mcp.previous.exe), which Help > Roll Back puts back, and
// shows the installed release's notes once (Help > What's New shows them on
// demand; not in an editor started with -reMcp).
namespace SelfUpdater
{
    constexpr UINT kCheckNow = 40984;
    constexpr UINT kToggleStartupCheck = 40985;
    constexpr UINT kAbout = 40986;
    constexpr UINT kWhatsNew = 41000;
    constexpr UINT kRollBack = 41001;
    // Posted to the frame by the worker that fetched the notes for What's New.
    constexpr UINT kShowFetchedNotes = 41002;
    // Posted to the frame by the check that found a newer release: the offer
    // window (its notes, download size, Install / Remind me later / Skip).
    constexpr UINT kShowOffer = 41003;

    // From DLL start-up, with the path of this DLL.
    void Initialize(const std::wstring& dllPath);
    // The Help menu entries: the update checks, What's New, Roll Back and
    // About RE+. From the frame's thread once its menu bar is up; it also
    // schedules the one-time What's New window after an update.
    void AppendHelpMenu(HMENU help, HWND frame);
    // From the frame's command dispatcher. True when the command was ours.
    bool HandleCommand(UINT command);
}
