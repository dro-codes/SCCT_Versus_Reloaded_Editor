# Regression tests

## HELI02 compiled-map recovery

`tools/test_map_recovery.cmd` covers the exact HELI02 brush whose three-vertex
bevel falls below native `CalcNormal` minimum area. Repair keeps the remaining
supporting planes and a closed brush within the native BSP precision band;
a long tapered brush verifies that distant intersections are rejected without
changing the input. Actor tests cover omitted all-null `MoversToLock` entries
while retaining checks for live references and other struct fields, and every
I3DL2 reverb preset a `ZoneInfo.ZoneEffect` can name — the whole standard set,
not the presets that happened to turn up in the maps recovered so far, since a
map-local object whose class is not recognised stops recovery rather than being
moved to the dependency package. Anything else map-local, an `ESBPatch` among
them, is still refused.

The native recovery harness with `-SourceMap <HELI02.sdc>` additionally checks
the map's duplicate portal outlines (cooked surfaces 1165 and 1168), recovery,
save, normal reopen and rebuild in a disposable installation.

The same script builds `SdcBlockModelTests.cpp` and
`RecoveredAssetPackageTests.cpp`, which cover reading a compressed package
(a saved `.sdc` map): a run of blocks, each two sizes and a zlib stream. The
game starts a block every 15 MB, but a map saved by the editor is one block as
large as the map, so a block has no size limit of its own. Its compressed
bytes must lie inside the file, its uncompressed size must be one those bytes
can produce (zlib expands at most 1032 times) and the package must stay within
1 GB; anything else is refused before it is decoded. The package tests decode a
72 MB block that did not compress, the same package in 15 MB blocks and 96 MB
of zeros (zlib's best ratio), and refuse truncated, overstated, empty and
oversized blocks, wrong sizes, trailing data and a package without its magic,
never touching an existing destination or temporary file.

## Map Design

`tools/test_workflow_tools.cmd` includes `MapDesignModelTests.cpp`: outward polygon
winding and volume, carve and shell room/corridor construction, zone portal sheets
and their brush flags, stair rise, ramp geometry, invalid dimensions, grid
snapping, bounds, snapping a dragged face or box onto neighbouring geometry,
projected convex outlines, vent and crawlway shapes with crouch clearance,
attaching a new piece to a wall, route journeys with climbs and crouched sections,
both teams' route times, a material on every generated polygon, the design check's unreached rooms, orphan doorways and merging carves, preview resizing against a fixed opposite face, traversal warnings for
steps/slopes/clearance, doorway placement in a room wall including rotated rooms
and rejected offsets, route lengths and spy/merc timings, floor ranges, the
storeys behind the floor slider (which brushes leave a surface to stand on and
which do not — walls, pillars and the volume a map is carved from — placed
pieces nesting a vent into the room's storey, an imported map's floors read from
the width of those surfaces rather than collapsing into one level, a floor laid
as many slabs weighing as much as one hall, scenery and crate lids excluded,
surfaces a step apart counted once, and the cap on how many storeys are kept),
native
group name handling, portable workspace export/import, image calibration, 3D
measurement, stable distribution, and repeated brush/object/event reference
remapping, mirrored poses checked vertex by vertex against the reflected
geometry, the piece clipboard's anchor and offsets, sightlines through a turned
doorway and through a wall, size presets, numbered names, and the floors: the
room under a point setting the floor spacing (its height as resized, a shell
room's wall included, shafts, openings and stairwells never counting, the room
whose floor the depth is on among stacked ones) and where a click places (the
Depth in the top view, the floor of the room under the click in the front and
side views, the clicked height over no room). `SecurityModelTests.cpp` covers the security device catalogue, Unreal
yaw from two points, laser length and direction from two clicks, beam ends,
sensor boxes, tag allocation, and the wiring report with its unwired detectors,
unfed alarms, dangling events and empty sensors.

The native `-GenerateFixture -WorkflowTools` run creates and resizes real brushes,
checks Undo/Redo identities, imports ramps/stairs, places a carved room and a
portal doorway and confirms the sheet counts as a zone portal, writes and clears
layer membership in the native `Group` field, reads the editor's grid spacing,
toggles native layer flags, and verifies temporary spawn restoration, including a
temporary team start that is created and removed again. The Play Level test
intercepts its launcher in the disposable process, checking the temporary pose at
launch without starting a game. UI checks use the actual blockout, doorway,
reference, calibration, measurement, player-reference, movement-limit, floor-filter, snap,
layer, route, route-comparison and workspace dialogs, box-select by dragging in the
design view, and the direct editing loop: a new blockout opening in the inspector
instead of a dialog, a placed piece loaded back into it, an inspector edit
replacing that piece's brushes rather than adding more, an arrow key nudging it by
one grid step, Escape ending the edit, the panel's Undo and Redo reaching the
editor's history, Copy and Paste placing a copy at the cursor and at a
right-clicked point, Duplicate placing one beside the original, Delete removing
a copy's brushes while its library entry stays, Undo bringing them back as that
piece and Redo deleting them again, a size preset resizing a placed piece in place, a
sightline inside a room reporting clear, the frame's Reloaded selection and
visibility commands (all of a class, invert, hide, unhide all), and the quick-add badge on a hovered wall placing a corridor
against it through its menu, the stale-geometry flag clearing when B runs a
geometry build, the design check window listing its issues, and the security
tools: creating an alarm, a laser and a motion sensor with its volume through the
ops, linking the laser's Event to the alarm's Tag and the alarm's outputs to a
target Tag, then the Security window placing an alarm with one click and a laser
and a motion sensor with two clicks each, the laser wired to that alarm with its
length and direction from the clicks, then moving a laser and a sensor (with its
volume) through `security.move` and by dragging the laser's body and its beam-end
handle in the plan, the badge sitting outside a hovered wall and surviving the
cursor moving onto it, the menu bar replacing most side buttons, and the
right-click menu starting a corridor preview where the plan was clicked. A piece's brushes are also checked against its
preview at a position off the editor grid. Locks, groups, group moves and lifts go
through `design.flags`, `design.group`, `design.translate` and `design.lift`, each
checked to be one Undo step; the lift is inspected for its two keys and its
stand-on state. `map_design_preview.bmp`, `map_design_workspace.bmp`,
`design_workspace.json` and `design_clearances.json` are retained in the fixture.
All native tests use a separate temporary installation, never a working map.

`LevelSnapshotModelTests.cpp` covers the level snapshot's pure parts: the
snapshot package name, the 24-bit BMP handed to the editor's texture importer
(bottom-up rows, padding), the `.utc` container header, the package tables of
a stock-shaped `<Map>-i` package (position-ciphered names, numbered name
references, imports, exports with their classes), a texture export's size,
format and mip count read past struct, bool and float properties, and the
check of the saved file (a 256 x 256 Menu texture, no objects lost, truncated
or foreign files rejected). The editor side imports the BMP with `TEXTURE
IMPORT` into the loaded `<Map>-i` package and saves it with the stock
`SAVEMAPPROP`, so it is exercised in the editor rather than here.

`AppearanceModelTests.cpp` covers the Appearance setting's pure parts: the
theme names read from and written to `Reloaded_Editor.ini`, which font faces
and sizes become Segoe UI 9 pt, rewriting a dialog template's font (plain and
extended templates, items with text, ordinal titles and creation data), checked
by making the template into a real hidden dialog and comparing its controls,
and the Modern Dark palette. Hooking the editor's window calls is checked in
the editor with the smoke probe's `appearance` suite.

`MinimapModelTests.cpp` covers Generate Minimap's pure parts: the game's
world-to-picture transform through `LevelInfo.SnapshotCamera` (checked with
AquaD's own camera: the centre, both corners, +X right and +Y down, a turned
camera, back from pixel to world), framing a map in a square and the stock
camera form that shows it, the game's floor choice from `MapFloors` and the
height bands of the floors, which faces and meshes are floor, silhouette,
wall or obstacle on a band, the footprint hull, the gameplay framing that
leaves an outdoor map's backdrop out, marker numbering by objective, the
typed floor list, the property text for `SnapshotCamera` and `MapFloors`,
the bottom-up 32-bit TGA, and the check of the saved interface package's
Briefing texture. Reading the BSP, importing the pictures and setting the
LevelInfo happen in the editor and were checked there on ShipD and AquaD.

`CrashRecoveryModelTests.cpp` covers the crash-recovery offer's pure parts:
the session marker read back (an `=` in the map path, LF endings, unknown
keys) and rejected without its pid or creation time, a marker of a live
editor told from one whose process is gone or whose pid was reused, the
latest of several abandoned sessions, the stock autosave names (not
`Autoplay.sdc` or `Auto0.sdc.bak`), the newest autosave of a session with
older ones ignored, the session's crash report by pid and time (not
`pid12345` for `pid1234`, nor an earlier process's report), and the offer's
choices and text with and without an autosave, the map or a crash report. The
start-up offer itself, the marker's clean-exit removal and **File > Open
Latest Autosave...** were checked by hand in a disposable installation; the
native runner turns the offer off (`[CrashRecovery] OfferAtStartup=0`) because
the workflow suite stops and restarts the editor.

`SaveBackupModelTests.cpp` covers the map backups' pure parts: the file a
`MAP SAVE` or `MAP LOAD` command names (quoted or not, in any case, among other
parameters; not a package save, a `SAVEAS` or an empty `FILE=`; the stock
autosave's own command), Play Level's `Autoplay.sdc` left
alone, backup names stamped with the time and read back from the end (a map
name with underscores of its own, invalid dates refused), whether the file on
disk still needs a copy (size and write time against the newest backup only),
the times a copy keeps, `SavedOver.txt` read by file name in any case and with
LF endings, pruning (the newest five of each map, `Keep`, a map's newest always
staying, the `MaxTotalMB` cap going oldest first across maps, a backup saved
over neither counted nor removed, files that are not backups never deleted),
and which paths are backups (`ReloadedEditor\Backups` with either slash, not
MapsEd maps or snapshot backups).

`UnsavedChangesModelTests.cpp` covers when the unsaved-changes warning before
Open Recent, Find Usages in All Maps and Open Map Backup is due: a map is clean
at the revision its own load left, however late that is noticed, so edits made
straight after a stock File > Open still count; each map opened starts over;
a new map is clean and an imported one unsaved until saved; Open Recent's
settings and Save target are part of opening; a save is a save only into the
open map's own file (any case, either slash; Save As under its new name), so
the stock autosave, Play Level's runtime copy, a release copy and a save while
the map has no file leave the warning in place, as a cancelled or refused save
does by never being reported.

`EntryThumbnailModelTests.cpp` covers the Working Views and Actor Assemblies
thumbnails' pure parts: file names taken from the entry id (lower case, one
per kind, ids that could leave the folder refused), parsing them back, the
sweep of pictures no saved entry owns (deleted entries and interrupted writes,
never foreign files), the saved size (256 wide with the viewport's aspect, a
tall viewport fitted by height), letterboxing into the list's cells, and the
camera pose that frames an assembly's actor locations (yaw kept, a fixed
three-quarter pitch, far enough for the narrower field of view, a minimum
distance for one actor, bad FOV or bounds handled).

`WindowPlacementModelTests.cpp` covers where RE+ windows reopen: the
`[WindowPlacement]` entry written and read back (negative places, maximised,
blanks around the numbers; four or six numbers, a maximised flag other than 0
or 1, no size, letters, decimals, an empty field and an out-of-range number
refused), sizes kept between a usable minimum (or the window's own, when
larger) and 16384, and whether the place can be used: enough of the title bar
on one monitor's work area to drag it (maximised edges just off it, a window
mostly off either side, a few pixels above the top), not a sliver, not under
the taskbar or above the monitor, not on an unplugged second monitor, and no
overflow far beyond any monitor.

`ViewBookmarkModelTests.cpp` covers camera bookmarks, which are Working Views
with a key: slots 1 to 9 only (0, 10, negative, text and fractional slots are
not keys), finding a slot's entry among ordinary views, a new bookmark holding
the cameras with no actor visibility to restore and the hidden groups left as
they are, Ctrl+N again updating that entry in place (id and name kept, cameras
and time refreshed, nothing appended), ordinary views never touched, two
entries holding one slot (the first keeps it, the other loses only its slot),
and a missing view list starting a new one.

`UsageMarksModelTests.cpp` covers Find Usages' check boxes as the list is read
again: rows keyed by actor path (any case), property and BSP surface; the
checks a Refresh gives with **Limit to selection** on and off; marks following
reordered rows and duplicate keys pairing up in order; new rows checked when
replaceable or unchecked under the limit; a row that becomes read-only
unchecked and one that becomes replaceable checked again unless it was unchecked
by hand; a row unchecked by hand remembered while read-only and when it leaves
the list, so it comes back unchecked; and after a geometry build, surface
numbers no longer trusted, so surface rows come back unchecked when any face
had been left out, while actor rows keep their marks.

`MapUsagesModelTests.cpp` covers Find Usages in All Maps on generated SCCT
packages: object names (class-quoted, groups with spaces, malformed names
refused), matching imports by their whole owner chain rather than by name (a
bare name, a skipped group, a prefix or a same-named object in another
package is not a match; a package or group matches everything inside), and
counting references in tagged property lists despite the name numbers SCCT
writes after every name reference without counting them in tag sizes:
object properties, tagged structs, arrays of objects, of names and of tagged
structs, state frames, and the faces of a brush's `Polys` (whose material is
how a source map names a BSP texture). Unreadable property lists and faces
that do not fill their export are counted, not misread; truncated or foreign
data is refused. `MapUsagesFileTests.cpp` (in `tools/test_map_package.cmd`,
it needs zlib) reads the compressed `tests/maps/OffsE` maps: a mesh placed 71
times, an editor sprite on 56 lights, a group with a space, a whole package,
a damaged copy and the folder walk with cancel, then the same map grown to
80 MB and written back as one block (stored and compressed), and block
headers that cannot be right. `MapUsagesFileTests.exe
<folder>... <Package.Group.Name> [autosaves]` also prints what the window
would list for real folders and how long the scan took.

`StageModelTests.cpp` covers the zone model: generated Tag names and their
parsing, plan validation (including refusing a plan from the earlier
counting-event version), building the change batch for a fresh map (a mission
per zone holding its objectives unchained with its threshold and briefings,
the map's mission chained over them in order and carrying how many objectives
win the match, completion events, Tags
allocated where missing or shared, timed doors set to stay open, new sound
triggers and announcement alarms), reading the result back into the same plan,
a matching map needing no changes, a match total smaller than the zones' own
being kept, moving an objective between zones,
reordering zones without rebuilding their missions, emptying a zone's event,
reusing a mission and event the author wired by hand, refusing an objective
that is in no zone or a map with no mission, cleaning up the earlier wiring,
and the design-check issues. The native `-WorkflowTools` suite applies a
two-zone plan through `stage.apply`, checks the zone missions' settings and
the top mission's chaining, the completion's action order, that no terminal is
locked and the door's state, reads the plan back, confirms a matching plan is
a no-op, undoes and redoes it in one step each, and opens the Zones window to
check its lists.

`tools/test_map_package.cmd` also covers packaging a map's Map Design workspace and
the reference images it names, skipping missing or unsafe image names, and
tolerating an unreadable workspace file.

## Brush edge grid snap

The focused native `-BrushGridSnapOnly -GenerateFixture` run also checks Brush
Visibility: category selection, hiding and deselection, Undo, isolation, opening
the panel through the native menu dispatcher, Show all and checkbox toggles.
It saves `brush_visibility.bmp` in the disposable test installation.

`tools/test_workflow_tools.cmd` runs the standalone `BrushGridSnapTests.cpp`
checks for nearest bounds, negative coordinates, ties, per-axis spacing, no-ops
and invalid inputs. The native `-WorkflowTools -GenerateFixture` suite exercises
the brush and BSP-face context menus, axis isolation, preserved dimensions,
Undo/Redo, and rejection of an empty selection.
Use `-BrushGridSnapOnly -GenerateFixture` for the focused native snapping run.
This run also checks builder fitting against the displayed vertices of a brush
rotated on all three axes, and four-corner portal creation, native portal flags,
source vertex selection preservation, menu availability and one-step Undo/Redo.
The portal is one sheet polygon whose world corners are the selected corners,
with the sheet flags on actor and polygon and no rotation or pivot.
A thin sliver of corners disables the menu item and adds nothing, and a failed
property edit rolls back without leaving a Redo step that would replay it.
The model suite checks that the portal sheet's corners are the selected corners
on vertical, horizontal and slanted planes, with unordered corners and repeated
polygon copies, and rejects non-planar, concave, non-four selections and ones
with an edge under one unit or under one square unit of area.
This also checks the vertex right-click popup, selection preservation on cancel,
world-space selected-corner alignment, exact preservation of unselected vertices,
shared polygon corners, and vertex Undo/Redo.

## Favorites window

`tools/test_workflow_tools.cmd` runs `FavoritesWindowModelTests.cpp`: the browsers'
own favourite sections read back as they read them (Count, repeats, blanks) and
written in their format; tag names cleaned for the ini; creating, renaming
(including a case-only rename and a clash), deleting, adding to and removing from
tags as key-by-key writes, with a stale editor's writes keeping another editor's
change; stray tag names ignored; the rows for each type, tag, untagged, package and
search filter (tags and the type are searched, and the English for French names
through the search hook) in every sort order, unloaded favourites last; the
package and tag lists with counts; the packages to load once per type; the
status summary; and where the browser window's own tabs go while the Favorites
tab is there (always in front of it, so it stays last and the stock tabs keep
their indices).

There is no native suite test. It was checked by hand in a disposable editor:
the window lists favourites from all three browsers with texture thumbnails,
Use sets the current material and static mesh, Show in Browser switches each
browser to its Favorites view with the favourite selected, tag and remove changes
reach the ini and the browsers, and a browser's own toggle or another editor's
ini change reaches the open window within a second.

## Undo History

`tools/test_workflow_tools.cmd` runs `UndoHistoryModelTests.cpp`: the rows (one
before the oldest step, then one per step), the current row from the buffer's
UndoCount, which rows are greyed for Redo, the signed number of steps to a row
(clamped at both ends), walking there one Undo or Redo at a time and stopping at
the first step the editor refuses (an operation recording, nothing left), a
thousand-step walk, title clean-up, memory sizes against the buffer's limit, the
status line after a reset, and the reports after a jump. For checkpoints it
checks naming and renaming, the start row, ids following UTransBuffer::Begin
(undone steps dropped, one or two of the oldest trimmed, the whole buffer
replaced), resets, and that prints Begin could not have produced (a changed
survivor, redo left over, two steps added, a reordered buffer) drop every
checkpoint rather than move one onto the wrong step.

Checkpoints were checked by hand in a disposable installation (Select All /
Select None steps): a checkpoint from Edit > Add Undo Checkpoint kept its step
through four more edits and a jump back to it from the jump list ("Undid 4
steps"), one on an undone step went when a new edit replaced the redo steps
while the other stayed, one survived edits and a stock Undo with the panel
closed, Delete removed one, and File > New cleared both with a note. Trimming by
a full 8 MB buffer was not repeated for checkpoints; the model covers it.

There is no native suite test yet. It was checked by hand in a disposable
installation: the UTransBuffer layout read live (vtable, 8 MB MaxMemory, reset
reason), titles matching the buffer after brush, actor, property, RE+ Hide and
move edits, jumps back and forth exported to T3D identical to the same stock
Undo/Redo steps, the list following stock Undo, the memory limit dropping the
oldest of 7,944 steps, MAP NEW clearing it, and refreshes still arriving while
the frame's own posted commands starve WM_TIMER.

## Viewport measure tool

`tools/test_workflow_tools.cmd` runs `MeasureModelTests.cpp`: which axis each
2D view (RendMap 12/13/14) cannot see, grid snapping that spares a surface
normal and that axis, distance, horizontal run, deltas and the readout lines in
units and Map Design's player heights and run times (a climb included), ends
clicked in one 2D view, in two different ones and against a 3D click, rejected
input, the row-vector screen maths against hand-built orthographic and
perspective matrices (projection, the mouse in a 2D plane and the mouse ray
onto a floor), and the Start / To / M chain. There is no native suite check:
the right-click path was verified by hand in a disposable install.

## Viewport keys

`tools/test_workflow_tools.cmd` runs `ViewportKeysModelTests.cpp`: Ctrl+D
duplicates on its first press and swallows the auto-repeats while D is held, and
its character is kept from the stock key handler too; Ctrl+Shift+D, Ctrl+Alt+D,
plain D and Ctrl+W go on to that handler; J toggles Game View only without Ctrl,
Shift or Alt, judged by the press when the viewport saw it (so letting go of
Shift first is still Shift+J) and by the keys held at the key-up when it did
not; a Ctrl+D key-up duplicates only when the viewport did not see the press;
and the Shift+letter keys a Geometric Event cannot use (A, S, R, D, N and J, in
either case) are named with their UnrealEd command while the shipped E, H, P,
L, Z and F stay free. It also runs `GridSizeShortcutTests.cpp`: which viewports
are level viewports, the next grid preset up or down (including between presets
and at either end), and the Ctrl + mouse wheel gesture's notches per viewport.

## Placement tools

`tools/test_workflow_tools.cmd` runs `PlacementModelTests.cpp`: rotator to axes
and back, a turned, scaled mesh's world box (PrePivot before the scale) and a
collision cylinder's, aligning minimum / centre / maximum to the key actor,
distributing along an axis and on the line between the first and last
selected, the drop's rays (the nearest hit holds the box, start-solid rays are
ignored, a sunk box rises, a pawn rests on its collision height), turning to sit
on a floor, ceiling or wall, copies in a line, round a centre (on the
selection's own circle or at a radius, partial arcs, turning or not) and along
a path by count or spacing, the copy limits, preview box edges and the
selection order. For Snap to Grid it checks pivots rounded on X and Y or on
all axes (ties away from zero), box edges by the nearer edge with the minimum
on ties, an unused axis's grid ignored, a zero or missing grid refused, moves a
float cannot hold dropped far from the origin, and yaw rounded to 90, 45 or 15
degrees within one turn; for Push to Wall Along, the four axis directions, the
rays across the face and the distance to the wall ahead. There is no native
suite check yet; before Snap to Grid and Push to Wall Along existed, a scratch
probe drove every command and the copies window in a disposable install on ShipD: drops to
floor, ceiling and wall with Undo, End over a level viewport, alignment and
distribution with Undo, line / radial / path copies of a mesh and of a CSG
brush (Undo and Redo), Tags kept on copies, the RE+ Tools and actor menus, and
the viewport preview.

## Select and hide

`tools/test_workflow_tools.cmd` runs `SelectionModelTests.cpp`: the actor rows
read from the editor's list (the level info, builder brush and viewport cameras
never taken), all of this class and all with this Tag (case folded, several
classes or Tags at once, a None Tag adding nothing) and Invert taking visible
actors only, security cameras included; Hide selected taking hidden actors still
in the selection along so they leave it too; Isolate needing a visible selection
and hiding only the visible rest; Unhide all; and the messages when there is
nothing to act on.

## Map Outliner

`tools/test_workflow_tools.cmd` runs `MapOutlinerModelTests.cpp`: the cells
(the short class name, None shown blank, the location in whole units, Hidden
and Locked as words, a broken location read as 0), Group fields split into
names (trimmed, once each whatever their case, None left out), the search
(every word must appear in some column, Tag, Event, the full class name,
location and the Hidden / Locked words searched, a word never spanning two
columns, None not a value), sorting by each kind of column (names with
numbers in order, blanks last, descending), grouping by class and by group (an
actor in two groups under both, ungrouped actors last, a collapsed group
keeping its header and count, groups with no match left out), what selected
lines stand for (a header for all its rows, each row once), Follow Selection's
lines, hidden rows never selected, Delete keeping the level info and builder
brush and its question, the footer, and 20,000 rows. The `outliner` smoke suite
(tests/smoke/README.md) covers the window in a real editor.

## Copy and paste properties

`tools/test_workflow_tools.cmd` runs `PropertyPasteModelTests.cpp`: which copied
properties start unticked (Location, Rotation, the brush transform, what an
actor is drawn as, the names that wire and group it, an SMagicEvent's Groups,
the editor flags and every reference to an actor, nested ones included) and
which start ticked (DrawScale, Skins, a light's brightness, a class); which are
never offered (a brush's Model, an emitter's emitters, Karma parameters, a
reference of unknown or owned class, even inside a struct); the list's order by
category and name; which values name something in the map they were copied
from (an actor, or a mesh, material or class stored in MyLevel); a target taking
a value only through the same schema in any class (kind, enum choices, struct
fields, reference class; not the category); and the value, dialog and result
texts.

## Animation import options

`tools/test_workflow_tools.cmd` runs `AnimImportOptionsModelTests.cpp`: which of
Merge / Overwrite / Keep Notifies apply together, case-insensitive sequence and
bone names, the skeleton check (bone count, names and order), what happens to each
sequence of the file (added, replaced keeping or discarding notifies, skipped as
already present or repeated in the file), a whole-set replace carrying notifies by
name, and the confirmation, nothing-to-merge and log texts. For the Animation
Browser it checks deleting a sequence (the others keep their order; an index out
of range changes nothing) with its confirmation text, and the rename check:
letters, digits, _ and - only, no clash with another sequence of the set
(ignoring case), and renaming a sequence to its own name allowed.

There is no native suite test yet. It was checked by hand in a disposable
installation by importing generated `.psa` files (SCCT's extended format and a
standard ActorX one) into `MAL_anm.ventilateur_anm` with every option combination,
reading the live `UMeshAnimation` arrays back after each, saving the package to a
scratch folder and reopening it in a fresh editor.

## Property filter and multi-edit

`tools/test_workflow_tools.cmd` runs `PropertyFilterModelTests.cpp`: the filter's
words (blank-separated, case folded, all required), which rows of a Properties
window stay for a filter (a category name keeps its properties, a property keeps
its category and expanded members, `events tag` matches across the path, unnamed
rows follow their parent, paths reset at the next category), where the list goes
under the filter bar without running into the button below it, and when a row of
a selection shows "(multiple values)".

Not covered by the native suite. It was checked by hand in a disposable install:
a light and a static mesh actor selected together, the filter narrowed, widened
and cleared with Esc (categories opened before filtering open again), Tag and
DrawScale set once for both, Undo and Redo restoring and reapplying both actors
in one step, and the filter kept across a change to a single selected actor.

## Lighting Budget

`tools/test_workflow_tools.cmd` includes `LightingBudgetModelTests.cpp`: the
editor's light classification (Off, Unflagged, Static, InGame, Static/InGame,
Dynamic, with its dynamic test on bDynamicLight, LightEffect 22 and the two heat
values), which lights count in game, the stock intersection test including
bAffectOwnZoneOnly, per-zone totals, the worst leaf by the BSP's own leaf
number with duplicate and unknown list entries ignored, the largest overlapping
group as a maximal clique (a chain is not a group; a group straddling zones counts
each zone's own share and is listed under the zone holding most of it), hotspot
order and de-duplication, threshold clamping, the build warning's own limits, an
unbuilt BSP, and the warning text. The fixes from a row: Make static (clears
bApplyToInGameLighting and bDynamicLight, sets bApplyToStaticLighting; lights kept
dynamic by LightEffect 22 or the heat values are left out and counted), Turn off
the weakest N (LightType None, by LightBrightness x LightRadius, ties in list
order, lights that already cost nothing skipped), duplicate and unknown entries,
and the confirmation and result texts.

## Render Budget

`tools/test_workflow_tools.cmd` includes `RenderBudgetModelTests.cpp`: per-zone
totals (actors, those drawn in game, static meshes and their triangles, BSP
polygons by their front zone with invisible and portal surfaces left out, unique
materials and the textures they reach, emitters), hidden meshes not counted,
guideline flags and clamping, hotspot order (zone measures and single heavy meshes
by how far over they are) and texts, the point-in-zone BSP walk, camera axes from
an Unreal rotation, the view frustum, polygon clipping, the portal walk (a door
ahead and the one beyond it are seen, one behind or off to the side is not, the
frustum narrows door by door, a camera standing in a portal sees through it, a
camera outside every zone culls nothing, a cut-short search says so) and the view
totals (actors the engine drew add their zones; BSP counts only for zones seen
through portals), and Meshes by cost (one row per mesh asset with its drawn
copies, the triangles of one and of all, heaviest in total first, then the
heavier copy, then by name; hidden copies and emitters left out; the rows add
up to the zone totals' mesh triangles; a zone's actors only, an actor named
twice counted once, one that is gone skipped).

## Map Check

`tools/test_workflow_tools.cmd` includes `MapCheckModelTests.cpp`: point queries
copied from `UModel::PointRegion` (zone of the last node's side, solid behind CSG
walls, a point on a plane counts as in front, an unbuilt BSP and a broken child
index), segment-through-portal tests in either winding, the collision cylinder
samples, which actors are checked and which are tested against geometry
(triggers and objectives are not), actors in the void, far outside the bounds,
beyond the world limit, stuck in BSP or a static mesh, an origin resting on a
floor, the tolerance and far-distance settings, a portal with a gap (the two
rooms are one zone; the leak path goes round the sheet through the gap, never
through it) and one that fills its opening (the path is marked as through the
sheet), unused and unconnected portals, both faces of a sheet as one row, the
64-zone caveat, ZoneInfos in solid space, zones without a ZoneInfo, a level open
to the outside, the stock entries' severities, and an unbuilt map. Floating
meshes: a static mesh above the surface below it gets a row, and two or more a
row that selects them all; one over a mesh that does not block, inside that
mesh's box (a shelf), is listed apart without the End advice, while soft boxes
beside, above or well below it change nothing; a flat mesh lifted 2 units or
less is left out; the gap text, the gap limits and `FloatingMaxGap=0` turning
the check off; a failed line check stopping the scan with a note; and nothing
without a run or a built BSP. BSP point indices are read unsigned, as the
patched engine reads them.

Not covered by the native suite. It was checked in a disposable install with an
injected probe on ShipD: the clean map, then a player start moved into the void,
one sunk 40 units, a light moved 10,000 units out and a static mesh actor's
bStatic cleared (listed by the stock check, captured without its dialog); then,
reloaded, a second ZoneInfo placed beyond a sealing zone portal and the portal
taken out with Build Geometry and Build BSP (MAP REBUILD, BSP REBUILD): Map Check
listed the two ZoneInfos in one zone with a 2,036-unit leak path drawn in the
viewports, as the stock check's "in the same zone as" entry agreed; and a
ZoneInfo moved into solid space, reported by both. Rows selected and framed their
actors and portal surfaces.
## Light and Shadow map

`tools/test_workflow_tools.cmd` includes `LightShadowModelTests.cpp`: threshold
clamping and banding (NaN counts as dark), lightmap luminance in the stored
B,G,R,A order with bilinear sampling and the 2x display scale, light colour
shares from hue and saturation, the engine's falloff (2 at the light,
2(1 - 3x^2 + 2x^3) at a share x of the reach, the light's angle on a surface)
and its NonIncidence, QuadraticNonIncidence, Cylinder and Sunlight variants,
spotlight cones (inside, at the edge, behind, narrow and closed cones, the
switch), the LE_ESpotShadow frustum widening from NearPlaneRatio to
SpotWidth by SpotHeight at EchelonRange (and the box at 0.99), transforms and
their inverse, segments against boxes and triangles, the bounding volume
hierarchy checked against every box, a static mesh box blocking a light but
not beside or over it, a lamp's small fitting not putting it out while a big
hall around it still blocks, static mesh floors on the top face only (mirrored
too) with their vertex baked lighting, walkable floors in either winding, grid
sampling with no duplicates on shared edges and one patch per stacked storey,
lights adding up to the 255 cap, BSP line of sight (a light behind a solid
slab is blocked; one embedded a little in its fitting is not), the work kept
between samples (an unchanged map reuses every light, a moved light or a mesh
moved within a light's reach is worked out again, new patches are added, a
setting or BSP change starts again, each matching a sample from nothing), the
three brightness sources, summaries, legend and readout text, the drawn
outline per band, the readout lookup, and the patch cap keeping small floors
before vast ones.

The smoke suite `lightshadow` (`tests/smoke/SmokeSuites.inl`) turns the map on
in a disposable editor, checks there are patches and some on static meshes,
moves an in-game light and then a static mesh near it and checks the next
sample keeps the other lights, switches static mesh floors off and on in the
settings window (Apply, the summary and `[LightShadowMap] MeshFloors`), and
saves pictures of the editor and the settings window.

Checked by hand in a disposable install on ShipD and BankD (not covered by the
native suite): the menu toggle and its check mark, sampling and the log line,
patches drawn in the 3D view (depth-tested) and the top view with the legend,
front and side views showing the legend only, the readout under the mouse in
the top view, the settings window's Apply writing `[LightShadowMap]` and
resampling, and Map Design's layer.

## Local BSP lighting matching

`tools/test_workflow_tools.cmd` runs `LocalLightingMatchTests.cpp`. It tests
closest-triangle sampling, degenerate triangles, signed RGB residuals, clipping,
alpha preservation, and retained local variation. The native
`-ReopenOnly -VerifyPreservedLighting -VerifySelectedLighting` suite also invokes
the matching menu, checks cancellation and exact preservation of unselected
BSP/meshes, then saves and reopens the resulting combined bake. These byte checks
do not establish visual fidelity or correct shadows. `-LightingBrush
'Brush3702,Brush3728'` targets named brush faces in that suite instead of its
default single-face fixture. Segment tests cover empty space, solid half-spaces,
an intervening wall, invalid child indices and cyclic BSP data.

Use `-ReopenOnly -MatchLightingAfterLoad -LightingBrush 'Brush3702,Brush3728'`
to test matching immediately after loading a source map, without warming the
collision/lighting state through a preceding rebuild. Native probes also check
all six compressed-collision bounds accessors using the September 13 crash's
exact finite bounds while all eight x87 registers are occupied. Each must return
the correct endpoint pointer and preserve the complete x87 state.

## English for French asset names

`tools/test_workflow_tools.cmd` runs `AssetNameGlossModelTests.cpp`: splitting
names at separators, digits, CamelCase and acronyms with Windows-1252 accents
folded; the dictionary format (sections, comments, a later file winning, `word =`
removing); token-by-token glosses that keep unknown words and numbers, give
English names no gloss, and translate words that are also English (Sale,
Carton, Grand) only beside another French word; French plurals in s and
au/eu/ou + x without turning `vertex` into *green* or `bass` into *low*; glued
words (`boisplanche`, `BetonWall`) split only into known parts with at least one
French one; and the search rule (the whole query against the name as the stock
filter does, or every word against the name or its English). It also loads the
built-in dictionary and checks names from the stock packages, and covers the
names the Sound Browser gives an imported or renamed sound: only letters,
digits, _ and - (accents folded, runs of other characters made one _), a nested
group keeping its dots, a group already loaded kept as typed, and `FitsLookup`,
the engine's limit of 255 characters in all and 63 between dots.

`tools/asset_names/scan_names.py` lists every name in an install's packages and
counts their words; it is how `fr-en.txt` was built and measured (96% of the
French word uses in the stock names are translated). After editing `fr-en.txt`,
run `tools/asset_names/build_dictionary.py` to regenerate
`Reloaded.Editor/AssetNameDictionary.gen.h`.

Not covered by the native suite: the glosses drawn in the browser lists, the
Texture Browser label, caption and Filter hooks, the hover tooltip and the
RE+ Options checkbox were checked by hand in a disposable installation.

## Maps with their own sounds

`tools/test_workflow_tools.cmd` runs `PackageLoadFixModelTests.cpp`: the stock bytes of
`UObject::LoadPackage` that `PackageLoadFix` replaces (as read from `ChaosTheory_Editor.exe`),
its call to `GetPackageLinker` and the found path's `EndLoad`, the jump to the stub and
reading it back, code that is not the stock editor's left alone, and the Sound Browser
refresh's call to `LoadPackage` and the one package it skips (`MyLevel`). That a map whose
`MyLevel` holds a sound opens again, collects garbage, renders, autosaves and plays was
checked in a disposable installation of the editor.

## Map packaging and selective Tag renaming

Run `tools\test_map_package.cmd` from an x86 Visual Studio developer prompt after installing the manifest dependencies. The packaging suite covers compressed and raw SCCT package tables, older name encodings, transitive/cyclic dependencies, missing and ambiguous packages, texture counterparts, byte-identical base comparisons, exclusions, stale files, archive paths, and no-overwrite publication. `MapPackageTests.exe <game-root> <playable-map> [new-zip]` also supports read-only inspection or packaging of real maps.

The workflow model and native suites cover excluded Tag/Event assignments, stale previews, partial rename undo, and the actual popup checkboxes. Native screenshots include `tag_rename_exclusions.bmp` and `map_package_preview.bmp`. The packaging dialog check scans a saved playable map and verifies that its checkbox cannot be cleared.

## Editing workflow tools

The native `-GenerateFixture -WorkflowTools` suite checks mission/objective
context-menu availability and command dispatch, repeated additions preserving
existing links, computer and bomb triggers, paired flag/drop-zone creation,
single-step Undo/Redo, connection discovery, stale/wrong-parent rejection,
and Gameplay Connections button state with Follow Selection enabled/disabled.
It also clicks the native popup with a non-frame owner and the graph node popup
with a different viewport selection, and checks that all created actors are
within 32 units of their parent, including both members of a flag/drop-zone pair.

Map JSON tests cover document validation, nested symbolic references, a native
scene containing light/sound/emitter/trigger/event actors and particle components,
forward references, delayed links, exact placement, stale updates, batch rollback,
Undo/Redo, file dialogs, preview cancellation/application and save/reopen. See
[MapAuthoring.md](../docs/MapAuthoring.md) for the format and fixture artifacts.

SMagicEvent JSON tests cover export/import round trips, field and version
validation using the live schema, protected actor identity/placement, invalid
later actions and missing object references leaving the event unchanged,
stale snapshots, one-step undo/redo, and the actual Save/Open dialogs including
cancellation. `magic_event_export.json` and `event-ui-roundtrip.json` are retained
in the disposable native fixture. See [SMagicEventJson.md](SMagicEventJson.md).

`MapOptimiseModelTests.cpp` (built by `tools\test_map_package.cmd`) reads a
synthetic version 300 package's export and import tables back with full
paths, classes and sizes (also from a saved map of one 72 MB block, as a
large release copy is read back), and covers the per-package report, asset sizes
(an asset and everything inside it), move planning and the stock rename
command, and finding what a saved copy still imports. The native
`-OptimiseAssetsOnly` run places a mesh from EST_STM (in a group, with a
material from EST_TXT), checks the report and the File menu window, writes a
release copy into the map and another with its own asset package, checks the
working map is back on the packs, and loads both copies. With
`-OptimiseExtraMap <map.sdc>` it also moves every pack of a full-size map (a
CoD4 port: 841 assets in about 10 seconds).

`StairSmoothModelTests.cpp` covers staircase recognition (straight, curved and
spiral flights, one brush or a brush per step, either polygon winding, turned
flights, landings, several staircases at once, and shapes that are not
stairs) and checks every ramp is a closed, outward-wound solid. The native
`-StairSmoothOnly` run smooths a Map Design spiral from the actor menu's
command and a one-brush straight flight, with the level's collision hash
present, and checks the BlockingVolume has its own BSP that is solid under
the ramp top and open above, below and outside, that smoothing again replaces
the ramp, Remove deletes it, the frame's Undo and Redo take each in one step,
and a plain brush is offered nothing.

`SoftBodyStepModelTests.cpp` covers the soft-body step cap (RealtimeFix): a new
simulation without render bounds always takes its build's first step, with
nothing banked or with a banked step, while a simulation with bounds steps only
on banked time; the banking itself (time adds up to the 1/120 s minimum, a
stall is capped at 0.05 s); and a body built in a 240 fps realtime viewport,
which gets its first step and then about 120 steps a second. Only the
constructor's exact zero radius counts as a new simulation: a cloth gone NaN
stays under the cap. The save-time step for simulations still without bounds
(maps saved by RE+ before the first step was restored) is native and was
checked in the editor and in Play Level, not by this test.

The native workflow suite checks the static-mesh **Position the builder brush
around this** menu action with signed nonuniform scale, rotation and PrePivot,
then verifies the six box faces, centered placement, multiple-mesh bounds,
preserved selection and viewports, one-step undo/redo, and rejection of empty
or mixed selections. It also checks selected-material assignment and a valid
default texture on all six faces when no material is selected. All brush
and mesh changes occur in the disposable fixture. The brush-fit regression uses
an elongated brush rotated on all three axes, away from the world origin, with
an offset pivot and polygon centre. It checks the actual menu command, preserved
orientation baked into the polygons for native BSP building, local dimensions,
independently calculated world bounds, unchanged
source geometry/selection/viewports, and one-step Undo/Redo. The oracle does not
reuse the production vertex transform. The same shape is then tested with its
rotation baked into the source polygons and a zero Rotation property.

The SCamNetwork suite checks separate networks, linear and circular ordering,
singleton loops, broken references, inconsistent first flags, duplicate/missing
cameras, and rejection of partial-network edits. Native tests verify camera
creation and reciprocal link edits in a single undo step, stale-edit rejection,
the manager menu, rename, next-camera preview and exact viewport restoration,
reordering, detach, DPI layouts, and camera persistence after restarting the editor.
The isolated test directory includes `camera_network_manager.bmp`,
`camera_network_manager_144dpi.bmp`, and `camera_network_saved.json`.

The SMagicEvent workbench adds model validation tests plus native fixtures for
array edits, precise timeline drags, actor/component and brush-volume creation,
mover keys, rollback, undo/redo, and rejection of edits made from outdated snapshots.
The fixture calls the native activation thunk to check delay,
Sequence, Repeat and ValidOn semantics, then saves and reopens a complete event
with two triggers, mover, sound, emitter and damage volume in a second process.
See [SMagicEventSemantics.md](SMagicEventSemantics.md) for the engine behaviour.
Screenshots and `magic_*.json` diagnostics remain in the isolated test directory.

From an **x86 Native Tools Command Prompt for Visual Studio**:

```bat
tools\test_workflow_tools.cmd
```

The model suite covers nested native T3D, quoted reference paths, authored strings,
canonical member names, internal reference/tag remapping, explicit external bindings,
position/rotation composition, in-place updates, version rejection and atomic persistence.
It also covers inline sub-objects: stock particle emitters exported under the map package
and workbench components exported under their actor both place directly in the map package,
components of different actors sharing a name stay distinct, and names and saved text stay
the same across repeated save and place cycles, and a definition saved before sub-objects
were canonicalized (package-level references still spelled `MyLevel.Model5`) places with
those references following the renamed objects and does not require the stock sub-emitters
its capture listed as dependencies. `EmitterLibraryModelTests.cpp` covers the
Emitter Library entry schema and validation, categories, ids and text cleaning, drafts from a
capture (placement lines, class-default Tags, stripped and rejected links, map-local assets),
the user file (save, update, delete, hide and restore built-ins, damaged entries, a damaged
file refusing edits), looping (every built-in loops with the actor's own AutoReset and wait,
no system waits for a trigger, removes itself or plays forever, trigger spawners burst each
round, systems kept off by TriggerDisabled=False and nested blocks are left alone, waits
outside 0-3600 seconds are refused), and the
built-in defaults in `EmitterLibraryDefaults.gen.h` (generated by
`tools/emitter_library/build_defaults.py`): every embedded entry validates, holds emitters only,
spells enums by name, lists exactly the assets it uses, has no placement lines, and places its
sub-emitters under the map package. The native suite saves a workbench emitter from the actor
menu's conditions, places it with a live sub-emitter in one Undo step and saves the placed copy
again without its names growing; it then places every built-in entry, checks its sub-emitters
and that a capture of the placed actors resolves every asset the entry lists, and undoes each
placement. The suite moves an existing `library.json` and `emitter_library.json` aside first.
`EmitterEditModelTests.cpp` covers the Emitter Library edit panel's model: reading, replacing,
adding and removing one property line of one particle system with every other byte (CRLF,
nested blocks, the actor's own lines) kept; struct members; the values the panel shows,
including the class defaults for lines and members the editor's export leaves out; each
field's edit and refusal (whole, capped particle counts, non-negative capped rates, ordered
ranges, uniform size without a height, capped velocities and acceleration, 0-255 colours,
package textures outside the map); colour keys that keep their time and alpha; tint as
`ColorMultiplierRange`; a new texture's dependency replacing the old one's; saving an edited
copy as a new user entry or over the user's own; and an edit of every built-in system
changing only its line.
The graph suite checks separate islands, cyclic/self/parallel links, unresolved targets,
relationship/class filters, bounded neighbourhood expansion, isolated actors, deterministic
layout and a 600-actor fan-out without overlapping nodes. Radial layout checks cover
hub selection independent of actor order and parallel properties, circular fan-out,
and label bounds for nested branches with cyclic cross-links. The native harness
also renders a 16-target event island and verifies hub search and selection.
The native surface-menu checks verify Select Brush availability, single and multiple
source-brush selection after rebuilding, unchanged cameras and face selection, and
rejection of faces without master polygons without disturbing actor selection.
It also checks actor-tag labels and classification of nested EventGroup links in filters
and island grouping. The native fixture imports `SBase.SMagicEvent` with two Groups and
multiple EventGroup entries, verifying incoming links, shared Tags, `None`, and an
unmatched target. `workflow_magic_schema.json` records the native structure layout and
`workflow_magic_edges.json` records the resulting relationships.

Run the native integration suite against a disposable editor copy:

```powershell
./tools/run_native_map_recovery_test.ps1 -EditorDll ./bin/Reloaded.Editor.dll -GenerateFixture -WorkflowTools -TimeoutSeconds 180
```

This uses the existing isolated driver; it never runs the test against the installed
game directory. The report and workflow JSON/BMP artifacts remain in the printed test
directory. The workflow probe verifies native BSP replacement scope, undo/redo and
source-polygon rebuild persistence; cyclic/one-to-many Event/Tag relationships and
instance isolation; brush insertion, undo/redo, rebuild and save/reopen; viewport
restoration and in-place updates; and the actual view/assembly/connection dialogs,
including adding/removing members and selection-based updates without deleting scene actors,
stable pivots and changed future placements. It then starts a second editor process and
checks camera restoration, another in-place view update and placement of the updated
assembly from disk.
The first process also opens the native graph panel, renders whole-level and selected-actor
graphs, searches and focuses an actor, and exercises native double-click navigation.
Graph screenshots are retained as `workflow_graph_*.bmp`.
Tag-rename coverage verifies a shared two-actor group and both ordinary Event and nested
EventGroup dependants, one-step undo/redo of nested arrays, collision/None/stale-preview
rejection, and the graph's rename/confirmation/cancel workflow. Native FName construction
is verified at `0x10fb9610` with `NAME_Add=1`; existing FName spellings are never edited.

Build both solution configurations with `SCCT` cleared. Use **Rebuild** when switching
configurations: the renderer static library currently shares its output path. The new
modules are included unconditionally in the editor project. The native integration
is specific to the supported ChaosTheory editor executable and must be checked again
if native addresses or property layouts change.

Also check these cases manually: static-mesh replacement with material overrides,
game-specific array/structure actor links,
Save As/map switching, GE selections, and external-binding changes during assembly edits.
Use disposable source maps and include stale/deleted instance members and missing packages.

## Mover Workbench

`MoverModelTests.cpp` covers the key list behind RE+ Tools > Mover Workbench: where the
mover stands on a key (BasePos + KeyPos, BaseRot + KeyRot), a place as a key, Add Key
Here (a new last key, the mover not moving, at most 8 keys), Update Key, Delete Key (the
mover staying on its key or the one taking its place, never the last key), Move Up and
Down (the mover following its key), Go to Key, telling a move in the viewport (off its
key, or the stock move that takes the base along at key 0 or rewrites a later key) from
another edit, and measuring a new key against the path at rest after such a move; the
preview's poses (legs of MoveTime, eased or not, a pause capped at 3 s, the way back,
the short way round), ghost outlines carried and turned about the pivot and boxed past
the cap, the plain names of InitialState and the other settings, and the key arrays as
the editor reads and writes them. The editor side runs in the `mover` smoke suite.

## Map recovery

ClarD soft-body regression (2026-09-13): one native simulation step before
recovery reproduced the import error on ESBStripDoorActor1009. Points and springs
matched, but the old settings comparison included timestep fields at 0x12C/0x130
and the zero-to-0.01 damping change made by Reloaded's realtime hook.
The comparison now skips simulation scratch at 0x11C..0x133 and treats that
specific damping fallback as unchanged only when the actor's EnergyFactor is
zero. Other physical settings still require an exact match.

The portable settings test checks scratch changes, default damping, nonzero
authored damping and changes to every compared byte. Since 2026-10-09 a
regenerated body always takes its build step, while a body RE+ saved before
that fix never stepped and still holds the constructor's 25 and 1 at
0x154/0x158, where Update copies the actor's +0x380 and bit 2 of its +0x384.
Those two fields are compared by the actor's values; the test checks a
never-stepped original against its stepped regeneration, and that a change to
either actor value is still caught. For a native reproduction,
add `-TraceSoftBodies -StepCookedSoftBodies` to the ClarD recovery command. This
steps each supported cooked soft body once before capturing its recovery data.
ClarD also exceeded the PC engine's signed 16-bit collision-bound index limit.
On overflow, recovery now sorts its generated structural cells by longest extent
and rebuilds with fewer BSP splits. It leaves polygon coordinates, materials and
non-solid sheet order unchanged. Ordinary rebuilds keep the saved brush order;
maps close to the collision limit start with the same BSP settings.
The fallback produced 32,575 collision slots with a highest start index of
32,564; the final BSP build reduced that to 32,431 slots. Recovery preserved
109 portal outlines and passed 12,206 solid/empty probes.
The native source checks now reject collision indices outside the PC range after
recovery, reopening and rebuilding.
The ClarD run passed recovery after a soft-body simulation step, ordinary
rebuilding, a solid-brush and actor Tag edit, File Save, reopening and export.
A separate fresh-editor run also passed load/rebuild/File Save/reopen without
invoking recovery. Both runs used the Release DLL built on 2026-09-13.

A later user run hit the static-mesh collision-box assertion at `0x10EC27F2`
during lighting. Its retained endpoints were finite and ordered. The compressed
collision decoder now builds bounds through integer comparisons of the float
bits, preserving the coordinates and avoiding x87 register use. Non-finite
endpoints still go through the native constructor and its diagnostics.
The native constructor comparison covers 10,000 endpoint pairs. A full x87 stack
produced incorrect bounds with the stock constructor and exact bounds with the
replacement. Portable tests cover the dump's endpoints, 100,000 finite random
pairs, aliased inputs and non-finite rejection. The original user assertion has
not been reproduced in a full recovery run.
With the replacement, menu-driven ClarD recovery completed its lighting,
save/reopen and retained-data checks. A separate fresh-process test passed
ordinary geometry/BSP/lighting/path rebuilding, File Save, reopening and export.

Use `-RecoveryMenu -VisibleEditor` for the actual recovery menu, file picker and
confirmation dialog in the isolated test. `-FpuBoundsProbe` checks native bounds
with a full x87 stack; combined with `-RecoveryMenu`, it then runs recovery.
With `-RecoveryMenu` the probe also captures the progress window
(`recovery_progress.bmp`) from its own thread, records the Recovery Report's
summary and row count, chooses the first row of each kind through
Select and Frame, and saves `recovery_report.bmp`/`recovery_report_after.bmp`.

`-NoMerge` recovers with File > Merge Recovered Geometry off (face joining only,
the earlier behaviour). `-CancelAtStage <n>` acts as if Cancel were pressed when
`MapRecoveryModel::Stage` n starts (0 Preparing ... 10 Paths) and then checks
that the editor holds a new empty map with no File Save target, that the
source, playable map, recovery folder and asset package are gone, and that a
subtractive cube still builds. Both go through the
`ReloadedRecoverMapToSourceWithOptions` export, whose report lines
(`recovery_report cells=... brushes=... polygons=...` and one line per row) are
recorded.

Geometry merging (2026-10-05): faces-joined cells may grow over neighbours
while they stay inside the original cells (`GrowConvexBrushes`, overlapping
brushes of one CSG kind). Results against the earlier face joining, with the
same solid/empty, lighting, save/reopen and actor checks passing:
OffsD 1,803 cells -> 1,205 brushes / 9,448 polygons before, 810-837 brushes /
8,683-8,797 polygons now; AquaD 302 cells -> 234 / 1,940 before, 215 / 1,792 now.
The editor's BSP builder does not always reproduce overlapping brushes exactly
(OffsD missed 3 of 8,046 samples on the first build); recovery then splits the
grown brushes around the misses back into their joined brushes and builds
again, up to six times. The user's ShipD still fails: before this change in
bevel canonicalization, now at one sample beside a 1-unit sliver under two
slopes 0.22 units apart, which no brush grouping reproduces. BankD fails the
imported-actor check (StaticMeshActor307's StaticMesh) with and without merging.

Sub18 regression (2026-09-13): retained portal surface 1282 has a vertex
0.074219 units from its cooked BSP plane. Recovery accepts the native BSP
splitter's 0.25-unit coplanar band, using normalized plane distance and retaining
the original vertex coordinates. Hallway and living-room ZoneEffect objects are
preserved in the dependency package. ESBPatchActor simulations regenerate from
their authored settings, with native comparisons of topology, pinned anchors,
springs and physical settings after import, rebuild and ordinary reopening.
The point serializer at `0x110D34C0` puts a transient vector at `0x28..0x33`;
its Z component must not be included in the authored-data comparison.
The isolated Sub18 run passed recovery, saving, ordinary reopening and a further
geometry/BSP/lighting/path rebuild: 17 patches, 71 portal outlines, 333 structural
brushes, 1812 retained actors and 3980 solid/empty probes. The portable recovery
suites also pass, including patch class/setting mismatch rejection and the new
ZoneEffect classes. Visual and gameplay checks were not part of this run.

EDE64 regression (2026-09-13): retained portal 121 faces opposite its cooked
surface, and some portal surfaces lack PF_NotSolid. Recovery preserves authored
portal winding while validating the undirected plane and reconstructing solid
space independently. Arena and Cave audio effects are preserved as dependencies.
The polygon-import suite includes the actual EDE64 edge from approximately
`-5.7e-14` to `384`: unchanged endpoints lie on the edge without needing a rounded
subtraction, while removed intermediate points still require exact collinearity.
The isolated run passed recovery, ordinary save/reopen and subsequent rebuild:
259 structural brushes, 1403 actors, nine strip doors, 80 portal outlines and
4338 solid/empty samples. It preserved 995 original mesh colour streams. Lift
SLift14593 has 72 cooked colours versus 168 rebuilt render vertices, so recovery
retains its newly calculated colours and verifies them after reopening. A mesh
asset identity change, missing instance or invalid stream still stops recovery.

From an **x86 Native Tools Command Prompt for Visual Studio**, run:

```bat
tools\test_map_recovery.cmd
```

The asset-package suite links zlib from `vcpkg_installed`: like
`tools\test_map_package.cmd`, the script looks in `vcpkg_installed\x86-windows-static`
and falls back to the doubly nested triplet folder a fresh install makes.

The portable suites verify structural BSP classification, closed brush geometry,
texture-region partitioning, native polygon vertex limits, actor and LevelInfo
transfer, GE gameplay data, and lossless asset-package decompression. They include
malformed input, numeric edge cases, deep trees, work limits and no-overwrite cases.
The geometry tests compare independently classified points before and after reconstruction.
The leaf-light table suite checks signed-index overflow, exact ordered lists,
shared suffixes, empty lists, capacity rejection and randomized roundtrips.
Surface fixtures also cover concave and non-planar non-solid sheets: triangulation
retains their original 3D float vertices and supplies each triangle's normal.
Native import applies additional consecutive-vertex and small-area cleanup.
The polygon-import suite reproduces those rules, including their float arithmetic,
and checks exact outline preservation. Recovery checks every exported polygon before import, since native cleanup
can otherwise leave a gap in a brush. Surface tests cover exact convex
coalescing, material divisions at native import and BSP split precision, and removal of collapsed bevels
while keeping the remaining planes and checking that each brush is closed. Brush ordering
tests preserve coordinates and reject mixed CSG operations. Also rebuild and inspect the map in the editor, especially around thin geometry.

The [native editor integration test](#native-editor-integration) also exercises
conversion, geometry edits, ordinary reopening and builds in a disposable editor.

## Native editor integration

Run recovery tests in a temporary copy of the supported editor. The installed
DLL, configuration and maps are left alone. From PowerShell:

```powershell
./tools/run_native_map_recovery_test.ps1 -EditorDll ./bin/Reloaded.Editor.dll -GenerateFixture
./tools/run_native_map_recovery_test.ps1 -EditorDll ./bin/Reloaded.Editor.dll -SourceMap 'C:/path/to/compiled.sdc'
./tools/run_native_map_recovery_test.ps1 -EditorDll ./bin/Reloaded.Editor.dll -SourceMap 'C:/path/to/compiled.sdc' -EditGeometry
./tools/run_native_map_recovery_test.ps1 -EditorDll ./bin/Reloaded.Editor.dll -SourceMap 'C:/path/to/source.sdc' -ReopenOnly -ExtraAssetPackage 'C:/path/to/source_Assets.usx'
```

The harness requires the Visual Studio x86 C++ tools and uses `SCCT` as the
installed System directory (override with `-GameSystem`). It creates a separate
editor installation in a unique temporary directory, copies configuration and
maps, and shares only asset directories that the test does not write. Its
StaticMeshes directory is a complete copy because recovery may create a new
dependency package there. The generated fixture is an ordinary room brush that
the native editor builds and compiles before attempting recovery.
`-StartupConfigSystem` can copy startup INI files from a previously successful
isolated test while retaining the installed assets. Startup critical errors are
recorded separately from recovery failures.

An injected probe invokes the recovery API on the editor's UI thread, then uses
ordinary map loading and geometry/BSP/lighting/path rebuilding. It records
actor, brush, polygon and BSP counts and exports a final T3D. `PASS` requires
geometry to survive recovery, ordinary reopen, and another normal rebuild, and
the normal File Save target to identify the recovered source. The generated
fixture additionally adds a solid pillar to the recovered room, rebuilds it,
then saves and reopens it while checking that the edited BSP survives.
`-EditGeometry` also exercises a real geometry edit on an existing compiled
map: it finds empty BSP space, adds a small solid brush, rebuilds and saves,
then verifies the changed space and surface count after ordinary reopening.
It also changes a retained static mesh actor's Tag through native property
import and checks that the property survives the same save/reopen cycle.
`-GenerateFixture -RootOutside` exercises the complementary solid cube in an
empty world and edits a cavity into it, preserving `UModel::RootOutside=1`.
`-ReopenOnly` starts a fresh editor and exercises ordinary source loading,
rebuilding, saving and export without invoking the recovery API. Optional
`-ExtraAssetPackage` paths copy generated dependencies into that isolated
editor. `-GenerateFixture -ImportBaseline` isolates the native T3D import,
platform actor synchronization, rebuild and source/runtime save path.
`-GenerateFixture -ExpectRecoveryFailure` blocks geometry interchange writing
after cooked loading with an owned directory. It checks that the rejected
conversion clears the previous normal File Save target, then loads the original
source and verifies that a new geometry edit rebuilds normally.
`-ImportTextOnly -SourceMap <fixture.t3d>` isolates ordinary native T3D import
and export without recovery, rebuilding or saving. It writes
`System/import_only.t3d`; compare that output with the fixture to check its properties. This mode confirmed that
spaced object paths require nested quotes, for example
`StaticMesh=StaticMesh'"Oilrig_SM.Third Floor.topcatwalk"'`, and polygon material
paths require double quotes, for example
`Begin Polygon Texture="Oilrig_TXT.Top floor.yellowmetal"`.
Adding `-BuildImportedText` runs ordinary geometry and BSP rebuilding before
export and writes `System/NativeImportedBsp.json` for spatial comparisons.
The probe also writes cooked and failed BSP trees as JSON for spatial diagnosis.
`-InspectCookedOnly` loads and exports a compiled file, records its native BSP
arrays and player-start occupancy, then stops before reconstruction.
`-TraceSoftBodies` additionally records native soft-body fields and strip-door
point/spring arrays for comparing procedural regeneration with cooked data.
`-TraceLeafLights` writes the native leaf indices and light tables after builds
and reopening to diagnose visibility-table limits.
`-TraceLighting` records reflected light settings, loaded light-actor membership,
mesh leaf-light candidates, and each mesh instance's baked BGRA colour stream at
cooked/imported/built/reopened stages in `System/lighting_*`. The trace includes the colour bytes. `-InspectCookedOnly -TraceLighting
-RelightCookedMeshes` additionally refreshes mesh render data and runs native
mesh shadow-mask generation and colour baking against the original BSP in the
disposable editor. This diagnostic skips the full lighting command, BSP rebuild and map save.
Lighting traces also include per-light shadow-mask visibility and before/after
snapshots around an ordinary full rebuild. Add `-AllLeafLightCandidates` to the
mesh-only diagnostic to test candidate gathering independently of spatial leaf
membership; add `-SkipMeshShadowOcclusion` to isolate shadow rejection. Both
experiments restore temporary actor state and do not save a map.
The current OffsD findings and remaining limitations are recorded in
[MapRecoveryLightingStatus.md](MapRecoveryLightingStatus.md).
`-ReopenOnly -VerifyPreservedLighting` checks a recovered source with nontrivial
lighting: mesh bytes must survive an ordinary build, mesh/BSP bytes must survive
File Save/reopen, cancelling Recalculate Lighting must change nothing, accepting
it must replace the bake, and later ordinary lighting commands must preserve the
replacement. `-TraceLighting` also dumps BSP atlas bytes before/after the build
and after saving/reopening for independent comparison.
Add `-GeometryOnlyBuild` to the reopen test to issue only MAP REBUILD in the
native build bracket. This covers preservation's internal BSP finalization and
lighting transfer without a subsequent explicit BSP build masking a sequencing
bug. Use an edited recovered source for the stale visibility-index regression.
`-ReopenOnly -VerifyPlayMapSave` exercises the native Play Level save call and
its temporary runtime package repair without launching the game. Test a map
larger than the 15 MB uncompressed writer buffer and a small map. Inspect the
resulting `Packages/Maps/Autoplay.sdc` with `-InspectCookedOnly` to verify that
the native compiled-map reader can load it.
Ordinary recovery verifies strip-door topology, rest lengths, fixed anchors,
physical settings and all exported authored actor properties at import, build
and reopening. Decorative sheet tests check native polygon acceptance and
exact boundary-edge preservation across alternative triangulations.
Actor tests also cover stale Xbox labels on confirmed PC runtime actors,
retained particle components and references, and exclusion without native PC
membership evidence. Recovery verifies rebuilt mesh-lighting bindings after
normal saving/reopening. The native rebuild probe includes the normal Build
UI's platform-lighting cache finalizer before subsequent saves.
`-CompactPoints` is a separate native compaction experiment that wraps completed
CSG operation boundaries; it is excluded from ordinary source recovery checks.
The first trial reclaimed unused points but later encountered a stale point
reference, so it is not ready for normal builds.
Reports, source/runtime maps, interchange files and crash dumps remain in the
printed `TestRoot` for inspection. The disposable editor is stopped on finish
or timeout. The harness tests map editing and saving. Test gameplay separately.

## Play Level resolution regressions

For a selective-lighting regression, add `-VerifySelectedLighting` to a native
`-ReopenOnly -VerifyPreservedLighting` run with a recovered map containing lit
static meshes. It checks cancellation, a changed selected mesh, exact unchanged
mesh and BSP chart texels, selected BSP recalculation, and preservation of the
combined bake through File Save/reopen. Atlas packing may change; chart texels
are compared independently of their atlas locations.

After adding or subtracting geometry, build geometry, select affected BSP faces
or their source brushes and static meshes, then use **Build > Recalculate
Selected Lighting...**. Include surfaces where shadows should appear or disappear.
Selecting a light alone does not select its receivers. The selected areas use
the native baker and may differ from the original bake. Unselected chart texels
are copied without resampling; incompatible chart identities/dimensions fail
with a message to reopen the saved map. Shared charts require selecting their
adjoining faces too.

Reloaded's `labs_borderless_fullscreen` setting in `SCCT_Versus.config` controls
borderless presentation for both play commands and ordinary game launches.

`tools/test_workflow_tools.cmd` runs `PlayLevelConfigTests.cpp` and
`PlayLevelCommandTests.cpp`. They cover resolution selection, preserving source
INI bytes and unrelated settings, configuration failure paths, and recognizing
explicit INI overrides.
Play Level clones `Default.ini` to `Reloaded_PlayLevel.ini` and sets viewport
and menu dimensions before launching. It defaults to the editor monitor's
current display mode; `[PlayLevel] ResolutionX/ResolutionY` in
`Reloaded_Editor.ini` can override both dimensions. An explicit launch `INI=`
is respected. The legacy executable-token repair remains separate and active.
