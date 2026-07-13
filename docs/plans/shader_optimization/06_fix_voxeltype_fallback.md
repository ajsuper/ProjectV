# Plan 06: Fix findVoxelTypeDataIndex Fallback Bug

## Objective

Fix a correctness bug in `findVoxelTypeDataIndex` (lines 1191–1203). The
"fast approximate fallback" loop has an unconditional `return` that
always returns the first entry in the search radius, regardless of whether
it matches the target Z-order. This means any voxel that isn't found by
the binary search returns the wrong voxel's data.

This is a **correctness fix, not a performance optimization**. Frame time
should not change.

## The bug

```glsl
// ---------- Fast approximate fallback ----------
// beginningIndex is now the insertion point
const int SEARCH_RADIUS = 8;

int start = max(0, beginningIndex - SEARCH_RADIUS);
int end   = min(count - 1, beginningIndex + SEARCH_RADIUS);

for (int i = start; i <= end; i++) {
    return i * VOXEL_TYPEDATA_SLICES;  // BUG: returns on first iteration
}
```

The `return` is inside the `for` loop body but NOT inside any conditional.
It executes on the very first iteration (`i == start`) and returns
immediately, regardless of whether `voxelTypeDatas(i * SLICES + start)`
matches the target Z-order.

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_fixFallback.sc`. Make ALL changes only in the
   copy.

2. **Fix the fallback** in `findVoxelTypeDataIndex`:
   - The fallback should scan `start..end` and return the index whose
     Z-order key matches the target, OR return -1 if none match.
   - Corrected logic:
     ```glsl
     for (int i = start; i <= end; i++) {
         uint key = voxelTypeDatas(i * VOXEL_TYPEDATA_SLICES + int(voxelTypeDataStartIndex));
         if (key == ZOrder) {
             return i * VOXEL_TYPEDATA_SLICES;
         }
     }
     return -1;
     ```
   - Also note: `findVoxelTypeDataIndexExact` (line 1142) has a similar
     loop structure — check if it has the same bug and fix if so. It
     appears to use a different (correct) pattern with a conditional
     return, but verify.

3. **Do not change any other function.**

## How to test

1. Change the include in `albedo.frag` to
   `#include <pjv_utils_DDA_fixFallback.sc>`.
2. Uncomment the `fetchVoxelData` line in `albedo.frag` so voxel colors
   are actually rendered.
3. Run `bash compFast.sh`.
4. Run `./docs/examples/terrain_generator/terrain_generator`.
5. Compare the image to the baseline (with fetchVoxelData). If the binary
   search was always finding its target (likely for most hits), the image
   may be identical — the bug only manifests for voxels that the binary
   search misses. Look for subtle color differences, especially at chunk
   boundaries or sparse regions.
6. Frame time should be ~identical to the 32ms fetchVoxelData baseline.

## What to report back

- Whether the bug was actually causing visible artifacts (or if the
  binary search was always finding its target and the fallback was
  never reached).
- Whether `findVoxelTypeDataIndexExact` had the same bug.
- The exact diff.
