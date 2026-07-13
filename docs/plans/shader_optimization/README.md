# Shader Optimization Plans

These plans are designed to be executed independently by separate LLM sessions.
Each plan instructs the LLM to **copy** `include/pjv_utils_DDA.sc` to a new file
`include/pjv_utils_DDA_<suffix>.sc` and make changes only in that copy. This
allows all optimizations to be tested in isolation without conflicting with each
other or with the original.

## How to use

1. Launch a separate LLM session for each plan file in this folder.
2. Give the LLM the plan file content as its prompt.
3. Each LLM creates its own `pjv_utils_DDA_<suffix>.sc` variant.
4. To test a variant: change the `#include <pjv_utils_DDA.sc>` line in
   `docs/examples/terrain_generator/fastRenderer/pathTracerShaders/albedo.frag`
   to `#include <pjv_utils_DDA_<suffix>.sc>`, then run `compFast.sh`.
5. Compare frame times against the baseline (original file).

## Context for all LLMs

- The shader is a tree64 DDA voxel ray marcher. The hot path is
  `marchRayThroughTree64_DDA` (called from `castRayThroughTree64`, called from
  `marchGrid`).
- The test harness is `docs/examples/terrain_generator/` — an albedo-only
  renderer that casts one ray per pixel through the scene.
- Build shaders: `bash compFast.sh` from
  `docs/examples/terrain_generator/`.
- Build the test binary: `make -C docs/examples/terrain_generator`.
- Run: `./docs/examples/terrain_generator/terrain_generator`.
- **Do not modify the original `pjv_utils_DDA.sc`** — always work in your copy.
- The baseline frame time is ~41ms (no fetchVoxelData) or ~32ms (with
  fetchVoxelData) on the author's MacBook. Measure against the same
  configuration you're testing.
- Previous investigation found that `fetchVoxelData`'s binary search
  accidentally improves performance via an unknown Metal compiler/hardware
  interaction. Do not remove or modify `fetchVoxelData` in your variant
  unless your optimization specifically targets it.

## Plans

| File | Optimization | Suffix | Expected impact |
|---|---|---|---|
| `01_subtree_empty_flag.md` | Add subtree-empty bit to tree64 node data | `_emptyflag` | 1.5–3× on sparse scenes |
| `02_coarse_cellmap_mip.md` | Add coarse grid mip over cellMap | `_coarsegrid` | 2–5× on sparse grids |
| `03_redundant_headers.md` | Fix redundant headers() call in fetchVoxelData | `_noRedundantHeaders` | 4 fewer fetches per hit |
| `04_move_lut_arithmetic.md` | Replace MOVE_LUT with Z-order arithmetic | `_noLUT` | 1.1–1.3× on ALU-bound GPUs |
| `05_voxeltype_direct_address.md` | Direct-addressed voxelTypeData (no binary search) | `_directVoxelType` | Eliminates ~100 fetches per hit (verify no regression) |
| `06_fix_voxeltype_fallback.md` | Fix the findVoxelTypeDataIndex fallback bug | `_fixFallback` | Correctness fix, no perf change |
