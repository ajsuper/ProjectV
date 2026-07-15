# Plan 09: Tree64 March Early-Exit with maxDistance

## Objective

`marchRayThroughTree64_DDA` and `castRayThroughTree64` do not receive a
`maxDistance` parameter. The march runs until it finds a hit or exits the
chunk's bounding box — even if there's already a closer hit from a previous
grid cell or loose chunk.

Adding `maxDistance` lets the march **early-exit** when `rayT` exceeds the
closest known hit, skipping unnecessary DDA steps and node fetches.

## Where the wasted work happens

In `marchGrid` (line 1380), the grid DDA steps through cells front-to-back.
It already checks `if (tCurrent >= maxDistance) break` at the grid level.
But when it calls `castRayThroughTree64` for an occupied cell, the tree64
march inside runs to completion — even if the ray would find a hit at
t=50 inside this chunk while a previous chunk already found a hit at t=30.

The march does ~30 DDA steps + ~10 tree64 fetches past t=30 for nothing.

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_maxdist.sc`. Make ALL changes only in the copy.

2. **Add `maxDistance` parameter** to `marchRayThroughTree64_DDA`:
   ```glsl
   SceneIntersectData marchRayThroughTree64_DDA(
       Ray ray, RayQuery rayQuery, BoxAABB boundingBox,
       uint tree64StartIndex, uint tree64EndIndex, uint tree64Resolution,
       float maxDistance)  // NEW
   ```

3. **Add early-exit checks** in the march:
   - In the outer loop (line 937), at the top:
     ```glsl
     if (rayT >= maxDistance) {
         returnData.foundBox.size = -1;
         returnData.steps = stepCount;
         return returnData;
     }
     ```
   - In the inner DDA loop (line 999), after computing `rayT` (line 1016):
     ```glsl
     if (rayT >= maxDistance) {
         returnData.foundBox.size = -1;
         returnData.steps = stepCount;
         return returnData;
     }
     ```

4. **Add `maxDistance` parameter** to `castRayThroughTree64`:
   ```glsl
   SceneIntersectData castRayThroughTree64(
       Ray ray, RayQuery rayQuery, uint headerIndex,
       float maxDistance)  // NEW
   ```
   - Convert `maxDistance` from world space to voxel space before passing
     to the march. The march runs in voxel units where `boundingBox.size =
     header.resolution`. The conversion (inverse of line 1138):
     ```glsl
     float maxDistanceVoxel = maxDistance * (tree64BoundingBox.size / header.scale);
     ```
   - Pass `maxDistanceVoxel` to `marchRayThroughTree64_DDA`.

5. **Update all callers**:
   - `marchGrid` (line 1389): pass `maxDistance` to `castRayThroughTree64`.
     `maxDistance` is already a parameter of `marchGrid`.
   - `raySceneIntersect` loose chunk path (line 1437): pass
     `closestDistance` to `castRayThroughTree64`.

6. **Edge case**: if `maxDistance` is infinity (or very large, like the
   initial `100000000` at line 1410), the early-exit never triggers and
   behavior is identical to the original. This is the correctness fallback.

## How to test

1. Change the include in `albedo.frag` to
   `#include <pjv_utils_DDA_maxdist.sc>`.
2. Run `bash compFast.sh`.
3. Run `./docs/examples/terrain_generator/terrain_generator`.
4. **Image must be identical to baseline.**
5. Compare frame time to baseline.

## Expected impact

- **Single-grid scenes with one chunk in the ray's path**: near-zero (the
  march was already going to run to completion or find a hit).
- **Multi-chunk scenes where the first chunk has a hit**: moderate — the
  march in subsequent chunks early-exits instead of running to completion.
- **Scenes with many loose chunks**: moderate to large — the loose chunk
  path tests all chunks and each march runs to completion currently.
- **Single chunk, no hit (ray misses everything)**: near-zero — the march
  still runs to the bounding box exit (no closer hit to prune against).

The win is proportional to how many marches are currently running past a
known closer hit. For the terrain_generator test scene, this depends on
how many chunks the ray passes through.

## What to report back

- Confirmation that the image is identical to baseline.
- Frame time comparison vs baseline.
- Whether the early-exit triggers often (you can check by temporarily
  adding a counter or using a debug color).
