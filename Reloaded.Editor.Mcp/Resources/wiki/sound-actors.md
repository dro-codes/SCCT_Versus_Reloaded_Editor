# Sound Actors

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Sound-Actors

SCCT Versus has several sound actors that play music or ambient sounds in response to events. The property sheets are image-only; the tables below are the text.

## SoundTrigger

Plays a sound when an event triggers it.

| Property | Description |
|---|---|
| `Sound` | The sound asset to play. |
| `SoundPitch` | Pitch of the sound. **`64.0` is the default (no change).** Higher values raise the pitch and lower values drop it. |
| `SoundRadius` | The farthest distance from the trigger at which the sound can be heard. |
| `SoundVolume` | Volume from `0` (silent) to `255` (maximum). |

## MusicTrigger

Plays a song when an event triggers it.

| Property | Description |
|---|---|
| `bInstigatorOnly` | If `True`, only the instigator hears the music. |
| `bToggle` | Each activation toggles the music ON or OFF. |
| `FadeInTime` | Fade-in duration of the song, in seconds. |
| `FadeOutTime` | Fade-out duration of the song, in seconds. |
| `Song` | The sound asset to play. |

## SAmbientSoundTrigger

For ambient sounds that are **triggered on and off**, rather than playing constantly as the standard `AmbientSound` actor does.

| Property | Description |
|---|---|
| `bInitialyOn` | If `True`, the sound is active when the level begins. This is the engine spelling: one `l`. |
| `bToggle` | Each activation toggles the sound ON or OFF. |

The sound itself is assigned in the `Sound` category, as on a standard `AmbientSound`.

## Related

- `scct://wiki/sound-browser`: packages, `.uas` streams, and Random/Sequence resources.
- Mover sounds (`OpeningSound` and others) are in `scct://wiki/movers`.
- Gameplay object sounds (`SGPOSound`) are in `scct://wiki/gameplay-objects`.
