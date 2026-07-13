# Plan 05: Direct-Addressed VoxelTypeData (Eliminate Binary Search)

## Objective

Replace the binary search in `findVoxelTypeDataIndex` (up to 100 texture
fetches per hit) with a direct-addressed lookup (1 fetch). This is the
single biggest fetch-count reduction in the shader.

**WARNING**: Previous investigation found that `fetchVoxelData`'s binary
search accidentally improves performance via an unknown Metal
compiler/hardware interaction. Removing the binary search **may regress
performance** even though it does less work. Measure carefully.

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_directVoxelType.sc`. Make ALL changes only in
   the copy.

2. **Understand the current layout**:
   - `voxelTypeData` is a sorted array of 3-uint records:
     `[ZOrder, color, normals]` per voxel.
   - The binary search looks up the ZOrder to find the voxel's data.
   - This means empty voxels don't have entries — it's a sparse array.

3. **Design the direct-addressed layout** (shader-side only):
   - For a chunk of resolution R, a voxel at (x, y, z) has Z-order index
     `Z = calculateZOrderIndex(x, y, z, R)`.
   - Direct addressing means: `voxelData[Z]` always exists, with a sentinel
     value (e.g., `0xFFFFFFFF` in the color field) for empty voxels.
   - This requires the CPU to build a dense array of size R³ — the shader
     can't change the data layout, but it CAN be written to work with
     either layout (sparse sorted or dense direct) via a `#define`.

4. **Add a `#define DIRECT_VOXEL_TYPE`** guard in the shader:
   - When defined: `findVoxelTypeDataIndex` computes `Z = calculateZOrderIndex(x,y,z,res)`
     and directly fetches `voxelTypeDatas(Z * 3 + startIndex)` (or
     `Z + startIndex` if the dense layout uses 1 uint per voxel for the
     occupancy check, with color/normals packed elsewhere).
   - When not defined: falls back to the original binary search.
   - The direct path skips the binary search loop entirely.

5. **Simplify the direct fetch path**:
   ```glsl
   int findVoxelTypeDataIndex_direct(int x, int y, int z,
                                     uint voxelGridResolution,
                                     uint voxelTypeDataStartIndex,
                                     uint voxelTypeDataEndIndex) {
       uint Z = calculateZOrderIndex(uint(x), uint(y), uint(z), voxelGridResolution);
       // Direct: Z-order index IS the array index (dense layout)
       uint entryIndex = Z * VOXEL_TYPEDATA_SLICES + voxelTypeDataStartIndex;
       // Check if voxel exists (sentinel check)
       uint key = voxelTypeDatas(int(entryIndex));
       if (key == 0xFFFFFFFFu) return -1; // empty voxel
       return int(entryIndex);
   }
   ```

6. **Modify `findVoxelTypeDataIndex`** to dispatch between direct and
   binary-search paths based on `#define DIRECT_VOXEL_TYPE`.

7. **Do NOT modify CPU code.** The direct path will return wrong results
   on current data (which is sparse, not dense), but the shader will
   compile and the performance can be measured. For correctness testing,
   leave the `#define` off; for performance testing, turn it on (knowing
   the colors will be wrong but the fetch pattern is what we're measuring).

## How to test

**Two tests are needed:**

### Test A: Correctness (define OFF)
1. Change the include in `albedo.frag` to
   `#include <pjv_utils_DDA_directVoxelType.sc>`.
2. Ensure `DIRECT_VOXEL_TYPE` is NOT defined.
3. Run `bash compFast.sh`.
4. Image must be identical to baseline (falls back to binary search).
5. Frame time should match baseline.

### Test B: Performance (define ON)
1. Define `DIRECT_VOXEL_TYPE` at the top of `albedo.frag` (or in the
   shader copy).
2. Run `bash compFast.sh`.
3. Run `./docs/examples/terrain_generator/terrain_generator`.
4. Image will be WRONG (colors/normals will be garbage) — this is
   expected. We're measuring the fetch pattern, not correctness.
5. Compare frame time to:
   - Baseline without fetchVoxelData (~41ms)
   - Baseline with fetchVoxelData (~32ms)
   - This variant (direct fetch, no binary search)

**Interpretation:**
- If direct fetch is FASTER than 32ms → the binary search was costing
  performance and the accidental speedup was something else. The CPU-side
  dense layout change is worth implementing.
- If direct fetch is ~32ms → same performance, no win from removing the
  search. Not worth the CPU-side layout change.
- If direct fetch is ~41ms → removing the binary search removed the
  accidental speedup. Confirms the binary search should be left alone.

## What to report back

- The `#define` guard structure.
- Frame times for both Test A and Test B.
- Which of the three interpretations above applies.
- Whether the dense layout (R³ entries) would be feasible memory-wise for
  typical chunk resolutions (e.g., 64³ = 262k entries × 3 uints = 3 MB
  per chunk — comment on this).
