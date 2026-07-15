# Plan 12: Cache Parent Data + Speculative Sibling Prefetch

## Objective

Systematically reduce texture reads in the tree64 DDA march by:
1. **Caching parent node data in nodeStack** — eliminates the re-fetch on every pop
2. **Speculatively fetching the next valid sibling during descent** — eliminates the next descent's fetch when the ray steps to that sibling (which it usually does at fine levels)

Combined, this reduces effective texture reads by ~50–80% depending on the ray's traversal pattern.

## Why this works where the forward mask didn't

The forward mask only helps **sparse nodes** (skips empty cells). If the scene
is dense (terrain with mostly occupied cells in the ray's path), there aren't
many empty cells to skip — the forward mask rarely triggers.

Cache + prefetch helps **regardless of scene density**. Every pop benefits from
the cache. Every sibling transition benefits from the prefetch. These happen
in both dense and sparse scenes.

## The fetch pattern today

```
March flow for a ray descending, stepping, popping:

1. Fetch root data                        [REAL FETCH]     line 936
2. Descend to child A: fetch A's data     [REAL FETCH]     line 987
3. DDA step through A's children
4. Boundary crossing → pop: fetch parent  [WASTED RE-FETCH] line 1043
5. DDA step to next cell (sibling B)
6. Descend to child B: fetch B's data     [REAL FETCH]     line 987
7. DDA step through B's children
8. Boundary crossing → pop: fetch parent  [WASTED RE-FETCH] line 1043
...
```

## The optimized fetch pattern

```
1. Fetch root data, CACHE in nodeStack[0]  [REAL FETCH]
2. Descend to child A: fetch A's data      [REAL FETCH]
   ALSO: speculatively fetch next sibling B [FREE — same cache line as A]
3. DDA step through A's children
4. Boundary crossing → pop: READ FROM CACHE [NO FETCH — cached in step 1]
5. DDA step to next cell (sibling B)
6. Descend to child B: USE PREFETCHED DATA  [NO FETCH — prefetched in step 2]
   ALSO: speculatively fetch next sibling C [FREE — same cache line as B]
7. DDA step through B's children
8. Boundary crossing → pop: READ FROM CACHE [NO FETCH — cached in step 1]
...
```

## Implementation

### Part 1: Cache parent data in nodeStack

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_cacheprefetch.sc`. Make ALL changes only in the copy.

2. **At root fetch** (line 936):
   ```glsl
   data = tree64(tree64StartIndex);
   nodeStack[0].cachedData = data;  // Cache root data
   ```

3. **At descent** (line 987, after fetching child data):
   ```glsl
   data = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
   nodeStack[nodeStackQuantity - 2u].cachedData = data;  // Cache for later pop
   ```

4. **At pop** (line 1043, replace fetch with cache read):
   ```glsl
   // BEFORE:
   data = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
   // AFTER:
   data = nodeStack[nodeStackQuantity - 2u].cachedData;
   ```

### Part 2: Speculative sibling prefetch

5. **Add prefetch state variables** at the top of the march function (after
   line 936):
   ```glsl
   Tree64NodeData prefetchedData;
   uint prefetchedZOrder = 0xFFFFFFFFu;  // Invalid Z-order = no prefetch
   ```

6. **When descending** (after line 975, where we compute the child's data
   index), also compute and fetch the next valid sibling:

   ```glsl
   // Current child
   uint currentZOrder = nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent;
   uint childrenBefore = calculateSiblingsBeforeThisZOrder(4, data.data1, data.data2, currentZOrder);
   uint childIndex = bottomChildPointer + parentDataIndex + childrenBefore;
   nodeStack[nodeStackQuantity - 1u].dataIndex = childIndex;

   // --- Speculative prefetch of next valid sibling ---
   // Find the next set bit in the valid mask after currentZOrder.
   // This is a sibling whose data is adjacent in tree64Data (same cache line).
   uint nextZ = currentZOrder + 1u;
   bool foundNext = false;
   for (uint z = nextZ; z < 64u; z++) {
       if (checkZOrderInValidMasks(data.data1, data.data2, z)) {
           uint nextChildrenBefore = calculateSiblingsBeforeThisZOrder(4, data.data1, data.data2, z);
           uint nextChildIndex = bottomChildPointer + parentDataIndex + nextChildrenBefore;
           prefetchedData = tree64(nextChildIndex);
           prefetchedZOrder = z;
           foundNext = true;
           break;
       }
   }
   if (!foundNext) {
       prefetchedZOrder = 0xFFFFFFFFu;
   }
   ```

   **Note**: The loop above scans up to 63 entries, but breaks on the first
   valid one. In practice, the next valid sibling is usually within a few
   positions. The fetch is a cache hit (same cache line as the current child),
   so it's nearly free.

   **Important**: This prefetch is speculative. The DDA might not step to
   this sibling next — it might step to a different cell, or pop multiple
   levels. If the speculation is wrong, the prefetched data is simply
   ignored (no correctness impact, just a wasted cache-hit fetch).

7. **When descending and we have a matching prefetch** (replace line 987):
   ```glsl
   // Check if we can use prefetched data
   uint descendingZOrder = nodeStack[nodeStackQuantity - 1u].thisNodeZOrderInParent;
   if (prefetchedZOrder == descendingZOrder) {
       data = prefetchedData;
       prefetchedZOrder = 0xFFFFFFFFu;  // Consume
   } else {
       data = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
   }
   nodeStack[nodeStackQuantity - 2u].cachedData = data;  // Cache for later pop
   ```

### Correctness

- **Cache**: The cached data is the same data that would be fetched. Tree64
  data is immutable during a single march. Correctness is guaranteed.
- **Prefetch**: The prefetched data is for a specific Z-order. When
  descending, we verify the Z-order matches before using the prefetched
  data. If it doesn't match, we fall back to a normal fetch. Correctness
  is guaranteed.
- **Prefetch loop**: The scan for the next valid sibling uses the same
  `checkZOrderInValidMasks` and `calculateSiblingsBeforeThisZOrder`
  functions already used by the descent code.

### Register pressure

- Cache: 3 uints per stack entry × 5 entries = 15 extra registers (the
  `cachedData` field already exists in `CombinedNode64`).
- Prefetch: 3 uints (Tree64NodeData) + 1 uint (prefetchedZOrder) = 4 extra
  registers.
- Total: ~19 extra registers.

**Note**: Previous investigation found that register pressure can have
paradoxical effects on this GPU (mode 9 showed high register pressure
accidentally improved performance via occupancy reduction → better cache).
The extra registers from cache+prefetch may actually HELP on Apple Silicon
while reducing fetches. Measure carefully.

## How to test

1. Change the include in `albedo.frag` to
   `#include <pjv_utils_DDA_cacheprefetch.sc>`.
2. Run `bash compFast.sh`.
3. Run `./docs/examples/terrain_generator/terrain_generator`.
4. **Image must be identical to baseline.**
5. Compare frame time to baseline (both with and without fetchVoxelData).

## Expected impact

- **Dense scenes**: cache eliminates pop re-fetches (happens every pop,
  regardless of density). Prefetch eliminates some descent fetches (when
  the ray steps to the predicted sibling). Expected 30–50% fetch reduction.
- **Sparse scenes**: same cache benefit, plus the forward mask (if combined)
  skips empty nodes. Expected 40–60% fetch reduction.
- **The cache alone** (Part 1) is a 3-line change and should be tested
  first. The prefetch (Part 2) is more complex and should be tested
  separately to measure its incremental benefit.

## What to report back

- Frame time for cache-only (Part 1) vs baseline.
- Frame time for cache+prefetch (Part 1+2) vs cache-only.
- Whether the image is identical to baseline.
- Whether the prefetch hit rate is high (add a debug counter if possible).
- Any register pressure effects (compare to the mode 9 investigation).
