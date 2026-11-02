# ui

The UI: two layers over the frame, the developer's (Dear ImGui) and the player's (RmlUi), fed from
one input stream and drawn in one order. Namespace `projv::ui`. Built with `PROJV_BUILD_UI` (on by
default), which defines `PROJV_UI` for consumers. Plan: [ui.md](../plans/ui.md).

| Belongs in ui? | |
|---|---|
| ✅ `ui::installUi()`, the layers, their routing and panels | Drawing and routing a UI |
| ❌ a HUD's look, a menu's layout | The program's `ui/` folder |

```text
 PreUpdate: platform fills Input ──▶ ui: route input ──▶ dev layer ──▶ user layer ──▶ game (Input::wantCapture*)
 Render:    renderer's passes ──▶ endFrame: user layer (USER_VIEW) ──▶ dev layer (DEV_VIEW) ──▶ bgfx::frame
```

## `ui/ui.h`

[`include/ui/ui.h`](../../include/ui/ui.h): the host.

| Name | What |
|---|---|
| `installUi(app, renderInstance, options)` | Emplaces the `Host`, routes input each PreUpdate after `"platform"`, draws the layers as an overlay, installs the dev layer per `options`. After `graphics::installPlatform` |
| `UiOptions` | `devLayer` (unset: on in a `PROJV_DEV_UI_DEFAULT` build, else only with `--dev-ui`), `devToggle` (F1), `devShown` (unset: `PROJV_DEV_UI_SHOWN=1`) |
| `setCommandLine(argc, argv)`, `devUiRequested(argc, argv)` | `--dev-ui`: call `setCommandLine` from `main` |
| `Host` | The `world.ctx()` resource. `setLayer(level, layer)`, `layer(level)`, `route(input)`, `draw(size)` |
| `Level` | `Dev`, `User`: routing order (the dev layer is offered input first, and drawn on top) |
| `Layer` | One level's UI: `visible()`, `input(input, taken)` returning a `Capture`, `draw(view, size)` |
| `Capture` | What a layer took: `keyboard`, `mouse`. The host ORs them into `Input::wantCaptureKeyboard`/`Mouse` and removes them from `Input` (`maskKeyboard`, `maskMouse`) |
| `USER_VIEW`, `DEV_VIEW` | bgfx views 254 and 255: above any renderer's passes |

```cpp
projv::graphics::installPlatform(app, renderInstance);
projv::ui::installUi(app, renderInstance);
// in a game system:
if (!input.wantCaptureMouse && input.pressed(projv::MouseButton::Left)) fire();
```

## `ui/dev.h`

[`include/ui/dev.h`](../../include/ui/dev.h): the developer layer's API. Includes `imgui.h`: a panel
is ImGui code. Guide: [Add a dev panel](../guides/dev-panel.md).

| Name | What |
|---|---|
| `addPanel(app, name, fn)` | A window shown with the dev layer, built in Render while it is shown; kept and free when it is not |
| `shown(app)` | Whether the dev layer is installed and shown |
| `initRenderer(view)`, `shutdownRenderer()`, `renderDrawData(drawData)` | ImGui on bgfx, for the current context: the engine's embedded `vs_imgui`/`imgui` shaders. For a program with an ImGui context of its own (the scene editor) |
| `textureID(handle)` | An engine texture as an `ImTextureID`, for `ImGui::Image` |

## `ui/dev_layer.h`

[`include/ui/dev_layer.h`](../../include/ui/dev_layer.h): the dev layer as the host holds it.

| Name | What |
|---|---|
| `DevLayer(app, renderInstance)` | Its own ImGui context, fed from `Input`, drawn in `DEV_VIEW`. `renderInstance` nullptr is headless (tests) |
| `setShown(bool)`, `visible()` | F1's two states. Shown, it releases a captured cursor; hidden, it captures it again |
| `keepCursor()` | While shown, releases a cursor the program captured again; `installUi` runs it each frame |

Panels it always has: **Frame** (frame time and graph, each system's time by stage, from
`Application::systemSeconds`) and **Log** (the last 4000 lines, by category, filterable).
