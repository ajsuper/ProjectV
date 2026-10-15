# utils

The voxel layer: compose I/O, the component tree, editing, materials, picking, animation, collision
geometry. Namespace `projv::utils`.

**No EnTT and nothing from the runtime** (the `layering` ctest), so tools without a game loop can use it.

| Belongs in utils? | |
|---|---|
| ✅ `utils::createTree64()`, `utils::convertVoxelPositionToWorldPosition()` | Voxel data and maths |
| ❌ `utils::createApplication()` | `core/` |

Explained in: [The scene](../concepts/scene.md), [Voxel data](../concepts/voxel-data.md),
[Materials](../concepts/materials.md), [Editing](../concepts/editing.md).

## `utils/compose_io.h`

[`include/utils/compose_io.h`](../../include/utils/compose_io.h): reading and writing compose
folders and `.data` files.

| Name | What |
|---|---|
| `loadComposeFromDisk(folder)` | A compose folder → `Scene` |
| `instantiateComposeInto(scene, folder, parent, pos, rot, scale)` | Graft a folder under a new Asset node |
| `instantiateSceneInto(scene, loaded, name, parent, ..., sharedBlobs)` | Graft an already loaded `Scene`, optionally sharing blobs |
| `saveComposeToDisk(scene, root, folder)` | A subtree (or `INVALID_COMPONENT_HANDLE` for all) → `compose.json` + `.data`. Check the result. |
| `parseComposeJson`, `writeComposeJson` | `compose.json` ↔ `ComposeDoc` |
| `readDataFile`, `writeDataFile` | `.data` ↔ `DataFile` |
| `readDataFileHeader`, `readDataBlock` | Block table only, then one block: for streaming |
| `dataFileFromComponent(scene, handle)` | A Chunk/Grid component's live voxels as a `DataFile` |

## `utils/scene_query.h`

[`include/utils/scene_query.h`](../../include/utils/scene_query.h): the component tree.

| Name | What |
|---|---|
| `findComponentByPath`, `findComponentsByName`, `findComponentByLocalId`, `getComponentPath` | Lookup |
| `listComponents`, `rootComponents`, `getComponentVoxelCount` | Enumeration |
| `getComponentWorldMatrix` / `Position` / `Rotation` | World transform |
| `setComponentPosition` / `Rotation` / `Scale` / `Transform` | Local transform; rebakes the subtree |
| `addComponent(scene, kind, name, parent, resolution, voxelScale)`, `isValidChunkResolution` | Create (resolution: power of four) |
| `duplicateComponent`, `setComponentParent`, `deleteComponent` | Copy, reparent, delete a subtree |
| `refOf`, `resolve`, `isComponentAlive` | Holding components across frames |
| `reserveComponentSlot`, `placeComponent`, `allocateChunkSlot` | Rows for code that builds records by hand |
| `nextLocalId`, `ensureUniqueLocalId` | Persistent ids |
| `chunkHasGeometry` | Alive, pooled, non-empty |

## `utils/editing.h`

[`include/utils/editing.h`](../../include/utils/editing.h): per-component edit queues.

| Name | What |
|---|---|
| `queueVoxelAdd(scene, comp, ops)`, `queueVoxelRemove(scene, comp, ops)` | Queue edits in component voxel space |
| `updateScene(scene)` | Apply every queue on the CPU; then `graphics::flushSceneUpdates` |
| `convertChunkToGrid(scene, comp)` | A chunk component → a one-cell grid (done for you on overflow) |
| `expandGridToInclude(grid, cell, scene, gridIndex)` | Grow a grid, either direction |

## `utils/material.h`

[`include/utils/material.h`](../../include/utils/material.h): palettes.

| Name | What |
|---|---|
| `internMaterial(scene, comp, name, color, physics)` | The slot for a colour, adding it if new (thread-safe) |
| `findMaterialByName`, `findMaterialByColor` | Lookup |
| `addMaterial`, `setMaterialColor`, `setMaterialProperties`, `setMaterialName`, `setMaterialPhysics` | Edit a palette |
| `removeMaterial(scene, comp, slot, replacement)` | Remove a slot and renumber voxels |
| `countMaterialUsage`, `findMaterialChunks` | Where a slot is used |
| `bakeMaterialsFromBrickMap`, `brickMapSetMaterial`, `brickMapGetMaterial` | Material bytes ↔ brick map |

## `utils/attachments.h`

[`include/utils/attachments.h`](../../include/utils/attachments.h): tool data saved with
components, never interpreted by the engine.

| Name | What |
|---|---|
| `AttachmentTraits<T>` | Specialise: `key`, `version`, `onDuplicate`, `save`, `load` |
| `getAttachment<T>`, `setAttachment<T>`, `removeAttachment<T>` | Typed access (`AttachmentScope::Component` or `Document`) |
| `hasAttachment`, `removeAttachment(key)`, `attachRaw`, `attachmentsForSave` | By key / as JSON text |
| `clearAttachments`, `duplicateAttachments`, `appendAttachments` | Lifecycle |

## `utils/picking.h`

[`include/utils/picking.h`](../../include/utils/picking.h): CPU ray casts against the voxels.

| Name | What |
|---|---|
| `pickVoxel(scene, origin, dir, maxDistance, override)` | Nearest drawn voxel → `VoxelPick` (chunk, component, voxel, face normal, slot) |
| `rayDirectionThroughImage(uv, resolution, cameraDir, fov)` | The ray the shader shot through that pixel |
| `queryVoxelMaterial(blob, resolution, coord, slotOut)` | One voxel's slot, or false if empty |
| `VoxelSolidityOverride` | Make a drag keep hitting the surface it started on |

## `utils/voxel_management.h`

[`include/utils/voxel_management.h`](../../include/utils/voxel_management.h): brick maps and tree64.

| Name | What |
|---|---|
| `createVoxelBrickMap(dims)`, `brickMapSetVoxel`, `brickMapClearVoxel`, `brickMapHasVoxel`, `cloneBrickMap` | The editable form |
| `updateChunkFromBrickMap(chunk, map)`, `buildTree64FromBrickMap(map, res)` | Brick map → tree64 |
| `brickMapFromTree64(geometry, materialIDs, res)` | tree64 → brick map |
| `downsampleTree64(...)` | Drop levels (storage LOD) |
| `createChunk(header)`, `createChunkScaleFromVoxelScaleAndResolution`, `createVoxelGrid`, `buildVoxelGridFromBrickMap` | Older helpers |

## `utils/voxel_math.h`

[`include/utils/voxel_math.h`](../../include/utils/voxel_math.h)

| Name | What |
|---|---|
| `createZOrderIndex`, `reverseZOrderIndex` | Morton order ↔ coordinates |
| `floorDiv`, `floorMod` | Rounding toward −∞ (cell of a negative voxel) |
| `computeBrickDims(res)`, `computeBrickCoord`, `computeBrickLocalPos`, `computeBrickZOrder`, `computeLocalZOrder` | Brick addressing |
| `convertVec3ToHeaderPosition`, `convertHeaderPositionToVec3` | Packed positions |

## `utils/animation.h`

[`include/utils/animation.h`](../../include/utils/animation.h): animated voxels by envelope.

| Name | What |
|---|---|
| `MotionKind` | `None`, `Sway` (displacement: grass, leaves), `Advect` (fire, smoke) |
| `MotionSet` | One row of the 16-row table: amplitude, frequency, speed, direction, turbulence, ... |
| `AnimationState` | `time` + `sets[16]`; lives on `GPUData::animation` |
| `bakeAnimationEnvelope(scene, anim)` | Build every animated blob's envelope → `EnvelopeBakeReport` |
| `envelopeResolutionFor(res)` | Envelope resolution (a quarter) |

## `utils/collision_geometry.h`

[`include/utils/collision_geometry.h`](../../include/utils/collision_geometry.h): what voxels
collide as; nothing else decides it.

| Name | What |
|---|---|
| `buildCollisionPieces(blob, res, params, materials)` | Voxels → boxes (`CollisionPieces`), never bigger than the voxels |
| `CollisionParams` | Representation, piece budget, decode limit |
| `SlotMaterials` | Palette slot → physics material index |
| `decodeOccupancy`, `decodeVoxelSlots` | A blob as a dense box of cells |

## `utils/loose_bvh.h`

[`include/utils/loose_bvh.h`](../../include/utils/loose_bvh.h): BVH over loose chunks, built on
every flush. `PROJV_LOOSE_BVH=0` turns it off.

| Name | What |
|---|---|
| `buildLooseBVH(scene, chunks, maxLeaf)` | Build |
| `traverseLooseBVH`, `overlapLooseBVH` | Ray and box queries |
| `blobContentBounds`, `looseChunkWorldBounds` | Tight bounds (cached per blob) |
| `rayBoxInterval`, `looseBVHEnabled` | Helpers |

## `utils/voxel_islands.h`

[`include/utils/voxel_islands.h`](../../include/utils/voxel_islands.h)

| Name | What |
|---|---|
| `splitVoxelIslands(scene, comp, piecesAsRoots)` | Split a loose chunk into face-connected pieces → `IslandSplit` |
