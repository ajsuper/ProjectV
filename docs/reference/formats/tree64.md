# tree64

The sparse voxel tree of `.data` v2–v3, the brick-map authoring path and animation envelopes: a flat
`uint32_t` array of nodes, 64 children each. **No longer the engine's format**: a blob's voxels are a
[tile table](tile-table.md), which keeps this page's leaf encoding exactly and replaces the levels above
it. A blob converts between the two on demand (`utils/blob_voxels.h`). Constants and helpers:
[`data_structures/voxel.h`](../../../include/data_structures/voxel.h). Builder:
[`utils/voxel_management.h`](../../../include/utils/voxel_management.h). Concepts:
[Voxel data](../../concepts/voxel-data.md).

It is the sparse voxel octree of Laine and Karras (cited in [`SOURCES.md`](../../../SOURCES.md))
with four children per axis instead of two. A 2D drawing of the idea (4 children per node instead of 64):
[`docs/images/IMG_9025.jpeg`](../../images/IMG_9025.jpeg).

## Nodes

Each node is **three `uint32_t`**: one RGB32U texel on the GPU.

```text
 word 0          word 1          word 2 (data3)
┌───────────────┬───────────────┬──────────────────────────────────────────────┐
│ mask, high 32 │ mask, low 32  │ internal: [31..1] child pointer │ [0] = 0    │
│ children 0-31 │ children 32-63│ leaf:     [31..2] material offset│[1] uniform│[0] = 1 │
└───────────────┴───────────────┴──────────────────────────────────────────────┘
```

| Part | Meaning |
|---|---|
| mask (64 bits) | Which of the 64 children exist. Child `i` is bit `63 − i` of `(word0 << 32) | word1`, so child 0 is the top bit of word 0. |
| child index `i` | The child's 4×4×4 Z-order position: `i = x0 | y0<<1 | z0<<2 | x1<<3 | y1<<4 | z1<<5` (bit `n` of each coordinate) |
| bit 0 of data3 | `TREE64_LEAF_FLAG`. Set on the **last level**, whose mask bits are voxels, not nodes. |
| internal: bits 1–31 | Child pointer (`data3 >> 1`), **relative**, in nodes |
| leaf: bit 1 | `TREE64_UNIFORM_FLAG`: every solid voxel in this leaf has the same material |
| leaf: bits 2–31 | Material offset (`data3 >> 2`): a byte index into the block's material array |

```cpp
tree64IsLeaf(data3);  tree64LeafIsUniform(data3);  tree64LeafMatOffset(data3);
encodeLeafNode(materialOffset, uniform);
```

## Levels and pointers

```text
resolution R = 4^L  →  L levels      (64 → 3, 256 → 4, 1024 → 5)

[ root ][ level 1 nodes ...... ][ level 2 nodes (leaves) ............ ]
   │        ▲ only existing children are stored, in child-index order
   └────────┘ child pointer
```

- **Root first, one contiguous run per level, leaves last.**
- **Children are packed**: only set mask bits have a node, in ascending child index.
- **Finding child `i`** of the node at index `n`:
  `n + childPtr + (number of set mask bits below i)`. `n + childPtr` is the node's first child
  (`addPointersTree64` sets it).
- A node one level up from a leaf covers 16³ voxels; the root of a 64³ tree covers all of it.

## Material bytes

Each solid voxel has one byte, a slot in its component's palette.

| Leaf | Byte for the voxel at child index `i` |
|---|---|
| uniform | `materials[offset]`, for every voxel |
| not uniform | `materials[offset + number of set mask bits below i]` |

On the GPU the chunk header's `materialIDStartIndex` is added. On heightfield terrain ~96% of leaves
are uniform.

## Storage LOD: coarsening is truncation

Because levels are contiguous and pointers relative, dropping the finest level is cheap
(`downsampleTree64`). The engine applies storage LOD to tile tables (`downsampleTileTable`, which gives
the table of the downsampled tree), at GPU upload only. On a tree:

1. **Cut the array** after the level to keep. Every pointer in the prefix stays valid.
2. **Mark the new last level as leaves.** Its mask bits, which meant "which children exist", now
   mean "which voxels are solid".
3. **Give each new leaf a material offset**: one representative byte per set bit, collapsed to one
   when they agree.

Do **not** run `addPointersTree64` on the result. It ORs pointer bits in, so a second pass corrupts
the prefix.

One step drops one level: **4× coarser per axis, never 2×**. Depth is derived as
`log2(resolution) / 2` with truncation on both CPU and GPU, so resolution must stay a power of four:
`64 → 16 → 4`. `clampRenderLOD` keeps at least the root level (resolution 4).

## Envelopes

An animation envelope is a second tree64 in the same format, at resolution / 4, whose leaf offsets
index the motion-set bytes instead of materials. See [.data](data-file.md#blob-region).
