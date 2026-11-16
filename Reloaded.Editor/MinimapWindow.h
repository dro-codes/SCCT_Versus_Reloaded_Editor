#pragma once
#include <windows.h>
#include "WorkflowModel.h"
// Build > Generate Minimap (41240): a clean top-down plan of the open map, one
// floor at a time, with objectives and spawns marked. It sets the game's
// in-game map (LevelInfo.MapFloors and SnapshotCamera, one texture per floor
// in the map package), writes the lobby's Briefing picture into the map's
// interface package (<Map>-i.utc, backed up first, as Level Snapshot does),
// or saves the picture as PNG/TGA. See MinimapModel.h for what the game reads.
namespace MinimapWindow
{
    constexpr UINT Command = 41240;
    // Control ids inside the window; a test can post WM_COMMAND with them.
    constexpr int kFloor = 301, kFrom = 302, kTo = 303, kSize = 304, kStyle = 305, kObjectives = 306, kSpawns = 307,
                  kMeshes = 308, kFraming = 309, kRefresh = 310, kSaveImage = 311, kWriteBriefing = 312, kSetInGame = 313,
                  kStatus = 314, kPreview = 315, kApplyRange = 316, kFloorList = 317;
    void Open(HWND owner);
    // From the frame's command dispatcher. True when the command was ours.
    bool HandleCommand(UINT command);
    // Set as the in-game map without the window (the bridge's minimap.apply),
    // as its button does. options, all optional: floors [Z of each floor's
    // base] (default: the map's own floors, else those found in its geometry),
    // framing "gameplay" | "everything" | "current", style (palette index or
    // name), objectives (true), spawns (false), meshes (true), and briefing
    // (false): also write the Briefing picture of <Map>-i, of the whole map or
    // of briefingFloor (1-based). Returns {floors, unused, camera, briefing?}.
    // One Undo step for the LevelInfo; the textures are not undone.
    Workflow::Json Apply(const Workflow::Json& options);
}
