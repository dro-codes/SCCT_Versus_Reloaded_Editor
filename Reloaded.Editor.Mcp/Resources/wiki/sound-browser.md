# Sound Browser

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Sound-Browser

The Sound Browser stores, organises and configures every sound a map uses.
- Each sound lives in a package (for example `Amb_Aqua`) and can be sorted into groups such as `Ambiances`.
- From the browser you can preview sounds, edit their properties, and create resources that control how sounds are chosen and played.

The browser window is image-only.

## Sound properties

Right-click a sound and choose `Properties` for its **Flags** and override options.

| Flag | Description |
|---|---|
| `2D` | Plays the sound in 2D: it is not at a point in the world, and always sounds as if it plays in your ears, wherever you are in the level. |
| `Loop` | The sound loops automatically. |
| `Stream` | Marks the sound as a **stream**. The Unreal package holds only a **stub** (a reference, not the audio), and the audio is encoded as **Ogg Vorbis inside the `.uas` package**. See the tip below. |
| `Xbox HD Stream` | Ignore on PC. The Xbox version kept some streams in `.hds` files, which PC does not use. |
| `4.0/5.1 Surround` | Ignore for now; it is not expected to be used on PC. |
| `Override radius` | Hard-codes a radius for this sound. An actor whose `Sound` is this sound always has its `SoundRadius` forced to this value, even if the map gives the actor a different radius. |
| `Override volume` | Fine-tunes the volume in dB. `0` is neutral; lower is quieter, down to **`-96` for silence**. |

> **TIP:** this is what happens to a streamed WAV:
> 1. Import the WAV, set its flag to `Stream`, and save. The `.uax` package **no longer stores the audio**.
> 2. On **Build UAS**, the `.uax` points to the sound as a stream, and the audio goes into the `.uas` package.
>
> Use this for larger ambient sounds tied to a major zone, such as a long ambient loop for an outdoor courtyard, or for a music track.

## Resources

Resources decide *how* sounds are picked and played.
- Create one with `File` → `New`. The **New Resource** dialog asks for a `Name` and a type: `Random`, `Sequence` or `Switch`.
- Each resource window is image-only.

### Random

- A pool of sounds, each with a `Weight`. Each play picks one sound at random by weight, so heavier sounds play more often.
- Unassigned weight is the **Probability of silence**, shown at the bottom of the window.
- **Set Equal Probabilities** spreads the weight evenly.
- Example: `aqua_ecranCracA` 40, `aqua_ecranCracB` 40 and `aqua_ecranCracC` 20. The first two play twice as often as the third.

### Sequence

- Plays a list of sounds in order. Each entry references a sound in the package and sets how it repeats:
  - the `Repeat` column plays it a set number of times in a row;
  - the `Timed Loop` column keeps it repeating for a number of seconds.
- **Up** / **Down** reorder the list.
- Example: `aqua_Punch` repeating for `2s`, then `aqua_fumeAigu` repeating for `10s`.

### Switch

Swaps sounds by **surface type**. Map building does not need it, so you can safely ignore it. The developers used it to change footstep and bullet-impact sounds depending on the surface walked on or shot.

## UAS packages

Ubisoft's custom sound package, **`.uas`**, is used alongside the normal Unreal `.uax` sound packages.
- `.uax` still holds most regular sounds, including the short ambient sounds usually played by an `AmbientSound` actor.
- **Longer ambient tracks and the level's music** are in `.uas` packages, encoded as **Ogg Vorbis**.

Tooling:
- The UnrealEd that ships with the game cannot create or modify `.uas`.
- **The Reloaded Editor can build `.uas` packages**, so a map no longer has to reuse a stock one.

How the engine finds it:
- When a map loads, the engine looks in the `Sounds` directory for **the `.uas` whose name matches the map's filename**. For `AquaD.sdc` it loads `Sounds\AquaD.uas`.
- **Only one `.uas` package can be used per map**, because the engine looks only for that single package.

### Building a UAS package

1. Import the audio with `File` > `Import` and pick the WAV.
2. Open its properties, set the flag to `Stream`, and save. The `.uax` now stores a stub instead of the audio.
3. In the Sound Browser's `File` menu, choose **`Build UAS`**.
4. When asked which packages to include, tick the map's own ambient package **`Amb_<mapname>`** (such as `Amb_Aqua`) **and `Interface`**.
   - The editor encodes the streamed sounds as Ogg Vorbis into the `.uas` named after the map.
   - The `.uax` keeps the references to them.

> **TIP:** **always include `Interface`, and always build a `.uas`**, even when the map has no streamed audio of its own. `Interface` holds the objective sounds and the win/lose music, which every map needs.

5. Choose the Ogg Vorbis **quality**.
   - The default **`5.0`** matches what Ubisoft used, and is the safe choice.
   - **Going lower is not recommended.** Higher costs file size.
6. Press OK to build the UAS.

The dialogs are image-only.
