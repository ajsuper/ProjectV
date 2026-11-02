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
| `Meter` | A reading that holds still: `add(value, seconds)`; `average()`, `min()`, `max()` over the last complete period (`setPeriod`, 0.5 s), republished once a period; `ready()`, `last()`, `samples()`. The Frame panel reads every timing through one |
| `editable(app, renderer, uniform, &value, EditRange)` | Lets the Renderer panel edit a program's own variable (`float`, `int`, `bool`, `vec3`, `vec4`), shown on `uniform`'s row while `renderer` is active. `EditRange`: `label`, `min`/`max` (a slider; equal: a drag), `speed`. Unbound uniforms are read-only |
| `forgetEditables(app, renderer)` | Drops `renderer`'s bindings, before it or the variables go |
| `opaqueImage(handle, size, uv0, uv1)` | `ImGui::Image` drawn without blending: a target whose alpha is data |
| `initRenderer(view)`, `shutdownRenderer()`, `renderDrawData(drawData)` | ImGui on bgfx, for the current context: the engine's embedded `vs_imgui`/`imgui` shaders. For a program with an ImGui context of its own (the scene editor) |
| `textureID(handle)` | An engine texture as an `ImTextureID`, for `ImGui::Image` |

## `ui/dev_layer.h`

[`include/ui/dev_layer.h`](../../include/ui/dev_layer.h): the dev layer as the host holds it.

| Name | What |
|---|---|
| `DevLayer(app, renderInstance)` | Its own ImGui context, fed from `Input`, drawn in `DEV_VIEW`. `renderInstance` nullptr is headless (tests) |
| `setPanelOpen(name, open)`, `windowsDrawn()` | The Panels menu's toggles, and what the last draw built (tests). `PROJV_DEV_UI_OPEN=Physics,Renderer` opens windows from the start |
| `setShown(bool)`, `visible()` | F1's two states. Showing releases a captured cursor, hiding captures it again. While shown the program may capture it (a click on the scene) and play on; the panels then see no mouse until it is released (Esc) |

Panels it always has:

| Panel | What |
|---|---|
| **Frame** | Every reading a `Meter`: average, min and max over the period (0.25-2 s), a max well over the average in orange; `pause` freezes it. **CPU**: frame time, a stacked graph by stage (and time in no system), each stage's and system's time (`Application::systemSeconds`). **GPU**: GPU frame time, a stacked graph by pass, and the active renderer's passes in order -- name, target size, time, share -- then any other view (the UI's). **bgfx**: backend, draws, render-thread submit and waits, memory |
| **Log** | The last 4000 lines, by category, filterable |
| **Physics** | `PhysicsStats`: step time, steps and ms a frame (graphed), bodies, awake, debris; the counters that should stay 0 (dropped contacts, refusals, non-finite recoveries, left the world) in red when not. Only with physics |
| **Network** | A server: each client's round trip, loss, KiB/s each way (sent, stacked by client), inputs waiting, entities. A client: snapshots a second, corrections, mismatches. Only in a session |
| **Renderer** | The active renderer: its passes (target, size, GPU time), its uniforms read-only with the program's `editable` controls under them, its targets as thumbnails (click: full size) |
| **Entities** | Every entity by authored name, filterable by name, id or component type; the selected one's component types (EnTT's names) and its registered components' JSON. Read-only |

A menu bar (**Panels**) opens and closes each; Frame, Log and a program's panels start open, the
four above closed. A panel whose subject is absent has no window.

The graphs keep 240 columns (frames; GPU frames for the GPU), scaled to the 97th percentile, so a
hitch is cut and marked red rather than flattening the rest; guides at 120, 60 and 30 fps. Hover
one for that column's breakdown. Sampling runs every frame the layer is shown, the Frame window open
or not. While shown, the layer turns on bgfx's profiler (`BGFX_DEBUG_PROFILER`, per-view GPU
timestamps) and turns it off when hidden: a program that sets it itself (the `*_GPU` benchmarks) sets
it again after. A pass's name is its shader's file name (`BGFXDependencyGraph::name`, given to
`bgfx::setViewName` each frame it draws), numbered when one shader runs in several passes.
