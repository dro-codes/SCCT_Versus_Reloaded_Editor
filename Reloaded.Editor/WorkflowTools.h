#pragma once
#include <windows.h>
#include <string>
#include "WorkflowModel.h"
namespace WorkflowTools
{
    constexpr UINT kConnections = 40920;
    constexpr UINT kAddObjective = 40949, kAddComputerObjective = 40950, kAddBombObjective = 40951, kAddFlagObjective = 40952;
    // Players in Match on an objective's, zone mission's or mover's right-click menus
    // (ObjectivePlayersModel.h): every match, the presets, Custom..., and for a mover
    // Open at the start when in range, and on a mission Objectives to Win by Match
    // Size.... 41409 was Play Level as... (now the top bar).
    constexpr UINT kObjectivePlayersFirst = 41400, kObjectivePlayersCustom = 41408, kObjectivePlayersOpen = 41410,
                   kObjectiveTargets = 41411, kObjectivePlayersLast = 41411;
    constexpr UINT kViews = 40921;
    // RE+ Tools > Camera Bookmarks (ViewBookmarkModel.h): Go to Bookmark 1-9
    // (41440-41448) and Set Bookmark 1-9 (41449-41457).
    constexpr UINT kBookmarkGoFirst = 41440, kBookmarkSetFirst = 41449, kBookmarkLast = 41457;
    constexpr UINT kBrushVisibility = 40953;
    constexpr UINT kAssemblies = 40922;
    constexpr UINT kSaveAssembly = 40923;
    constexpr UINT kFindMaterial = 40924;
    constexpr UINT kFindMesh = 40925;
    constexpr UINT kFitBuilderBrush = 40933;
    constexpr UINT kFitBuilderBrushToBrush = 40980;
    constexpr UINT kAddVertexPortal = 40981;
    constexpr UINT kPlaceBuilderBrush = 40982, kRebuildPlaceBuilderBrush = 40983;
    constexpr UINT kSmoothStairs = 41385, kRemoveStairSmoothing = 41386;
    // RE+ Tools > Add Conveyor Belt and the viewport's Add Conveyor Belt Here (ConveyorModel.h).
    constexpr UINT kAddConveyorBelt = 41395, kAddConveyorBeltHere = 41396, kAddNextConveyorBelt = 41397,
                   kAddNextConveyorBeltLeft = 41398, kAddNextConveyorBeltRight = 41399;
    constexpr UINT kSaveToEmitterLibrary = 40968;
    constexpr UINT kBrushSnapX = 40936, kBrushSnapY = 40937, kBrushSnapZ = 40938, kBrushSnapAll = 40939;
    constexpr UINT kSurfaceSnapX = 40940, kSurfaceSnapY = 40941, kSurfaceSnapZ = 40942, kSurfaceSnapAll = 40943;
    constexpr UINT kVertexSnapX = 40944, kVertexSnapY = 40945, kVertexSnapZ = 40946, kVertexSnapAll = 40947;
    bool HandleCommand(UINT command);
    // Camera bookmark slot (1-9) of the open map: store the viewport cameras in
    // it, with a thumbnail, or go back to them. From a key (fromMenu false) a
    // failure or an empty slot beeps and logs; from the menu it says so in a box.
    void CameraBookmark(int slot,bool store,bool fromMenu);
    bool AppendObjectiveMenu(HMENU menu,const Workflow::Json& actor);
    void RunObjectiveCommand(UINT command,const Workflow::Json& snapshot);
    void Initialize();
    // One bridge request, {"op": name, ...arguments}: the MCP ops (McpOps.h)
    // first, then the workflow ops ReloadedWorkflowRequest has always taken.
    // Returns the op's result and throws a sentence for the user on failure.
    // UI thread only.
    Workflow::Json Request(const Workflow::Json& request);
    // The open map's entry in the workflow library (library.json: Map Design's
    // "design", the MCP sidecar's "mcp", views, instances), and putting a copy
    // back, which a plan that fails does after design.register or the sidecar
    // changed it. An untitled map's entry stays in memory until its first save.
    Workflow::Json LibraryEntry();
    void RestoreLibraryEntry(const Workflow::Json& entry);
    // design.register: pieces design.block / design.blockbatch placed (a piece
    // or a list of them per entry), into Map Design's pieces, named as the
    // panel names new ones ("Room 3"), and element records ({kind, points,
    // members, command}) into its elements. Returns {registered, elements}.
    Workflow::Json RegisterDesignPieces(const Workflow::Json& pieces, const Workflow::Json& elements);
    // design.unregister: drops the pieces and elements with any of these brushes.
    size_t UnregisterDesignPieces(const Workflow::Json& members);
    // The geometric events Map Design and the bridge recorded (the editor keeps
    // no list of its own that RE+ can read).
    Workflow::Json DesignElements();
    // The MCP sidecar (maps[key].mcp): the level spec the map was built from.
    Workflow::Json McpSidecar();
    void SetMcpSidecar(const Workflow::Json& sidecar);
    void FindUsages(HWND owner, void* asset, bool mesh);
    // Find Usages in All Maps for the browser's current material or mesh.
    void FindUsagesInAllMaps(HWND owner, bool mesh);
    void RenameActorTag(HWND owner,std::string path,std::string type);
    // Save the selected emitters as a new Emitter Library entry: asks for the
    // name, category and description. Returns the saved entry, or null when
    // cancelled; errors are thrown for the caller to show.
    Workflow::Json SaveEmitterSelection(HWND owner, bool confirm = true);
    // Asks for an entry's name, category and description, starting from the
    // entry's own. Returns {name,category,description}, or null when cancelled.
    Workflow::Json AskEmitterDetails(HWND owner, const Workflow::Json& entry, const char* title);
    // The same with a library's own category choices (a JSON array of names).
    Workflow::Json AskDetails(HWND owner, const Workflow::Json& entry, const char* title, const Workflow::Json& categories);
}
