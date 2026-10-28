# Voxel data

What is inside a `GeometryBlob`: a **tile table** the GPU marches and physics reads, and **material
bytes**, one palette slot per solid voxel. A **tree64** and a **brick map** still exist as the older
form and the authoring path. Headers: [`utils/tile_table.h`](../../include/utils/tile_table.h),
[`utils/blob_voxels.h`](../../include/utils/blob_voxels.h),
[`data_structures/voxel.h`](../../include/data_structures/voxel.h). Byte layouts:
[tile table](../reference/formats/tile-table.md), [tree64](../reference/formats/tree64.md).

## Forms of the same voxels

```text
                       edit in place (applyTileEdits: only the touched 16³ blocks)
                          ┌──────────────┐
   tile table + materials ◀──────────────┘          ← what .data v4 stores, the GPU reads,
        ▲        │                                      physics collides as, edits change
        │        │ createTexturesForScene / flushSceneUpdates
        │        ▼
        │   GPU textures (tree64Data, materialIDs, headerData, ...)
        │
        │ buildTileTable                treeFromTileTable
   tree64 + materialIDs  ◀──────────────────────────────────  (v2–v3 files, envelopes, brick-map bakes)
        ▲
        │ updateChunkFromBrickMap / bakeMaterialsFromBrickMap
   brick map   (the authoring path: generators and tools that place many voxels)
```

| Form | Type | Good at | Lives |
|---|---|---|---|
| tile table | `utils::TileTable` | Marching, in-place edits, collision, compact storage | The blob's stored form |
| material bytes | `std::vector<uint8_t>` | One palette slot per solid voxel (a tile: one byte) | With the table |
| tree64 | `std::vector<uint32_t>` | Old files, envelopes, the authoring path | Derived on demand |
| brick map | `VoxelBrickMap` (`GeometryBlob::brickMap`) | Random writes while authoring | Derived, never saved |

**Read and write a blob's voxels through [`utils/blob_voxels.h`](../reference/utils.md#utilsblob_voxelsh)**
(`blobTiles`, `setBlobTiles`, `editBlobTiles`, `blobTree`, `setBlobTree`). A blob holds whichever form
was written last and derives the other on demand, keyed by `contentStamp`; the blob's fields are the
storage behind them, and reading one directly can return the form that was not written last.

## Tile tables

Three fixed levels: 64³ **regions**, 16³ **blocks**, 4³ **bricks**. Region and block entries are dense
masks over the box the content spans, addressed straight from a voxel's coordinate, so no read waits
on another. Each carries two 64-bit masks over its children: *occupied*, and *full* (entirely solid,
all one material: a **tile**). A tile has nothing below it, a bit and a material byte; only partly
filled bricks have leaves. Memory follows the surface, not the volume.

```cpp
const utils::TileTable& t = utils::blobTiles(blob);
uint8_t slot;
bool solid = utils::tileTableVoxel(t, {x, y, z}, &slot);

utils::TileTable& edit = utils::editBlobTiles(blob, chunk.resolution);
utils::applyTileEdits(edit, {{{x, y, z}, /*add*/ true, /*slot*/ 3}});
```

## tree64

The form before tile tables, and still the form of `.data` v2–v3, envelopes and the brick-map path:
a sparse tree with **64 children per node** (4 per axis). Its **leaves** are the tile table's leaves,
bit for bit.

```text
resolution 64 = 4³  →  3 levels
  level 0  root      1 node      each child bit = a 16³ block
  level 1            ≤ 64        each child bit = a 4³ block
  level 2  leaves    ≤ 4096      each child bit = one voxel
```

- **A node is three `uint32_t`**: two words of child mask (64 bits) and one word of pointer + flags.
- **A leaf's third word carries a material offset**, a byte offset into the material bytes.
- **A uniform leaf** (every solid voxel the same slot) stores one material byte instead of one per voxel.

```cpp
tree64IsLeaf(data3);          // bit 0
tree64LeafIsUniform(data3);   // bit 1
tree64LeafMatOffset(data3);   // bits 2..31
```

## Resolution is a power of four

4, 16, 64, 256, 1024, ... never 32 or 128. Depth is `log2(resolution) / 2`, truncated, on both CPU
and GPU, so a 32³ chunk is traversed as if it were 16³.

```cpp
utils::isValidChunkResolution(64);    // true
utils::isValidChunkResolution(128);   // false
```

`resolution` and `voxelScale` are fixed for a component's life. Together they set its world size,
`nativeScale = voxelScale * resolution`. Changing either is a resample, not an edit, which is why
neither has a setter.

## Material bytes

Each solid voxel has one byte: a **slot in its component's palette**, not a colour. The palette
lives in `compose.json`, so two components can instance one `.data` and be coloured differently.
See [Materials](materials.md).

```text
leaf data3 ──material offset──▶ materials[offset + rank of the voxel among the leaf's set bits]
                                 (uniform leaf: materials[offset], for every voxel; a tile: its own byte)
```

Slot 255 is `INVALID_MATERIAL`, so a component holds **255 usable entries** (`MAX_PALETTE_ENTRIES`); a
file's 256th entry is folded into its nearest on load.

## Brick maps

The authoring form: sparse 64³ bricks, each a bitmask plus a position → slot map. Generators and tools
that place many voxels at once build one; ordinary edits (`updateScene`) go to the tile table directly
and drop the blob's brick map.

```cpp
auto map = utils::createVoxelBrickMap(utils::computeBrickDims(64));
utils::brickMapSetVoxel(*map, x, y, z, slot);         // O(1)
utils::brickMapClearVoxel(*map, x, y, z);
utils::brickMapHasVoxel(*map, x, y, z);

utils::updateChunkFromBrickMap(chunk, *map);           // → chunk.geometryData (a tree64, then buildTileTable)
std::vector<uint8_t> materialIDs;
utils::bakeMaterialsFromBrickMap(chunk.geometryData, materialIDs, *map);
```

See [Editing](editing.md).

## Level of detail

Two independent knobs:

| Knob | Set with | Changes | For |
|---|---|---|---|
| Storage LOD | `setBlobRenderLOD(scene, pool, lod)`, `requestChunkLOD(scene, chunk, res)` | How many levels are **uploaded**; each step is 4× coarser per axis | **VRAM**. Distant geometry resident at a fraction of the cost. |
| Traversal LOD | `RayQuery` fields in the shader (`pjvQueryLinearLOD`, `pjvQueryLODFootprint`) | How deep a ray descends, by distance | Shading detail |

- **The CPU copy is always full resolution.** Storage LOD is applied at upload by
  `downsampleTileTable`; editing, saving and picking never see it.
- **Storage LOD is not a frame-time fix.** It barely changes render cost. The warning, and what to
  look at instead, is at the top of `RayQuery` in
  [`shaders/include/pjv_utils_DDA.sc`](../../shaders/include/pjv_utils_DDA.sc).
- **A shared blob uploads once**, at the finest LOD any of its chunks asked for. Each chunk still
  caps its own traversal (`traversalLOD` in its GPU header).
- **Finer than stored needs new geometry**: `requestChunkLOD` returns `NeedsRegeneration`; generate
  it and install it with `replaceChunkGeometry`, which keeps the chunk's handle and cell, with no pop.

## Content bounds

`utils::blobContentBounds` gives the box the voxels actually occupy, cached on the blob. The GPU
header carries it, so a ray that misses the box skips the march. The loose-chunk BVH is built from
it. It is invalidated by `markBlobContentChanged`. See [The scene](scene.md#the-geometry-pool).

## Animation envelopes

A blob may carry a second, quarter-resolution tree (`envelope`) marking where animated voxels
*could* be drawn, plus a motion-set byte per cell. `geometry` always stays the rest pose, so
everything animation-blind (picking, physics, the voxelizer) is unaffected. Baked by
`utils::bakeAnimationEnvelope` ([`utils/animation.h`](../../include/utils/animation.h)); design in
[plans/animation.md](../plans/animation.md).
