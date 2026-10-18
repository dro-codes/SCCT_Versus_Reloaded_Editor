# Fish

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Fish

SCCT Versus added several fish actors: **`SFish`**, **`SFish_Big`** and **`SFish_Follow`**. They were mainly used in the **Aquarius** map, in its aquarium tanks.
- Fish use **no engine physics or AI**. Simple scripted movement swims them around their assigned area.

The property sheets are image-only; the tables below are the text.

## SFish

The standard fish. It swims around the area set by its **movement box**, changing direction and speed at random over time.
- The movement box is **centred on the actor's placed location**.
- Fish with `bPilote=False` start at a **random position** inside this area.
- Rotation values use Unreal rotation units: **`65536` = `360°`**.

| Property | Description |
|---|---|
| `AnimsFish` | Animation names: `[0]` plays while swimming forward, `[1]` while turning. |
| `bPilote` | Marks this fish as a **leader** for `SFish_Follow` actors. Pilot fish start at their placed editor position. |
| `fAngleVmin` | Minimum random pitch angle while swimming. |
| `fAngleVmax` | Maximum random pitch angle while swimming. |
| `fEloignementMaxX` | **Half-size** of the movement box along X, in Unreal units. |
| `fEloignementMaxY` | **Half-size** of the movement box along Y, in Unreal units. |
| `fEloignementMaxZ` | **Half-size** of the movement box along Z, in Unreal units. |
| `fRotHmin` | Minimum horizontal turn rate, in rotation units per second. |
| `fRotHmax` | Maximum horizontal turn rate, in rotation units per second. |
| `fSpeedMin` | Minimum swim speed, in Unreal units per second. |
| `fSpeedMax` | Maximum swim speed, in Unreal units per second. |
| `fTempsAvanceMin` | Minimum length of the forward swimming phase, in seconds. |
| `fTempsAvanceMax` | Maximum length of the forward swimming phase, in seconds. |
| `fTempsTourneMin` | Minimum length of the turning phase, in seconds. |
| `fTempsTourneMax` | Maximum length of the turning phase, in seconds. |
| `me` | Unused; nothing in the code references it. |

## SFish_Big

A larger variant for slower creatures; Aquarius uses it for manta rays, dolphins and sharks.
- It has the same properties as `SFish`, with **smoother movement** designed for larger creatures.
- It adds no properties.

## SFish_Follow

A follower fish, for **formations**. It follows a leader `SFish` that has `bPilote=True`.
- Make the formation by placing the followers around the pilot in the editor. At run time each follower keeps its position relative to the leader.

> **IMPORTANT:** an `SFish_Follow` with no valid `Pilot`, or whose pilot does not have `bPilote=True`, **does not move**.

| Property | Description |
|---|---|
| `AnimsFish` | Animation names: `[0]` plays while moving toward the formation position, `[1]` while idle. |
| `fRefreshTimeMin` | Minimum time between formation updates, in seconds. |
| `fRefreshTimeMax` | Maximum time between formation updates, in seconds. Lower values update more often, so followers stay closer to the leader. |
| `Pilot` | The leader this fish follows. It must be an `SFish` or `SFish_Big` with `bPilote=True`. |

## How the MCP tools apply this

`environment` with `fish[{id, class, at, properties}]` (spec `environment.fish`) places generic actors. `class` defaults to `SFish`; set `SFish_Big` or `SFish_Follow` explicitly.

The compiler passes `properties` through, so write the wiki values yourself:
- **Movement box:** use half-sizes. `fEloignementMaxX/Y/Z` = tank inner size / 2, centred on `at`.
- **Leader:** set `bPilote: "True"`.
- **Follower:** set `Pilot: {"$ref": leaderId}`, and place it around the pilot in its formation position.
- **Animations:** `AnimsFish` names come from existing Ubisoft animations (Aquarius); there is no Animation Browser.

`map_validate` checks that every follower's pilot is an `SFish`/`SFish_Big` with `bPilote True`.
- Rules: `fish.pilot`
