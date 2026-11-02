# ui

The UI: two layers over the frame, the developer's (Dear ImGui) and the player's (RmlUi 6.3, FreeType 2.14.3), fed from
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
| `UiOptions` | `devLayer` (unset: on in a `PROJV_DEV_UI_DEFAULT` build, else only with `--dev-ui`), `devToggle` (F1), `devShown` (unset: `PROJV_DEV_UI_SHOWN=1`), `userReload` (unset: on with `PROJV_DEV_UI_DEFAULT`) |
| `installUi` | Also installs the user layer (`ui/user.h`) |
| `setCommandLine(argc, argv)`, `devUiRequested(argc, argv)` | `--dev-ui`: call `setCommandLine` from `main` |
| `Host` | The `world.ctx()` resource. `setLayer(level, layer)`, `layer(level)`, `route(input)`, `draw(size)` |
| `Level` | `Dev`, `User`: routing order (the dev layer is offered input first, and drawn on top) |
| `Layer` | One level's UI: `visible()`, `input(input, taken)` returning a `Capture`, `draw(view, size)` |
| `Capture` | What a layer took: `keyboard`, `mouse`. The host ORs them into `Input::wantCaptureKeyboard`/`Mouse` and removes them from `Input` (`maskKeyboard`, `maskMouse`) |
| `USER_VIEWS_FIRST`..`USER_VIEW`, `DEV_VIEW` | bgfx views 128-254 (the user layer, a new one at each framebuffer switch) and 255: above any renderer's passes |

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

## `ui/user.h`

[`include/ui/user.h`](../../include/ui/user.h): the user layer, what the player sees. RmlUi documents
(`.rml`, `.rcss`) beside the program; no RmlUi type in the header. Guide:
[A HUD from a document](../guides/ui-document.md).

| Name | What |
|---|---|
| `open(app, name, path, DocumentOptions)`, `openFromMemory(app, name, rml, ...)` | Loads a document under `name`. `DocumentOptions`: `shown` (true), `modal` (takes all input while shown), `fadeOut` (seconds it is still drawn after `hide`, for a transition out) |
| `show`, `hide`, `close`, `isOpen`, `isShown` | By name |
| `loadFont(app, path, fallback)` | A TTF/OTF face for every document |
| `model(app, name)` -> `Model` | A data model: `.bind(name, &value)` for `float`, `int`, `bool`, `std::string`, before opening a document that names it (`data-model`). Bound values are compared each frame and redrawn when they change; controls bound to one (`data-value`, `data-checked`) write it |
| `UiAction` (in `projv::ui`) | The event an element's `on<event>="name arg..."` sends: `document`, `name`, `args`, `event` |

```cpp
ui::user::loadFont(app, "shared_ui/fonts/LatoLatin-Regular.ttf");
ui::user::model(app, "hud").bind("score", &score);
ui::user::open(app, "hud", "ui/hud.rml");
app.events().on<ui::UiAction>([](const ui::UiAction& a) { if (a.name == "pause") pause(); });
```

| Input it takes | When |
|---|---|
| the mouse | over an element that takes pointer events (`pointer-events: none` on a HUD's body gives the rest to the game) |
| the keyboard | while a text field (`input type="text"`, `textarea`) has focus |
| both | while a `modal` document is shown |

**Fades are style.** A shown document's body has the class `ui-shown`, added the frame after it is
shown (so a transition starts from the hidden state) and removed on `hide`:

```css
body { opacity: 0; pointer-events: none; transition: opacity 0.35s quadratic-in-out; }
body.ui-shown { opacity: 1; pointer-events: auto; }
.panel { backdrop-filter: blur(0dp); transition: backdrop-filter 0.4s cubic-out; }   /* composited apart */
.ui-shown .panel { backdrop-filter: blur(18dp); }
```

A fading document is not modal. A backdrop filter is not dimmed by the element's opacity, so it
fades by its own radius.

A dev build (`UiOptions::userReload`, on with `PROJV_DEV_UI_DEFAULT`) reloads a document when its
`.rml`, or an `.rcss` beside it, changes; shown documents stay shown.

## `ui/user_layer.h`

[`include/ui/user_layer.h`](../../include/ui/user_layer.h): the user layer as the host holds it.
`installUi` makes it.

| Name | What |
|---|---|
| `UserLayer(app, renderInstance, reloadOnChange)` | An RmlUi context fed from `Input`, drawn through the engine's `vs_rmlui` and `rmlui*` shaders: geometry, textures (PNG, TGA, JPEG through bimg), scissor, clip masks, transforms, layers, filters and gradients. `renderInstance` nullptr is headless: a recording render interface (tests) |
| `visible()` | Whether any document is shown |
| `drawCallsRecorded()`, `elementText(document, id)`, `elementBox(document, id, ...)` | For tests: what the headless interface drew, an element's text and border box |

What a style sheet can use, drawn by the bgfx render interface (RmlUi's GL3 backend, ported):

| | |
|---|---|
| `filter`, `backdrop-filter` | `opacity`, `blur`, `drop-shadow`, `brightness`, `contrast`, `invert`, `grayscale`, `sepia`, `hue-rotate`, `saturate` |
| `box-shadow`, `mask-image` | through layers |
| `decorator` | `linear-gradient`, `radial-gradient`, `conic-gradient`, and their `repeating-` forms (up to 16 stops); images |
| `transition`, `animation`, `transform` | RmlUi's own, on the layer's clock |

```text
 renderer's window passes ──▶ frame texture (while a document is shown)
 user layer: base layer ◀─ frame texture; documents drawn into it (layers, filters: views 128..)
             base layer ──▶ window (replaces it)      dev layer ──▶ view 255
```

A backdrop filter blurs the scene because the base layer starts from the frame texture. A program
that draws its own frame without `performRenderPasses` gets the UI blended over the window instead,
and a backdrop sees only the UI under it. OpenGL is not handled (its render targets are bottom-up).
Decorators that run a program's own shaders are not built.
