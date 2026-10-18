#pragma once
#include <cstdint>

class LightmapFix
{
public:
    static void Initialize();

    // The Save hooks are call-site patches, so any other .sdc write must call this itself.
    static void RepairSavedMap(const char* mapsEdPath);

    // Stream offset where the pre-fix writer destroyed 64 bytes of map data, or 0.
    static uint32_t ScanForDamage(const char* path);

    // Where the warning for a damaged map goes; without a sink it is a message box.
    // Returns the sink it replaces, for the caller to put back.
    typedef void (__cdecl *DamageSink)(const char* message);
    static DamageSink SetDamageSink(DamageSink sink);

    // Told of every save that reached RepairSavedMap and wrote its file:
    // File > Save and Save As by any route (menu, Ctrl+L, the engine's own
    // request), and the RE+ tools that save. A cancelled Save As never gets
    // here, and a save the engine could not write (a read-only map) is left
    // out: the file's write time did not change.
    typedef void (__cdecl *SavedSink)(const char* path);
    static void SetSavedSink(SavedSink sink);
};
