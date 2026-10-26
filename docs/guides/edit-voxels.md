# Edit voxels

A brush: pick, add/remove, recolour, undo. Concepts:
[Editing](../concepts/editing.md). Headers: [`utils/picking.h`](../../include/utils/picking.h),
[`utils/editing.h`](../../include/utils/editing.h),
[`utils/material.h`](../../include/utils/material.h).

```text
 cursor ──rayDirectionThroughImage──▶ ray ──pickVoxel──▶ VoxelPick ──▶ ops ──queueVoxelAdd/Remove──▶
        updateScene ──▶ flushSceneUpdates
```

## 1. Pick

```cpp
using namespace projv;

core::vec2 uv = input.cursor / core::vec2(window.getWindowResolution());      // top-left origin
core::vec3 dir = utils::rayDirectionThroughImage(uv, core::vec2(window.getWindowResolution()),
                                                 cameraDirection, 60.0f);    // must match the shader's FOV
utils::VoxelPick pick = utils::pickVoxel(scene, cameraPosition, dir);
if (!pick.hit) return;
```

| Field | Meaning |
|---|---|
| `component`, `chunk` | Owner and chunk |
| `voxelCoord` | In **the chunk's** voxel space |
| `faceNormal` | Entry face, one axis ±1. `voxelCoord + faceNormal` is the empty cell in front. |
| `worldPosition`, `distance` | |
| `materialSlot` | Palette slot |

## 2. Turn the pick into component coordinates

Ops are in the **component's** space; for a grid cell, add the cell's offset:

```cpp
core::ivec3 toComponent(const Scene& scene, const utils::VoxelPick& pick) {
    const Chunk& chunk = scene.chunks[pick.chunk];
    if (chunk.gridIndex < 0) return pick.voxelCoord;
    const SceneGrid& grid = scene.grids[chunk.gridIndex];
    int lin = chunk.cellIndex;
    core::ivec3 cell{lin % grid.dims.x, (lin / grid.dims.x) % grid.dims.y, lin / (grid.dims.x * grid.dims.y)};
    return (grid.originCellCoord + cell) * int(chunk.header.resolution) + pick.voxelCoord;
}
```

## 3. Add or remove a sphere

```cpp
std::vector<PendingVoxelOp> sphere(core::ivec3 centre, int radius, uint32_t colour) {
    std::vector<PendingVoxelOp> ops;
    for (int z = -radius; z <= radius; z++)
        for (int y = -radius; y <= radius; y++)
            for (int x = -radius; x <= radius; x++)
                if (x * x + y * y + z * z <= radius * radius)
                    ops.push_back({true, centre + core::ivec3(x, y, z), colour});
    return ops;
}

core::ivec3 at = toComponent(scene, pick);
if (adding) utils::queueVoxelAdd(scene, pick.component, sphere(at + pick.faceNormal, 3, colour));
else        utils::queueVoxelRemove(scene, pick.component, sphere(at, 3, 0));
utils::updateScene(scene);
graphics::flushSceneUpdates(scene, gpuData);
```

- **One `updateScene` per frame**, not per op.
- **Removing nothing changes nothing**: no grid growth, no empty cells.
- **Shared blobs are forked** before they are edited.

### Keep the stroke on its starting surface

Otherwise a held stroke climbs its own deposits or sinks into its own holes:

```cpp
std::set<std::tuple<ChunkHandle, int, int, int>> added, removed;   // filled as the stroke goes

auto override = [&](ChunkHandle c, core::ivec3 v, bool solid) {
    if (added.count({c, v.x, v.y, v.z}))   return false;   // see through what this stroke added
    if (removed.count({c, v.x, v.y, v.z})) return true;    // still stop where it removed
    return solid;
};
utils::VoxelPick pick = utils::pickVoxel(scene, origin, dir, 1.0e6f, override);
```

A voxel forced solid reports `materialSlot` 0: don't sample colours with an override like this.

## 4. Recolour without touching geometry

Recolouring a material is a palette write:

```cpp
utils::setMaterialColor(scene, pick.component, pick.materialSlot, packRGB10(0.9f, 0.2f, 0.2f));
graphics::updatePaletteEntry(scene, gpuData, pick.component, pick.materialSlot);   // one texel
```

To paint individual voxels, re-add them with a new colour or `op.slot`; an add over a voxel
replaces its material.

## 5. Undo

The scene editor records each edit as a pair of closures
([`examples/60-scene-editor/edit_history.h`](../../examples/60-scene-editor/edit_history.h)):

```cpp
struct EditRecord {
    std::string label;
    std::function<void()> undo, redo;
    size_t memoryCost;          // the log evicts the oldest past a budget
    std::string coalesceKey;    // a colour drag merges into one step
};
```

For a stroke, record each touched voxel's prior state while queueing; undo queues the inverse
ops. The editor turns slot recycling off (`scene.slots.enabled = false`) because its history holds
bare handles. New code should hold `ComponentRef`s and `utils::resolve` them.

## Bodies the physics owns

Edit components with a `RigidBody` or `StaticCollider` through the runtime, so the edit lands on a
fixed step, ordered with other commands:

```cpp
auto crater = runtime::voxelsInSphere(world, entity, hitPoint, 2.0f);   // removal ops, ready to use
runtime::editVoxels(world, entity, crater);                              // applied at the next step
```

Pieces cut loose become bodies (`BodyBroken`, `cause: Edit` or `Support`). See
[Physics](../concepts/physics.md#editing-bodies) and [`16-sandbox`](../../examples/16-sandbox/)'s bombs.
