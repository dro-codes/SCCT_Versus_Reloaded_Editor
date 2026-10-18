---
name: ge_pass
title: Geometric event pass
description: Add the Spy traversal layer to the open map (ledge grabs, hand-over-hand, pipes, ladders, ziplines, fences) measured against the wiki, and check existing GEs.
arguments:
  - name: area
    description: Where to work (room or zone names); default the whole map.
    default: the whole map
  - name: focus
    description: GE kinds to concentrate on (ledge, hoh, pipe, ladder, zipline, fence, pole), or "all".
    default: all
wiki: [geometric-events, material-classes]
rules: true
---
# Geometric event pass

**Area:** {{area}}. **Kinds:** {{focus}}.

GEs give Spies their routes: ledges up to balconies, pipes up shafts, hand-over-hand across gaps, and ziplines down. Mercenaries can only use ladders. Good GEs create **alternative spy routes** that avoid the Mercenaries' main paths and the security.

## Measurements (from the embedded geometric-events page)

| Kind | Measurements |
|---|---|
| Ledge grab | Grab height 208-320 standing, ≤448 with a running jump, 512 for a co-op boost (144 deep platform). Obstacles under 115 are jumped. 64 from the ledge to the wall behind to pull up. Ledges ≥32 long. |
| Fence | 88-128 high (96 recommended), with `bClimbable` set on the fence texture as well as the GE |
| Hand-over-hand | 216 clearance below |
| Ladder | An even number of bars, ≥8. Spies and Mercenaries can both use it |
| Zipline | Hang point ≤384 above the floor, 198 clearance below (161 tucked). Sloped, on the cable's upper edge |
| Pipe and ladder points | Lower end first, with the side point at that end's height pointing where the Spy approaches from |
| Joins | A connected pipe + hand-over-hand, or pipe + zipline, share the exact same vertex |

## Recipe

1. **Read the map.**
   - Call `editor_status`, `map_query` with `what:"design"` (blockout pieces and their bounds), `map_query` with `what:"ges"` (existing GEs, where reported) and `plan_render`.
   - Call `view_screenshot` on the area.
2. **Find candidates.** Look for:
   - edges of platforms, balconies and crates at 208-448 above a floor;
   - vertical pipes and shafts;
   - ceiling pipes and beams with 216 or more clearance below;
   - high-to-low spans for ziplines;
   - fences 88-128 high.

   Measure with `workflow_request` op `scene.trace` (segments from above down to the floor) where the blockout does not tell you the heights.
3. **Place the GEs.**
   - Call `geometric_events` with `ges:[{kind, points}]`:
     - 2 points along the edge for `ledge`, `hoh`, `zipline`, `fence` and `pole`;
     - 3 points for `pipe` and `ladder`: bottom, top, then the side the player climbs from.
   - The tool orders pipe and ladder points low end first and runs the editor's `GE ADD`.
   - Run it with `dryRun:true` first.
   - For a fence, set `bClimbable` on its texture in a map-owned package (`workflow_request` `object.set`), or ask the user to do it.
4. **Dynamic GEs.**
   - If geometry changes during play, add `geTriggers:[{id, toggle, geMask}]` (`SGETrigger`).
   - Set the starting `GEActivationMask` with `map_settings` (the spec's `level`).
   - The GE IDs themselves are assigned with the editor's `Set GE ID` (right-click a GE); ask the user to do that by hand.
5. **Check.**
   - Call `map_validate` with `include:["wiki"]`, and fix every `ge.*` finding. Each cites the measurement it failed.
   - Call `view_screenshot` on each new route.
6. **Report.** List the routes added (from → to, using which GEs), the measurements, and what to playtest in game.
