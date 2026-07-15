# Plan 07: Ray-Direction-Aware Forward Mask Skip

## Objective

When the DDA march is inside a tree64 node and the current cell is not
occupied, instead of stepping one cell at a time through potentially empty
cells, check whether ANY occupied cell exists ahead of the ray in the node.
If not, skip to the node's exit in one step.

This uses the node's **already-fetched** valid mask (no new texture fetches)
and the ray's direction to build a conservative "forward mask" of cells the
ray could possibly hit. If `forwardMask & validMask == 0`, skip the node.

**No CPU-side changes required.** Pure shader optimization. Testable via
include-swap.

## Mechanism

A 4×4×4 node has 64 children, with a 64-bit valid mask (`data.data1` =
bits 0–31, `data.data2` = bits 32–63). The ray is at cell `(cx, cy, cz)`
with direction signs `(sx, sy, sz)` where each is -1, 0, or +1.

The ray can only ever visit cells satisfying:
- If `sx > 0`: `x >= cx` (ray moves +X, never goes back)
- If `sx < 0`: `x <= cx`
- If `sx == 0`: `x == cx` (ray parallel to this axis plane)
- Same for y and z

The **forward mask** = `xMask(sx, cx) & yMask(sy, cy) & zMask(sz, cz)`.

If `(forwardMask & validMask) == 0`, the ray cannot hit any occupied cell
in this node. Skip to the node's exit boundary.

## Implementation

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_fwdskip.sc`. Make ALL changes only in the copy.

2. **Precompute axis-forward masks** (12 masks total, each 64 bits = 2 uints):

   For a 4×4×4 grid with Z-order indexing (XYZXYZ bit layout, 6 bits per
   index), precompute:
   - `X_GE[4][2]` — for each cx, the mask of all cells with x >= cx
   - `X_LE[4][2]` — for each cx, the mask of all cells with x <= cx
   - `X_EQ[4][2]` — for each cx, the mask of all cells with x == cx
   - Same for Y and Z

   That's 36 uint pairs = 72 uints = 288 bytes. Alternatively, compute
   them on the fly. Since there are only 4 values per axis and the masks
   are simple bit patterns, on-the-fly computation may be cleaner:

   ```glsl
   // Build forward mask for axis with sign s, current cell coord c (0..3)
   // Returns two uints (lo, hi) of the 64-bit mask
   // For +direction: mask = cells where coord >= c
   // For -direction: mask = cells where coord <= c
   // For 0:         mask = cells where coord == c
   ```

   **Important**: The Z-order layout makes axis-aligned masks non-trivial
   to compute with simple bit tricks. The cleanest approach is to
   precompute the 36 mask pairs as constant arrays (like MOVE_LUT but
   smaller). Or use `inverseZOrderIndex` to decompose each of the 64
   Z-order indices and test the condition (64 iterations — but this is
   a one-time setup per node, not per step).

   **Recommended**: Precompute as `const uint` arrays. 36 entries × 2
   uints = 72 uints — smaller than MOVE_LUT (448 uints).

3. **Add a forward-mask check function**:
   ```glsl
   bool anyValidAhead(uint mask1, uint mask2, uint zOrderInParent,
                      ivec3 stepDir) {
       // Decode zOrderInParent to (cx, cy, cz)
       uvec3 cell = inverseZOrderIndex(zOrderInParent, 4u);

       // Build forward mask
       uint fwdLo, fwdHi;
       // X axis
       if (stepDir.x > 0)      { fwdLo = X_GE_LO[cell.x]; fwdHi = X_GE_HI[cell.x]; }
       else if (stepDir.x < 0) { fwdLo = X_LE_LO[cell.x]; fwdHi = X_LE_HI[cell.x]; }
       else                    { fwdLo = X_EQ_LO[cell.x]; fwdHi = X_EQ_HI[cell.x]; }
       // AND in Y axis
       // AND in Z axis
       // (use branchless select if preferred)

       // Check against valid mask
       return ((fwdLo & mask1) | (fwdHi & mask2)) != 0u;
   }
   ```

4. **Integrate into `marchRayThroughTree64_DDA`**:

   In the DDA stepping loop (the `for (int i = 0; i < 12; i++)` loop
   around line 999), after a step where `checkZOrderInValidMasks` returns
   false (cell not occupied), and before the next iteration:

   ```glsl
   if (!checkZOrderInValidMasks(data.data1, data.data2, ...)) {
       // Current cell not valid — check if anything is ahead
       if (!anyValidAhead(data.data1, data.data2,
                          nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent,
                          stepI)) {
           // Nothing ahead — skip to node exit
           // Advance rayT to the exit of the current node
           // The node spans [traversalPosition, traversalPosition + stepSize]
           // Compute exit t and jump there
           // Then break to let the boundary-crossing logic pop us up
           BoxAABB nodeBox;
           nodeBox.position = vec3(traversalPosition);
           nodeBox.size = float(stepSize) * 4.0; // full node extent
           float exitT = getRayBoxExitDistanceForSureHit(ray, nodeBox);
           rayT = exitT;
           // Advance traversalPosition to the exit cell
           // (or just let the boundary-crossing logic handle it)
           break; // exit DDA loop, let pop logic handle it
       }
       // Something IS ahead — continue normal DDA stepping
   }
   ```

   **Alternative integration point**: In the descent loop, when
   `checkZOrderInValidMasks` returns false (the `else` branch at line 988
   that currently just `break`s), check `anyValidAhead` first. If nothing
   ahead, skip the node exit instead of just breaking to the DDA loop.

5. **Correctness invariant**: The forward mask is a **superset** of cells
   the ray will actually visit. If `forwardMask & validMask == 0`, the ray
   truly cannot hit anything. If it's nonzero, the ray MIGHT hit something
   (but might not — the forward mask is conservative). This means:
   - False positive (mask says something ahead, but ray misses it): safe,
     just continue normal DDA stepping.
   - False negative: **impossible by construction** — the forward mask
     includes all cells the ray could reach.

6. **Edge cases to handle**:
   - Ray exactly on a cell boundary (ambiguous which cell it's in)
   - Ray parallel to an axis (stepDir component == 0, use EQ mask)
   - Node at the finest level (stepSize == 1) — forward mask still works
   - Ray exiting through a corner/edge (multiple axes tie in tMax)

## How to test

1. Change the include in `albedo.frag` to
   `#include <pjv_utils_DDA_fwdskip.sc>`.
2. Run `bash compFast.sh`.
3. Run `./docs/examples/terrain_generator/terrain_generator`.
4. **Image must be identical to baseline** — any difference means the
   skip is incorrectly skipping occupied cells. Check especially:
   - Thin walls / single-voxel features (easy to skip past)
   - Diagonal rays through sparse regions
   - Chunk boundaries
5. Compare frame time to baseline (both with and without fetchVoxelData).

## Expected impact

- **Sparse nodes (mostly empty)**: large win — skip many DDA steps per node.
- **Dense nodes (mostly full)**: near-zero — forward mask almost always
  intersects valid mask, so no skip happens. The check costs ~5 ALU ops.
- **Coarse tree levels**: biggest win — skipping one coarse node skips
  4096+ voxels of potential traversal.
- **Net**: should help sparse scenes (terrain with empty space) and be
  neutral on dense scenes. Estimated 1.2–2× on typical terrain.

## What to report back

- Whether you used precomputed constant arrays or on-the-fly computation
  for the axis masks.
- Where you integrated the check (descent loop, DDA loop, or both).
- How you handle the "skip to node exit" (rayT advancement + position
  update).
- Confirmation that the image is identical to baseline.
- Frame time comparison vs baseline (both fetchVoxelData on and off).
- Any edge cases that caused incorrect images.
