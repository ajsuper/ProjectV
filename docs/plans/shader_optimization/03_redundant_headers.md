# Plan 03: Fix Redundant headers() Call in fetchVoxelData

## Objective

`fetchVoxelData` (line 1255) calls `findVoxelIndex`, which internally calls
`headers(headerIndex)` (4 texelFetch calls, line 1235). Then `fetchVoxelData`
calls `headers(headerIndex)` **again** at line 1259 to get
`voxelTypeDataStartIndex` — a value already available from the first fetch.

This wastes 4 texture fetches per hit pixel. Fix it by reusing the header
from `findVoxelIndex`.

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_noRedundantHeaders.sc`. Make ALL changes only in
   the copy.

2. **Modify `findVoxelIndex`** (line 1234):
   - Change the return type or add an out-parameter to return both the
     voxel index AND the `voxelTypeDataStartIndex`.
   - Option A: Change signature to return a struct with both values.
   - Option B: Add an `out uint voxelTypeDataStartIndex` parameter.
   - Option C: Have `findVoxelIndex` return the full `chunkHeader` and let
     the caller extract what it needs.
   - Pick whichever is cleanest. Option B is probably simplest.

3. **Modify `fetchVoxelData`** (line 1255):
   - Remove the redundant `headers(headerIndex)` call at line 1259.
   - Use the `voxelTypeDataStartIndex` returned from `findVoxelIndex`.
   - Everything else stays the same.

4. **Do not change any other function.** This is a small, surgical fix.

## How to test

1. Change the include in `albedo.frag` to
   `#include <pjv_utils_DDA_noRedundantHeaders.sc>`.
2. Run `bash compFast.sh`.
3. Uncomment the `fetchVoxelData` line in `albedo.frag` (since the speedup
   only manifests with fetchVoxelData active).
4. Run `./docs/examples/terrain_generator/terrain_generator`.
5. Image must be identical to the `fetchVoxelData` baseline.
6. Frame time should be slightly lower than the 32ms fetchVoxelData baseline
   (4 fewer fetches per hit). The improvement may be small (1–2ms) since
   the second headers() call likely hits L2 cache.

## What to report back

- Which option you chose (A/B/C) and why.
- The exact diff.
- Frame time comparison vs baseline (with fetchVoxelData active).
