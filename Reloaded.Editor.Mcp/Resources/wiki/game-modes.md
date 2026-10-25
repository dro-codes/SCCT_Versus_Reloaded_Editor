# Game Modes

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Game-Modes

A new level should consider three game modes: **Story**, **Disk Hunt** and **Deathmatch**.
- Story is the primary mode.
- Maps should ideally support Disk Hunt and Deathmatch as well. Then the map still works when a player selects it in another mode.

Each mode is driven by an `SMission` actor whose `GameMode` selects the mode:

| Mode | `SMission.GameMode` | Objective actors |
|---|---|---|
| Story | `GM_Multi` | `SObjective` → `SComputerObjectiveTrigger` / `SFlag` (+ `SFlagDropZone`) / `SBombTargetObjectiveTrigger` |
| Story sector (one per sector) | `GM_Undefined` | the sector's `SObjective`s |
| Disk Hunt | Not stated in the Game Modes text (the value is image-only). Map Settings names the mode `GM_DiskHunt`. | `SDisk`, listed directly in the SMission `Triggers` |
| Deathmatch | `GM_DeathMatch` | none |

## Story

Story supports three objective types. A map can use any one of them, or combine several in the same level:
- Computers: `SComputerObjectiveTrigger`
- Hard disks: `SFlag` / `SFlagDropZone`
- Bombs: `SBombTargetObjectiveTrigger`

### SComputerObjectiveTrigger (computer / ND133 "modem" neutralization objective)

This is the **ND133 neutralization** objective — the one the Spy's **Modem** gadget services (`SGadBomb`, in-game name "Modem": *"Disables an ND133 by hacking its control device"*). The Spy sticks a modem on a surface with a **clear line of sight to the ND133, within range**, and it neutralizes the ND133 remotely; the Spy can leave and come back to finish it. The game enforces placement with player messages *"No surface to stick the modem"*, *"You are too far from a ND133"*, *"Modem can't see ND133"*, *"Can't place a modem from this position"*; HUD states are `<TriggerName> : Neutralization in progress` then `ND133 neutralized`.

Adding one in the editor:
1. Select the actor in the **Actor Class Browser**.
2. Right-click in the viewport and choose `Add SComputerObjectiveTrigger Here`.
3. Edit its properties with `F4`, or right-click → `SComputerObjectiveTrigger Properties`.

> **IMPORTANT:** place the computer so it does **not intersect any collision geometry**. If it does, players cannot capture it. The correct height is **approximately 78 units from the ground**.

- `DominationCaptureTime`: time the Spies need to neutralize the ND133 (the modem's hack duration).
- `InteractionDistanceToCenter`: the modem's **reach** — how close to the ND133 the Spy must plant it (line of sight is enforced separately). The `gameplay` tool exposes this as the trigger's `reach`.
- `TriggerName`: name shown on the HUD, as in `<TriggerName> : Neutralization in progress`.
- **Gotcha:** `CaptureTime` does **not** affect `SComputerObjectiveTrigger` actors. Only `DominationCaptureTime` needs to be set.
- To finish the objective, link it to an `SObjective` (see below).

> **Design note:** to make the modem mechanic matter, place the ND133 where a Spy must find a plant spot with line of sight to it (behind glass, down a Merc-watched sightline), with a covered approach (a vent, ledge or zipline) — so the Mercenaries sweep likely modem spots rather than just camp the ND133.

### SFlag (hard disk objective)

The hard disk must be extracted into a drop zone (`SFlagDropZone`).

- `DropZone`: add the name of your `SFlagDropZone` actor to the list.
  - At least **1** drop zone is needed, and up to **5** can be used.
  - Several `SFlag` actors can share the same drop zones.
- `ResetTime`: time before a dropped disk returns to its original location.
- `TriggerName`: name shown on the HUD, as in `<TriggerName> : Data stolen`.
- Link it to an `SObjective`.

#### SFlagDropZone

The drop zone for `SFlag` objectives. Place one in the level and link it through the `DropZone` field of an `SFlag`. **No properties need to be configured** on `SFlagDropZone`.

### SBombTargetObjectiveTrigger (bomb / sabotage objective)

A **bomb the Spy plants and then protects** until it detonates — distinct from the ND133/modem neutralization above. The plant action reads `Place Bomb`; once planted a countdown runs and the Mercenaries race to find and defuse it. HUD states: `<TriggerName> : Under attack`, then `: Bombed` if it blows or `: Secured` if the Mercs save it. Spy tip: *"After placing your bomb, try to protect it. Use teamplay to cover a zone."* Merc tip: *"Once the bomb countdown appears, you have a few seconds to find the bomb."*

- `CaptureTime`: the bomb's **fuse**, in seconds, once it is planted.
- `TriggerName`: name shown on the HUD, as in `<TriggerName> : Under attack`.
- Link it to an `SObjective`.
- (From the Alarms page: `SBombTargetObjectiveTrigger` **cannot be locked** by an alarm.)

### SObjective

Each objective in a level is managed by an `SObjective`. Every objective trigger (`SComputerObjectiveTrigger`, `SFlag`, `SBombTargetObjectiveTrigger`) **must be linked to an `SObjective` to appear in game**.

- `Description`: the objective text for the **Spies**, shown in Mission Status (F8 by default).
- `DescriptionDEF`: the objective text for the **Mercenaries**, shown in Mission Status (F8 by default).
- `ObjectiveName`: shown on the HUD when the objective is completed, as in `<ObjectiveName> : Neutralized`.
- `Triggers`: the objective triggers linked to this `SObjective` (`SComputerObjectiveTrigger`, `SFlag` or `SBombTargetObjectiveTrigger`). Add entries with the `Add` button and type the **exact name** of each trigger.
  - Tip: with an actor selected, `Ctrl+B` copies its name to the clipboard.
- `TriggersERVisibilityZone`: a `ZoneInfo`. With it set, all of this objective's triggers show on Enhanced Reality (ER) even when the player is outside the `SObjective`'s zone.
  - By default, outside the zone the HUD shows a single icon at the `SObjective` location. It shows every trigger location only when the player is in the same zone.

### SMission (Story)

An `SMission` can be placed anywhere in the map. It sets the game mode and holds all the `SObjective` actors:

```
SMission
 └─ SObjective
      ├─ SComputerObjectiveTrigger
      ├─ SFlag
      └─ SBombTargetObjectiveTrigger
```

- `bChained`: whether objectives are sequential (linear) or all active at once.
  - With `True`, objectives activate one at a time, as in linear sector maps such as Orphanage.
- `AtResetEvent` (Events section): an event sent at the beginning of a Story game. It is sent **only once**, when the level initialises after loading.
- `Description`: mission text for the Spies.
- `DescriptionDEF`: mission text for the Mercenaries.
- `GameMode`: use `GM_Multi` for Story.
- `MinimumObjectives`: how many objectives the Spies must neutralise to win the Story match.
- `Objectives`: every `SObjective` of this mission. Add entries with `Add` and type each objective's **exact name**.

The SMission property sheets are image-only for the full list.

### Sector maps

Linear sector maps such as Orphanage use several `SMission` actors, one per sector, so objectives activate in turn as play moves through the map:
1. Make one `SMission` per sector, with `GameMode` set to `GM_Undefined`. Give it that sector's objectives.
2. Make one `SMission` for Story as a whole, with `GameMode` set to `GM_Multi`. Add every sector `SMission` (Sector 1, Sector 2, Sector 3, ...) to its `Objectives` list.
3. Set `bChained` on the overall Story `SMission`, so a later sector's objectives do not activate until the previous sector is complete.
4. Keep `bChained` **off** on the individual sector `SMission` actors.

## Disk Hunt

Disk Hunt has one objective type: `SDisk`.

### SDisk

- Place **at least as many disks as the Spies need to collect**.
- For randomness, place more disks than will appear in the game. For example, a map might place 30 `SDisk` actors of which only 20 are active.
- `Group`: groups shape the spawn randomness. The engine picks random disks from Group 1, then Group 2, then Group 3, and cycles back to Group 1 once every group has been used.
- `ResetTime`: time before a dropped disk returns to its original location.

> **IMPORTANT:** give the disk enough free space around it, and keep it clear of the floor and other geometry. Players cannot pick up a disk that intersects geometry.

### SMission (Disk Hunt)

Unlike Story, Disk Hunt does **not** use `SObjective` actors. Every `SDisk` is linked **directly** to the `SMission`.
- `AtResetEvent`: an event sent once at the beginning of a Disk Hunt game. A common use is **disabling security systems**, since Disk Hunt objectives are scattered disks rather than structured Story objectives.
- `Description` / `DescriptionDEF`: mission text for Spies and Mercenaries.
- `Triggers`: every `SDisk`. Add entries with `Add` and type the exact name of each trigger.
- The `GameMode` value is image-only here. The Map Settings page names the Disk Hunt mode `GM_DiskHunt`.

## Deathmatch

Deathmatch has **no objective actors** and is simple to set up.

### SMission (Deathmatch)

- `AtResetEvent`: an event sent once at the beginning of a Deathmatch game. A common use is **disabling security systems**, since Deathmatch has no objectives.
- `Description` / `DescriptionDEF`: mission text for Spies and Mercenaries.
- `GameMode`: use `GM_DeathMatch`.

## How the MCP tools apply this

The `gameplay` tool and the `gameplay` section of a `scct.level-spec` (built with `level_build`) write all of this with the wiki defaults:

Story:
- An `SMission` with `GameMode GM_Multi` lists the `SObjective`s by exact name in `Objectives`.
- Each `SObjective` lists its triggers in `Triggers`.
- `Description` holds the Spy text and `DescriptionDEF` the Merc text.

Objective triggers:
- A terminal is an `SComputerObjectiveTrigger`.
  - It goes at floor+78, and a trace checks that it is clear of collision.
  - It uses `DominationCaptureTime`. A warning fires if `CaptureTime` is set.
- An `SFlag` needs 1-5 `SFlagDropZone`s.
  - Each `dropZones` entry is `{id, at}`, a new `SFlagDropZone`, or `{ref: id}`, one another flag's entry made: **flags may share drop zones**, and a shared zone is made once.
  - A `ref` that names no drop zone, or one a flag lists twice, is an error (`story.flag-dropzones`).
- A `terminal` trigger is an `SComputerObjectiveTrigger` (the ND133 the Spy neutralizes with a modem)
  and uses `DominationCaptureTime` (plus `reach` → `InteractionDistanceToCenter`, the modem's reach);
  a `bomb` trigger is an `SBombTargetObjectiveTrigger` (a planted
  bomb with a fuse) and uses `CaptureTime`. When the author gives no objective `description`/
  `descriptionDef`, the `SObjective` gets default coaching per kind — terminal → Spy "Neutralize
  <name>: place your modem where it can see the ND133, then fall back while it hacks." / Merc "Defend
  <name>; stop the spies neutralizing the ND133."; bomb → Spy "Plant a bomb at <name> and protect it
  until it detonates." / Merc "Defend <name>; find and defuse the bomb before it blows."; flag →
  steal/carry.

Sectors (`gameplay.story.sectors`):
- The top mission is `GM_Multi` with `bChained True`.
- Each sector mission is `GM_Undefined` with `bChained False`.
- `Zone<N>_Complete` SMagicEvents and `Zone<N>_Announce` SAlarms fire on sector completion (the RE+ Zones window shape).
- An `open` action holds its door open (`TriggerToggle`) unless `hold: false`. A held door must open from nothing else (error `story.action-door-trigger`); see `scct://wiki/movers`.
- `map_query what:"stages"` reads them back.

Disk Hunt:
- An `SMission` (`GM_DiskHunt`) lists every `SDisk` in `Triggers`.
- `Group` cycles 1/2/3 over the placed disks.
- Disks are kept clear of geometry.
- The disk count must be at least the Map Settings `ScoreLimit`.

Deathmatch:
- An `SMission` (`GM_DeathMatch`).
- With `disableSecurity`, its `AtResetEvent` fires an SMagicEvent that untriggers the security.

Before you rely on an enum value, check the live enum names with `map_query what:"schema" classes:["SBase.SMission"]` (the schema takes full class names).

- Rules: `story.no-mission`, `story.no-multi`, `story.no-objective`, `story.unresolved-reference`, `story.trigger-kind`, `story.objective-orphan`, `story.objective-no-trigger`, `story.mission-empty`, `story.minimum-objectives`, `story.sector-chain`, `story.visibility-zone`, `modes.missing`, `objectives.flag-dropzones`, `objectives.dropzone-unused`, `objectives.terminal-placement`, `objectives.terminal-capture-time`, `objectives.modem-reach`, `diskhunt.disk-count`, `diskhunt.disk-group`, `diskhunt.disk-clearance`, `diskhunt.disk-unlisted`, `deathmatch.security`, `objectiveplayers.problems`, `objectiveplayers.targets`
