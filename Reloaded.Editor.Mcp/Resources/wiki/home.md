# SCCT Versus Reloaded Editor wiki

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Home

This wiki documents the Unreal Editor used by Splinter Cell: Chaos Theory Versus. It covers how it differs from stock Unreal Engine 2, and the changes the Reloaded patch introduces.

SCCT Versus is built on Unreal 2 technology, so people who know legacy Unreal map editing should find the editor familiar. General Unreal 2 level-building tutorials apply. The wiki covers the custom features unique to SCCT Versus.

## Table of contents (wiki page → digest slug)

| Wiki page | Digest (`scct://wiki/<slug>`) |
|---|---|
| Spawn Points | `spawn-points` |
| Game Modes | `game-modes` |
| Map Settings | `map-settings` |
| Level Properties | `level-properties` |
| Vision Mode Properties | `vision-mode-properties` |
| Geometric Event (GE) | `geometric-events` |
| Events | `events` |
| Alarms | `alarms` |
| Movers | `movers` |
| Elevators (under Movers) | `elevators` |
| Lighting | `lighting` |
| Material Classes | `material-classes` |
| Static Meshes | `static-meshes` |
| Sound Actors | `sound-actors` |
| Sound Browser (not in the wiki sidebar; linked from Map Packaging) | `sound-browser` |
| Gameplay Objects | `gameplay-objects` |
| Buttons and Hacking Panels (under Gameplay Objects) | `buttons-and-hacking-panels` |
| Level Ingredients (under Gameplay Objects) | `level-ingredients` |
| Zones and Volumes | `zones-and-volumes` |
| SoftBody Actors | `softbody-actors` |
| Animated Meshes | `animated-meshes` |
| Fish | `fish` |
| Map Tutorials | `map-tutorials` |
| Map Packaging | `map-packaging` |

`scct://wiki/index` maps the pages to the MCP tools. It also has the minimum viable Versus map checklist.

## Conventions used in every digest

- Class and property names are copied verbatim from the wiki, including its spellings. Examples: `TriggerMethode`, `strenght`, `bInitialyOn` on `SAmbientSoundTrigger`, `iAttenteAvantActivation`, `fEloignementMaxX`. Do not "correct" them, because the engine uses these names.
- Units are Unreal units (uu). The wiki states 1 cm = 1 uu, and the RE+ tools assume about 100 uu per metre. Z is up. Rotations use 65536 units per full turn: 90° = 16384, 180° = 32768.
- Property values in RE+ map JSON (`scct.map-changes`, `actors_edit`) are strings:
  - Numbers are written as `"64"`.
  - Booleans are written as `"True"` / `"False"`.
  - Enum values are bare, such as `"GM_Multi"`.
  - Text is JSON-quoted, such as `"\"Server room\""`.
  - Structs are complete objects.
- Where a digest says "image-only", the wiki gives that information only in a screenshot. The digest marks it so you do not mistake an absent value for a default.
