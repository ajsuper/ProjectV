# Tile tables: one voxel format for rendering, physics and disk

Status: **in progress**, 2026-10-24, on branch `tile-tables`. Phases 0 and 0b (the lab) done; in the
engine, Phases 1-5 done as far as the delivery log below says, and what is left is listed there.

Goal: replace the tree64 as the format voxels are stored, edited, simulated and drawn in, with one
that (a) answers "what is at this voxel" in constant time without decoding anything, (b) edits in
place, and (c) stays sparse for scenes that are **solid all the way through**, not just hollow shells,
so high-fidelity solid scenes stay affordable.

## Why

**Physics and destruction are too slow for games.** Measured on Voxel Mayhem with West Point Marina
(`MAYHEM_MAP`, 10 cm voxels), one cannonball or blast:

| Where the hit step's time went | ms |
|---|---|
| Re-creating the static world's whole Jolt body (3,887 chunk shapes) after any edit | 107-131 |
| Working out what the hit breaks (decoding 64^3 chunks out of their trees, grouping shards) | 7-54 |
| Cutting the pieces out (new components with palette copies) and the support pass | 4-21 |
| The Jolt step, ~450 bodies | 5-10 |

and after it, up to 8 catch-up steps a frame (`Time::maxFixedStepsPerFrame`), so a 150 ms step became
500-1,000 ms frames. Rendering and uploads were 1-5 ms.

Part of that is how bodies are built (one compound for the whole world, boxes as the collision
representation; see "Related" below). But the rest comes back to the format: a tree is good for the
GPU's ray march and for compression on disk, and bad for what physics, fracture and editing do --
random lookups, neighbour walks, small writes. Every one of them decodes a chunk out of its tree
first, and every edit rebuilds the tree.

**The block table already showed the way.** `docs/plans/block-table-traversal.md` flattened the
tree's top two levels into coordinate-addressed tables, so a ray's step reads them in parallel and
waits on one read (the leaf): 1.3-2x faster across the renderers. This plan finishes the job.

**The tree does not keep solid scenes cheap.** It skips empty space, but every 4^3 of a solid region
is a 12-byte leaf node (uniform leaves only shrink its materials to one byte): about 0.2 bytes per
solid voxel, ~200 MB for a solid 1024^3 block. Its cost grows with volume, not surface.

## The design

Three fixed levels (64^3 regions, 16^3 blocks, 4^3 bricks), as the block table has. The region and
block levels are dense within a chunk and addressed straight from a voxel's coordinate, so no read
waits on the one before. Each carries **two** 64-bit masks over its children:

- **occupied**: which children hold anything;
- **full**: which children are entirely solid and one material -- a *tile*.

A full child has nothing below it: its material is one byte in a list ranked by the full mask. Only
partly filled bricks have leaves (a 64-bit voxel mask and their materials: one byte if uniform, else
one per voxel ranked by the mask), allocated per block.

```
region texel (one per 64^3, from the coordinate)   occupied (64 bits), full (64 bits)
  + its full blocks' materials start               (a separate section: read on a hit only)
block texel (one per 16^3, from the coordinate)    occupied (64 bits), full (64 bits)
  + its first leaf, its full bricks' materials start (separate section)
leaf (one per partly filled 4^3 brick)             voxel mask (64 bits), materials start | uniform
```

A ray's step reads the region and block masks (independent) and:
- skips the largest empty cube when an occupied bit is clear, as now;
- stops at a full bit: a tile is a surface hit on entry, with no leaf to read;
- otherwise reads the leaf (the one dependent read, as now).

Its material is looked up once, after the march (one site for all three kinds of hit). Physics and
editing read the same structure on the CPU: a lookup is region -> block -> tile or leaf -> bit, all
O(1); "is this point inside solid" is usually one full bit.

What each kind of space costs:
- **empty**: dense bits inside a chunk (32 B per 16^3 block); nothing between chunks, whose grid cells
  do not exist;
- **solid, one material**: a bit and a byte per 4^3 brick, or per 16^3 block when the whole block is;
- **surface detail**: leaves, as now.

### Decisions

| # | Decision | Why |
|---|---|---|
| D1 | No state carried between steps: every read addressed from the cell | Each attempt to cache in the renderers made them slower (in the block-table work, remembering three cubes' positions cost every ray 5-12%) |
| D2 | ~~Chunks stay at most 256^3~~ Superseded by D9 | The dense levels grew with a chunk's volume (130 KB at 256^3, 8 MB at 1024^3). With D9 they grow with the content's box instead |
| D3 | Masks contiguous; the starts (first leaf, first material) in their own section | A step reads only masks. (Measured: no difference in the lab, kept for the cache) |
| D4 | The march's loop only tests bits; the material is fetched once after it | Three material fetches inside the loop cost 8-12%; one after it, 5.8-6.9% (Phase 0) |
| D5 | Tiles are single-material only | A tile is a bit plus a byte. Mixed interiors (strata, rebar) keep leaves; see Open questions |
| D6 | Store *occupied* and *full*, not partial and full | Partial masks save an AND in the leaf rank and cost an OR in every occupancy test: 4-6% slower (Phase 0b) |
| D7 | No mask caching across steps, even for the two small texels | Kept until the ray leaves them: 12-16% slower (Phase 0b), as caching was for the tree march and the engine's |
| D8 | Leaf materials stay a byte per voxel | Decided: no 4-bit packing |
| D9 | The dense levels cover the box the content spans, not the chunk's cube (a header texel holds the region and block boxes) | A big sparse chunk paid for its cube: the marina's one 1024^3 body was 6.2 MB of index for 411 occupied blocks. The march is clipped to the block box, so no step tests a box -- and a ray stops walking a sparse chunk's empty part |

## Phase 0: the lab experiment (done)

In the ray lab (`~/Development/ProjectV-blocktable/examples/99-ray-lab`, mode 17 `pt_tile_exact`;
the lab's README has the details). Tile tables are built from each chunk's tree64 and checked voxel by
voxel, occupancy and palette slot, against the authoring volume: **0 differ**. `--solid` fills the
lab's ground down to y = 0 (14-22 voxels deep) instead of a 6-voxel crust.

| | Hollow | Solid ground |
|---|---|---|
| tree64 + block tables (today's engine) | 4.69 MB | 6.17 MB |
| **tile tables** | **4.69 MB** | **4.41 MB** |

| GPU ms, path-trace pass, 1273x669 | Plaza | Hall | Tunnel |
|---|---|---|---|
| block tables (today's engine, the lab's march) | 8.26-8.38 | 8.14-8.17 | 9.09-9.24 |
| tile tables | 8.78-8.93 | 8.58-8.66 | 9.73-9.79 |

- **Speed: 5.8-6.9% slower** than the block table, the same on both scenes. The target was 5%.
- **Memory: equal on the hollow scene, 29% less on the shallow solid one** -- where the tree grew 36%.
  A deeper solid widens that: the tree grows with volume, the tiles with surface.
- **Materials are now the largest part** (2.1 of 4.4 MB): mixed grass keeps a byte per voxel (D8).

## Phase 0b: layouts, caching, incoherent rays, memory scaling (done)

- **Layouts.** Partial masks in place of occupied: 4-6% slower than mode 17. Masks kept across steps
  (re-read only on crossing a block or region): 12-16% slower. Both: as slow as caching. (D6, D7.)
- **The gap** moved between 1.4% and 7.6% across runs with no change to what mode 17 computes (a
  helper function moved): call it ~4-7%, with about 3% of run-to-run and code-layout variation.
- **Incoherent rays.** With bounce rays skipped, both modes take 1.2-1.4 ms of the ~8.3-8.9: ~85% of
  the path tracer is incoherent bounce rays. Tiles cost them 2.5-3% more, and the coherent part 3-5%
  more -- no worse for incoherent rays.
- **Images.** Mode 17 is pixel-identical to the lab's exact dense volume (mode 4), as mode 14 is to the
  engine's query (mode 2). (Those two families differ by a uniform 0.77/255 on the plaza, a
  pre-existing split in the lab, not the tiles.) Data check: 0 voxels differ, loose objects included.
- **Memory, worked out** rather than measured: a solid cube of L voxels, one material inside, surface
  detail one brick deep, costs the tree (L/4)^3 x 17 B and the tiles about 6 (L/4)^2 x 17 B plus the
  dense tables -- ~8x less at 256^3, ~20x at 1024^3, ~30x at 4096^3 (18 GB against 0.6 GB). The ratio
  grows about as L/24; the lab's 4-5-brick-deep ground is the shallow end (29%).

## Phases

1. **Storage.** Tile tables as the CPU representation of a blob, with a per-block allocator for
   leaves and material runs (with slack, compacted when it fragments). Built from tree64 on load, so
   every existing asset works. `componentVoxels`, picking, collision decode and hashing read it.
2. **Edits in place.** An edit rewrites the blocks it touches: masks, tiles split into leaves where a
   cut goes through them (the rest of the interior stays tiles), leaves merged back into a tile when a
   brick fills. Uploads become per-block. Replaces the brick map as the editable form.
3. **GPU.** The engine's march (`pjv_utils_DDA.sc`) on tile tables, as Phase 0's; the tree64 march and
   `PJV_TREE64_MARCH` go. LOD: march LOD as now (decided on entering a cube); storage LOD as a coarser
   tile table (1/4 per level, a representative material per cell).
4. **Disk.** `.data` v2 stores tile tables per block. v1 still loads (converted on load). The
   converters write v2.
5. **Physics on it.** Fracture, islands and support read in place: no decode. Then the voxel
   collision shape (below).

## Delivery

### 2026-10-23/24 -- stage A (GPU) and stage B (CPU storage, edits, physics, disk)

**GPU (Phase 3).** A blob's GPU range is its tile table (`utils/tile_table.h`): region masks, region
starts (four to a texel), block masks, block starts (two blocks to a texel), then the leaves in the
tree's own leaf encoding. A full child reads as a synthetic uniform leaf (`pjvTileLeafOf`), so every
leaf reader in `pjv_utils_DDA.sc` -- materials, animation probes, the peel -- is unchanged. The header's
`blockTableStartIndex`/`envelopeTableStartIndex` are now `leafStartIndex`/`envelopeLeafStartIndex`. The
tree march and `PJV_TREE64_MARCH` are gone (no tree is on the GPU); so is `pjvTableLeaf`. The envelope
is a tile table too. Packing the starts tighter cut the dense index from 8,320 to 6,224 texels per
256^3 chunk.

**CPU storage (Phase 1).** A blob holds its voxels as a tile table or a tree, whichever was written
last, and derives the other on first read after a change, keyed by the content stamp
(`utils/blob_voxels.h`: `blobTiles`, `blobTree`, `setBlobTiles`, `setBlobTree`, `editBlobTiles`, ...).
The fields were renamed so the compiler found every direct use; all of them now go through the
accessors. The GPU uploads the blob's table as it is.
Collision decode, content bounds, voxel counts, picking, material usage, slot remaps and the scene
editor's palette undo read tiles.

**Edits (Phase 2).** `applyTileEdits`: only the touched 16^3 blocks are decoded (to a dense 16^3 grid)
and re-encoded; the rest are carried over with their offsets rebased. A block that fills with one
material becomes a tile; a tile cut into becomes leaves only in the bricks the cut reaches. The brick
map is no longer the editable form. Uploads are still whole-blob.

**Physics (Phase 5).** Fracture's `voxelsNear` walks only the 16^3 blocks inside the search sphere
(`forEachTileVoxelIn`), not whole decoded chunks; `voxelCount` sums tile counts. The local support pass
(a static collider too big for the whole-body island search) floods 16^3 blocks decoded on demand.

**Disk (Phase 4).** `.data` v4: a block's geometry words are its tile table and its material bytes the
table's own; the layout is otherwise v3's. v2 and v3 still load, as trees. The writer always writes v4,
converting a block handed to it as a tree (the converters). Blocks read from a file, and tables off the
wire, are checked with `tileTableWellFormed` before anything reads them. Replication sends tile tables.

**Checked.**
- Images: the fast and voxel renderers pixel-identical to `main` (mean |difference| 0.0) on
  SanMiguel-8192 and Teardown castle, after both stages; restir within its own noise (0.004-0.038).
- Tests: the unit suite (348 cases, among them the golden physics recording bit for bit), the soak and
  the `net` preset pass. New: `test_tile_table.cpp` -- every voxel and slot against the tree at each
  resolution, solid interiors, storage LOD, edits against a reference grid, edited-equals-rebuilt, the
  walks, the tree round trip, `.data` v4 round trips and a v3 fixture, well-formedness.
- Voxel Mayhem self-test, sandbox self-test: pass. The Teardown converter writes v4 and its verify loads
  West Point Marina back as all 59.5M voxels; the v4 marina renders.

**Measured.**

| | `main` | tiles |
|---|---|---|
| GPU geometry texels, SanMiguel-8192 / castle / marina | 18.07M / 1.12M / 2.86M | 20.10M / 1.09M / 3.05M |
| frame, voxel renderer (1273x669) | 2.86 / 1.97 / 2.22 ms | 3.14 / 2.09 / 2.34 ms |
| marina on disk: world.data / bodies | 73 MB / 1.4 MB | 86 MB / 9.1 MB |
| Voxel Mayhem castle cannon / blast, physics in the first 0.5 s | 214-223 / 273 ms | 208-224 / 263-275 ms |

Hollow scenes cost more (accepted for scenes that are not the goal); solid ones are where the format
pays (Phase 0b's scaling). The park's chunks are small, so its breaks cost the same; the gain from not
decoding is on big chunks (the marina's), whose map loading is not on this branch yet.

**Left.**
- Per-block uploads, and the per-block leaf allocator with slack the plan describes. Edits now splice
  the touched blocks in place (below), but the blob still re-uploads whole. Measured, not worth a layout
  change yet: a 36k-texel span uploads in ~0.15 ms (so ~0.4 ms for a 94k-texel chunk), against 0.3-1.4 ms
  for the edit itself.
- The authoring brick-map path still builds a tree first (converted to tiles on the terrain's workers,
  elsewhere on first read). A brick map -> tile table builder would skip it, but buys little: tree and
  table together are 0.32 ms of a 256^3 terrain chunk's 85 ms on the workers (columns 45, voxels 37).
- Voxel Mayhem's `MAYHEM_MAP` and its marina measurements live on `restir-pt`; re-measure the fracture
  cost there once both branches meet.
- The voxel collision shape (below).

### 2026-10-24 -- the dense levels over the content's box (D9)

The bodies' growth was not small bodies (a 16^3 body's index is 64 bytes) but big, sparse ones: of the
marina's 9.1 MB of bodies, 6.2 MB was one 1024^3 body's index -- 262,144 blocks, 411 of them occupied.
A table now starts with a header texel giving the region box and the block box its content spans
(origin, size - 1; 10 bits an axis), and the region and block arrays run over those boxes only. A
lookup outside a box is empty. Build and edit both go through one emitter, so an edit that grows or
shrinks the content re-lays the box, and an edited table is still byte-identical to one built fresh.
The march is clipped to the block box (entered by the same rules as the chunk's box: the floor on a grid
plane, the normal, `entered` from the plane), so its reads need no clamp or test. The lookups outside
the march (`pjvTileLeaf`, the probes) clamp and test.

Found on the way: the dense 16^3 grid the edits decode to held `slot + 1` in a byte, so **palette slot
255 read as empty** -- an edit with slot 255 vanished, and once the builder shared that path, the
marina (palette quantised to 256) lost 11,578 voxels at conversion. The grid is 16 bits now; so is the
local support pass's, which had the same bug (a support resting on slot-255 voxels read as missing).
Tested (`palette slot 255 survives the build and edits`).

| | `main` | stage B | box |
|---|---|---|---|
| GPU geometry texels, SanMiguel-8192 / castle / marina | 18.07M / 1.12M / 2.86M | 20.10M / 1.09M / 3.05M | **15.18M / 0.96M / 2.37M** |
| frame, voxel renderer, 2550x1370 | 6.27 / 5.29 / 6.07 ms | +11 / +6 / +8% | 6.80 / 5.63 / 6.50 ms (**+8.4 / +6.4 / +7.1%**) |
| marina on disk: world.data / bodies | 73 MB / 1.4 MB | 86 MB / 9.1 MB | 82.9 MB / 1.46 MB |

The tile tables are now **smaller than the tree** on all three scenes (-16, -14, -17%). Frame time:
the box test, measured on its own, cost ~1.5% (6.92 vs 7.02 ms on SanMiguel with dense boxes and the
test compiled in and out); the clip removed it and then some. The smaller data alone changed nothing
(dense boxes 7.05 ms, real boxes 7.02). Images: fast and voxel renderers pixel-identical to `main` on
SanMiguel and castle (no pixel differs). The converter's verify reads the marina back as all
59,550,142 voxels. Unit tests (350), layering, soak, net (5/5), Voxel Mayhem and sandbox self-tests pass.

The physics demos' self-test fails now and then on "F's shards went on through the wall" (60% needed):
this branch gave 98, 98 and 100 of 164, `main` 109 and 100 (and one run that crashed on exit). It moves
run to run on both and sits at its threshold; not this change, but the check wants a margin.

### 2026-10-24 -- storage LOD on tile tables

`downsampleTileTable(table, steps)` makes the coarse table straight from the fine one; the upload no
longer goes tile table -> tree -> `downsampleTree64` -> tile table. A coarse voxel is solid when anything
it covers is, and takes the slot of the first voxel it covers in Z-order: the voxels `downsampleTree64`
keeps (its "first set voxel of the first child", nested Z-orders being one Z-order over the cube), which
answers open question 3 -- the first, as before. A fine brick's first voxel is its first material byte,
so the walk reads one byte per brick and decodes no leaf; from two steps up a whole 16^3 block falls
inside one coarse voxel and only its first brick is read. The result goes through the same encoder and
emitter as a build, so it is **byte-identical** to the old path's: tested at 16^3 to 1024^3, one to six
steps (`storage LOD from the table is the table of the downsampled tree, byte for byte`).

On a dense 256^3 chunk (70,478 texels): one step 6.6 ms, two 0.26 ms. The old path took ~5 / 0.4 ms
once the blob's tree existed, but making the tree from the table took ~460 ms the first time, and kept
it in memory after. The animation envelope's bake, the last thing at run time that read the tree, now
fills its brick map from the tiles (same report on the advanced renderer's scene: 73 blobs, 474,029
cells; the 14 s it takes is the bake's own, the same either way). The terrain generator (the one user
of `requestChunkLOD`) runs.

### 2026-10-24 -- the build from a tree, 22x faster; one form a blob

Streaming exposed it: the terrain generator spent **8.4 ms a blob** uploading (7.9 s in a 40 s run)
against `main`'s 0.95, because every streamed chunk arrives as a tree and `buildTileTable` ran on the
main thread at 120 ms for a 256^3 chunk (the block table it replaced took 0.03 ms). The build
expanded each block to a dense 16^3 grid and re-encoded it, decoding a Z-order per voxel. Now each block
is encoded from its leaves where the walk meets it (a 16^3 node's children are its leaves, in Z-order),
judging full and uniform from the material bytes exactly as the grid encoder does, so the output is
unchanged (every test that compares a build with an edit or a downsample byte for byte still passes);
the dense grid edits use reads a Z-order table. **120 ms -> 5.4 ms** on that chunk.

Then the terrain worker builds the table itself (`ProcessedChunk::tiles`) and the main thread installs it
(`setBlobTiles`, refines through `replaceChunkTiles`, which now takes the refine's new resolution). Its
uploads: 1.06 / 1.17 ms a blob against `main`'s 0.95 / 1.21 -- the same, within this measurement's
spread (one run each side gave 0.48 and 2.33: the mix of 256^3 and coarse chunks in the first seconds
decides it).

A blob derived tiles from its tree and kept both; tiles derived from a tree now **replace** it (the form
becomes Tiles; a tree asked for again is rebuilt from them). `setBlobTreeMaterials` -- materials joined
to a just-interned tree -- refuses and logs once the tree is gone, rather than join bytes to a tree
rebuilt from tiles that were made without them; every caller calls it straight after interning.
Tested (`tiles made from a tree replace it; a refine installs a table at its new resolution`). Unit
tests (352), layering, soak, net (5/5), Voxel Mayhem and sandbox self-tests pass.

### 2026-10-24 -- edits in place

`applyTileEdits` decoded every block of the chunk and emitted the table again for each batch: **4.4 ms**
on a 256^3 chunk of 94,596 texels, whatever the edit's size. It now decodes only the touched blocks
and splices each into the table: its region bits and solid byte, its block entry, its run of materials
and its leaves replaced, and the offsets behind them shifted (the leaves' material offsets, the later
blocks' starts, the later regions' starts). The result is the bytes a fresh build gives -- tested over
48 random batches at 64^3 and 256^3 against the table rebuilt from its own voxels (41 went in place, 7
repacked). It repacks as before when the box would change (a block filled outside it, one emptied on its
edge) or more than 32 blocks are touched. On that chunk: one voxel **0.31 ms**, a radius-8 sphere (8
blocks) **1.38 ms**. Voxel Mayhem's cannon (small chunks) is unchanged: 166 / 162 ms of physics in the
first 0.5 s against `main`'s 167 / 156.

## Related: physics that reads voxels directly

Teardown collides voxel shapes against each other directly: a voxel corner can only meet a face, and
edges meet edges. So a shape's collision is its voxels -- nothing to build, nothing to rebuild after an
edit. As a custom Jolt shape on tile tables:
- voxel vs voxel: each surface corner of one tested against the other's tiles and leaves (O(1)), edge
  vs edge for what corners miss, contacts reduced to ~4 a pair;
- voxel vs convex (characters, cannonballs): the cells in their overlap only;
- raycasts: the march.
That removes the 107-131 ms world rebuild, the box budget (`CollisionParams::pieceBudget`, where 4,096
left 431 of the marina's chunks colliding as solid blocks) and the 2.2 s collision build at load. Risks:
tunnelling of fast small pieces (Teardown substeps), many-contact flush faces, the edge-edge test. Its
own plan once Phase 1 lands.

Near-term physics fixes that do not wait for this:
- fewer catch-up steps when the fixed clock falls behind, so one slow step is not followed by eight;
- one static body per block (or group of blocks) for big static colliders, so an edit rebuilds only
  what it touched;
- debris as cheap bodies in the engine (one box, discrete, not colliding with debris -- what Voxel
  Mayhem does for maps today);
- pieces copying only the palette entries they use (a blast took the global palette from 1,585 entries
  to 56,625).

## Open questions

1. Mixed-material interiors: worth a "palette tile" (a full child with a few materials in a small
   per-child array), or are leaves enough?
2. Animation envelopes are trees today; they follow the same conversion, but are they worth tiles?
3. ~~Storage LOD's representative material for a tile-table cell: the most common, or the first?~~
   The first in Z-order, as the tree's was (2026-10-24, storage LOD on tile tables).
