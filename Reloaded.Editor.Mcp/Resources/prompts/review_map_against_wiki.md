---
name: review_map_against_wiki
title: Review the map against the wiki
description: Audit the open map against every SCCT Versus wiki rule and the minimum viable map checklist, and report findings by wiki page with the fix for each. Changes nothing unless asked.
arguments:
  - name: profile
    description: draft (map in progress) or release (before sharing).
    default: release
  - name: focus
    description: Areas to look at closely (spawns, modes, security, camnet, lights, zones, ges, settings, release), or "everything".
    default: everything
wiki: [index, spawn-points, game-modes, level-ingredients, alarms, zones-and-volumes, lighting, map-settings, map-packaging]
rules: false
---
# Review the map against the wiki

**Profile:** {{profile}}. **Focus:** {{focus}}.

**This is a review: change nothing** unless the user asks you to apply fixes. If they do, use the `fix_validation` prompt.

The rule catalogue is not embedded, to keep this recipe short: read `scct://rules` (every check with its draft and release severity and its wiki page) before you design or fix anything.

## Recipe

1. **State.** Call `editor_status`, then `map_query` with `what:"summary"`: file, unsaved changes, actor counts, spawns per team, missions, objectives, security, cameras.
2. **Automatic checks.** Call `map_validate` with `profile:"{{profile}}"` and a high `limit`, so you see every finding.
   - Each finding has `severity`, `rule`, `wikiPage`, `message`, `actors` and usually `fix`.
   - Rules and their severities in each profile are in `scct://rules`.
3. **Walk the checklist.** Go through the **Minimum viable Versus map checklist** in the embedded index, item by item, and confirm each with data. For each item, write ✅, ⚠️ or ❌ with the evidence.
   - Spawns: `map_query` with `what:"spawns"` (per team, ≥3) and `view_screenshot` with `preset` `spawn:spy` and `spawn:merc`.
   - Modes: `map_query` with `what:"missions"`: a `GM_Multi` mission with objectives and triggers, `GM_DiskHunt` with `SDisk`s, `GM_DeathMatch` with a security reset.
   - CamNet: `map_query` with `what:"cameras"`: ≥1 camera, exactly one `bFirstCam`, a closed `NextCam`/`PrevCam` loop, collision on.
   - Security: `map_query` with `what:"security"` and `what:"wiring"`: detector Events reach alarm Tags, `MessagePriority` 0-5, no locked bomb targets.
   - Terminals: `view_screenshot` with `preset:"objective:<name>"`: about 78 above the floor and clear of geometry.
   - Lights: `map_query` with `what:"lights"`: only `LE_None`, `LE_Spotlight`, `LE_Sunlight`, or `LE_ESpotShadow` for Echelon lights.
   - Zones: a `LocationName` in each area, spawns included, and no `SAlarmZoneInfo`.
   - Level and Map Settings: `map_query` with `what:"level"`. Map Settings ranges, CAPS, index parity, interface textures.
   - Release: file name, package names, `.uas` with `Interface`.
4. **Things the checks cannot see.** Note what needs a playtest: capture times, sightlines, alarm timing, spawn fairness, readable HUD text. Suggest `view_screenshot` angles.
5. **Report.** Group by wiki page, errors first. For each finding give:
   - what is wrong, with actor names;
   - the wiki rule, quoted or paraphrased from the embedded page;
   - the fix: the tool call from the finding's `fix`, or the intent tool (`gameplay`, `security`, `mechanisms`, `events`, `environment`, `geometric_events`, `map_settings`, `release`) or `actors_edit` call that would do it.

   End with a verdict: ready for {{profile}} or not, and the three most important fixes.
