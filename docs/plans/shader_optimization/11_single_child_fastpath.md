# Plan 11: Single-Child Fast Path

## Objective

When a tree64 node has exactly one valid child (popcount of the valid mask
== 1), the descent is deterministic: there's only one child to go to,
regardless of the ray's position. We can skip:
1. The Z-order computation from the ray position
   (`getZOrderInParentFromThisNodesLevel`)
2. The validity check (`checkZOrderInValidMasks` — we know it's valid)
3. The siblings calculation (`calculateSiblingsBeforeThisZOrder` — with
   only one child, siblings before = 0)

In sparse trees (like terrain surfaces), most interior nodes have very
few children — often just 1 or 2. The single-child case may be the
**most common descent pattern**.

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_singlechild.sc`. Make ALL changes only in the
   copy.

2. **Add a popcount helper** (if not already available — `countBits` exists
   at line 689):
   ```glsl
   uint validMaskPopcount(uint mask1, uint mask2) {
       return countBits(mask1) + countBits(mask2);
   }
   ```

3. **Add a "find first set bit" helper** that returns the Z-order index of
   the single set bit:
   ```glsl
   uint findSingleValidZOrder(uint mask1, uint mask2) {
       // Returns the Z-order index (0..63) of the single set bit
       if (mask1 != 0u) {
           // Bit is in mask1 (z-order 0..31)
           // Use findLSB to find the bit position
           return uint(findLSB(int(mask1)));
       } else {
           // Bit is in mask2 (z-order 32..63)
           return 32u + uint(findLSB(int(mask2)));
       }
   }
   ```
   Note: `findLSB` is defined at line 768. GLSL also has a builtin
   `findLSB` — use whichever the compiler accepts.

4. **Add the fast path** in the descent loop, before the existing
   `checkZOrderInValidMasks` call (around line 942):
   ```glsl
   // Fast path: single valid child — skip Z-order computation + siblings
   if (validMaskPopcount(data.data1, data.data2) == 1u) {
       uint childZOrder = findSingleValidZOrder(data.data1, data.data2);

       // Check if it's a leaf
       if ((data.data3 & 0b1) == 1u) {
           // Single leaf child — return hit
           returnData.foundBox.position = traversalPosition;
           returnData.foundBox.size = stepSize;
           returnData.steps = stepCount;
           returnData.rayT = rayT;
           returnData.normal = hitNormal;
           return returnData;
       }

       // LOD check
       if (candidateNodeLevel <= computeTargetLOD(rayT, rayQuery)) {
           returnData.foundBox.position = traversalPosition;
           returnData.foundBox.size = stepSize;
           returnData.steps = stepCount;
           returnData.rayT = rayT;
           returnData.normal = hitNormal;
           return returnData;
       }

       // Descend: siblings before = 0 (only one child)
       // Z-order in parent = childZOrder (deterministic)
       BoxAABB candidateBox;
       candidateBox.position = vec3(traversalPosition);
       candidateBox.size = float(stepSize);
       ivec3 highResPosition = determineTraversalCoordinatesFromRayAndBoxAndRayDistance(ray, candidateBox, rayT);
       uint bottomChildPointer = (data.data3 >> 1) & 0b01111111111111111111111111111111;
       uint parentDataIndex = nodeStack[nodeStackQuantity - 2u].dataIndex;
       // childrenBeforeThisNode = 0 (single child — no siblings before)
       nodeStack[nodeStackQuantity - 1u].dataIndex = bottomChildPointer + parentDataIndex;
       nodeStack[nodeStackQuantity].thisNodeZOrderInParent = childZOrder;
       nodeStack[nodeStackQuantity].dataIndex = 0;
       nodeStackQuantity += 1;
       candidateNodeLevel -= 1;
       shift = 2u * candidateNodeLevel;
       stepSize = 1u << (shift);
       traversalPosition = (highResPosition >> shift) << shift;
       data = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
       continue; // Re-enter descent loop at new level
   }
   ```

   **Important**: The single-child fast path is taken when the CURRENT
   NODE (whose data is in `data`) has exactly one valid child. The
   Z-order of that child is deterministic. We skip the ray-position-based
   Z-order computation and the siblings calculation.

5. **The fast path also applies in the DDA stepping loop** (line 1065).
   When a new cell becomes current and we check validity, if the node has
   a single child, we know the cell is valid (it's the one child) and can
   skip the `checkZOrderInValidMasks` call. However, the DDA stepping
   loop's validity check also serves as a "did we enter a new valid cell"
   gate — with a single child, every step in the node is invalid EXCEPT
   the one child. So the fast path here would be: skip all cells except
   the single child's position. This is more complex — implement the
   descent fast path first and measure.

## How to test

1. Change the include in `albedo.frag` to
   `#include <pjv_utils_DDA_singlechild.sc>`.
2. Run `bash compFast.sh`.
3. Run `./docs/examples/terrain_generator/terrain_generator`.
4. **Image must be identical to baseline.**
5. Compare frame time to baseline.

## Expected impact

- **Sparse trees (terrain)**: moderate — many interior nodes have 1-2
  children. Saves ~10 ALU ops (Z-order compute + siblings) per descent
  for single-child nodes.
- **Dense trees**: near-zero — most nodes have many children.
- **Does not save texture fetches** — the child data fetch at line 987
  is still needed. This is an ALU optimization, not a fetch optimization.

## What to report back

- Confirmation that the image is identical to baseline.
- Frame time comparison vs baseline.
- Whether the fast path triggers often (add a debug counter or color
  tint to check).
