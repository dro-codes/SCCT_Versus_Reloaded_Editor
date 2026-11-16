---
name: add_game_mode
title: Add a game mode
description: Add Story, Story sectors, Disk Hunt or Deathmatch to the open map, wired the way the wiki requires, with its Map Settings entry.
arguments:
  - name: mode
    description: story, sectors, diskhunt or deathmatch.
    required: true
  - name: details
    description: What the mode should contain, such as "two terminals in the server room and a bomb in the vault" or "30 disks, 20 active, 12 to win".
wiki: [game-modes, spawn-points, events, map-settings]
rules: false
---
# Add a game mode: {{mode}}

{{#details}}**Requested:** {{details}}
{{/details}}
Follow the embedded **game-modes** page exactly. Name actors in lists by their **exact names**. The tools write those lists from spec ids.

The rule catalogue is not embedded, to keep this recipe short: read `scct://rules` (every check with its draft and release severity and its wiki page) before you design or fix anything.

## Recipe

1. **Read the map.**
   - Call `editor_status`, then `map_query` with `what:"summary"`, and with `what:"missions"`. Note the existing `SMission`s and their `GameMode`, the `SObjective`s, the triggers and the spawns.
   - Call `map_query` with `what:"schema"` and `classes:["SBase.SMission"]` (the schema needs full class names), and use its `GameMode` choices (`GM_Multi`, `GM_Undefined`, `GM_DiskHunt`, `GM_DeathMatch`) exactly as listed.
2. **Plan the mode.** Use what it needs:
   - **story:**
     - One `SMission` with `GameMode GM_Multi`, `Description` (Spy text), `DescriptionDEF` (Merc text), and `MinimumObjectives` no larger than its objectives.
     - Each `SObjective` lists its triggers in `Triggers`, with an `ObjectiveName`.
     - The triggers:
       - `SComputerObjectiveTrigger`: floor+78, clear of collision, with `DominationCaptureTime` (not `CaptureTime`) and `TriggerName`;
       - `SFlag`: with 1-5 `SFlagDropZone`s in `DropZone` and a `ResetTime`;
       - `SBombTargetObjectiveTrigger`: with `CaptureTime`.
   - **sectors:**
     - One `SMission` per sector with `GameMode GM_Undefined` and `bChained False`, holding that sector's objectives.
     - A top `SMission` with `GM_Multi` and `bChained True`, listing the sector missions in play order.
     - Optionally `STriggerPlayerStart`s that move spawns forward, and `SMessageTrigger` / `Zone<N>_Announce` messages such as "Go to Sector 2".
   - **diskhunt:**
     - `SDisk`s: at least the Map Settings `ScoreLimit`; place extra for randomness. Use `Group` 1/2/3 and a `ResetTime`, and keep them clear of geometry.
     - One `SMission` (`GM_DiskHunt`) whose `Triggers` lists every disk directly (no `SObjective`).
     - An `AtResetEvent` that disables security.
   - **deathmatch:**
     - One `SMission` with `GameMode GM_DeathMatch`.
     - If the map has security, an `AtResetEvent` → `SMagicEvent` that **untriggers** the alarms and detectors.
     - Optional separate spawns (`STriggerPlayerStart`).
3. **Apply.** Call `gameplay` with the matching part of the spec's `gameplay` section, at the top level of the arguments:
   - `story` (`mission`, `objectives[triggers]`, `sectors`), for story and sectors;
   - `diskhunt` (`disks`, `resetTime`, `mission`, and `disableSecurity` for the reset that turns security off);
   - `deathmatch` (`disableSecurity`, `mission`);
   - and `modes` with the mode (`story`, `diskhunt` or `deathmatch`; sectors are `story`) for "{{mode}}". It is added to the modes the map already has.

   Run it with `dryRun:true` first and read the preview, then without.
4. **Map Settings.**
   - Call `map_settings` with `modes:[{mode, respawn, timeLimit, spiesLives, sentriesLives}]`, one entry for this mode (matched by `mode`, so the others stay): `respawn` 5-15, lives 0-10, `timeLimit` 0-20. For Disk Hunt also give `scoreLimit`, `handicap` and `numDisksMax`.
   - `Modes[i]` and `ModesStrings[i]` must stay index-aligned. If Map Settings does not exist yet, save, `editor_restart`, then retry.
5. **Check.**
   - Call `map_validate` with `profile:"draft"`, and fix every `story.*`, `modes.*`, `objectives.*`, `diskhunt.*` and `deathmatch.*` finding.
   - Call `view_screenshot` with `preset:"objective:<name>"` for each new objective.
6. **Report.** List the mission, its objectives and triggers by name, and what to playtest.
