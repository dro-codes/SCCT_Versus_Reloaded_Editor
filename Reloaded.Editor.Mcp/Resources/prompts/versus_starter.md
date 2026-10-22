---
name: versus_starter
title: Versus starter map
description: Build the RE+ Versus starter layout with the wiki's gaps fixed (4 starts a team, terminal at floor+78, a 3-camera CamNet, zones, all three modes), ready to reshape.
arguments:
  - name: mapName
    description: The map's display name (CAPS). The file becomes its first 4 letters + D.
    default: VERSUS STARTER
  - name: roomSize
    description: Objective room size in Unreal units, "width x length x height".
    default: 1024 x 1024 x 320
  - name: corridorLength
    description: Length of each corridor between a spawn room and the objective room, in Unreal units.
    default: 768
  - name: extras
    description: Anything to add on top, such as "a second floor", "a bomb objective" or "more security".
wiki: [index, spawn-points, game-modes, level-ingredients, alarms, map-settings]
docs: [map-design]
rules: false
---
# Versus starter map: {{mapName}}

**The RE+ panel's Versus starter layout breaks wiki rules:**
- 2 starts per team, where the wiki asks for at least 3;
- the terminal at floor+64, where the wiki asks for about 78;
- no `SCamNetwork`, which softlocks Mercenaries.

Reuse **only its geometry**. Build the gameplay from `scct://examples/versus-starter`, which fixes all three:
- 4 starts per team;
- the terminal at floor+78 with `DominationCaptureTime`;
- a 3-camera CamNet loop with exactly one `bFirstCam`;
- a laser feeding a full alarm;
- ammo and medkits;
- zones with `LocationName`;
- Story, Disk Hunt and Deathmatch missions;
- Map Settings with one `Modes`/`ModesStrings` entry per mode.

Parameters:
- **Objective room:** {{roomSize}}
- **Corridor length:** {{corridorLength}}
{{#extras}}- **Extras:** {{extras}}
{{/extras}}
The rule catalogue is not embedded, to keep this recipe short: read `scct://rules` (every check with its draft and release severity and its wiki page) before you design or fix anything.

## Recipe

1. **Connect.** Call `editor_status`. If the open map has unsaved work, ask the user, then call `map_file` with `action:"new"`.
2. **Load the example.** Read `scct://examples/versus-starter`.
   - Set `map.name` and `settings.mapName` to "{{mapName}}".
   - Set `map.file` to its first 4 letters + `D`.
3. **Resize.** Set `ObjectiveRoom.size` to {{roomSize}}, and both corridors' length to {{corridorLength}}.
   - Spawn rooms keep `attach` to their corridors, or move their `at` so they stay attached.
   - Keep the vent route (96 × 125, crouch) from the spy spawn into the objective room.
4. **Extras.** {{#extras}}Add {{extras}} as spec sections, following the embedded wiki pages:
   - extra objectives in `gameplay.story.objectives`;
   - more security in `security`;
   - more floors in `storeys` plus `vertical`.
{{/extras}}{{^extras}}None were asked for; keep the example's gameplay as it is.
{{/extras}}5. **Check offline.** Call `level_spec_validate` (expect zero release errors), then `plan_render`. Check that:
   - the spy starts are more than 512 uu from the terminal;
   - the teams are more than 768 uu apart;
   - the terminal is within 4096 uu of the merc starts.
6. **Build.**
   - Call `level_build` with `mode:"preview"`, then `mode:"apply"` with `build:"geometry"` and `screenshot:true`.
   - Call `view_screenshot` with `preset` `overview`, `spawn:spy`, `spawn:merc` and `objective:ServerObjective`.
7. **Validate.** Call `map_validate` with `profile:"draft"`, and fix anything it reports.
8. **Save and restart.** Call `map_file` with `action:"save_as"` and `mapName:"{{mapName}}"`, then `editor_restart`, then `map_settings` with `settings` (the spec's `settings` section). Then call `map_file` with `action:"save"`.
9. **Finish.** Call `map_validate` with `profile:"release"`. Only the interface textures and the `.uas` should remain; the `prepare_release` prompt covers them.
10. **Hand over.** Tell the user the pieces are Map Design pieces: they can open **RE+ Tools > Map Design** to drag, resize and extend them.
