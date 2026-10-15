# Voxel data

What is inside a `GeometryBlob`: a **tree64** the GPU marches, a **material byte** per solid voxel,
and, while it is being edited, a **brick map**. Headers:
[`data_structures/voxel.h`](../../include/data_structures/voxel.h),
[`utils/voxel_management.h`](../../include/utils/voxel_management.h). Byte layout:
[tree64 format](../reference/formats/tree64.md).

## Three forms of the same voxels

```text
                 edit (O(1) per voxel)
   brick map  ◀───────────────────────┐
  (64³ bricks, bitmask + slot map)    │ brickMapFromTree64   (lazily, on the first edit)
       │                              │
       │ updateChunkFromBrickMap      │
       │ bakeMaterialsFromBrickMap    │
       ▼                              │
   tree64 + materialIDs  ─────────────┘          ← what .data stores and the GPU reads
       │
       │ createTexturesForScene / flushSceneUpdates
       ▼
   GPU textures (tree64Data, materialIDs, headerData, ...)
```

| Form | Type | Good at | Lives |
|---|---|---|---|
| tree64 | `std::vector<uint32_t>` (`GeometryBlob::geometry`) | Ray marching, compact storage | Always |
| material bytes | `std::vector<uint8_t>` (`GeometryBlob::materialIDs`) | One palette slot per solid voxel | Always |
| brick map | `VoxelBrickMap` (`GeometryBlob::brickMap`) | Random writes | Only after an edit; derived, never saved |

## tree64

A sparse tree with **64 children per node** (4 per axis).

```text
resolution 64 = 4³  →  3 levels
  level 0  root      1 node      each child bit = a 16³ block
  level 1            ≤ 64        each child bit = a 4³ block
  level 2  leaves    ≤ 4096      each child bit = one voxel
```

- **A node is three `uint32_t`**: two words of child mask (64 bits) and one word of pointer + flags.
- **Levels are stored root-first and contiguously**, and child pointers are relative. That is what
  makes coarsening a truncation (see [LOD](#level-of-detail)).
- **A leaf's third word carries a material offset**, a byte offset into `materialIDs`.
- **A uniform leaf** (every solid voxel the same slot) stores one material byte instead of one per
  voxel. On heightfield terrain ~96% of leaves are uniform.

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
leaf data3 ──material offset──▶ materialIDs[offset + rank of the voxel among the leaf's set bits]
                                 (uniform leaf: materialIDs[offset], for every voxel)
```

Slot 255 is `INVALID_MATERIAL`; a component holds up to 256 entries (`MAX_MATERIALS_PER_COMPONENT`).

## Brick maps

The editable form: sparse 64³ bricks, each a bitmask plus a position → slot map.

```cpp
auto map = utils::createVoxelBrickMap(utils::computeBrickDims(64));
utils::brickMapSetVoxel(*map, x, y, z, slot);         // O(1)
utils::brickMapClearVoxel(*map, x, y, z);
utils::brickMapHasVoxel(*map, x, y, z);

utils::updateChunkFromBrickMap(chunk, *map);           // → chunk.geometryData (tree64)
std::vector<uint8_t> materialIDs;
utils::bakeMaterialsFromBrickMap(chunk.geometryData, materialIDs, *map);
```

After the first edit the brick map stays on the blob; `updateScene` rebakes from it. See
[Editing](editing.md).

## Level of detail

Two independent knobs:

| Knob | Set with | Changes | For |
|---|---|---|---|
| Storage LOD | `setBlobRenderLOD(scene, pool, lod)`, `requestChunkLOD(scene, chunk, res)` | How many levels of the tree are **uploaded**; each step is 4× coarser per axis | **VRAM**. Distant geometry resident at a fraction of the cost. |
| Traversal LOD | `RayQuery` fields in the shader (`pjvQueryLinearLOD`, `pjvQueryLODFootprint`) | How deep a ray descends, by distance | Shading detail |

- **The CPU copy is always full resolution.** Storage LOD is applied at upload by
  `downsampleTree64`; editing, saving and picking never see it.
- **Storage LOD is not a frame-time fix.** It barely changes render cost. The warning, and what to
  look at instead, is at the top of `RayQuery` in
  [`include/pjv_utils_DDA.sc`](../../include/pjv_utils_DDA.sc).
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
