#pragma once
#include <windows.h>

// File > Compare with Backup...: pick a saved copy of the open map (one of RE+'s backups, a stock autosave or any
// .sdc) and see what differs: actors added, deleted, moved and changed, with the backup's value beside the open
// map's, in the Map JSON preview list. A row click selects the actor; ticked rows come back with Bring Back
// Selected, through the Map JSON import, as one Undo step. The comparison is CompareBackupModel.h; loading the
// copy is CompareBackupNative.inl.
namespace CompareBackup
{
    constexpr UINT kCompare = 41600; // the compare-with-backup block, 41600-41619
    void Open(HWND frame);
}
