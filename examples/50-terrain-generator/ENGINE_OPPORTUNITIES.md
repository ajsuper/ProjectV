# Engine Opportunities in the Terrain Generator

This document catalogues everything the terrain generator example currently
handles itself that should instead be generic engine infrastructure.  The
example is ~2,345 lines.  Roughly 400–500 lines are terrain-specific (noise,
trees, surface materials); the remaining ~1,800 lines are infrastructure that
every user of the engine will need to re-implement.

---

## 1. Async Generation Pipeline (Threading + Work Queues)

### Short Outline

The example hand-rolls a complete multi-threaded work pipeline: a fixed-size
thread pool with priority queues, backpressure, dedup tracking, result
draining, and per-worker stats.  The engine provides **none** of this — no
thread pool, no work queue, no result ring buffer, no backpressure mechanism.

### Deeper Analysis

**What the example builds** (lines 168–214, 1170–1258):

- `WorkerState` — a struct carrying `std::deque<ChunkWorkItem>` (work queue),
  two `std::vector<ProcessedChunk>` (readyChunks / readyRefines), dual
  mutex+CV pairs with a strict lock ordering discipline, an `std::atomic<bool>
  running` flag, a per-LOD completed counter, and a `queuedCoords` dedup set
  maintained incrementally rather than rebuilt each frame.

- `terrainWorkerFunc` — a free function that becomes `std::thread::detach()`ed
  N times.  It waits on `workCv` with a 100ms timeout, applies backpressure
  only to non-priority items (the `jumpQueue` predicate), pops work from the
  front of the deque, calls `generateChunkVoxels`, packs the result into a
  `ProcessedChunk`, and pushes to `readyChunks` or `readyRefines` under the
  result mutex.

- The main-thread drain (`generatePendingChunks`, ~230 lines) locks
  `resultMutex`, pops refines and admissions from the two result queues,
  applies admission logic (`admit` lambda) and refine installation
  (`replaceChunkGeometry`), then locks `workMutex` to push new work and erase
  consumed dedup entries.

**What the engine provides:** *Nothing.*  There is no threading primitive in
`include/core/` or `src/core/`.  Every user who wants background generation,
streaming, or compute must build their own thread pool.

**What the engine should provide:**

| Primitive | Notes |
|-----------|-------|
| `AsyncPipeline<WorkItem, ResultItem>` | Template with `numWorkers`, `maxWorkQueueDepth`, `maxResultBacklog`, `maxResultsPerFrame` |
| Priority enqueue (`enqueue(item, priority)`) | `push_front` for priority (refines, re-checks), `push_back` for bulk |
| `drainResults(admitFn) -> bool` | Main-thread drain with user-provided admission callback; returns true if anything changed |
| Backpressure predicate | Workers block if result-backlog cap is reached, unless item is priority |
| Per-category stats | Atomic counters indexed by work-type tag |
| `shutdown()` | Graceful drain + thread join |

**Lines to move:** ~180 (WorkerState + terrainWorkerFunc) → `src/core/async_pipeline.cpp`

---

## 2. LOD Sweep Scheduling (Frame-Budgeted Cursor Sweep)

### Short Outline

The engine already has `requestChunkLOD` (the *mechanism* for changing a
blob's render resolution), but the example must build the entire *policy*: a
distance-to-LOD-ring function, a cursor-based sweep with per-frame budgets,
deferred-refine tracking, refine-handle guarding, and the `lodSweepNeeded`
gating flag.  Any application that uses LOD will rebuild this sweep.

### Deeper Analysis

**What the example builds** (lines 273–310, 1309–1318, 1357–1513):

- A 3-ring scheme: LOD 0 (`kLodRadiusFine` = 5 chunks, resolution 256³),
  LOD 1 (up to `kLodRadius` = 22 chunks, resolution 64³), LOD 2 (beyond 22,
  resolution 16³).  This is *policy* — the radii and resolutions are
  application-specific.

- `applyLODRing` (~150 lines) is a cursor-based partial sweep:
  - Gated by `lodSweepNeeded` so it does not run every frame
  - Walks `activeChunks` at budgeted `kMaxLodScanPerFrame` = 8192 coords/frame
  - Expands the scan radius by `max(2, moved)` to cover chunks that left the
    LOD disc since the last frame
  - Calls `requestChunkLOD` for every resident chunk; collects
    `NeedsRegeneration` results into `toRefine`, counts `Coarsened` as
    `applied`
  - Tracks `refiningHandles` to avoid re-queuing the same chunk while its
    refine is in flight
  - Returns `applied > 0` so the caller knows whether a GPU flush is needed
  - Resets the cursor only on large camera jumps (`moved > 2`), so continuous
    flight makes forward progress

**What the engine provides:**

- `requestChunkLOD(Scene&, ChunkHandle, uint32_t requestedRes)` — the atomic
  operation.  Computes `levels = log4(native / requested)`, sets
  `chunk.requestedLOD`, may call `setBlobRenderLOD` (which marks
  `blob.dirty`), returns `Unchanged / Coarsened / NeedsRegeneration`.
- `setBlobRenderLOD`, `GeometryBlob::renderLOD`, `Chunk::requestedLOD`,
  `ChunkHeader::traversalLOD` — all the storage side for LOD truncation.
- `downsampleTree64` — the actual GPU-side decimation (called inside
  `flushSceneUpdates`).

**What the engine is missing — the *sweep orchestrator*:**

| Component | What It Does |
|-----------|--------------|
| Sweep cursor | Advances a scan position over the active-chunk map across frames so one frame never walks 700k entries |
| Per-frame budget | `kMaxLodScanPerFrame` + `kMaxRefinesPerFrame` — how many chunks to visit and how many async refines to queue |
| Movement margin | Expands the scan radius to catch chunks that crossed the LOD boundary since the last scan |
| Deferred-refine tracking | Counts `NeedsRegeneration` candidates past the refine budget so the sweep re-runs next frame |
| Refine-handle guard | `refiningHandles` set prevents duplicate refine requests while one is in flight |
| Gate flag | `lodSweepNeeded` — only sweeps when an answer can have changed (camera moved, refine completed, chunk admitted coarser-than-desired) |
| Admission refine routing | Newly-admitted chunks that need regeneration queue a refine directly rather than waiting for the sweep cursor to reach them |

**Lines to move:** ~170 (`applyLODRing` + cursor logic + gating) →
`src/utils/lod_sweep.cpp`, driven by a user-provided `LODPolicy` struct.

**What stays in the example:** The 3-ring radii, resolution schedule, and
per-LOD octave detail.  These are terrain-specific.

---

## 3. World Streaming (Ring Admission + Eviction)

### Short Outline

The example builds a camera-relative streaming ring: precomputes sorted XZ
offsets, maintains a cursor (`pendingIndex`) into that ring, admits
`kMaxNewPerFrame` chunks per frame, handles the "known empty" sentinel
protocol for culling open sky/bedrock, expands the `SceneGrid` on demand,
reuses freed chunk slots, and budget-evicts distant chunks.  The engine has
`SceneGrid` and `expandGridToInclude` but no streaming harness.

### Deeper Analysis

**What the example builds** (lines 401–449, 1512–1553, 1558–1788):

- A nearest-first sorted template of XZ offsets (`ts.xzOffsets`), built once at
  startup from all (dx, dz) pairs inside `kViewRadius`.  Moving the camera only
  retargets the origin (`ts.pendingOrigin = camChunk`), so the ring is a
  sliding window.

- `pendingIndex` is a cursor through this template, resetting to 0 when the
  camera chunk changes.  `generatePendingChunks` walks at most
  `kMaxScanPerFrame` = 16384 coords/frame and pushes at most
  `kMaxNewWorkPerFrame` = 512 items to the work queue.

- Admission (`admit` lambda, ~100 lines): checks for existing resident chunks
  and empty sentinels, rejects stale results (coords now outside view radius),
  calls `expandGridToInclude`, inserts into `grid.cellToChunk`, records the
  `kInvalidChunkHandle` sentinel for empty results (with
  `emptyChunks[coord].{lod, marginal}` so a finer ring can later overrule),
  reuses recycled chunk slots from `freeChunkSlots`, calls `internChunkGeometry`
  + `requestChunkLOD`, and directly queues refines for chunks generated at a
  coarser resolution than the camera currently needs.

- Eviction (`evictDistantChunks`, ~42 lines): budgeted to `kMaxEvictPerCall` =
  512 per frame, with `kEvictHysteresis` = 2 chunks of overlap so a chunk
  oscillating on the boundary is not thrashed.  Calls `releaseChunk` and
  collects freed handles into `freeChunkSlots`.

**What the engine provides:**

- `SceneGrid` with `origin`, `cellSize`, `rotation`, `dims`, `cellToChunk`,
  `originCellCoord`.
- `expandGridToInclude(SceneGrid&, ivec3 cell, Scene&, int gridIndex)` —
  dynamic grid reallocation that shifts the origin in both directions.
- `internChunkGeometry` / `forkBlob` / `releaseBlob` — geometry pooling with
  ref-counted COW.
- `Chunk::gridIndex` / `cellIndex` — direct chunk→grid-cell binding for O(1)
  eviction lookup.

**What the engine is missing — the *streaming harness*:**

| Component | What It Does |
|-----------|--------------|
| Ring template | Precomputed nearest-first offset list around a camera centre |
| Streaming cursor | Advances over the ring template, resets on chunk-boundary crossing |
| Admission budget | Caps new chunks per frame (`kMaxNewPerFrame`) and scan work per frame (`kMaxScanPerFrame`) |
| Empty-sentinel protocol | Records which ring decided a coord was empty so a finer ring can overrule; tracks marginal vs. deep emptiness |
| Eviction budget | Caps evictions per frame; hysteresis to avoid boundary thrash |
| Slot recycling | `freeChunkSlots` vector reused by admission |

**Lines to move:** ~400 (`generatePendingChunks` + `evictDistantChunks` +
`releaseChunk` + `admit` lambda + ring template setup) → `src/utils/streaming.cpp`

---

## 4. GPU Dirty Tracking and Auto-Flush

### Short Outline

The example manually wires a `needsFlush` boolean through every mutation site:
`generatePendingChunks` returns `generated > 0`, `applyLODRing` returns
`applied > 0`, and the caller ORs them together.  The engine has the
incremental upload machinery (`flushSceneUpdates` → `uploadDirtyBlobs` →
`updateDirtyHeaders`) but no automatic dirty detection — the caller must
remember to flush.

### Deeper Analysis

**What the example builds** (lines 2078–2080):

```cpp
bool needsFlush = generatePendingChunks(scene, ts);
needsFlush |= applyLODRing(scene, ts, camChunk);
if (needsFlush) projv::graphics::flushSceneUpdates(scene, gpuData);
```

This is error-prone in two directions: (1) forgetting to flush means dirty
blobs and header rows never reach the GPU, leaving chunks at stale
resolutions; (2) flushing when nothing is dirty costs an upload pass.

**What the engine provides:**

- `GeometryBlob::dirty` — set by `forkBlob`, `internChunkGeometry`,
  `setBlobRenderLOD`, and `replaceChunkGeometry`.
- `Chunk::headerDirty` — set by `replaceChunkGeometry`,
  `requestChunkLOD` (when LOD changes but blob doesn't), and P6
  transform operations.
- `flushSceneUpdates` — iterates dirty blobs, uploads only changed texels
  via `bgfx::updateTexture2D`, rewrites only dirty/new header rows, and
  falls back to a full repack when allocators overflow.

**What the engine should provide:**

- A single entry point: `Scene::dirty` (bool) set by any mutation.
- `Scene::flushIfDirty(GPUData&)` — calls `flushSceneUpdates` if dirty, clears
  dirty flag.  The application never wires `needsFlush` manually.

**Lines to move:** The `needsFlush` pattern in the example (~3 lines) is
symptomatic of a missing engine pattern.  The actual change is ~5–10 lines in
`scene.h` + `gpu_interface.cpp`.

---

## 5. Programmatic Scene Grid Creation

### Short Outline

The engine loads grids from `compose.json` (via `compose_io.cpp`) but has no
C++ API for creating a streaming grid at runtime.  The example manually
constructs a `SceneGrid`, sizes its `cellToChunk` array, sets `originCellCoord`,
and wires its `componentHandle` — 30 lines that should be one function call.

### Deeper Analysis

**What the example builds** (lines 1811–1837):

```cpp
SceneGrid grid;
grid.origin = vec3(-r * kChunkSize, yMin * kChunkSize, -r * kChunkSize);
grid.cellSize = kChunkSize;
grid.dims = ivec3(2 * r + 1, yMax - yMin + 1, 2 * r + 1);
grid.rotation = quat(1, 0, 0, 0);
grid.cellToChunk.assign(dims.x * dims.y * dims.z, -1);
grid.originCellCoord = ivec3(-r, yMin, -r);
// ... and later:
ComponentRecord comp;
comp.kind = ComponentKind::Grid;
comp.gridIndex = gridIndex;
comp.name = "terrain";
```

**What the engine should provide:**

```cpp
// One-line creation:
auto [gridIndex, compHandle, gridHandle] =
    scene.createStreamingGrid("terrain", ivec3{110, -1, 110},   // min cell
                                           ivec3{110, 16, 110}, // max cell
                                           kChunkSize);
```

**Lines to move:** ~30 → one function in `scene.h` / `src/utils/scene_builder.cpp`.

---

## 6. Material Palette Lifecycle

### Short Outline

The example manually interns colours via `internMaterial` (the thread-safe
path), tracks palette versioning to decide whether to rebuild the global
palette texture, and builds its own surface-colour lattice quantizer to stay
within the 255-entry palette budget.  The engine provides `internMaterial` and
`rebuildGlobalPaletteTexture` but no higher-level palette budget enforcement.

### Deeper Analysis

**What the example does** (lines 490–640, 927–941, 1873–1895):

- A fixed 216-entry colour lattice (6 levels per RGB channel) plus a few
  water colours to fit in 255 palette entries.
- `internColor` lambda caches per-voxel seen colours so duplicate per-chunk
  colours only cost one lookup per unique colour, not one per voxel.
- Tree materials (leaves, bark) are quantized to the same lattice at startup.
- A `bakeMaterials` function writes per-voxel IDs using the scheme's material
  lookup.

**What the engine provides:**

- `internMaterial(Scene&, ComponentRecord&, vec4 color)` — thread-safe
  palette insertion that returns a material index and auto-expands the
  palette.
- `rebuildGlobalPaletteTexture` — packs all components' palettes into the
  GPU palette texture, returns whether it grew.

**What the engine should provide:**

- A `PaletteBudget{ maxEntries }` that applications can configure so
  `internMaterial` returns an error or evicts least-recently-used entries when
  full.
- A default quantizer utility that maps an arbitrary colour to the nearest
  entry on a lattice — the 6-level RGB lattice is generic enough to be a
  utility parameterised by levels-per-channel.

**Lines to move:** The palette budget enforcement and quantizer (~120 lines) →
`src/utils/palette_budget.cpp` or `include/utils/material.h`.

---

## 7. Entry-Point Plumbing (ECS Resource Wiring)

### Short Outline

`main()` wires all ECS resources by hand: `createGlobalResource<Scene>`,
`createGlobalResource<GPUData>`, `createGlobalResource<TerrainState>`,
`createGlobalResource<CameraState>`, `createGlobalResource<RenderInstance>`.
It also attaches the three lifecycle callbacks (`startup`, `update`, `render`).
This is boilerplate every application will repeat.

### Deeper Analysis

**What the example builds** (lines 2296–2316):

```cpp
createGlobalResource<Scene>(world);
createGlobalResource<GPUData>(world);
createGlobalResource<TerrainState>(world);
createGlobalResource<CameraState>(world);
createGlobalResource<ConstructedRenderer>(world);
createGlobalResource<RenderInstance>(world);
Application{}.run(startup, update, render);
```

**What the engine should provide:**

A `StandardApplication` subclass that pre-registers `Scene`, `GPUData`,
`VirtualCamera`, and `RenderInstance` as global ECS resources.  The user
provides `startup(Scene&)`, the engine wires the rest.

```cpp
StandardApplication app;
app.run([](Scene& scene, Application& app) {
    // user startup — scene is ready, GPU data is allocated
});
```

**Lines to move:** ~15 lines of resource registration → `Application` base
class.

---

## Summary Table

| # | System | Example LOC to Move | Engine Module | Priority |
|---|--------|---------------------|---------------|----------|
| 1 | Async generation pipeline | ~180 | `core/async_pipeline` | High |
| 2 | LOD sweep scheduling | ~170 | `utils/lod_sweep` | High |
| 3 | World streaming | ~400 | `utils/streaming` | High |
| 4 | Auto-flush dirty tracking | ~5 (wiring fix) | `graphics/gpu_interface` | Medium |
| 5 | Programmatic grid creation | ~30 | `utils/scene_builder` | Low |
| 6 | Palette budget + quantizer | ~120 | `utils/material` | Low |
| 7 | ECS resource wiring | ~15 | `Application` base class | Low |
| **Total** | **~920 lines** | | | |

After these moves the terrain generator would drop from ~2,345 lines to
~1,425 lines, of which 400–500 are actual terrain logic and the remainder is
the renderer setup (cascade path tracer, sun/sky, TAA, etc.).