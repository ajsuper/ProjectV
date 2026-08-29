# 60 — Scene Editor

ProjectV's scene editor / modelling tool: a window whose interface is made of **ImGui dock panels**, one of which is the scene rendered live by the engine, with **File ▸ Load Scene…** loading any Compose scene folder off disk at runtime.

Editing is organised around **tools** — Select, Move, Sculpt, Paint — chosen from a strip of icons down the left edge of the scene, or with `Ctrl+Q`/`W`/`E`/`R`. The tool is what a left-click in the viewport means, and the right-hand column shows the tool's settings. Select and Move are complete; Paint recolours voxels along a click-and-drag, and Sculpt adds and removes geometry along one. Palette editing — recolour, rename, add and remove entries — is complete, and everything is undoable.

There are three top-level tabs, each of which owns the whole window: **Edit** (everything above), **Render** (`F11`, a path traced image of the same camera), and **Brushes** (`F10`, [the Brush Lab](#the-brush-lab) — where programmable brushes are written in Lua and tried on the scene without keeping the result).

## The shell it is built on

**One frame carries both the scene and the interface.** The scene's render passes draw into an offscreen texture instead of the back buffer, and ImGui draws that texture as an image inside the Viewport panel. That is what lets the scene live in a dock node the user can move, resize, and tab like any other panel — rather than being the window, with the interface floating on top of it. Every editor tool that will want a panel beside the scene depends on this.

**The scene is a replaceable runtime resource.** The previewer loads one scene at startup and lives with it. An editor cannot: File ▸ Load Scene… tears down the GPU state (`destroyGPUData`) and uploads a different scene (`createTexturesForScene`) without a restart, and a bad path leaves the scene that is already open untouched.

The viewport renderer itself is the [ScenePreviewer](../10-scene-previewer/)'s — one primary ray per pixel, pure albedo, no lighting — with the display pass retargeted from the back buffer to an offscreen texture. It is the right renderer to start an editor from: it shows what is actually *in* the scene rather than how it is lit, and at one ray per pixel it leaves the frame budget for interface and, later, editing.

## How to Build

```bash
git submodule update --init external/imgui
git submodule update --init examples/SceneEditor/external/lua   # Lua 5.4.7, for the Brush Lab
cmake --preset dev
cmake --build --preset dev --target scene_editor
```

ImGui comes from the engine's `external/`; Lua from this example's. Without either, the configure
step skips this example with a message naming what to check out.

The binary and its staged renderer folders and brushes land in `build/examples/scene_editor/`.

Lua lives in *this example's* `external/` rather than the engine's: nothing in the engine embeds a scripting language, and programmable brushes are the editor's feature rather than the renderer's. It is compiled straight into the binary like ImGui, so there is nothing to install and no shared library to find at runtime. `make` says which command to run if the folder is empty, rather than failing with forty errors from a header nobody in this repo wrote.

## How to Use

```bash
./scene_editor [scene-directory]
```

With no argument it opens the previewer's bundled `StonehillCastle` if it is there, so the editor starts on something rather than an empty panel. Either way **File ▸ Load Scene…** switches scenes.

| Input | Action |
|-------|--------|
| Right-mouse drag in Viewport | Fly the camera (cursor is captured for the duration) |
| `W`/`S`, `A`/`D`, `R`/`F` | Forward/back, strafe, up/down — while flying |
| Middle-mouse drag | Pan — the scene follows the cursor across the image plane |
| Scroll wheel | Get closer: dolly under perspective, zoom under orthographic |
| `Ctrl` + scroll wheel | Movement speed for the fly-through |
| `H` | Re-frame the camera on the scene |
| Left-click in Viewport | Whatever the active tool says — see Tools below |
| `Ctrl+Q` / `Ctrl+W` / `Ctrl+E` / `Ctrl+R` | Select / Move / Sculpt / Paint |
| `Ctrl+T` / `Ctrl+Y` | Place / Region |
| `Ctrl+O` / `Ctrl+Shift+R` | Load Scene… / reload the current scene |
| `Ctrl+Z` / `Ctrl+Shift+Z` | Undo / redo |

Under **Place** and **Region**, with something in the open asset selected (see [Assets](#assets-the-place-and-region-tools) below), the keyboard also means:

| Input | Action |
|-------|--------|
| Arrow keys | Nudge one voxel of the open asset's lattice, in the camera's ground plane |
| `PgUp` / `PgDn` | Nudge one voxel vertically |
| `Ctrl` + arrows | Turn 90° about the lattice axis most nearly facing the camera |
| `Delete` | Remove the selected item from the contents |

There is deliberately no `Enter` to commit and no `Esc` to cancel. An asset is not a modal state waiting to be resolved, it is an object that stays where you left it; baking is a press on a button that says what it will do.

Panels dock, tear off, and tab by dragging their title bars. The layout is saved to `imgui.ini` beside the executable and restored on the next run; **View ▸ Reset Layout** puts it back to the default.

### The layout

Two columns and a strip, which is the shape editors of this kind converge on for a reason — the left column answers *what exists*, the right one *what is being done to it*:

```
┌─────────────┬──────────────────────────────────┬─────────────────┐
│ ASSETS      │  StonehillCastle › keep › model  │ INSPECTOR       │
│   what is   │  ┌──┐                            │ TOOL            │
│   in the    │  │QW│        viewport            │ PALETTE         │
│   open one  │  │ER│                            │                 │
├─────────────┤  └──┘                            │                 │
│ LIBRARY     │      [AO] [normals] [sun]        │                 │
│   what      ├──────────────────────────────────┴─────────────────┤
│   could be  │ HISTORY                                            │
└─────────────┴────────────────────────────────────────────────────┘
  1.4 ms (700 fps) | 2663 x 1303 | 6 chunks, 2 components | Select | …
```

The right-hand three are **stacked, not tabbed**, and that is the point: they are not alternatives. Sculpting wants a brush, a colour, and the target's transform in view at once, and while they shared one dock node the tool panel had to carry its own duplicate copy of the palette grid — two places showing "the current colour", which is how you end up sculpting in a colour you were not looking at.

### Panels

| Panel | Shows |
|-------|-------|
| **Viewport** | The scene, rendered at exactly the panel's pixel size, with the breadcrumb, the tool strip, the render toggles and the navigator floating over it |
| **Assets** | The contents of the asset you have open, one level deep, in order: its op, drag to reorder, click to select, the arrow to look inside, `open` to go in. Above it the breadcrumb you came down; below it what the contents resolve to, and Add / Save / Bake |
| **Library** | A disk browser over the folders assets live in: navigate, see which hold a `compose.json`, list what is inside one without loading it, open it, or bring it into the asset already open as a Copy, a Bake or a Link |
| **Inspector** | The selected component: kind, source, voxel count, local and world transform |
| **Tool** | The active tool's settings. Title and contents follow the tool — a setting the current brush or shape cannot use is **absent**, not greyed out, since a disabled control still reads as part of the tool and still has to be ruled out |
| **Palette** | The selected component's materials as a grid of swatches — edit colours, add and remove entries, see where each is used |
| **History** | Every edit made, with undo/redo and click-to-jump |
| **Statistics** | Frame time, camera, accumulated samples — a floating window under **View ▸ Statistics**, not a dock panel |

### The render toggles

Five buttons on the bar at the bottom of the scene image. What sits underneath them is the scene previewer's renderer, which is one primary ray per pixel and the voxel's **stored albedo written out with no lighting at all** — so a material that reads wrong there is wrong in the data, not in the lighting. That is the property the whole viewport is built to protect, and it is why the first four toggles are applied by a separate pass (`shade.frag`) reading the geometry the primary ray already wrote, rather than by the march itself. Each of the four multiplies a scalar into the albedo; turn them all off and the image is byte for byte the unmodulated albedo again.

| Toggle | Default | What it does | Costs |
|--------|---------|--------------|-------|
| **Ambient occlusion (screen-space)** | **on** | Contact darkening from the depth of the screen-space neighbourhood: creases go dark, and the ground under an object reads as attached rather than passing behind it | 16 texture taps per pixel |
| **Ambient occlusion (ray traced)** | off | The same question asked of the scene instead of the screen: four short rays into the hemisphere above the surface, reaching 24 voxels instead of 3 | Four scene rays per pixel |
| **Normal shading** | **on** | A fixed brightness per face axis, plus darker undersides. The two faces meeting at a voxel edge differ, so form is legible inside a flat-coloured region | One dot product per pixel |
| **Sun shadow** | off | One visibility ray towards a fixed, world-space sun. Occluded points fall to 55% | A full scene ray per lit pixel |
| **Advanced preview** | off | The three material properties the other four ignore: **emission, reflections and transparency**. Not a readability aid — the only toggle here that changes what the *materials* look like rather than what the geometry looks like | A peeled primary march, plus a scene ray per glossy or metallic pixel |

**The advanced preview is the odd one out and is documented on its own below.** The other four darken stored albedo; it *adds* to it, and what it adds is not a readability aid but the three material properties the Viewport has no other way of showing you.

**Occlusion and normal shading are on because between them they are what makes an unlit scene read as solid at all.** The axis shading separates the two faces at an edge; the occlusion puts objects *on* the ground instead of in front of it. Neither is light transport and neither is pretending to be — they are readability aids for judging shape, and the reason they are cheap enough to leave on is that neither casts a ray.

The occlusion's 16 taps are grainy for the few dozen frames the accumulation pass takes to settle, and that grain is the honest reason it used to be off. It is on now anyway: meeting a scene that already looks like an object is worth more than never being surprised by a moment of noise in the creases. Any toggle also invalidates the accumulated image the same way a camera move does, so switching one resolves from scratch instead of fading in through 64 frames of history.

#### The two occlusion buttons are one choice, not two

They are the same effect estimated two ways, so **turning either on turns the other off**. Multiplying two occlusion terms together is not a stronger answer; it is the same darkening applied twice. The bar therefore reads as a three-state choice — off, cheap, or thorough — with the two states sitting next to each other.

The screen-space one can only see what the albedo pass happened to draw. Occlusion from geometry off the side of the screen, behind the surface being darkened, or hidden behind something nearer is invisible to it — and so is anything more than a few dozen pixels away, because its tap disc is measured in *pixels* and a disc wide enough to cross a room thrashes the texture cache for a result that reads as a smear rather than a crease. Below two pixels it gives up and returns "unoccluded", which is why a scene viewed from far enough back loses its occlusion entirely.

The ray-traced one has neither limit, and buys the two things the screen-space estimator is worst at:

- **An object darkens as it *approaches* a wall**, not only once they touch. The hit distance is fed back linearly, so a surface pressed against another goes fully dark and one at arm's length contributes almost nothing — which is exactly the cue that says how close two things are.
- **A large concave space is darker than an open one.** A courtyard, a room, the inside of an arch: at a 24-voxel reach these read as enclosed. At 3 voxels they are indistinguishable from a field.

Four rays per pixel per frame is noisy on its own — four binary tests, against sixteen smoothly-varying taps — and it leans on the accumulate pass harder than anything else in the viewport does. A still camera averages 64 frames, so a settled image is up to 256 rays and measurably converged; while flying, you get four. Three details make that work:

- **The rays are cosine-weighted over the hemisphere**, sampled by Malley's method. Ambient occlusion *is* the cosine-weighted blocked fraction, so drawing the cosine out of the pdf leaves the estimator a plain average with no per-sample weight at all.
- **The directions come from one R2 low-discrepancy sequence, offset per pixel.** A pair of hashes costs the same and stratifies far worse, and with a running mean doing the integration a sequence that clumps converges to a blotchy mean however many frames it is given. The per-pixel offset (a Cranley–Patterson rotation) is what stops every pixel casting the same four directions and turning the noise into a fixed pattern locked to the geometry.
- **The sequence restarts from zero on every camera move**, indexed by frames-since-move — the same count the accumulate pass divides by. That makes the 64 frames it averages a stratified set rather than 64 arbitrary points, and keeps the index small enough that float32 still resolves the `fract`.

**The AO rays march at full resolution, and that is not a place to save time.** A coarser LOD cap returns interior nodes as solid, and an interior node is 4 or 16 voxels across — wider than the quarter-voxel the ray origin is lifted off the surface. Every ray would immediately hit the coarse node holding its own starting surface, and the scene would render black.

**What bounds a ray is the distance test, not the step budget.** 48 steps is only enough to be sure of reaching 24 voxels; a ray that stops beyond that distance is treated as having escaped. That cutoff is the point rather than a compromise — occlusion is a local question, and a ray long enough to reach the far side of a scene answers "how big is the scene", darkening an open plain and a small room by the same amount.

**The sun shadow is the one toggle that is a claim about the *scene* rather than about the surface.** Occlusion says "these two surfaces are close together"; a shadow says "this object stands here, relative to everything else in the scene". That is a different and often more valuable question, which is why it is worth reaching for deliberately rather than paying for always. It costs a second full scene march for every lit pixel, roughly doubling what a frame spends — about what the ray-traced occlusion costs, and not by accident: that pass spends four rays of 48 steps against this one's single ray of 256, so the two expensive toggles come to roughly the same bill and neither is a surprise after the other.

It is a single ray and it is deliberately hard-edged. The direction carries no per-frame jitter, so unlike the occlusion it contributes nothing for the accumulate pass to resolve and looks identical in the first frame of a camera move and the hundredth. Softening it would mean jittering the direction inside a cone and letting the running mean average it out — but a *binary* test averaged one sample per frame is far noisier while flying than a 16-tap average is, so a soft shadow would undo the one thing the hard one has going for it.

Three details worth knowing, because each is a bug that showed up before it was fixed:

- **The sun is world-fixed, not camera-relative.** A shadow that swung with the camera cannot tell you where an object stands, because the cue moves with the eye instead of staying with the scene.
- **Faces turned away from the sun are darkened without a ray at all.** That is not only the obvious saving — a little under half the pixels — it is also what keeps the terminator on the geometry, where a grazing `NdotL` would otherwise leave it on the shadow ray's epsilon.
- **The ray's start offset is measured in voxels, not world units.** Editor scenes sit at coordinates in the thousands, where a float32 ULP is around a thousandth of a unit, so a fixed epsilon small enough for a 0.01-unit voxel is below the noise floor of the position it is added to — and the ray re-hits the voxel it left, which shows up as stippled darkening across every lit face. A quarter of a voxel along the normal is inside the adjoining empty cell at either end of the range of scenes the editor opens. It is the same reason the occlusion radius is expressed in voxels.

#### The advanced preview

The fifth button, and the only one on the bar that is not a readability aid. A material carries three properties the Viewport draws nothing of — **emission, the specular lobe, and transparency** — and the Palette panel used to say so in as many words, because until this existed the only way to find out what a slider had done was to switch to Render mode. That is a real cost: Render mode re-frames the shot under a sun and a sky and then takes hundreds of frames to converge, so "did raising glossiness do anything" became a question you answered by leaving the thing you were doing.

This answers it in place. It stays inside the viewport's own flat, sunless, background-only world and adds nothing but what the materials themselves contribute:

| What | How |
|------|-----|
| **Transparency** | The primary march peels — it sees *through* transparent voxels to the nearest opaque surface and tints what it finds by the layers it crossed. Identical traversal, identical stochastic alpha to Render mode's, so glass that reads right here reads right there. `Transmission` deepens the tint with distance travelled *through* the body, as opposed to `Transparency`, which is charged once at the surface |
| **Refraction** | With the Palette panel's **Refraction** budget above zero, the ray also *bends* at an interface whose `IOR` is not 1, and total internal reflection falls out of it — past the critical angle a thick glass edge goes mirror-like. The budget is a **renderer** setting, not a material one, and is shared with Render mode from one field so the preview and the render cannot disagree. Zero by default: a bend is a branch every transparent layer of every ray tests for, so a scene with no glass does not pay for it |

**Diagnosing refraction.** `EDITOR_REFRACT_DEBUG=<1..4>` replaces the viewport with one of four
diagnostic views, and `EDITOR_GLASS="<prefix>=<alpha>"`, `EDITOR_GLASS_IOR`, `EDITOR_GLASS_DENSITY`
and `EDITOR_REFRACTION=<0..3>` set a material up from the command line so two runs are comparable.
The views exist because "the picture is wrong" is never one question — each isolates one stage, and
the one that comes back uniform is the one that is not at fault:

| Mode | Shows | Rules out |
|---|---|---|
| 1 | Discarded hits — magenta `rayT <= 0`, cyan degenerate normal | The renderer refusing a hit the traversal actually found |
| 2 | Bends taken — grey 0, red 1, green 2, blue 3+ | A discontinuous bend *count* versus a wrong bend |
| 3 | Hit coarseness — green full-res, yellow one LOD step, red two or more | LOD, asked directly |
| 4 | Stop reason — green opaque, red layers, blue iterations, yellow stall, magenta segments | "Ran out of budget" versus "finished, and the answer is wrong" |
| 5 | Crossed versus bent — grey nothing crossed, **red** crossed but never bent, green bent | Whether the fault is before or after the bend condition |

| **Emission** | The emissive radiance of the hit, plus the glow of every transparent layer in front of it, each already dimmed by what sat in front of *it* |
| **Reflections** | One GGX sample per pixel per frame, marched into the scene. Same lobe, same Fresnel, same masking term the path tracer uses — the estimator is written out in closed form because there is one lobe here and nothing to weigh it against |

**A scene whose materials have none of the three set looks identical with this on.** Not approximately: `metallic 0` leaves the albedo whole, an opaque scene's transmittance is exactly one, and `glossiness 0` with `metallic 0` drives the dielectric F0 to zero, which returns from the reflection before it spends a ray. That is the same zero rule the material word itself is built on, and it is what makes the toggle safe to leave on while working.

**What it does *not* do is light the scene.** An emitter glows; it does not brighten the wall beside it. That is light transport, it needs a path, and Render mode is where paths live. The distinction is worth keeping straight, because "my emitter is not lighting anything" is a correct observation about this mode rather than a bug in it.

##### Emission and reflection are added, not multiplied — and that needs its own render target

The four readability toggles all darken. Adding a glow to the albedo *before* them means the occlusion dims an emitter for sitting in a crease, the axis shading dims it for facing sideways, and the sun shadow dims it for facing away from a sun this mode does not otherwise have. All three are nonsense: every one of those terms is a statement about **diffuse reflectance**, and a light source is not shaded by the aids that exist to make unlit geometry legible.

So `albedo.frag` writes what it produces to a **fourth attachment of its own** — `previewGlow` — and `shade.frag` adds it once, last, after every multiply has had its say. With the toggle off the target is zero, the add is a no-op, and the pass is byte for byte what it was before any of this existed.

The one thing that attachment costs is a slot shuffle: the engine binds every texture of every input framebuffer in order, so a fourth attachment on FBO 1 pushed the occlusion buffer from sampler 3 to sampler 4 in `shade.frag` and `denoise.frag`. `occlusion.frag` gets the glow bound at slot 3 and simply never declares it.

##### Both new samples are stochastic, which is why they cost one ray

The peel's alpha decision and the reflection direction are each one random sample per pixel per frame, seeded per pixel and per frame — the same arrangement the ray-traced occlusion uses, leaning on the same accumulate pass. So both are grainy while the camera moves and resolve within a second of it stopping, and neither needs a second ray to look smooth once you are still. A deterministic alternative exists for neither: alpha compositing N layers analytically means N shading evaluations rather than one, and a deterministic reflection is a mirror, which is the one roughness a real material almost never has.

##### The display pass had to learn about overshoot

`display.frag` is a deliberate straight copy, and it says so at length: the viewport's output is stored reflectance, already in [0, 1], and tone mapping it would mean the editor is no longer showing you the colour in the file.

The advanced preview is the first thing upstream that produces values above 1 — emission is radiance and its strength control reaches 245. Left alone those clip, and clipping is the worst available answer, because every channel that overshoots saturates to the same 1.0: a bright red emitter and a bright blue one both arrive on screen as a white blob, and *what colour is this emitter* is the question the toggle was added to answer.

Borrowing Render mode's ACES-plus-gamma pipeline would fix that and break something worse — that curve lifts and desaturates the whole image, including the pixels that are plain reflectance and were already correct, so toggling the preview would appear to change every material in the scene. Instead there is a rolloff built outwards from the straight-through rule:

- **Identity below the knee.** A pixel whose brightest channel is under 0.75 passes through untouched. Stored reflectance lives down there, so the guarantee survives the toggle intact.
- **Hue preserved above it.** The curve is applied to the brightest channel and the others scaled by the same ratio. Per-channel compression pulls an overshooting colour towards white as it brightens; scaling by the peak keeps a red emitter red however far past 1 it goes.
- **Asymptotic, not clamped.** The remaining quarter of the range is spent on an exponential approach to 1.0, whose slope at the knee matches the identity below it, so there is no crease at the join and no second brightness at which things start clipping after all.

##### The peel depth was measured, not guessed

The transparency budget is a **compile-time** loop bound the SPIR-V path unrolls, so every pixel of every frame carries the code whether it peels or not — including the plain view, which is what the viewport spends almost all its time drawing. The obvious conclusion is to keep the viewport's budget below Render mode's 65 layers, and it is wrong: with the preview off, the shader runs the same speed at 65 as at 2 (1172 against 1201 fps, same scene and framing). The branch is uniform across the draw, so the unrolled peel costs the plain path nothing — and a shallower budget here would have made the preview disagree with the render it is a preview *of*, which is the one thing it cannot afford to do.

With the preview on it is not free, and what it costs depends on the scene rather than on the number: the loop leaves at the first opaque voxel and again once the transmittance is spent, so opaque geometry runs one iteration and clear glass runs the budget. On a test scene with a lot of shallow water, 65 measured about twice 33. That is the price of agreeing with the render, and only the person who asked to see it pays it.

##### Two honest limits

- **A metal goes dark.** `metallic 1` has no diffuse lobe, so what it shows is its reflection — and in a mode with no sun and no sky, the thing it mostly reflects is a deliberately dark background. Chrome in an empty scene is nearly black here and looks like metal in Render mode. That is the truthful preview of the material rather than a flattering one, and the reflection does pick up nearby geometry, which is what makes it read as a mirror rather than as a hole.
- **The occlusion rays do not peel.** Glass occludes exactly as an opaque voxel does. Deliberately: occlusion is a readability aid measuring how enclosed a point is, glass in a window frame does enclose it, and teaching four rays per pixel to peel would cost that pass the thing that makes it affordable.

### The navigator

The bottom-right corner of the scene image: an orientation cube, three projection buttons, and an eyeball. They exist because the mouse buttons and the keyboard between them cannot answer three questions that come up constantly.

**Which way am I facing, and can I face an axis instead?** — the cube, drawn in the camera's own orientation with its three visible faces in the same axis colours the transform gizmo uses (X red, Y green, Z blue; the negative faces the same hue at two thirds the brightness, so `+X` and `-X` are distinguishable without reading the label). Click a face to look straight down that axis. **Drag the cube to orbit** — and the cube is the only place an orbit is offered, which is not an accident: orbiting needs a point to turn *about*, and everything else here moves a free-flying camera that never commits to one.

A label is dropped from any face turned close to edge-on, where the quad is a sliver barely taller than the glyph and the text would spill over its neighbours — which reads worse than no label, because it then appears to belong to whichever face it landed on. The colour still says which axis it is.

**Is distance distorting what I am looking at?** — the three projection buttons. These are not projection matrices; the renderer is a ray marcher, so each is a different **ray generator**:

| Mode | The rays | Why you would want it |
|------|----------|----------------------|
| **Perspective** | One origin, directions fanning over a 60° vertical FOV | Depth reads naturally. The default |
| **Orthographic** | Parallel directions; the *origin* slides across a plane `orthoHeight` units tall | Distance stops changing size, so two voxels line up on screen exactly when they line up in the scene — the question voxel work asks most often |
| **Isometric** | Orthographic, plus the camera snapped to yaw 45° / pitch −35.26° | All three axes foreshortened equally. A view as much as a projection, so selecting it moves the camera |

The one thing an orthographic ray generator needs that a perspective one does not is somewhere to *start*. A perspective ray leaves the camera, and there is nothing in front of it the camera is not already outside of. Parallel rays have no such guarantee — their plane sits at the camera position, so anything the camera has flown past would simply be missing from the same view without perspective. So the plane is pushed back along the view direction until it clears the scene's bounding sphere, and by the **minimum** offset that does so, because every unit of it is empty space the DDA has to step through on the way in.

Two consequences worth knowing:

- **The scroll wheel changes meaning.** Moving forward along a parallel ray produces an identical image, so under orthographic the wheel scales the height the view spans instead. Same gesture, same intent, the only arithmetic that can express it.
- **The occlusion pass needs telling.** `shade.frag` converts a world radius into screen pixels, and under perspective that divides by the distance to the camera. Under parallel rays the scale is constant across the whole image — leave the distance term in and distant geometry loses its occlusion to a radius that shrank away.

Every overlay the editor draws over the scene — the yellow selection box, the transform gizmo, the symmetry planes, the part outlines — goes through one projection function that carries the same branch, because all of them lay world geometry over an image the shader produced. A basis that disagreed with the shader by so much as a handedness would put every outline in the wrong place.

**What, exactly, am I looking at?** — the eyeball. Click it, then click a voxel, and the camera moves out along the normal of the face you hit and looks straight back down it: that face ends up centred **and exactly perpendicular** to the view. The distance is preserved, so it reframes without also zooming, and the voxel becomes what the cube orbits and what the wheel dollies toward.

Perpendicular is the whole point, and it is what separates this from the Inspector's **Look at this component**, which keeps the current angle and only slides. A face seen square-on is the one view in which a screen distance is a true distance along it — nothing to correct for by eye — so it is the view to line something up in, or to check a wall is flat from. The face normal arrives from `pickVoxel` in the *chunk's* voxel grid and has to be rotated into world space first; a rotated chunk is exactly where the two disagree, and where squaring up matters most.

This is also the one place the camera is allowed to point **straight** up or down. Free-look stops at 1.55 radians, because a *drag* through the pole flips the image over — but a snap never passes through anything, and clicking the eyeball on a floor means perpendicular, not 1.2° short of it. The basis stays well defined at the pole because `computeCameraBasis` swings its reference axis to `+Z` there, exactly as `rayStartDirection` does on the GPU. Clicking the cube's `Y` face gets the same exactness for the same reason.

It shares the armed-picker treatment with the palette's eyedropper — an outline round the image and a sentence at the top — and arming either disarms the other, because two one-shots both waiting for the next click would resolve to whichever branch was tested first.

The whole cluster stands down when the Viewport is too narrow to hold it, and also when it would collide with the render toggles centred on the same bottom edge. The toggles win: they are the older of the two, and widening the panel a little brings the navigator back.

### The colour language

Colour means **kind**, in every panel that lists components, from one table (`componentKindStyle`). There used to be two half-systems that disagreed — the component tree tinted `(folder)` amber and `(data)` blue, while the Library tinted scene folders pale blue — so the same colour meant "a data leaf" in one panel and "a whole scene" in the next.

| Kind | Colour | Reads as |
|------|--------|----------|
| **data** | blue | one voxel volume, at one resolution and one voxel scale |
| **grid** | teal | many blocks on one lattice — boolean ops do not apply |
| **asset** | amber | a container you can open; a folder with its own `compose.json` |
| **linked** | violet | a container you do **not** own; saving writes the reference |
| **derived** | grey | scaffolding the editor rebuilds, never written to disk |

The distinction worth paying an extra colour for is **asset versus linked**. Link-ness is otherwise invisible until somebody edits one and is surprised that another changed, which is exactly what a colour should be spent on: a property that changes what an edit *does* and that you cannot otherwise see.

The status bar along the bottom of the window carries the summary permanently: frame time, viewport resolution, scene size, active tool, and the last message. Messages also float over the viewport for a few seconds after they change, because most of them answer something the user just did *there* — reading the answer at the far corner of the screen was the whole problem with keeping them in a docked panel.

## Tools

The tool is what a left-click inside the viewport means. Without one, every new interaction has to negotiate for the same button: the gizmo wants a drag, picking wants a click, and a sculpt stroke wants a drag over the same pixels the gizmo is drawn on. A mode makes that a choice stated once rather than a modifier remembered per action.

`Ctrl` is part of every shortcut because the bare letters are already spoken for — `W`/`A`/`S`/`D`/`R`/`F` fly the camera while the right button is held, so unqualified `Q`/`W`/`E`/`R` would mean a fly-through that silently changed tool on the way past.

| Tool | | Left-click does |
|------|---|-----------------|
| **Select** | `Ctrl+Q` | Selects the component owning the voxel under the cursor. Clicking past everything clears the selection |
| **Move** | `Ctrl+W` | The transform gizmo: three arrows to translate, three rings to rotate. Clicking away from a handle still selects |
| **Sculpt** | `Ctrl+E` | Drag to add or remove voxels with a Sphere or Cube brush, to Smooth or Bump a surface, or to Extrude a whole face. `Alt`+click samples instead |
| **Paint** | `Ctrl+R` | Drag to repaint voxels with the palette's current entry, in one of five shapes (the two fills run once per click). `Alt`+click samples instead |
| **Place** | `Ctrl+T` | Drops one of seven primitives into the **open asset**, on the surface under the cursor or `Place distance` down the ray when there is nothing there. With nothing open the first one creates an asset and opens it. Clicking an item grabs it instead, and the panel's fields then edit *that* item |
| **Region** | `Ctrl+Y` | Selects voxels already in the scene — two corners of a box, a face, or a connected volume — to lift into the open asset, delete or recolour. `Alt`+click samples instead |
| **Custom** | `Ctrl+U` | Drag to run whichever [programmable brush](#the-brush-lab) is selected — the one tool whose verb is not fixed by the editor but by a Lua file. `Alt`+click samples instead |

The Custom tool is the odd one out and deliberately so: the other six do one thing each, and what a drag does under this one comes out of a file in the brushes folder. Its panel is a brush picker and that brush's settings; everything about *writing* a brush stays in the Brush Lab.

`Ctrl+Y` used to be a second binding for Redo. The keyboard-row logic wants `Q`/`W`/`E`/`R`/`T`/`Y` in order, and Redo keeps `Ctrl+Shift+Z`, which the Edit menu has always advertised alongside the alias.

A miss deselects only under Select, whose whole job is choosing: clicking past everything is how you say "nothing". Under the other tools a near miss is a slip of the hand, and losing the selection — and with it the gizmo, or the brush's target — would cost far more than it saves.

Selecting from the viewport also **opens the asset the picked component sits in** and scrolls the row into view. The contents list is one level deep by construction, so showing a selection means standing where it is — and a selection the panel meant to show it cannot show is not a selection.

### The breadcrumb

A strip along the top of the viewport reading `document › asset › … › selection`, every element clickable, and the same path the Assets panel carries over its list. The thing an edit lands on is the selection, and the only other place that says so is a highlighted row in a panel that might be scrolled away from it. A tool about to add or remove voxels needs its target stated where the voxels are.

Clicking an **ancestor** opens it, which is how you step back out of an asset you went into. The open asset is drawn amber — what an asset reads as everywhere else — and the selection yellow, because they are two different facts and are usually two different components.

### Sculpting

Hold the left button and drag. Every frame casts one ray, turns where it landed into a cell of the target component, and stamps the brush there — so the geometry appears under the cursor as it is drawn, not at the end.

| Brush | One dab covers |
|-------|----------------|
| **Sphere** | A ball of voxels centred on the cell under the cursor (radius in voxels, up to 24) |
| **Cube** | A box centred on it, aligned to the component's axes (up to 48 a side) |

**Add** puts voxels in the empty cell just outside the face the ray hit; **Remove** takes the cell that is there. Additions use the palette's current entry, exactly as Paint does, and a component **grows to fit them** — a loose Chunk becomes a Grid and a Grid expands (`applyComponentQueue`), so a stroke is not confined to the bounds the component happened to load with.

With nothing under the cursor the dab lands `Place distance` world units down the ray. That is the only way to put the first voxel into a component that is still empty, and it is also what keeps a stroke flowing when the drag runs off the edge of the object it started on.

Two more entries — **Smooth** and **Bump** — [reshape rather than place](#smooth-and-bump), and the last, **Extrude**, is not a brush at all — see [below](#extrude).

#### Why the ray ignores the stroke's own geometry

This is the whole difficulty of the tool, and it is not a subtlety — it is the difference between a brush and a catastrophe.

A stroke edits the scene it is casting into. By the second frame the scene already contains the first frame's dab, which sits *nearer the camera* than the surface did. Believe the scene and the next dab goes on top of that one, and the one after on top of that: an additive drag walks a column of voxels straight back up the ray toward the camera, a brush-width per frame, for as long as the button is held. Removal has the mirror version — the ray drops through the hole it just opened and eats a tunnel through the object.

So for as long as the button is held, the ray is shown **the surface the stroke started against**. `utils::pickVoxel` takes an optional `VoxelSolidityOverride`, called for every cell its traversal steps into — not only the solid ones, because forcing an *empty* cell back to solid is half of what it is for:

* a cell this stroke **added** reports empty, so the ray passes through and keeps finding the original surface;
* a cell this stroke **removed** reports solid, so the ray still stops there and the brush does not sink.

`sculptStrokeTouched` is that set: every cell the stroke has changed, and only for the stroke's own component — everything else in the scene answers normally, so a drag that wanders across a second object still lands somewhere sensible.

A voxel forced solid has no material to report, because it is not in the tree any more. Such a pick carries slot 0, so **never hand a solidity override to anything that reads `materialSlot`** — a sampler or the eyedropper.

#### The rest of a stroke

A stroke locks its **component**, its **mode** and its **colour** the moment the button goes down. An edit queue belongs to a component, so a drag that wandered onto a second object would otherwise start writing into it halfway through — one gesture, two undo entries, and geometry where nobody was pointing.

Dabs are **interpolated between frames** at half the brush radius. A drag is sampled once per frame, so how far the cursor travels between dabs depends on the frame rate and how fast the mouse is moving; without interpolation a quick flick lays a dotted line of disconnected blobs. Past 48 steps in one frame the stroke thins out rather than the frame stalling — a dropped frame mid-stroke is far more disruptive than a slightly sparse one.

Everything one frame produced goes out in **one** `queueVoxelAdd`/`queueVoxelRemove` + `updateScene`, for the same reason painting queues a whole stroke at once: `updateScene` forks and rebuilds every chunk a queue touches. And the whole drag is **one** history entry — a drag is a single gesture, and undoing it should put the scene back to where the button went down.

#### Smooth and Bump

The two brushes that reshape rather than place. Neither stamps anything: each runs one pass of an operator over the cells inside the brush sphere and **repeats it on a tick** — ten times a second — for as long as the button is held. That is what makes them behave the way a physical tool does, hold longer and get more, and it is why they work with the cursor standing still, unlike the shape brushes. A tick is a fixed unit of effect rather than a frame's worth, so holding for "about a second" means the same thing on any machine.

**Smooth is a majority filter** over the 3×3×3 neighbourhood including the cell itself: solid if strictly more than 13 of the 27 are (the [Strength](#smooths-strength) setting can widen that neighbourhood, and below full strength it narrows what the filter is allowed to act on). A majority filter *is* a smoothing operator on a voxel grid — it is stable on a flat surface (a surface cell sees 18 of 27 and stays; the cell above it sees 9 and stays empty), shaves anything that protrudes, and fills anything dented in. Rounding convex features and filling concave ones falls out of the same test rather than needing two.

**Bump** dilates or erodes by one layer per tick through face neighbours only, in the colour of whatever it is growing out of, followed by however many smoothing passes the `Blend` setting asks for. Dilating everything inside a sphere on its own gives a disc with a cliff around it — the bump reads as "a sphere was stamped here" rather than as the surface being pushed — and the blend has to reach *past* the brush to fix it, because the join it is rounding off is exactly at the rim.

##### Smooth's strength

One slider, `0.00`–`3.00`, and **1.00 is the line between two different things**. Below it the filter stays 3×3×3 and the strength decides which cells it may touch; above it the filter widens. The default is `1.00`, which is the plain majority filter the brush has always been, so the setting changes nothing until it is moved.

**Below 1 — how out of place a cell has to be.** Everywhere else "strength" is how far each point slides toward the smoothed result, which needs a continuous surface to slide along. Here a cell is solid or it is not, so the only thing a weakened filter can choose is *which* cells it flips, and the honest ordering is by how badly each one disagrees with its own neighbourhood. Distance from the threshold is exactly that number, and it is symmetric — a solid cell flips at 13 neighbours or fewer, an empty one at 14 or more, so both sides run from 1 (a hair over the line) to 13 (an isolated voxel, or a hole with solid on every side). The slider is a floor on it:

| Strength | Cutoff | Spares |
|----------|--------|--------|
| `1.00` | 1 | Nothing. Rounds edges and corners as well as noise — the aggressive end |
| `0.75` | 2 | A clean 90° edge (about 2), which is the first thing users notice being eaten |
| `0.25`–`0.50` | 4 | Staircase steps and gentle curvature; still fills a one-voxel pit and shaves a one-voxel spike (both about 4) |
| `0.00` | 6 | Everything but noise: a lone voxel and a fully enclosed hole are 13, and very little else reaches 6 |

**Above 1 — how large a feature the filter can see.** Past `1.00` the cutoff has nothing left to give: at 1 the filter already flips every cell it can. So the strength widens the neighbourhood instead, one step per whole number — `2.00` is a 5×5×5 majority (63 of 125), `3.00` a 7×7×7 one (172 of 343).

That is not the same as holding the button longer, and the difference is the whole point of the range. Holding reruns the 3×3×3 filter on its own output, and it has **fixed points**: a two-layer slab, a large sphere, a dome spread over a dozen voxels are all things it will never touch, because at the one-voxel scale they are already flat. A wider kernel asks the same question over a larger piece of the surface and sees them. The self-test proves exactly this — a two-layer slab survives four ticks at strength 1 untouched, and one tick at strength 2 takes the whole thing (50 solid of 125 where it wants 63).

> ⚠️ **A body thinner than the kernel dissolves rather than smooths.** That is what "smooth at a five-voxel scale" means for something two voxels thick — and voxelised meshes are usually hollow shells a few voxels thick, so a wide setting aimed at one takes it out. The Tool panel says so beside the slider.

Both halves have to be **selective rather than merely slower**, since these brushes repeat on a tick and a setting that only delayed the same verdict would converge on the same result within a second and be no setting at all. Below 1 that means a weak pass held for four ticks takes the floating voxel and still leaves the spike and the dent; above 1 it means reaching a shape that no amount of holding at 1 ever reaches.

Widening costs the kernel's volume per cell — 343 array reads against 27 — so the **colour vote deliberately stays on the inner 3×3×3** and runs only for cells actually being filled. Widening the kernel widens what counts as rough, not what a cell is made of, and a vote over 343 cells would be both wrong (colours from three voxels away are not what this cell adjoins) and, on a photo-textured scene where nearly every voxel has its own palette entry, the most expensive thing in the tool. With that, a radius-10 tick measures **0.35 ms at strength 1 and 1.00 ms at strength 3**, against a 100 ms budget.

Bump's own blend passes ignore the setting entirely and always run at 3×3×3, cutoff 1. They exist to kill the cliff at the brush's rim; a weakened pass would spare exactly that, and a widened one would reach well past it.

#### Why these two are affordable

**The cost is reading the geometry, not writing it.** Both operators ask about a cell and then about the cells around it, and neighbouring cells overlap heavily — a 3×3×3 count re-reads 18 of the 27 its neighbour just read. Asked of the tree64 one at a time that is ~28 descents per cell: half a million per tick of a radius-10 Smooth, five million a second at ten ticks. Copying the box into a flat array first makes it one descent per cell and turns every pass into array indexing, and the same tick drops to ~17k descents and **0.35 ms** against a 100 ms budget. Widening the kernel multiplies the array reads, not the descents, which is what keeps [strength 3](#smooths-strength) affordable at 1.00 ms.

Every cell of a pass is decided against the state at the *start* of that pass, gathered first and written afterwards. Deciding against a half-updated array would make the result depend on the loop's direction — a pass sweeping in +x would smooth differently from one sweeping in −x, and a bump would run away across the sphere in a single tick instead of growing one layer.

Both read the **current** geometry rather than the stroke's starting state, which is a deliberate exception to [the lie told to the ray](#why-the-ray-ignores-the-strokes-own-geometry): each pass must build on the last, or holding the button would do the same thing forever. It is safe because neither takes its *position* from the geometry — the ray still finds the original surface, which is what keeps the brush parked where it was aimed, and only the operator inside the sphere looks at what is actually there now.

#### The world lattice

Sculpting needs something painting never did: a coordinate for a cell that **does not exist yet**, because additive strokes create cells. `ComponentVoxelSpace` therefore carries the component's lattice in world space — a Grid lays its cells at `origin + R * (cellIJK * cellSize)` and each cell subdivides into `resolution` voxels per axis, so cells and voxels fall on one lattice and there is a single mapping for the whole component rather than one per cell:

```
centre(coord) = latticeOrigin + R * ((vec3(coord - coordOrigin) + 0.5) * voxelSize)
```

`componentVoxelToWorld` and `worldToComponentVoxel` are that mapping and its exact inverse. `coordOrigin` is `originCellCoord * resolution`, and is non-zero only after a Grid has expanded *downward* — which moves `grid.origin` without moving any existing geometry, and is exactly the case that gets this wrong silently.

### Extrude

Sits in the brush list but works nothing like the other two. Click a face, drag, and that whole face moves. Nothing is stamped along the path of the cursor; the shape of the result is the shape of the face, and the cursor supplies exactly one number — how far.

**What counts as "the face you clicked on"** is every solid voxel that

* lies in the same plane as the one under the cursor,
* has its face open in the same direction — part of the *surface*, not buried behind it,
* is reachable from the clicked voxel through face neighbours within that plane, and
* under the `Material` [scope](#selection-scope), uses the same palette entry.

Connectivity is 4-way within the plane, for the same reason the fills are 6-way: diagonal connectivity leaks across the gap where two faces touch only at a corner. `gatherFaceRegion` is shared with Paint's Fill face, which selects exactly this region and recolours it instead of moving it.

The voxels pulled out take **their own source voxel's material**, not the palette's current entry. Extruding a wall extends that wall; being handed whatever colour happened to be selected in another panel is not what "pull this face out" means. Which way the face goes is which way you drag — out to extend, back in to carve, and past the original face it keeps cutting — so the tool has no Mode setting, no size, and no Place distance, and the panel does not show them.

#### Why it needs no solidity override

The brush casts a ray every frame and therefore needs the [override](#why-the-ray-ignores-the-strokes-own-geometry) to keep from chasing its own deposits. Extrude fixes its face and its axis on the press and reads depth by projecting the cursor onto that axis with `closestPointOnAxis` — the same projection a translate gizmo handle uses. The geometry it creates was never in a position to mislead it, because after the press it never asks the geometry anything.

#### The layer records

`extrudeAppliedDepth` is signed and is the whole committed state: positive layers added outward, negative carved inward, zero untouched. Every frame walks it toward whatever the cursor now says, **one layer at a time**, which is what makes a drag reversible in place — pulling out to five and back to two leaves exactly the state a drag straight to two would have produced.

Each applied layer records what it *displaced*, split two ways: cells that were empty (reverse by removing) and cells that held something (reverse by writing their old contents back). The naive version — "pulling a face out fills empty space, so retracting empties it again" — is wrong the moment a face is extruded into geometry that was already there: a stair tread pushed into the tread above it, a wall pushed into a floor. Retracting would delete voxels the drag never created, and so would undo. With the records, reversing a layer is one rule in both directions.

### Painting

Hold the left button and drag. One dab covers one of five shapes, chosen in the Tool panel:

| Mode | Covers |
|------|--------|
| **Voxel** | Just the one under the cursor |
| **Sphere** | Every solid voxel within a radius of it (radius in voxels, up to 32) |
| **Cube** | Every solid voxel in a box centred on it (width/height/depth in voxels, up to 64) |
| **Fill face** | Across the one surface you clicked: same plane, face exposed, 4-connected |
| **Fill volume** | Through the solid: 6-connected in three dimensions, inside included |

All of them **only recolour voxels that already exist** — a paint brush never creates one. Adding geometry is the Sculpt tool's job, and a brush that quietly filled the empty space it passed over would be the most surprising tool in the editor. They also all stay inside the component that was clicked, because an edit queue belongs to a component.

#### The stroke

The first three shapes are **swept**, exactly like the [sculpt brush](#sculpting): every frame the cursor moves casts one ray, turns where it landed into a cell of the target component, and stamps a dab there. Repainting a wall is a sweep over it, not one click per brush-width.

A stroke locks its **component** and its **colour** the moment the button goes down, for the same reasons the sculpt stroke does: an edit queue belongs to a component, and one gesture should be one entry in the history. Dabs are **interpolated between frames** at half the shape's smallest extent (one voxel for the `Voxel` shape), because a drag is sampled once per frame and without interpolation a quick flick lays down a dotted line. Past 32 steps in one frame the stroke thins out rather than the frame stalling, and the status bar says so.

Everything one frame produced goes out in **one** `queueVoxelAdd` + `updateScene`, and the whole drag is **one** history entry.

**The two fills take no part in the drag.** They run on the press and the rest of the gesture is ignored — a bucket fill is a click everywhere else, and re-running one every frame would re-walk a region of up to four million voxels to discover the previous frame had already painted all of it. The Tool panel says which of the two the current shape is.

Where the sculpt stroke needs [an elaborate lie told to its own ray](#why-the-ray-ignores-the-strokes-own-geometry), this one needs nothing: painting never changes what is solid, so the ray sees the same surface on the last frame of the stroke as on the first. It needs no journal of original state either — `collectPaintTargets` refuses to collect a voxel that is already the colour being painted, so a voxel this stroke has covered is skipped on every later frame and reaches the undo record exactly once. The only repeats it can produce are *within* one frame, where consecutive dabs of an interpolated run overlap and the scene has not been written yet; those are dropped before the queue.

**The two fills are kept apart deliberately**, and the difference between them is the difference between recolouring a wall and recolouring the building. A volume fill spreads in three dimensions through anything it touches, so it reaches the *inside* of a shape as readily as the outside and does not stop at a corner the way someone looking at the surface expects. A face fill stays on the surface pointed at — it is the same region the [Extrude](#extrude) tool moves, recoloured instead of pulled. Both take the [scope toggle](#selection-scope) below; `Fill volume` + `Whole volume` is the one combination that repaints an entire connected model in a click, which is occasionally what is wanted and never what is wanted by accident, so it takes two deliberate choices to reach.

Sizes are in **voxels**, not world units: this is a tool that addresses the voxel grid, and a radius in world units would mean a different number of voxels per component depending on each one's `voxelScale`. The world size is shown underneath for the number you actually picture.

Under the `Material` scope both fills compare **palette entries, not colours**, so two entries that happen to hold the same colour stay separate regions — they are separate materials, and merging them would recolour more than was pointed at. Both spread through face neighbours only; diagonal connectivity leaks a fill through the gap where two walls touch at an edge.

### Symmetry

A block at the bottom of the Sculpt and Paint panels. Switch it on and every dab is mirrored as it is laid down — one gesture, up to eight copies, all of them exact.

**A symmetry is a map from cells to cells, or it is a resample**, and that one requirement decides the whole feature. The maps that qualify are the ones whose linear part is a signed permutation of the axes: there are 48 of them, they are closed under composition, and none of them rounds anything. Every mirror offered here is one, so a mirrored dab is the same cells reflected rather than a rasterisation of a rotated shape — no aliasing, no holes, and lifting a mirrored region and folding it back is byte-identical.

That set is bigger than the obvious design allows for, and the difference is exactly the case that motivates the feature. Three mirror planes give eight *octants*, so about a vertical axis they give **four** copies; the eighth only arrives by flipping top against bottom, which is not what a pillar wants. The four mirrors of a square — the two axis planes **and the two diagonals** — give eight copies about that axis, and the diagonals are signed permutations just like the axis planes are. So they are first-class toggles here rather than something to fake by rotating a plane 45°, and eight-fold detail on a pillar costs no accuracy at all.

| Want | Turn on |
|------|---------|
| A mirrored face | Mirror X (or Y, or Z) |
| Four copies about Y | Mirror X + Mirror Z |
| **Eight copies about Y** | Mirror X + Mirror Z + Diagonal ZX |
| Sixteen | the above + Mirror Y |

Anything else — six-fold, eight-fold *rotational* — is a rasterise and is not offered yet.

#### The frame belongs to the component

One frame per component, in that component's own voxel lattice. Symmetry is a property of the thing being sculpted rather than of the tool, so selecting something else brings *its* mirrors back rather than dragging the last one along, and storing the origin in the component's own coordinates is what keeps every mirror an exact integer map with no conversion in the inner loop.

Mirrored cells are written into the stroke's own component and nowhere else. Symmetry across sibling components — a stroke on one part landing in the next — is a later stage; see [`docs/plans/symmetry_system.md`](../../docs/plans/symmetry_system.md).

#### Half a voxel is a real choice

The planes are given as positions in the voxel grid and drag in half-voxel steps, because the half is the whole question:

| Plane at | Runs through | Effect |
|----------|--------------|--------|
| `8.0` | a column of cell centres | that column is its own mirror image — unpaired, written once, no doubled seam |
| `8.5` | a cell boundary | nothing is fixed; every cell pairs off |

**Centre on content** picks the right one for you: a body 16 voxels wide gets a boundary plane and one 17 wide gets a centred plane, because the parity follows from the extent rather than from a setting.

It also decides whether a diagonal is available at all. The diagonal of a plane maps cells to cells only when its two axis planes agree in parity — otherwise it would take cell centres onto cell corners — so the toggle greys and says which two planes disagree. Nudging either by half a voxel fixes it. That is a statement about what the lattice permits, not a warning that you have done something wrong.

#### The planes are drawn because the failure is invisible

A frame whose origin has drifted off the geometry does not look like broken symmetry. It looks like a second object being built in empty space some distance away, often outside the view — and neither the copy count in the panel nor the result on screen can tell you, because the result is exactly what the frame asked for. So the mirrors are drawn where they stand, dashed (a solid rectangle through a model reads as geometry), each axis in its own colour and the diagonals in amber.

They are checkboxes and three numbers rather than a plane gizmo on purpose: a gizmo would be a fourth thing negotiating for the left button in a viewport where the tool, the picker and the transform handles already do.

#### Where it applies

**Sphere and Cube** replicate cell by cell. **Smooth and Bump** replicate by *centre* — each copy runs the same filter over its own neighbourhood and reads the geometry actually there, where mirroring an operator's output would reflect one site's result onto another site's shape. Copies whose boxes overlap near a plane run in sequence and read each other's writes, so the result there is order-dependent by a cell or two; that is the same exception these two already make in reading current rather than stroke-start geometry.

**Paint** replicates the dab's centre, and for the two fills its **seed** — a mirrored fill runs the traversal again from the mirrored voxel and finds whatever region is there, which stays right even where the two regions are not congruent. Under the `Material` scope the copy spreads through the *original's* material rather than through whatever sits at its own seed.

**Extrude** mirrors whole faces rather than dabs, because it has no dab — it fixes a face and a direction on the press and then reads one number. So the press gathers a face at each mirror image of the seed, and the one depth the cursor supplies drives all of them, **each along its own normal**. That last part is what makes the far copy a mirror rather than a translation: on the other side of a plane, "outward" points the other way, so a symmetric shape grows at both ends instead of growing at one and being carved at the other.

The mirrored faces are **gathered, not derived**. Mapping the primary face's coordinates would assume the geometry over there is the mirror of the geometry here, and where it is not, the extrusion pushes cells that are part of no surface at all. Re-running `gatherFaceRegion` from the mirrored seed asks the geometry instead of telling it — the same choice the mirrored fill makes about its seed. A mirror image with no face under it simply contributes nothing.

Faces are made **disjoint** as they are gathered. They can overlap, and the case that matters is common: a face straddling a mirror plane is its own image, so the mirrored gather returns the very same region. Left alone it would be extruded twice — written twice into one layer record and counted twice in the undo.

#### Why the mirroring happens where it does

The copies are made in `stampSculptDab`, on the way into the stroke journal — not in `applyVoxelSculpt` on the way out.

Everything downstream of `sculptStrokeOriginal` is derived from it: the undo and redo lists, and [the lie told to the ray](#why-the-ray-ignores-the-strokes-own-geometry). Mirror after the journal and the copies are written but never remembered — undo restores one copy of eight and leaves seven behind, and an additive drag starts climbing its own mirrored deposits back toward the camera at seven sites at once. Mirror before it and undo, redo, the ray override, one-history-entry-per-gesture, and the dedupe that stops a cell *on* a plane being written twice all come for free.

The group always holds the identity as its first element, so symmetry off is the same loop running once. There is no second code path.

**Cost scales with the group.** Eight copies is eight times the tree64 descents per dab — a radius-24 sphere is ~58k cells before mirroring — and for Smooth and Bump it is eight snapshot boxes per tick (~2.8 ms against a 100 ms budget). The panel states the multiplier.

### Assets: the Place and Region tools

The workflow this is built for, stated the way a user states it:

> You have a list of assets. You can move the shapes around in these assets and use the existing paint/sculpt tools. You can switch assets by just clicking on the other asset in the list. You have per-asset save/load, and you can copy other assets into the asset you are working with.

The decision the rest follows from:

> **There is only the asset. It is a folder with a `compose.json`. Everything else is a view of one.**

A scene is an asset. A stack of booleans is an asset. A component of an asset is a thing you can open and edit as an asset in its own right. There is no top and no privileged kind — which is what the format has always said, and what the editor spent a while disagreeing with by inventing a *scene*, an *assembly* and a *folder* as three different things over one file structure. Every confusion downstream was a symptom: an Assembly panel that sat on the left describing a container that may or may not have existed, an "Assemble" tool named after a noun in a list of verbs, and a `resolveActiveAssembly` that existed at all because "which container am I adding to?" had three possible kinds of answer.

Concretely:

> **An asset is a live `ComponentKind::Asset` component whose contents are its children, and whose voxels are the boolean fold of those children in order.**

The editor's job reduces to three verbs, and the Assets panel is where all three live:

| Verb | Means |
|------|-------|
| **Open** | Make this asset the edit root. What is listed, what the breadcrumb ends at, and where new geometry lands |
| **Copy** | Bring another asset into the open one, where it becomes an ordinary component |
| **Save** | Write the open asset back to a folder |

A **part** gets to be a real `ComponentKind::Chunk` component so that it inherits, at no cost:

* **rendering** — the raycaster already draws any component and already honours `ChunkHeader::rotation`, so a rotated part is visible exactly as it will resolve;
* **the transform gizmo**, which already operates on `editor.selectedComponent`;
* **the selection outline** (`collectLeafChunks`);
* **Sculpt and Paint working on a part before it is baked**, which is the whole of "edit it how you please" for free.

An **asset** gets to be a real Asset component so that it inherits the **hierarchy**: parent/child is already in `ComponentRecord`, `localPosition`/`localRotation`/`localScale` already compose down it, `getComponentWorldMatrix` already walks it, and moving a parent already moves its children. A CSG stack is a parent with an ordered child list and one enum per child, and almost all of that already existed and was load-bearing elsewhere.

So there is exactly **one selection** (`editor.selectedComponent`), **one list** (the Assets panel), and **one container pointer** (`editor.openAsset`). A part is an ordinary component and the gizmo needs no idea that any of this exists.

#### Expand, Open, Select — three verbs, three hit targets

The distinction the panel is built on, and the one the tree it replaced could not make. Expanding a hierarchy node and selecting it used to be the same gesture wearing two hats, so opening a folder to look inside it also retargeted everything else.

| Gesture | Does | Changes |
|---------|------|---------|
| The **arrow** | Expands the row to show what is inside it | Nothing but the view |
| The **name**, or `open` | Selects. Double-click, or the trailing `open` button on an asset row, opens it | The selection; opening also changes the edit root |
| **Shift+click** | Adds the row to the pick, for the verbs that act on several at once | The pick; a plain click clears it |
| **Drag onto a folder** | Puts the component inside that asset | Its parent — not its place in the world |
| **Drag onto anything else** | Reorders, which the fold cares about | The stack order |
| **Right-click** | Rename, Duplicate, Move to, Copy to, Remove | — |

Every row carries those, including the **expanded peek rows** under a folder. They used not to: `drawNestedRow` had no context menu and no drag handles at all, so anything you could see by expanding a folder in the top-level list could be selected and opened and nothing else — while the very same component became fully editable the moment you opened its parent instead. One component, two rows, two different sets of verbs depending on which one you happened to be looking at. Both kinds of row now go through one `drawRowVerbs`, so they cannot drift apart again.

A peek row is a *view* of a list rather than the list itself, so it has no stack position and does not reorder. Everything else a row can do, it can do.

#### Moving a component into an asset

Dragging a row means two things, and what it lands on decides which. Onto a **folder** it means *put this inside that asset*; onto anything else it means *reorder*. So a folder is not a reorder target — drop on one of its neighbours to reorder past it. Putting-inside is the more useful reading of that gesture and the one every file manager already teaches. **Move to ▸** in the row menu is the same verb for when the target is not on screen, since the list is one level deep and a drag can only reach a sibling.

This is the verb the panel was missing outright, and the reason a finished pillar could not be got into a temple: `setComponentParent` composes the transforms so a moved component does not budge in world space, and the contents list *is* the child list — but nothing in the editor called it except `wrapRootStack` at load. An asset could be opened, renamed, duplicated and deleted, and could not be put anywhere.

**It arrives as `Place`.** An imported asset does the same, for the same reason: it comes at its own voxel scale, and folding it into its new home's lattice would resample it before you said you wanted that. Setting the row to Union afterwards is one click, and it is the click that says "yes, resolve this into my lattice".

**Copy to ▸** is the same list of destinations asked about a new object rather than this one: duplicate, then move the copy. Twelve pillars in a temple is one pillar copied eleven times, and the geometry is shared until one of them is edited — `GeometryBlob` is refcounted by source and `forkBlob` has a sole-owner fast path.

The list is always **one level deep**, plus whatever has been expanded to peek at. Depth is navigated by *opening*, which is what keeps the panel readable on a scene with ten thousand components: you are never scrolling a tree, you are standing somewhere in it. **Isolate** takes it further and draws only what is inside the open asset — working on one tower of a cathedral should not mean rendering the cathedral.

#### The resolve is derived, not declared

There is no create-an-assembly verb and no destroy-one, because there is no assembly. `syncResolves` runs once a frame: an asset whose contents carry boolean ops has a `Resolve`, an asset whose contents do not has none, and nothing else decides. The open asset gets one regardless of its ops, because it is where the next primitive lands and it needs a lattice before there is anything on it to derive one from.

That replaced four separate mechanisms that all existed to answer *which container is this?* — a create verb, a destroy verb, an active-assembly pointer, and a load-time adoption pass. The ops on disk already answered it. The one thing that is still not derivable is `wrapRootStack`, which runs at load and edits the graph rather than reading it: `saveComposeToDisk` writes a node's children as the folder's component list, so an asset saved as *its own* folder comes back as root components carrying ops with no parent to own them, and they are given one.

#### The flow

1. **Open.** Click an asset in the Assets panel, or start on the document and let the first placement make one. The breadcrumb over the list and over the viewport says where you are.
2. **Place.** `Ctrl+T`, then click in the viewport. A primitive is seated against the face under the cursor, or at `Place distance` down the ray when the click went past everything — which is the only way to put the first shape of an asset into empty space. With nothing open the click creates an asset and opens it, so the tool never has to ask what you are editing before you have made anything.
3. **Repeat.** Every later click adds an item at the current kind and size, into the same asset. Drop the pillar, drop the arch, drop the sphere you mean to carve with — no commit in between, no target question, no mode.
4. **Look at the result.** The viewport shows the **resolved** asset: its contents folded together in its lattice, rebuilt when everything settles. **Show sources** switches to seeing the shapes themselves. The default is the result, because the result is what is being made.
5. **Arrange.** The gizmo, the arrow keys, the Inspector's numeric fields. An item is an ordinary component, so all three are the ordinary ones.
6. **Boolean.** The contents list *is* the stack: one row per item in evaluation order, each with its op, drag to reorder.
7. **Copy.** Bring another asset in from the Library as a Copy, a Bake or a Link. Once it is in, it is an ordinary component of the open asset and every adjustment after that happens in final context — which is why there is no prefab-edit mode and no need for one.
8. **Bake.** To one Data, or into another component.
9. **Sculpt.** The baked result is an ordinary component and every tool already applies.

#### Ops

`projv::BooleanOp` — the same enum the `ComponentRecord` carries and `compose.json` round-trips, rather than an editor-local "merge mode", because it is a property of the part rather than of the act of committing.

| Op | | Does |
|----|---|------|
| **Place** | `.` | Not folded. Keeps its own geometry and its own voxel scale, and survives a bake as a separate component |
| **Union** | `+` | Adds the item's cells to the result. Its own colours win |
| **Subtract** | `-` | Takes the item's cells out. A sphere subtracted is a crater |
| **Intersect** | `&` | Keeps only the cells the item and the result share. The result's colours survive |

#### The Place panel edits the selection, or the next shape — never both

Two sets of values, and it matters that they are two. `editor.shape*` is the **template** a new shape
is made from; a placed shape's recipe (kind, dimensions, hollow, wall) lives on **its own `Part`** and
is what the panel edits while it is selected.

They used to be one set, and that was a defect rather than an untidiness. The fields kept the last
selection's numbers, so clicking a second shape and nudging anything resized *that* shape to the
first one's dimensions — and changed its kind too, silently, since `regeneratePart` read the panel
for that as well. It reads nothing but the `Part` now, so every caller gets "rebuild this shape"
rather than "apply the tool panel to this shape".

**The kind is read-only on a placed shape.** A box is a box; "make this box a sphere" is not a resize,
it is discarding one object and making another, and offering it inline as a dropdown makes an
irreversible substitution look like a setting. Placing a sphere is the way to have a sphere.

The op radio follows the same rule and says which it is on: **Combines as** when something in the
open asset is selected (editing that component's `op`, the same value the contents row shows — one
value in two places, not two values), **Drops in as** when nothing is. The snap controls, `Place
distance` and the material row are template-only either way, because none of them is a property of a
shape that already exists — free placement, which *is* one, lives on the component and is shown in
the Inspector instead.

The glyphs are ASCII on purpose. They used to be `∪` `∖` `∩` `·`, which are outside the default ImGui font's range — so every op control in the editor rendered the same missing-glyph box, four different operations that all looked alike, in the one place where telling them apart is the entire job.

That distinction is the whole difference between this and what it replaced. A merge mode said what a stamp would do *to a target*, so two floating stamps had no relationship to each other at all and "box minus sphere" could not be said until the box was already committed scene geometry. An op says what a part does *to the parts above it*, which is the thing a user is actually thinking about, and it is expressible from the first primitive onward.

**The first contributing row seeds the accumulator whatever its op says.** An empty accumulator intersected with anything is empty, and an empty accumulator subtracted from is still empty, so a stack whose first row is Intersect or Subtract would silently resolve to nothing at all — an outcome with no visible cause. Seeding makes row zero mean "this is what we start from", and the panel greys that row's op control to say so.

Order matters, so rows drag to reorder: which row a subtract sits on is the difference between a hole and nothing at all.

**A Subtract with nothing above it promotes the row above from Place to Union.** Seeding makes row zero mean something, but it also means a stack whose only folding row is a `Subtract` folds to the subtracting body — the thing that was meant to cut the hole, drawn as the result, with the object it was meant to cut still drawn beside it as a placement. That is the state you land in the moment you compose two finished `.data`s: a merged asset holds one *placed* row, because a placement is not a composition, and the fold is right to skip it. So when a row is set to Subtract or Intersect and nothing above it folds, the nearest placed row above becomes a `Union`, the status line says which row moved and why, and one Ctrl+Z puts both ops back — the promotion and the op change are a single history entry, so the assist can never outlive the action that asked for it. With nothing above it at all the row still seeds, and the status line says that too rather than leaving it to be inferred.

It is an assist and not a rule: a row above that already folds means the Subtract has something to compose against, and a placed row is a deliberate statement that it is not part of the composition. Nor is it done by having the fold read a leading `None` as `Union` — `compose.json` writes that row as `none`, so the editor's picture would disagree with the file and any other loader would read the placement rather than the fold. The promotion changes the document, so the document stays the truth.

#### The resolve

Two steps, and the split is what makes an asset affordable to arrange:

1. **The pull.** Each item's cells, in the asset's lattice. Cached per part on the transform it was computed for, so nudging one part in a stack of ten re-walks one part.
2. **The fold.** Those cell sets combined left to right by their ops into one accumulator.

The lattice is derived from the **asset node's own world frame**, not from the result chunk. The result's origin moves whenever the fold's bounding box moves, so using it as the lattice would shift the coordinate system every time a part was nudged — and a fold whose coordinates mean something different from one frame to the next cannot be cached, compared, or reasoned about.

#### Every edit to a part has to invalidate, in both directions

Sculpting, carving and painting a part all change what it folds to, so all three drop the part's cached cell set and mark the result stale. Two of them used not to.

`applyVoxelSculpt` did its invalidation inside its `if (add)` branch, so **carving a part never reached the fold**; and `applyVoxelPaint` did no invalidation at all, so **recolouring one never did either**. The palette poll below does not cover the second case, because painting with an entry that already exists changes no palette — which is the most ordinary paint there is.

Both produced the same misleading symptom, and it pointed away from the cause: nothing happened until the part was **moved**, at which point the transform funnel invalidated through `noteComponentMoved` and every earlier edit appeared at once. It read as "the result only regenerates when I move it" — a problem with the *rebuild*, when it was a problem with what marked the rebuild necessary.

`ASSEMBLYTEST` 13 asserts all three directions against the fold *and* against the staleness flag. The flag matters on its own: a check on the fold alone passes on a rebuild that only ever happens by accident, and painting with a colour not yet in the palette sneaks through the palette poll even with the invalidation gone.

The result is rebuilt only once everything **holds still** for `ASSEMBLY_SETTLE_SECONDS`. While an asset is moving its result comes down and its contents come back, which is both the honest picture (a fold from two frames ago is not what the stack says now) and the responsive one: the drag moves the part itself, at frame rate, with no fold in the loop. That gate is now load-bearing rather than a nicety — with the result as the default view, without it every frame of every drag is a fold.

#### The pull must not be a push

This is the one part that is easy to get wrong in a way that looks almost right.

Forward-mapping each part voxel to a lattice cell (**push**) leaves holes. A rotation is not area-preserving on a lattice, so two source voxels can land in one cell while a neighbouring cell receives none — and on a hollow shape, whose walls are one or two voxels thick, those gaps perforate the surface. The result is a rotated shape you can see through.

So the resolve iterates the **lattice** cells instead (**pull**): take the part's oriented bounding box, walk every lattice cell inside it, inverse-transform that cell's centre into the part's voxel space, and sample. Every cell gets exactly one answer, so the surface is closed by construction.

The cost is therefore the **volume of the oriented bounding box**, not the part's voxel count — which is why that box's volume is what the budgets cap, and why a bake that would exceed `ASSEMBLY_MAX_BAKE_CELLS` is refused rather than truncated: a fill that stops halfway leaves a smaller fill, but a bake that stops halfway leaves half an object.

With `Square to world` on the pull degenerates to the exact integer remap — the inverse transform is a signed axis permutation and every lattice cell in the box maps to exactly one source cell — so one code path serves both modes.

A part is read into a dense occupancy-and-colour box once before the walk, so the inner loop is a bit test rather than a tree64 descent. It is read back **from the scene** rather than from the list that built it, so a part that has been sculpted or painted resolves as it looks — which is the whole point of it being a real component.

#### Snapping: one grid for the document, and the objects allowed off it

The snap controls live in the **Place tool's** panel, under the size fields and above the material row. They used to sit on the container, in a panel on the far side of the screen from the arrow keys and the gizmo that actually turn things — a setting about *how you place* filed under *the thing being built*, with nothing on screen connecting the two. They are a property of the tool, so they are on the tool.

**One grid for the whole document.** `Snap to grid` quantizes a component's **world** position onto a single lattice — world origin, world axes, `documentSnapVoxel` apart. This is not what it used to do: snapping quantized `localPosition` against the component's *own asset's* lattice, which got two things wrong at once. A component whose parent had no resolve never snapped at all — that is every component at the document root, the level at which a scene is actually composed. And each asset carried its own lattice phase, so two objects in two different assets could each be perfectly snapped in their own frame and still be half a voxel out of step with each other. Two finished `.data`s that would not line up was the reported symptom.

An asset already sitting on the document grid at the same voxel scale produces exactly the numbers it produced before, so nothing that worked has changed.

`Step` is how many voxels one position is — one for detailing, 16/32/64 for standing modular pieces against each other. `Grid` is the voxel size itself: derived from the finest voxel scale in the document until you set it, then pinned, so importing a finer asset later cannot re-phase the grid under what is already placed.

**`Square to world`** is separate, because the two are wanted separately. On, rotation is a multiple of 90° about a **world** axis, so the fold is a **1:1 integer remap**: every source voxel lands on exactly one cell, nothing aliases, and lifting a region and dropping it back where it came from is byte-identical. Off, free rotation, and the fold rasterises the rotated part into the lattice. That is not a degraded fallback — it is the point of the setting. A wedge meant to sit at 30° in the final geometry, or a tree placed at its own angle so a dozen copies do not read as a dozen copies, can only be made this way.

##### Freedom is a property of the object

The controls above are the default for **new** placements. What an existing component does is recorded on the component, in `freeComponents`, and that distinction is not cosmetic.

Snapping runs **inside `applyComponentTransform`**, not in the gizmo. That is the funnel every path that moves a component goes down — the gizmo, the arrow keys, the Inspector's fields, and the undo and redo of all three — so all of them pick up snapping and cache invalidation, and pick up exactly the same ones. It is idempotent, so replaying a history record is not a second nudge.

But only while the setting has not changed underneath the record. With a global flag alone: turn snapping off, stand a tree at 30°, turn it back on to place the next wall, and the tree is straightened by the next arrow-key nudge or by an undo that happens to touch it — silently, by an action that had nothing to do with rotation, long after the decision that made it.

So there are three ways to be off the grid, and they differ in how long they last:

| Layer | Mechanism | Lasts |
|-------|-----------|-------|
| Gesture | Hold **Alt** while dragging the gizmo. | The drag — and it marks the object, see below |
| Object | `freeComponents`, shown as `~free` on the contents row. | Until cleared |
| Default | The panel's controls. | New placements |

**An Alt-drag also marks the component free**, and that is deliberate rather than incidental. Alt alone would last exactly as long as the gesture — right for the gesture, wrong for the object: the drag's own undo record holds an off-grid value, so redoing it without Alt held would push it back through the funnel and straighten the pose the drag existed to create. The pre-drag flag is latched in `gizmoDragStartFree` for the same reason the anchor is: every frame after the first would otherwise read what this drag just wrote and record an undo that cannot clear it.

The Inspector shows **Snap to grid** (clears the flag and rounds the component) or **Place freely** (sets it without a drag), and the Place panel's **Pull contents onto the grid** does the whole open asset at once. That last one used to fire as a side effect of ticking the snap checkbox; now that freedom is per component, a setting that silently overrode every deliberate pose in the asset was the wrong shape, so asking for it is its own button.

> **Known gap:** `freeComponents` is editor-side, so it does not survive a save. The pose itself is safe on disk — it is just a transform — but the exemption is lost, and the first nudge after a reload straightens it. The fix is an optional `"snap": false` field on the component in `compose.json`; until then the per-object guarantee holds within a session only.

##### The one thing the gizmo has to know first

With one exception, and it was a bug for as long as snapping existed: **a rotate drag moved the object before it turned it**, so an origin did not survive a rotation.

The rotate handle compensates its position so the pivot stays put — it solves for the `localPosition` that holds the pivot under the rotation being applied. It was solving that against the *free* rotation read off the cursor, while the funnel went on to snap the rotation to the nearest 90° afterwards. So the compensation held the pivot for a rotation that never landed, leaving the component displaced by `(R_snapped − R_free) * pivot` — as large as the pivot's own distance from the origin. On screen: the object slides steadily away while the ring is dragged, and jumps into orientation at each 90° step.

The rotation snap is available on its own as `latticeSnappedRotation`, and the gizmo now snaps *before* it compensates. The funnel still snaps on the way through, which is a no-op on an already-snapped value — so this is the gizmo asking the funnel a question, not a second copy of the rule, and the arrow keys, the Inspector and the undo of all three are untouched.

The position still rounds to the lattice *after* the compensation, so a rotation about a content centre can still move the origin by up to half a voxel. That one is not a bug: the compensated position is not generally on the lattice, and landing on the lattice is the invariant that matters more.

#### One asset, one lattice

Every item must share a voxel scale or the fold is a resample, so the **asset owns `voxelScale`** and an item inherits it at creation. This is a real constraint and it does not go away — but it is the invariant of the `.data` file a bake has to produce (one `resolution` and one `voxelScale` shared by every block in it) rather than something the editor imposes to keep the fold cheap, which is a good sign about it rather than a bad one.

#### Baking

One verb, three destinations. Committing used to force one choice across two unrelated axes: "Merge" meant *resolve to voxels* **and** *write into somebody else's grid*, while "Keep as component" meant *do not resolve* **and** *become your own object* — so the fourth combination, resolve to voxels as an object of my own, was the one thing you could not ask for, and it is the one this flow is made of.

| Bake | Does |
|------|------|
| **Merge to one Data** | Resolves the contents to voxels as one `.data` **inside** the asset. Shift+click rows to merge only some of them |
| **Into `<selected>`** | Writes the resolved form into another component with its own op — so carving a crater still works, and now the carving tool can be a composed form rather than one primitive |
| **Save…** | Writes the open asset itself — every item with its op — as a compose folder. It re-opens as the same editable asset, which is what makes a bake something you can come back from |

##### Merging does not dissolve the asset

It used to, and that one line was what stranded a finished asset. The merge replaced the node with a bare Chunk at the node's *parent* — and a Chunk is not a folder, so `Save as asset` (gated on `kind == Asset`) went grey on it, and no verb could put a loose component into another asset. **Finishing a pillar was the act that made it unusable in a temple.**

The container is the thing the asset system manages, so merging its contents must not remove it. The merged `.data` becomes the asset's *content*: the asset stays an asset, still openable, still saveable, still able to take more components later.

When the whole stack is merged the result arrives as **Place** — nothing is left for it to compose with, so the asset simply holds one placed `.data` and `syncResolves` retires the resolve. When only some rows are merged the result inherits the op of the first row it consumed, and takes that row's position in the list, so a partial merge is not also a reorder.

##### Merging some of the contents

Shift+click rows to pick several, then **Merge N to one Data**. The rest of the contents are left alone — which is the point: an asset is allowed to hold several components, and merging is something you do to a few of them rather than the act of finishing it off. A plain click clears the pick, so there is no multi-select mode to be stuck in.

A picked **Place** row *is* merged, where the whole-stack merge leaves placed rows alone. The difference is deliberate: a Place row is not composition and the format has always let it survive a bake, but a row you deliberately picked and asked to become one `.data` is a statement that it should be in there — and it is drawn on screen, so leaving it out would merge less than what is in front of you.

**A bake lands in the middle of its chunk, not in a corner.** A chunk is a resolution cube and a form
almost never is: a 24-voxel shape needs a 64 chunk, so writing it at the chunk's own origin leaves it
jammed into one corner with 40 voxels of nothing on each far side. Everything derived from the chunk
rather than from the voxels then describes that corner — the gizmo pivot most visibly — and the shape
you are holding is nowhere near the handle you are dragging. `centreBakeInChunk` is the same answer
`centrePrimitiveInChunk` has always given a placed primitive, and the offset it returns is taken back
off the component's origin so the object does not jump by half a chunk at the instant it is baked.

Each baked voxel carries **the part voxel's own colour**, not the palette's current entry — the same principle Extrude follows in taking its source voxel's material. A lifted region keeps its pattern; a shape sculpted in two colours keeps both.

**The undo record is exactly Extrude's**, and for exactly the same reason. Displaced cells are split two ways: cells that were empty (reverse by removing) and cells that held something (reverse by writing the old contents back). The naive version — "adding fills empty space, so undo empties it again" — is wrong the moment a form lands on geometry that was already there, and deletes voxels the bake never created.

Undo of a bake also **brings the whole list back**, item for item, at the transforms they had, through `NodeRecipe`. A bake that could not be unbaked would make every arrangement a one-way door. The recipe carries each part's *voxels* rather than only its primitive, because a part can be sculpted and painted before the bake and rebuilding from the primitive would silently discard those edits — and it carries the primitive too, so a revived part is still resizable.

Unlike a stamp's placement, **moving a part is an ordinary undoable edit**. The old exception — aiming stays out of the history — existed because a stamp was destroyed by its merge, so an undo entry pointing at one would have slid an invisible object around instead of putting geometry back. A part persists, so the exception went with the thing that forced it.

#### `op` on disk, and the loop closing

`ComposeComponent::op` is written as `none` | `union` | `subtract` | `intersect`, and **omitted entirely when it is `none`** — so a plain placement list comes off this writer looking exactly like the ones already on disk. The default of `none` is what makes the field backward compatible in both directions: it reinterprets nothing already written, and a loader that does not know the field reads a composed asset as its placed parts, which is a degraded but coherent picture.

Because assets recurse, that one field gives **nested CSG for free**: subtracting a whole sub-asset is a child of `type: asset` with `op: subtract`, and `loadComposeFromDisk` already walks it. An Asset item resolves as the union of the leaves under it, and a sub-asset resolves to its own result first — one recursive walk rather than a second evaluator.

Loading brings a stack back editable rather than inert, and it costs no load-time pass to do it: `syncResolves` notices, on the frame after the load like on every other frame, that an asset's contents carry ops. The one thing that does need a load-time step is `wrapRootStack` — see above. Without it, "save as an asset" would be a one-way door with a reassuring name, and it would fail silently, because every item would still be sitting there in the list doing nothing.

`utils::instantiateComposeInto` grafts a compose folder into a scene that is already open, remapping every handle it carries: component handles, chunk handles, grid indices, geometry pool indices, and the loose list. The Library panel's **Bring into open asset** is that, and it makes an asset on disk the **third source an item can come from** — identical in every way that matters to a primitive and a lifted region: a thing with geometry, a transform, and an op.

```
        ┌─────────── sources ───────────┐
        │  primitive (procedural)       │
        │  lifted region (from scene)   │──▶  an item in an asset  ──▶  bake  ────┐
        │  asset (from disk)  ◀──────────────────────────────────────────────────┘
        └───────────────────────────────┘
```

The machine's output is its own input. Bake a buttress, copy it into the cathedral, subtract a window from it. That is the thing that makes the flow feel like modelling rather than like committing edits.

An imported asset arrives as **Place**, deliberately: it comes at its own voxel scale, and folding it straight away would resample it before the user has said they want that. Setting the row's op to a boolean is one click, and it is the click that says "yes, resolve this into my lattice".

#### Region

The Region tool selects voxels that are already in the scene and lifts them into the open asset. Once lifted it has a stack row, an op, a gizmo and a bake, exactly like anything else — which is the payoff of a part being one thing with three sources rather than three things that look alike.

| Selector | Picks |
|----------|-------|
| **Box** | Click one corner voxel, then the opposite one. A live wireframe between the two makes the second click aimable |
| **Face** | `gatherFaceRegion`, unchanged — the whole coplanar, same-facing, 4-connected surface |
| **Volume** | The paint fill's breadth-first traversal, returning its bitset instead of painting it |

A selection is **bounds plus one bit per cell**, not a `vector<ivec3>`: the volume fill already established why, since a million-voxel selection is 128 KB one way and 12 MB the other, and the bitset also answers "is this coordinate in the selection?" in constant time, which the lift and the delete both ask once per voxel.

**Copy** lifts and leaves the source; **Cut** lifts and removes it in one history entry; **Duplicate** lifts a copy offset by one bounding box. **Delete** and **Fill** act on the selection in place. Removing a cut part puts the source voxels back, or a cut would be a delete with a misleading name.

A lift builds a fresh Chunk through `queueVoxelAdd` — never `utils::duplicateComponent`, which does not handle Grid components, and a selection routinely lives in one. It is positioned so its lattice *coincides* with the source's, so the part appears exactly over the original with no visual jump, and folding it straight back is a byte-identical no-op.

#### Duplicate

`Duplicate` on a contents row copies the component beside the original — `DUPLICATE_OFFSET_VOXELS`
(four) along the lattice's local X, so the offset is a whole number of voxels and the grid has
nothing to undo. A few voxels rather than one bounding box, the way a lifted region is offset: the
point is that the copy is *findable*. Exactly on top of the original it is invisible and every later
click hits whichever of the two the ray reaches first, so "nothing happened" is the only available
reading.

It carries the editor's own record too, which the engine helper cannot know about: a duplicated
primitive is still a primitive, with the same kind and dimensions, and still resizes. `cutFromSource`
is deliberately dropped — a cut's restore list belongs to the one component that made the cut, and a
second component offering to put the same voxels back would put them back twice.

Four engine bugs sat under this, and the first explains why a duplicate used to be *invisible*:

* **`duplicateComponent` re-baked the copy with its parent's world matrix, not its own.** A duplicate
  carries the source's `localPosition`, so the copy's chunk headers were written at the parent's
  origin instead. Its transform fields agreed with the original while its geometry — and the
  selection outline and gizmo pivot, both derived from those headers — sat somewhere else entirely.
  The same mistake, at a third site, as the one `setComponentParent` had; `getComponentWorldMatrix`
  is the form that cannot make it, because it ends at the handle inclusive.
* **`op` and `externalSource` were not copied.** Without the first, a duplicated child of a boolean
  stack came back as a plain placement — the copy of a subtracted window filled the hole it was cut
  from. Without the second, a duplicated link quietly became a copy, which is the one distinction the
  two modes exist to make.
* **A double `refCount++` on the fork path.** `forkBlob` has a sole-owner fast path that returns the
  source index without copying — a genuine second owner, worth counting — but when it *does* copy,
  the fresh blob already carries `refCount = 1`, and counting it again pinned the blob for the life
  of the scene.
* **A reference into `scene.components` held across a `push_back` onto it.** `duplicateComponent`
  took `const ComponentRecord& src`, appended the copy — which can move the vector's buffer — and
  then read `src.kind` and `src.children` through that now-dangling reference to drive the recursion
  into the source's children. A use-after-free, and a quiet one: a freed block usually still holds
  the old bytes, so it read correctly essentially always. Duplicating a **nested** asset — a temple
  holding a pillar, which is what the contents list is made of once anything has been moved into
  anything — is the case that walks furthest down it. The fix scopes the reference so that a later
  `src.` does not compile, and drives the recursion from a copied child list, which is needed anyway
  because each recursion appends to `scene.components` again.

  `ASSEMBLYTEST` 20 calls `shrink_to_fit()` on `scene.components` before duplicating, so the append
  is guaranteed to reallocate. That is a **trap, not a proof**: the check passed with the bug present
  and the reallocation forced, because the stale read still returned the old bytes. It is there so
  the reallocating path is exercised on every run rather than only on unlucky ones.

  `setComponentParent` was checked for the same shape and does not have it — it pushes to a
  *children* vector, not to `scene.components`, so its reference stays valid.

`ASSEMBLYTEST` 18 checks the **raw engine call on its own**, before the editor's verb. It has to:
`duplicateComponentInEditor` follows every duplicate with a transform (the nudge), and
`setComponentTransform` re-bakes correctly, so it silently repairs the very thing the check is for. A
test that only went through the editor verb passed with the engine bug fully intact — which it did,
once, before this was split.

#### Two things worth knowing

**A component is created per placement and deleted on removal.** Deletion is soft — handles are indices, so a record can only be unparented and renamed `__deleted__`, never erased. An earlier design pooled these to avoid that cost and it was the wrong trade: a pooled component is a *live row in the contents list* that outlives what it held. A leftover node is a bug; a few hundred bytes of unreclaimed record is a known cost of the handle model.

The one exception is a size drag on a *procedural* part, which refills the same component in place — that runs every frame the field is dragged. Only a change of resolution (fixed for a component's whole life) needs a new component, and two things have to survive that move, neither of which is visible from the result: the **selection**, or the next field edit acts on nothing and the part you are adjusting silently stops being the one you are adjusting; and the part's **position in the stack**, since a fold is ordered and a resize is not a reorder.

The selection hand-over has to be asked for *before* the delete, not after. `deleteComponent` clears any editor state pointing at the handle it is killing, so by the time it returns `editor.selectedComponent` is already `INVALID` and a test against the old handle can never match. The default part is 16 voxels and 16 is exactly its resolution, so the **first** tick of a size drag crosses a boundary — this is the common path, not an edge case, and `ASSEMBLYTEST`'s last check is there because getting it wrong looks like nothing at all until you try to make a second adjustment.

**`deleteComponent` renames the whole subtree, not just its root.** The rename to `__deleted__` is what marks a record dead, and every flat scan over `scene.components` — in this editor and in `saveComposeToDisk` — filters on exactly that name. A descendant left with its own name is still a live component to all of them, pointing at a chunk that has just been killed and at a parent that has disowned it. Nothing used to delete a parent that had children, which is why this never bit; an asset deletes one every time it is baked.

The related engine fix is in `setComponentParent`, which rebaked the moved subtree from the *ancestors'* accumulated transform and never multiplied in the moved component's own local one — so a reparent silently snapped the component to its new parent's origin. Every descendant was already correct; it was only the root of the moved subtree that lost its placement.

### Selection scope

Five things spread a selection outward from the voxel you clicked — the Extrude face gather, both fills, and the Region tool's Face and Volume selectors — and they all answer the same question, so they answer it the same way (`SelectionScope`):

| Scope | Spreads until |
|-------|---------------|
| **Material** | The material changes. This is the tool picking the brick out of a brick-and-mortar wall |
| **Whole face** / **Whole volume** | The geometry runs out. Ignores materials entirely |

It has to be a choice rather than a default, because on a surface of one material the two are identical and on one built from several neither answer is more obviously right. It also depends enormously on how the scene was made, which is not something the tool can infer — and the difference is not subtle:

| Scene | `Material` face | `Whole face` |
|-------|-----------------|--------------|
| StonehillCastle (flat-coloured) | 3,213 voxels | 4,101 |
| Sibenik (photo-textured) | 1 voxel | 35,431 |
| LostEmpire (photo-textured) | 4 voxels | 31,963 |

A voxelisation made from photographic texture gives nearly every voxel its own palette entry, so `Material` selects a handful of voxels and `Whole face` is the only useful setting; on flat-coloured geometry `Material` is the precise one. Neither is the right default for both, which is exactly why it is a toggle.

Extruded voxels carry **their own source voxel's material** up each column, not one colour for the whole face. Under `Material` that is a distinction without a difference, but under `Whole face` it is what stops a patterned surface from flattening to whichever entry happened to be clicked.

The caps (radius 32, cube side 64, fill 4,000,000 voxels) are about a dab staying inside a frame. Every candidate coordinate costs a tree64 descent to find out whether a voxel is there at all, so the *scanned box* — not the painted count — sets the cost: radius 32 is a 65³ box, a quarter of a million descents. Fill needs its own limit for a different reason: it has no bound of its own, and one started on a terrain's ground material would walk the entire component. Its budget counts voxels **of the region**, not coordinates probed — those differ by about a factor of seven, and conflating them is a bug this code has already had (below).

#### Why the volume fill is breadth-first over a bitset

Three details of the traversal are load-bearing, and each was wrong once:

* **The budget counts region voxels, not probes.** The visited set holds every *rejected* neighbour too, roughly six or seven per voxel of the region. Measuring the budget against it stopped a nominal one-million fill at about 160,000 voxels of actual region.
* **Breadth-first, not depth-first.** A stack-based fill runs to the end of one axis before taking its first step along another. Stop one early and the result is not a compact partial region — it is long tendrils with unpainted material between them.
* **Visited is a bitset per chunk, not a hash of packed coordinates.** A chunk's voxels are a dense cube of known size, so a bit each is the natural set: 256³ is 2 MB, allocated only for chunks the fill enters, and a test is an index rather than a hash. The hashing version cost ~40 bytes and a hash per voxel, which is what forced the budget low enough for the first two problems to bite.

Together the first two produced a fill that stopped at a fraction of its region and painted a *striped* subset of it — and the stripes were the traversal order, which is invisible from the outside because a fill's shape is supposed to be arbitrary. None of it reproduces on a small region, which is how it survived the first round of testing.

#### How it reaches the geometry

Paint writes through the engine's existing edit pipeline: `queueVoxelAdd` → `updateScene` → the GPU flush the render loop already performs. The queue works in packed colours rather than slot numbers and `internMaterial` resolves a colour back to a slot on the way in, so painting with the palette's current entry writes that entry's colour and lands back on that entry — the editor never has to reason about slot numbering.

The whole stroke is queued in **one** call. `updateScene` forks and rebuilds every chunk a queue touches, so a thousand voxels queued together cost one rebuild of each chunk they fall in, where a thousand separate calls would cost a thousand.

#### The two coordinate mappings

A pick reports the voxel's coordinate *inside the chunk it hit*; the queue takes coordinates in the component's continuous voxel space; and a brush has to ask about coordinates nobody picked. So both directions are needed:

* `pickToComponentVoxelCoord` — chunk-local → component space, the direction a pick arrives in.
* `componentVoxelToChunk` — back again, the direction a brush asks in.

For a loose Chunk these are the identity. For a Grid they are not: the chunk is one cell, and `applyComponentQueue` buckets an op out to a cell with `floorDiv(position, resolution)` against `grid.originCellCoord`. The two mappings must be exact inverses, and **nothing else in the editor would notice if they were not** — a brush whose reverse map is off paints a plausible-looking region in the wrong cell, which reads as "the tool is weird near chunk edges" rather than as a bug.

### The self-test

`EDITOR_SELFTEST=1` runs several read-only checks after the scene loads. All of them exist because the failures they catch are invisible from the outside:

```bash
EDITOR_SELFTEST=1 ./scene_editor ../10-scene-previewer/scenes/StonehillCastle
# SELFTEST paint coords: 50653 probes, 5494 solid, 0 mismatches, 0 unmappable
# SELFTEST sculpt lattice: 1728 probes, 0 lattice mismatches, 0 round-trip failures
# SELFTEST fill: comp=0 slot=0 gathered=1556106 truncated=0 leaks=0
# SELFTEST fill face: comp=0 normal=(-1,0,0) gathered=3213 off-plane=0 outside-volume=0 -> PASS
# GIZMOTEST: cursor asks for 17.248 along the axis; live pivot gives [17.248, -0.000, 17.248, -0.000],
#            frozen gives [17.248, 17.248, 17.248, 17.248]
# GIZMOTEST: live pivot oscillates PASS | frozen pivot holds PASS
# SYMMETRYTEST: mirror X + mirror Z + diagonal ZX -> 8 element(s), 8 distinct image(s), free axis held yes
# SYMMETRYTEST: eightfold PASS | closed PASS | order 4 PASS | centred box holds PASS
# SYMMETRYTEST: parity - even origin fixes 1 column(s), odd fixes 0 and pairs 7 with 8 yes -> PASS
# SYMMETRYTEST: mismatched-parity diagonal dropped -> 4 element(s), expected 4 | PASS
```

#### Why symmetry is tested twice

`SYMMETRYTEST` is pure arithmetic over the [mirror group](#symmetry) and proves the group: eight elements *and* eight distinct images (eight maps that collapse onto four positions would still count to eight), closure under composition, every element of order four, a frame centred on a mixed-parity box mapping that box onto itself corner for corner, the parity rule fixing exactly one column or none, and a diagonal across mismatched parity being dropped rather than rounded into place.

`SYMSTROKETEST` drives a real dab and proves the *hook*, which fails differently: the group can be flawless while the copies are written outside the stroke journal, and then everything looks correct until the stroke is undone and seven of the eight copies stay behind. It runs under `EDITOR_SCULPTTEST` because it edits the scene.

```bash
EDITOR_SCULPTTEST=1 ./scene_editor ../10-scene-previewer/scenes/StonehillCastle
# SYMSTROKETEST comp=0 group=8 dab=33 centre=(-8060,-1152,-8056)
# SYMSTROKETEST   one dab writes every copy: 264 cell(s), expected 264 -> PASS
# SYMSTROKETEST   the written set is closed under the group -> PASS
# SYMSTROKETEST   every copy reached the journal: 264 of 264 -> PASS
# SYMSTROKETEST   voxels 23821 -> 24085 (expected 24085), undo back to 23821 -> PASS
```

The dab is placed where all eight images are clear of existing geometry, which is what makes the counts exact — Add skips a cell that is already solid, so a dab landing on something would give a perfectly correct run a number nobody can predict. The closure check is the one a hook that mirrored only the dab's *centre* would fail, having passed the count check on a symmetric brush. The last line is the one that matters: it fails loudly if anyone ever moves the mirroring downstream of the journal.

#### Why the gizmo drag measures against a frozen pivot

`GIZMOTEST` guards the one property a translate drag has to have: **with the cursor held still, every frame must agree on where the component belongs.**

It did not, and had not since the gizmo was written. `closestPointOnAxis` reports `t` relative to the axis line's *origin*, and the gizmo passed it the live pivot — which moves with the component the drag is moving. Shifting that origin by `d` along the axis changes `t` by exactly `−d`, so applying a delta of `d` made the next frame compute a delta of zero, which moved the component back, which restored the original delta.

The result is the `[17.248, -0.000, 17.248, -0.000]` above: a perfect two-frame oscillation between the target position and the start position, for as long as the button is held. On screen that is the object jittering in place — and its colours flickering, because two positions were feeding the same temporal accumulation buffer. The fix is to capture the pivot when the drag begins and measure the whole drag against that (`EditorState::gizmoDragStartAnchor`); the gizmo itself still follows the object, only the measurement is frozen. The rotate handles use the same frozen anchor: a rotation about the pivot is meant to leave the pivot exactly where it is, but deriving the plane from the component being rotated made that an assumption rather than a guarantee.

The test asserts both halves — that the live-pivot loop oscillates and the frozen one does not — because "the fixed version is stable" is only meaningful next to evidence that the broken version was not.

#### `ASSEMBLYTEST`

Its own switch, because it edits the scene — and it builds its own components rather than touching the loaded one, so it is worth running on any scene at all, including one with nothing to sculpt.

```bash
ASSEMBLYTEST=1 ./scene_editor ../10-scene-previewer/scenes/StonehillCastle
# ASSEMBLYTEST: one 8^3 box unioned -> 512 cell(s), expected 512 | PASS
# ASSEMBLYTEST: 8^3 minus 8^3 at +4 -> 256 cell(s), expected 256 | PASS
# ASSEMBLYTEST: 8^3 intersect 8^3 at +4 -> 256 cell(s), expected 256 | PASS
# ASSEMBLYTEST: Intersect on row 0 seeds -> 512 cell(s), expected 512 | PASS
# ASSEMBLYTEST: stack order A-B=256 then B,A=768, expected 256 then 768 | PASS
# ASSEMBLYTEST: four 90-degree turns compose to the identity | PASS
# ASSEMBLYTEST: merge keeps the asset - survived yes , holds 1 child, merged is a placed child yes
#               -> 256 voxel(s) (expected 256), undo restores 2/2 row(s) | PASS
# ASSEMBLYTEST: merged asset moves into another - parented yes , arrives placed yes , temple holds 1 ,
#               world drift 0.0000 , undo puts it back yes | PASS
# ASSEMBLYTEST: hollow sphere at 37 degrees -> 2994 shell cell(s), flood never reached the interior | PASS
# ASSEMBLYTEST: compose round trip wrote [union subtract intersect ] read [union subtract intersect ] | PASS
# ASSEMBLYTEST: reload adopted 1 resolve/2 part(s), folds to 256 (was 256) | PASS
# ASSEMBLYTEST: resize across a resolution boundary keeps selection yes, stack 3->3, row 1->1 | PASS
# ASSEMBLYTEST: recolour reaches the fold 0x3FFFFFFF -> 0x0FFC00, cells held yes | PASS
# ASSEMBLYTEST: edits reach the fold - 512 cells, add 513 , carve 512 , painted cell(s) 1
#               | stale add yes carve yes paint yes | PASS
# ASSEMBLYTEST: mirrored extrude - 2 face(s), 128 voxel(s), opposed normals yes
#               | 512 -> 640 , undo 512 | PASS
# ASSEMBLYTEST: copy modes - link writes a reference yes | copy owns its folder yes (1 entry)
#               | bake writes one data yes (1 entry) | PASS
# ASSEMBLYTEST: place with nothing open -> 1 new asset(s), 2 item(s), second click stayed put yes | PASS
# ASSEMBLYTEST: resolve follows the ops - adopted yes | retired yes | kept while open yes | PASS
# ASSEMBLYTEST: rebuild reads the shape, not the panel - 16^3 box held 16 and stayed a Box with a
#               stale panel; its own 24 applied as 24 | PASS
# ASSEMBLYTEST: bake centres in its chunk - 8^3 occupies [4,4,4]..[11,11,11] (expected 4..11),
#               world drift 0.0000 | PASS
# ASSEMBLYTEST: duplicate lands beside the original - raw copy drift 0.000, editor step 4.000
#               (expected 4), drawn yes | op kept yes | recipe kept yes | PASS
```

The last two are the ones that guard the redesign rather than the fold. **Place with nothing open** asserts that a second click lands in the *same* asset — if `openAsset` were not being set by the first placement, every click would build a fresh container and the shapes would never combine, which looks exactly like "the boolean ops do not work". **Resolve follows the ops** asserts `syncResolves` in both directions, because a sync that only ever adds is the failure that leaks a fold per folder the user has ever touched. **Rebuild reads the shape** is the one that guards the template/recipe split: it leaves the panel holding one shape's numbers and rebuilds a different shape, which is exactly how the bug it replaced was hit.

A fold that is off by one cell produces a perfectly plausible result in the wrong place — the same failure class the paint-coordinate test guards. It reads as "the tool is a bit weird" rather than as a bug, and no amount of looking at the screen settles it, so the properties are asserted numerically instead.

Two of them are worth calling out.

**The hollow sphere at 37°** is the pull-rasterisation test, and it is the one that fails loudly if anyone ever reimplements the fold as a forward map. It floods the empty space *around* the resolved shell and asserts the flood never reaches the middle; one perforation in a two-voxel wall and it does.

**The copy-mode check** is asserted against the *files*, not the scene, because that is the only place the three modes differ. It also has a cautionary history: the first version compared the `source` strings, which pass identically for Copy and Link (the imported node is named after the folder it came from), and the second joined the link's `../../source` back onto the output folder — where it resolves to the real source directory, which exists either way. Both passed against a build with the feature switched off. The check that works asks whether saving *descended*: a link leaves no subfolder behind, a copy leaves one.

**The resize check** guards the selection hand-over described [above](#two-things-worth-knowing). Without it the line reads `keeps selection NO, row 1->-1` — the part is still in the stack and still the right size, and only the fact that you cannot adjust it twice in a row says otherwise.

**Two of them are the two halves of one round trip.** `compose round trip` proves the ops reach the file; `reload adopted` proves they are read back into something you can carry on working on. It was the second that caught `setComponentParent` dropping the moved component's own local transform — every part came back at the right size, in the right stack, with the right op, and stacked on top of each other at the origin, so the subtract cancelled the union exactly and the form resolved to nothing.

**`runPaintCoordSelfTest`** round-trips both coordinate mappings against the tree64. A Grid's resolution is *not* reliably available from `dataReferences[dataRefID]`: `dataRefID` is `-1` until a component's first edit assigns one (`ensureDataReference`), so on a freshly loaded scene — which is every scene, before anything is painted — the reverse map reported an empty world and every brush found nothing. The resolution is read off the first populated cell instead, the same fallback `ensureDataReference` uses.

**`runSculptLatticeSelfTest`** checks the world lattice against the per-chunk mapping `utils::pickVoxel` inverts to report `worldPosition`, and round-trips world↔voxel. A wrong `coordOrigin` on a downward-expanded Grid fails **here and nowhere else**: every chunk stays individually correct, and the whole component is simply offset by a cell or two from where the tool aims.

**`runFillSelfTest`** checks that a volume fill is **adjacency-closed**: no solid voxel of the seed's material may touch the gathered set without being in it. It seeds on the component's most common material, because the small regions were always correct — that is precisely why the striping survived the first round of testing. It reported 1,147,019 leaks on StonehillCastle before the traversal was fixed, and 0 after. It then gathers a **face** fill from the same seed and checks the two properties that justify the mode existing at all: it never reaches a voxel the volume fill did not, and it never leaves its plane. A face fill that escaped its plane would be a volume fill wearing the safer name — exactly the surprise the split exists to prevent.

Run all three after touching either mapping, the fill traversal, or anything about how a Grid derives its resolution or moves its origin.

#### The sculpt stroke test

`EDITOR_SCULPTTEST=1` is a **separate switch, because this one edits the scene** — in memory, never saved, and undone again through the history before it returns (which also checks that undoing a stroke restores the voxel count exactly). The read-only checks above are cheap to leave on; an editor that edits the scene on startup is not.

It runs a 20-frame stroke with the ray **held still**, which is the sharpest form of the problem: every frame casts the same ray into a scene that now contains the previous frame's dab. Four cases — add and remove, each with the override kept and removed. The control clears `sculptStrokeTouched` between frames, which is precisely the tool with the fix taken out; without it, "the voxels stayed near the surface" would also be the result if the stroke had quietly placed nothing at all.

```bash
EDITOR_SCULPTTEST=1 ./scene_editor ../10-scene-previewer/scenes/StonehillCastle
# SCULPTTEST comp=0 surface=398.43 voxelSize=1.000 frames=20 tolerance=5.00
# SCULPTTEST   add,    override: 99 voxels, span [396.0, 401.5], ray now 398.43 -> PASS
# SCULPTTEST   add,    control : 2124 voxels, span [333.9, 401.5], ray now 333.70 -> PASS
# SCULPTTEST   remove, override: 38 voxels, span [396.3, 401.1], ray now 398.43 -> PASS
# SCULPTTEST   remove, control : 38 voxels, span [396.3, 401.1], ray now nothing -> PASS
# SCULPTTEST   controls must drift and overrides must hold; undo back to 1727740 -> PASS
```

The control climbs 64.7 units toward the camera when adding, and when removing it bores clean out of the scene — `ray now nothing`. On Sibenik, which has interior geometry to eat, the removal control tunnels 31 units deeper instead.

`runExtrudeSelfTest` runs alongside it, checking three things that all fail silently:

```bash
# EXTRUDETEST comp=0 face=3213 voxels normal=(-1,0,0)
# EXTRUDETEST   face is one plane/material/surface: 0 off-plane, 0 wrong material, 0 buried -> PASS
# EXTRUDETEST   depth 1 adds one per face voxel: 1727740 -> 1730953 (expected 1730953) -> PASS
# EXTRUDETEST   reversible mid-drag: out to 5 (1743805) and back (1727740), in to -3 (1718101) and back (1727740) -> PASS
# EXTRUDETEST   undo of a depth-3 drag: 1737379 -> 1727740 (baseline 1727740) -> PASS
# EXTRUDETEST   extruded over an existing voxel, then retracted: survived=true colour kept=true -> PASS
```

1. **The face is a face.** A gather that leaked through the material test or out of the plane still produces a plausible-looking extrusion — of the wrong region.
2. **A drag is reversible in place**, before any undo is involved.
3. **Depth one is exact.** Every voxel of a face has, by definition, an empty cell in front of it, so one layer out adds precisely one voxel per face voxel. Any other number means the face and the layer disagree about which cells they cover.

The last line **builds its case on purpose**: no natural face in any shipped scene extrudes into occupied space (the counts show every layer landing in empty air), so the test parks a voxel of another colour two layers out and checks it survives the round trip with its own material intact. Without it the displacement path would be untested — which matters, because that defect was found by reading the retract path, not by watching it fail.

**It asserts on where the ray still thinks the surface is, not on how far the placed voxels reach**, and the difference matters: the two failures do not look alike from the geometry's side. Climbing piles voxels toward the camera and shows up in their extent; tunnelling only does if there is something deeper to eat, and a voxelised surface is usually a *shell*. The first version of this test measured extent and passed the removal control after it had bored straight through the wall. Both failures share exactly one signature — the ray stopped agreeing with where the stroke started — and that holds whatever the object is made of.

`runSculptOperatorSelfTest` runs under it too, and unlike the others it does **not** trust whatever scene happens to be open: it builds a two-layer slab in empty space with a one-voxel spike on it, a one-voxel dent in it, and a voxel floating clear of both. On a real scene "did it get smoother" has no answer; on a shape built for the purpose every assertion is exact. One layer would not do — a lone layer is not stable under a majority filter (its cells see only 9 of 27) and the test would be measuring that instead.

```bash
# OPERATORTEST comp=0 slab=13x13 at (-254,-254,-254) radius=5.0 (built slab=true spike=true dent=true noise=true)
# OPERATORTEST   smooth at full strength removes a spike, fills a dent and clears noise: ... -> PASS
# OPERATORTEST   smooth at strength 0 (cutoff 6) takes only the noise, however long it is held: ... -> PASS
# OPERATORTEST   smooth at strength 2 (5x5x5) takes a two-layer slab that strength 1 cannot: true -> PASS
# OPERATORTEST   smooth leaves a flat surface alone: true -> PASS
# OPERATORTEST   bump pulls one layer and pushes it back: flat=true pulled=true pushed=true -> PASS
# OPERATORTEST   blend softens the rim: tallest step 3 at blend 0, 2 at the default 1, 1 at 4 -> PASS
# OPERATORTEST   smooth tick at radius 10: 0.35 ms at strength 1, 1.00 ms at 3 (budget 100 ms) -> PASS
```

The three that matter most are the ones a bug would leave looking plausible. **Stable on a flat surface** is the difference between a smooth brush and a delete brush — a majority filter that drifted would eat a wall just for being pointed at. **However long it is held** and **that strength 1 cannot** are the two halves of what makes [the strength setting](#smooths-strength) a strength rather than a delay — the first that a weak pass is selective, the second that a wide one reaches a shape no amount of holding does. And the **rim step** is measured, not eyeballed: the tallest single-cell step anywhere around the bump's edge, which is the thing that reads as a stamped sphere.

`runPaintStrokeSelfTest` runs under the same switch, for the same reason — it paints and then undoes itself. It presses at one point on a surface, samples once more a couple of dozen voxels along it, and releases, then asserts the four things a stroke has to get right:

```bash
# SELFTEST paint stroke: comp=0 sweep=23 voxels painted=24 (two dabs alone=2) entries=1 duplicates=0 wrong-after-paint=0 wrong-after-undo=0
# SELFTEST paint stroke: one entry per stroke PASS | no repeats PASS | colour applied PASS | undo restored PASS | gap filled PASS
```

Only the first two of those can be seen from a single frame. **One entry per stroke** is what makes a sweep one press of `Ctrl+Z` rather than a dozen. **No repeats** is the within-frame overlap being dropped. And **gap filled** is the point of the whole stroke, asserted against the strongest control available: the same two dab centres collected on the untouched scene with nothing between them. Two dabs paint two voxels; the stroke paints the twenty-four along the path. Without interpolation those numbers are equal — which is exactly the dotted line clicking repeatedly already gave.

#### Undo

Undo repaints each voxel with the colour it had — a per-voxel list, since a sphere or a cube can span several materials. Only voxels whose colour would actually change are collected, so a brush dragged over a wall it has already painted queues nothing and the undo step is the set of voxels that really moved. A whole stroke is **one** entry, so undoing puts the scene back to where the button went down.

The coordinate list is shared between the undo and redo closures rather than copied into each; a fill can run to a million voxels, and three copies of that list would be 36 MB the history is charged for and has to carry. `record.memoryCost` reports the real figure, so a big fill counts properly against the log's 512 MB cap.

## The Library

The left column's second half: a persistent browser over the folders scenes and assets live in. Folders holding a `compose.json` are tinted and marked `[scene]`; selecting one lists its top-level components, read with `parseComposeJson` — **no geometry is read and nothing reaches the GPU**, so browsing a 3 GB scene costs a file read.

It is deliberately separate from the Assets panel above it rather than being extra rows in the same list. Assets answers "what is in the thing I have open" and its rows are renamed, reordered and deleted; the library answers "what could be" and its rows are browsed and brought in. One list with two sets of operations that mean different things on rows that look alike is worse than two panels.

**Bring into open asset** grafts the selected folder into the [open asset](#assets-the-place-and-region-tools), in front of the camera, via `utils::instantiateComposeInto` — the folder is loaded into a temporary `Scene` and then appended, with every handle it carries remapped: component handles, chunk handles, grid indices, geometry pool indices, and the loose list. It arrives as an ordinary component of whatever is open, which is what makes an asset on disk the third source an item can come from.

#### The three modes

The mode sits beside the button rather than behind a confirmation dialog, because it is not a confirmation — it is part of the verb. "Copy this in" and "link this in" are different things to have asked for, and which one you meant is worth stating before rather than regretting after.

| Mode | On disk | In memory | Gives up |
|------|---------|-----------|----------|
| **Copy** | Its own subfolder under the target, with its own `compose.json` and `.data` files | One pooled blob shared with the source until either is edited, then forked | Propagation from the source |
| **Bake** | One flattened `.data` entry | One blob, one chunk, one GPU header row | The internal structure — it stops being a stack |
| **Link** | A `type: asset` entry pointing at the source folder | Same as Copy | The ability to diverge |

**All three are identical in memory** — the geometry has to be loaded either way for the renderer to see it — so nothing on screen distinguishes them. They differ at exactly one moment: what a later save writes. That is the worst possible place for a bug to hide, which is why the self-test asserts against the files rather than against the scene.

**Copy** is the default. "Unlinked" is an *authoring* property, not a storage cost: the geometry pool is keyed on (canonical path, block coords, mutability), so two copies of a tree that nobody has edited are one blob.

**Bake** is `foldNode`'s pull run at import time — `pullPartIntoLattice` already walks an Asset's leaves and unions them. A forty-component asset placed fifty times is two thousand header rows before baking and fifty after. Offered at copy time because that is when the choice is real; afterwards you would be flattening something you may already have started editing. A bake past `ASSEMBLY_MAX_BAKE_CELLS` is refused, and the import stays as a Copy rather than being lost to the refusal.

**Link** is what the format has always supported and the editor never exposed. It needs one runtime field — `ComponentRecord::externalSource` — because a link that is not marked survives until the first save and then quietly becomes a copy: the user asked for propagation, still believes they have it, and no longer do. `saveComposeToDisk` writes a marked node as a bare reference and does not descend into it. The path is written relative whenever one can be formed, *including* one that climbs out to a sibling (`../assets/buttress`), since that is the ordinary shape of a project tree and keeping it relative is what lets the whole tree be moved.

Geometry is copied rather than shared with anything already loaded: the two `Scene`s have separate geometry pools and no common blob identity, so deduplication against components already in the scene is not attempted. Instancing *within* the imported folder is preserved.

This is still a *browser* rather than co-resident scenes, and deliberately: [loading is two-phase](#loading-is-two-phase) precisely because holding two scenes' textures at once killed a Vulkan driver. An import reads one folder and lets the temporary `Scene` go; opening a second scene to borrow from would keep both resident on purpose.

The Inspector's local Position, Rotation, and Scale are editable — drag to change, undoable, live in the viewport as you drag. Rotation is shown and edited as Euler degrees (a quaternion has no natural "drag this number" widget); World transform stays read-only, shown for reference. "Reset transform" clears all three to identity.

Editing Scale required a real engine fix, not just UI: `ComponentRecord::localScale` existed but `rebakeSubtree` (`scene_query.cpp`) never applied it to a Chunk's `header.scale` or a Grid's `cellSize` — and separately, extracting rotation from a matrix that still had scale baked into its columns would have silently produced a wrong quaternion the moment scale stopped being 1.0. Fixed by decomposing (position, rotation, uniform scale) properly — extract scale via column length, normalize before `quat_cast` — the same technique `loadComposeFromDisk` already used when a compose.json leaf's transform is first decomposed. `Chunk::nativeScale` / `SceneGrid::nativeCellSize` (new, runtime-only — `ChunkHeader` itself is disk-persisted and untouched) hold the transform-independent size so repeated edits don't compound. Verified against the `editing_p1` test driver: identical pre-existing failures before/after (none touch transforms), plus three new checks (2x scale, 0.5x, restore) all pass.

## Selecting a component

There are three ways in — clicking a voxel in the viewport (Select or Move tool), clicking a row in the Assets panel, and clicking an element of the breadcrumb — and all three land on the same selection, which every other panel follows.

Whichever route it came by, the selection outlines every `.data` box it covers in **yellow** in the Viewport — the node itself if it's a Chunk or Grid, every occupied cell if it's a Grid, or every leaf beneath it if it's an Asset folder (`main.cpp`'s `collectLeafChunks`). The Inspector's "Boxes" count is the same set.

The outline is drawn in screen space: each chunk's OBB corners (`header.position + header.rotation * localOffset`, the same convention `fetchVoxelColor` and `utils::pickVoxel` use) are projected with `worldToViewportPixel`, the exact inverse of the shader's `rayStartDirection` — so the box lines up with the geometry the raymarcher actually drew, at any camera angle. A corner behind the camera drops its edges rather than clipping them; adequate for a selection hint, wrong for anything claiming precision.

## Palette editing

The Palette panel shows one component's materials as a grid of colour swatches with the number of voxels using each underneath — a palette is a set of colours, so the colours are what the grid shows, and names live in the detail pane below. Selecting a swatch opens it:

* **Colour** — a picker that recolours live. Dragging it repaints every voxel using that slot as you drag, because a recolour cannot move a palette offset: it takes `graphics::updatePaletteEntry`, a single texel write, rather than a full palette rebuild that would also dirty every chunk header.
* **Voxels** — how many voxels use the entry, what share of the component that is, and which chunks they are in. Clicking a chunk moves the camera to it.

**Add entry** appends a white material. **Remove entry** deletes one — and because slot numbers *are* the data (every voxel stores its slot as a byte), removing renumbers every slot above it and rewrites the material byte of every voxel that referenced them. When the entry is in use, a dialog asks which entry its voxels should become. Geometry shared with another component is copy-on-written first, so the other component's colours are untouched.

The counts are true voxel counts, not material-byte counts: a *uniform* leaf stores one byte for up to 64 voxels (~96% of leaves on terrain), so counting the `materialIDs` array would under-report by an order of magnitude. `utils::countMaterialUsage` walks the tree instead.

### The eyedropper

The dropper button in the Palette panel's header (and in the Tool panel's material row) arms a pick; the next click in the viewport selects the palette entry of the voxel under the cursor (Esc cancels). Under **Paint** and **Sculpt**, `Alt`+click does the same thing without arming anything first — the standard modifier in every paint program, and the reason neither tool needs the armed mode. Both place the palette's current entry, so "use that colour" has to be reachable without leaving the tool. The ray is cast on the **CPU** — `utils::pickVoxel` sorts chunks by their bounding boxes and walks the winner voxel by voxel, reading materials straight out of the tree64 with `utils::queryVoxelMaterial`.

That is deliberate. The GPU knows the answer, but reading it back stalls the pipeline and needs the viewport renderer to keep a G-buffer it otherwise has no use for. A CPU pick costs one ray on a click, needs nothing from the renderer, and hands back the exact voxel coordinate and chunk — which is what voxel editing will need next, not just a colour.

`rayDirectionThroughImage` is a line-for-line port of `rayStartDirection` in `pjv_utils_DDA.sc`. If that shader's ray generation changes, this must change with it or picks land beside what was clicked.

## Why the colour lookup uses the march's integer coordinate

Worth writing down, because the symptom was subtle and the cause was not where it looked.

Once a component had been moved or rotated, some of its voxels rendered in a **neighbouring voxel's colour** — scattered among correct ones, stable for a given transform, absent at the origin, and not visible in the stored data at all. Clicking a wrong-looking voxel reported the *right* colour, because CPU picking (`pickVoxel`) hands its DDA's own integer coordinate straight to `queryVoxelMaterial` and never leaves voxel space.

The GPU did leave voxel space. `marchRayThroughTree64` produced an exact `ivec3` and then converted it to a world-space box; `fetchVoxelColor` converted that box *back* — rotate by R⁻¹, subtract the chunk's world position, divide by scale, multiply by resolution, truncate. Algebraically an identity, but not in float32:

* the local offset is added to the world translation **P** and subtracted back, losing low bits in proportion to `|P|` (twice, in fact — the old code added `P` and immediately subtracted it again);
* the result is **truncated**, so an error of a single ULP below an exact integer drops the coordinate a whole cell.

Modelling that arithmetic in float32 over a chunk's voxels gives the rate directly:

| case | wrong voxels |
|------|--------------|
| at the origin, unrotated | 0.00% |
| translated a little | 6.25% |
| rotated, at the origin | 9.81% |
| rotated and translated | 39.40% |

The 0% at the origin is why it read as a gizmo bug: the shipped scenes load at the origin unrotated and are correct, so the artefact appears the moment anything is moved.

The fix is to stop reconstructing what was already known. `SceneIntersectData` now carries `voxelCoord` straight from the DDA, and `fetchVoxelColorAtCoord` indexes with it. `fetchVoxelColor(BoxAABB, …)` is kept — a dozen shaders across the other examples still call it — but is now a thin wrapper over the exact path and marked as the lossy entry point. Those examples still have the bug; porting one is a matter of passing `hit.voxelCoord` instead of `hit.foundBox`.

## Undo

Every edit is recorded in `edit_history.{h,cpp}` as a pair of closures — one that reverses it, one that reapplies it — rather than as a struct in a central switch. The operations coming next (voxel add, erase, transforms) reverse in completely different ways, and each knows how to reverse itself better than a central authority would.

* `Ctrl+Z` / `Ctrl+Shift+Z`, and the Edit menu, which names the step it will undo. (`Ctrl+Y` was a second Redo binding until the Region tool took the letter.)
* The History panel lists every edit; clicking one walks the scene to the state just after it.
* A colour drag is **one** undo step: consecutive edits to the same target within a second coalesce, keeping the colour the drag started from.
* Removing a palette entry is undone from a **snapshot** (the palette, the affected blobs' material bytes, and which blob each chunk pointed at). The operation renumbers slots and rewrites voxels, so there is no inverse to compute — only a record to restore. The log is capped at 512 MB and drops its oldest steps past that.

## Loading a scene

File ▸ Load Scene… opens a directory browser. A folder holding a `compose.json` is a scene and is tinted and marked `[scene]`; anything else is a step on the way to one. Click to select, double-click to commit — opening an ordinary folder, or loading a scene one. The path field at the top takes a typed path on Enter.

Any Compose folder works, not just the bundled ones:

```
../10-scene-previewer/scenes/Sibenik        # a bundled scene
../20-mesh-voxelizer/trees/Oak_Leav         # a 64^3 asset
../30-renderers/SponzaScene/              # the Sponza atrium
```

## How the scene gets into a dock panel

The editor's renderer (`editorRenderer/`) is the previewer's three passes with one change in `render.json`: the display pass writes to `frameBufferOutputID: 3` rather than `-1`. FBO 3 is a new `RGBA8` texture, `viewportColor`, and that texture is what `ImGui::Image` draws in the Viewport panel.

Three consequences shape `main.cpp`'s render loop, which is written out rather than calling `renderConstructedRenderer`:

* **The back buffer and the scene's targets have different sizes.** The back buffer follows the OS window (`bgfx::reset`); the scene's textures follow the Viewport panel. The engine's `resizeFramebuffersAndTheirTexturesIfNeeded` couples the two — it calls `bgfx::reset` with the size it is given — so the editor has its own `resizeViewportTargets`, which is that function's texture half without the reset, and which destroys the framebuffer handles it replaces (the editor resizes on every frame of a splitter drag; leaked handles would exhaust bgfx's framebuffer pool in seconds).
* **The panel's size is applied one frame late, deliberately.** The interface hands ImGui the viewport texture's handle, so the handle must not change after the interface is built. Resizing happens at the top of the frame, before anything reads it, which means the texture ImGui is given is the one that frame's scene passes render into.
* **View order does the compositing.** The scene passes take bgfx views 0–2, ImGui takes view 200, and bgfx submits views in ascending ID order — so the scene is finished before the interface samples it, regardless of the order the C++ submits them in.

Because the scene is rendered at exactly the panel's resolution and the `windowRes` uniform is set to the same, there is no letterboxing or aspect distortion in the panel, and the accumulation pass resets whenever the panel is resized (its history is the wrong size).

## The ImGui backend

Dear ImGui ships a GLFW **platform** backend (used here as-is) but no bgfx **renderer** backend — bgfx keeps its ImGui integration inside its example framework, which drags in bx allocators, the entry-point layer, and its own embedded shaders. `imgui_impl_bgfx.{h,cpp}` is the same job done against the engine's primitives: shaders through `graphics::loadShader`, geometry through bgfx transient buffers, one bgfx view.

It implements ImGui 1.92's `ImTextureData` protocol (`ImGuiBackendFlags_RendererHasTextures`), so ImGui creates, updates, and destroys its own atlases through `ImDrawData::Textures` rather than the backend building a font texture up front.

The two ImGui shaders live in their own directory, `editorRenderer/imguiShaders/`, because of the `varying.def.sc` beside them: `ImDrawVert` fixes ImGui's vertex as a 2D position, one UV, and a packed RGBA byte colour, which cannot share the viewport renderer's varying definition where `a_position` is the vec3 of a fullscreen quad. Note that `varying.def.sc` files carry **no comments** — shaderc's parser for them silently drops the declaration that follows one.

## Files

```
main.cpp                              The editor: state, panels, camera, render loop
imgui_impl_bgfx.{h,cpp}               Dear ImGui renderer backend for bgfx
CMakeLists.txt                        Builds ImGui and Lua, then the editor and its shaders
editorRenderer/render.json            Four passes: albedo, shade, accumulate, display (targets FBO 3)
editorRenderer/resources.json         previewColor/Normal/Position, shadedColor, accumColor,
                                        viewportColor + their FBOs
editorRenderer/editorShaders/         albedo, shade, accumulate, display (+ fullscreen quad VS)
editorRenderer/imguiShaders/          vs_imgui, imgui (+ their own varying.def.sc)
```

Scenes are not bundled: the editor points at `../scene_previewer/scenes/` in the build tree, whose `.data` files are tens of megabytes and are already in the repository once.

## ProjectV Features Used

* `utils::loadComposeFromDisk` — Compose scene folder → `Scene`
* `utils::instantiateComposeInto` — the same walk grafted onto a branch of a scene already open, so an asset on disk can become a part
* `utils::saveComposeToDisk` / `writeComposeJson` / `writeDataFile` — the write path a bake to disk goes down
* `ComponentRecord::op` / `ComposeComponent::op` (`projv::BooleanOp`) — the one field that makes an asset a re-openable stack rather than a folder of parts
* `Scene::components` (name, kind, parent/children, local transform) — the component tree the Assets panel walks one level of
* `utils::getComponentPath`, `getComponentWorldPosition`, `getComponentVoxelCount` — the inspector panel
* `graphics::createTexturesForScene` / `destroyGPUData` — per-load GPU upload and teardown
* `graphics::updatePaletteEntry` — single-texel palette writes for live recolouring
* `utils::addMaterial` / `setMaterialColor` / `setMaterialName` / `removeMaterial` — palette editing
* `utils::countMaterialUsage` / `findMaterialChunks` — voxels per slot, and which chunks they are in
* `utils::pickVoxel` / `queryVoxelMaterial` / `rayDirectionThroughImage` — CPU picking, for click-to-select, the eyedropper, and every frame of a sculpt stroke (via `VoxelSolidityOverride`)
* `utils::queueVoxelAdd` / `queueVoxelRemove` / `updateScene` + `graphics::flushSceneUpdates` — the write path every editing tool uses
* `utils::addComponent` / `isValidChunkResolution` — creating an asset node and the components under it
* `utils::parseComposeJson` — the Library's scene contents, read without loading any geometry
* `graphics::loadRendererSpecification` / `constructRendererSpecification` — the JSON-described viewport renderer
* `graphics::performRenderPasses`, `updateUniforms`, `setUniformToValue` — the render loop, driven by hand
* `graphics::getTextureAttachments` — rebuilding framebuffers on viewport resize
* `core::ecs` — Startup/Update/Render/**Shutdown** stages and global resources

## Third-Party Dependencies

* **Dear ImGui** (docking branch) — `external/imgui`, a submodule of the *engine*, not of this example: it is intended to become part of the engine proper. Nothing in `lib/` links against it yet, so the example compiles the five ImGui translation units it needs directly.
* **bgfx / bx / bimg**, **GLFW** — as every windowed example.

## Why a saved scene used to reload black

A cache guard must not skip when there is nothing cached, and `rebuildGlobalPaletteTexture`'s did.

The global palette texture is rebuilt when a watermark — the sum of every component's `paletteVersion` — differs from the one the GPU was last uploaded with. That answers *"has any palette changed since the last upload"*. It cannot answer *"has there ever been an upload"*, and the two come apart in exactly one case, which happens to be the ordinary one:

* `loadComposeFromDisk` assigns `materialPalette` straight out of the file and never touched `paletteVersion`, so every component of a freshly loaded scene sat at **0** and the whole scene summed to **0**;
* a fresh `GPUData` also stores **0**.

The two matched, the palette was declared already uploaded, and **the texture was never created at all**. Every voxel sampled an invalid texture and the scene rendered black — while the Palette panel was entirely correct, the usage counts were right, and painting a voxel reported it was already that colour, because nothing whatsoever was wrong on the CPU side. Adding any palette entry bumped a version, broke the tie, and repaired the entire scene at once, which is a confusing thing to watch and points at the palette rather than at the upload.

Testing `bgfx::isValid(materialPaletteTexture)` alongside the watermark is what closes it, and it is the honest test: an invalid handle means the GPU cannot be holding what the CPU has, whatever the versions say. `loadComposeFromDisk` now also stamps `paletteVersion = 1`, matching what `addComponent` puts on the default palette it creates — 0 means "never set", which was the wrong thing for a component that has a palette to report regardless of what depended on it.

The diagnostic line above the guard was printing a *different* expression from the one the code branched on, so it cheerfully logged `rebuilt=true` on every frame it skipped the rebuild. It prints the real condition now, plus whether a texture exists at all.

## The Brush Lab

A third top-level tab, beside **Edit** and **Render** (`F10`, or the strip in any tab's menu bar). It is where **programmable brushes** are written and tried: a brush is a Lua file in `brushes/` next to the executable, and the Lab is a library list, the declaration read back, the settings it asks for, its source, its output, and a preview that runs it on the open scene and throws the result away.

A brush is a tab rather than a panel because it is not part of any document. It is a file in the user's own folder, shared by every scene they open, and designing one is a four-surface loop that has nothing to say while a scene is being arranged. It costs nothing on the renderer: it borrows Edit's, so switching allocates no targets and the preview is the same stored-albedo view the Viewport shows — the only honest view of what a brush actually wrote.

The design, and the reasoning behind every decision below, is in [`docs/plans/brush_system.md`](../../docs/plans/brush_system.md).

### What a brush is

One file that declares what it is, what settings it wants, what materials it can write — and then answers **one question per voxel**:

```lua
return {
    name = "Cracked Rock",
    kind = "material",                              -- material | geometry | scatter
    needs = { "position", "skinDepth", "material" }, -- only these are computed
    maxSkinDepth = 6,

    params = {
        { name = "frequency", label = "Crack frequency", type = "float",
          default = 0.09, min = 0.01, max = 0.5 },
        { name = "onlyMaterial", label = "Only this material", type = "text", default = "" },
        { name = "seed", type = "seed", default = 8891 },
    },

    -- Twelve greys as one ramp. The colours are what the entries are given *if they have to be
    -- created*; once they exist they belong to the palette, and any role can be pointed at an entry
    -- that is already there.
    materials = {
        { name = "rock", steps = 12, color = { 0.60, 0.58, 0.54 }, colorTo = { 0.10, 0.10, 0.09 } },
    },

    apply = function(ctx, p)
        if p.onlyMaterial ~= "" and not string.find(ctx.material or "", p.onlyMaterial, 1, true) then
            return nil                              -- leave this voxel alone
        end
        local f1, f2 = pv.worley(ctx.x * p.frequency, ctx.y * p.frequency, ctx.z * p.frequency, p.seed)
        local crack = 1.0 - pv.smoothstep(0.0, 0.18, f2 - f1)
        crack = crack * pv.clamp(1.0 - ctx.depth / 7, 0.0, 1.0)   -- a crack is a surface feature
        return 1 + math.floor(pv.clamp(crack, 0.0, 0.999) * 12)   -- an index into `materials`
    end,
}
```

Seven brushes ship in `brushes/`: `cracked_rock`, `crevice_shade`, `grass_tint` and `palette_paint` (material), `pitted_stone` — *Cracked Stone*, which opens cracks rather than painting them (geometry) — and `grass_tufts` and `trees` (scatter). **New Brush** writes a working template of the chosen kind, because the first thing anyone does with a new brush is preview it.

### Three kinds, decided by what they write

| Kind | Asked about | Returns |
|------|-------------|---------|
| **Material** | solid voxels only | a material index, or `nil`. Cannot add or remove geometry |
| **Geometry** | every cell of the dab, empty ones included | an index (fill), `false` (empty it), or `nil` |
| **Scatter** | **sites** on the surface, not cells | a list of voxels to grow there, or `nil` |

A rock texture and a grass tint are the *same kind*: both recolour what exists. That is the distinction the machinery cares about, because it is exactly what decides whether an empty cell is a question the brush gets asked. A grass brush is two brushes composed — `grass_tint` paints the ground, `grass_tufts` plants what stands up out of it.

### Scatter: sites, not cells

A scatter brush plants objects. It is not asked about cells at all — it is asked about **sites**, and it answers with a shape:

```lua
apply = function(ctx, p)
    if ctx.ny < 1.0 - p.maxSlope then return nil end          -- too steep; nothing grows
    local height = 1 + math.floor(pv.hash(ctx.x, ctx.y, ctx.z, p.seed) * p.height)
    if not pv.fits(ctx.x, ctx.y + 1, ctx.z, ctx.x, ctx.y + height, ctx.z) then return nil end
    local voxels = {}
    for i = 1, height do voxels[#voxels + 1] = { 0, i, 0, 1 } end   -- dx, dy, dz, material
    return voxels
end
```

Offsets are from the site; `{x=,y=,z=,material=}` works as well as the positional form. `nil` is the ordinary answer — a scatter brush is asked about every eligible site and plants on a fraction of them.

**Where the sites are is the editor's job, and it is the part that decides whether the tool feels right.** Three things are wanted at once: plants that do not clump; a second pass over planted ground that plants nothing new; and the same ground giving the same plants every time. All three fall out of choosing sites from a **lattice fixed to the model** rather than from the dab. The component's voxel space is divided into cells `spacing` across, each cell hashes to one offset inside itself, and a surface voxel is a site exactly when it sits at its own cell's offset. The dab only decides which cells are *visited* — never which voxel within them wins — so a cell answers the same whether it was reached from the middle of a stroke or clipped by its edge.

The lattice is 2D, in the two axes across the surface, with the third being whichever axis that bit of surface most faces. On ground that is one plant per `spacing × spacing` column; on a wall it is the same rule turned on its side.

Two settings are read by the **editor** rather than by the script, by name — so a scatter brush's spacing is tuned in the same panel as everything else, with no second mechanism and no declaration naming which parameter means what:

| | |
|---|---|
| `spacing` | how far apart sites are, in voxels. One placement per cell at most |
| `density` | what fraction of those cells are taken, 0–1 |

`spacing` is the lattice's cell size, which is an *average* spacing rather than a hard minimum: two sites in neighbouring cells can land against each other if one jitters to its high edge and the other to its low one. What cannot happen is two in one cell.

**The fit test is all-or-nothing.** A placement whose every cell is free is planted; one that would grow through anything is refused whole. Planting the half that fits is what makes a scatter of trees look like a scatter of half trees, and "check that it fits" is the whole of the request. The brush can look further than its own voxels with `pv.solid(x, y, z)` and `pv.fits(x0,y0,z0, x1,y1,z1)`, in the component's own coordinates — a tree asking about its headroom before it commits. Both count what the dab has planted so far, so two plants cannot grow through each other.

One rule the host enforces that the lattice cannot: **nothing is ever planted on what this stroke has already planted.** A blade of grass is itself a solid voxel with air above it, so without that, a second dab finds every blade tip and grows another blade out of it — a tower per site, taller every pass. The stroke journal is the record of what the stroke made, which makes it exactly the right question to ask.

#### `trees`, and why a tree is the real test of all this

A blade of grass is three voxels and fits almost anywhere; the fit machinery barely matters to it. A tree is a trunk and a canopy that may be a thousand voxels across, and the interesting question stops being what it looks like and becomes *is there room for it here*. `brushes/trees.lua` asks three times, cheapest first, because most sites are rejected and the cheap tests are what keep a dab affordable:

1. **the ground** — slope, enclosure, and whether the base is standing on anything (`footing` counts how many of the nine cells under it are solid, so a tree will not root on a one-voxel spur off a cliff);
2. **a thin column straight up**, which catches a low ceiling for the cost of a dozen cells — derived from the tree's real height (trunk + twice the canopy radius), because a headroom check that stops short of the crown passes sites the editor then refuses anyway, having paid to generate a whole tree first;
3. **the whole canopy box plus `clearance`**, and only when clearance is asked for — with clearance 0 it asks exactly the question the editor already answers exactly, and asking twice is the most expensive thing in the brush.

`clearance` is the one check the editor cannot do on the brush's behalf: the editor knows what a placement *occupies*, not what it wants left alone around it.

Broadleaf, conifer and bare are checkboxes like the grass brush's flowers, and blossom is a fourth that speckles a broadleaf canopy. Trunks lean on the same quadratic arc the grass blades use, with the elbow filled in so a leaning trunk cannot come apart into floating segments.

### Why a brush returns an index and not a colour

This is the one part that is forced rather than chosen. A component's palette holds 255 entries (material IDs are one byte), the edit queue carries *colours*, and `internMaterial` adds an entry for every colour it has not seen — so a brush returning free-form RGB exhausts the palette within one stroke and then fails loudly having painted nothing.

So a brush declares its output set, the editor resolves it against the palette once per stroke, and the brush picks by index. Three things follow, all wanted: the palette cost is known before it runs (the Brush tab states it, next to how much room the target has left); a brush can set glossiness and emission per voxel, which a colour could not; and entries are matched **by name**, so re-running a brush lands on the same entries instead of adding a second set beside them. A `steps` ramp generates its entries, so a crack that darkens with depth does not have to write out sixteen greys.

### The palette is the single source of truth

**A brush does not own colours. It owns roles, and a role points at a palette entry.**

Materials are created, recoloured, renamed and removed in one place — the Palette panel — and the Brush Lab shows *that* panel at the foot of its right-hand column, not a copy of it. Same function, same state, and the same width and corner of the window it occupies in the Edit tab, so it is not a panel to re-find on switching. The entry selected in the Lab is the entry selected in the Edit tab, and a colour changed in either is changed for good.

Everything a brush can write is listed under **What it writes**, one row per role:

```
6 palette entries  | Box has 7 of 255
▣ rock.1 -> granite_dark  x
▣ rock.2
▢ rock.3
▣ paint (setting) -> lichen  x
```

- **Click a role's swatch, then click a palette entry.** That is the assign gesture, and it is the same gesture for a declared material and for a colour setting.
- A **hollow** swatch means no entry of that name exists yet: the brush would create one in the colour the script suggests. A **filled** one is an entry that exists, drawn in the colour it actually is.
- `->` names the entry a role has been pointed at; `x` puts it back to the entry named after itself.
- Bindings are stored **by name**, in the sidecar. A slot index is a fact about one component's palette and means something else in the next one; a name is what `internMaterial` already matches on, so a binding made on the castle still means the right thing when the brush is taken to the terrain — and it survives the palette being reordered by a removal, which renumbers every slot above it.

**A colour setting is a palette reference, not a colour picker.** `{ name = "paint", type = "color" }` draws a swatch that assigns an entry — there is no RGB to edit in the Settings tab, because editing a colour is something you do to an entry, in the one place entries are edited. A picker there would write a number into the brush that the palette knows nothing about, and the two would then disagree about what "the tint" is. The script gets `p.paint.r/.g/.b` as before, plus **`p.paint.index`** — a material index, which is exactly what `apply` returns. `brushes/palette_paint.lua` is the whole idea in one file.

**The declared colours are creation defaults.** They are used on exactly one occasion: no entry of that name exists and one has to be made. After that the palette's copy wins — its colour, its glossiness, its emission — and the script's numbers stop being consulted. Otherwise running the brush over one more patch would quietly undo a colour that had just been set by hand. Which is also why a brush still declares colours at all: one dropped into a fresh scene has to do something visible without a setup ritual first.

Recolour an entry in the strip below and the next dab paints the new colour, with nothing else to change.

### The context is declared, not queried

`needs` is a list, not a set of callbacks, and that is a cost decision. Skin depth and crevice occupancy are questions about a voxel's *neighbours*, and neighbouring voxels' neighbourhoods overlap almost entirely — asked of the tree64 one cell at a time that is ~28 descents per cell per field, which is what made Smooth unaffordable before `SculptScratch` existed. So the editor reads the dab's neighbourhood into a flat box once and every query becomes an array index.

| Field | What the script sees |
|-------|----------------------|
| `position` | `ctx.x/y/z` — the component's own voxel lattice. The **default** deliberately: it is coherent across the whole `.data` and across separate strokes, where world position re-textures the object every time it is moved |
| `world` | `ctx.wx/wy/wz` — world units, for things that belong to the world rather than the object |
| `material` | `ctx.slot`, `ctx.r/g/b`, `ctx.material` — what is in the cell now. This is how "only the stone" is a setting rather than a second brush |
| `solid` | `ctx.solid`. Free, and always present for a geometry brush |
| `skinDepth` | `ctx.depth` — 0 for a voxel with an exposed face, 1 for one behind it. Costs a margin of `maxSkinDepth + 1` |
| `crevice` | `ctx.crevice` — the solid fraction of a ball. The number that makes creases darker. **Flat open ground already reads about 0.6**, since half the ball around a surface voxel is the ground it stands on: a threshold below that rejects the field you were aiming at, which is a mistake that looks like a broken brush rather than a mis-set number |
| `distance` | `ctx.distance` — 0 at the dab's centre, 1 at its rim. Free |
| `normal` | `ctx.nx/ny/nz` — the gradient of the local solid mask, so it points out of the material and is near zero deep inside |

The declaration buys the margin: a brush that does not ask for skin depth does not pay for it, and one that asks for 6 pays for 6 rather than the worst case. **Skin depth is breadth-first from every exposed face**, giving exact 6-connected distance — a chamfer pass would be cheaper and would answer a different question (a diagonal counted as one step reports a corner voxel as shallower than it is). Outside the box reads as *solid*, which is the safe direction: an empty answer there would invent a surface at the boundary and report every deep voxel near it as skin.

### Settings, in the script or in the editor

Both, and **the script cannot tell the difference** — it reads them out of the same table. The schema comes from the script's `params`; anything added with *Add a setting…* in the Lab goes in a `<brush>.params.json` sidecar beside the file, along with the values of both sets. Which is the order this actually happens in: a setting starts life as a knob you wanted while looking at the panel, and gets written into the script once it has earned its place.

Two files rather than one because they have different authors. The `.lua` belongs to whoever wrote the brush and to version control beside it; the values are the local user's dial settings and change every time a slider moves. Writing them back into the script would mean an editor that rewrites the author's source on every drag.

Eight types — number, whole number, checkbox, **colour** (a palette reference, see above), choice, text, asset path, and **seed**, which is separate from a whole number because it comes with a *Roll* button: "give me a different arrangement" is a different verb from "set this to 8891". Roll steps by a fixed amount rather than randomising, so a sequence of arrangements can be walked back through.

### Hot reload

Editing the file in your own editor works exactly as well as editing it in the Lab: the folder is polled once a frame (one `stat` per brush) and a changed file is reloaded, **keeping the values you have tuned** — matched on name and type. Without that the Lab would be unusable, since a script is saved every few seconds while a number is being tuned and a reload that reset every slider would undo the tuning being done.

### Preview writes to the scene, and takes it all back

Preview is a real edit, journalled and rolled back. Not a copy of the component (which would have to be built and uploaded per dab, and still would not answer what the brush does to *this* model against its actual neighbours) and not a shader trick (which cannot show a geometry brush at all, because the geometry is the output).

What makes it safe is one rule on top of the journal: **preview writes never reach the undo history.** So previewing cannot change the document — the worst case is a revert that has to be asked for — and leaving the tab discards the lot. That is caught where `editor.mode` is written rather than in the Lab's own button, because the tab can also be left by the strip, by `F10`, by `F11` straight to Render, or by the View menu.

The journal holds what each cell was *before* the preview first touched it, which is what makes the revert exact however many dabs overlap: a cell already journalled is skipped before the script is called, so it cannot be changed twice. A cell the brush *declined* is not journalled and is asked again — deliberately, because remembering every cell ever considered would put the whole swept volume into the structure the revert walks, and a carving brush's answer legitimately changes once material has gone.

**The revert takes the palette back too.** A brush resolves its roles into the palette before it paints a single voxel, so a preview creates entries whether or not it ends up changing anything — and a preview that restored every voxel but left six new entries behind would still have edited the document, silently, with no undo entry, in the one structure whose numbering *is* data. Entries the preview created are popped again after the voxels are restored, which is the order that makes it safe: nothing references them by then, so nothing is renumbered. The unwind stops if the last entry is no longer one the preview made, or is still in use — the palette is not private to the preview, and an entry left behind is untidy where removing the wrong one rewrites the material byte of every voxel above it.

The strip above the image reports three numbers, and each is one a brush author asks for: how many voxels changed, how many the script was asked about, and how long the dab took — including reading the declared neighbourhood, not just the script's own answers. If the per-voxel figure is far above what the script's arithmetic could account for, the cost is the snapshot, and the fix is a smaller `maxSkinDepth` than a simpler script.

### Lua, and why per-voxel scripting is affordable

Lua 5.4.7, pinned as a submodule in this example's `external/` and compiled straight into the editor like ImGui. Built with `LUA_USE_POSIX` rather than `LUA_USE_LINUX` on purpose: the Linux preset adds `LUA_USE_DLOPEN`, which would give a script `package.loadlib` and with it arbitrary native code. The one capability the sandbox cannot take back is the one not compiled in.

`dofile`, `loadfile`, `load`, `require`, `package`, `io`, `debug`, `collectgarbage` and `os` are removed. `os` goes whole rather than being trimmed to `os.time`, because a brush must be a pure function of its context and the clock is exactly what would let one stop being one. This is a guard against accidents and against a brush quietly ceasing to be reproducible — not a security boundary, and a brush is a file the user put in their own folder.

**The noise is native, and that is the whole performance answer.** `pv.worley`, `pv.noise`, `pv.fbm`, `pv.hash`, `pv.clamp`, `pv.lerp` and `pv.smoothstep` are C++; the script composes them, which is the part that is actually the brush's design. Measured on the shipped brushes: **85–385 ns per voxel, 2.6–11.8 M calls a second.** A radius-8 dab is ~2000 voxels.

The runaway guard is **250 ms per call**, checked from a count hook — because a script is saved and reloaded every few seconds while it is being written, so `while true do end` is a normal event rather than an exotic one. It has to be time per *call*: Lua's count hook fires on a counter that runs across the whole state, so an instruction budget kills a stroke of a hundred thousand honest calls exactly as reliably as it kills one infinite loop, and at a random voxel.

`print()` from a script and every error it raises go to the **Output** tab, which carries an error count on its label so a failing brush says so from whichever tab is up. An error is reported once per stroke, not once per voxel: the first one latches, and the rest of the dab is skipped.

### Using a brush: the Custom tool

The Lab is where a brush is *written*. The **Custom** tool (`Ctrl+U`, Edit tab) is where one is *used*, and it edits the document — one stroke, one entry in the History panel, undo and redo like anything else.

Its panel is the brush picker and that brush's own two panels, and nothing else: no New, no Copy, no source editor, no rescan. Those are what the Lab is for, and having them in both places would mean two ways to write a brush and two places for that to go wrong. What is here is what you reach for while modelling — pick a brush, read what it will do, turn its knobs.

The panels are literally the Lab's, drawn by the same functions against the same state. A parameter changed here is changed there and saved to the same sidecar; a material role bound here is bound there. There is one brush, not a working copy per tab.

**Below the surface the tool and the preview are the same machine.** Same dab functions, same palette resolution, same fit test, same journal — the only difference is the two ends: a stroke begins by clearing the journal and ends by turning it into a history entry rather than reverting it. That sharing is the point rather than an economy. A preview whose machinery differed from the tool's would be a rehearsal of something else, and the first time the two disagreed the preview would stop being worth looking at.

Two details the stroke inherits from the tools beside it: it is confined to **one component** for its whole life (the journal is keyed on component-space coordinates, so a drag wandering onto a second object would file its cells under the first one's lattice and undo them into the wrong place), and it keeps **the brush it began with**, since the library is polled for changes every frame and a hot reload landing mid-drag would otherwise give one history entry describing two different brushes.

Undo restores voxels, not the palette: entries the stroke created stay behind, unused. That is the same bargain undoing an additive sculpt strikes with the empty Grid cell it leaves — the alternative is renumbering slots, which is the one palette operation that rewrites geometry. The Palette panel's *Remove entry* is how they go.

### What is not built yet
* **Scatter plants voxels, not instances.** Every placement is baked into the target component's geometry, which is right for grass (half a million components would be madness) and wrong for trees: two hundred trees should share one geometry blob, which `geometryPool`'s refcounting already makes nearly free. Placing an instance rather than voxels is the next thing scatter wants, along with `asset` parameters that name a compose folder to plant.
* **Binding to an unnamed entry is refused.** A binding is a name, and a photo-textured voxelisation gives nearly every entry no name at all. The Lab says so and points at the name field rather than naming the entry for you — a palette edit nobody asked for, and one that would need its own undo entry.

### `BRUSHTEST`

Its own switch, because it edits the scene. It builds a 21³ slab in empty space inside an existing component — inside its current bounds, so a Grid is never made to expand — runs every runnable brush over it, and tears it down.

```bash
BRUSHTEST=1 ./scene_editor ../10-scene-previewer/scenes/Untitled\ 3
# BRUSHTEST comp=1 slab=21^3 at (2,2,2)
# BRUSHTEST   one voxel in is skin depth 1 -> PASS
# BRUSHTEST   the centre is skin depth 10 -> PASS
# BRUSHTEST   an empty cell reports -1 -> PASS
# BRUSHTEST   crevice is higher inside than on a corner (0.186589 -> 1.000000) -> PASS
# BRUSHTEST   the normal on the top face points up (1.000000) -> PASS
# BRUSHTEST   cracked_rock: revert restores every voxel's solidity -> PASS
# BRUSHTEST   cracked_rock: revert restores every voxel's colour -> PASS
# BRUSHTEST   cracked_rock: a repeated dab is identical -> PASS
# BRUSHTEST   cracked_rock: four overlapping dabs still revert exactly -> PASS
# BRUSHTEST cracked_rock (Material): 519 voxels changed per dab
# BRUSHTEST   pitted_stone: four overlapping dabs still revert exactly -> PASS
# BRUSHTEST   the test leaves no geometry behind -> PASS
# BRUSHTEST: all checks passed
```

Each check exists because its failure is invisible from the outside:

* **Skin depth against known values.** A wrong depth field still produces a plausible-looking texture — just one whose cracks are in the wrong places, or that run all the way through. The slab's depths are known by construction, which is what makes it a test subject rather than just a surface.
* **The normal on the top face points up.** Getting this wrong flips every "upward faces only" brush onto the undersides of things.
* **A revert restores every voxel's solidity and colour**, compared cell by cell over the slab *and a margin shell around it*. This is the promise the tab makes, and a revert that missed the cells a geometry brush **removed** would leave the document modified with no undo entry — the worst failure this system can have, and one nobody would notice until they saved.
* **Determinism**, and **four overlapping dabs still revert exactly**. The first is what makes a preview a preview of anything; the second is what makes a *drag* safe rather than just a click.
* **A bound role paints the entry's colour, not the script's**, and costs the palette nothing where an unbound one costs an entry (measured as the difference between resolving the same brush both ways, since the brush's *other* roles still have to be created). This is the whole of "the palette is the source of truth", and it fails silently: a binding that were ignored would still paint something plausible, just in the colour the script suggested rather than the one the user chose.
* **A custom stroke commits and comes back.** One history entry per stroke (measured by the cursor, not the entry count — recording after an undo discards the redo tail, so the list can stay the same length while an entry is genuinely added), undo restores every voxel, redo puts back exactly what the stroke did. An undo that does not restore is the worst failure this tool can have: it is silent, and by the time it is noticed the stroke it should have reversed is many edits back.
* **Revert takes back the palette entries it created**, checked entry by entry — name and colour — before and after, and again after four overlapping dabs. This is the check that would have caught the leak that shipped in the first version of the Lab: the voxels came back and the entries stayed.
* **Scatter plants, spaces and does not double-plant.** One site per lattice cell; a second dab in the same place plants nothing new; a dab nudged sideways plants the rim it newly reached and *roots nothing in a cell that already has a plant*. That last one is the property in its precise form — testing "a nudged dab plants nothing" would be testing that the brush does not work, and both of the looser versions of this check passed while the brush was growing towers of grass on top of itself.
* **A low ceiling refuses placements.** A plate is laid three voxels over the slab and the same dab run underneath it: `trees` plants 3 in the open and 0 under the ceiling, `grass_tufts` 63 and 12. This is the half of the fit test that fails silently — one that never refuses looks exactly like one that works, until a tree grows through a roof. The slab the test builds reserves 26 voxels of sky above it for the same reason: with two voxels of headroom every tree is correctly refused, and the test learns nothing about trees.

Two switches exist for the same reason the mode and tool ones do — a preview is reached by dragging on the image, and a screenshot or a smoke test has no way to drag:

```bash
EDITOR_START_MODE=brushes ./scene_editor <scene>              # open on the Lab
EDITOR_BRUSH_DEMO=1 EDITOR_START_BRUSH=cracked_rock ./scene_editor <scene>
```

`EDITOR_BRUSH_DEMO` turns preview on and lands one dab a few frames in. It hunts outward from the centre of the image for geometry rather than assuming the middle is on it — the Lab's column is a different shape from the Viewport panel the camera was framed for, so the scene is routinely off to one side, and a demo dab reporting "nothing under the cursor" would look exactly like a broken preview.

## Loading is two-phase

Releasing the old scene and building the new one in the same frame keeps both resident: bgfx frees a destroyed texture only after the frames that might still reference it have been rendered. A 3.2 GB scene reloaded that way asks an 8 GB card for 6.4 GB, and the Vulkan driver dies mid-submit. So a load releases, lets eight frames pass, then builds — the viewport shows its empty state for those frames. A bad path is rejected *before* anything is torn down.

## Selecting a component: the yellow outline

Clicking a row in the Assets panel outlines every `.data` box it covers in yellow in the Viewport — the node itself if it's a Chunk or Grid, every occupied cell if it's a Grid, or every leaf beneath it if it's an Asset folder (`main.cpp`'s `collectLeafChunks`). The Inspector's "Boxes" count is the same set.

Implementing it was mostly a matter of getting the projection math exactly right rather than anything algorithmically hard: each chunk's OBB corners (`header.position + header.rotation * localOffset`, the convention `fetchVoxelColor` and `pickVoxel` already use) are projected with `worldToViewportPixel`, which is the algebraic inverse of the shader's `rayStartDirection` — build the same camera basis (right/up/forward from yaw+pitch), but instead of turning a screen UV into a ray direction, turn a world-space offset into a screen UV. No new render pass, no GPU work: it's ImGui line-drawing on top of the already-rendered frame, using the same per-frame camera state the render loop already tracks. The one bug it caught along the way was unrelated to the outline itself — a stale "last item" in the old hierarchy's click handler (see git history) that meant clicking a tree node never actually selected it.

## What's Next

* **One brush registry.** Programmable brushes are a tool now (Custom, `Ctrl+U`), but the five native sculpt brushes are still `SculptBrush` — an enum with switch statements in five places, entirely separate from the scripted path. Folding them into the same registry is what would prove the interface: Sphere, Cube, Smooth, Bump and Extrude registering through it, rather than a hypothetical script.
* **Scatter brushes.** Declared and checked, but not run. What is missing is one placement pass and three host services every scatter brush would otherwise reimplement: surface sampling, blue-noise thinning to a minimum spacing (deterministic from a coordinate hash, so re-dabbing does not double-plant), and a fit test. The output mode then splits by scale — grass blades **bake** into the target's voxels, because half a million components is madness; trees **instance**, which is nearly free since `geometryPool` refcounts blobs and two hundred trees sharing one cost a single upload.
* **A brush preview in the viewport**, and a highlight of the face Extrude would take. Sculpting commits on the first frame of a drag and shows the result; what it cannot show is what the *next* press would affect. Both are already computed — the brush cell by `processSculptSample`, the face by `gatherFaceRegion` — so this is an outline to draw, not a calculation to add. The face highlight matters most: how far a face spreads depends on the scene's materials, and right now the only way to find out is to drag it.
* **Rotating the cube brush.** The box is axis-aligned in the component's voxel space. The Tool panel used to advertise `WASDQE` rotation, which never existed and has been removed rather than left as a promise; a rotated box means rasterising an oriented box into the grid rather than scanning an axis-aligned one.
* **Deletion is soft, and assemblies exercise it hard.** `deleteComponent` releases the geometry, unlists the chunk and renames the whole subtree `__deleted__`, but cannot erase it: handles are indices into `scene.components` and `scene.chunks`, so erasing one would have to rebase every handle in the scene, in the undo history's closures, and in the editor's own state. A long session of placing and baking parts therefore grows both vectors slowly. A reaper that compacts them and remaps handles is the real fix; a session-length cap is not.
* **Undo of an additive stroke leaves empty cells behind.** Removing the voxels does not remove a Grid cell the stroke caused to be created — an empty chunk, drawing nothing and costing a header row. Harmless, and the next save drops it, but a cell reaper would be tidier.
* **Scaling a lifted part.** The gizmo has no scale handles, and scaling voxels is a resample rather than an edit. A *procedural* part resizes by regenerating, which is exact and free; a lifted or imported one does not resize at all. A resampling lift is the missing piece, and it is a different operation from everything the fold does today.
* **A procedural source in the compose schema.** An item written to disk carries its geometry, not the recipe that made it, so a reloaded asset's primitives come back as voxels and stop being resizable. Writing `"source": "proc:box?w=48&h=64&d=48"` — something the loader can satisfy without touching the disk — is the difference between an asset you can come back to and a mesh. `Part::procedural` already carries everything such a source would need to say.
* **Moving an imported asset between voxel scales.** An import arrives at its own scale and therefore lands as **Place**; setting it to a boolean folds it into the open asset's lattice, which is a resample the fold performs without saying so. It should say so, and offer to rebuild the item at the asset's scale instead.
* **`duplicateComponent` still does not handle Grid components.** Nothing in the fold path calls it — a lift builds a fresh Chunk through `queueVoxelAdd` — but the contents list's Duplicate menu item does. Stated here so nobody "optimises" the lift into it later.
