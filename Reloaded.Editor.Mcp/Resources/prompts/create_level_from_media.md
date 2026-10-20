---
name: create_level_from_media
title: Create a level from video or images
description: Rebuild a real or drawn place as an SCCT Versus map from a video, screenshots, photos or a floor-plan image. You read the media; the server extracts frames, overlays measurement grids, compiles and builds.
arguments:
  - name: media
    description: Path(s) to the video or image files (comma-separated), or a note that they are attached to the conversation.
    required: true
  - name: mapName
    description: The map's display name (CAPS). The file becomes its first 4 letters + D.
  - name: modes
    description: Game modes the map must play.
    default: story, diskhunt, deathmatch
  - name: knownSize
    description: Any real measurement you know from the media, such as "the hall is 20 m long" or "the doors are standard".
  - name: notes
    description: What to keep, simplify or invent (gameplay, missing floors, areas off camera).
wiki: [index, spawn-points, game-modes, level-ingredients, geometric-events]
docs: [tools]
rules: false
---
# Create an SCCT Versus level from media

**Media:** {{media}}
**Modes:** {{modes}}
{{#mapName}}**Map name:** {{mapName}}
{{/mapName}}{{#knownSize}}**Known measurement:** {{knownSize}}
{{/knownSize}}{{#notes}}**Notes:** {{notes}}
{{/notes}}
## Who does what

**The server does not interpret media. You do.** The server only gives you what you need to look and measure:
- `media_extract_frames` turns a video into still PNG frames, optionally with a contact sheet.
- `media_read_image` resizes or crops an image, and can overlay a labelled measurement grid. It returns the image to you.
- `image_convert` converts images for import.
- `plan_render` and `view_screenshot` let you compare your result with the source.

Every judgement is yours: what a room is, where walls and doors are, how big things are. Record these judgements in the spec's `provenance`.

The rule catalogue is not embedded, to keep this recipe short: read `scct://rules` (every check with its draft and release severity and its wiki page) before you design or fix anything.

## Recipe

0. **Is it already an SCCT Versus map?** If the footage shows one of the game's own maps (or a custom Versus map the user has), do not rebuild it from frames: use the game's file. A source map in `Packages\MapsEd` opens with `map_file` `action:"open"`; a compiled one in `Packages\Maps` becomes editable with `map_recover` (the `recover_map` prompt walks through it). Use the frames only to check the result.

1. **Connect.** Call `editor_status`; use `editor_launch` if no editor is running. If the open map has unsaved work, ask the user before `map_file` with `action:"new"`.

2. **Get frames (video).** Call `media_extract_frames` on the video.
   - Start with `mode:"scene"` (a new frame on each scene change) and `sheet:true` (a contact sheet), so you see the whole clip at a glance.
   - Then use `mode:"interval"` with `intervalS` (for example `intervalS:1`, one frame per second) and `start` / `end` over the stretches that show the layout: walk-throughs, pans, overhead shots.
   - Keep the frame numbers and timestamps; you will cite them in `provenance.frames`.
   - For still images, skip this step.

3. **Look at every relevant frame.**
   - Use `media_read_image` with `path` (a frame) to view it at a readable size (`maxEdge`), and with `crop` to look at details such as doors, stairs, vents and ladders.
   - For each frame, write a short note: which space it shows, from where, looking which way, and what connects to what. Build a room graph: spaces as nodes, doorways and stairs as edges.

4. **Read a grid.**
   - For a floor plan or a top-down view: call `media_read_image` with `grid` (a measurement grid overlay: `{spacingPx}`, or `{cellUu, uuPerPx}` once the scale is known), and read each wall line and opening against the labelled cells.
   - For perspective frames: overlay a grid and count known repeating elements, such as floor tiles, ceiling panels, windows or doors, to estimate lengths along each wall.
   - Note the cell size in pixels and what it maps to.

5. **Calibrate the scale.** Convert to Unreal units with known sizes:
   - **A standing player is 180 uu tall** (and 96 wide). A person in the frame is about 180 uu.
   - About **100 uu per metre** (1 cm = 1 uu). Real buildings convert directly; game spaces are usually a little larger.
   - **Doors are about 96 × 128 uu.** Use 128 wide × 256 high for comfortable Versus openings.
   - **Stair risers ≤ 35 uu** (the pawn's step-up height). The blockout generator uses ≤ 24 rise and ~32 run per tread.
   - Vents are 96 × 125 (crouch); crawlways are 105 high.
   - Typical rooms are 256-320 high; corridors are 192 or more wide.
   {{#knownSize}}- Calibrate against the user's measurement first: {{knownSize}}.
   {{/knownSize}}- Cross-check two independent references, such as a door and a person. Where they disagree, prefer the larger playable size.
   - Round to the editor grid: 16 or 32 uu.

6. **Write the spec** (`scct.level-spec` v1, `scct://schema/level-spec`).
   - `geometry.pieces`: one piece per space from your room graph. Use `attach` / `doorways` for connections, and `storeys` plus `vertical` for floors, stairs and lifts.
   - **Gameplay is yours to design**; the media rarely shows it. Follow the embedded wiki:
     - Starts: 4 per team, far apart.
     - Story objectives: a terminal at floor+78 with `DominationCaptureTime`, flags with 1-5 drop zones, bombs.
     - Disk Hunt disks, and a Deathmatch reset.
     - A CamNet loop with exactly one first camera.
     - `SAmmoStock` and `SInfirmary`.
     - Alarms.
     - Zones with `locationName` (spawn rooms included).
     - `ges` for ledges, pipes and ladders you saw.
   - `provenance`:
     - `source`: the media paths;
     - `frames`: `[{frame or time, shows, notes}]`;
     - `notes`: your scale calibration (which references, what factor) and every place you guessed or simplified.
{{#notes}}   - Apply the user's notes: {{notes}}
{{/notes}}
7. **Compare offline.**
   - Call `level_spec_validate`, and fix every error at its JSON pointer.
   - Call `plan_render` with the spec. Compare the PNG with the floor plan, or your sketch from the frames: proportions, connections, door positions.
   - Iterate the spec until it matches.

8. **Build.**
   - Call `level_build` with `mode:"preview"`, then `mode:"apply"` with `build:"geometry"`.
   - Call `view_screenshot` with a `camera` (`{location:[x,y,z], rotation:[pitch,yaw,roll]}`, the rotation in engine units, 65536 per turn, not the spec's degrees) placed where a key source frame was taken. Compare the two images, adjust the spec, and run `level_build` with `sync:"incremental"`.

9. **Validate.** Call `map_validate` with `profile:"draft"`, and fix the findings (each has a `fix`). Check climbing GEs against the measurements in the embedded geometric-events page.

10. **Save and finish.**
    - Call `map_file` with `action:"save_as"`{{#mapName}} and `mapName:"{{mapName}}"`{{/mapName}}, then `editor_restart`, then `map_settings` with the spec's `settings`.
    - Then `map_validate` with `profile:"release"`.

11. **Report.** Tell the user:
    - the scale used;
    - which frames informed which spaces;
    - what you invented for gameplay;
    - what differs from the source, and why.
