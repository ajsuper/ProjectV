# tile table

A block's voxels as three fixed levels -- 64³ regions, 16³ blocks, 4³ bricks -- in one flat array of
RGBA32U texels. What `.data` v4 stores, what a `GeometryBlob` holds, what the GPU marches, what edits
change in place and what physics reads. Header and API:
[`utils/tile_table.h`](../../../include/utils/tile_table.h). Plan:
[voxel-storage-tiles.md](../../plans/voxel-storage-tiles.md).

It replaced the [tree64](tree64.md) as the engine's format. The leaves are the tree64's leaves, bit for
bit; what changed is above them: the region and block levels are dense masks addressed straight from a
voxel's coordinate, so no read waits on another, and a child that is solid and one material is a
**tile** -- a bit and a byte, with nothing below it.

## Masks

Every region and block entry carries two 64-bit masks over its 4×4×4 children (bit 63 of `hi:lo` is
Z-order child 0, as in the tree64):

| Mask | Bit set when the child |
|---|---|
| occupied | holds any voxel |
| full | is entirely solid, all one material: a tile |

`full` is always within `occupied`. Only bricks that are `occupied & ~full` have leaves.

## Layout

```text
texel  0                header: region box origin, region box size − 1,
                                block box origin,  block box size − 1   (10 bits an axis: x | y<<10 | z<<20)
       1 .. +RC         region masks   (occupied hi, occupied lo, full hi, full lo)   x-fastest over the box
         .. +RS         region starts  one word a region, 4 a texel: its first full-block material
         .. +BC         block masks    (occupied hi, occupied lo, full hi, full lo)   x-fastest over the box
         .. +BS         block starts   two words a block, 2 a texel: first partial leaf, first full-brick material
       leafStart ..     leaves         (voxel mask hi, lo, encodeLeafNode(material, uniform), 0)
```

`RC`/`BC` are the region and block boxes' volumes; `RS = ceil(RC / 4)`, `BS = ceil(BC / 2)`.
`tileTableLayout(table)` reads the header into a `TileLayout` with each array's start.

- **The boxes cover the content, not the chunk's cube.** A long, sparse chunk (a pipe, a crane) pays for
  its bounding box. A coordinate outside a box is empty.
- The region box holds the block box (block `b` is in region `b / 4`). Mask bits are Z-orders by
  position in the chunk, so a box never changes what a bit means.
- A block that is itself a tile stores nothing at its block entry: the region's `full` bit answers for it.
- Leaves are in rank order over each block's partial bricks. A tile reads as a synthetic leaf (every bit
  set, uniform, its material offset the tile's byte), so code that reads leaves needs no second path.

## Material bytes

`TileTable::materials`: palette slots. A leaf's offset indexes them exactly as in the
[tree64](tree64.md#material-bytes); a tile's byte is found by ranking its `full` bit from its region's
or block's start.

## Using one

| Do | With |
|---|---|
| Read or write a blob's voxels | `utils/blob_voxels.h`: `blobTiles`, `setBlobTiles`, `editBlobTiles` -- never the blob's fields directly |
| Look up one voxel | `tileTableVoxel(table, v, &slot)`, as the shader does |
| Visit voxels | `forEachTileVoxel`, `forEachTileVoxelIn(table, low, high, visit)` |
| Edit in place | `applyTileEdits(table, edits)`: only the 16³ blocks touched are re-encoded |
| Build from a tree | `buildTileTable(tree64, materials, resolution)` (old files, the brick-map path) |
| Storage LOD | `downsampleTileTable(table, steps)`: 4× per axis a step, at upload only |
| Check untrusted input | `tileTableWellFormed(table)` before anything reads it (files, the wire) |

A blob holds whichever form was written last and derives the other on demand, keyed by its
`contentStamp`; the setters take a fresh stamp (`markBlobContentChanged`).

## On the GPU

The table is uploaded as it is (downsampled first when the blob has a `renderLOD`); the march reads
it level by level from the voxel's coordinate. Physics collides voxels as their tile table, read in
place, when `CollisionParams::representation` is `Tiles` (`utils/tile_collision.h`).
