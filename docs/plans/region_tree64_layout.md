# Region-Based Tree64 Texture Layout

**Date:** 2026-07-15
**Status:** planned

Replace the 1D linear tree64 texture allocator with a column-based region allocator
where each blob occupies a fixed-width (64 texel) column of dynamic height. Blob
nodes wrap at column boundaries instead of texture boundaries, making the 2D
footprint compact and encouraging GPU texture cache locality.

## Motivation

Currently, each tree64 blob is serialized as a 1D strip in the global texture. A
5000-node blob spans 5000 texels linearly (one very long row). When the traversal
jumps between tree levels, it fetches nodes that may be thousands of texels apart.

With a 64-wide column, the same blob is roughly 64×79 — a compact rectangle.
"Near in index" ≈ "near in 2D texture space," so GPU texture cache swizzling
finds nodes from the same blobs in fewer cache tiles.

### Waste comparison

| Blob size | Current (25% + 64 padding) | Region-based (row remainder) |
|-----------|---------------------------|------------------------------|
| 500 nodes | 689 allocated, 189 waste | 512 allocated, 12 waste |
| 5000 nodes | 6314 allocated, 1314 waste | 5056 allocated, 56 waste |
| 50000 nodes | 62564 allocated, 12564 waste | 50048 allocated, 48 waste |

## Design

### Column width
- **64 texels** (power of 2, tuned for typical blob sizes of hundreds to low thousands)
- Texture width is always a multiple of 64 (enforced by chooseDataDims)
- Column layout: `64 × N` stripes stacked across the texture

### Shader addressing
`tree64Dims` repurposes unused channels:
```
vec4(textureWidth, log2(textureWidth), columnWidth, log2(columnWidth))
```

`tree64()` rewritten to accept local node index + global base offset:
```glsl
Tree64NodeData tree64(uint localNodeIndex, uint globalBaseOffset) {
    uint texW  = uint(tree64Dims.x);   // texture width (power of 2)
    uint texSh = uint(tree64Dims.y);   // log2(texture width)
    uint colW  = uint(tree64Dims.z);   // column width (64)
    uint colSh = uint(tree64Dims.w);   // log2(column width)

    uint localX  = localNodeIndex & (colW - 1u);
    uint localY  = localNodeIndex >> colSh;
    uint globalIdx = globalBaseOffset + localX + localY * texW;

    int x = int(globalIdx & (texW - 1u));
    int y = int(globalIdx >> texSh);
    uvec4 pixel = texelFetch(tree64Data, ivec2(x, y), 0);
    // unpack as before
}
```

### Traversal
All `dataIndex` values on the node stack become LOCAL node indices. Root is at
local index 0. `childPtr` (node count in serialized sequence) works identically
as a local offset. The traversal parameter changes from `geometryStartIndex +
nodeCount` to `globalBaseOffset + nodeCount`.

### GPUChunkHeader — no layout change
- `geometryStartIndex` — **global base offset** (linear texel index of blob rectangle top-left)
- `geometryEndIndex` — **node count** (was `offset + count`, now just `count`)

### No per-blob headroom (Phase 1)
Each blob gets exactly `ceil(nodes / 64)` rows. Growth triggers a move + new
column allocation. COW edit pattern already creates new blobs, so in-place
growth is rare. If it becomes a perf issue, add +1 row headroom later.

## New Files

### `include/graphics/column_allocator.h`
```cpp
struct ColumnAllocator {
    static constexpr uint32_t COL_WIDTH = 64;
    static constexpr uint32_t INVALID = 0xFFFFFFFFu;

    uint32_t numColumns = 0;     // derived from texture width / 64
    uint32_t texHeight = 0;      // texture height (e.g. 4096)

    struct Entry {
        uint32_t blobID;         // pool index for bookkeeping
        uint32_t startRow;       // row where this blob starts
        uint32_t nodeCount;      // nodes in this blob
    };
    std::vector<std::vector<Entry>> columns;   // stack per column (bottom-up)
    std::vector<uint32_t> nextFreeRow;         // high-water mark per column

    void reset(uint32_t texW, uint32_t texH);
    // Returns global linear texel offset, or INVALID if full
    uint32_t alloc(uint32_t nodes, uint32_t blobID);
    void free(uint32_t globalOffset, uint32_t nodes);
    void reserve(uint32_t globalOffset, uint32_t nodes); // seed from repack
};
```

## Modified Files

### `include/data_structures/gpuData.h`
- `GPUBlobRange`: drop `geomTexelAllocated`, `typeTexelAllocated`
- `GPUData`: `tree64Alloc` type from `RangeAllocator` → `ColumnAllocator`
  (voxelType alloc stays RangeAllocator — no traversal benefit)

### `src/graphics/gpu_interface.cpp`
- `chooseDataDims`: width constrained to `COL_WIDTH * nextPowerOfTwo(...)`
- `uploadTexelSpan`: column-aware upload — one rect per blob row, no row-wrapping
- `growDataTextures`: column-based packing, no `paddedAlloc`
- `uploadDirtyBlobs`: column-based allocation, free old / alloc new on growth
- `buildDataAndHeaderTextures`: column-based packing for bulk build
- `makeHeader`: `geometryEndIndex = geomTexelLen` (node count, not offset+len)
- `createTexturesForScene`: adapt to column allocator seeding
- `rebuildSceneTextures`: adapt to column allocator reset

### `src/graphics/perform_renderer.cpp`
- `tree64Dims` uniform set: populate `.z` (64) and `.w` (6) with column dims

### `include/pjv_utils_DDA*.sc` (8 shader variants)
- `tree64()`: add `uint globalBaseOffset` parameter, column-aware address calc
- `tree64s()`: same treatment for the low-level uint reader
- `marchRayThroughTree64_DDA`: rename params (`tree64StartIndex` → `globalBaseOffset`,
  `tree64EndIndex` → `nodeCount`), all `tree64()` calls pass base offset
- `castRayThroughTree64`: pass `geometryStartIndex` as base offset,
  `geometryEndIndex` as node count
- No bounding-check changes needed (`tree64EndIndex` was already unused in traversal body)

## Vertex

- Column allocator is header-only — no new `.cpp`
- VoxelType texture pipeline unchanged (linear allocator, separate problem)
- All existing P5/P6 tests should pass (voxel counts, refCounts, positions unchanged)
- Shader compilation verified via `compTree64.sh` or equivalent
- `geometryStartIndex`/`geometryEndIndex` field-semantics change: audit all
  CPU-side readers (makeHeader, any logging/debug code)