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

### What's Next (Phase 3+)
- P3: Loose chunk → 1-cell grid conversion on overflow.
- P4: `rebuildSceneTextures` from existing `createTexturesForScene` logic.
- P5+: Removal of legacy `chunkQueue`, incremental GPU upload, mutability policies, persistence.
Full plan can be found in docs/plans/edit.md
