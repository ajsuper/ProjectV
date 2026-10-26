# utils

The voxel layer: compose I/O, the component tree, editing, materials, picking, animation, tile tables,
loading part of a scene, collision geometry. Namespace `projv::utils`.

**No EnTT and nothing from the runtime** (the `layering` ctest), so tools without a game loop can use it.

| Belongs in utils? | |
|---|---|
| ✅ `utils::buildTileTable()`, `utils::createZOrderIndex()` | Voxel data and maths |
| ❌ `utils::executableDirectory()` | `core/` |

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
| `readDataFileHeader`, `readDataBlock` | Block table only, then one block: for streaming. A v4 block is checked (`tileTableWellFormed`) as it is read. |
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

## `utils/tile_table.h`

[`include/utils/tile_table.h`](../../include/utils/tile_table.h) (its templates are in
[`tile_table_inl.h`](../../include/utils/tile_table_inl.h)): a blob's voxels as regions, blocks and bricks. Format:
[tile table](formats/tile-table.md). Plan: [voxel-storage-tiles](../plans/voxel-storage-tiles.md).

| Name | What |
|---|---|
| `TileTable` | `resolution`, `texels`, `leafStart`, `materials`, counts |
| `TileLayout`, `tileTableLayout(table)` | Where the arrays are, from the header texel; `regionIndex`, `blockIndex` |
| `buildTileTable(tree64, materials, res)` | From a tree (old files, the brick-map path) |
| `tileTableVoxel(table, v, &slot)` | One voxel, looked up as the shader does |
| `applyTileEdits(table, edits)` (`TileEdit`) | Edit in place; only the touched 16³ blocks are re-encoded |
| `downsampleTileTable(table, steps)` | Storage LOD, 4× per axis a step |
| `tileTableWellFormed`, `tileTableRecount` | Check an untrusted table; recompute its counts |
| `tileTableVoxelCount`, `tileTableBounds` | Size and box |
| `forEachTileVoxel`, `forEachTileVoxelIn` | Visit voxels |

## `utils/blob_voxels.h`

[`include/utils/blob_voxels.h`](../../include/utils/blob_voxels.h): **how to read and write a
`GeometryBlob`'s voxels.** A blob holds whichever form (tile table or tree64) was written last and
derives the other on demand, keyed by `contentStamp`; the blob's fields are the storage behind these,
not an API.

| Name | What |
|---|---|
| `blobTiles`, `shareBlobTiles` | The table; shared (stays as it is whatever is done to the blob after: what a physics shape reads) |
| `setBlobTiles`, `editBlobTiles(blob, res)` | Replace it; edit it in place. Both take a fresh content stamp. |
| `blobTree`, `blobTreeMaterials`, `setBlobTree`, `setBlobTreeMaterials`, `treeFromTileTable` | The tree form, for what still takes a tree |
| `blobMaterials`, `editBlobMaterials` | The material bytes of either form (a palette remap) |
| `blobEmpty`, `blobVoxelBytes` | Size |
| `replaceChunkTiles(scene, chunk, tiles, res)` | A chunk's voxels replaced (a streaming worker builds the table off-thread and installs it here) |

## `utils/block_table.h`

[`include/utils/block_table.h`](../../include/utils/block_table.h): the dense top of a **tree64**,
for the older GPU march (`PJV_TREE64_MARCH` in the shader). Built at upload, never authored or stored.
Plan: [block-table-traversal](../plans/block-table-traversal.md). A tile table has these levels in
its own layout.

## `utils/tile_collision.h`

[`include/utils/tile_collision.h`](../../include/utils/tile_collision.h): what collision asks of a
tile table, read from its levels with nothing built beforehand.

| Name | What |
|---|---|
| `tileBoxesIn(table, layout, low, high, out)` | The solid boxes reaching a range: a full block is one box; the rest merge greedily |
| `tileBlockBoxes` | One block's boxes |
| `tileTableRaycast` | A ray's first solid voxel |
| `tileBrickSlot`, `tileCellShift` | A voxel's slot; how many sub-shape id bits a voxel index needs |

## `utils/scene_loading.h`

[`include/utils/scene_loading.h`](../../include/utils/scene_loading.h): loading part of a scene.
Blocks (one chunk, or one grid cell) come into memory and go, as a rule the program writes decides.
Plan: [world-loading](../plans/world-loading.md). The entity side: [`runtime/loading.h`](runtime.md#runtimeloadingh).

| Name | What |
|---|---|
| `SceneBlocks` | The blocks of a scene opened lazily (`open`, `load`, `unload`, `edited`, `bounds`, `adopt`, `setKeptLimit`) |
| `SceneLoader` | Drives it: `open`, `setRule`, `setViewers`, `update(scene)`, `settle`, `stats` |
| `LoadPolicy`, `loadEverything()`, `loadNear(active, kept, margin)`, `loadInside(box, presence)` | Ready-made rules; or write a `LoadRule` |
| `Presence` | `Absent`, `Kept`, `Active` |
| `Viewer`, `LoadCandidate`, `LoadHint`, `LoaderConfig`, `LoaderStats`, `WorldBox` | Rule inputs and settings |
| `residentVoxelBytes(scene)` | What is in memory |

A block out reads `CELL_NOT_LOADED` (empty to everything that draws or collides); edits to it are
dropped, and a block changed since it was read is kept when unloaded and comes back as left.

## `utils/voxel_management.h`

[`include/utils/voxel_management.h`](../../include/utils/voxel_management.h): brick maps and tree64 -- the
authoring path and the older form; a blob's voxels are a [tile table](formats/tile-table.md).

| Name | What |
|---|---|
| `createVoxelBrickMap(dims)`, `brickMapSetVoxel`, `brickMapClearVoxel`, `brickMapHasVoxel`, `cloneBrickMap` | The editable form |
| `updateChunkFromBrickMap(chunk, map)`, `buildTree64FromBrickMap(map, res)` | Brick map → tree64 |
| `brickMapFromTree64(geometry, materialIDs, res)` | tree64 → brick map |
| `downsampleTree64(...)` | Drop levels of a tree. The engine's storage LOD is `downsampleTileTable`. |
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
| `CollisionParams`, `CollisionRepresentation` | `Boxes` (default) or `Tiles` (voxel bodies collide as their tile table, read in place); piece budget, decode limit. `defaultCollisionRepresentation()` honours `PROJV_COLLISION=tiles` |
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
