# 30 — Renderers

Several renderers, one scene, one camera. The comparison is the point: each answers "how should this
scene be lit" differently, and the only way to judge that is to put them side by side under
identical conditions.

The bundled scene is the **Sponza atrium** (`SponzaScene/`), voxelized with
[MeshVoxelizer](../20-mesh-voxelizer/).

## A renderer here is data

A renderer is a folder under `renderers/` holding three things:

```
renderers/<name>/
    render.json      the pass order
    resources.json   the shaders, render targets and framebuffers
    shaders/         the fragment shaders, plus a fullscreen-quad vertex shader
    README.md        what it does and when to reach for it
```

`main.cpp` supplies the window, the camera, the scene and the per-frame uniforms. Everything that
makes one renderer differ from another lives in its folder. Adding another is dropping in a
directory and adding one entry to `buildRendererRegistry()`.

That is also why this example is the right place to learn the renderer format: there are several
worked examples of it, at every scale from four passes to ten.

## The renderers

| Renderer | Lighting | Noise strategy | Notes |
|---|---|---|---|
| [pt-reference](renderers/pt-reference/) | two-bounce path trace through the materials | accumulate; hard reset on camera or sun move | The reference: exact, slow, converges while parked. What the others are measured against. The default. |
| [fast](renderers/fast/) | none — direct + RTAO | deterministic; TAA for AA only | Cheapest. Nothing to converge because nothing is random. |
| [reconstruction](renderers/reconstruction/) | none — direct sun and an ambient term | none needed | Traces at half resolution and rebuilds full from voxel face identity. Q toggles it. |
| [restir-pt](renderers/restir-pt/) | two bounces through the materials, ReSTIR PT at 1/16 of the pixels | path reuse (reconnect / replay) + diffuse and specular denoised apart; ½-res G-buffer reconstructed, then accumulated | G views, J reuse, Q reconstruction, P final render / raw path trace (V vignette, B grade, E auto-exposure, K depth of field, O fog, L sun shafts, SSAO), U soft sun shadow. `--restir-gi 2`: a quarter of the pixels. |
| voxel | ray-traced GI from a probe grid | TAA | The examples' game renderer, from `examples/shared/voxelRenderer`. T toggles TAA. |

`pt-reference` replaced the gallery's two older path tracers, `tree64` and `taa`, and `restir-pt` replaced
`restir` (ReSTIR GI), all on 2026-10-22: neither old path tracer knew about materials, and a reference
must shade what it checks the way the renderer it checks does. The material model all of them share is
the engine's `shaders/include/pjv_bsdf.sc`.

`--scene material-room` opens a room built in main.cpp for comparing materials: a checkered glossy floor,
a metal wall, a mirror panel, a red wall, a skylight and a window, a semi-glossy block, a white cube and a
lamp. See pt-reference's README for comparing restir-pt against the reference on it.

## How to build

```bash
cmake --preset dev
cmake --build --preset dev --target renderer_gallery
```

That builds every renderer's shaders too. All of them share the engine's voxel DDA
(`pjv_utils_DDA.sc`, on the shader include path automatically) and this example's own
`sharedShaders/` — the sun/sky model (with `pjv_path_light.sc`, the path tracers' version of it) and
the cascade maths — so they stay in step with each other and with the engine.

## How to run

```bash
cd build/examples/renderer_gallery
./renderer_gallery --renderer fast
```

```
--renderer <name>    which renderer to run (default: pt-reference)
--scene <directory>  any folder holding a compose.json (default: SanMiguel), or material-room
--list               print the available renderers and exit
--look x,y,z,tx,ty,tz  start the camera at x,y,z looking at tx,ty,tz
--capture <prefix>   save frames 300-302 as <prefix>-<frame>.tga, then exit
```

For measuring: `GALLERY_CAPTURE_AT=4,8,16,300` captures those frames instead, and
`GALLERY_SUN_STEP_AT=n` lowers the sun ~14° at frame n (how fast an image follows a change of light).

Selection is a flag rather than a prompt so the gallery can be scripted. A screenshot matrix or a
benchmark across all of them is a shell loop:

```bash
for r in $(./renderer_gallery --list | grep -oE '^  [a-z-]+'); do
    ./renderer_gallery --renderer "$r"
done
```

| Input | Action |
|-------|--------|
| `W` / `S` | Move forward / backward |
| `A` / `D` | Strafe left / right |
| `R` / `F` | Move up / down |
| `Shift` | Move four times as fast (the base speed crosses the scene in about ten seconds, at most 80 voxels a second) |
| Mouse | Look around (cursor is captured; `Esc` releases it, left-click re-captures) |
| Scroll wheel | Raise / lower the sun |

When the camera is stationary the accumulating renderers refine each frame. Movement, or a sun
change, resets accumulation in `pt-reference` and shortens the history in the reprojecting ones.

## A note on the bundled scene

`SponzaScene/` is a `.data` container **version 1**, which the current loader rejects — so it
parses, reports two components, and contains no geometry. The renderer will start and show an empty
frame, and says so at startup:

```
[WRN] Scene contains no chunks; the image will be empty.
```

Until it is re-voxelized, point `--scene` at something that loads:

```bash
./renderer_gallery --renderer fast --scene ../scene_previewer/scenes/SmallVox
```

See [docs/plans/known-latent-issues.md](../../docs/plans/known-latent-issues.md) for the full list
of affected assets.

## ProjectV features used

| Feature | Usage |
|---------|-------|
| **Core ECS** | `Application`, global resources, all four system stages |
| **Core paths** | `executableDirectory` — the binary enters its own directory so it runs from anywhere |
| **Core math** | `vec2`/`vec3`/`vec4`, `cos`/`sin` — camera direction and uniform packing |
| **Logging** | `info`/`warn`/`perf` via the spdlog wrapper |
| **Compose I/O** | `loadComposeFromDisk` |
| **GPU interface** | `createTexturesForScene`, `setTextureToData`, `destroyGPUData`, `GPUData` |
| **Manage resources** | `RendererSpecification`, `ConstructedRenderer`, `constructRendererSpecification` |
| **Disk I/O** | `loadRendererSpecification`, `loadShader` |
| **Render instance** | Window creation, active renderer, `shouldClose` |
| **Perform renderer** | `renderConstructedRenderer`, `setUniformToValue` |

## Third-party dependencies

- [stb_image](https://github.com/nothings/stb) — the blue-noise LUT (`LDR_RGBA_7.png`)
- [bgfx](https://github.com/bkaradzic/bgfx) — GPU abstraction (via ProjectV)
- [GLFW](https://www.glfw.org/) — window and input (via ProjectV)
