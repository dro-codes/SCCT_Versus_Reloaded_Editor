# Map Packaging

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Map-Packaging

When you release a new map:
- provide **all the files it needs**;
- **avoid modifying original packages**, which causes compatibility problems.

This page covers the naming conventions and the files to include.

## Map filename

- Original game maps usually use **3 letters followed by 2 numbers**.
- Enhanced SCCT Versus maps usually use **letters only**.
- **Recommended format:** the **first 4 letters of the map name plus a suffix letter**, using **`D` for the default version**. For example, Aquarius is `AquaD`.

## Package naming

**All packages must have unique base names.**
- Unreal Engine does not use file extensions internally; the extension only helps users tell file types apart.
- Internally, **packages with the same base name are the same package**. For example, these conflict:
  - `Packages\Textures\Aquarius.utx`
  - `Packages\StaticMeshes\Aquarius.usx`

  The extensions differ, but the engine sees both as `Aquarius`, which causes problems.

### Recommended package naming

| Package type | Naming pattern | Example |
|---|---|---|
| Textures | `<MapName>_TXT.utx` | `Aquarium_TXT.utx` |
| Static Meshes | `<MapName>_STM.usx` | `Aquarium_STM.usx` |
| Sounds | `Amb_<MapName>.uax` | `Amb_Aqua.uax` |

> **NOTE:** **do not modify or rename original game packages or maps.** It can cause compatibility problems or version mismatches for other players.

## Release files

A release archive contains:
- `Packages\Maps\<MapName>.sdc`
- `Packages\MapsEd\<MapName>.sdc` (not needed to play the map, but needed to edit it later in UnrealEd)
- `Packages\Textures\<MapName>-i.utc` (the Map Settings file)
- `Packages\Sounds\<MapName>.uas` (see `scct://wiki/sound-browser`; the wiki's own link points at a "Sounds#uas-packages" anchor)
- Any new packages made for the map, such as:
  - `Packages\Textures\<MapName>_TXT.utx`
  - `Packages\StaticMeshes\<MapName>_STM.usx`
  - `Packages\Sounds\Amb_<MapName>.uax`

> **TIP:** if a `<MapName>-i.utx` is generated, the game does not use it and it can be deleted. Only the `.utc` version is needed.

> **WARNING:** **if you lose your `MapsEd` map file, you can no longer change the map in UnrealEd.** The file generated in `Packages\Maps` **cannot be converted back** into a `MapsEd` file.
> - Back up your `MapsEd` file, or include it in the release archive.
> - Losing the `Maps` file is not a problem: the `MapsEd` file can easily generate a new one in the editor.

## How the MCP tools apply this

`map_file action:"save_as"` names new maps by the convention:
- First 4 letters of `mapName` + `D` (`Aquarius` → `AquaD`).
- Saved in `Packages\MapsEd`, ASCII, without spaces.
- Saves keep backups. The MapsEd file is never deleted.

Package writes:
- Every package the tools write must be **map-owned**: `<Map>_TXT`, `<Map>_STM`, `Amb_<Map>` and `<Map>-i`.
- Stock packages and maps are never written or renamed.
- Duplicate base names across folders are refused.

`release` checks and assembles the release:
- `.uas` built with `Interface` (`scct://wiki/sound-browser`);
- `<Map>-i.utc` present and any `-i.utx` noted for deletion;
- package names unique;
- the archive file list above.

`map_validate profile:"release"` reports naming problems. Given the `release` check's `findings` in its `release` argument, it also reports the files, dependencies and optimise suggestions only that check can see.
- Rules: `release.package-names`, `release.file-name`, `release.naming-convention`, `release.uas`, `interface.textures`, `release.files`, `release.dependencies`, `release.optimise`
