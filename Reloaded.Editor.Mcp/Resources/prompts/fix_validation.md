---
name: fix_validation
title: Fix validation findings
description: Run map_validate (or take its findings), apply each fix with a dry run first, and loop until the map is clean for the chosen profile.
arguments:
  - name: profile
    description: draft or release.
    default: draft
  - name: scope
    description: errors (only errors) or all (errors and warnings).
    default: errors
  - name: findings
    description: Findings JSON from an earlier map_validate call, to start from instead of validating again.
wiki: [index]
rules: true
---
# Fix validation findings

**Profile:** {{profile}}. **Scope:** {{scope}}.
{{#findings}}**Starting findings:**

```json
{{findings}}
```
{{/findings}}
## Recipe

1. **Findings.** {{^findings}}Call `map_validate` with `profile:"{{profile}}"`.{{/findings}}{{#findings}}Use the findings above, but call `map_validate` with `profile:"{{profile}}"` once to confirm they are current.{{/findings}}
   - Sort the findings errors first.
   - Keep the scope: {{scope}}. Info rows are never fixed.
2. **Read before fixing.** For each `wikiPage` that appears, read `scct://wiki/<wikiPage>`. Use the class and property names exactly as written there.
3. **Fix in groups.** Do the groups in this order:
   1. structure: missing missions, spawns, the CamNet;
   2. references: dangling Events, unresolved lists;
   3. values: heights, ranges, flags;
   4. settings and release.

   For each finding:
   - If it has `fix: {tool, args}`, call that tool with those args plus `dryRun:true`. Read the preview, then call it again without `dryRun`.
   - If it has no `fix`, choose the tool:

     | Finding is about | Tool |
     |---|---|
     | spawns, missions, objectives, disks | `gameplay` |
     | alarms, detectors, CamNet | `security` |
     | doors, lifts, buttons | `mechanisms` |
     | Tag/Event links, SMagicEvent | `events` |
     | lights, zones, volumes, ingredients, objects | `environment` |
     | GEs | `geometric_events` |
     | Level Properties, Map Settings | `map_settings` |
     | packages, `.uas` | `release` |
     | one property on one actor | `actors_edit` with `set` |

   - Where the map was built from a spec (`map_query` with `what:"spec"` is not null), prefer editing the spec and calling `level_build` with `sync:"incremental"`, so the spec stays the truth.
   - If a call fails as `stale`, re-read with `map_query` and retry once. If it is `denied`, stop and ask the user.
4. **Rebuild when needed.** After any brush or blockout change, call `map_build` with `what:"geometry"`. After static lighting changes, use `what:"all"` before release.
5. **Re-validate.** Call `map_validate` with `profile:"{{profile}}"` again.
   - Repeat steps 3-5 at most 5 times.
   - If a finding survives two attempts, stop, explain it, and leave it for the user.
6. **Report.**
   - What was fixed, with tool calls and each `undo.call`, so the user can revert any of them with `editor_undo`.
   - What remains, and why.
   - The final summary counts.
