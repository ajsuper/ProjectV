# UI: a developer layer and a user layer

Status: **in progress**: approved 2026-11-01 (RmlUi, D5; one shared demo style sheet, D13); U0 and U1 built 2026-11-01, U2 next. Follows [shader-library.md](shader-library.md): the
UI's shaders are engine shaders (its S0) and its blur is the library's filters (its S1).

**Mechanism, not policy** (as in the shader library): the engine provides the two layers, the render
backends, routing, binding and panels; it ships no look. The user layer's default style sheet is a
reset, nothing more -- every look lives in a program's `ui/` folder.

Goal: two kinds of interface, drawn by the engine rather than by each program.

| Level | Who sees it | What it is for | Library |
|---|---|---|---|
| **Developer** | whoever runs a dev build | stats, inspectors, tweakables, the log; tools like the scene editor | Dear ImGui (already in `external/imgui`, docking branch) |
| **User** | the player | HUD, menus, pause, settings, scoreboards, prompts | RmlUi (recommended, D5) |

## Why: where it stands (2026-10-27)

- **The engine draws no text.** ImGui exists only inside the scene editor, with its own bgfx backend
  (`imgui_impl_bgfx.h`, since promoted to [src/ui/dev/imgui_bgfx.cpp](../../src/ui/dev/imgui_bgfx.cpp))
  and shaders staged in its renderer folder. No other program can use it without copying both.
- **Player-facing UI is a fragment shader.** The party and playground demos draw their HUDs in
  `hudShaders/hud.frag` (266 and 279 lines): a 3x5 dot font, 24 characters a line packed six bits a
  character into two `vec4` uniforms, two dozen named uniforms (`hudTop0` ... `hudAim`). Every new
  line of text is a new uniform pair; there is no layout, no lower case, no mouse.
- **The networking overlay waits on this.** [networking.md](networking.md): "the engine draws no
  text ... an overlay waits on physics R13 or a text pass". Stats are logged instead.
- `Input` already has `wantCaptureKeyboard` / `wantCaptureMouse`, left for the program to fill.
- `renderConstructedRenderer` calls `bgfx::frame()` itself, so nothing can draw after the renderer's
  passes without owning the whole frame as the editor does.

## Decisions

**D1. One engine module, `ui/`, two levels on one host.** `include/ui/`, `src/ui/`, built as
`projv_ui` (optional, `PROJV_BUILD_UI`, on by default). `ui::installUi(app, renderInstance)` adds the
host: it feeds both levels from the platform's events, orders their draws, and answers `Input`'s
capture flags. *Why:* both levels need the same three things (events in, a view on top, capture out);
doing them once is what keeps a click from reaching the menu, the inspector and the game at once.

**D2. Layering, as Jolt and ENet are done.** Only `src/ui/dev/` includes ImGui; only `src/ui/user/`
includes RmlUi; no public header names either library's types except `ui/dev.h`, which includes
`imgui.h` on purpose (panel code *is* ImGui code). `utils/`, `data_structures/`, `net/` never include
`ui/`. The `layering` ctest gains these rules. *Why:* the user layer's library is the decision most
likely to be revisited; keeping it behind engine types means revisiting it touches `src/ui/user/` only.

**D3. A draw hook after the renderer, before `bgfx::frame()`.** `renderConstructedRenderer` splits
into submitting the passes and ending the frame, with `RenderInstance` holding a list of overlay
callbacks run between them. Views: the renderer's passes as now, then the user layer, then the dev
layer (`ui::USER_VIEW`, `ui::DEV_VIEW`, at the top of bgfx's view range). The back buffer is the
target, at window size. *Why:* bgfx orders by view ID; the editor already does this by hand
(`EDITOR_IMGUI_VIEW_ID`), and the HUD shader works around its absence by re-drawing the frame.

**D4. Events in from the engine's platform layer, not from ImGui's GLFW backend.** The platform system
(`graphics::installPlatform`) already polls GLFW; it gains a per-frame text-input queue (codepoints,
from `glfwSetCharCallback`, chained as the scroll callback is) and the host translates `Input` into
`ImGuiIO::Add*Event` and RmlUi's `Context::Process*`. Routing order each frame: dev layer (when shown)
first, then the user layer, then the game; each layer that wants an event takes it, and the host sets
`Input::wantCapture*` from what was taken. *Why:* one event stream, one owner; `imgui_impl_glfw`
installs its own callbacks and would bypass the user layer. It also makes both layers drivable from a
test with no window.

**D5. The user layer is RmlUi** (MIT, C++17, HTML/CSS-like documents, data bindings, spatial
keyboard/gamepad navigation, used in shipped games). Its 6.x styling has what a polished, modern UI
needs as *style*, not code: transitions, keyframe animations, transforms, gradients, `box-shadow`,
`filter` and `backdrop-filter: blur()`, and decorators that run a program's own shaders. The engine
writes its bgfx render interface **in full** -- geometry, textures, scissor, clip masks, transforms,
and the layer and filter calls that blur, shadows and backdrops need (offscreen layers, downsample and
separable blur from `pjv_filter.sc`, composite) -- and its system interface (time from `Time`, logging
through `core/log.h`). Exact names are checked against the RmlUi 6 headers when it lands. *Why over the
others:*

| Option | Blur, animation | Customising | Why not |
|---|---|---|---|
| **RmlUi** | in the style sheet | RCSS, templates, custom elements, shader decorators | we write the bgfx backend, and its layers and filters are the larger half |
| Dear ImGui, styled | no animation system; blur only by hand-written draw callbacks | colours and spacing, in code | tool-shaped; right for the dev layer |
| Our own UI | what we build | total | layout, text, focus, scrolling, animation, filters: RmlUi again, slower |
| Clay, Nuklear | none | Clay is layout only; Nuklear looks like a tool | we would still write text, input, focus, effects |
| Ultralight (HTML) | real CSS, blur included | total | closed source, licence capped by revenue, we still write its GPU driver |
| NoesisGUI (XAML) | strong, blur included | XAML tooling | commercial, closed source |

**D6. User UI is data beside the program**, as renderer folders are: `ui/` in an example's folder with
`.rml` documents, `.rcss` styles and fonts, staged by `projv_add_example(... ASSET_DIRS ui)`. Dev
builds reload a document when its file changes. *Why:* layout and look change without a rebuild, and
the engine already resolves data against the working directory.

**D7. Gameplay talks to the user layer through data models and Events, never callbacks.** A program
binds values (`ui::user::Model model = ui::user::model(app, "hud"); model.bind("score", &score);`)
and marks them dirty; a UI action (`data-event-click="pause"`) arrives as a `ui::UiAction{document,
name, args}` event, handled in Update like any other. *Why:* the simulation must stay deterministic
and networkable: a button press is input, like a key, and becomes a command the same way; nothing in
the UI runs inside a step.

**D8. Fonts: FreeType**, a new submodule and vcpkg port, used by both layers (RmlUi's default font
engine; ImGui through `imgui_freetype`).

| | FreeType | stb_truetype | MSDF (msdfgen) |
|---|---|---|---|
| Small text (10-14 px) | best: hinted, sharp | softer, unhinted | worst |
| Scaled, animated, in-world text | re-rasterised per size | same | best: one atlas, any size |
| Outline, glow, shadow | RmlUi's font effects | same, through our own font engine | nearly free in the shader |
| RmlUi | its default font engine | a font engine to write | a font engine and a text shader to write |
| ImGui | `imgui_freetype` | its default | no |
| Dependency | new | already in bgfx's `3rdparty` | new, plus atlas baking |

MSDF is the addition if text must scale in 3D (U5). Complex scripts (shaping) would be HarfBuzz, which
RmlUi supports; not planned.

**D9. The dev layer is the editor's backend, promoted.** `imgui_impl_bgfx.cpp` moves to
`src/ui/dev/`, its shaders become engine shaders (shader-library D3), and the scene editor switches to
it in the same commit: one copy. Most examples and demos install it. It is **shown with F1**
(configurable), hidden by default, and a program adds panels:

```cpp
projv::ui::dev::addPanel(app, "Mayhem", [](projv::Application& app) {
    ImGui::SliderFloat("blast radius", &app.world.ctx().get<Tuning>().radius, 1.0f, 20.0f);
});
```

The engine ships panels for what it owns: frame time and per-stage system timings, the log (an spdlog
sink, filterable by category), physics (bodies, islands, step time, debris), networking
(`clientStats`/`sessionStats`: the overlay networking.md deferred), the renderer (its uniforms,
editable; its passes and targets), and an entity inspector (entities, their components by name).
*Why:* the panels are the reason to want a dev layer in every program, not just the editor.

**D10. In every build, behind `--dev-ui`.** The dev layer is compiled into release builds too; in
`dev` it is available by default, in `release` only when the program is started with `--dev-ui`
(read by the host from the command line, `ui::UiOptions::devLayer` to set it in code). Without it, F1
does nothing and panels cost nothing (`addPanel` registers, nothing draws).

**D11. Tests are headless.** RmlUi runs with a recording render interface (no GPU): tests load a
document, drive it with synthetic `Input`, and check layout boxes, data-binding updates and the
`UiAction`s that come out. ImGui runs with no backend (`ImGui::NewFrame` on a fake display size) to
check the panel registry and capture routing. Nothing new needs a display.

**D12. Colour.** UI colours are authored in sRGB, as stored colours are
([color-convention.md](color-convention.md)); both layers draw after the display transform, into the
same back buffer it writes, and U0 settles which conversion (if any) the UI shaders apply.

**D13. The demos share one look.** `examples/shared/ui/` holds a common style sheet and fonts that the
`_demo`s use; each demo adds its own documents beside it. *Why:* one look to polish, and the demos read
as one project. It is the demos', not the engine's (the engine's default sheet stays a reset).

## Milestones

| | What | Proven by |
|---|---|---|
| **U0** | `ui/` module and `projv_ui` target; overlay hook in `renderConstructedRenderer` (D3); text-input queue in `Input`; host with routing and capture (D4); layering rules | unit tests: routing order, capture flags, text queue; every example still runs unchanged |
| **U1** | Dev layer: backend promoted (D9), F1, `--dev-ui` (D10), `addPanel`, frame/systems/log panels; the scene editor on the engine's backend; installed in most examples and demos | editor unchanged in use; F1 in each |
| **U2** | Engine panels: physics, networking, renderer, entity inspector | `physics`, `mp_client` and `renderer_gallery` show them |
| **U3** | User layer: RmlUi and FreeType as submodules and vcpkg ports; the render interface with geometry, textures, scissor, clip masks and transforms; data models, `UiAction`, hot reload | headless tests (D11) |
| **U4** | The render interface's layers and filters: blur, `backdrop-filter`, drop shadows, shader decorators. A dedicated example, `72-user-interface`: a polished, animated UI over a live scene -- frosted panels, a main menu, settings bound to values, a HUD -- its look entirely in its `ui/` folder | the example; its look checked by the user |
| **U5** | The party and playground HUDs rebuilt as documents, `hud.frag` and its uniforms deleted; a pause menu and settings in both. The basic renderer's `crosshair` option goes ([shader-library.md](shader-library.md) S4): the sandbox, physics and multiplayer client draw their own | `selftest.sh` for both; look checked by the user |
| **U6** | Gamepad navigation (needs gamepads in `Input` first); a document rendered to a texture, for in-world screens; MSDF text if those need it | each with an example |

Docs with each milestone: `docs/reference/ui.md` (a row per public header), a concept page
(`docs/concepts/ui.md`: the two levels, routing, the views), a guide for each level (adding a dev
panel; a HUD from a document), and `engine-promotion-candidates.md` gaining the ImGui backend as
ruled.

## As built

**U0** (2026-11-01).
- `projv_ui`: an object library folded into `projectV`, as `projv_physics` is, behind `PROJV_BUILD_UI`
  (on), which defines `PROJV_UI` for consumers. It holds the host now; the two layers join it, each
  compiled with its own library's include paths.
- `ui/ui.h`:
  - `Host` (in `world.ctx()`) holds one `Layer` per `Level` (`Dev`, `User`). `route(input)` offers the
    input to the visible layers in that order, telling each what the ones before it took, and writes
    `Input::wantCaptureKeyboard`/`Mouse` from the union. It is recomputed every frame.
  - `draw(size)` draws the user layer in `USER_VIEW` (254), then the dev layer in `DEV_VIEW` (255).
  - `installUi(app, renderInstance)` routes in a PreUpdate system ordered after `"platform"`, and draws
    as an overlay. The overlay looks the host up each frame rather than holding it: `world.ctx()` may
    move a small resource as it grows.
- D3: `RenderInstance::addOverlay(order, fn)` / `drawOverlays(size)`. `graphics::endFrame(instance,
  size)` draws the overlays, then `bgfx::frame()`; `renderConstructedRenderer` ends with it. The scene
  editor, which submits its own frame, moves onto `endFrame` with its ImGui in U1.
- D4: `Input::text`, the frame's typed code points, from GLFW's character callback, chained as the
  scroll callback is. The translation into ImGui's and RmlUi's events is each layer's (U1, U3).
- Layering (D2, `ctest -R layering`): core, graphics, utils, data_structures and net never include
  `ui/`; in the engine, ImGui only from `src/ui/dev/` and `ui/dev.h`; RmlUi only from `src/ui/user/`,
  anywhere in the tree.
- Tests (`ui:*`, 7 cases, no window): routing order, a hidden layer skipped, capture reaching the next
  layer and the game and reset each frame, text reaching the layers, draw order and views, overlay
  order, and `installUi` routing after the platform system and drawing as an overlay. The GLFW
  character callback itself needs a window and is not unit-tested.
- Docs: `docs/reference/ui.md` (new module page, in the header check and the site), `Input::text`,
  `endFrame` and the overlays in `graphics.md`.
- Checked: Hello Voxel and the sandbox self-test run as before, with no overlay installed.

**U1** (2026-11-01).
- **ImGui, once.** Dear ImGui (`external/imgui`, 1.92.9b docking) is compiled into `projv_ui`; its
  headers are public on `projectV` (and installed under `third_party/imgui`), because `ui/dev.h`
  includes `imgui.h`. The scene editor's own build of it is gone: it keeps ImGui's GLFW platform
  backend only.
- **The backend, promoted (D9).** The editor's `imgui_impl_bgfx` is `src/ui/dev/imgui_bgfx.cpp`
  (`ui::dev::initRenderer`, `renderDrawData`, `textureID`, `shutdownRenderer`).
  - Its shaders are engine shaders (`shaders/engine/imgui/`). `projv_embed_engine_shaders` now takes
    several directories, each with its own `varying.def.sc`.
  - Its state is per ImGui context (`io.BackendRendererUserData`), so the editor's context and the
    dev layer's cannot clobber each other.
  - The editor draws through it. Its UI screenshot (`EDITOR_UI_SHOT`) shows the whole interface as
    before.
- **The dev layer (`ui/dev.h`, `ui/dev_layer.h`).** Its own ImGui context, fed from `Input`, not
  ImGui's GLFW callbacks (D4): keys and modifiers on their edges, typed text, the cursor, buttons and
  the wheel.
  - F1 (`UiOptions::devToggle`) shows it. Showing releases a captured mouse-look cursor, and hiding
    gives it back. While shown, a click on the scene captures it again and play goes on with the
    panels up: a captured cursor is the game's, and the panels see no mouse until Esc releases it.
    (The first version kept releasing it, which left no way back into the game with the panels
    shown. Found by hand in Voxel Mayhem.)
  - Panels: `addPanel(app, name, fn)`, each a window built in Render while shown; registered panels
    cost nothing otherwise.
  - The engine's panels: **Frame** (frame time and its graph; every system's time by stage, from the
    new `Application::systemSeconds`) and **Log** (an spdlog ring-buffer sink, 4000 lines, by
    category, filterable).
  - Its GPU resources go in a Shutdown system ordered before `"shutdown"`, since Voxel Mayhem shuts
    bgfx down in its own. It exits cleanly with the layer shown.
- **D10.** `PROJV_DEV_UI_DEFAULT` (on; off in the `release` preset). Off, only `--dev-ui` installs the
  layer: `ui::setCommandLine(argc, argv)` from `main`. `PROJV_DEV_UI_SHOWN=1` starts it shown, for
  captures.
- **Where it differs from D4: the host removes what a layer took.** D4 had it only set
  `Input::wantCapture*`, leaving each program to check. The ten programs read input in about 200
  places, so `Host::route` also masks the taken mouse or keyboard out of `Input`
  (`Input::maskMouse`, `maskKeyboard`). A click on a panel never reaches the game, with no change to
  any program. The mask is a view: the keys' and buttons' state underneath is kept. (The first
  version cleared it, so a button released after a masked frame had no release edge, and a panel
  resize stayed held for good. Found by hand; `ui: a button released over a panel still ends its
  drag` is the regression test.)
- **The UI writes colour, not alpha.** The backend blended alpha like colour, so a 95%-opaque panel
  left the back buffer's alpha at 0.95 (127 at the panel's edge), and a compositor showed the desktop
  through the panel. Found by hand. It now writes RGB only: the sandbox's capture under a panel and
  the editor's whole window read alpha 255.
- **Installed in** Hello Voxel, entities, the sandbox, physics, world loading, the multiplayer
  client, Voxel Mayhem, the explosion demo, and the party and playground clients: every program on the
  engine's platform layer. Not in the gallery, the advanced renderer, the terrain generator or the
  scene previewer, which poll GLFW themselves. They move onto `installPlatform` first.
- **Tests** (`ui:*`, 12 cases, headless):
  - U0's, and the masking;
  - a panel registered before the layer exists, drawn only while shown;
  - a real ImGui context with no GPU that takes the mouse over its panel and not elsewhere, nor
    over it while the cursor is captured;
  - `--dev-ui`;
  - `systemSeconds`.
- **Checked by eye:** the sandbox's capture with `PROJV_DEV_UI_SHOWN=1` shows the Frame and Log
  panels over the scene. F1 by hand in each program is still to do.
- **Docs:** `docs/concepts/ui.md` (new), the guide [Add a dev panel](../guides/dev-panel.md), and
  `ui/dev.h`, `ui/dev_layer.h` and the options in `docs/reference/ui.md`.

**Frame panel, a profiler** (2026-11-02, between U1 and U2). Asked for: readings that hold still,
colour in the graph, and the GPU by pass.
- **`ui::dev::Meter`** (public, for programs' panels too): the mean, least and greatest over the last
  complete period, republished once a period. Every reading in the panel is one; the period is chosen
  in the panel (0.25-2 s, default 0.5 s). A max over 1.5x the average shows orange.
- **Sampled in the layer, not the window**: `DevLayer::Profiler` samples every frame the layer is
  shown, before the panels, so a collapsed Frame window keeps its history. `pause` freezes it.
- **Stacked graphs**, drawn with ImGui's draw list: CPU by stage plus time in no system, GPU by pass
  plus the gap between passes; 240 columns, scaled to the 97th percentile (a hitch is cut and marked
  red), guides at 120/60/30 fps, a tooltip per column. Stage and pass colours match the tables'
  swatches; passes take a palette by view, so they keep their colours.
- **GPU by pass**: showing the layer sets `BGFX_DEBUG_PROFILER`, hiding clears it (hidden costs
  nothing). GPU samples are taken when `Stats::gpuFrameNum` moves, not every frame. Passes are named
  for the profiler: `BGFXDependencyGraph::name` (its shader file's stem, numbered when a shader is
  in several passes; the basic renderer's is "basic"), given to `bgfx::setViewName` each frame the
  pass draws, since renderers in one program share view IDs. `drawnSize` is the size it last drew
  at. The table lists the active renderer's passes in order, an `enabled = false` one as "off",
  then any other view ("dev ui"). A view whose name changes (the gallery switching renderers)
  starts its readings over.
- **bgfx** section: backend, back buffer, draws/computes/blits, render-thread submit and waits,
  texture, target and GPU memory.
- Tests (`ui:*`, 15 cases): a Meter's period, a sample longer than its period, and 300 headless frames
  shown.
- **Fixed with it: the dev layer's scissors on a scaled display.** The layer took
  `DisplayFramebufferScale` from `glfwGetFramebufferSize`, but the engine resets bgfx's back buffer
  to the window's (logical) size, so at 170% Wayland scaling every scissor came out 1.7x too large
  and too far down: a window's first line cut off, the Log's contents clipped away entirely (since
  U1), and ImGui's font rasterised at 1.7x and drawn shrunk. The scale is now the back buffer's
  (`bgfx::Stats::width`/`height`) over the window's. Found from the panel's captures; bgfx's
  Vulkan scissors, logged, showed the 1.7x. The scene editor resets bgfx to the framebuffer size and
  has its own ImGui, so it was never affected.
- Checked by eye: sandbox (one pass) and Voxel Mayhem (restir-pt), live and in captures.

## Open questions

None open. Answered 2026-11-01: RmlUi for the user layer (D5); the demos share one style sheet (D13);
engine shaders are embedded in the library (shader-library D3).
