# 30 — Renderers

Four renderers, one scene, one camera. The comparison is the point: each answers "how should this
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
makes one renderer differ from another lives in its folder. Adding a fifth is dropping in a
directory and adding one entry to `buildRendererRegistry()`.

That is also why this example is the right place to learn the renderer format: there are four
worked examples of it.

## The four

| Renderer | Lighting | Noise strategy | Notes |
|---|---|---|---|
| [tree64](renderers/tree64/) | full path trace | accumulate; hard reset on camera move | The baseline. Converges to a reference image while parked. |
| [taa](renderers/taa/) | full path trace | temporal reprojection + denoise | Stays converged *while moving*. Teaches the reprojection machinery every temporal filter uses. |
| [fast](renderers/fast/) | none — direct sun + RTAO | deterministic; TAA for AA only | Cheapest. Nothing to converge because nothing is random. |
| [reconstruction](renderers/reconstruction/) | direct sun + ambient, spatially softened shadow | half-resolution trace, rebuilt to full from voxel face identity | The most effective trick for a weak GPU: a quarter of the pixels traced. `Q` toggles the reconstruction off (point-sampled) for the A/B. No README; the comments in `main.cpp` and its shaders are the description. |

Read them in that order: accumulation, then reprojection to survive motion, then dropping GI
entirely for speed, then tracing fewer pixels and reconstructing the rest.

## How to build

```bash
cmake --preset dev
cmake --build --preset dev --target renderer_gallery
```

That builds every renderer's shaders too. All four share the engine's voxel DDA
(`pjv_utils_DDA.sc`, on the shader include path automatically) and this example's own
`sharedShaders/` — the sun/sky model and the cascade maths — so they stay in step with each other
and with the engine.

## How to run

```bash
cd build/examples/renderer_gallery
./renderer_gallery --renderer fast
```

```
--renderer <name>    which renderer to run (default: tree64)
--scene <directory>  any folder holding a compose.json (default: the bundled Sponza)
--list               print the available renderers and exit
```

Selection is a flag rather than a prompt so the gallery can be scripted. A screenshot matrix or a
benchmark across all four is a shell loop:

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
