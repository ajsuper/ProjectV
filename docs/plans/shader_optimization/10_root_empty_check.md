# Plan 10: Root-Level Empty Check

## Objective

After fetching the root node data at the start of the march (line 936), if
the root's valid mask is all zeros (`data.data1 == 0 && data.data2 == 0`),
the entire tree64 is empty — return miss immediately without entering the
DDA loop.

This is a trivial 2-comparison check that can save the entire march for
empty chunks. In an editing system, chunks may be allocated but empty
(e.g., freshly created cells that haven't been filled yet).

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_rootempty.sc`. Make ALL changes only in the copy.

2. **Add the check** right after the root data fetch (line 936):
   ```glsl
   Tree64NodeData data = tree64(tree64StartIndex);

   // Fast path: if the root has no valid children, the entire tree is empty.
   if (data.data1 == 0u && data.data2 == 0u) {
       returnData.foundBox.position = vec3(0);
       returnData.foundBox.size = -1;
       returnData.steps = 0;
       returnData.rayT = -1.0;
       returnData.normal = vec3(0.0);
       return returnData;
   }
   ```

3. **That's the entire change.** One if-statement, 2 comparisons.

## How to test

1. Change the include in `albedo.frag` to
   `#include <pjv_utils_DDA_rootempty.sc>`.
2. Run `bash compFast.sh`.
3. Run `./docs/examples/terrain_generator/terrain_generator`.
4. Image must be identical to baseline (if no empty chunks exist, the
   check never triggers).
5. Compare frame time to baseline.

## Expected impact

- **No empty chunks**: zero impact (the check is 2 comparisons, negligible).
- **Some empty chunks**: saves the entire march (DDA loop + all fetches)
  for each empty chunk the ray enters.
- **Many empty chunks** (e.g., after grid expansion in the editing system):
  could be significant.

## What to report back

- Confirmation that the image is identical to baseline.
- Frame time comparison vs baseline.
- Whether any empty chunks were encountered (the check triggered).
