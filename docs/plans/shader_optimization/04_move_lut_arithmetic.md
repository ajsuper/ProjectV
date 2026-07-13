# Plan 04: Replace MOVE_LUT with Z-Order Arithmetic

## Objective

Replace the 448-element `MOVE_LUT` constant array (line 152, ~1.8 KB) with
arithmetic Z-order neighbor computation. The LUT forces a large constant
array into the shader that may spill to slow memory on some GPUs. Arithmetic
replaces it with ~5–10 ALU instructions per DDA step.

This is expected to be a **modest win on ALU-bound GPUs** (NVIDIA/AMD
discrete) and a **near-zero change on bandwidth-bound GPUs** (Apple Silicon).

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_noLUT.sc`. Make ALL changes only in the copy.

2. **Understand the current LUT** (lines 152–217):
   - `MOVE_LUT[zOrder * 7 + encodeDir(direction)]` returns the new Z-order
     index after moving one step in the given direction.
   - `encodeDir` (line 735) maps `vec3` direction to [0..6]:
     `+X=1, -X=2, +Y=3, -Y=4, +Z=5, -Z=6, zero=0`.
   - The Z-order index uses XYZXYZ bit layout (2 bits per axis, 6 bits total
     for a 4×4×4 grid → indices 0–63).
   - Moving +X means incrementing the X coordinate, which in Z-order means
     adding a specific bit pattern.

3. **Implement Z-order arithmetic neighbor computation**:
   - The key insight: Z-order indices don't map to simple +1/-1 in each
     axis, but the "Kummer/Hook" method computes Z-order neighbors via
     big-endian integer arithmetic.
   - For a 2-bit-per-axis Z-order (branching factor 4, indices 0–63):
     - To move +X: add `0b001` (bit 0 of the Z-order) using "carryless add"
       that doesn't propagate into Y/Z bits.
     - To move -X: subtract `0b001` similarly.
     - To move +Y: add `0b010`.
     - To move -Y: subtract `0b010`.
     - To move +Z: add `0b100`.
     - To move -Z: subtract `0b100`.
   - The "carryless add" for Z-order: to add `delta` to `zOrder` without
     carry between axes, XOR the axis's bits with the increment, but
     propagate carry WITHIN the axis's 2 bits.
   - **Practical approach**: For each axis, extract the 2 bits, convert to
     a 2-bit unsigned integer, add/subtract 1, clamp to [0, 3], and
     re-interleave. This is ~10 ALU ops per step.

   Here's a reference implementation for the +X case (you'll need all 6):
   ```glsl
   uint moveZOrder_noLUT(uint zOrder, vec3 direction) {
       // Extract X bits (bits 0 and 3 of zOrder in XYZXYZ layout)
       uint xBits = (zOrder & 1u) | ((zOrder >> 3) & 1u) << 1;  // 2-bit X coord
       uint yBits = ((zOrder >> 1) & 1u) | ((zOrder >> 4) & 1u) << 1;
       uint zBits = ((zOrder >> 2) & 1u) | ((zOrder >> 5) & 1u) << 1;

       if (direction.x == 1.0) xBits = min(xBits + 1u, 3u);
       else if (direction.x == -1.0) xBits = (xBits == 0u) ? 0u : xBits - 1u;
       else if (direction.y == 1.0) yBits = min(yBits + 1u, 3u);
       else if (direction.y == -1.0) yBits = (yBits == 0u) ? 0u : yBits - 1u;
       else if (direction.z == 1.0) zBits = min(zBits + 1u, 3u);
       else if (direction.z == -1.0) zBits = (zBits == 0u) ? 0u : zBits - 1u;

       // Re-interleave
       return (xBits & 1u) | ((yBits & 1u) << 1) | ((zBits & 1u) << 2) |
              (((xBits >> 1) & 1u) << 3) | (((yBits >> 1) & 1u) << 4) | (((zBits >> 1) & 1u) << 5);
   }
   ```
   Note: the branch-heavy `if/else if` above may be worse than the LUT on
   some GPUs. Consider a branchless version using `select()`/`mix()`.
   Also verify the clamping behavior matches the LUT exactly (the LUT may
   not clamp — check what it returns for boundary cases like zOrder=0
   moving -X).

4. **Remove the `MOVE_LUT` array** (lines 152–217) and the `moveZOrder`
   function (line 745). Replace `moveZOrder` calls with your new function.

5. **Critical**: The new function must produce **bit-exact** results to the
   LUT for all 64 × 7 = 448 entries. Check the LUT values for boundary
   cases (e.g., zOrder=0 moving -X, zOrder=63 moving +X) — does it clamp,
   wrap, or return the same value? Your arithmetic must match.

## How to test

1. Change the include in `albedo.frag` to `#include <pjv_utils_DDA_noLUT.sc>`.
2. Run `bash compFast.sh`.
3. Run `./docs/examples/terrain_generator/terrain_generator`.
4. Image must be identical to baseline — any difference means the Z-order
   arithmetic is wrong.
5. Compare frame time to baseline (both with and without fetchVoxelData).

## What to report back

- Whether you used the branchy or branchless version.
- How you handled boundary cases (clamp vs wrap).
- Confirmation that the image is identical to baseline.
- Frame time comparison vs baseline.
