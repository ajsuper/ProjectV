# PathTracer

An interactive real-time path tracer that loads a pre-voxelized scene from disk and renders it into ProjectV's tree64 voxel structure. The camera can be freely navigated. At startup you choose between several independent renderers — the three documented in detail below, plus experimental per-face and world-space radiance-cascade variants.

The included scene is the **Sponza atrium** (`SponzaScene/`), voxelized with [MeshVoxelizer](../MeshVoxelizer/).

## Renderers

On launch you are prompted to select a renderer:

### 1. tree64 (`tree64Renderer/`) — accumulation

The original ProjectV path tracer. It accumulates samples across frames while the camera is still and **resets accumulation whenever the camera moves**. Its four-pass pipeline (`tree64Renderer/render.json`):

1. **path_trace** — casts rays into the tree64 voxel structure and accumulates radiance samples
2. **accumulate** — blends the current frame with the history buffer when the camera is still
3. **denoise_ao_gi** — applies a denoising pass over ambient occlusion and global illumination
4. **post_and_display** — tone-maps and outputs the final image to the screen

### 2. reprojection (`reprojectionRenderer/`) — temporal reprojection

A from-scratch unidirectional path tracer with **temporal reprojection**, so the image stays converged *even while the camera moves* (no hard reset). Each frame path traces a single sample per pixel and reuses history by projecting this frame's world-space hits back through the previous frame's camera. Four passes (`reprojectionRenderer/render.json`):

1. **path_trace** — 1-spp path trace (cosine-weighted diffuse GI + next-event estimation to the sun with shadow rays), writing a G-buffer of radiance, world position, normal and albedo. The primary ray is deterministic (no jitter) so each pixel locks to one surface point for exact history reuse.
2. **reproject_accumulate** — reprojects each pixel into the previous frame, validates the match against the stored position/normal G-buffer (rejecting disocclusions), and folds the new sample into a ping-pong history buffer with a *motion-adaptive* running average. History is point-sampled at the nearest texel (no per-frame bilinear smearing). When the camera is still the average grows very long so noise converges toward zero; when it moves the history is kept short so it stays responsive and does not ghost/smear.
3. **denoise** — an edge-aware spatial à-trous/bilateral filter guided by the noise-free G-buffer (normal + world-position + albedo edge-stopping). This cleans the residual noise that the deliberately-light moving history leaves, so motion is both sharp *and* clean without leaning on heavy temporal blending.
4. **display** — ACES tone-map + gamma to the screen

The reprojection maths (`worldToUV` in `reproject_accumulate.frag`) is the exact inverse of the engine's `rayStartDirection`, and the history buffer is a ping-pong framebuffer (a pass that both reads and writes framebuffer 2), which the engine detects automatically. All renderers share the engine-level voxel DDA traversal in `pjv_utils_DDA.sc`.

A blue-noise LUT (`LDR_RGBA_7.png`) is uploaded to the GPU at startup to decorrelate samples between frames.

### 3. fast (`fastRenderer/`) — deterministic direct lighting + denoised RTAO, TAA-antialiased

A deliberately cheap, (almost) noise-free renderer for when you want a **reliable, performance-friendly** image rather than full global illumination. It casts **no random GI bounces** — every pixel is shaded from a single primary ray plus one hard sun shadow ray, so the raw image is already stable before any temporal work. Ambient occlusion is **ray-traced** (RTAO) but kept cheap by tracing only a couple of rays per pixel and denoising them. Five passes (`fastRenderer/render.json`):

1. **gbuffer_shade** — one primary ray (sub-pixel *jittered* each frame, Halton), shaded with a single **hard sun shadow ray** (crisp deterministic direct light, no soft-shadow noise). It writes a G-buffer with the direct sun light and the sky-ambient term on **separate** targets (plus world position + normal), so AO can occlude the ambient without dimming sunlit surfaces. The sun shadow ray uses a visibility-only trace that skips the expensive per-hit voxel-colour decode (`fetchVoxelData`).
2. **rtao** — ray-traced ambient occlusion (deferred). From each pixel's G-buffer position it casts a few short hemisphere occlusion rays through the voxel DDA. Unlike screen-space AO this is (near) ground truth — it sees occluders that are **off-screen or hidden behind silhouettes**, which is why it reads noticeably better on this blocky geometry. The catch is cost: hemisphere rays are incoherent and the DDA (`pjv_utils_DDA.sc`) has no working LOD, so every ray marches at full leaf resolution and a *single* 24-step AO ray costs ~2× a 128-step sun ray. It's kept affordable the way real-time RTAO is: only **`AO_SAMPLES` rays per pixel per frame** (default 2, a visibility-only trace), with the noise removed by the denoiser below + TAA rather than by brute-force sampling. (An earlier revision used screen-space AO here for speed, but with the denoiser in place RTAO costs about the same and looks better, so it was restored.)
3. **combine_blur** — an **edge-aware bilateral blur** of the raw AO, the spatial half of the denoiser. Weights are stopped on G-buffer depth and normal so occlusion is averaged only across coplanar neighbours (creases/silhouettes stay crisp, flat areas go smooth); because each neighbour pixel traced a *different* rotated direction, a 5×5 blur is worth ~25 effective AO directions. It then applies the blurred AO to the sky-ambient term only (`direct + ambient·ao`) and carries the position/normal forward. The AO ray directions are rotated per pixel (interleaved-gradient noise) *and* per frame, so this blur plus the temporal pass fully resolve the few-ray noise.
4. **taa** — temporal anti-aliasing. When the camera is **still** it accumulates each pixel's own history (identity, texel-snapped, no resampling blur) into a long running mean, so the per-frame jitter and AO rotation resolve into supersampled, crisp, noise-free edges (and the AO converges toward many-sample quality). When the camera **moves** it reprojects through the previous camera, samples history bilinearly, and **clamps it to the local 3×3 colour neighbourhood** (the deghoster) with a short history so motion stays clean and responsive.
5. **display** — ACES tone-map + gamma to the screen.

The only stochastic elements are the sub-pixel jitter (for AA) and the per-frame AO ray rotation (which the denoiser + TAA converge); the lighting itself is deterministic, so a still camera reaches a clean image in a fraction of a second. Per pixel it issues `2 + AO_SAMPLES` scene rays. `AO_SAMPLES` (top of `rtao.frag`) is the quality/perf knob — 1 is about as cheap as the old SSAO, 2 (default) is cleaner, higher is cleaner still. Its TAA reuses the same `worldToUV`/ping-pong reprojection machinery as renderer 2.

### Adding another renderer

Renderers are registered modularly in `main.cpp` via `buildRendererRegistry()`. Each entry names its resource directory, vertex shader, and a per-frame uniform-upload function, so adding a third renderer is a single `push_back` — `startup()` and `render()` need no changes.

## How to Build

```bash
cmake --preset dev
cmake --build --preset dev --target path_tracer
```

That builds every renderer's shaders too. All seven share the engine's voxel DDA
(`pjv_utils_DDA.sc`, on the shader include path automatically) and this example's own
`sharedShaders/` (the sun/sky model and the cascade maths), so they stay in sync.

The binary and its staged renderer folders and scenes land in `build/examples/path_tracer/`.

## How to Use

```bash
cd build/examples/path_tracer
./path_tracer
```

On startup the console prompts you to pick a renderer (`1` = tree64, `2` = reprojection, `3` = fast, `4` = face, `5` = radiance cascades, `6` = radiance cascades per-face; default `1`). The application then opens a 1920×1080 window and begins rendering the Sponza scene. Navigate the scene with the keyboard and mouse:

| Input | Action |
|-------|--------|
| `W` / `S` | Move forward / backward |
| `A` / `D` | Strafe left / right |
| `R` / `F` | Move up / down |
| Mouse | Look around (cursor is captured; `Esc` releases it, left-click re-captures) |
| Scroll wheel | Raise / lower the sun (day–night cycle) |

When the camera is stationary, the accumulating renderers refine the image each frame, progressively reducing noise. Any movement (or a sun change) resets accumulation.

To render a different scene, replace `SponzaScene/` with any Compose scene directory produced by MeshVoxelizer and update the `loadComposeFromDisk` path in `main.cpp`.

## ProjectV Features Used

| Feature | Usage |
|---------|-------|
| **Core ECS** | `Application`, `world`, global resources, system stage assignment (`Startup`/`Update`/`Render`) |
| **Core Math** | `vec2`/`vec3`/`vec4`, `cos`/`sin` — camera direction and uniform packing |
| **Logging** | `info`/`warn` via the spdlog wrapper for structured output and frame profiling |
| **Compose I/O** | `loadComposeFromDisk` — loads a Compose scene folder and flattens it into a Scene |
| **GPU Interface** | `createTexturesForScene`, `GPUData` — uploads voxel scene data to the GPU |
| **Manage Resources** | `RendererSpecification`, `ConstructedRenderer`, `setTextureToData` — builds the renderer pipeline |
| **Disk I/O** | `loadRendererSpecification`, `loadShader` — loads renderer config and compiled shaders |
| **Render Instance** | Window creation, active renderer management, window resolution query |
| **Perform Renderer** | `renderConstructedRenderer`, `setUniformToValue` — drives per-frame rendering and uniform upload |

## Third-Party Dependencies

- [stb_image](https://github.com/nothings/stb) — blue-noise texture loading
- [bgfx](https://github.com/bkaradzic/bgfx) — cross-platform GPU abstraction (via ProjectV)
- [GLFW](https://www.glfw.org/) — window and input handling (via bgfx/ProjectV)
