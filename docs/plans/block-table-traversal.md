# Block tables: walking the tree64 without descending it

Status: **done** (Phases 1–4); one Phase 4 item tried and dropped (see Delivery).

## Why

A ray marching a chunk's tree64 re-descends it at every step: root → 64³ node → 16³ node → leaf, each
read waiting on the one before (a child's address needs its parent's mask, a popcount and the child
pointer). In `examples/99-ray-lab` (a clone at `~/Development/ProjectV-raylab`, not in this repo), the
same slim loop over the real tree64 was barely faster than the engine's march, and ~2.7× slower than a
dense volume doing the same job: the cost was the chain of dependent reads, not the march code.

A dense volume is fastest but costs memory in proportion to the world's volume (49 MB for the lab's
scene beside a 4.1 MB tree64) and needs world-space updates for moving bodies. The lab's per-chunk brick
maps (mode 10) got most of the speed with one dependent read per step, but copied every brick (9.4 MB).

## The design

**Keep the tree64; flatten its top.** Per blob, a block table (`utils/block_table.h`):

- one texel per 64³ region: a copy of the region node's mask (which 16³ blocks are occupied);
- one texel per 16³ block: a copy of the block node's mask (which 4³ bricks) and the index of its first
  child leaf.

Nothing below the 16³ level is copied: the leaves are the tree's own nodes. A step reads the region and
block texels (addressed from the cell, so independent) and then at most one leaf — one dependent read
where the descent had four. Exact materials, uniform leaves, the peel, animation and everything else
that reads a leaf work unchanged.

In the lab this was **mode 14/15**: 7–9% slower than the brick-map copy when exact, equal with mode 6's
tricks, for **0.58 MB** (14% of the tree) instead of 9.4 MB.

**Where it lives.** Appended to the blob's own range in `tree64Data`, right after its nodes — and the
envelope's table after the envelope's nodes. `GPUBlobRange::geomTexelLen`/`envTexelLen` count both;
`geomNodeCount`/`envNodeCount` say where the tables start. So every allocation, repack, rollback and
overlap check carries the tables with no code of its own, and no texture or sampler stage was added.
The header's `blockTableStartIndex`/`envelopeTableStartIndex` replace `materialIDEndIndex`/`dataRefID`,
which no shader read.

**Built at upload.** `packTreeWithTable` (`gpu_interface.cpp`) builds the table from exactly the tree it
uploads (after storage LOD), with `utils::buildBlockTable`. The resolution comes from the tree's depth
(`utils::tree64Resolution`). Measured on Voxel Mayhem's cannon break: flush 1.16 ms with tables, 1.17–1.26
without — no measurable cost.

**Nothing changes for developers.** `raySceneIntersect`, `raySceneIntersectFrom`, `raySceneTransmittance`,
the query constructors and opt-ins, `fetchVoxelMaterialFromHit`/`fetchVoxelColor` keep their signatures
and results.

## The march (`pjv_utils_DDA.sc`)

`marchRayThroughTree64_DDA` keeps its signature; its unused `tree64EndIndex` parameter became
`blockTableStart`. Each step finds the largest empty aligned cube round the current voxel (64, 16, 4 or 1)
from the region mask, the block mask and the leaf, and leaves it through the exit face, setting the next
cell exactly on that axis. One voxel-hit site carries the tree march's decisions unchanged: the `tMin`
gate, the zero-measure corner guard, the material fetch, the animated skip and `pjvPeelConsume`.

Matching the tree march exactly took two rules, each found by an A/B that differed:

- **LOD cut-offs are decided on entering a cube**, as the tree march decided them on descending into a
  node. Deciding every step coarsened a block halfway through a ray already inside it (the taa
  renderer's LOD bounces differed by 0.8 levels in 255). Which cube a step entered is read off the
  boundary it crossed, one integer: remembering the three cubes' positions cost every ray 5-12%.
- **A coarsened hit's material** (`MATERIAL_INDEX_NEEDS_DESCENT`, looked up at the cube's corner, often
  not a voxel) takes the next present child at each level whose bit is unset — where the tree's
  `childPtr + rank` landed. Reading those corners as empty made coarse probe and shadow hits black and
  dimmed the voxel renderer by 6–8 levels.

`pjvProbeLeafNode` (animation) and `fetchVoxelMaterialAtCoord` go through the table too.
`#define PJV_TREE64_MARCH` before including the file compiles the tree march and the descents instead,
for A/B captures.

## Delivery

**At full screen, in the ray lab** (`examples/99-ray-lab` run on both engines with `--fullscreen`:
2560×1440 on the 144 Hz monitor; path-trace pass GPU ms, views plaza / hall / tunnel). The lab's own
traversals (modes 4, 14) do not use the engine's march and are the control: they match across builds.

| | Old engine | Block tables | |
|---|---|---|---|
| mode 2, `raySceneIntersectFrom` | 58.4 / 69.0 / 74.5 | 42.9 / 41.8 / 46.9 | 1.36–1.65× |
| mode 1, `raySceneIntersect` (full query) | 58.7 / 68.8 / 73.9 | 44.8 / 47.1 / 52.1 | 1.31–1.46× |
| mode 14, the lab's own block-table march (control) | 37.3 / 33.2 / 37.1 | 38.3 / 33.3 / 37.2 | — |

The engine reaches 79–89% of the lab's own march over the same tables. Narrowing that:
- the lab's scene loop with the engine's per-chunk march (`castRayThroughTree64`) and without the LOD
  bookkeeping measured 38.0 / 35.7 / 40.9, so the march itself is within ~10% of the lab's;
- the rest is `raySceneIntersectFrom`'s generality -- nearest-hit shadows, the loose-chunk machinery --
  where a merged loop, grids first and an any-hit flag were all measured and none helped in the engine
  (below);
- building the loose BVH from two chunks instead of eight helped 3-5% (the lab has six objects, each of
  which the linear list decoded a 6-texel header for on every ray).

RTX 3070, 1273×1370 window, old march vs block tables, back to back (GPU ms; advanced renderer: whole
frame, the mean of the 100-frame stat windows after the first five of a 900-frame run):

| | Old | Block tables |
|---|---|---|
| tree64 path tracer, SanMiguel / Bistro | 16.50 / 14.72 | 8.72 / 7.99 |
| fast renderer gbuffer_shade, SanMiguel / Bistro | 2.86 / 3.20 | 2.35 / 2.37 |
| voxel renderer gbuffer, SanMiguel / Bistro | 0.96 / 1.31 | 0.77 / 0.91 |
| voxel renderer probes, SanMiguel / Bistro | 0.37 / 0.29 | 0.23 / 0.22 |
| Voxel Mayhem frame (gbuffer, probes) | 4.27 (2.29, 0.50) | 4.18 (2.23, 0.45) |
| advanced renderer, animated close-ups A / B | 37.0 / 45.7 | 25.3 / 32.1 |
| advanced renderer, no animation | 20.6 | 16.4 |
| advanced renderer, everything 40% transparent / + refraction | 91.3 / 94.3 | 67.2 / 68.1 |

Images (mean |difference| in 0–255, at frame 300): tree64, fast and voxel renderers and the advanced
renderer (animation, transparency, refraction) identical to within 0.002; taa 0.01–0.35 (a path tracer
with LOD bounces); Voxel Mayhem 0.09; the voxel renderer's SanMiguel view 0.23 from `maxDistance` now
being honoured (below).

Mayhem barely moves: its world is many 16³ chunks, whose trees are one block deep.

**Phase 4, scene level:**

- Done: `raySceneIntersectFrom` honours `RayQuery::maxDistance` (the search starts pruned there). The
  voxel renderer's probes ask for it and now get it: 0.50 → 0.46 ms in Mayhem.
- Tried and dropped: **one `castRayThroughTree64` call site** for loose chunks and grid cells (the lab's
  shape). No gain on the gallery scenes; Mayhem's gbuffer went 2.3 → 3.6 ms and its probes 0.5 → 1.5 ms,
  the merged loop holding the BVH stack and the grid walk live at once.
- Tried and dropped: **an any-hit opt-in** for shadow rays. No gain on the gallery scenes (a grid's walk
  already stops at its first occupied hit); its checks cost Mayhem's gbuffer 0.2 ms more than they saved.
- Tried and dropped: **grids before loose chunks** (so loose chunks are pruned by the ground's hit), and
  a **`PJV_NO_PEEL`** compile switch: both within noise in the lab.
- Tried and dropped: **skipping `computeTargetLOD` for queries that ask for no LOD.** It made the
  advanced renderer's animated and transparent queries 50-60% slower (26 → 43 ms, 67 → 101 ms) -- a
  compiler layout effect, not work -- and gained nothing in the lab.
- Done: the loose BVH is built from **two** loose chunks, not eight (3-5% in the lab).

Verification run: `ctest --preset dev` (with `test_block_table.cpp`: every voxel of trees at 4/16/64/256,
empty, full and LOD-downsampled, through table → leaf → bit), `MAYHEM_SELFTEST=1` and `MAYHEM_BENCH=cannon`
and `SANDBOX_SELFTEST=600` built with `-DPROJV_VALIDATE_GPU_RANGES` (no overlaps).

**Measuring the advanced renderer:** its first stat windows include compile and warm-up hitches; a
240-frame run averaged them in and once reported a 50% regression that was not there. Use 900 frames
and drop the first five windows (`ADVANCED_CAPTURE_FRAME=900`).

**Not measured:** the scene editor and terrain generator (the latter's VRAM statistics now count each
blob's table in `geomTexelLen`).

## Known, pre-existing (seen during the A/B, not caused by this)

- `renderer_gallery --renderer reconstruction` crashes before frame 300 on every scene, on the old code too.
- The gallery and advanced renderer crash intermittently on exit (segfault or floating-point exception),
  as the bgfx Vulkan timer-query crash already noted; the capture scripts retry.
