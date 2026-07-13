# Phase 9 — Tree64 Primitives + Transactions + Factory + Cleanup

## Goal

Add the remaining high-value features from the cross-scenario analysis:
direct tree64 voxel manipulation (O(log n) instead of O(n) decompress),
edit groups for transactional editing, factory functions for procedural
content creation, and removal of all legacy dead code.

This is the final polishing phase. Everything above this is in place:
hierarchical components (P6), policy-driven editing (P7), persistence (P8).

## What exists (before P9)

- Editing pipeline: `queueVoxelAdd/Remove` → `updateScene` → `flushSceneUpdates`.
- Every edit decompresses the full chunk into a `VoxelBatch`, applies ops,
  then rebuilds the entire tree64. This is O(n) per edit regardless of how
  few voxels changed.
- No transaction/group boundaries — edits are applied immediately on drain.
- No bulk creation API — procedural code must manually create chunks, intern
  geometry, push components, and register loose chunks (7+ steps).
- Legacy `chunkQueue` on `Chunk` (unused since P1).
- Legacy `looseChunkCount` on `Scene` (redundant with `looseChunks.size()`).
- `Chunk::mutability` removed in P7, but the field may still exist in some
  comments or serialization paths.

## Sub-phases

### P9.1 — Direct tree64 set/remove primitives (voxel_management.h, voxel_management.cpp)

**Files:** `include/utils/voxel_management.h`, `src/utils/voxel_management.cpp`

**P9.1a — Low-level tree64 patch**

Add functions that modify a single voxel in an already-decompressed tree64
buffer WITHOUT going through the full VoxelBatch pipeline:

```cpp
namespace projv::utils {

/**
 * Set a single voxel in a chunk's geometry blobs. The chunk must be
 * the sole owner of its blob (refCount == 1). This is a direct tree64
 * patch — it does NOT decompress the full chunk into a VoxelBatch.
 *
 * Use case: simulation loops that modify one voxel at a time.
 * For bulk edits, queueVoxelAdd/Remove is still preferred.
 *
 * @param scene  The owning scene.
 * @param chunk  The chunk to modify (must be sole owner of its blob).
 * @param localPos  Local voxel position in the chunk.
 * @param color  New color. Set to Color{0,0,0} to remove.
 * @return true if the voxel was set; false if the chunk is shared or
 *         the position is out of bounds.
 */
bool setVoxelDirect(Scene& scene, Chunk& chunk,
                    const core::ivec3& localPos, const Color& color);

/**
 * Remove a single voxel from a chunk. Equivalent to
 * setVoxelDirect(scene, chunk, localPos, Color{0,0,0}).
 */
bool removeVoxelDirect(Scene& scene, Chunk& chunk,
                       const core::ivec3& localPos);

/**
 * Query whether a voxel exists at the given position (without decompressing
 * the full chunk). Returns true if the voxel is present, and optionally
 * fills in `color`.
 *
 * @param scene  The scene.
 * @param chunk  The chunk to query.
 * @param localPos  Local voxel position.
 * @param color  [out] The voxel color if found (may be nullptr).
 * @return true if a voxel exists at the position.
 */
bool hasVoxelDirect(const Scene& scene, const Chunk& chunk,
                    const core::ivec3& localPos, Color* color = nullptr);

}
```

**P9.1b — Implementation strategy**

The tree64 is a 3D Morton-order (Z-order) tree stored as a `uint32[]` with
3 uints per node (RGB). Direct patching requires:

1. Compute the Z-order index of `localPos`.
2. Traverse the tree64 to find the leaf node at that index.
3. Patch the leaf's color data.
4. Update any parent nodes if needed (color aggregation).

This is O(log(resolution)) per voxel, compared to O(resolution^3) for full
decompression.

The implementation details depend on the tree64 encoding. Review the
existing tree64 traversal code in `voxel_management.cpp` (functions like
`updateChunkFromItsVoxelBatch`, `getChunkVoxelBatch`) to understand the
node layout, then write a targeted traversal + patch.

**P9.1c — Integration with the editing pipeline**

The direct primitives are a separate path — they do NOT use the queue/drain
pipeline. Callers use them directly:

```cpp
// Simulation: modify one voxel per frame
for (int i = 0; i < 100; ++i) {
    setVoxelDirect(scene, chunk, somePos, someColor);
}
// Then flush to GPU once
chunk.blob.dirty = true;
flushSceneUpdates(scene, gpuData);
```

This is much faster than the queue path for single-voxel edits but bypasses
the policy system (Locked/Direct/Copy checks). Callers must check policy
themselves.

### P9.2 — Edit groups / transactions (editing.h, editing.cpp)

**Files:** `include/utils/editing.h`, `src/utils/editing.cpp`

**P9.2a — Add transaction state to EditingContext**

```cpp
struct EditGroup {
    std::string name;                          // human-readable label
    std::vector<ComponentHandle> components;   // components touched in this group
    // TODO: snapshots for undo (P9.2c)
};

struct EditingContext {
    Scene& scene;
    EditingPolicy policy;

    // P9: Edit group stack
    std::vector<EditGroup> editGroups;
    bool inTransaction = false;
};
```

**P9.2b — Transaction API**

```cpp
namespace projv::utils {

/**
 * Begin a transaction. All subsequent edits are queued but NOT applied
 * until endEditGroup() is called. Nesting is supported (stack).
 * @param ctx  The editing context.
 * @param name  Optional label for the group.
 */
void beginEditGroup(EditingContext& ctx, const std::string& name = "");

/**
 * End the current transaction and apply all queued edits. Flushes the
 * component queues and returns a SceneDiff for the group.
 * @param ctx  The editing context.
 * @return SceneDiff for all edits in this group.
 */
SceneDiff endEditGroup(EditingContext& ctx);

/**
 * Discard the current transaction. Clears edit queues for all components
 * touched in this group. No changes are applied.
 * @param ctx  The editing context.
 */
void discardEditGroup(EditingContext& ctx);

}
```

**P9.2c — Implementation**

`beginEditGroup`: push a new `EditGroup` onto the stack, set
`inTransaction = true`.

`endEditGroup`: call `updateComponent` for each component in the current
group's list, pop the group, return the merged `SceneDiff`.

`discardEditGroup`: iterate all components in the group, clear their
`editQueue.ops`, pop the group.

The key change to `queueVoxelAdd/Remove`: when `inTransaction` is true,
just append to the queue (existing behavior) but also record the component
handle in the current group's `components` list:

```cpp
bool queueVoxelAdd(EditingContext& ctx, ComponentHandle h,
                   const std::vector<PendingVoxelOp>& voxels) {
    if (h >= ctx.scene.components.size()) return false;
    auto& q = ctx.scene.components[h].editQueue.ops;
    q.insert(q.end(), voxels.begin(), voxels.end());

    // Record component in the current transaction group
    if (ctx.inTransaction && !ctx.editGroups.empty()) {
        auto& group = ctx.editGroups.back();
        if (std::find(group.components.begin(), group.components.end(), h)
            == group.components.end()) {
            group.components.push_back(h);
        }
    }
    return true;
}
```

**P9.2d — Coalescence**

Within a transaction, multiple `queueVoxelAdd` calls to the same position
should coalesce (last write wins). Modify `PendingVoxelOp` handling to
replace existing ops at the same position:

```cpp
// In queueVoxelAdd, before appending:
for (auto& op : voxels) {
    // Remove any existing op at the same position
    auto it = std::find_if(q.begin(), q.end(), [&](const PendingVoxelOp& existing) {
        return existing.position == op.position;
    });
    if (it != q.end()) {
        *it = op;  // replace in-place
    } else {
        q.push_back(op);
    }
}
```

This is a significant behavior change — currently, duplicate positions are
appended. The coalescence should be gated on `inTransaction` to avoid
breaking existing code.

### P9.3 — Factory functions (voxel_management.h, voxel_management.cpp)

**Files:** `include/utils/voxel_management.h`, `src/utils/voxel_management.cpp`

**P9.3a — Bulk creation API**

```cpp
namespace projv::utils {

/**
 * Create a new loose chunk in the scene from a header and voxel batch.
 * Interns geometry, creates a ComponentRecord, registers as loose.
 * One-stop-shop for procedural content creation.
 *
 * @param scene     The scene to add to.
 * @param header    Chunk header (position, rotation, scale, resolution, etc.)
 * @param voxels    Voxel data to fill the chunk with.
 * @param name      Component name (auto-generated if empty).
 * @param sourcePath  Provenance path (for debugging).
 * @return ComponentHandle of the new component.
 */
ComponentHandle createChunkInScene(Scene& scene,
                                   const ChunkHeader& header,
                                   const VoxelBatch& voxels,
                                   const std::string& name = "",
                                   const std::string& sourcePath = "generated");

/**
 * Create a new Grid component in the scene from a grid descriptor and
 * per-cell voxel batches. Creates all necessary chunks and component records.
 *
 * @param scene     The scene to add to.
 * @param gridDesc  Grid descriptor (origin, cellSize, dims, rotation).
 * @param cellData  Per-cell voxel data (must match gridDesc.dims size).
 * @param name      Component name.
 * @return ComponentHandle of the new grid component.
 */
ComponentHandle createGridInScene(Scene& scene,
                                  const SceneGridDescriptor& gridDesc,
                                  const std::vector<VoxelBatch>& cellData,
                                  const std::string& name = "");

}
```

**P9.3b — Implementation of `createChunkInScene`**

```cpp
ComponentHandle createChunkInScene(Scene& scene,
                                   const ChunkHeader& header,
                                   const VoxelBatch& voxels,
                                   const std::string& name,
                                   const std::string& sourcePath) {
    // 1. Create chunk
    Chunk chunk = createChunk(header);
    moveVoxelBatchToChunk(voxels, chunk);
    updateChunkFromItsVoxelBatch(chunk, true);

    // 2. Intern geometry
    internChunkGeometry(scene, chunk);

    // 3. Register in scene.chunks
    ChunkHandle chunkHandle = static_cast<ChunkHandle>(scene.chunks.size());
    scene.chunks.push_back(std::move(chunk));

    // 4. Create component record
    ComponentHandle compHandle = static_cast<ComponentHandle>(scene.components.size());
    ComponentRecord rec;
    rec.kind = ComponentKind::Chunk;
    rec.chunkHandle = chunkHandle;
    rec.name = name.empty() ? "chunk_" + std::to_string(chunkHandle) : name;
    rec.sourcePath = sourcePath;
    scene.components.push_back(std::move(rec));

    // 5. Point chunk at component
    scene.chunks[chunkHandle].componentHandle = compHandle;

    // 6. Register as loose
    scene.looseChunks.push_back(chunkHandle);
    scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());

    return compHandle;
}
```

### P9.4 — Legacy cleanup

**Files:** `include/data_structures/scene.h`, `src/utils/editing.cpp`,
`src/utils/voxel_management.h`, `src/utils/compose_io.cpp`

**P9.4a — Remove `Chunk::chunkQueue`**

The `VoxelBatch chunkQueue` field on `Chunk` was the legacy edit staging
area. It has been unused since Phase 1 (the per-component `editQueue` replaced
it). Delete the field and all references:

```cpp
// In scene.h, remove from Chunk struct:
//     VoxelBatch chunkQueue;   // DELETE
```

Search for all references to `chunkQueue` in the codebase and remove them.

**P9.4b — Remove `Scene::looseChunkCount`**

The `looseChunkCount` field is redundant — `looseChunks.size()` gives the
same value. Replace all uses with `looseChunks.size()` and delete the field:

```cpp
// In scene.h, remove from Scene struct:
//     uint32_t looseChunkCount = 0;   // DELETE

// Replace all uses:
//   scene.looseChunkCount  →  static_cast<uint32_t>(scene.looseChunks.size())
```

Files to update: `compose_io.cpp` (loader assembly), `gpu_interface.cpp`
(`syncSceneTables` — `gpuData.looseCount` should still be set from
`looseChunks.size()`), `editing_p1/main.cpp` (test assertions).

**P9.4c — Remove dead Mutability references**

In P7, `Mutability` was removed from `scene.h` and `Chunk::mutability` was
deleted. But some comments, documentation, or dead code paths may still
reference it. Scan for:

- `mutability` (outside of `EditingPolicy` and `ComposeComponent`)
- `Mutability::Locked`, `Mutability::Direct`, `Mutability::Copy`

Update comments and remove any remaining dead code.

### P9.5 — Update all examples and tests

**Files:** `docs/examples/editing_p1/main.cpp`, `docs/examples/edit_demo/main.cpp`

**P9.5a — editing_p1: Add tests for P9 features**

```cpp
// --- Direct tree64 test ---
{
    Scene scene = makeTestScene();  // single 64^3 chunk with 5 seed voxels
    Chunk& chunk = scene.chunks[0];
    // Ensure sole ownership
    int32_t poolIdx = forkBlob(scene, chunk.geometryPoolIndex);
    chunk.geometryPoolIndex = poolIdx;

    // Set a voxel directly
    bool ok = setVoxelDirect(scene, chunk, {10, 10, 10}, Color{255, 0, 0});
    check(ok, "direct setVoxel succeeded");
    check(scene.geometryPool[poolIdx].dirty, "direct set marked blob dirty");

    // Query it back
    Color c;
    bool found = hasVoxelDirect(scene, chunk, {10, 10, 10}, &c);
    check(found, "direct hasVoxel found the voxel");
    check(c.r == 255 && c.g == 0 && c.b == 0, "direct voxel color matches");
}

// --- Transaction test ---
{
    Scene scene = makeTestScene();
    EditingContext ctx{scene, EditingPolicy{}};

    beginEditGroup(ctx, "test_group");
    queueVoxelAdd(ctx, 0, {{true, {5,5,5}, Color{255,0,0}}});
    queueVoxelAdd(ctx, 0, {{true, {10,10,10}, Color{0,255,0}}});
    check(ctx.scene.components[0].editQueue.ops.size() == 2,
          "ops queued during transaction");

    // Discard
    discardEditGroup(ctx);
    check(ctx.scene.components[0].editQueue.ops.empty(),
          "ops cleared after discard");
}

// --- Factory function test ---
{
    Scene scene;
    ChunkHeader hdr;
    hdr.resolution = 64;
    hdr.scale = 32.0f;
    // ... fill header ...

    VoxelBatch batch;
    batch.push_back(createVoxel(Color{255,0,0}, {0,0,0}));

    ComponentHandle h = createChunkInScene(scene, hdr, batch, "test_chunk");
    check(h != INVALID_COMPONENT_HANDLE, "factory created component");
    check(scene.components[h].name == "test_chunk", "factory set name");
    check(scene.chunks.size() == 1, "factory created chunk");
    check(scene.looseChunks.size() == 1, "factory registered loose");
}
```

**P9.5b — edit_demo: Use factory functions**

Replace the preview sphere creation code (which manually creates chunks,
interns geometry, pushes to scene, and registers loose) with a call to
`createChunkInScene`. This is a simplification and proof of the factory API.

## File change summary

| File | Change |
|------|--------|
| `include/utils/voxel_management.h` | Declare `setVoxelDirect`, `removeVoxelDirect`, `hasVoxelDirect`, `createChunkInScene`, `createGridInScene`. |
| `src/utils/voxel_management.cpp` | Implement tree64 traversal + patch. Implement factory functions. |
| `include/utils/editing.h` | Add `EditGroup`, `beginEditGroup`, `endEditGroup`, `discardEditGroup` to `EditingContext`. |
| `src/utils/editing.cpp` | Implement transaction stack. Add coalescence in queue functions. |
| `include/data_structures/scene.h` | Remove `Chunk::chunkQueue`. Remove `Scene::looseChunkCount`. Remove dead Mutability comments. |
| `src/utils/compose_io.cpp` | Update all `looseChunkCount` → `looseChunks.size()`. |
| `src/graphics/gpu_interface.cpp` | Update `gpuData.looseCount` assignment. |
| `docs/examples/editing_p1/main.cpp` | Add P9 tests (direct tree64, transactions, factory). |
| `docs/examples/edit_demo/main.cpp` | Use factory functions for preview sphere. |
| `AGENTS.md` | Update Phase 9 status. Mark Phase 6-9 as delivered. |

## Definition of done

- `setVoxelDirect` modifies a single voxel in O(log(resolution)) without
  decompressing the full chunk.
- `hasVoxelDirect` queries a single voxel without full decompression.
- `beginEditGroup` / `endEditGroup` / `discardEditGroup` work correctly:
  - Ops queued between begin/end are not applied until end.
  - `discardEditGroup` clears all queued ops without applying them.
  - Nested groups are supported (stack).
- `createChunkInScene` creates a chunk, interns geometry, registers
  component, and registers loose handle in one call.
- `Chunk::chunkQueue` is removed from the codebase.
- `Scene::looseChunkCount` is removed; all uses use `looseChunks.size()`.
- `edit_demo` uses `createChunkInScene` for the preview sphere.
- All P1-P8 assertions still pass.