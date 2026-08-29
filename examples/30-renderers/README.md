# 30 — Renderers

Seven renderers, one scene, one camera. The comparison is the point: each answers "how should this
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
makes one renderer differ from another lives in its folder. Adding an eighth is dropping in a
directory and adding one entry to `buildRendererRegistry()`.

That is also why this example is the right place to learn the renderer format: there are seven
worked examples of it, at every scale from four passes to ten.

## The seven

| Renderer | GI | Noise strategy | Notes |
|---|---|---|---|
| [tree64](renderers/tree64/) | full path trace | accumulate; hard reset on camera move | The baseline. Converges to a reference image while parked. |
| [reprojection](renderers/reprojection/) | full path trace | temporal reprojection + à-trous denoise | Stays converged *while moving*. The machinery the others reuse. |
| [fast](renderers/fast/) | none — direct + RTAO | deterministic; TAA for AA only | Cheapest. Nothing to converge because nothing is random. |
| [face](renderers/face/) | full, per voxel face | none needed; exact-key history gate | Zero spatial noise by construction. Blocky by design. |
| [radiance-cascade](renderers/radiance-cascade/) | screen-space cascades | temporal mean | Kept for the comparison. Screen space cannot see off-screen. |
| [world-cascade](renderers/world-cascade/) | world-space cascades | temporal mean on indirect only | The gather traces real voxel rays. Best-looking here. |
| [world-face-cascade](renderers/world-face-cascade/) | world-space cascades, per face | exact-key history gate | Fewest gather rays of the three cascade renderers. |

Read them in that order and the argument is visible: accumulation, then reprojection to survive
motion, then dropping GI entirely for speed, then changing the *unit of work* to kill noise at the
source, then cascades, then fixing what screen space cannot do, then combining the last two ideas.

Per pixel, cost runs roughly `fast` < `face` < `world-face-cascade` < `radiance-cascade` <
`world-cascade` < `reprojection` < `tree64`.

## How to build

```bash
cmake --preset dev
cmake --build --preset dev --target renderer_gallery
```

That builds every renderer's shaders too. All seven share the engine's voxel DDA
(`pjv_utils_DDA.sc`, on the shader include path automatically) and this example's own
`sharedShaders/` — the sun/sky model and the cascade maths — so they stay in step with each other
and with the engine.

## How to run

```bash
cd build/examples/renderer_gallery
./renderer_gallery --renderer world-cascade
```

```
--renderer <name>    which renderer to run (default: tree64)
--scene <directory>  any folder holding a compose.json (default: the bundled Sponza)
--list               print the available renderers and exit
```

Selection is a flag rather than a prompt so the gallery can be scripted. A screenshot matrix or a
benchmark across all seven is a shell loop:

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
| Mouse | Look around (cursor is captured; `Esc` releases it, left-click re-captures) |
| Scroll wheel | Raise / lower the sun |

When the camera is stationary the accumulating renderers refine each frame. Movement, or a sun
change, resets accumulation in `tree64` and shortens the history in the reprojecting ones.

## A note on the bundled scene

`SponzaScene/` is a `.data` container **version 1**, which the current loader rejects — so it
parses, reports two components, and contains no geometry. The renderer will start and show an empty
frame, and says so at startup:

```
[WRN] Scene contains no chunks; the image will be empty.
```

Until it is re-voxelized, point `--scene` at something that loads:

```bash
./renderer_gallery --renderer world-cascade --scene ../scene_previewer/scenes/SmallVox
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
