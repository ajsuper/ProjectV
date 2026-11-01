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
| `installUi(app, renderInstance)` | Emplaces the `Host`, routes input each PreUpdate after `"platform"`, draws the layers as an overlay. After `graphics::installPlatform` |
| `Host` | The `world.ctx()` resource. `setLayer(level, layer)`, `layer(level)`, `route(input)`, `draw(size)` |
| `Level` | `Dev`, `User`: routing order (the dev layer is offered input first, and drawn on top) |
| `Layer` | One level's UI: `visible()`, `input(input, taken)` returning a `Capture`, `draw(view, size)` |
| `Capture` | What a layer took: `keyboard`, `mouse`. The host ORs them into `Input::wantCaptureKeyboard`/`Mouse` |
| `USER_VIEW`, `DEV_VIEW` | bgfx views 254 and 255: above any renderer's passes |

```cpp
projv::graphics::installPlatform(app, renderInstance);
projv::ui::installUi(app, renderInstance);
// in a game system:
if (!input.wantCaptureMouse && input.pressed(projv::MouseButton::Left)) fire();
```
