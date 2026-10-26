# Editing

Changing voxels at runtime: queue the change, apply it on the CPU, push it to the GPU, and save it
if you want to keep it. Headers: [`utils/editing.h`](../../include/utils/editing.h),
[`graphics/gpu_interface.h`](../../include/graphics/gpu_interface.h),
[`utils/compose_io.h`](../../include/utils/compose_io.h).

## The pipeline

```text
 queueVoxelAdd / queueVoxelRemove      per component, in component voxel space
            │   ComponentRecord::editQueue
            ▼
 utils::updateScene(scene)             CPU: fork shared blobs, edit the brick map,
            │                          rebake tree64 + materialIDs, grow grids, mark blobs dirty
            ▼
 graphics::flushSceneUpdates(scene, gpu)   GPU: upload dirty blobs, rewrite changed header rows
            │
            ▼
 utils::saveComposeToDisk(scene, root, folder)   optional: write compose.json + .data
```

```cpp
using namespace projv;

std::vector<PendingVoxelOp> ops;
for (int x = 0; x < 8; x++)
    ops.push_back({.isAdd = true, .position = {x, 0, 0}, .packedColor = packColor({200, 40, 40})});

utils::queueVoxelAdd(scene, component, ops);
utils::updateScene(scene);
graphics::flushSceneUpdates(scene, gpuData);
```

## What a `PendingVoxelOp` says

| Field | Meaning |
|---|---|
| `isAdd` | Overwritten by `queueVoxelAdd` (true) and `queueVoxelRemove` (false) |
| `position` | Integer voxel coordinates **in the component's own space**: not world, not Z-order. May be negative or past the edge. |
| `packedColor` | Colour to add, interned into the component's palette |
| `slot` | A palette slot to use instead of `packedColor`; `INVALID_MATERIAL` (default) means intern the colour |

## What `updateScene` does

- **Copy-on-write.** Shared blobs are forked first; a sole owner is edited in place.
- **Brick maps on demand**, built from the tree64 on the first edit; tree64 and material bytes are
  rebaked after.
- **Growth.** An edit past a chunk component's edge turns it into a one-cell grid
  (`convertChunkToGrid`) and adds cells as needed (`expandGridToInclude`), in any direction.
- **Content changes are announced.** Rebaked blobs get a new `contentStamp`, so physics rebuilds
  shapes and splits what was cut loose.

## Getting it to the GPU

| Call | Use when | Cost |
|---|---|---|
| `createTexturesForScene(scene)` | First upload | Everything |
| `flushSceneUpdates(scene, gpu)` | After `updateScene`, after a delete, after palette size changes | Dirty blobs + dirty header rows + small tables |
| `updateChunkHeader(scene, gpu, chunk)` | Only a chunk's placement changed | One header row |
| `updatePaletteEntry(scene, gpu, comp, slot)` | `setMaterialColor` / `setMaterialProperties` during a drag | One texel |
| `rebuildSceneTextures(scene, gpu)` | Full repack | Everything, keeps samplers |
| `destroyGPUData(gpu)` | Before reloading a different scene | Frees all of it |

## Moving and restructuring

Transform and tree edits apply immediately on the CPU; flush to see them:

```cpp
utils::setComponentTransform(scene, handle, position, rotation, scale);
utils::duplicateComponent(scene, handle);        // shares geometry until edited
utils::deleteComponent(scene, handle);           // subtree; rows are reused later
graphics::flushSceneUpdates(scene, gpuData);
```

Move a linked component through its entity's `Transform` instead ([Entities](entities.md)).

## Picking: which voxel is under the cursor

```cpp
core::vec3 dir = utils::rayDirectionThroughImage(uv, resolution, cameraDirection, 60.0f);
utils::VoxelPick pick = utils::pickVoxel(scene, cameraPosition, dir);
if (pick.hit) {
    // pick.component, pick.chunk, pick.voxelCoord, pick.faceNormal, pick.materialSlot
    // add on the face:    voxelCoord + faceNormal
    // remove the voxel:   voxelCoord
}
```

- **Only what is drawn is hit**: the loose list and grid cells, not every live chunk.
- **Nearest wins**, even when chunks overlap.
- **`VoxelSolidityOverride`** keeps a drag hitting the surface it started on.

`voxelCoord` is in the **chunk's** space (the component's, for a loose chunk). For a grid cell the
component-space position is
`(grid.originCellCoord + the cell's coordinate in the grid) * resolution + voxelCoord`.

## Physics bodies

Edit physics-owned bodies through `runtime::editVoxels(world, entity, ops)`: it applies at the next
fixed step in command order, so peers edit on the same tick. Direct `utils` edits also work; physics
sees the new content stamp. See [Physics](physics.md#editing-bodies).

`utils::splitVoxelIslands(scene, component)` cuts a loose chunk component into its face-connected
pieces: the original keeps the largest
([`utils/voxel_islands.h`](../../include/utils/voxel_islands.h)).

## Saving

```cpp
utils::saveComposeToDisk(scene, INVALID_COMPONENT_HANDLE, "out/MyScene");   // whole scene
utils::saveComposeToDisk(scene, assetHandle, "out/House");                  // one subtree
```

- Writes `compose.json` plus one `<name>.data` per chunk or grid component, and a subfolder per
  asset. Chunks sharing a blob share one `.data`.
- **A linked asset** (`ComponentRecord::externalSource`) is written as a reference, not copied.
- **Check the return value**: false means a file did not finish writing.

!!! note "`mutability` today"
    `mutability` decides **sharing** at load (different policies never share a blob) and is saved
    back. Per-policy write-back (in place for `direct`, new file for `copy`) is not implemented:
    `saveComposeToDisk` writes current voxels into the given folder. See
    [plans/edit_p8.md](../plans/edit_p8.md).

Filling a new component in one go: [Build voxels in code](../guides/voxels-in-code.md).
