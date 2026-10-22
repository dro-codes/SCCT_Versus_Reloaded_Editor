# Map Settings

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Map-Settings

`SMapSettings` stores the session parameters:
- match length
- team lives
- respawn time
- briefing text
- the textures for the map thumbnail, the briefing screen and the loading screens

The editor **generates these settings automatically when the map is saved**.

## Opening Map Settings

- Right-click an empty space in the map and choose `Map Settings` from the context menu.
- Empty space is easiest to hit in **Wireframe** mode. In other render modes you may need to fly outside the map and right-click empty space.

> **NOTE (gotcha):** on a brand-new map, `Map Settings` does not work until you have **saved the map to a file, then exited and reopened it**.

The full SMapSettings property sheet is image-only.

## Top-level properties

- `MapName`: the name the engine displays for the level. Use **all caps**, the Ubisoft standard.
- `ExpliMapInBriefSentry`: the narrative briefing that introduces the mission to the **Mercenary** team. It is shared by all three game modes.
- `ExpliMapInBriefSpy`: the narrative briefing that introduces the mission to the **Spy** team. It is shared by all three game modes.
- `IsAmateur`: unused; safe to ignore.
- `LegendMap`: the short mission description on the map selection screen. It usually holds the date, time and location, and a brief narrative description of the map.
- `LegendMapInBrief`: the major zones of the map shown on the briefing minimap. Each numbered entry matches a numbered marker on the minimap texture.

**Formatting rule:** the vertical bar (pipe) `|` starts a new line in formatted text. `LegendMap` and `LegendMapInBrief` need it to match the formatting of Ubisoft's standard maps.

## Modes

`Modes` is an array holding the map's three supported game modes: Story, Disk Hunt and Deathmatch. Each entry configures one mode.

- **Index rule:** the order does not matter (Story can be `[0]`, `[1]` or `[2]`). But each index must match the same index in the strings array.
  - Example: if Story is `Modes[1]`, Story's text entries must also be at `[1]`.
  - The wiki's section and property list call the strings array `ModesStrings`. One sentence spells it `ModeStrings`.
- Add one entry per game mode:
  - `GM_Multi`: Story
  - `GM_DiskHunt`: Disk Hunt
  - `GM_DeathMatch`: Deathmatch

Fields of each `Modes` entry:

| Field | Meaning | Supported values |
|---|---|---|
| `GameModeId` | The game mode | `GM_Multi`, `GM_DiskHunt` or `GM_DeathMatch` |
| `Handicap` | **Disk Hunt only.** Number of active disks that spawn during the match | |
| `NumDisksMax` | **Disk Hunt only.** Maximum number of disks that can be selected as active, usually the total number of `SDisk` actors placed | |
| `RespawnTime` | Respawn time in seconds | **5-15** |
| `ScoreLimit` | **Disk Hunt only.** Number of disks needed to win | |
| `SentriesLives` | Lives of the **Mercenary** team | **0 (infinite) - 10** |
| `SpiesLives` | Lives of the **Spy** team | **0 (infinite) - 10** |
| `TimeLimit` | Match length in **minutes** | **0 (infinite) - 20** |

The Modes screenshot is image-only.

## ModesStrings

`ModesStrings` is an array of the text strings for the three supported modes. Each entry matches the **same index** in `Modes`.

- `ExplicationModeSentry`: the Mercenary team's main objective in this game mode.
- `ExplicationModeSpy`: the Spy team's main objective in this game mode.
- `ExpliModeInBriefSentry`: a detailed narrative of the map for the Mercenary team.
  - It usually covers the map's layout (referencing the minimap zones) and its key features.
  - It is written as if it were a briefing from the mission handler.
- `ExpliModeInBriefSpy`: the same, for the Spy team.
- `LegendModeInBriefSentry` / `LegendModeInBriefSpy`: **not displayed in game on PC**.
  - They only describe the game mode, and are identical for both teams on every map.
  - Leave them empty or copy the Ubisoft standard text:

| Game mode | `LegendModeInBriefSpy` / `LegendModeInBriefSentry` (Ubisoft standard text) |
|---|---|
| Story | The spies must accomplish a certain number of mission objectives. The objectives can be of three kinds. The Mercenaries must prevent them or eliminate the spies. |
| Disk Hunt | The spies must go through the level and collect a certain number of disks. The Mercenaries must prevent them or eliminate the spies. |
| Deathmatch | Both spies and mercenaries must eliminate the opposing team. |

## Textures

`SMapSettings` stores **4 textures**, used for the map thumbnail, the briefing screen and the loading screens.

> **NOTE:** prepare the textures as **DirectDraw Surface (`.dds`) with DXT compression** before importing them into the editor. Uncompressed RGBA8 **Targa (`.tga`)** also works, but is generally not recommended because the files are much larger than DXT `.dds`.

Import settings:
- **Package:** `<MapName>-i`. For the map file `AquaD.sdc`, enter `AquaD-i`.
- **Group:** leave blank.
- **Name:** one of `Menu`, `Briefing`, `LoadingATT0` or `LoadingDEF0`.

| Usage | Name | Resolution | Compression |
|---|---|---|---|
| Map thumbnail | `Menu` | 256x256 | DXT5 |
| Briefing screen | `Briefing` | 256x256 | DXT5 |
| Spy loading screen | `LoadingATT0` | 512x512 | DXT1 |
| Mercenary loading screen | `LoadingDEF0` | 512x512 | DXT1 |

Alpha:
- `Menu` and `Briefing` are usually imported as **alpha** textures (`bAlphaTexture=True`) with **DXT5**.
- If you forgot to enable **Alpha** on import, open the texture properties and set `bAlphaTexture=True` by hand.
- Tip: the `Menu` texture uses a specific **fade mask** around its edges. To match the Ubisoft standard, apply the SCCT Menu alpha mask to the thumbnail before importing. The mask is a 256x256 image and is **image-only** on the wiki.

## Saving

1. Complete all your Map Settings.
2. Save the texture package that holds your textures.
3. Save and exit the map in the editor. This generates the `SMapSettings` file at:

```
Packages\Textures\<MapName>-i.utc
```

> **TIP:** the editor may also create a file of the same name with the extension `-i.utx`. The game does not use it, so it is safe to ignore. Good practice is to delete the `-i.utx` and keep only the `-i.utc`.

## How the MCP tools apply this

Writing the settings:
- `map_settings` (and a spec's `settings` and `interface` sections via `level_build`) writes `MapName` in CAPS. `|` is kept as the line break.
- `Modes[]` and `ModesStrings[]` are index-aligned, with one entry per mode the map plays.
- Values are checked against the supported ranges: `RespawnTime` 5-15, `SpiesLives`/`SentriesLives` 0-10, `TimeLimit` 0-20.
- `LegendModeInBrief*` defaults to the Ubisoft standard text above.

Interface textures:
- `map_settings` with `interface` (bridge op `interface.texture`) imports `Menu`/`Briefing` (256, DXT5, alpha) and `LoadingATT0`/`LoadingDEF0` (512, DXT1) into `<Map>-i`.
- `image_convert` prepares power-of-two images first.

Order of operations:
- Map Settings needs the save → exit → reopen cycle. Run `map_file action:"save_as"`, then `editor_restart`, before `map_settings`.
- When SMapSettings cannot be written over the bridge, the tool says so and lists the manual steps above.

Release:
- `release` checks for `<Map>-i.utc` and reminds you to delete a stray `-i.utx`.
- Rules: `settings.missing`, `settings.ranges`, `settings.map-name-caps`, `settings.mode-parity`, `settings.modes-missing`, `interface.textures`
