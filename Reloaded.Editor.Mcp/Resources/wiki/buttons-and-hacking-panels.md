# Buttons and Hacking Panels

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Buttons-and-Hacking-Panels

The **`SInterrupteur`** actor (French for "switch") is used for **every button and hacking panel**. It is the interactive object that triggers events. The property sheet is image-only; the table below is the text.

| Property | Description |
|---|---|
| `bInitiallyUsable` | Whether the interaction can be used when the level starts. With `False`, the object cannot be used until something triggers it through its `Tag`. |
| `bInteractInFront` | **Usually recommended.** `SInterrupteur` objects are typically on walls and should not be usable from behind. |
| `bMaintainsSwitch` | A Spy must **hold** the button to keep the switch active. The Spy stays interacting until moving away, which sends an **Untrigger**. Common for cooperative mechanics, such as one Spy holding the switch while another enters. **Works only for Spies.** |
| `bTriggerOnceOnly` | After one use the switch becomes inactive and cannot be used again. |
| `fInteractionHeight` | Height of the interaction prompt. **Recommended: `120.0`.** Enhanced maps try to stay consistent across levels. |
| `fInteractionRadius` | Distance at which the interaction prompt becomes available. **Recommended: `60.0`.** Enhanced maps try to stay consistent across levels. |
| `iAttenteAvantActivation` | Delay in seconds between a player triggering the interaction (pressing the button, or finishing the hack) and the object executing its event. Rarely used; an `SMagicEvent` is usually better for this kind of delay. |
| `iAttenteAvantDesactivation` | Delay in seconds after the interaction executes before it resets or deactivates. Only relevant when `iAttenteAvantActivation` is greater than `0`. |
| `iHackingTime` | Hacking time in seconds. At **`0`**, both Spies and Mercenaries play a **button press** animation. **Above 0**, only the Spy plays a hacking animation, and Mercenaries stand still during the interaction (they have no hacking animation). **Mercenaries were not meant to hack panels (`iHackingTime > 0`)**; it technically works, but treat it as unsupported. |
| `InteractionSound` | Played when the interaction is used. |
| `iPriority` | Order of interactions on the HUD. Higher numbers are listed higher. |
| `iReusableTime` | Seconds before the switch can be used again after being triggered. |
| `sDescriptionText` | Text in the interaction prompt when the player approaches, for example "Open". |
| `sWarnHackingText` | Text shown to Mercenaries when a hack begins. Left blank, the default message **"Hacking in progress..."** is used. |
| `TriggerMethode` | Which trigger method the switch sends when activated. See `scct://wiki/events`. |
| `UsableBy` | Which team can use the switch: `U_DEFAULTUSABILITY`, `U_BOTH`, `U_SPAWNDEFENSE` (Mercenaries only), `U_SPAWNOFFENSE` (Spies only) or `U_NO_ONE` (disabled). |
| `UseRelativeLocation` | Offset of where the player stands while interacting. **Button meshes often use `X=55.0 Y=0.0 Z=0.0`; hacking panels about `X=39.5 Y=3.0 Z=0.0`.** |
| `UseRelativeRotation` | Rotation offset of the interaction position. **Usually `32768`**, which makes the player face the object. Rarely needs changing. |

Elevator switches: set `TriggerMethode` to `TriggerToggle`, and set initial usability by the lift's starting floor. See `scct://wiki/elevators`.
