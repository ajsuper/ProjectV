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

## Phase 1 + 2 Status (2026-07-10)

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

### What's Next (Phase 5+)
- P5+: Removal of legacy `chunkQueue`, incremental GPU upload, mutability policies, persistence.
Full plan can be found in docs/plans/edit.md


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
