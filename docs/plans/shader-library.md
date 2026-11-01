# Shader library and an engine basic renderer

Status: **in progress**: S0 and S1 built 2026-10-28, S2 2026-10-30, S3 and S4 2026-11-01; S5 next. Approved 2026-10-27 (open questions answered:
tolerance D5, `projv::cam` removed D4, embedded engine shaders D3, S5 in scope with modularity first D8). Comes before
[ui.md](ui.md), whose passes (blur, composite) are built from it.

Goal: what every renderer re-derives -- the camera, random numbers, sampling, filtering, reflection
models, atmospheric scattering -- is written once, in the engine, on every shader's include path. And
a program that only wants to *see* its scene gets a renderer from the engine instead of copying one.

**The rule: mechanism, not policy.** The library provides functions that take their parameters: a
phase function, a transmittance, a camera ray, a GGX lobe. It does not provide a look: no sky colours,
no sun, no fog density, no field of view, no tone choices beyond the colour convention's. Those stay in
each renderer's folder, written with the library's functions.

## Why: where it stands (2026-10-27)

Three files already form a library, on every shader's include path through `projv_compile_shaders`,
used by 40 shaders: `include/pjv_color.sc` (89 lines), `include/pjv_bsdf.sc` (201),
`include/pjv_utils_DDA.sc` (4,459: the traversal, materials, the motion table). Beyond those, the 134
shader files (24.5k lines) copy each other:

| What | Copies |
|---|---|
| The fullscreen `vs_quad.sc` | 11 copies, identical |
| `uniform vec4 cameraPos; cameraDir;` | 23 files; FOV a `#define 60.0` in shaders and a constant in C++ that must agree |
| A camera ray (`rayDirection`, `grrCameraRay`, `upscaleRayDirection`, `viewRay`) | 4+ files |
| World-to-UV reprojection (`worldToUV`, `grrWorldToUV`, `projectIntoPreviousFrame`) | 4 files |
| `texelTent` | 5 |
| Hash and RNG (`hashSeed`, `randomUnit`, `halton`) | 4 each |
| `basisAround`, `sampleSunCone`, octahedral decode | 2-3 each |
| Fog optical depth and in-scatter (`grrFogOpticalDepth`, `fogOpticalDepth`) | 2 implementations |
| Mie / Henyey-Greenstein phase (`pjvMiePhase`, `pathPhase`) | 2 |
| `grr_common.sc` (voxel renderer, night renderer); `pjv_sun_sky.sc`, `pjv_cascade_common.sc` (gallery, advanced renderer) | copied, then drifted (`pjv_sun_sky.sc`: 195 diff lines) |

Mechanism and policy are mixed in the same files: `grr_common.sc` has the camera ray next to the sky's
colours; `pjv_sun_sky.sc` has a Mie phase function next to `SUN_WARMTH`; `pjv_utils_DDA.sc` defines
`WORLD_SCALE` and `RENDER_MODE`.

The basic renderer (`examples/shared/basicRenderer`, `basic_renderer.h`) is used by six feature
examples, staged into each by `RENDERER ../shared/basicRenderer`.

## Decisions

**D1. Mechanism in the engine, policy in the renderer.** A library function takes what it needs as
arguments, or reads an engine-owned uniform (D4). A file in the library declares no other uniform and
no constant that is a look. The test for a line: would two renderers that look different want
different values? Then it is policy.

**D2. Layout.** `shaders/include/` becomes the engine's shader include root (replacing `include/` for
`.sc`), one file per subject, every file guarded, every symbol prefixed `pjv`:

| File | What | From |
|---|---|---|
| `pjv_math.sc` | constants, `pjvBasisAround`, octahedral encode/decode, safe normalize | `pjv_sun_sky.sc`, `pjv_cascade_common.sc` |
| `pjv_random.sc` | integer hashes, `pjvRandomUnit`, Halton, R2, Bayer | 4 copies of each |
| `pjv_sampling.sc` | cosine hemisphere, uniform cone, sphere, disk | `sampleSunCone`, `pjvProbeCosineDir` |
| `pjv_camera.sc` | the engine camera (D4): ray from a pixel, world to UV, reprojection to the previous frame, jitter | `grrCameraRay`, `worldToUV`, `projectIntoPreviousFrame` |
| `pjv_microfacet.sc` | reflection models: Lambert, GGX D, Smith G (separable, height-correlated), Schlick and exact dielectric Fresnel, their sampling and pdfs | `pjv_bsdf.sc`'s pieces |
| `pjv_bsdf.sc` | the engine material (`decodeMaterial`'s fields) as a BSDF, built from `pjv_microfacet.sc`. It stays: it is what the engine's material data *means*, not a look | as is |
| `pjv_atmosphere.sc` | phase functions (Rayleigh, Henyey-Greenstein, Cornette-Shanks), Beer-Lambert transmittance, exponential height-fog optical depth (analytic), distance fog, ray-sphere for a planet's shell, single-scatter in-scatter integral | `grrFogOpticalDepth`, `fogOpticalDepth`, `pjvMiePhase`, `pathPhase` |
| `pjv_filter.sc` | tent, Catmull-Rom, Gaussian weights, separable blur taps, bilateral weight terms | `texelTent` x5, `sampleCatmullRom`, `geomWeight` |
| `pjv_color.sc` | as is: the colour convention | as is |
| `pjv_material.sc`, `pjv_motion.sc`, `pjv_scene.sc` | `pjv_utils_DDA.sc` split by subject (S5) | `pjv_utils_DDA.sc` |
| `pjv_engine.sc` | every engine-owned uniform, declared once (`passTargetRes`, `passInputRes`, `pjvAnimTime`, the camera) | scattered |

Files keep their `pjv_` names, so `#include "pjv_color.sc"` resolves unchanged. During S5
`pjv_utils_DDA.sc` is a temporary file that includes the split ones; S5 ends by deleting it, each
shader including only what it uses (D8).

**D3. Engine shaders are embedded in the library.** The engine's own shaders (`shaders/engine/`: the
fullscreen `vs_quad`, the basic renderer, later the UI's) are compiled with shaderc's `--bin2c` once
per profile the build targets -- SPIR-V (Vulkan, Linux and Windows) and Metal (Apple); another
profile when a backend needs it -- into generated headers in the build tree, compiled into `projv`.
`graphics::engineShader("vs_quad")` returns the binary for the running renderer type
(`bgfx::getRendererType()`), ready for `bgfx::createShader` or a `Shader::shaderFileContents`. Nothing
is staged and nothing is read from disk. *Why:* removes the 11 copies, and a program cannot run with
engine shaders that do not match its engine. Cost: each engine shader is compiled once per profile,
and the library carries their bytes (the basic renderer's fragment shader includes the traversal:
on the order of 100-150 kB a profile).

**D4. A camera model, owned by the engine; `projv::cam` goes.** `graphics::CameraView` in
`world.ctx()`: position, orientation, vertical FOV (or orthographic height), near and far, sub-pixel
jitter. The renderer driver uploads it, and the previous frame's, as engine-owned uniforms before each
pass, as it does `passTargetRes`. `pjv_camera.sc` reads them. A renderer stops declaring
`cameraPos`/`cameraDir` and stops defining `FOV`. *Why:* the C++ and the shaders hold the camera's FOV
separately today, and reprojection needs the previous frame's camera, which each renderer currently
tracks in its own uniforms. Perspective and orthographic are both mechanisms; the program picks.
`data_structures/camera.h` and its global `projv::cam` are deleted in S2; their one user, the terrain
generator (29 uses), moves to `CameraView`.

**D5. A hoist changes nothing visible, within a stated tolerance.** Each milestone captures every
renderer in the gallery and every demo before and after (`--capture`, at camera poses the user picks;
stochastic renderers with the same seeds and the same accumulated frame count) and compares them with
a small tool (`tools/imgdiff`, on bgfx's bundled `stb_image`). A capture passes when, over its 8-bit
channels:

| Measure | Limit | Why this number |
|---|---|---|
| Mean absolute error | ≤ 0.1% (0.26 / 255) | Reordered floating-point arithmetic moves values by a fraction of a step; a real change (a different constant, a lost term) moves a whole image more than this |
| Pixels off by more than 2 / 255 | ≤ 0.1% | A difference of 1-2 steps is rounding, dither or the last bit of a hash; more than that on more than one pixel in a thousand is a different value somewhere |
| Largest single difference | reported, not limited | One pixel on a silhouette or a firefly may flip; the two limits above catch it if it is not alone |

A failing capture is looked at, not re-thresholded: either the hoist changed a value (fix it), or the
copies genuinely differed and the change is meant (its commit says so and the user checks it). Where
two drifted copies disagree (D2's "From" column), each renderer keeps its own *policy* and calls the
shared *mechanism*: nothing is unified that would change a look.

**D6. The basic renderer moves into the engine.** It is a mechanism: a way to see a scene without
writing a renderer. `graphics::installBasicRenderer(app, renderInstance, BasicRendererOptions)` adds
a Render system that flushes the scene's changes and draws from `CameraView`. Its look (sun
direction, sun and sky colours, ambient) is in `BasicRendererOptions`, with neutral defaults. The
crosshair option leaves it: a crosshair is UI, drawn by a program (or the dev layer).
`examples/shared/basicRenderer` and `basic_renderer.h` are deleted; Hello Voxel still spells the
steps out by hand, as it teaches them.

**D7. The library is checked.** A ctest compiles a test fragment shader that includes every library
file, alone and together, for each profile the tree builds (SPIR-V, GLSL; HLSL and Metal where
shaderc has them), and a scan fails on a `uniform` outside `pjv_engine.sc` or a symbol without the
`pjv` prefix. `docs/reference/shaders.md` has a row per file.

**D8. Modularity first: a shader compiles only what it uses.** Measured here before: a shader's
compiled size costs speed directly. A second inlined march call site made every mode of one shader
~2x slower; compiling the animated march out of the physics demos' G-buffer (`PJV_NO_ANIMATION`)
took it from 9.3 to 3.4 ms. Today the traversal's opt-ins are *runtime* flags (`PJV_Q_TRANSPARENCY`,
`PJV_Q_REFRACTION`, `PJV_Q_ANIMATION`, `PJV_Q_WANT_MATERIAL`, `PJV_Q_ANIM_DEBUG_SOLID`), so every
shader that marches compiles all of them; the biggest binary is 1.2 MB (the advanced renderer's
G-buffer). So:

1. **Every library file includes what it needs and nothing else**, and nothing in the library is an
   umbrella. A shader that wants filters gets filters, not the traversal.
2. **Each traversal feature is a compile-time gate, off by default:** `PJV_SCENE_TRANSPARENCY`,
   `PJV_SCENE_REFRACTION`, `PJV_SCENE_ANIMATION`, `PJV_SCENE_MATERIAL`, `PJV_SCENE_DEBUG`, defined by
   the shader before it includes `pjv_scene.sc`. Off, the code is not there. The opt-in functions
   that set a feature's runtime flag exist only when its gate is defined, so asking for a feature a
   shader did not compile in is a compile error, not a silent no-op. Within a shader that has a
   feature, the runtime flag still picks per query. `PJV_NO_ANIMATION` becomes the default.
3. **No merging of divergent loops to save size.** One loop with one march call for the loose-chunk
   loop and the grid walk was 2-4x slower (lanes desync). Gates cut code; they do not restructure
   control flow.
4. **Sizes are checked.** A script (`tools/shader_sizes`) lists every compiled shader's size; each
   milestone records the table before and after in this plan. No shader grows without a reason in its
   commit. S5 is where sizes are expected to fall, for every shader that carries a feature it never
   asks for.
5. **Timings.** The gallery and the demos' benches (`DEMOS_BENCH`, `MAYHEM_BENCH`) are compared before
   and after S2 and S5, same window size, one interleaved run, with the user told before it starts.
   The march keeps one call site per shader.

## Milestones

| | What | Proven by |
|---|---|---|
| **S0** | `shaders/include/` root, `pjv_engine.sc`; embedded engine shaders (D3); the 11 `vs_quad` copies replaced; `tools/imgdiff` and `tools/shader_sizes`, and the baseline captures and size table | every example runs; compile test (D7) |
| **S1** | `pjv_math`, `pjv_random`, `pjv_sampling`, `pjv_filter`; the copies replaced | captures within D5; sizes |
| **S2** | The camera model (D4); every renderer's camera uniforms, FOV defines and reprojection replaced; `projv::cam` deleted | captures within D5; sizes; timings; a unit test of `CameraView`'s uniforms against a CPU ray |
| **S3** | `pjv_microfacet` (the BSDF rebuilt on it); `pjv_atmosphere`; each renderer's fog and sky become its own policy over them | captures within D5; sizes |
| **S4** | The basic renderer in the engine, embedded (D6); the six feature examples switch | each runs; `docs/guides` page for it |
| **S5** | `pjv_utils_DDA.sc` split by subject, then deleted; the feature gates (D8.2); each shader defines the gates it uses; `WORLD_SCALE` and `RENDER_MODE` leave the library | captures within D5; sizes fall; timings no worse, better where a shader shed features |

Then [ui.md](ui.md) starts, on S0's embedded engine shaders and S1's filters.

## As built

**S0** (2026-10-28).

- `include/*.sc` moved to `shaders/include/`, installed as `<prefix>/include/projv/shaders`, and found
  there by `projv_compile_shaders`.
- `pjv_engine.sc` declares `passTargetRes`, `passInputRes[8]`, `pjvAnimTime` and `pjvMotionSets[48]`.
  It replaces 48 declarations in renderer shaders and two in `pjv_utils_DDA.sc`, which now has an
  include guard.
- The scene's samplers and texture sizes (`tree64Data`, `tree64Dims`, ...) are engine-owned too, but
  they stay in `pjv_utils_DDA.sc` until S5 moves them to `pjv_scene.sc`.
- `shaders/engine/vs_quad.sc` is compiled into the library by `cmake/ProjectVEngineShaders.cmake`
  (`--bin2c`; `PROJV_ENGINE_SHADER_PROFILES`, spirv off Apple and metal on it).
  `graphics::loadEngineShader("vs_quad")` / `engineShader(name[, type])` read it from
  `graphics/engine_shaders.h`.
- The 11 copies of `vs_quad.sc` are deleted. All 14 call sites (examples, demos, manual tests) use the
  embedded shader. Renderer folders no longer carry a vertex shader.
- Tools:
  - `tools/shader_sizes.py` (`--save`, `--against`, `--root` for a baseline worktree).
  - `projv_imgdiff` (`tools/imgdiff/`; the measure is in `imgdiff.h`, unit-tested).
- `ctest -R shader_library` (D7) compiles every library file alone and together on SPIR-V, GLSL 440
  and Metal, and scans for stray uniforms and unprefixed symbols. `pjv_utils_DDA.sc` is exempt from
  the scan until S5.
  - Only Metal rejects an undefined name in an unused function; the other two profiles drop the
    function first. That is why Metal is in the list.
  - HLSL needs the Windows compiler and is not checked.

**Sizes.** No renderer shader changed size (98 compared, total 5902.2 kB); the engine's `vs_quad`
(0.9 kB) replaces 11 copies. 69 binaries differ in their bytes, because moving the declarations
changes the order of the uniforms and so their register offsets and SPIR-V ids.

**Captures.** The new binary captured all five gallery renderers at frame 300 (default framing,
840x1072), once with the new shaders and once with the HEAD shaders swapped in. All pass D5:
- four are bit-identical;
- restir-pt's largest difference is 1/255, with mean 0.0000%.

**Baseline views** (picked 2026-10-28). Each milestone captures every gallery renderer at these
views and compares the captures with `projv_imgdiff`:

| Scene | Command (from `build/examples/renderer_gallery`) |
|---|---|
| Material room: glossy floor, metal, mirror, colour bleed | `GALLERY_SUN=19.0125 GALLERY_CAPTURE_AT=300 ./renderer_gallery --scene material-room --renderer <r> --look 12.68,2.63,3.31,5.64,2.11,10.39 --capture <prefix>` |

SanMiguel, the default scene, is not used: its materials are plain, with no water and no
reflections. Water, sky and fog are tested separately, in Teardown scenes and in the terrain
generator. The terrain generator first needs a key that prints the current view and captures by
frame rather than by time.

Largest shaders (the table the later milestones are measured against):

| Shader | kB |
|---|---:|
| `40-advanced-renderer/.../gbuffer.bin` | 1219.2 |
| `60-scene-editor/renderRenderer/.../path_trace.bin` | 606.4 |
| `60-scene-editor/editorRenderer/.../albedo.bin` | 530.6 |
| `60-scene-editor/editorRenderer/.../gi.bin` | 259.1 |
| `30-renderers/restir-pt/.../sample.bin` | 209.7 |
| `40-advanced-renderer/.../probe.bin` | 190.3 |
| **all 99** | 5903.2 |

**S1** (2026-10-28).

New library files:
- `pjv_math.sc`: `PJV_PI`, `PJV_TWO_PI`, `pjvBasisAround` (moved from `pjv_bsdf.sc`), and the
  octahedral `pjvOctEncode`/`pjvOctDecode`.
- `pjv_random.sc`:
  - `pjvPcg`, then `pjvRandomUnit` (the 32-bit scaling) and `pjvRandomUnit24` (the 24-bit one);
  - `pjvHashSeed`;
  - `pjvHash13`/`pjvHash23`/`pjvValueNoise`, moved from `pjv_utils_DDA.sc`;
  - `pjvInterleavedGradientNoise`, `pjvBayer4`, `pjvHalton`.
- `pjv_sampling.sc`: `pjvSampleCosineHemisphere` and `pjvSampleUniformCone`, both in local space.
- `pjv_filter.sc`: `pjvTexelTent` and `pjvCatmullRomTaps`.

The 24-bit and 32-bit PCG forms both stay. They give different values, and each renderer keeps the one
it had: restir-pt and pt-reference use the 24-bit form; the editor, the advanced renderer and the DDA's
peel use the 32-bit one.

Replaced:
- PCG, 8 copies. Most were spelled out; the two in pt-reference and restir-pt were named `refPcg` and
  `restirPcg`.
- `hashSeed` ×5, `basisAround` ×4, `texelTent` ×5, `bayer4` ×2, `halton` ×4, `sampleCatmullRom` ×2.
- Interleaved gradient noise in 11 files: 3 as functions, the rest inline. Where restir-pt adds a
  frame offset inside the outer `fract`, it is now `fract(pjvInterleavedGradientNoise(p) + offset)`.
  That is the same value except for float rounding, and moves four restir-pt shaders by +0.1-0.3%.
- The octahedral codecs, in restir-pt and the cascade code.
- The four sun-cone and three cosine samplers. Their bodies now call the library, and each keeps its
  own basis. The night renderer's tangent comes from +y and the probe's from the least-aligned axis;
  changing either would change the noise pattern.

Left as they are:
- The one-off hashes (`fract(sin)` ones, `pathHash`, `grrHash3`, `smokeHash`). Each uses its own
  constants, so they are separate functions, not copies.
- `geomWeight`. Its exponent and `PLANE_LEAK_SCALE` are policy.
- The copies of `pjv_cascade_common.sc` and `pjv_sun_sky.sc` in `30-renderers` and
  `40-advanced-renderer`. Example sharing examples is outside this plan; each now calls the library.
- Gaussian and separable-blur weights. Nothing in the tree has them yet; they arrive with the UI's
  blur (ui.md U4).

**Sizes.** 93 of 99 compiled shaders are the same size. The six that changed are within ±0.3%
(probe, raygi, and four restir-pt passes). The total is unchanged at 5903.2 kB.

**Captures.** All five gallery renderers at the baseline view pass against the S1 "before" set:
- four are bit-identical;
- restir-pt has mean 0.0016%, largest 13/255, the same as its run-to-run noise (0.0005%, 13/255).

The ESSL 300 output of the small shaders, which bgfx's GLSL optimiser inlines completely, is identical
for 54 of 83. The big traversal shaders pass through the optimiser unchanged, so this check says
nothing about them.

**Programs outside the gallery** (2026-10-30). Each was captured with the S1 shaders, then with the S0
binaries swapped into its staged folders, and every pair is bit-identical:

| Program | Capture |
|---|---|
| Advanced renderer | `ADVANCED_CAPTURE`, frame 240 |
| Explosion demo, city map, graphics 3, night renderer | `EXPLOSION_RECORD=30`, frames at 2.0 s, before the detonation |
| Scene editor viewport (albedo, shade, GI) | `EDITOR_BENCH=60`, `EDITOR_BENCH_SIZE=960x600`, SanMiguel |
| Scene editor Render mode (path_trace) | `EDITOR_SAVE_RENDER=150`, SanMiguel |

Sibenik cannot be used: its `.data` is version 1, which the loader rejects.

**S2** (2026-10-30).

*Where it differs from D4:*
- `CameraView` is in `data_structures/camera_view.h` and namespace `projv`, with the other plain data
  types, rather than `graphics::`. Its functions are in `graphics/camera.h`.
- The camera is set per renderer (`graphics::setCamera(renderer, view)`, stored on
  `ConstructedRenderer`), not read from `world.ctx()`. The driver (`renderConstructedRenderer`,
  `performRenderPasses`) takes a renderer and has no world. More to the point, the previous frame has to
  be the view that renderer last drew from: its history textures are from then. The editor's viewport
  and path tracer each keep their own history, where before they shared the editor's.
- Orientation is a forward vector with the engine's no-roll basis, the convention every renderer
  already used. `CameraView` also has an orthographic height and backoff (the editor's orthographic
  and isometric views), near/far, and a sub-pixel jitter. Every renderer still makes its own jitter
  in its shader.

*Built:*
- Seven engine uniforms in `pjv_engine.sc`: `pjvCameraPos` (w: FOV), `pjvCameraDir` (w: orthographic
  height), `pjvCameraLens`, `pjvCameraJitter`, and the previous frame's `Pos`/`Dir`/`Lens`. The driver
  sets them before every pass.
- `pjv_camera.sc`:
  - any camera: `pjvCameraBasis`, `pjvPerspectiveRay`, `pjvOrthographicOrigin`,
    `pjvPerspectiveUV`, `pjvOrthographicUV`;
  - the engine's: `pjvView*`, `pjvViewRayOrigin`/`Direction`, and `pjvPrevViewUV`, which reprojects
    under the projection that frame had.
- `graphics/camera.h` is the same arithmetic on the CPU: `cameraBasis`, `cameraRayOrigin`/`Direction`,
  `cameraWorldToUV`, `setCamera`, `cameraUniformValues`.
- `pjv_utils_DDA.sc`'s `rayStartDirection` calls `pjvPerspectiveRay`.

*Moved onto it:* every renderer's shaders. That is the gallery's five, the basic, voxel, night and
advanced renderers, both editor renderers, and the party and playground HUDs.
- The 23 `#define FOV 60.0` (and `GRR_FOV`, `RESTIR_FOV`, `REF_FOV`) are gone.
- So are the declarations of `cameraPos`/`cameraDir`/`prevCameraPos`/`prevCameraDir`/`cameraProjection`,
  and the 25 hand-copied bases.
- The copied rays and inverses now call `pjv_camera.sc`: `worldToUV`, `grrCameraRay`, `grrWorldToUV`,
  `restirWorldToUV`, `rayDirection`, `upscaleRayDirection`, `viewRay`, `primaryRay`,
  `projectIntoPreviousFrame`.
- C++: every `setUniformToValue("cameraPos"...)` is now `setCamera`. The gallery, the editor and
  restir-pt's `Frame` no longer track the previous camera for upload. restir-pt's pixel-width term
  reads the FOV from the camera rather than a literal 30°.

*`projv::cam`:* `data_structures/camera.h` is deleted. The plan was wrong that the terrain generator
used it: its 29 `cam.` uses are its own `CameraState cam`, and nothing included the header.

*Sizes:* +0.2% in total (5903.2 → 5916.2 kB); 49 of 99 shaders unchanged.
- The traversal shaders are within 0.4%.
- The small post-processing passes grow 2-10%. The field of view is now a uniform, so
  `tan(radians(fov / 2))` is computed at run time instead of folded into a constant. And the
  editor's reprojections branch on the previous frame's own projection.

*Captures:*
- Gallery at the baseline view, five renderers: all pass. restir-pt mean 0.029%; the others ≤ 0.0015%,
  a handful of silhouette pixels flipped. The window came up at 840×520, not 1072 tall, so the S1
  captures were retaken at the same size, just before, from a build of S1.
- Advanced renderer (frame 240): passes. Mean 0.0005%, 0.01% of pixels over 2/255.
- Explosion demo and the editor (viewport, Render mode): S1 captured; S2 not yet.

*Not done:* an orthographic editor capture, since the bench views are perspective only.

*Timings* (D8.5, measured 2026-11-01, after S4). Before is S1 (f59f8a1) with only the gallery's
fixed bench window and no-input commits cherry-picked, so both run at 1280x720. After is c664a39,
which includes S3 and S4. Each was built in a clean worktree, and the runs were interleaved
before, after, before, after.
- Gallery `--bench` (GPU ms a frame, frames 120..360), at the baseline view and at SanMiguel's
  default framing:

  | Renderer | Room, before | Room, after | SanMiguel, before | SanMiguel, after |
  |---|---:|---:|---:|---:|
  | pt-reference | 8.65, 8.66 | 8.59, 8.67 | 9.71, 9.75 | 9.73, 9.72 |
  | fast | 2.13, 2.09 | 2.12, 2.08 | 2.11, 2.10 | 2.09, 2.13 |
  | reconstruction | 1.02, 1.01 | 1.02, 1.01 | 1.18, 1.20 | 1.21, 1.18 |
  | restir-pt | 4.64, 4.70 | 4.63, 4.70 | 4.68, 4.68 | 4.68, 4.67 |
  | voxel | 1.68, 1.60 | 1.69, 1.66 | 2.05, 2.03 | 2.06, 2.05 |

- Voxel Mayhem, `MAYHEM_BENCH=cannon MAYHEM_GPU=1` (restir-pt): 8.03 and 7.93 ms before, 8.02 and
  7.94 after.
- No pass moved by more than 0.03 ms, which is inside run-to-run noise. Uploading the camera, and
  then its tangent, costs nothing measurable.
- `DEMOS_BENCH` no longer exists; the physics example has no render bench.

*Tests:* `camera:*` in `test_shaders.cpp`:
- the ray equals `utils::rayDirectionThroughImage` for four views, the near-vertical fallback
  included;
- `cameraWorldToUV` inverts the ray, perspective and orthographic, and rejects a point behind;
- the uniforms' layout.

The manual tests `editing_p1` and `edit_demo` were found not to compile, for reasons that predate this
plan: known-latent-issues #16.

**S3** (2026-11-01).

*Library:*
- `pjv_microfacet.sc`: Lambert, GGX D, Smith G1 and height-correlated G, Schlick and exact dielectric
  Fresnel, the cosine and GGX-half samplers, and the power heuristic. The terms that were in
  `pjv_bsdf.sc` moved unchanged, and `pjv_bsdf.sc` is now the engine material built on them.
  `PJV_BSDF_PI` gave way to `PJV_PI`.
- `pjv_atmosphere.sc`: the isotropic, Rayleigh, Henyey-Greenstein, Schlick and Cornette-Shanks phase
  functions; Beer-Lambert; `pjvExpMean`; exponential height layers (density, rise, optical depth);
  distance fog; ray-sphere.
- Not added: the single-scatter in-scatter integral. No renderer has one, and its sample count and
  density profile are a renderer's to pick. It is written when one asks for it.

*Moved onto it:*
- Phase functions:
  - the advanced renderer's `pjvMiePhase` is Schlick's approximation, not HG, and is now
    `pjvPhaseSchlick`;
  - pt-reference's `pathPhase`, restir-pt's `restirPhase` and the editor's `phaseHenyeyGreenstein` are
    now `pjvPhaseHenyeyGreenstein`. The editor's extra outer clamp could not take effect at its
    g ≤ 0.95.
- The editor's preview lobe in `albedo.frag` (its copies of `smithG1`, `fresnelSchlick`,
  `alphaFromGlossiness`, `sampleGGXHalfVector`) is now `pjv_bsdf.sc`'s.
- The height fogs: the voxel and night renderers' `grrFogOpticalDepth`, the advanced renderer's
  `fogOpticalDepth`, and restir-pt's `restirLayerDepth`.
  - Each keeps its own density, colour, eye clamp and rise floor (−40, −30, −20).
  - Those two bounds go together. With the library flooring the rise at −40 for everyone, the advanced
    renderer's −30 eye clamp no longer matched, so the bounds are the caller's.

*Renamed:* the advanced renderer's own `pjv_atmosphere.sc` is now `pjv_cascade_atmosphere.sc`. The
engine's include root is searched first, so the library's file of that name would have shadowed it.

*Sizes:* 95 of 99 unchanged; volumetric, compose and godrays +0.2 to +0.8%.

*Captures:* everything is bit-identical to S2, except restir-pt (largest 1/255, 0.0008% mean):
- the gallery's five renderers at the baseline view, now in a fixed 1280×720 window;
- the advanced renderer;
- the explosion demo;
- the editor viewport and Render mode.

*Making captures repeatable.* Three changes, each its own commit:
- `RenderInstance::resizable`: the gallery, the advanced renderer and the editor capture in a
  fixed-size window. The desktop's tiling had produced 840×1072, 840×520 and 953×326 on successive
  runs.
- An unattended gallery run, and an advanced-renderer capture, take no input. A click or a mouse
  movement over the window had turned the camera mid-capture. That produced two false failures here
  (3.8% and 0.36% mean), both traced to input.

*S2, settled:* S1 to S2 on the explosion demo is 0.12% of pixels over 2/255, against a 0.1% limit;
the mean is 0.006%. The pixels are isolated, on the window frames' voxel edges. The cause is the field
of view becoming a uniform, so its tangent is computed at run time instead of folded at compile time.
The next commit uploads the tangent instead.

*FOV tangent* (2026-11-01). `pjvCameraLens.w` now holds `tan(fov / 2)`, computed on the CPU in double
precision. `pjv_camera.sc`'s perspective functions take that tangent (`pjvTanHalfFov(degrees)` for any
other camera), and every renderer reads `pjvViewTanHalfFov()`/`pjvPrevViewTanHalfFov()` instead of
computing `tan(radians(pjvViewFov() * 0.5))` per pixel. The old `rayStartDirection(..., pjvViewFov())`
calls are now `pjvViewRayDirection`.
- Against S1, from before the FOV was a uniform:
  - the editor viewport is bit-identical again;
  - the explosion demo is back inside D5: 0.045% of pixels over 2/255, where it was 0.12%.
- Against S3, everything passes: the gallery (restir-pt mean 0.021%), the advanced renderer and the
  editor's Render mode.
- Total size 5915.9 kB, against 5916.4. S2's growth was mostly the uniforms and the basis struct, not
  the tan.

**S4** (2026-11-01).

*Built:*
- `shaders/engine/basic.frag`, embedded beside `vs_quad` (spirv 88.4 kB). Its look is seven uniforms of
  its own (`pjvBasicSun`, `...SunColour`, `...AmbientAbove`/`Below`, `...SkyAbove`/`Below`,
  `...Options`); the resolution is `passTargetRes`, not a `windowRes` the program sets.
- `graphics/basic_renderer.h`:
  - `installBasicRenderer(app, renderInstance, options)`: creates it, makes it active, and adds the
    Render system `"basic renderer"`, which flushes the `Scene` into the `GPUData` and draws;
  - `BasicRendererOptions` (the defaults are the old look) and the `BasicRenderer` ctx resource;
  - `createBasicRenderer` and `drawBasicRenderer`, for a program that draws by hand.
- `constructShaders` takes a `Shader` with no path as its contents, so a `RendererSpecification` can
  be built in memory. That is how `createBasicRenderer` builds one: no folder, no JSON.

*Where it differs from D6:*
- **The crosshair stays**, as `BasicRendererOptions::crosshair`, until a program can draw its own. The
  sandbox, physics and multiplayer client aim with it, and there is no UI layer yet. It goes in
  [ui.md](ui.md) U5. Asked 2026-11-01.
- **The camera comes from a `CameraView` in `world.ctx()`**, which the system copies to the renderer
  with `setCamera`. That is D4's first idea, which S2 moved off the driver: the driver still takes the
  camera per renderer, and only this system reads the ctx.
- **Hello Voxel keeps a renderer folder**, `examples/00-hello-voxel/renderer`, since it teaches the
  folder. It is the shared folder moved there, without the crosshair or any uniforms of its own. It is
  what the guides now say to copy.

*Switched:* the scene previewer, entities, sandbox, physics, world loading and the multiplayer client.
Each one's Render system now sets the `CameraView`, ordered `{.before = "basic renderer"}`. The
previewer's bench is a separate system, `{.after = "basic renderer"}`. The `exit_path` manual test
uses `createBasicRenderer` and no longer changes directory to find a folder.
`examples/shared/basicRenderer` and `basic_renderer.h` are deleted.

*Docs:* [Draw a scene](../guides/draw-a-scene.md) (new), `graphics/basic_renderer.h` in
[graphics](../reference/graphics.md), and the pages that pointed at the shared folder: first app,
write a renderer, rendering, the renderer-folder format, and the examples' READMEs.

*Fixed on the way:* `ctest -R shader_library` had failed since S0 on this machine (CMake 3.31). Run with
`-P`, the script set no policy version, so `if(... IN_LIST ...)` was an error. Its own commit.

*Sizes:* the engine gains `basic_spirv`, 88.4 kB, and the shared `basic.bin` (87.7) is gone. Hello
Voxel's copy is 86.8 kB. The other 98 shaders are unchanged.

*Captures:* the sandbox, `SANDBOX_MEASURE=100` with `SANDBOX_CAPTURE` (a still scene, with the
crosshair), passes against HEAD: mean 0.0000%, largest 1/255. Two runs of HEAD are bit-identical. The
1/255 comes from the ambient term, now a mix of two colours rather than `0.3 + 0.1 * n.y`, and from the
sun being normalised on the CPU.

*Checked:* `DEMOS_SELFTEST=1 ./physics` passes, as do `WORLD_LOADING_SELFTEST`, `PREVIEW_BENCH`, the
multiplayer `selftest.sh` and a windowed client. Hello Voxel and entities run with no errors, and
`exit_path a` and `b` pass.

**S5** (in progress, 2026-11-01). Four steps, each its own commit: split, gates, includes, prefix.

*Split.* `pjv_utils_DDA.sc` (4,419 lines) is now three files and a temporary umbrella:
- `pjv_motion.sc` (the motion table, the sway and advection fields);
- `pjv_material.sc` (the palette and material bytes, `VoxelMaterial`, `decodeMaterial`);
- `pjv_scene.sc` (everything else: the scene's textures, `RayQuery`, the march, the animated march,
  the peel, `raySceneIntersect`, `fetchVoxel*`).

`pjv_utils_DDA.sc` includes `pjv_camera.sc` and `pjv_scene.sc`, and keeps `WORLD_SCALE` until its users
define it. The tree walk's leftovers went with the split, about 500 lines that nothing referenced: the
move LUT, the node stack, `inverseZOrderIndex`, `calculateZOrderIndex`, the box-from-parent helpers,
the ray-box exits, `tree64s`, `headersLength`, `rayStartDirection`, `returnStruct`, `Voxel`,
`RENDER_MODE`, `MAX_STACK_SIZE` and `VOXEL_TYPEDATA_SLICES`.
- The scene's samplers stay in `pjv_scene.sc` and `pjv_material.sc`, not `pjv_engine.sc`. Every pass
  includes the engine file, and the advanced renderer's `accumulate.frag` binds its own texture at
  stage 9.
- Sizes: 99 of 100 shaders unchanged; restir-pt's `shafts` moved by bytes. The traversal shaders'
  bytes differ at equal size, because the samplers are now declared in a different order.
- Captures: pt-reference, fast, reconstruction and voxel at the baseline view are bit-identical to
  c664a39.

*Gates* (D8.2). `PJV_SCENE_TRANSPARENCY`, `PJV_SCENE_REFRACTION` (needs transparency),
`PJV_SCENE_ANIMATION`, `PJV_SCENE_MATERIAL` and `PJV_SCENE_DEBUG` (needs animation, for the
envelope view), all off by default.
- Off, a gate's code is not compiled, and neither is its opt-in (`pjvQueryTransparency`, ...).
  Asking for a feature that is off is a compile error. Defining the old `PJV_NO_ANIMATION` is also
  an error, so a stale define cannot pass silently.
- What each gate takes out:
  - transparency: the peel's consume in the march, the solo-interval pass, and every pass of the
    scene query after the first;
  - refraction: the bend, and the peel's hand-back of a bending interface;
  - animation: the animated march and its resolve, and the march's animated skip;
  - material: the march's leaf-material fetch, unless transparency or animation needs it anyway.
- Shaders that ask:
  - the advanced renderer's G-buffer, all four;
  - its probe pass, material;
  - the editor viewport and path tracer, transparency and refraction;
  - the editor's GI and shade passes, transparency.
  The six `PJV_NO_ANIMATION` defines are gone: off is now the default.
- `ctest -R shader_library` also compiles `pjv_scene.sc` with every gate on.
- Sizes, against S4: 25 shaders smaller, none larger, 6003.5 -> 5554.9 kB (-7.5%).
  - The editor's viewport (`albedo`, 530.9 -> 326.5 kB) and path tracer (607.0 -> 395.7 kB) carried
    the animated march without ever animating. They set their query flags at run time, so the
    compiler could not prove the march dead.
  - Shaders whose queries were constant (`pjvPrimaryQuery` with no options) had it folded away
    already, and shrink 1-3% (the basic renderer, the gallery's G-buffers, the night renderer).
  - The advanced G-buffer, which asks for everything, is -0.4%.
- Captures, against c664a39, with the working tree's uncommitted `RESTIR_GI_MAX_GLOSSINESS` 0.9 in both:
  - bit-identical: the gallery's five renderers at the baseline view, the sandbox's still scene, the
    editor viewport and Render mode;
  - the advanced renderer, largest difference 1/255.

*Includes.* `pjv_utils_DDA.sc` is deleted.
- The 20 files that included it now include `pjv_scene.sc`. Every one marches rays, so none gets
  less than the traversal, and since S2 each includes `pjv_camera.sc` itself.
- `WORLD_SCALE` has left the library. The cascade files already defined it, and the fast
  renderer's G-buffer, its one other user, defines its own.
- Comments and docs that named the file or `rayStartDirection` now name `pjv_scene.sc` or
  `pjvPerspectiveRay`.
- Sizes are unchanged from the gates step: the umbrella added only `pjv_camera.sc` and
  `WORLD_SCALE`, and nothing else changed.

*Prefix* (D2, D7). Every file-scope symbol of the three files has the prefix:
- functions: `pjvRaySceneIntersect`, `pjvFetchVoxelMaterialFromHit`, `pjvDecodeMaterial`, ...; `headers`
  is `pjvHeader`;
- structs: `PjvRay`, `PjvRayQuery`, `PjvSceneHit`, `PjvSceneIntersectData`, `PjvVoxelMaterial`,
  `PjvChunkHeader`, `PjvMotionSet`, ...;
- defines: `PJV_MAX_PEEL_ITERATIONS`, `PJV_MAX_REFRACTION_SEGMENTS`, `PJV_MATERIAL_INDEX_NEEDS_DESCENT`.

How it was done:
- Shaders: renamed everywhere. `Ray`, `Grid`, `headers` and `tree64` are also English words, so in
  comment text those four were left alone.
- C++ comments and the docs (not the plans, which are history): only the names that are not also C++
  identifiers (`VoxelMaterial`, `MotionSet` and `chunkHeader` are C++ types too).
- Checked first: no renderer defined a symbol of any old name (the editor's `MAX_PEEL_ITERATIONS`
  overrides renamed with it), and none of the new names existed anywhere.

Result:
- Sizes identical for all 100 shaders, as a pure rename gives: SPIR-V carries no names.
- `ctest -R shader_library` scans every library file now. The scene's textures are its one
  exception to the uniform rule: engine-owned, but declared by `pjv_scene.sc` and `pjv_material.sc`,
  which read them. A deliberate stray uniform and an unprefixed function in `pjv_motion.sc` were both
  caught.

## Open questions

None open. Answered 2026-10-27: tolerance (D5), `projv::cam` (D4), embedded (D3), S5 in scope (D8).
