#pragma once
#include "WorkflowModel.h"
#include "CharacterSkinsImage.h"
#include <memory>

namespace MapCheck { struct Scene; struct Settings; struct FloorScan; }

namespace LightShadow { struct Scene; struct Settings; }
namespace LightShadow { struct Scene; }
namespace Reach { struct Scene; class World; }
namespace Minimap { struct Camera; struct Scene; }
namespace UnsavedChanges { struct Opened; }
namespace Outliner { struct Row; }

namespace Workflow::Editor
{
    void Initialize();
    std::filesystem::path Directory();
    std::string MapKey();
    // The open map's file as the frame shows it (unfolded), and the stock Save As setter.
    std::string MapFile();
    void SetMapFile(const std::string& path);
    std::string LevelPath();
    uintptr_t LevelIdentity();
    unsigned Revision();
    // Revision without the transactions that only change the selection.
    unsigned ContentRevision();
    unsigned MapGeneration();
    // The last MAP LOAD, NEW or IMPORT that succeeded, as it left the
    // generation and ContentRevision (UnsavedChangesModel.h).
    UnsavedChanges::Opened MapOpened();
    Json Actors(bool selectedOnly = false);
    Json SelectedIdentities();
    // Paths of the selected actors, cheaper than SelectedIdentities.
    std::vector<std::string> SelectedActorPaths();
    Json BrushVisibility();
    void SetBrushVisibility(int category, const std::string& action);
    Json HiddenActors();
    size_t RestoreHiddenActors(const Json& hidden);
    void Select(const Json& identities, bool focus = false);
    Json SelectedSurfaceBrushes();
    Json BspSurfaceOwners();
    Json SelectedMeshBounds();
    void FitBuilderBrushToMeshes();
    void FitBuilderBrushToBrushes();
    // Moves the builder brush to the viewport's last right-click point (grid
    // snapped along the surface when the grid is on), optionally rebuilding it
    // as the default cube. On a surface click it rests on the surface.
    void PlaceBuilderBrushAtClick(bool rebuild, bool onSurface);
    bool CanAddVertexPortal();
    Json AddVertexPortal();
    Json BrushSnapBounds(bool surfaces = false);
    Json SelectedBrushVertices();
    void SnapSelectedBrushVertices(unsigned axes);
    void SnapBrushesToGrid(unsigned axes, bool surfaces = false);
    Json CaptureView();
    std::string RestoreView(const Json& view);
    // Thumbnail framing: a viewport camera moved so the identities' locations
    // fill the shot (Thumbnail::FrameBox), and put back byte for byte.
    struct CameraState { uintptr_t camera = 0; std::array<unsigned char, 12> location{}, rotation{}; };
    CameraState FrameCamera(uintptr_t camera, const Json& identities, double aspect);
    void RestoreCamera(const CameraState& state);
    std::string CurrentAsset(bool mesh);
    Json FindUsages(const std::string& asset);
    void ReplaceUsages(const Json& usages, const std::string& source, const std::string& replacement);
    Json Connections(const std::string& actor);
    Json AddObjectiveActor(const Json& owner, const std::string& type);
    Json PreviewTagRename(const Json& actor,const std::string& newTag);
    void RenameTag(const Json& preview);
    Pose BuilderPose();
    Json CaptureAssembly(const Json& members, const Pose& frame);
    Json PlaceAssembly(const Json& definition, const Pose& frame, const std::map<std::string,std::string>& bindings);
    // The same, one Undo step, with every placed actor also filed in `group`
    // (a linked prefab instance's Group, PrefabModel.h) when it is not empty.
    Json PlaceAssembly(const Json& definition, const Pose& frame, const std::map<std::string,std::string>& bindings, const std::string& group);
    // Linked prefabs: every live actor whose Group names an instance, as
    // {path,class,name,groups,selected,position,rotation}.
    Json PrefabActors();
    // The Group field of each selected actor.
    std::vector<std::string> SelectedGroups();
    // Deletes each job's members ({members,group,position,rotation}) and
    // places `definition` in their place, filed in the job's group, all in one
    // Undo step. Returns per job {members,names,tags,position,rotation,removed}.
    Json RebuildPrefabInstances(const Json& definition, const Json& jobs);
    bool Compatible(const std::string& actorPath, const std::string& type);
    bool Exec(const std::string& command);
    void Redraw();
    // The editor's main frame (the root window the menus belong to), or null.
    HWND MainWindow();
    bool CanUndo();
    bool CanRedo();
    // A mark taken before a multi-step operation; UndoBackTo undoes every step
    // recorded since and drops them from the redo queue. Returns the count.
    unsigned UndoMark();
    size_t UndoBackTo(unsigned mark);
    // Reflected authoring API. Snapshots carry map identity and property values;
    // mutations reject stale snapshots and use one native transaction.
    Json ExportMapAuthoring();
    Json PreviewMapAuthoring(const Json& document);
    Json ApplyMapAuthoring(const Json& document, const char* label = "Import map JSON changes");
    // File > Compare with Backup (CompareBackupModel.h). Loads a map file beside the open map under a package
    // name of its own, reads both levels' actors the way Map JSON export does, unloads the copy and returns
    // {rows, tally, current, backup, ...}: what differs, the backup's values as "before".
    Json CompareWithMapFile(const std::filesystem::path& file);
    // Brings back the given rows of that comparison through the Map JSON import, as one Undo step. Refuses
    // when the map changed since the comparison.
    Json BringBackFromComparison(const Json& comparison, const std::vector<size_t>& rows);
    Json EventClasses();
    Json CameraNetworks();
    void OrderCameras(const Json& snapshot,const Json& paths,bool loop,const std::string& detach = "");
    Json AddNetworkCamera(const Json& snapshot,const Json& paths,bool loop);
    Json EventAssets(const std::string& type, bool classes = false);
    Json CreateEventComponent(const Json& owner, const std::string& type);
    void CaptureMoverKey(const Json& mover, int key);
    void PlayLevel();
    void BuildGeometry();
    unsigned long GeometryBuilds();
    Json DesignScene();
    // A brush's wireframe for the Map Design panel, kept out of JSON: a
    // medium map holds hundreds of thousands of edges, and as JSON each one
    // costs the 32-bit editor a dozen allocations. Edges shared by two
    // polygons are kept once. low and high bound every polygon vertex, even
    // when the edges were left out to stay within the panel's budget.
    struct DesignWire
    {
        std::vector<std::array<float,6>> edges;
        Vector low{},high{};
        bool measured=false;
    };
    // The panel's scene: DesignScene's actors without "edges", each with
    // "index" into wires and "brush" when it has polygons. Polygons with
    // unreadable vertices are skipped rather than failing the whole map, and
    // past the edge budget brushes keep only their bounds (counted in dropped).
    Json DesignScene(std::vector<DesignWire>& wires,size_t& dropped);
    Vector DesignGrid();
    // Followers move by delta inside the same transaction: a group keeping up
    // with one of its members.
    Json DesignBlockout(const Json& spec,const Pose& frame,const Json& previous = Json{},const Json& followers = Json{},const Vector& delta = {});
    Json DesignBlockoutBatch(const Json& items,const Json& followers = Json{},const Vector& delta = {});
    void DesignAlign(const Json& scene,int axis,const std::string& mode,double spacing);
    void DesignLayer(const Json& members,bool hidden,bool locked,const std::string& group = "",const std::string& groupAction = "none");
    void DesignSetFlags(const Json& members,int hidden,int locked);
    size_t DesignTurnActors(const Json& members,double degrees,const Vector& pivot);
    Json DesignActorSpans();
    size_t DesignSetHidden(const Json& hide,const Json& show);
    void DesignGroupMembers(const Json& members,const std::string& group,const std::string& action);
    void DesignTranslate(const Json& members,const Vector& delta);
    Json CreateLift(const Vector& position,double width,double length,double thickness,double rise,double moveTime);
    Json DesignPolyFlags(const Json& members);
    size_t DesignSetPolyFlags(const Json& members,const Json& flags);
    size_t DesignSendToLast(const Json& members);
    Json DesignSpawns();
    Json DesignClearances();
    void DesignPlay(const Json& start,const Pose& pose,bool launch = true);
    Json DesignTemporaryStart(const std::string& type,const std::string& team,const Pose& pose);
    // Lighting Budget: every light with the raw flags the stock light checks
    // read, the in-game render sphere of those that count, each BSP leaf's
    // in-game light list (regenerated as Tools > Check InGame / Dynamic Lights
    // in Leaves does, then restored to the build's lists) and the zone names.
    // See LightingBudgetModel.h.
    Json LightingBudgetScene();
    // Map Check (MapCheckModel.h): the BSP as last built, zones, zone portals,
    // actors with the editor's encroachment test, and the stock CheckForErrors
    // entries (when settings.stockChecks).
    MapCheck::Scene MapCheckScene(const MapCheck::Settings& settings);
    // Map Check's floating meshes: how far Drop to Floor would move each
    // visible static mesh, tracing at most maxGap below it, and the boxes of
    // the meshes that do not block (the trace may not see them).
    MapCheck::FloorScan MapCheckFloorGaps(double maxGap);
    // Selects the actors (framing them when asked) and the BSP surfaces of a row.
    size_t SelectMapCheckRow(const std::vector<std::string>& actors, const std::vector<int>& surfaces, bool frame);
    // Light and Shadow map (LightShadowModel.h): walkable BSP floor triangles
    // with their lightmap UVs and atlases, the in-game lights at their render
    // spheres, the BSP planes for line of sight, and (as the settings ask) the
    // static meshes as blockers and floors. Atlases and mesh shapes are kept
    // between calls while they stay the same; LightShadowForget drops the atlases.
    struct LightShadowSceneStats { size_t bspFloors=0, meshFloors=0, meshTriangles=0; };
    LightShadowSceneStats LightShadowScene(LightShadow::Scene& scene, const LightShadow::Settings& settings);
    void LightShadowForget();
    // spheres, and the BSP planes for line of sight.
    void LightShadowScene(LightShadow::Scene& scene);
    // Reachability (ReachabilityModel.h): the player starts with their teams,
    // the objectives and pickups, the BSP's bounds and the pawn sizes of the
    // loaded spy and merc classes; and the engine's collision queries on the
    // open level (a swept box and the spawn test), which throw once another
    // map is opened.
    Reach::Scene ReachabilityScene();
    std::unique_ptr<Reach::World> ReachabilityWorld();
    // Player count rules (ObjectivePlayersModel.h): the map's SPlayerCountRules actor
    // (or a version 1 SObjectivePlayers one, moved over on the next change). Rules:
    // {placed, legacy, extra, compiled, actor, playLevel (the count an editor Play Level
    // plays with, 0 for every objective), rules:[{path, min, max, open, mover}]}. Setting
    // an objective's, zone mission's or mover's range compiles and places the actor when
    // needed, in one Undo step; 0 and 0 clears the rule, and the actor goes once nothing
    // is left. open (movers only): opens and locks open inside the range.
    Json ObjectivePlayerRules();
    Json SetObjectivePlayers(const Json& subject, int minimum, int maximum, bool open = false);
    Json SetObjectivePlayLevelPlayers(int players);
    // A mission's objectives to win by match size: sizes is [1v1, ..., 8v8], each 1 to
    // ObjectivePlayers::MostTargets or 0 for the mission's own MinimumObjectives; all 0
    // clears it. Rules gains targets:[{path, sizes}].
    Json SetMissionTargets(const Json& mission, const Json& sizes);
    // The identity of a live SObjective (or SMission) or Mover actor at that address, as
    // a Properties window holds it, with mover and mission flags; null for anything else.
    Json ObjectivePlayersSubject(uintptr_t object);
    // The SMission actors only, as ObjectiveActors lists them (path, objectives,
    // minimum): it never touches the selection.
    Json ObjectiveMissions();
    // Conveyor belts (ConveyorModel.h). State: {compiled, version, belts:[identity]}.
    // Prepare imports the belt's texture and meshes and compiles its classes into the
    // map package when they are missing or older. Add places a belt (spec: optional
    // location [x,y,z], rotation [pitch,yaw,roll] and properties {name: value}),
    // at the viewport's last click when no location is given, with the stock items;
    // with after (a belt's name) it continues that belt and is linked in after it.
    // Sync sizes every belt's frame to its Length and Width as they are edited.
    Json ConveyorState();
    void PrepareConveyors();
    Json AddConveyorBelt(const Json& spec);
    // The one selected actor when it is a conveyor belt (an identity), else null.
    Json SelectedConveyorBelt();
    size_t SyncConveyorBelts();
    // Import OBJ (ObjImportModel.h, ObjImportNative.inl). spec: file, package (MyLevel is
    // the open map), group, name, scale, up ("y"/"z"), collision (none, box, polygons,
    // groups, parts, file), split, partTriangles, place. Preview reads the files and says
    // what the import would make (triangles, hulls, parts, each material's source) without
    // changing anything; Import makes the meshes and, with place, puts them where the
    // viewport was last clicked in one Undo step.
    Json ObjImportPreview(const Json& spec);
    Json ImportObjMesh(const Json& spec);
    // Character Skins (CharacterSkinsModel.h): the map's Character Skins actor.
    // Settings: {placed, legacy, extra, compiled, slots:{SpyBody,...: material path or ""},
    // models:{SpyModel,...: skeletal mesh path or ""}, goggles:{SpyGoggleOffset,...: [x,y,z]}}.
    // Apply compiles the class into the map package when needed, replaces an earlier
    // version's actor, places the actor once and sets its values in one Undo step;
    // Remove deletes the actor (of any version).
    Json CharacterSkinSettings();
    Json LoadedSkeletalMeshes();
    Json ApplyCharacterSkins(const Json& slots, const Json& models, const Json& goggles);
    void RemoveCharacterSkins();
    // Writes the stock textures of the four slots as 32-bit TGA files (SpyBody.tga, ...).
    Json ExportDefaultCharacterSkins(const std::filesystem::path& folder);
    // Imports an edited image into MyLevel.CharacterSkins for a slot; returns its path.
    // name, when given, is the texture's name; otherwise the slot's, made unique.
    std::string ImportCharacterSkin(const std::string& property, const std::filesystem::path& file, const std::string& name = {});
    // Character Skin presets (CharacterSkinPresetsModel.h): one-team presets, built-ins
    // painted over the stock textures and the user's in skin_presets.json with their
    // pictures in skin_presets\<id> (a version 1 file is split into team presets when
    // first read). View: {entries (merged, flagged), categories, problems,
    // wearing:{spy, merc: what each team wears on the map, in words}}.
    Json SkinPresets();
    // The preset's team's two slots' pictures (body, head) as the preset would dress
    // them: painted, read from its files, read from a loaded texture, or the stock
    // picture for a stock slot. An empty image where none can be shown (a material that
    // is not loaded). A size above 0 returns size x size thumbnails, painted on shrunk
    // stock pictures (fast).
    std::vector<CharacterSkins::Image> SkinPresetImages(const Json& entry, int size = 0);
    // The four slots' pictures as the map's Character Skins dress them, in Slots order.
    std::vector<CharacterSkins::Image> MapSkinImages(int size = 0);
    // Imports the preset's pictures (reusing any already imported) and applies it with
    // ApplyCharacterSkins, the other team's values as the map has them; returns the
    // map's settings.
    Json ApplySkinPreset(const std::string& id);
    // Saves one team's Character Skins values (team "spy" or "merc") as a new user
    // preset; details holds name, category and description. Textures stored in the
    // map are kept as pictures.
    Json SaveSkinPreset(const std::string& team, const Json& slots, const Json& models, const Json& goggles, const Json& details);
    Json UpdateSkinPreset(const std::string& id, const Json& changes);
    // Deletes a user preset with its pictures, or hides a built-in.
    void DeleteSkinPreset(const std::string& id);
    void RestoreSkinPresets();
    // A preset file to share (*.skinpreset) with its pictures inside, and back; import
    // returns the saved entries (two for an old file that dressed both teams).
    void ExportSkinPreset(const std::string& id, const std::filesystem::path& file);
    Json ImportSkinPreset(const std::filesystem::path& file);
    // A user preset for a team carrying a model: the PSK (rigged to the team's stock
    // skeleton), one TGA per material named after it beside it, and the goggle offset
    // from a JSON beside it when there is one. details: {name, category, description}.
    Json AddSkinPresetModel(const std::string& team, const std::filesystem::path& psk, const Json& details);
    // Generate Minimap (MinimapModel.h): the built BSP faces, static mesh boxes
    // and the LevelInfo's SnapshotCamera / MapFloors. ApplyMinimap imports one
    // TGA per floor as <map package>.Minimap.Floor<n> and sets SnapshotCamera
    // and MapFloors (FloorZ, texture) in one Undo step; returns {floors:[{z, texture}],
    // unused:[Floor textures past the new count, left from an earlier run]}.
    Minimap::Scene MinimapScene();
    Json ApplyMinimap(const Minimap::Camera& camera, const std::vector<std::pair<double, std::filesystem::path>>& images);
    // File > Export > Map to OBJ (ObjExportModel.h). Options: folder, meshes,
    // textures, selectedOnly (bools) and centimetres (per Unreal unit).
    // Writes <Map>.obj, <Map>.mtl and textures/*.png into the folder and
    // returns what it wrote: obj, mtl, polygons, actors, meshes, faces,
    // vertices, objects, materials, textures, texturesFailed, seconds.
    Json ExportMapObj(const Json& options);
    // Selects exactly the live actors with these paths, one pass over the
    // level; focus frames them in the viewports. Returns how many were found.
    size_t SelectActorPaths(const std::vector<std::string>& paths, bool focus);
    // Map Outliner (MapOutlinerModel.h). OutlinerRows reads every actor but the
    // viewport cameras in one pass, in the level's order: name, path, class,
    // Tag, Event, Group, location, hidden, selected and bLockLocation, with
    // the class's property offsets found once per class. The key changes
    // whenever the rows may be stale: a map opened, an edit, the actor list
    // grown or moved, a stock Undo or Redo. With the key unchanged the row
    // handles are live, and OutlinerFlags re-reads only the hidden and
    // selected bits (1 when a selection bit changed, 2 a hidden one). OutlinerSelect selects exactly
    // those actors (focus frames them) and returns how many it found;
    // OutlinerDelete deletes them, never the level info, the builder brush or
    // a camera, in one Undo step, and returns how many went.
    struct OutlinerKey
    {
        unsigned generation = 0, content = 0; int actors = 0; uintptr_t data = 0; int undoPosition = 0;
        bool operator==(const OutlinerKey&) const = default;
    };
    void OutlinerRows(std::vector<Outliner::Row>& rows);
    OutlinerKey OutlinerChangeKey();
    unsigned OutlinerFlags(std::vector<Outliner::Row>& rows);
    size_t OutlinerSelect(const std::vector<uintptr_t>& actors, bool focus);
    size_t OutlinerDelete(const std::vector<uintptr_t>& actors);
    // Lighting Budget fixes, one undoable transaction: fix 0 makes the lights
    // static, 1 turns them off (LightingBudget::Applied). Returns how many of
    // the paths were found and changed.
    size_t LightingBudgetFix(const std::vector<std::string>& paths, int fix);
    // Render Budget (RenderBudgetModel.h): every actor (zone, kind, drawn in
    // game, static mesh triangles, materials), every BSP polygon's zone,
    // surface and triangles, the surfaces' materials, the materials' textures
    // and the zone names.
    Json RenderBudgetScene();
    // The perspective viewport's camera, the actors the engine draws from it
    // (on a fresh repaint), the BSP nodes for the point-in-zone walk and the
    // zone portal polygons.
    Json RenderBudgetView();
    // Recovery report (MapRecoveryNative.inl): selects actors by object name,
    // or BSP surfaces by index, and optionally frames them in the viewports.
    size_t SelectActorNames(const std::vector<std::string>& names, bool focus);
    size_t SelectSurfaces(const std::vector<int>& surfaces, bool focus);
    // Security devices: creation, the motion sensor with its volume, and wiring.
    Json SecurityActors();
    Json Lights();
    Json ObjectiveActors();
    Json CreatePlayerStart(const std::string& type,const std::string& team,const Pose& pose);
    Json AddAlarmLocks(const Json& alarm,const Json& targets);
    Json SetActorProperties(const Json& identity,const Json& properties);
    Json InspectSettingsActor(const Json& identity);
    Json CreateSecurityActor(const std::string& type,const Pose& pose,const Json& properties);
    Json CreateMotionSensor(const Vector& low,const Vector& high,const Json& properties);
    Json MoveSecurityActor(const Json& identity,const Pose& pose,const Json& properties,const Json& followers = Json{});
    void LinkDetectorToAlarm(const Json& detector,const Json& alarm);
    void UnwireDetector(const Json& detector);
    void DeleteSecurityActor(const Json& identity);
    void AddAlarmOutputs(const Json& alarm,const Json& targets);
    void DesignRemoveTemporaryStart(const Json& identity);
    // Stages: the actors a plan can name, the plan the map carries, and
    // applying a plan in one Undo step.
    Json StageActors();
    Json StagePlan();
    Json PreviewStages(const Json& plan);
    Json ApplyStages(const Json& plan);
    Json InspectActor(const Json& identity);
    // Copy Properties / Paste Properties (PropertyPasteModel.h). Copy: {actor,
    // level, generation, properties:[{name, category, schema, value,
    // lineage}]}, every editable property, and StaticMesh, DrawType and
    // DrawScale(3D) on actors other than brushes, whose value can be written
    // back; level and generation say which map it came from. Paste writes
    // each entry onto every target that has the property with the same
    // schema, in one Undo step ("Paste properties"); returns {actors,
    // properties (values written), skipped}.
    Json CopyActorProperties(const Json& identity);
    Json PasteActorProperties(const Json& targets, const Json& properties);
    Json ExportEventJson(const Json& identity);
    void ImportEventJson(const Json& snapshot,const Json& document);
    void EditActor(const Json& snapshot, const Json& changes);
    Json CreateEventActor(const std::string& type, const Json& event = Json{}, bool trigger = false, const std::string& geometry = "point", int group = 0);
    void LinkEventActor(const Json& event, const Json& target, bool trigger, int group = 0);
    // Emitter Library (entry schema in EmitterLibraryModel.h). SelectedEmitters
    // is cheap enough for building a context menu. CaptureEmitters returns an
    // unsaved draft (no id) around the members' centre; map names its source
    // when the frame's file name is not the open map. PlaceEmitterEntry is one
    // Undo step and selects the new actors. Storage calls do not need a map.
    // A damaged emitter_library.json leaves the built-ins and packs listed,
    // reports EmitterLibrary::DamagedFile() as its only problem and refuses writes.
    // Effect packs are read from Directory()/EmitterPacks/*.json; a pack entry
    // whose required packages are not installed refuses to place with
    // EmitterLibrary::MissingPackagesMessage.
    Json SelectedEmitters();
    Json CaptureEmitters(const Json& members, const std::string& map = "");
    Json PlaceEmitterEntry(const Json& entry, const Pose& pose);
    // The library as one snapshot, rebuilt only when emitter_library.json or a file
    // in EmitterPacks changes (size or time) or this editor writes the user file;
    // each rebuild has a new revision. Unchanged pack files are not read again.
    // Hold the shared lists instead of copying them.
    struct EmitterLibraryView
    {
        std::shared_ptr<const Json> entries;    // built-ins, pack entries, your entries (EmitterLibrary())
        std::shared_ptr<const Json> categories; // EmitterCategories()
        std::shared_ptr<const Json> problems;   // EmitterLibraryProblems(): the user file's first, then the packs'
        std::shared_ptr<const Json> packs;      // listed packs: {id,name,description,requires,entries,problems,file,hidden}
        size_t userProblems = 0;                // how many problems are the user file's
        unsigned revision = 0;
    };
    EmitterLibraryView EmitterLibraryState();
    Json EmitterLibrary();
    Json EmitterLibraryProblems();
    Json EmitterCategories();
    unsigned EmitterLibraryRevision();
    // The listed packs with "missing": their required package files not in Packages.
    Json EmitterPacks();
    // A pack entry's required package files (that its dependencies use) that are not installed.
    Json MissingEmitterPackages(const Json& entry);
    // For tests: {"revision","packReads" (pack files parsed so far),"entries","packs","problems"}.
    Json EmitterLibraryCacheState();
    Json SaveEmitterEntry(const Json& entry);
    Json UpdateEmitterEntry(const std::string& id, const Json& changes);
    void DeleteEmitterEntry(const std::string& id);
    void RestoreBuiltinEmitters();
    // Placement tools (PlacementModel.h). PlacementSelection: the selected
    // actors (never the level info, builder brush or cameras) as identities
    // with address (for ordering only), name, position, rotation, box lo/hi,
    // kind (mesh, brush for volumes and movers, csg, actor) and locked.
    // SelectedActorAddresses is cheap enough for every viewport message.
    Json PlacementSelection();
    std::vector<uintptr_t> SelectedActorAddresses();
    // Moves actors by their "delta" in one Undo step; locked ones stay.
    // Returns how many moved (none: no Undo step).
    size_t PlacementMove(const Json& moves, const std::string& label);
    // Turns actors to their "rotation" in one Undo step; locked ones, and ones
    // already turned that way, stay. Returns how many turned (none: no Undo step).
    size_t PlacementTurn(const Json& changes, const std::string& label);
    // Drops the selection onto the surface below (0), above (1) or ahead (2),
    // or pushes it to the wall towards +X, -X, +Y or -Y (3-6), optionally
    // turning meshes to sit flat on it, in one Undo step; a drop that changes
    // nothing leaves none. Returns {moved,missed,unchanged,locked,brushes}.
    Json PlacementDrop(int surface, bool align);
    // Copies of the members, one per {before,pivot,yaw,turn,after}
    // (Placement::Copy), pasted in one Undo step; the originals and copies
    // end up selected. Returns the copies' identities.
    Json PlacementCopy(const Json& members, const Json& copies, const std::string& label);
    // Smooth Staircase (StairSmoothModel.h). The selected CSG brushes, plus on
    // the surface menu the brushes of the selected faces. State: {stairs,
    // steps, winding, ramps (earlier smoothing found)}, never throws for a
    // selection that is not a staircase. SmoothStairs lays an invisible
    // BlockingVolume ramp over each staircase's step edges in one Undo step,
    // replacing its earlier ramps; returns {ramps, replaced, steps, winding,
    // brushes}. RemoveStairSmoothing deletes those ramps; returns the count.
    // Optimise Map Assets (MapOptimiseModel.h). Report: {map, mapFile, packs:[{name,
    // file, fileSize, usedSize, suggested, assets:[{path, class, size, found}]}],
    // inside:[{path, class}], text, unreadable}: every asset the map reaches outside
    // itself and the game's code packages, by package, and the ones it carries. Release saves the map, then writes a copy under the
    // map's own name into options.folder (default ReleaseFolder), laid out like the
    // game (Packages/MapsEd, Maps, Textures/<Map>-i), carrying the assets it uses
    // from options.packs, inside it (destination "map") or in
    // options.assetPackage.usx ("package"), and puts every asset back;
    // options.overwrite replaces existing files. Returns {moved:[{from, to}],
    // written:[{file, size}], needs:[{name, file, size}], left, rounds, release,
    // folder, playable}.
    // With a base install, a pack whose file is identical there is installed
    // (players have it): never suggested, and listed last.
    Json OptimiseReport(const std::filesystem::path& base={});
    Json OptimiseRelease(const Json& options);
    // Where a map's release copy goes by default: <game>/Releases/<Map>.
    std::filesystem::path ReleaseFolder(const std::string& map);
    // Mover Workbench (MoverModel.h). SelectedMover: the one selected actor when it
    // is a mover (an identity), else null; cheap enough for a context menu. State:
    // {actor, name, class, revision, level, path (Movers::PathJson), location, rotation,
    // values:{MoveTime, StayOpenTime, ..., sounds}, choices:{enum and InitialState names},
    // triggeredBy:[{actor, property}], triggers:[{actor, property, resolved}]}.
    // SetMoverPath writes a key list and puts the mover on its key, SetMoverSettings
    // the other settings (by name, as text), WireMoverTrigger points a trigger's
    // Event at the mover's Tag: each one Undo step, returning the new state.
    // MoverPreviewPose moves the mover for drawing only (no Undo step, nothing
    // marked changed) until MoverRestorePose puts the bytes back.
    Json SelectedMover();
    Json MoverState(const Json& identity);
    Json SetMoverPath(const Json& identity, const Json& path, const std::string& label);
    Json SetMoverSettings(const Json& identity, const Json& changes);
    Json MoverOutline(const Json& identity);
    void MoverPreviewPose(const Json& identity, const Pose& pose);
    bool MoverPreviewing();
    void MoverRestorePose();
    Json MoverTriggerCandidates(const Json& identity);
    Json WireMoverTrigger(const Json& identity, const Json& trigger);
    Json StairSmoothState(bool surfaces);
    Json SmoothStairs(bool surfaces);
    size_t RemoveStairSmoothing(bool surfaces);
    // Scatter Props (ScatterModel.h). ScatterBrowserMesh: the Static Mesh
    // Browser's current mesh, or empty. ScatterMesh: {path, lo, hi} of a mesh,
    // loading its package when needed. ScatterArea: the builder brush seen from
    // above {circle:false, polygons, top, bottom}, or a circle of the radius
    // round the last viewport click {circle:true, centre, radius, z, top,
    // bottom}. ScatterPlan: where the props would rest (see ScatterNative.inl);
    // ScatterPlace pastes them in one Undo step, in the Scatter group, selected.
    std::string ScatterBrowserMesh();
    Json ScatterMesh(const std::string& path);
    Json ScatterArea(bool circle, double radius);
    Json ScatterPlan(const Json& request);
    Json ScatterPlace(const Json& props);
}
