# Events

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Events

Events trigger actions in a map, such as:
- switching lights on or off
- raising alarms
- opening doors
- disabling a camera network

## Events & Tags

Two properties control how actors send and receive events:
- `Event`: **sends** an event to other actors.
- `Tag`: **receives** events sent by other actors.

When an actor sends an event, **every actor with a matching Tag** reacts to it. Event names can be any string. Use clear, descriptive names to make level logic easy to understand and debug.

The property sheet is image-only.

## Trigger / Untrigger

Actors respond to two kinds of event:
- **Trigger** activates or enables an actor.
- **Untrigger** deactivates or disables an actor.

Actors use them differently:
- Some react only to Trigger.
- Some react to both Trigger and Untrigger.
- Some react only to Untrigger.

The actor's **`TriggerMethode`** controls this (the wiki and engine spelling).

### Trigger Methods

The trigger-method list is image-only. The text describes three of them:

- **`TriggerToggle`**: the actor changes state each time it receives a Trigger.
  - Example: a button sends Trigger, and the machine toggles ON or OFF on every press.
  - Several switches can safely send the same Trigger without synchronisation problems.
- **`TriggerControl`**: Trigger turns the actor ON and Untrigger turns it OFF.
  - The actor always ends in the correct state, which is especially useful when several actors must stay synchronised.
- **`UntriggerOnly`**: the actor sends **only Untrigger** events. This is commonly used for reset behaviour.
  - Example: a normal switch sends Trigger to turn a machine ON, and a reset switch sends Untrigger to force it OFF, whatever its current state.

Summary:

| Switch type | Machine type | Behaviour |
|---|---|---|
| `TriggerToggle` | `TriggerToggle` | Each press toggles the machine ON or OFF |
| `TriggerControl` | `TriggerControl` | Trigger turns the machine ON, Untrigger turns it OFF |
| `TriggerToggle` + `UntriggerOnly` | `TriggerControl` | One switch turns the machine ON, another always turns it OFF |

## SMagicEvent

`SMagicEvent` is for scripted events. **One incoming event can fire several different events in response**, with options for delays, toggles, or set Trigger/Untrigger states.

Setup:
1. Create at least one `EventGroup` in the `Groups` section with the Add button.
2. Add one or more event entries to its property list.

| Property | Description |
|---|---|
| `Delay` | Time between the SMagicEvent receiving an event and the event being sent. |
| `Event` | Name of the event to send. One SMagicEvent can send several events. |
| `Type` | What to send: `Trigger`, `Untrigger`, `TriggerUntrigger` (alternates, starting with Trigger) or `UntriggerTrigger` (alternates, starting with Untrigger). |
| `ValidOn` | Whether this event is sent when the SMagicEvent receives a `Trigger` or an `Untrigger`. |
| `Repeat` | How many times the `EventGroup` can fire: `0` unlimited, `1` once, `2` twice, and so on. When the count runs out the group is **disabled permanently**. **Negative values disable it immediately.** |
| `Sequence` | If `True`, the group sends **one event per activation**, in order, instead of all at once. Each trigger sends the next event in the list, and the sequence loops back to the start after the last one. **It only advances when the next event's `ValidOn` matches the incoming trigger.** Otherwise nothing is sent and the position is held. |
| `StopActor` | An actor whose trigger acts purely as a **cancel**: it aborts any events not yet sent, and does not activate any group itself. |

The property sheet is image-only.

Engine names (from RE+ `tests/SMagicEventJson.md` and `SMagicEventSemantics.md`, not the wiki):
- In map JSON, `Type` and `ValidOn` are the enum names `EVT_Trigger`, `EVT_Untrigger`, `EVT_TriggerUntrigger` and `EVT_UntriggerTrigger`.
- A group is `{Sequence, SequenceIndex, Repeat, EventGroup:[{Event, Delay, Type, ValidOn}]}`. Every leaf is a string.
- Delays are 0-86400 s.
- Every action scheduled by one activation shares one time origin. Queued events with equal delays may run in any order, so use distinct delays where order matters.

## SMessageTrigger

Shows a **large HUD message to both the Spy and the Mercenary**. It is mainly used in sector maps, for example to announce "Go to Sector 2" when a new sector activates.

| Property | Description |
|---|---|
| `DisplayTime` | Seconds the message stays on the HUD. |
| `MessageMerc` | Text on the Mercenary HUD. |
| `MessageSpy` | Text on the Spy HUD. |
| `Priority` | Priority when several messages are queued. Higher values are shown first. |

## SShaker

Shakes the player camera, for big explosions or heavy impacts. Create at least one entry in the `ShakeEvents` array (`Add`). Each entry is a shake configuration that fires when a specific actor sends an event.

| Property | Description |
|---|---|
| `bShake` | Enables camera shake. Set `bShake=True` to allow shakes when the event sender matches. |
| `bTilt` | Enables camera tilt. Set `bTilt=True` to allow tilts when the event sender matches. |
| `EventSender` | The actor that must send the event. Events from any other actor are ignored. |
| `FadeOut` | Seconds over which the effect fades out after it is applied. |
| `speed` | How quickly the effect reaches its target value. Higher is faster. |
| `strenght` | The intensity of the effect. Higher is stronger. (The engine property name is spelled `strenght`.) |
| `Zone` | A `ZoneInfo` the effect applies to. A player outside that zone feels nothing, even when the sender matches. |

## SNotifyNoise

Makes noises that show on the **Mercenary's reticle**.

| Property | Description |
|---|---|
| `bEnabled` | Whether the actor is active when the level starts. Trigger/Untrigger events can also activate or deactivate it. |
| `fIntensity` | The noise's intensity. **0.01 or lower does not appear** on the Mercenary reticle. **Maximum 1.0**, which reaches the **~15 metre** detection range. Lower values shorten how far away the noise shows on the HUD. |
| `fTimeRateInSec` | Interval in seconds at which the noise updates or triggers on the reticle. |
