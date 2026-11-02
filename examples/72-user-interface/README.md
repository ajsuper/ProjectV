# 72 — User interface

The player's UI ([plan](../../docs/plans/ui.md), U3): RmlUi documents beside the program, drawn over a
scene by the engine's user layer.

```bash
cd build/examples/user_interface && ./user_interface
UI_CAPTURE=/tmp/ui ./user_interface   # the menu open, frame 90 saved as /tmp/ui.tga, then it exits
```

M opens the menu (and frees the cursor); W/S/A/D, R/F and the mouse fly; F1 is the dev layer.

| File | What |
|---|---|
| `ui/hud.rml` | The frame rate, a greeting and a click count: `{{fps}}`, bound values. `pointer-events: none`, so the game keeps the mouse |
| `ui/menu.rml` | Buttons that send actions (`onclick="count 1"`), a slider for the sun's height (`data-value`), a crosshair checkbox (`data-checked`), a name field |
| `ui/ui.rcss` | This program's look, over the demos' shared `base.rcss` (staged as `shared_ui/`) |
| `main.cpp` | Binds `UiState`'s fields to the model "hud", opens both documents, handles the `UiAction`s |

**Edit them while it runs.** In the build folder, change `ui/ui.rcss` or a document and save: it
reloads within half a second. The next build copies the sources over them again -- copy back what
you keep.

The look is plain until U4 (blur, shadows, animation).
