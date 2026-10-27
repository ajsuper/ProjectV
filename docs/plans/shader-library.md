# Shader library and an engine basic renderer

Status: **approved**, 2026-10-27 (open questions answered: tolerance D5, `projv::cam` removed D4,
embedded engine shaders D3, S5 in scope with modularity first D8). Not started. Comes before
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

## Open questions

None open. Answered 2026-10-27: tolerance (D5), `projv::cam` (D4), embedded (D3), S5 in scope (D8).
