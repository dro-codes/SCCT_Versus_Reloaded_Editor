# Bridge ops

`workflow_request {op, args}` runs one op of the editor's workflow bridge by name, for what no other tool covers. The editor enforces each op's policy whatever the client asks:
- **read** ops change nothing in the map (a selection or camera move at most).
- **undoable** ops change the map inside an Undo step; the joinable ones can share one Undo step with the rest of an `mcp.plan`.
- **file** ops write files or packages (map saves, imports) and are not undoable; packages are only the map's own.
- **build** and **process** ops build the map or start and end processes.
- **raw** is a console command, off unless `[Mcp] AllowRawExec=1` (`editor_exec`).
- **blocked** ops are never run over the pipe: they open windows, undo blindly or skip the staleness checks.

Every op that changes something runs as a one-step plan, so approval, dry runs, the history file and the one Undo step apply to it. The undo ops and `exec` run on their own and cannot go inside a plan. Arguments are given as `args`; an op whose arguments are not listed here takes `{}` or the names shown, which come from `Reloaded.Editor/WorkflowTools.cpp`. Actors are `{path, class}` identities (from `actors.query` or `map_query`).

## Read

- `editor.state` {} → {open, level, generation, revision, contentRevision, mapKey, mapFile, map, levelPath, dirty, canUndo, canRedo, undoMark, geometryBuilds, version}
- `log.tail` {since, max} → {lines: [{seq, level, text}], last}
- `approval.status` {ticket} → {status: pending|approved|done|denied|stale|cancelled, outcome}
- `blob.read` {blob, offset, size} → {data (base64), eof}
- `blob.free` {blob}
- `actors` {selected}
- `actors.query` {classes, tags, names, near {at, radius}, box {min, max}, selected, fields, limit (500, at most 5000), cursor} → {actors: [{path, class, name, tag, event, position, rotation, selected, hidden, values}], total, next}
- `map`
- `map.file`
- `settings.inspect` {} → the LevelInfo {actor, level, generation, values, schema, unavailable}
- `authoring.mapid` {} → {map}
- `authoring.schema` {classes} → {classes: {class: {schema, choices, writable}}}
- `authoring.export` {} → {blob, size} (read it with blob.read)
- `authoring.preview` {document, baseDirectory} → {rows, tally, identities}; refuses texture and save operations as authoring.apply does
- `assets.search` {kind, query, limit} → {assets: [{path, kind, bounds}]}
- `view.screenshot` {viewport, maxEdge, camera {location, rotation}, frame} → {png, width, height}
- `view.capture`
- `view.restore` {view}
- `mapcheck.run` {} → {rows: [{severity, group, check, message, actors, surfaces}], summary}
- `lighting.budget` {}
- `scene.trace` {segments: [{from, to}]} → {hits: [{hit, at, normal, actor}]}
- `ge.list` {} → {elements}
- `object.inspect` {path}
- `mapsettings.get` {}
- `mcp.sidecar.get` {} → {sidecar}
- `props.copy` {actor}
- `design.pieces` {} → {pieces, elements}
- `design.scene`
- `design.grid`
- `design.view`
- `design.bspowners`
- `design.builds`
- `design.spawns`
- `design.clearances`
- `placement.selection`
- `security.actors`
- `light.list`
- `objective.actors`
- `stage.actors`
- `stage.read`
- `stage.preview` {plan}
- `optimise.report` {base}
- `magic.classes`
- `magic.assets` {type, classes}
- `magic.inspect` {actor}
- `magic.json.export` {actor}
- `camera.snapshot`
- `brush.visibility`
- `usages` {asset}
- `connections` {actor}
- `surface.brushes`
- `mesh.bounds`
- `brush.snap.bounds` {surfaces}
- `vertex.selection`
- `stairs.state` {surfaces}
- `tag.preview` {actor, tag}
- `assembly.capture` {members, position, rotation}
- `emitterlib.list`
- `emitterlib.packs`
- `emitterlib.cache`
- `emitterlib.problems`
- `emitterlib.categories`
- `emitterlib.selected`
- `emitterlib.capture` {members, map}
- `skins.settings`
- `objectiveplayers.state`
- `conveyor.state`
- `prefab.instances` {} → the map's linked prefab instances, each with its live members
- `prefab.counts` {} → {assembly id: linked instances in the map}
- `objimport.preview` {file, package, group, name, scale, up (y|z), collision (none|box|polygons|groups|parts|file), split, partTriangles}: what Import OBJ would make, changing nothing
- `mover.selected` {} → the one selected mover's identity, or null
- `mover.state` {actor} → {actor, name, class, revision, path, location, rotation, values, choices, triggeredBy, triggers}
- `mover.outline` {actor}
- `mover.candidates` {actor}: the triggers mover.wire can point at it

## Undoable

- `mcp.plan` (not joinable) {label, pre, steps, post, dryRun}: steps are {id, op, args, optional}; {"$bind": {step, ptr}} in an argument takes a value from an earlier step's result. pre runs first (loads, imports, saves; not undoable), steps are one Undo step and roll back together, post runs after (builds, texturing, Map Check, saves, screenshots), each on its own; an optional post step may fail. Deletes come before any add. A dialog the bridge does not know stops the plan at that step (dialog_blocked, with details stoppedAt, ran, wrote, built, rolledBack). scct://schema/build-plan
- `undo.call` (not joinable) {call} → {call, done, steps}: undoes MCP call n while its steps are the newest (refused: not_newest, already_undone, gone; invalid: unknown_call for a call the editor has no record of)
- `undo.steps` (not joinable) {count} → {done}
- `redo.steps` (not joinable) {count} → {done}
- `authoring.apply` {document, baseDirectory} → {identities: {create id: {path, class}}, deleted}; texture and save operations only into the map's own <Map>_<name> package (a texture also into MyLevel), else refused before anything runs: not_map_owned, package_name (<Map>, Amb_<Map>, or a base name another kind of package has), interface_package (<Map>-i), unsaved_map (the map has no file yet); a load alone is allowed
- `select` {actors, focus}
- `design.block` {spec, position, rotation, previous}
- `design.blockbatch` {items}
- `design.align` {scene, axis, mode, spacing}
- `design.layer` {members, hidden, locked, group, groupAction}
- `design.polyflags` {flags, members}
- `design.repairflags`
- `design.sendtolast` {members}
- `design.flags` {members, hidden, locked}
- `design.group` {members, group, action}
- `design.translate` {members, delta}
- `design.lift` {position, width, length, thickness, rise, moveTime}
- `design.tempstart` {class, team, position, rotation}
- `design.register` {pieces, elements}
- `design.unregister` {members}
- `mcp.sidecar.set` {sidecar}
- `placement.drop` {surface, align}
- `placement.move` {moves: [{actor, delta}]}
- `placement.turn` {changes: [{actor, rotation}]}
- `placement.copy` {members, copies}
- `props.paste` {targets, properties}
- `start.create` {class, team, position, rotation}
- `alarm.locks` {alarm, targets}
- `security.create` {class, position, rotation, properties}
- `security.sensor` {low, high, properties}
- `security.link` {detector, alarm}
- `security.outputs` {alarm, targets}
- `security.unlink` {detector}
- `security.delete` {actor}
- `security.move` {actor, position, rotation, properties}
- `stage.apply` {plan}
- `objectiveplayers.set` {objective, min, max, open}
- `objectiveplayers.playlevel` {players}
- `objectiveplayers.targets` {mission, sizes, op}
- `conveyor.add`
- `conveyor.sync`
- `camera.order` {snapshot, paths, loop, detach}
- `camera.add` {snapshot, paths, loop}
- `magic.component` {snapshot, class}
- `magic.key` {snapshot, key}
- `magic.json.import` {snapshot, document}
- `magic.edit` {snapshot, changes}
- `magic.create` {class, event, trigger, geometry, group}
- `magic.link` {event, target, trigger, group}
- `objective.add` {owner, class}
- `replace` {usages, source, replacement}
- `tag.rename` {preview}
- `assembly.place` {definition, position, rotation, bindings}
- `emitterlib.place` {entry, id, name, position, rotation}
- `brush.visibility.set` {category, action}
- `builder.fit`
- `builder.fit.brush`
- `vertex.portal`
- `vertex.snap` {axes}
- `brush.snap` {axes, surfaces}
- `stairs.smooth` {surfaces}
- `stairs.remove` {surfaces}
- `settings.edit` {snapshot (from settings.inspect), changes {property: value}}
- `ge.add` {command: ZL|HOH|PIPE|LADDER|FENCE|POLE|LG, points}
- `mover.path` {actor, path, label}: the Mover Workbench's key list
- `mover.settings` {actor, changes {setting: value as text}}
- `mover.wire` {actor, trigger}: the trigger's Event to the mover's Tag
- `prefab.selectinstance` {}: the selection's linked prefab instance
- `prefab.selectall` {assembly}: every linked instance of the assembly (the selection's when empty)
- `prefab.place` (not joinable) {assembly (id or name), position, rotation, bindings}: a linked copy, filed in its own Group
- `surface.texture` (not joinable) {texture, surfaces | brushes, faces}: faces by where their normal points (floor, ceiling, +X, -X, +Y, -Y)

## File

- `map.new` {discard} → editor.state
- `map.open` {path, discard} → editor.state; a compiled map in Packages\Maps is refused as compiled_map (use `map.recover`)
- `map.save` {path, overwrite} → {file}
- `map.recover` {source, destination, merge, keepXboxOnly, discard, maxRows} → {ok, file, source, merge, keepXboxOnly, seconds, summary, counts, categories, rows, rowsTotal, report, folder}; a dry run → {dryRun, source, destination, merge, keepXboxOnly, imports, missing, sourceBytes, checks}. Only on its own: workflow_request refuses it (use_map_recover), and in a plan (not_in_plan; the server refuses any plan holding it, the editor one of more than one step); map_recover runs it. Progress {stage, stageIndex, percent}; cancellable until it saves. Refused: source_map, destination_exists, unsaved_changes, missing_packages (details.missing), busy
- `asset.texture` {file, package, group, name, format, mips, masked, alphaTexture, lodSet}: the map's own package only
- `asset.mesh` {file (.ase), package, group, name, collision}: the map's own package only
- `package.save` {package, overwrite}: the map's own package only
- `object.set` {path, properties}
- `interface.texture` {slot: Menu|Briefing|LoadingATT0|LoadingDEF0, file}
- `minimap.apply` {floors, framing, style, objectives, spawns, meshes, briefing, briefingFloor}
- `uas.build` {packages, quality}
- `mapsettings.set` {changes}
- `optimise.release` {packs, destination: map|package, assetPackage, folder, overwrite}: the map must be saved (unsaved_map); folder (default <Game>\Releases\<Map>) a full path only inside <Game>\Releases, the work folder or [Mcp] WriteRoots, else refused as outside_write_roots, so overwrite only replaces files there; assetPackage the map's own <Map>_<name>
- `conveyor.prepare`
- `emitterlib.save` {entry, members, map}
- `emitterlib.delete` {id}
- `emitterlib.update` {id}
- `objimport.import` {file, package (MyLevel is the map), group, name, scale, up, collision, split, partTriangles, place}: File > Import OBJ Mesh; with place, the meshes go where the viewport was last clicked in one Undo step. The meshes and the .mtl's textures go only into MyLevel or the map's own <Map>_<name> package, such as <Map>_STM, else refused before anything runs: not_map_owned, package_name (<Map>, Amb_<Map>, or a base name another kind of package has), unsaved_map (a <Map>_ package before the map's first save)
- `prefab.save` {name, members}: the selection (or members) as a new assembly in the library
- `prefab.update` {}: the selection's instance becomes its prefab's content and the map's other linked copies are rebuilt in one Undo step; the library keeps the new content
- `prefab.break` {}: the selection's instance leaves its Group and the library's record of it goes

## Build

- `build.all` {} → {ok, log}
- `design.build` {} : the Map Design geometry build

## Process

- `editor.quit` {discard}
- `play.level` {}: needs [Mcp] AllowPlay=1

## Raw

- `exec` {command} → {ok, output}; needs [Mcp] AllowRawExec=1 (use editor_exec). Even then refused: a command starting with QUIT, EXIT, RELAUNCH, DEBUG, EXEC, MAP NEW/LOAD/SAVE/IMPORT/EXPORT, OBJ SAVEPACKAGE/EXPORT/IMPORT/LOAD, SAVEMAPPROP, LOADMAPPROP, TEXTURE IMPORT/EXPORT, STATICMESH IMPORT/EXPORT, MESH IMPORT, BRUSH IMPORT/EXPORT, DUMPINT, TRANSACTION or AUDIO, read word by word as the console reads it (denied_command); a FILE= anywhere outside the work folder and WriteRoots (outside_write_roots); a PACKAGE= other than MyLevel or the map's own (not_map_owned); more than one command, | or a line break (invalid)

## Blocked

- `light.set` {actor, properties}
- `design.removestart` {start}
- `design.play` {start, position, rotation, launch}
- `package.preview` {map}
- `map.setfile` {file}
- `emitterlib.restore`
- `skins.apply` {slots, models, goggles}
- `skins.remove`
- `preview.open`
- `preview.show` {entry}
- `preview.capture` {path}
- `preview.clear`
- `preview.close`
- `preview.state`
- `preview.attach` {hwnd}
- `preview.detach`
- `preview.step` {seconds}
- `preview.camera` {camera}
- `characters.open`
- `characters.show` {slots, models}
- `characters.capture` {path}
- `characters.clear`
- `characters.close`
- `characters.state`
- `characters.camera` {camera}
- `mover.preview` {actor, position, rotation}
- `mover.restore`
