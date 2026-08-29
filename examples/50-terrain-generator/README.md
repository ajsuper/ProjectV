# 50 — Terrain Generator

An infinite Perlin-noise terrain, generated on worker threads and streamed around the camera as you
fly through it. Chunks appear, refine as you approach, and are evicted behind you; you can add and
carve geometry while it streams.

This is the first example where the scene is **not** a fixed thing loaded at startup. Everything
before it opens a file; this one produces its world as you move, which changes almost every
assumption the earlier examples were allowed to make.

## Why

Three problems only show up once geometry arrives at runtime, and this example exists to work
through all three:

**Generation cannot happen on the main thread.** A 256³ chunk of noise takes long enough that
producing one per frame would stutter visibly. So generation runs on a pool of workers, and the
main thread does nothing but drain finished results and upload them.

**Uploads have to be incremental.** Rebuilding the whole scene's GPU textures whenever a chunk
lands would cost more than the generation did. The engine's `flushSceneUpdates` uploads only the
blobs marked dirty, which is what makes a steady stream of new chunks affordable.

**Detail has to be spatially graded.** Everything at full resolution exhausts memory within a few
hundred metres of the camera. Chunks are generated coarse and *refined* in place as you approach,
so the resolution you are paying for tracks the distance you are looking at.

## What it uses the engine for, and what it builds itself

The engine supplies the grid, the incremental GPU upload, the editing API and the renderer. It does
**not** supply a job system, and the ~2,300 lines here are mostly that gap:

- a fixed-size thread pool with priority queues, backpressure and dedup tracking
- a result ring the main thread drains under a strict lock ordering discipline
- admission and eviction policy keyed on distance from the camera
- the LOD refinement ladder

[`ENGINE_OPPORTUNITIES.md`](ENGINE_OPPORTUNITIES.md) is a detailed catalogue of exactly which parts
of this ought to be engine infrastructure rather than example code, written against the line
numbers. It is the most useful thing in this directory if you are thinking about where the engine
should grow next; roughly 1,800 of the 2,300 lines are infrastructure every streaming application
would otherwise re-implement.

## Chunks live in a grid, not loose

Earlier examples place chunks individually ("loose"). A streaming world cannot: with thousands of
chunks, a shader that has to test every one of them per pixel spends all its time on broadphase.

A `SceneGrid` is a regular lattice of cells, so the shader's `marchGrid` DDA path walks it
directly — the traversal *is* the broadphase, and cost stops scaling with chunk count. The grid
also expands in either direction as you fly, including into negative coordinates, which is what
`expandGridToInclude` and the `originCellCoord` rebasing exist for.

## How to build

```bash
cmake --preset dev
cmake --build --preset dev --target terrain_generator
```

## How to run

```bash
cd build/examples/terrain_generator
./terrain_generator
```

Terrain generates around the origin and keeps generating as you move. Startup is quick; the world
fills in over the first few seconds.

| Input | Action |
|-------|--------|
| `W` / `A` / `S` / `D` | Move |
| `R` / `F` | Up / down (fly mode) |
| Mouse | Look around (`Esc` releases the cursor) |
| `1` | Fly camera (default) |
| `2` | Player camera — gravity and terrain collision; `Space` jumps |
| `E` | Place a sphere of voxels 500 units ahead of the camera |
| `Q` | Carve the same sphere out of the terrain |
| Scroll wheel | Raise / lower the sun |

`E` and `Q` are the interesting ones: they are the engine's editing API
(`queueVoxelAdd`/`queueVoxelRemove` → `updateScene`) applied to geometry that was generated rather
than loaded, on a grid that may have to expand to accept the edit.

## Renderers

Two, both borrowed from [30-renderers](../30-renderers/):

- `worldCascadeRenderer/` — world-space radiance cascades. The default.
- `fastRenderer/` — direct sun plus RTAO. Cheaper, for when generation is what you are profiling
  and you do not want the renderer in the measurement.

Switching is an edit to the `loadRendererSpecification` path in `startup()`. Unlike the gallery,
this example has no `--renderer` flag; it is a terrain demo that happens to need a renderer rather
than a comparison of them.

## Measuring it

`measure.sh` runs the binary and averages the reported frame times, which is the honest way to
compare a change: frame time here moves with how much generation is in flight, so a single reading
means little.

Performance logging is on by default in the `dev` preset (`PROJV_LOG_PERF`). The lines worth
watching are `generateChunkVoxels` (worker cost), `updateChunkFromBrickMap` (tree64 build) and
`FLUSH` (how many blobs were uploaded this frame).

## ProjectV features used

| Feature | Usage |
|---------|-------|
| **Core ECS** | `Application`, global resources, system stages |
| **Core math** | Camera basis, collision, noise sampling |
| **Logging** | `info`/`perf` — the streaming and upload statistics |
| **Voxel management** | `createVoxelBrickMap`, `brickMapSetVoxel`, `updateChunkFromBrickMap`, `internChunkGeometry` |
| **Material** | `internMaterial` — per-component palettes for surface materials |
| **Editing** | `queueVoxelAdd`, `queueVoxelRemove`, `updateScene`, `expandGridToInclude` |
| **Scene query** | Grid cell lookup, component transforms |
| **GPU interface** | `createTexturesForScene`, `flushSceneUpdates`, `updateChunkHeader` — incremental upload is the load-bearing one |
| **Disk I/O** | `loadRendererSpecification`, `loadShader` |
| **Render instance** | Window creation, active renderer |
| **Perform renderer** | `renderConstructedRenderer`, `setUniformToValue` |

## Third-party dependencies

- [stb_image](https://github.com/nothings/stb) — the blue-noise LUT
- `renderdoc_app.h` — optional RenderDoc capture hooks
- [bgfx](https://github.com/bkaradzic/bgfx) and [GLFW](https://www.glfw.org/) (via ProjectV)

Noise is generated in-tree (`include/terrain_noise.hpp`, `include/noise.hpp`); nothing is pulled in
for it.
