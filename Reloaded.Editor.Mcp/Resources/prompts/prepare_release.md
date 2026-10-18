---
name: prepare_release
title: Prepare the map for release
description: Take a finished map to a shareable release, covering naming, Map Settings, interface textures, the .uas, package checks and the archive file list, as the wiki's Map Settings, Sound Browser and Map Packaging pages require.
arguments:
  - name: mapName
    description: The map's display name (CAPS), as players will see it.
    required: true
  - name: images
    description: Image files for the interface textures, as "menu=..., briefing=..., loadingAtt=..., loadingDef=..." (any subset).
  - name: briefing
    description: Briefing and legend text, or "write it" to have you draft it from the map.
    default: write it
wiki: [map-settings, sound-browser, map-packaging, level-properties]
rules: true
---
# Prepare {{mapName}} for release

{{#images}}**Interface images:** {{images}}
{{/images}}**Briefing text:** {{briefing}}

## Recipe

1. **Gate.** Call `map_validate` with `profile:"release"`.
   - Fix every error first; the `fix_validation` prompt covers that.
   - Release needs:
     - 3 or more starts per team;
     - all three modes;
     - a CamNet loop;
     - no unreachable objectives;
     - built geometry and lighting (`map_build` with `what:"all"`).
2. **File name.** Call `map_file` with `action:"save_as"` and `mapName:"{{mapName}}"`.
   - The file is the first 4 letters + `D` (Aquarius → `AquaD`), in `Packages\MapsEd`, ASCII, with no spaces.
   - If the map already has its file, call `map_file` with `action:"save"` instead.
3. **Restart.** Call `editor_restart`. A new map needs save → exit → reopen before Map Settings can be opened.
4. **Map Settings.** Call `map_settings` with these arguments (the SMapSettings property each one writes is in brackets):
   - `mapName:"{{mapName}}"` in CAPS (`MapName`);
   - `briefSpy` / `briefMerc`, shared by all modes (`ExpliMapInBriefSpy` / `ExpliMapInBriefSentry`);
   - `legend`: date, time, location and a one-line story (`LegendMap`), and `legendInBrief`: the numbered minimap zones (`LegendMapInBrief`), with `|` for line breaks;
   - `modes`, one entry per mode, which the tool writes as index-aligned `Modes[i]` + `ModesStrings[i]`:
     - `mode` `story` / `diskhunt` / `deathmatch` (`GameModeId` `GM_Multi` / `GM_DiskHunt` / `GM_DeathMatch`);
     - `respawn` 5-15 (`RespawnTime`), `spiesLives` / `sentriesLives` 0-10, 0 = infinite (`SpiesLives` / `SentriesLives`), `timeLimit` 0-20 minutes (`TimeLimit`);
     - for Disk Hunt, `scoreLimit`, `handicap` and `numDisksMax` (`ScoreLimit`, `Handicap`, `NumDisksMax`);
     - `strings:{explicationSpy, explicationMerc, briefSpy, briefMerc}` (`ExplicationModeSpy` / `ExplicationModeSentry`, `ExpliModeInBriefSpy` / `ExpliModeInBriefSentry`);
     - `strings.legendSpy` / `legendMerc` (`LegendModeInBrief*`) default to the Ubisoft standard text from the embedded page.

   {{#briefing}}Briefing and legend source: {{briefing}}.{{/briefing}}
5. **Interface textures.** These go in package `<File>-i`, with no group.
   - Prepare images with `image_convert` (power of two; `slot` sets the size and alpha for each):
     - `Menu` 256 × 256, alpha with the Menu fade mask;
     - `Briefing` 256 × 256, alpha;
     - `LoadingATT0` 512 × 512;
     - `LoadingDEF0` 512 × 512.
   - Import them with `map_settings` and `interface:{menu, briefing, loadingAtt, loadingDef}` (image paths):
     - `Menu` and `Briefing` as DXT5 with `bAlphaTexture=True`;
     - `LoadingATT0` and `LoadingDEF0` as DXT1.
   - Saving produces `Packages\Textures\<File>-i.utc`. Delete any `-i.utx` it leaves; the game does not use it.
{{^images}}   - No images were given. Ask the user for them, or render placeholders with `view_screenshot` (`maxEdge` 512) and convert those.
{{/images}}
6. **Minimap (optional).** Use `map_settings` with `minimap` (`true`, or `{floors, framing, ...}`), which fills `MapSnapshotInfo` (`MapFloors` per floor with `FloorZ`, plus `SnapshotCamera`) for the F7 Strategic Map.
7. **Sound.** Call `release` with `action:"sounds"` to build `Sounds\<File>.uas`.
   - Always include `Interface` (objective sounds, win/lose music), plus `Amb_<Map>` if the map has its own ambient package.
   - Quality is 5.0. Build it even when the map has no streamed audio.
8. **Packages.** Call `release` with `action:"check"`:
   - every new package has a unique base name: `<Map>_TXT.utx`, `<Map>_STM.usx`, `Amb_<Map>.uax`;
   - no stock package or map was modified;
   - each new package is saved.
9. **Final save.** Call `map_file` with `action:"save"`, then `map_validate` with `profile:"release"` (expect no errors).
10. **Archive.** Call `release` with `action:"package"` (or list the files for the user):
    - `Packages\Maps\<File>.sdc`
    - `Packages\MapsEd\<File>.sdc`: keep it backed up, because a Maps file cannot be turned back into a MapsEd file.
    - `Packages\Textures\<File>-i.utc`
    - `Packages\Sounds\<File>.uas`
    - the map's own `_TXT` / `_STM` / `Amb_` packages.
11. **Report.** List the files, the Map Settings written, and the remaining warnings. Remind the user to playtest every mode in game.
