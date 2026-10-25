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
| `[` / `]` | Lower / raise the sun |
| `Q` | Carve the same sphere out of the terrain |

`E` and `Q` are the interesting ones: they are the engine's editing API
(`queueVoxelAdd`/`queueVoxelRemove` → `updateScene`) applied to geometry that was generated rather
than loaded, on a grid that may have to expand to accept the edit.

## The terrain

Rebuilt from scratch to start simple (`include/simple_terrain.hpp`):

- **Height:** one fBm Perlin field (`noise.hpp`, five octaves, ~3,200-unit base wavelength), raised
  to a power so valleys are broad and hills stand up out of them. ~1,150 units of relief.
- **Rock is 3D.** Each voxel is solid where a density is positive: `(h - y) + reach * rockShape(x, y, z)`.
  On flat meadow `reach` is 0 and that is the heightfield; it rises to 42 units (~15 voxels) wherever the
  ground is rock (from ~35 degrees), and half that on steep grass. `rockShape` is mostly the rock's beds,
  each standing out or set back by its own amount and varied along the face, plus broad warped masses --
  so cliffs are shelves of rock over undercuts, with real overhangs, and steep grassy hillsides have
  outcrops under their turf. Only columns the 3D term or a boulder touches do per-voxel work, in a band
  around the surface; open ground keeps the plain heightfield.
- **Boulders:** lumpy, partly buried ellipsoids, merged into the ground with the same density test.
  Rare out in meadows, gathered on the gentle ground at the foot of cliffs, never on a slope. Trees are
  not planted where one sits.
- **Rock colour** is a 3D function on every voxel that can be seen: warm and cool regions, a tone per
  bed (matching the shelves), large Worley blocks with joint lines, broad mottling, rain streaks on
  faces, lichen and moss in patches on the tops.
- **Grass** tinted in broad patches by a low-frequency noise, a few voxels of dirt under it, and
  **grass blades** on top in the nearest ring: one to three voxels, densest in the lush patches,
  lighter toward the tip. (A coarser ring's voxel is wider than any blade, so they stop there.)
- **Conifers** (below).

The palette is twelve named entries (`terrain.grass.*`, `terrain.stone.*`, `terrain.dirt`,
`grass.blade.*`), interned once at startup, so a worker never takes the palette lock.
`TERRAIN_SEED` pins the world; `TERRAIN_CAPTURE=<file>` saves a screenshot after
`TERRAIN_CAPTURE_SECONDS` (default 20) and exits, with `TERRAIN_CAMERA="x,z,above,yaw,pitch"` to place
the camera first -- or `TERRAIN_CAMERA=cliff`, which finds the steepest ground near the origin and
frames it from below.

## Trees

`trees/` holds 21 species, each about 90 voxels tall, **grown by the scene editor's tree generator**
(`../60-scene-editor/brushes/trees.lua`) and stored in the current format, one compose asset per
species: `biomes/alpine/assets/<name>/compose.json` (with its palette) and `model.data`. The terrain plants the ten
**conifers** (spruces, firs, hemlock, larch, juniper, three pines); the broadleaf and bare species are
baked too but skipped by kind for now.

They are stamped into the terrain's own voxels during chunk generation, not placed as components:
see `include/tree_placement.hpp`. Placement is a pure function of world position, so neighbouring
chunks on different threads agree on every tree that straddles them. Two broad noise regions stand
in for the placer's climate fields: one picks which conifer grows (along the species' bands in
`biomes/alpine/designs.json`), the other thins and thickens the stands, and a forest-patch field opens
clearings. Trees keep off the rock: the slope test rejects anything steep enough to be stone.

One tree voxel is one terrain voxel (2.8 units), so a conifer is drawn at full detail in the nearest
ring and stands about 250 units -- ten metres or so against the 40-unit eye height. Each farther
ring reads a copy averaged for exactly its voxel size (4x and 16x).

**Tree colours keep their own palette entries**: every species comes from one brush, so they share
27 hue-shifted colours, interned exactly beside the ground's twelve.

### Biomes and their assets

Assets live per biome, `biomes/<biome>/`: `designs.json` (for each asset, a brush and its parameters)
and `assets/` (the baked compose assets plus `assets.json`, the manifest this example reads -- nothing
in `main.cpp` names a species). `alpine` holds the conifers the terrain plants today; `canyonlands` is
the first biome of the plan in `docs/plans/biome-canyonlands.md`: Utah junipers, pinyons, dead
junipers, sagebrush (the Trees brush) and Wingate, Navajo and Kayenta boulders (`rocks.lua`).

`terrain_asset_baker` grows every design with the editor's brush code (no shape logic of its own) and
adjusts its height until it is the design's `targetHeight`:

```bash
cmake --build --preset dev --target terrain_asset_baker -j$(nproc)
cd build/examples/terrain_generator && ./terrain_asset_baker canyonlands   # rewrites biomes/canyonlands/assets/
```

Rebuild `terrain_generator` afterwards to stage the new assets beside it. The baker needs the scene
editor's Lua submodule; without it the target is skipped and the committed assets are used as is.

`TERRAIN_LAB=assets` replaces the terrain with flat red sand and stands every canyonlands asset on it
in rows -- trees, then shrubs, then boulders -- to judge them one by one under the real renderer:

```bash
TERRAIN_LAB=assets TERRAIN_CAMERA="-400,-260,110,1.5708,-0.25" ./terrain_generator   # the junipers
```

`TERRAIN_LAB=faces` is the canyonlands rock faces as one cross-section (`include/canyonlands.hpp`):
a banded Chinle slope cut by gullies, a jointed Wingate wall with spall scars, wandering and forking
cracks and pour-off chutes that the gullies run down from, Kayenta ledges and bench, Navajo domes --
with Wingate blocks fallen on the slope and a few junipers for scale:

```bash
TERRAIN_LAB=faces TERRAIN_CAMERA="0,260,110,-1.5708,0.30" ./terrain_generator
```

### The canyonlands terrain

The default terrain is the canyonlands biome (`include/canyonlands.hpp`, plan:
`docs/plans/biome-canyonlands.md`); `TERRAIN_BIOME=alpine` brings back the Perlin hills and conifers.
Every column has `e`, its distance into the plateau from the nearest canyon (a network of warped Worley
edges at three scales), and rock is solid where `e` exceeds the strata's setback at that height --
Chinle slope, Wingate wall, Kayenta ledges, Navajo domes. `TERRAIN_CAMERA=canyon` stands on a canyon
floor beside a wall.

## Renderer

**ReSTIR GI** ([`../shared/restirRenderer/`](../shared/restirRenderer/), set up by
[`restir_renderer.h`](../shared/restir_renderer.h), shared with the renderer gallery's `restir`): a soft
sun shadow, two bounces of sun and sky light traced for a sixteenth of the pixels and shared between
pixels and frames, SSAO, distance fog, and a post (auto-exposure, grade, vignette, a slight depth of
field). `[` and `]` lower and raise the sun.

It replaced the feature examples' one-pass basic renderer, which made generation the only thing in the
frame time but drew every voxel as a flat-lit cube. Research into why Teardown's voxels look the way
they do comes back to light, not colour -- soft shadows, AO at two scales and bounce light on one byte of
palette per voxel -- and under real light the terrain's rock and the trees read as rock and trees. The
fog (`View::fogLength`, 8,000 units) also hides where the far LOD rings end. The example uploads the
scene's changes in Update, on its own budget, so it calls `restir_renderer::upload` and draws, without
the flush `restir_renderer::draw` would add.

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
| **Material** | `internMaterial` — per-component palettes for surface materials and tree colours |
| **Compose I/O** | `readDataFile`, `parseComposeJson` — the tree assets; `saveComposeToDisk` in the baker |
| **Editing** | `queueVoxelAdd`, `queueVoxelRemove`, `updateScene`, `expandGridToInclude` |
| **Scene query** | Grid cell lookup, component transforms |
| **GPU interface** | `createTexturesForScene`, `flushSceneUpdates`, `updateChunkHeader` — incremental upload is the load-bearing one |
| **Render instance** | Window creation; the renderer through `basic_renderer.h` |

## Third-party dependencies

- [bgfx](https://github.com/bkaradzic/bgfx) and [GLFW](https://www.glfw.org/) (via ProjectV)

Perlin noise is [siv::PerlinNoise](https://github.com/Reputeless/PerlinNoise), vendored as
`include/noise.hpp`.
