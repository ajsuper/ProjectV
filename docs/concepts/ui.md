# The UI

Two layers over the frame, drawn by the engine rather than by each program
([plan](../plans/ui.md)). Reference: [ui](../reference/ui.md).

| Layer | Who sees it | For | Library |
|---|---|---|---|
| **Dev** | whoever runs a dev build (F1), or a release build with `--dev-ui` | frame and system timings, the log, a program's panels, tools | Dear ImGui |
| **User** | the player | HUD, menus, settings | RmlUi (not built yet: ui.md U3) |

## One input stream, one order

```text
 PreUpdate   platform: GLFW -> Input
             ui: dev toggle (F1)
             ui: route input   dev layer -> user layer -> what is left is the game's
 Render      the renderer's passes -> endFrame: user layer (view 254), dev layer (view 255) -> bgfx::frame
```

- **Routing.** Each frame `Input` is offered to the dev layer first (when shown), then the user
  layer. What a layer takes -- the mouse over a panel, the keyboard in a text field -- is removed from
  `Input` before the game reads it, and `Input::wantCaptureMouse`/`Keyboard` say so. A click on a panel
  never also fires a gun.
- **The cursor.** Showing the dev layer releases a mouse-look capture so the panels can be reached.
  A click on the scene captures it again and plays on with the panels still up; a captured cursor is
  the game's, and the panels see no mouse until it is released (Esc, in the examples). Hiding the
  layer captures it again if showing released it.
- **Drawing.** The layers draw after the renderer's passes and before the frame is submitted, through
  `RenderInstance::addOverlay`, in bgfx views above any renderer's. A program that submits its own
  frame ends it with `graphics::endFrame` instead of `bgfx::frame` so they still draw.

## Mechanism, not policy

The engine ships the layers, routing, panels and backends; no look. A program's HUD and menus are
data beside it (RmlUi documents in its `ui/` folder), as its renderer is.

## Determinism

Nothing in the UI runs inside a simulation step. A UI action is input, like a key, and becomes a
command the same way (ui.md D7), so a networked or recorded session is unaffected by what a player
had open.
