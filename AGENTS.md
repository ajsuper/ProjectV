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

## Phase 1 Status (2026-07-10)

### Delivered
- `scene.h`: Added `PendingVoxelOp`, `ComponentEditQueue`, `DataReference`, `ComponentRecord::editQueue`/`dataRefID`, `Scene::dataReferences`, `forkBlob()`.
- `voxel_math.h/cpp`: Added `floorDiv`/`floorMod` for correct negative-coordinate cell bucketing.
- `include/utils/editing.h` + `src/utils/editing.cpp`: `queueVoxelAdd`, `queueVoxelRemove`, `updateScene` (loose chunks only, always-COW, CPU-only).
- `CMakeLists.txt`: `projectV-editing` static library target.
- `docs/examples/editing_p1/{main.cpp,Makefile}`: Test driver.
- `AGENTS.md`: This file.

### What's Next (Phase 2+)
- P2: Grid expansion (both directions via `expandGridToInclude`), cell bucketing with `floorDiv`/`floorMod`.
- P3: Loose chunk → 1-cell grid conversion on overflow.
- P4: `rebuildSceneTextures` from existing `createTexturesForScene` logic.
- P5+: Removal of legacy `chunkQueue`, incremental GPU upload, mutability policies, persistence.
Full plan can be found in docs/plans/edit.md
