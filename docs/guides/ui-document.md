# A HUD from a document

The player's UI as RmlUi documents beside the program: values bound in, actions out as events.
Header: [`ui/user.h`](../../include/ui/user.h). Concepts: [The UI](../concepts/ui.md). Example:
[72 user-interface](../../examples/72-user-interface/README.md).

## Lay it out

```text
build/examples/<program>/
  ui/hud.rml          the document
  ui/ui.rcss          its style
  shared_ui/          the demos' base.rcss and fonts (projv_add_example(... SHARED_UI))
```

```cmake
projv_add_example(my_game
    SOURCES    main.cpp
    ASSET_DIRS ui
    SHARED_UI
)
```

## Write the document

```xml
<rml>
<head>
    <link type="text/rcss" href="../shared_ui/base.rcss"/>
    <link type="text/rcss" href="ui.rcss"/>
</head>
<body class="hud" data-model="hud">
    <p>Score {{score}}</p>
    <button onclick="pause">Pause</button>
</body>
</rml>
```

- `data-model` names the model whose values `{{score}}` reads.
- `on<event>="name args"` sends a `UiAction` -- `onclick`, `onchange`, `onsubmit`.
- A HUD's body sets `pointer-events: none` (base.rcss does not), so the mouse stays the game's
  except over what takes it.
- RmlUi has no built-in style: `base.rcss` makes `div` and `p` blocks and styles the controls.

## Bind and open

```cpp
const std::string dir = projv::core::executableDirectory().string();
ui::user::loadFont(app, dir + "/shared_ui/fonts/LatoLatin-Regular.ttf");
ui::user::model(app, "hud").bind("score", &game.score);   // before opening
ui::user::open(app, "hud", dir + "/ui/hud.rml");
ui::user::open(app, "menu", dir + "/ui/menu.rml", {.shown = false, .modal = true});

app.events().on<ui::UiAction>([&](const ui::UiAction& a) {
    if (a.name == "pause") ui::user::show(app, "menu");
});
```

- **The variables are yours.** Bind them once; change them as the game runs. The layer compares them
  each frame.
- **Controls write them.** `<input type="range" data-value="volume"/>`, `<input type="checkbox"
  data-checked="vsync"/>`, `<input type="text" data-value="name"/>`.
- **Check what the UI took** before acting on input: `input.wantCaptureMouse`, `wantCaptureKeyboard`.

## Edit it live

In a dev build, save the `.rml` or an `.rcss` beside it while the program runs: the document reloads
within half a second, shown as it was.
