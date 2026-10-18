---
name: create_level_from_description
title: Create a level from a description
description: Design and build a complete, wiki-correct SCCT Versus map in the running editor from a written description, through a scct.level-spec.
arguments:
  - name: description
    description: What the level should be (setting, layout, size, objectives, mood). Free text.
    required: true
  - name: mapName
    description: The map's display name (CAPS, as in Map Settings). The file becomes its first 4 letters + D.
  - name: modes
    description: Game modes the map must play.
    default: story, diskhunt, deathmatch
  - name: profile
    description: Validation profile to finish at (draft or release).
    default: release
wiki: [index, spawn-points, game-modes, level-ingredients, alarms, zones-and-volumes, map-settings]
docs: [tools]
rules: false
---
# Create an SCCT Versus level from a description

You are building a playable **SCCT Versus** map (Spies vs Mercenaries) in the RE+ editor through the `scct-reloaded` MCP server.

**The description:**

> {{description}}

{{#mapName}}**Map name:** {{mapName}}. Use CAPS for Map Settings; the file is its first 4 letters + `D`.
{{/mapName}}{{^mapName}}**Map name:** choose a short evocative name in CAPS from the description. The file is its first 4 letters + `D` (Aquarius → `AquaD`).
{{/mapName}}**Modes:** {{modes}}.
**Finish at profile:** {{profile}}.

The SCCT Versus wiki digests and the rule catalogue are embedded below. They are the source of truth. Use class and property names exactly as written there.

The rule catalogue is not embedded, to keep this recipe short: read `scct://rules` (every check with its draft and release severity and its wiki page) before you design or fix anything.

## Recipe

1. **Connect.** Call `editor_status`.
   - With no editor, call `editor_launch`, or ask the user to start RE+ and enable **RE+ Tools > MCP > Enable MCP server**.
   - Note whether approval is on. If a change times out with `awaiting_approval`, call `approval_wait` with its ticket.
2. **Start a map.** If the open map has unsaved work, ask the user before replacing it. Then call `map_file` with `action:"new"`.
3. **Design on paper first**, in Unreal units (about 100 uu per metre, Z up, +X east, +Y north):
   - Spaces: storeys, rooms, corridors, vents (96 × 125 crouch), stairs and doorways.
     - A standing player is 180 tall and 96 wide; doors are about 96 × 128.
     - Typical rooms are 512-1024 across and 256-320 high; corridors are 192 wide.
   - Spawns: a spy spawn area and a merc spawn area, far apart.
     - Spy starts more than 512 uu from any objective, and the teams more than 768 uu apart.
     - Every objective within 4096 uu of a merc start.
   - Objectives: 1-3 Story objectives, each with its triggers (terminal / flag + drop zones / bomb).
     - Plan Disk Hunt disk areas.
     - Plan the Deathmatch reset that turns security off.
   - Mercenary support: a CamNet of 3 or more cameras covering the objectives and choke points, an ammo box (`SAmmoStock`) and medkits (`SInfirmary`).
   - Security: lasers, cameras and motion sensors feeding `SAlarm`s near the objectives.
   - Zones: name every area with a `ZoneInfo` `LocationName`, spawn rooms included.
4. **Write the spec** (`scct.level-spec` v1, `scct://schema/level-spec`).
   - Start from `scct://examples/versus-starter` or `scct://examples/three-modes-sectors` and adapt them.
   - Use `attach` and room-relative positions (`{in, u, v, h:"floor+N"}`) rather than raw coordinates, wherever you can.
   - Put the description in `provenance.source`.
   - Section by section:
     - `map`, `profile: "{{profile}}"`, `defaults` (Carve, thickness 16, lights "rooms", startsPerTeam 4)
     - `storeys`, `geometry.pieces` / `doorways` / `vertical`, `zones`
     - `spawns.spy` / `spawns.merc` (count ≥ 3, RE+ uses 4)
     - `gameplay.modes` and `gameplay.story` / `diskhunt` / `deathmatch`
     - `security.alarms` / `devices` / `camnet`
     - `mechanisms` (doors, lifts, buttons) where the design needs them
     - `environment.lights` / `ingredients` / `sounds`
     - `ges` for climbing routes
     - `settings` (`mapName`, briefs, legend with `|` line breaks, one `modes[]` entry per mode)
5. **Check offline.**
   - Call `level_spec_validate` with the spec, and fix every error at the JSON pointer it names.
   - Call `plan_render` with the spec and look at the PNG. Is the layout what the description asks for? Are both teams' routes to each objective sensible?
6. **Preview.** Call `level_build` with `mode:"preview"` and read the preview rows and the plan image.
7. **Apply.** Call `level_build` with `mode:"apply"` and `build:"geometry"`. It is one Undo step; `editor_undo` reverts it.
8. **Look.** Call `view_screenshot` with `preset` `overview`, `spawn:spy`, `spawn:merc` and `objective:<id>`.
9. **Validate.** Call `map_validate` with `profile:"draft"`.
   - Fix the findings. Prefer editing the spec and calling `level_build` with `sync:"incremental"`; each finding's `fix` names a tool call.
   - Repeat until no errors remain.
10. **Save.** Call `map_file` with `action:"save_as"` and `mapName` (the map name), then `editor_restart`. The wiki requires save → exit → reopen before Map Settings works.
11. **Map Settings.** Call `map_settings` with `settings` (the spec's `settings` section), and `interface` (image paths by slot: `menu`, `briefing`, `loadingAtt`, `loadingDef`) if the user supplied images. Then `map_file` with `action:"save"`.
12. **Finish.** Call `map_validate` with `profile:"{{profile}}"`. For release, continue with the `prepare_release` prompt's steps (the `release` tool).
13. **Report.** Give the user:
    - the file name;
    - what was built: rooms, starts per team, objectives per mode, cameras, alarms;
    - the remaining warnings;
    - what to playtest in game, such as terminal capture, CamNet access and disk pickup.

Never write stock packages. Never discard unsaved work unasked. Keep every change undoable through `editor_undo`.
