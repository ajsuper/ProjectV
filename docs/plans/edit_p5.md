# Phase 5 — Incremental GPU Upload

## Goal

Replace the full-rebuild `rebuildSceneTextures` (which destroys + recreates the
tree64 / voxelType / header textures and repacks every live blob on every edit)
with an incremental upload that reuses the persistent `RangeAllocator` /
`GPUBlobRange` state already living in `GPUData` and only touches the blobs /
header rows that actually changed since the last flush.

The COW editing model (P1–P4) guarantees content never mutates in place: every
edit forks a new pool blob (`forkBlob`) or interns a new one
(`internChunkGeometry`). So the "dirty set" after `updateScene` is exactly the
set of pool blobs that are live (`refCount > 0`) but either have no GPU range
yet or landed in a recycled slot whose stale range no longer fits. This makes
incremental upload straightforward: **new blobs get a range + upload; changed
chunk headers get a row rewrite; everything else is untouched.**

The three small scene tables (gridInfo / cellMap / looseList) stay on the full-
rebuild path in `syncSceneTables` — they are tiny (a few KB) and not the perf
problem P5 is solving.

## What already exists (P4 laid the groundwork)

- `RangeAllocator` (`include/graphics/range_allocator.h`) — first-fit free-list
  suballocator, pure CPU, `alloc/free/reserve/largestFreeRun`. Designed exactly
  for this.
- `GPUData` persistent fields (`include/data_structures/gpuData.h`):
  `tree64Alloc`, `voxelTypeAlloc`, `blobRanges`, `tree64Width/Height`,
  `voxelTypeWidth/Height`, `headerCapacity`, `looseCount/Capacity`.
- `GPUBlobRange` — per-blob GPU location + `uploaded` flag.
- `buildDataAndHeaderTextures` already seeds the allocators after the bulk pack
  (`tree64Alloc.reset(reserve(used))`) — so after `createTexturesForScene`, the
  layout state is live and incremental calls can use it.
- `bgfx::updateTexture2D` is already used in `setTextureToData`.

## What's missing

1. A dirty signal: no way to know which blobs are new/changed after `updateScene`.
2. An incremental upload path: `rebuildSceneTextures` unconditionally calls the
   full bulk `buildDataAndHeaderTextures`.
3. Texture-grow fallback: when an incremental alloc fails (allocator full), there
   is no "recreate + repack" escape hatch.
4. Header-grow fallback: when `scene.chunks.size()` exceeds `headerCapacity`,
   the header texture must be recreated wider.

## Sub-phases

### P5.1 — Dirty tracking on `GeometryBlob`

**File:** `include/data_structures/scene.h`

Add a `bool dirty = false;` field to `GeometryBlob`:

```cpp
struct GeometryBlob {
    std::vector<uint32_t> geometry;
    std::vector<uint32_t> voxelTypeData;
    std::string sourceDataPath;
    core::ivec3 sourceBlockCoord = core::ivec3(0);
    bool ownsSourceFile = true;
    uint32_t refCount = 0;
    bool dirty = false;   // P5: blob's GPU range is stale or unallocated; cleared by flushSceneUpdates.
};
```

Why a field on the blob (not a parallel vector in `GPUData`): the editing code
already touches `GeometryBlob` directly when it forks/interns, so setting the
flag at the source is one line and impossible to forget. A parallel vector
would need to track pool-slot reuse (`blobFreeList`), which is more fragile.

**File:** `include/data_structures/scene.h` — `forkBlob`

`forkBlob` deep-copies the blob; the fork is new and needs upload. Mark it:

```cpp
inline int32_t forkBlob(Scene& scene, int32_t srcIndex) {
    ...
    GeometryBlob fork = scene.geometryPool[srcIndex];
    fork.refCount = 1;
    fork.dirty = true;       // P5: new blob, no GPU range yet.
    return poolInsertBlob(scene, std::move(fork));
}
```

Note: the *source* blob is NOT marked dirty — its content and GPU range are
unchanged (other chunks still share it).

**File:** `include/data_structures/scene.h` — `internChunkGeometry`

A freshly interned blob is new to the pool:

```cpp
inline int32_t internChunkGeometry(Scene& scene, Chunk& chunk) {
    if (chunk.geometryPoolIndex >= 0) return chunk.geometryPoolIndex;
    GeometryBlob blob;
    blob.geometry = std::move(chunk.geometryData);
    blob.voxelTypeData = std::move(chunk.voxelTypeData);
    blob.refCount = 1;
    blob.dirty = true;       // P5
    chunk.geometryData.clear();
    chunk.voxelTypeData.clear();
    chunk.geometryPoolIndex = poolInsertBlob(scene, std::move(blob));
    return chunk.geometryPoolIndex;
}
```

**File:** `src/utils/editing.cpp` — `applyEditsToChunk` and the Chunk/Grid paths

These already write `fork.geometry = chunk.geometryData` / `fork.voxelTypeData`
after rebuilding. Since the fork was already marked dirty in `forkBlob`, no
extra mark is needed here — but add an assert / comment that the blob is dirty
so future maintainers don't add an in-place content mutation without setting it.

No change needed in `editing.cpp` for dirty flags (they come from
`forkBlob`/`internChunkGeometry`). The editing code is unchanged.

### P5.2 — Incremental blob upload

**File:** `src/graphics/gpu_interface.cpp` — new `static void uploadDirtyBlobs(Scene&, GPUData&)`

Core algorithm (called from the new public `flushSceneUpdates`):

```
for each index b in scene.geometryPool:
    GeometryBlob& blob = scene.geometryPool[b];
    GPUBlobRange&  r    = gpuData.blobRanges[b];   // grow blobRanges if b >= size

    if blob.refCount == 0 && r.uploaded:
        // Eviction path (not triggered by editing yet, but correct to have):
        // free GPU ranges, mark slot for recycle.
        tree64Alloc.free(r.geomTexelOffset, r.geomTexelLen);
        voxelTypeAlloc.free(r.typeTexelOffset, r.typeTexelLen);
        r.uploaded = false;
        continue;

    if blob.refCount == 0:
        continue;   // empty slot, nothing to do

    if !blob.dirty:
        continue;   // unchanged — its existing GPU range is still valid

    // --- This blob needs upload. Compute new sizes. ---
    uint32_t nodes      = blob.geometry.size() / 3;
    uint32_t typeTexels = (blob.voxelTypeData.size() + 3) / 4;
    uint32_t typeUints  = blob.voxelTypeData.size();

    // Does the existing range still fit? (handles recycled-slot reuse where
    // uploaded==true but the range belongs to the dead blob that lived here.)
    bool needRealloc = !r.uploaded
                   || r.geomTexelLen < nodes
                   || r.typeTexelLen  < typeTexels;

    if needRealloc:
        if r.uploaded:
            tree64Alloc.free(r.geomTexelOffset, r.geomTexelLen);
            voxelTypeAlloc.free(r.typeTexelOffset, r.typeTexelLen);
        uint32_t gOff = tree64Alloc.alloc(nodes);
        uint32_t tOff = voxelTypeAlloc.alloc(typeTexels);
        if gOff == INVALID || tOff == INVALID:
            // --- Fallback: grow textures + repack all live blobs. ---
            growDataTextures(scene, gpuData);   // see P5.4
            // After grow, re-allocate (now guaranteed to fit).
            gOff = tree64Alloc.alloc(nodes);
            tOff = voxelTypeAlloc.alloc(typeTexels);
        r = GPUBlobRange{ gOff, nodes, tOff, typeTexels, typeUints, true };
    else:
        // Same range, just refresh the stored counts (content changed in place
        // of the allocated span — currently never happens under COW, but cheap
        // and future-proof).
        r.geomTexelLen = nodes;
        r.typeUintLen   = typeUints;

    // --- Upload texels into the assigned region. ---
    // tree64 texture: update a w×h sub-rect starting at the 2D address of gOff.
    auto [gx, gy] = texelTo2D(r.geomTexelOffset, gpuData.tree64Width);
    auto [gw, gh] = nodeSpanTo2D(nodes, gpuData.tree64Width);
    bgfx::updateTexture2D(gpuData.tree64Texture, 0, 0, gx, gy, gw, gh,
                          bgfx::copy(packGeometryTexels(blob.geometry)));
    // voxelType texture: same idea for typeTexels.
    auto [tx, ty] = texelTo2D(r.typeTexelOffset, gpuData.voxelTypeWidth);
    auto [tw, th] = nodeSpanTo2D(typeTexels, gpuData.voxelTypeWidth);
    bgfx::updateTexture2D(gpuData.voxelTypeDataTexture, 0, 0, tx, ty, tw, th,
                          bgfx::copy(packVoxelTypeTexels(blob.voxelTypeData)));

    blob.dirty = false;
```

Notes:
- `texelTo2D(offset, width)` = `{ offset % width, offset / width }`.
- `nodeSpanTo2D(count, width)` computes the 2D footprint of a contiguous
  `count`-texel run starting at column 0 of a `width`-wide texture: height =
  `ceil(count / width)`, width = `min(count, width)` for the first row and full
  width thereafter. Since `updateTexture2D` takes a rectangular region, a run
  that wraps across rows must be uploaded as a `w × h` rectangle covering the
  whole span (the trailing texels of the last row are harmless padding as long
  as they don't clobber a neighbour — they won't, because the allocator reserved
  exactly `[offset, offset+count)` and any padding lands in free space or the
  blob's own slack). If the run doesn't wrap (count <= remaining column space),
  it's a single `count × 1` rect.
- `packGeometryTexels` / `packVoxelTypeTexels` already exist; reuse them.

### P5.3 — Incremental header update

**File:** `src/graphics/gpu_interface.cpp` — new `static void updateDirtyHeaders(Scene&, GPUData&)`

After `uploadDirtyBlobs`, some chunks now point at new pool indices (COW fork
repointed `chunk.geometryPoolIndex`) or are brand-new (grid cell creation). For
each such chunk, rewrite its 4-texel header row.

Dirty-header detection: a chunk's header is stale if its `geometryPoolIndex`
changed, if it's new, or if its pool blob's range changed. Simplest signal: in
`uploadDirtyBlobs`, collect a `std::vector<uint32_t> dirtyChunkHandles` — every
chunk whose `geometryPoolIndex` is a blob that was (re)allocated this flush.
Then:

```
for ChunkHandle h : dirtyChunkHandles:
    if h >= headerCapacity:
        // --- Fallback: grow header texture. ---
        growHeaderTexture(scene, gpuData);   // see P5.4
        break;   // grow rewrites ALL headers, so we're done.
    const Chunk& c = scene.chunks[h];
    GPUChunkHeader hdr = c.alive && c.geometryPoolIndex >= 0
        ? makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene)
        : degenerateHeader();
    // Header texture is a 1-row RGBA32U texture of (headerCapacity*4) texels.
    // One header = 4 contiguous texels starting at h*4.
    bgfx::updateTexture2D(gpuData.headerTexture, 0, 0,
                          h * 4, 0, 4, 1,
                          bgfx::copy(&hdr, sizeof(hdr)));
```

Collecting `dirtyChunkHandles`: iterate `scene.chunks`; a chunk is dirty if its
`geometryPoolIndex` points at a blob that was dirty this flush, OR the chunk is
new since last flush (track via a `gpuData.uploadedChunkCount` watermark: any
chunk handle `>= uploadedChunkCount` is new). Add this watermark to `GPUData`.

### P5.4 — Texture-grow fallbacks

**File:** `src/graphics/gpu_interface.cpp`

`growDataTextures(Scene&, GPUData&)` — the "sporadic" full repack for when an
allocator can't satisfy a request. This is essentially the current
`buildDataAndHeaderTextures` body, but:
- It does NOT destroy+recreate the textures from scratch blindly; it grows them
  (new dims = `withHeadroom(totalUsed)`), recreates, and repacks all live blobs
  into fresh contiguous ranges, reseeding the allocators.
- It marks every live blob's `blobRanges` as freshly allocated and uploads the
  full packed buffers via `createTexture2D` (one-shot, same as today).
- After grow, `dirty` flags on all live blobs are cleared (they're now uploaded).
- Only blobs that were already uploaded keep their content; blobs that were
  dirty and triggered the grow get their latest content included in the repack.

This is the rare slow path. Frequency is bounded by `withHeadroom` slack — a
grow doubles available space, so amortized O(1) grows per edit.

`growHeaderTexture(Scene&, GPUData&)` — analogous: pick a new
`headerCapacity = withHeadroom(scene.chunks.size())` (capped at `maxSlots`),
recreate the header texture, rewrite all rows from current `scene.chunks`.

### P5.5 — Public API

**File:** `include/graphics/gpu_interface.h` + `src/graphics/gpu_interface.cpp`

```cpp
/**
 * Incremental GPU update after `utils::updateScene`. Uploads only pool blobs
 * flagged dirty (new forks / interned chunks) and rewrites only the affected
 * header rows, reusing the persistent RangeAllocator / GPUBlobRange state in
 * gpuData. Grows the data / header textures only when an allocator is full
 * (rare, amortized O(1)). The small scene tables (gridInfo / cellMap /
 * looseList) are still rebuilt in full by syncSceneTables (they are tiny).
 *
 * First-time setup is still done by `createTexturesForScene` (which seeds the
 * layout state); subsequent edits call this instead of `rebuildSceneTextures`.
 */
void flushSceneUpdates(Scene& scene, GPUData& gpuData);
```

Implementation:

```cpp
void flushSceneUpdates(Scene& scene, GPUData& gpuData) {
    std::vector<uint32_t> dirtyChunks;
    uploadDirtyBlobs(scene, gpuData, dirtyChunks);     // P5.2
    updateDirtyHeaders(scene, gpuData, dirtyChunks);   // P5.3
    syncSceneTables(scene, gpuData);                   // unchanged (small textures)
}
```

Keep `rebuildSceneTextures` as a documented fallback (force-full-rebuild, e.g.
for debugging or after a scene reload that bypassed `updateScene`).

Add `uint32_t uploadedChunkCount = 0;` to `GPUData` (in `gpuData.h`) as the
new-chunk watermark for P5.3.

### P5.6 — Call-site update

**File:** `docs/examples/edit_demo/main.cpp`

Replace the two `rebuildSceneTextures(scene, gpuData)` calls (after
`updateScene`) with `flushSceneUpdates(scene, gpuData)`. This is the visual
verification point: edits must still appear correctly and performance should
improve (no full texture recreate per voxel).

`docs/examples/editing_p1/` — CPU-only, no GPU calls; no change needed.

`docs/examples/PathTracer/` — uses `createTexturesForScene` only (no edits);
no change needed.

### P5.7 — Headless test coverage

**File:** `docs/examples/editing_p1/main.cpp` (or a new `editing_p5` driver)

The `RangeAllocator` and the dirty-tracking logic are pure CPU, so they can be
unit-tested headlessly (no bgfx context):

1. **Dirty-flag propagation**: after `queueVoxelAdd` + `updateScene` on a loose
   chunk, assert the forked blob (`scene.geometryPool[chunk.geometryPoolIndex]`)
   has `dirty == true` and the original blob has `dirty == false`.
2. **RangeAllocator alloc/free/coalesce**: seed a `RangeAllocator(100)`, alloc
   `[0,30)`, `[30,40)`, free `[0,30)` → assert `largestFreeRun() >= 30` and a
   subsequent `alloc(30)` returns 0 (coalesced head).
3. **Recycled-slot reuse**: free a blob (refCount → 0) to return its slot to
   `blobFreeList`, fork a new blob (reuses the slot), assert the new blob is
   `dirty == true` and its old `blobRanges` entry has `uploaded == true` with a
   size that no longer matches (so `uploadDirtyBlobs` will reallocate).

The GPU-side `updateTexture2D` calls can only be exercised by `edit_demo`
(manual visual check) — no automated GPU readback test is added in P5.

## Risks / open questions

1. **2D sub-rect upload for wrapped runs.** `bgfx::updateTexture2D` takes a
   rectangular region; a contiguous texel run that wraps across texture rows
   must be uploaded as a `width × height` rectangle. If the run's start column
   + count exceeds the row width, the upload rect must start at column 0 of the
   start row and span `ceil(count_from_start_col / width)` rows. The trailing
   texels of the last row land in the allocator's free slack (harmless). Need to
   verify bgfx accepts a rect that extends into unallocated-yet texture space
   (it should — the texture is fully allocated, only the suballocator's free
   list is logical). **Mitigation**: if this proves fragile, allocate each blob
   in row-aligned chunks (round `geomTexelOffset` up to a row boundary). Wastes
   at most one row per blob; simplest to reason about.

2. **Header texture is 1-row.** `updateTexture2D` on a 1-row texture with `y=0,
   h=1` is the natural case — no wrapping concern. Good.

3. **`syncSceneTables` still destroys+recreates 3 textures per edit.** This is
   fine for P5 (tiny textures). If profiling later shows it matters, P6+ can
   incrementalize them too. Out of scope here.

4. **`poolInsertBlob` reuse + `blobRanges` size.** When `blobFreeList` reuses a
   slot, `geometryPool` size doesn't grow, but `blobRanges` must be at least as
   large. `uploadDirtyBlobs` must `blobRanges.resize(scene.geometryPool.size())`
   on entry (default-constructed `GPUBlobRange` has `uploaded=false` — correct
   for a recycled slot).

5. **`chunkQueue` (legacy edit staging on `Chunk`) is still present.** P5 does
   not remove it (that's a P6+ cleanup). It's unused by the P1–P4 editing path.
   No interaction with incremental upload.

## File change summary

| File | Change |
|------|--------|
| `include/data_structures/scene.h` | Add `bool dirty` to `GeometryBlob`; set it in `forkBlob` + `internChunkGeometry`. |
| `include/data_structures/gpuData.h` | Add `uint32_t uploadedChunkCount = 0;` to `GPUData`. |
| `include/graphics/gpu_interface.h` | Declare `flushSceneUpdates`. |
| `src/graphics/gpu_interface.cpp` | Add `uploadDirtyBlobs`, `updateDirtyHeaders`, `growDataTextures`, `growHeaderTexture`, `flushSceneUpdates`. Implement `flushSceneUpdates`. Keep `rebuildSceneTextures` as fallback. |
| `docs/examples/edit_demo/main.cpp` | Call `flushSceneUpdates` instead of `rebuildSceneTextures`. |
| `docs/examples/editing_p1/main.cpp` (or new `editing_p5`) | Add headless dirty-flag + RangeAllocator assertions. |
| `AGENTS.md` | Update Phase 5 status. |

## Definition of done

- `flushSceneUpdates` uploads only dirty blobs and rewrites only changed
  header rows; the tree64 / voxelType textures are never destroyed+recreated
  unless an allocator is full (grow fallback).
- `edit_demo` visual output is identical before/after the switch from
  `rebuildSceneTextures` to `flushSceneUpdates`.
- A single-voxel edit does not reallocate or re-upload untouched blobs (verify
  via the `[PERF]` log lines: `uploadDirtyBlobs` reports `dirtyBlobs=1` while
  `buildDataAndHeaderTextures` reported `liveBlobs=N`).
- Headless dirty-flag + RangeAllocator assertions pass.
- Allocator-grow fallback is exercised at least once in a stress test (many
  edits that exhaust headroom) and produces correct output.
