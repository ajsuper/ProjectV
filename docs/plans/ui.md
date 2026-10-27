# UI: a developer layer and a user layer

Status: **proposal**, 2026-10-27. Nothing built. Follows [shader-library.md](shader-library.md): the
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
  ([imgui_impl_bgfx.h](../../examples/60-scene-editor/imgui_impl_bgfx.h)) and shaders staged in its
  renderer folder. No other program can use it without copying both.
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

## Milestones

| | What | Proven by |
|---|---|---|
| **U0** | `ui/` module and `projv_ui` target; overlay hook in `renderConstructedRenderer` (D3); text-input queue in `Input`; host with routing and capture (D4); layering rules | unit tests: routing order, capture flags, text queue; every example still runs unchanged |
| **U1** | Dev layer: backend promoted (D9), F1, `--dev-ui` (D10), `addPanel`, frame/systems/log panels; the scene editor on the engine's backend; installed in most examples and demos | editor unchanged in use; F1 in each |
| **U2** | Engine panels: physics, networking, renderer, entity inspector | `physics`, `mp_client` and `renderer_gallery` show them |
| **U3** | User layer: RmlUi and FreeType as submodules and vcpkg ports; the render interface with geometry, textures, scissor, clip masks and transforms; data models, `UiAction`, hot reload | headless tests (D11) |
| **U4** | The render interface's layers and filters: blur, `backdrop-filter`, drop shadows, shader decorators. A dedicated example, `72-user-interface`: a polished, animated UI over a live scene -- frosted panels, a main menu, settings bound to values, a HUD -- its look entirely in its `ui/` folder | the example; its look checked by the user |
| **U5** | The party and playground HUDs rebuilt as documents, `hud.frag` and its uniforms deleted; a pause menu and settings in both | `selftest.sh` for both; look checked by the user |
| **U6** | Gamepad navigation (needs gamepads in `Input` first); a document rendered to a texture, for in-world screens; MSDF text if those need it | each with an example |

Docs with each milestone: `docs/reference/ui.md` (a row per public header), a concept page
(`docs/concepts/ui.md`: the two levels, routing, the views), a guide for each level (adding a dev
panel; a HUD from a document), and `engine-promotion-candidates.md` gaining the ImGui backend as
ruled.

## Open questions

1. **RmlUi** (D5): recommended, not yet agreed.
2. **The demos' look.** Whether the demos share one style sheet (in `examples/shared/ui/`) or each
   has its own; either way it is theirs, not the engine's.
3. Engine shaders: embedded in the library (shader-library D3).
