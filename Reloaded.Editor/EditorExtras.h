#pragma once
#include <windows.h>
#include <string>

// Editor-wide conveniences that live outside any one tool: the File menu's
// recent maps, playtesting from the viewport camera, quick selection and
// visibility commands on the actor menu, and the shortcut legend. Commands
// arrive through the frame's menu dispatcher.
namespace EditorExtras
{
    constexpr UINT kSelectSameClass = 40954;
    constexpr UINT kSelectSameTag = 40955;
    constexpr UINT kInvertSelection = 40956;
    constexpr UINT kHideSelected = 40957;
    constexpr UINT kIsolateSelected = 40958;
    constexpr UINT kUnhideAll = 40959;
    constexpr UINT kPlayFromCameraSpy = 40960;
    constexpr UINT kPlayFromCameraMerc = 40961;
    constexpr UINT kShortcuts = 40962;
    constexpr UINT kSelectSameMesh = 40963;
    constexpr UINT kLevelSnapshotViewport = 40964;
    constexpr UINT kLevelSnapshotFile = 40965;
    constexpr UINT kRecentFirst = 40970, kRecentLast = 40979;
    constexpr UINT kOpenLatestAutosave = 40987; // first of the crash-recovery block, 40987-40999
    constexpr UINT kOpenMapBackup = 40988;

    // From the menu-injection thread, once the editor frame exists: brings up
    // the timer, the frame subclass and the menu entries on the frame's thread.
    // Returns once they are in place: the calling thread owns the hook that
    // does the work, so it must not exit before then.
    void Attach(HWND frame);
    // From the frame's command dispatcher. True when the command was ours.
    bool HandleCommand(UINT command);
    // The Reloaded selection, visibility and properties (PropertyPaste.h)
    // entries of the actor right-click menu.
    void AppendActorMenu(HMENU menu);
    // On the frame's thread: opens a map file as Open Recent does, making it
    // the Save target. confirmDiscard asks first when the open map has
    // unsaved changes. Throws when the editor cannot open it.
    void OpenMap(const std::string& path, bool confirmDiscard);
    // The same, asking in a box owned by owner and titled title. False when
    // the user keeps the open map. Autosaves and kept backups are not listed
    // in Open Recent.
    bool OpenMap(const std::string& path, HWND owner, const char* title, bool confirmDiscard = true);
    // Whether the open map has changes its file does not, as the unsaved-changes
    // warning sees it (UnsavedChangesModel.h). False before the first map.
    bool HasUnsavedChanges();
}
