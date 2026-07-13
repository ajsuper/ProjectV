# Plan 02: Coarse Grid Mip over cellMap

## Objective

Add a coarse occupancy mip above the flat `cellMap` so that `marchGrid` can
skip runs of empty cells in O(1) instead of stepping through them one at a
time. This is expected to be a **2–5× speedup on sparse grids** where rays
spend many DDA steps walking through empty cells.

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_coarsegrid.sc`. Make ALL changes only in the copy.

2. **Add a new sampler** for the coarse mip:
   ```glsl
   USAMPLER2D(coarseCellMap, 16); // or next available sampler slot
   ```
   Check `gpu_interface.cpp` for which sampler slots are in use. If unsure,
   use a high slot (e.g., 16) and note it in a comment. **Do not modify
   CPU-side code** — just declare the sampler in the shader. If the sampler
   isn't bound, `texelFetch` will return 0 (which you treat as "empty").

3. **Design the coarse mip layout** (shader-side understanding):
   - The coarse map is a lower-resolution grid (e.g., 1/8 resolution per
     axis). Each coarse cell = 8×8×8 fine cells.
   - Stored as a 2D texture: `texelFetch(coarseCellMap, ivec2(linCoarse % width, linCoarse / width), 0).r`
   - Value: number of occupied fine cells in this coarse block, or just a
     boolean (0 = all empty, nonzero = has at least one occupied cell).
   - For a 3D grid with `dims = (W, H, D)`, coarse dims = `((W+7)/8, (H+7)/8, (D+7)/8)`.

4. **Modify `marchGrid`** (around line 1340):
   - After computing the entry cell and before the DDA loop, also compute
     the entry coarse cell.
   - In the DDA loop, **before** checking the fine `cellMap`, check the
     coarse map for the current coarse cell.
   - If the coarse cell is fully empty (0), skip ahead to the next coarse
     cell boundary along the ray. This is an "amortized DDA step" — compute
     the exit t of the current coarse cell and advance all fine DDA
     variables to that point in one step.
   - If the coarse cell is non-empty, fall through to the existing per-cell
     check (no change to current behavior).

5. **The skip-ahead logic**:
   - Compute the coarse cell's AABB in grid-local space.
   - Compute the exit t of that AABB along the ray using a slab test.
   - Set `tCurrent = max(tCurrent, exitT)`.
   - Recompute the fine cell from `p = lo + ld * tCurrent`.
   - Recompute `tMax` for the new fine cell.
   - Continue the DDA loop.
   - This is similar to "space skipping" in efficient GPU voxel traversers.

6. **Important**: If the coarse map returns 0 everywhere (uninitialized
   sampler), the shader must fall back to the original per-cell behavior.
   Treat "coarse value == 0 or unavailable" as "can't skip, proceed normally."

## How to test

1. Change the include in `albedo.frag` to `#include <pjv_utils_DDA_coarsegrid.sc>`.
2. Run `bash compFast.sh`.
3. Since the coarse map isn't populated by CPU code yet, the shader should
   fall back to per-cell stepping — image must be identical to baseline.
4. Frame time should be ~identical (the win only comes when the CPU
   populates the coarse mip).

## What to report back

- The coarse mip texture layout (dimensions, encoding, sampling logic).
- The skip-ahead logic (how you compute the exit and reset DDA state).
- Confirmation that the image is identical to baseline without the coarse
  map being populated.
- Any issues with the skip-ahead causing boundary misses (off-by-one in
  the exit t calculation).
