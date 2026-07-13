# Agent Notes

## Build Commands

### Editing library (Phase 1+)
```bash
cmake -S . -B build && cmake --build build --target projectV-editing
```

### Full project
```bash
cmake --build build
```

### Phase 1 test driver (CPU-only edit test)
```bash
make -C docs/examples/editing_p1 clean && make -C docs/examples/editing_p1
```

Run from project root:
```bash
./docs/examples/editing_p1/editing_p1 [<scene-folder>]
```

### Interactive edit demo (GPU, windowed)
```bash
make -C docs/examples/edit_demo clean && make -C docs/examples/edit_demo
```

Run from `docs/examples/edit_demo/`:
```bash
./docs/examples/edit_demo/edit_demo
```

Controls: WASD/R/F + mouse to fly, E to add voxel (cycles colors), Q to remove,
1-4 to change edit distance.

## Codebase Conventions

- Namespace `projv` for core types, `projv::utils` for utilities, `projv::graphics` for GPU.
- Chunk handle = index into `Scene.chunks` (stable).
- Component handle = index into `Scene.components` (stable).
- Geometry pool blobs are refcounted; `chunk.geometryPoolIndex < 0` = unpooled.
- `chunkQueue` on Chunk is the legacy edit staging area (to be removed after P1 verification).
- `editQueue` on ComponentRecord is the new per-component edit queue (P1+).
- `forkBlob` creates a COW copy without decrementing the original's refCount.

## Phase 1–5 Status (2026-07-11)

### Delivered (Phase 1)
- `scene.h`: Added `PendingVoxelOp`, `ComponentEditQueue`, `DataReference`, `ComponentRecord::editQueue`/`dataRefID`, `Scene::dataReferences`, `forkBlob()`.
- `voxel_math.h/cpp`: Added `floorDiv`/`floorMod` for correct negative-coordinate cell bucketing.
- `include/utils/editing.h` + `src/utils/editing.cpp`: `queueVoxelAdd`, `queueVoxelRemove`, `updateScene` (loose chunks only, always-COW, CPU-only).
- `CMakeLists.txt`: `projectV-editing` static library target.
- `docs/examples/editing_p1/{main.cpp,Makefile}`: Test driver.
- `AGENTS.md`: This file.

### Delivered (Phase 2)
- `expandGridToInclude(SceneGrid&, core::ivec3, Scene&, int)` — expands a grid in both directions, shifts origin so existing chunks don't move, maintains `originCellCoord` for linearization.
- `SceneGrid::originCellCoord` — tracks the block-space coordinate at grid.origin after (possibly negative) expansion.
- `queueVoxelAdd`/`queueVoxelRemove` now accept both Chunk and Grid components.
- `applyComponentQueue` extended for Grid components: cell bucketing via `floorDiv`/`floorMod`, grid expansion, per-cell COW fork + rebuild, automatic new-cell chunk creation.
- `ensureDataReference` extended for Grid components (reads resolution from first populated cell).
- Test driver updated with Sponza grid acceptance test + full programmatic grid test (expansion, COW fork, refCount, dataRefID, origin shift, voxel counts).

### Delivered (Phase 3)
- `convertChunkToGrid(Scene&, ComponentHandle)` — converts a loose Chunk-kind component to a 1-cell SceneGrid. Sets up grid fields (origin, cellSize, rotation, dims=1), registers in `scene.grids`, updates chunk's `gridIndex`/`cellIndex`, removes from `scene.looseChunks`, and flips `comp.kind` to Grid.
- `applyComponentQueue` Chunk path: now checks all ops for overflow (any axis >= resolution or < 0). If overflow detected, calls `convertChunkToGrid` and falls through to the Grid path (same editQueue, same call).
- Test driver: 20-check programmatic loose-chunk overflow conversion test (P3 scope) — verifies conversion to Grid, grid expansion to (2,1,1), COW fork of original chunk, new cell creation for overflow, refCount invariants, loose count decremented, origin unchanged.

### Delivered (Phase 4)
- `GPUChunkHeader`: replaced `padding[2]` with `uint32_t dataRefID; uint32_t padding[1]` — same total size, same layout.
- `makeHeader`: updated signature to `(const Chunk&, const GPUBlobRange&, const Scene&)`; populates `dataRefID` from the chunk's `componentHandle` → `ComponentRecord::dataRefID`.
- `buildDataAndHeaderTextures`: passes `scene` to the updated `makeHeader` call.
- `rebuildSceneTextures(Scene&, GPUData&)` — calls `buildDataAndHeaderTextures` + `syncSceneTables` to rebuild GPU texture content from CPU state after edits, reusing existing samplers. Declared in `gpu_interface.h`.
- Shader `GPUChunkHeader` in `pjv_utils_DDA.sc`: updated to match CPU struct (`uint dataRefID; uint padding[1]`).

### Delivered (Phase 5)
- `GeometryBlob::dirty` (in `scene.h`) — per-blob flag set when a blob is new or changed (set in `forkBlob()` and `internChunkGeometry()`).
- `GPUData::uploadedChunkCount` (in `gpuData.h`) — watermark for new-chunk header detection.
- `flushSceneUpdates(Scene&, GPUData&)` (in `gpu_interface.h/cpp`) — incremental GPU upload:
  - `uploadDirtyBlobs` — iterates dirty blobs, allocates ranges from persistent `RangeAllocator`, uploads only changed texels via `bgfx::updateTexture2D` with row-by-row wrapping.
  - `updateDirtyHeaders` — rewrites only header rows for chunks whose pool blob was just uploaded or that are new.
  - `growDataTextures` / `growHeaderTexture` — full repack fallback when allocators are full (rare, amortized O(1) via `withHeadroom`).
- `rebuildSceneTextures` kept as documented fallback.
- `docs/examples/edit_demo/main.cpp` — switched to `flushSceneUpdates`.
- `docs/examples/editing_p1/main.cpp` — 8 new P5 dirty-flag assertions (all pass).

### Delivered (Phase 6)
- `scene.h`: Added `ComponentKind::Asset`. Added `name`, `parent`, `children`, `localPosition`, `localRotation`, `localScale` to `ComponentRecord`.
- `compose.h`: Added `name` to `ComposeComponent`.
- `compose_io.cpp`: Parse `"name"` from compose.json. Auto-generate names from filenames with sibling disambiguation. Rewrote `loadComposeFromDisk` to create `ComponentRecord`s for every compose.json entry (including Asset folders). Parent/child linking. Direct chunk creation in `scene.chunks` (tree order). Rebuild `looseChunks` list from tree after assembly.
- `include/utils/scene_query.h` + `src/utils/scene_query.cpp`: New module — `getComponentPath`, `findComponentByPath`, `findComponentsByName`, `listComponents`, `getComponentVoxelCount`, `getComponentWorldMatrix`/`Position`/`Rotation`, `setComponentPosition`, `setComponentRotation`, `setComponentTransform` (with subtree rebake).
- `src/utils/editing.cpp`: Skip `ComponentKind::Asset` in `applyComponentQueue`.
- `CMakeLists.txt`: Added `projectV-scene_query` library target.
- `docs/examples/editing_p1/main.cpp`: 21 P6 assertions (names, paths, transforms, world matrix, voxel counts, listComponents, transform mutation + restore). Skip Asset kinds in `findComponents`.
- `docs/examples/edit_demo/main.cpp`: Skip Asset kinds in component search.
- `docs/examples/editing_p1/Makefile`: Link `projectV-scene_query`.
- `AGENTS.md`: This entry.

### Delivered (Logging System)
- `include/core/log.h`: Replaced spdlog re-export with category-based template
  functions (`trace`, `perf`, `edit`, `render`, `info`, `warn`, `error`). Each
  gated by `#if defined(PROJV_ENABLE_*)` — empty body when disabled, eliminated
  by compiler. Tags (`[TRC]`, `[PRF]`, `[EDT]`, `[RND]`, `[INF]`, `[WRN]`,
  `[ERR]`) baked into functions.
- `CMakeLists.txt`: Seven `option()` calls (all ON by default) +
  `PROJV_LOG_MINIMAL` preset (disables everything except WARN+ERROR) +
  `add_compile_definitions` to emit defines project-wide.
- Migrated all existing call sites: manual `[PERF]`/`[EDIT]` tags removed,
  `core::debug` → `core::trace`, `core::critical` → `core::error`, direct
  `spdlog::` calls → `core::` wrapper, `spdlog::set_level()` calls removed.
- `perform_renderer.cpp`: Per-frame render logging gated with `#if` + on-change
  detection (static bool renders once).
- `docs/examples/edit_demo/main.cpp` & `docs/examples/PathTracer/main.cpp`:
  Frame-time perf summary every 100 frames with `#if defined(PROJV_ENABLE_PERF)` guard.
- `CODING_STYLE.md`: Updated logging section with category table and gating
  guidance.
- `AGENTS.md`: This entry.

### What's Next (Phase 7+)
- Removal of legacy `chunkQueue`, mutability policies, persistence.


<!-- headroom:rtk-instructions -->
# RTK (Rust Token Killer) - Token-Optimized Commands

When running shell commands, **always prefix with `rtk`**. This reduces context
usage by 60-90% with zero behavior change. If rtk has no filter for a command,
it passes through unchanged — so it is always safe to use.

## Key Commands
```bash
# Git (59-80% savings)
rtk git status          rtk git diff            rtk git log

# Files & Search (60-75% savings)
rtk ls <path>           rtk read <file>         rtk grep <pattern>
rtk find <pattern>      rtk diff <file>

# Test (90-99% savings) — shows failures only
rtk pytest tests/       rtk cargo test          rtk test <cmd>

# Build & Lint (80-90% savings) — shows errors only
rtk tsc                 rtk lint                rtk cargo build
rtk prettier --check    rtk mypy                rtk ruff check

# Analysis (70-90% savings)
rtk err <cmd>           rtk log <file>          rtk json <file>
rtk summary <cmd>       rtk deps                rtk env

# GitHub (26-87% savings)
rtk gh pr view <n>      rtk gh run list         rtk gh issue list

# Infrastructure (85% savings)
rtk docker ps           rtk kubectl get         rtk docker logs <c>

# Package managers (70-90% savings)
rtk pip list            rtk pnpm install        rtk npm run <script>
```

## Rules
- In command chains, prefix each segment: `rtk git add . && rtk git commit -m "msg"`
- For debugging, use raw command without rtk prefix
- `rtk proxy <cmd>` runs command without filtering but tracks usage
<!-- /headroom:rtk-instructions -->
