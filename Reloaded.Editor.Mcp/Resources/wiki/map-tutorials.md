# Map Tutorials

Source: https://github.com/AllyPal/SCCT_Versus_Reloaded_Editor/wiki/Map-Tutorials

The game has a tutorial system, used in **Visit a Map**, that guides players through your level and shows tips along the way. It works by placing:
- a **path** for the player to follow, and
- **areas** where tutorial messages appear on the HUD.

## SHelpPath

Defines the visible path that guides the player through the map during a tutorial.
- It only appears when the level is played in **Visit a Map** under the **Tutorials** section.
- It starts as `DT_Sprite`. To show it as a Static Mesh, open its properties, go to `Display`, change `DrawType` from `DT_Sprite` to **`DT_StaticMesh`**, and assign a `StaticMesh` in the same category.
- No other configuration is needed. Place `SHelpPath` actors along the route the player should follow, to form the path.
- They usually make a visible path across the floor, with **smaller markers** where tutorial tips appear.

## SZoneTips

Shows tutorial messages on the HUD. Like `SHelpPath`, it appears only in **Visit a Map** under **Tutorials**.

Visibility in the editor and in game:
- By default its `DrawType` is **`DT_None`**, so it is **invisible in the editor**. To select and place it easily, set `DrawType` to `DT_Sprite` in `Display`.
- The sprite must not show in game: in the **`Advanced`** category, set **`bHidden=True`**. The actor stays visible in the editor but does not appear during play.

> **TIP:** if you lose an invisible `SZoneTips` after deselecting it, the `Search for Actors` button in the top toolbar can find and reselect it.

The property sheet is image-only.

| Property | Description |
|---|---|
| `bShowOnlyWithIngameHelp` | **Set it to `True`**, so the actor appears only in **Visit a Map** tutorials, not in normal multiplayer matches. |
| `bStayPermanently` | **Set it to `False`**, so the actor is active only in tutorial mode. |
| `sMessage_ATT` | The tutorial message shown to **Spies**. |
| `sMessage_DEF` | The tutorial message shown to **Mercenaries**. |

- Usually the same message works for both teams. Ubisoft mostly wrote **Visit a Map** tutorials from the Spy's point of view, since both teams share the same tutorial path.
- **TIP:** to see the trigger's collision radius and height, right-click in the viewport, hover over `Actors` and enable `Radii View`. A selected `SZoneTips` shows its collision as a red outline. Resize the trigger with `CollisionRadius` and `CollisionHeight` in the `Collision` category.

> **NOTE:** `sMessage_ATT` and `sMessage_DEF` are **arrays of two elements** (`[0]` and `[1]`). The game joins them with a space when it shows the message. In practice, put the whole message in `[0]` and leave `[1]` empty: the text length is not limited.

## SEndZoneTip

You will probably never need this actor. It behaves much like `SZoneTips`.
- It starts invisible: set `DrawType=DT_Sprite` to see and select it in the editor.
- As with `SZoneTips`, set `bHidden=True` in `Advanced`, so the sprite shows in the editor but not in game.

| Property | Description |
|---|---|
| `EndGameDelay` | When a player enters the actor's collision radius, the game ends after this delay, in seconds. |

- It also has the tutorial message properties (`sMessage_ATT`, `sMessage_DEF`) and the visibility settings (`bShowOnlyWithIngameHelp`, `bStayPermanently`).
- Assigning a message is **not recommended**, because the game ends shortly after the actor is triggered.
- Ubisoft used it in their tutorial maps to end the session automatically, without the player completing an objective. It is unlikely to help a typical multiplayer map.

The property sheet is image-only.
