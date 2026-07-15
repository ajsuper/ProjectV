# Plan 08: Cache Parent Node Data in nodeStack (Eliminate Redundant Texture Fetch on Pop)

## Objective

Every time the DDA pops up from a child node (boundary crossing), it re-fetches
the parent's data from the tree64Data texture (line 1043):
```glsl
data = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
```

This data was **already fetched** when we first descended into this level.
The `CombinedNode64` struct already has a `cachedData` field (line 65) that
is **never populated** — the comment literally says "This line is the issue."

Caching the fetched data in the stack eliminates this texture fetch on every
pop. This directly reduces texture fetches, which is the actual bottleneck.

## Why this works

The `nodeStack` has 5 entries. Each entry represents a level in the tree.
When we descend, we fetch the child's data and store it as `data` (the parent
for the next level). When we pop back up, we need the parent's data again —
but we already fetched it and could have cached it.

Trace:
1. Descend: fetch child data → `data = tree64(...)` → **cache in stack**
2. DDA step through child's cells
3. Boundary crossing → pop → **restore from cache instead of fetching**

The first fetch is always needed. The optimization eliminates the **re-fetch
on subsequent visits** (via pop).

## Instructions

1. **Copy** `include/pjv_utils_DDA.sc` to
   `include/pjv_utils_DDA_cachedstack.sc`. Make ALL changes only in the copy.

2. **At initialization** (line 936, after fetching root data):
   ```glsl
   data = tree64(tree64StartIndex);
   nodeStack[0].cachedData = data;  // Cache root data
   ```

3. **At descent** (line 987, after fetching child data):
   ```glsl
   data = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
   nodeStack[nodeStackQuantity - 2u].cachedData = data;  // Cache for later pop
   ```

4. **At pop** (line 1043, replace the texture fetch with a cache read):
   ```glsl
   // BEFORE:
   data = tree64(nodeStack[nodeStackQuantity - 2u].dataIndex);
   // AFTER:
   data = nodeStack[nodeStackQuantity - 2u].cachedData;
   ```

5. **That's it.** Three changes: cache at init, cache at descent, restore at pop.

## Correctness

The cache is valid because:
- Each stack entry's `dataIndex` doesn't change while we're deeper in the
  stack (we only modify the current top entry's dataIndex during descent).
- When we pop back to a level, the cachedData at that level is still the
  data we fetched when we first descended there.
- Tree64 data is immutable during a single march (no edits mid-frame).

## Cost

- **Extra registers**: 3 uints per stack entry × 5 entries = 15 extra registers.
  The `cachedData` field already exists in the struct, so the struct size
  doesn't change — but the compiler now needs to actually populate/read it,
  which may increase register pressure.
- **Saved fetches**: 1 texture fetch per pop (every boundary crossing).

## How to test

1. Change the include in `albedo.frag` to
   `#include <pjv_utils_DDA_cachedstack.sc>`.
2. Run `bash compFast.sh`.
3. Run `./docs/examples/terrain_generator/terrain_generator`.
4. **Image must be identical to baseline** — the cached data is the same
   data that would be fetched.
5. Compare frame time to baseline (both with and without fetchVoxelData).

## What to report back

- Confirmation that the image is identical to baseline.
- Frame time comparison vs baseline.
- Whether the extra register pressure (15 registers) caused any regression
  (remember: previous investigation found that register pressure can have
  paradoxical effects on this GPU — measure carefully).
