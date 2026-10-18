#pragma once
#include <windows.h>
#include <string>
#include <vector>

class RebuildAllMaps
{
public:
    static void Initialize();

    // Gated on -UnlockPackages.
    static bool Available();

    // True while the batch runs; nothing may prompt during it.
    static bool Running();

    static void Show(HWND hParent);

    // Saves the open map to path (a .sdc, as the engine opens it: relative to
    // System or in full) the way the batch saves each map: SaveBackups keeps
    // the file it replaces, then the editor's own map save, LightmapFix's
    // repair (which also tells the unsaved-changes warning the file was
    // written) and SAVEMAPPROP for the map's interface package. That package's
    // file is copied to System\ReloadedEditor\snapshot-backups and moved out
    // of the way for the save (put back when it writes none), so the stock
    // question about a smaller package never comes up. The Save target is
    // left as it was. Throws when the file was not written.
    static void SaveMapTo(const std::string& path);

    // The batch's rebuild of the open map: Build All (per Build Options)
    // inside the editor's prepare/restore pair, with no Lighting Budget
    // question and no Map Check window. Returns what the build reported
    // (Map Check entries, damage warnings). Throws when the build faulted.
    static std::vector<std::string> BuildCurrentMap();
};
