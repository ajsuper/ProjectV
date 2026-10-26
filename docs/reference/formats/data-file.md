# .data

The voxels of one component: one block's [tile table](tile-table.md) and its material bytes, or a
grid of them. A `PVDT` container. No placement and no palette; both live in [compose.json](compose-json.md). Read and
written by [`utils/compose_io.h`](../../../include/utils/compose_io.h); in memory `DataFile` /
`DataBlock` ([`data_structures/compose.h`](../../../include/data_structures/compose.h)).

```text
┌────────────────────┬──────────────────────────────┬──────────────────────────────────────────┐
│ header (24 bytes)  │ block table (64 B per block) │ blob region: each block's arrays, packed │
└────────────────────┴──────────────────────────────┴──────────────────────────────────────────┘
```

Little-endian throughout. Current version **4**; versions **2–4** are read. Version 1 (per-voxel
`voxelTypeData`, no material bytes) is refused: re-voxelize it.

| Version | Geometry words are | Block entry |
|---|---|---|
| 4 | a [tile table](tile-table.md)'s texels (4 words each) | 64 bytes |
| 3 | a [tree64](tree64.md) | 64 bytes |
| 2 | a tree64, no envelope | 40 bytes |

## Header

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 4 | `char[4]` | magic `"PVDT"` |
| 4 | 4 | `uint32` | version (`4`) |
| 8 | 4 | `uint32` | flags. Bit 0: some block has an envelope (a hint; the lengths are the authority). |
| 12 | 4 | `uint32` | block count (1 for a simple asset, N for a grid) |
| 16 | 4 | `uint32` | resolution: edge length in voxels, shared by every block, a power of four |
| 20 | 4 | `float32` | voxel scale: world size of one voxel |

## Block table

One entry per block, straight after the header. **64 bytes from v3, 40 in v2**, so a reader must pick
the stride from the version.

| Offset | Size | Type | Field | v2 |
|---|---|---|---|---|
| 0 | 4 | `int32` | grid x | ✓ |
| 4 | 4 | `int32` | grid y | ✓ |
| 8 | 4 | `int32` | grid z | ✓ |
| 12 | 4 | — | padding (0) | ✓ |
| 16 | 8 | `uint64` | geometry offset (bytes, from the file start) | ✓ |
| 24 | 4 | `uint32` | geometry length, in **`uint32`s** | ✓ |
| 28 | 8 | `uint64` | material offset (0 if absent) | ✓ |
| 36 | 4 | `uint32` | material length, in **bytes** (0 if absent) | ✓ |
| 40 | 8 | `uint64` | envelope offset (0 if absent) | |
| 48 | 4 | `uint32` | envelope length, in `uint32`s | |
| 52 | 8 | `uint64` | envelope motion offset (0 if absent) | |
| 60 | 4 | `uint32` | envelope motion length, in bytes | |

A v2 entry stops at byte 40; its envelope fields read as zero (no animation).

## Blob region

Per block, in table order, with no padding between arrays:

| Array | Element | Is |
|---|---|---|
| geometry | `uint32` | v4: the block's [tile table](tile-table.md), its header texel first. v2–v3: its [tree64](tree64.md) |
| materials | `uint8` | Palette slots, indexed by the leaves' material offsets (and, in a tile table, by tiles) |
| envelope | `uint32` | Optional: a second tree64 at resolution / 4 marking where animated voxels may be drawn |
| envelope motion | `uint8` | Optional: one motion-set byte per set envelope cell, addressed like the materials |

A v4 block's offsets are checked (`tileTableWellFormed`) as it is read; a malformed one is refused, not
trusted. Loading puts the table in a `GeometryBlob` as it is; a v2–v3 tree is converted to a tile table
(`utils/blob_voxels.h`). In memory a `DataBlock` carries whichever its file had: `tiles` (v4) or
`geometry` + `materialIDs` (v2–v3).

## Blocks and grids

| Blocks | Becomes | Block `(x, y, z)` |
|---|---|---|
| 1 | A loose chunk | `(0, 0, 0)` |
| N | A grid: cell `(x, y, z)` at `origin + (x, y, z) * resolution * voxelScale` | ≥ 0; negative cells are skipped on load |

## Reading part of a file

The block table is a streaming index: read it alone, then seek to the blocks you want.

```cpp
DataFileHeader header = utils::readDataFileHeader("terrain.data");   // no geometry read
for (const BlockEntry& entry : header.blocks)
    if (near(entry.gridX, entry.gridY, entry.gridZ))
        DataBlock block = utils::readDataBlock("terrain.data", entry);  // one block's arrays
```

`readDataBlock(path, index)` reads the table and then one block. Prefer the `BlockEntry` overload
in a loop.

## Writing

```cpp
DataFile file = utils::dataFileFromComponent(scene, handle);   // a Chunk or Grid component
if (!utils::writeDataFile("out/thing.data", file)) { /* incomplete on disk: don't reference it */ }
```

`writeDataFile` always writes version 4 (a block given as a tree is written as the tile table built from
it) and returns false if the file did not finish writing.
