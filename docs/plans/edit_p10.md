# Phase 10 — Per-Voxel Materials (Remove voxelTypeData, Embeds into tree64)

## Goal

Replace the old `voxelTypeData` system (3× uint32_t per voxel, sorted by Z-order,
GPU binary search on traversal) with a material-ID system: **1 byte per voxel**,
laid out contiguously per tree64 leaf node in traversal order, referenced by
material offset stored in the leaf node's `pointerAndLeafFlag`. Add a per-blob
material palette (`Material[]`) with persistent indices.

**Savings estimate:** ~12:1 memory ratio (12 bytes/voxel → 1 byte/voxel for the
payload) plus elimination of GPU binary search (O(log N) → O(1) popcount).

## What exists (before P10)

- `Chunk::voxelTypeData` / `GeometryBlob::voxelTypeData` — `std::vector<uint32_t>`,
  three words per voxel: `[ZOrderPosition, packedColor, packedNormal]`.
- `buildVoxelTypeDataFromBrickMapFast` — iterates brick bitmasks, emits triplets
  sorted by Z-order.
- `brickMapSetVoxel` — stores color in `BrickData::colors[localZOrder]` as packed
  RGBA (`uint32_t`).
- `brickMapGetColor` — looks up the brick's `colors` map.
- GPU traversal (`pjv_utils_DDA.sc`): `tree64Walk` reaches a leaf, then does a
  binary search on the voxelTypeData texture array by Z-order to find the color.
- `GPUChunkHeader` has `voxelTypeDataStartIndex` / `voxelTypeDataEndIndex` fields
  pointing into the voxelType texture.
- `GeometryBlob::brickMap` — `VoxelBrickMap` with `BrickData::colors` map (sparse
  `unordered_map<uint32_t, uint32_t>`, where the `uint32_t` value is a packed color).

## What's missing

1. A `Material` struct and per-blob palette in `GeometryBlob`.
2. `uint8_t materialID` per voxel in `BrickData` instead of `uint32_t packedColor`.
3. A contiguous `materialIDs` array per blob, populated in traversal order (matching
   leaf-node mask bit order).
4. Leaf-node `pointerAndLeafFlag` repurposed: when bit 0 = 1 (leaf flag), bits 1-31
   are an offset into `materialIDs` instead of the old zero.
5. `Material` palette — deduplicated list of materials with persistent indices.
6. GPU shader changes: remove binary search, add `materialIDs` texture and
   `materialPalette` buffer, use popcount to index into per-leaf material runs.
7. Remove `voxelTypeData` from `GeometryBlob`, `GPUChunkHeader`, and all related
   GPU infrastructure.

## Sub-phases

### P10.1 — Material type + palette (scene.h, material.h)

**Files:** `include/data_structures/scene.h`, `include/utils/material.h`

**P10.1a — Define `Material` struct**

Create `include/utils/material.h`:

```cpp
#ifndef PROJECTV_MATERIAL_H
#define PROJECTV_MATERIAL_H

#include <cstdint>
#include <vector>
#include <unordered_map>

namespace projv {

constexpr uint32_t MAX_MATERIALS_PER_BLOB = 256u;

struct Material {
    // Packed R10G10B10A2 color format (same as old packColor output).
    // A=3 for fully opaque.
    uint32_t packedColor = 0;

    // Future fields added here — floats for smoothness, metalness, etc.
    // Keep total size a power-of-two for GPU buffer alignment.
    uint32_t _pad = 0;
};

static_assert(sizeof(Material) == 8, "Material must be 8 bytes");

} // namespace projv

#endif
```

**P10.1b — Add `Material` palette to `GeometryBlob`**

In `include/data_structures/scene.h`, inside `GeometryBlob`:

```cpp
struct GeometryBlob {
    std::vector<uint32_t> geometry;            // tree64 nodes (3 u32s each)
    std::vector<uint8_t>  materialIDs;         // REPLACES voxelTypeData — one byte per voxel
    std::vector<Material> materialPalette;     // NEW — material definitions

    // Remove:
    // std::vector<uint32_t> voxelTypeData;

    std::unique_ptr<VoxelBrickMap> brickMap;
    std::string sourceDataPath;
    core::ivec3 sourceBlockCoord;
    bool ownsSourceFile = true;
    uint32_t refCount = 0;
    bool dirty = false;
};
```

**P10.1c — Remove `voxelTypeData` from `Chunk`**

In the `Chunk` struct, delete the `voxelTypeData` field:

```cpp
struct Chunk {
    ChunkHeader header;
    std::vector<uint32_t> geometryData;      // tree64 serialized nodes
    // DELETE: std::vector<uint32_t> voxelTypeData;
    uint32_t LOD;
    int32_t geometryPoolIndex;
    // ...
};
```

**P10.1d — Remove `voxelTypeDataStartIndex` / `voxelTypeDataEndIndex` from `GPUChunkHeader`**

```cpp
struct GPUChunkHeader {
    uint32_t chunkID;
    float positionX, positionY, positionZ;
    float scale;
    uint32_t resolution;
    uint32_t geometryStartIndex;
    uint32_t geometryEndIndex;
    // DELETE: uint32_t voxelTypeDataStartIndex;
    // DELETE: uint32_t voxelTypeDataEndIndex;
    uint32_t dataRefID;
    uint32_t padding[2];
    float rotationX, rotationY, rotationZ, rotationW;
};
```

Make sure the total struct size is unchanged (or padded correctly) to avoid
breaking the GPU texture row layout.

**P10.1e — Delete `_pad` from old header**

The old header replaced `padding[2]` (newer layout) with `dataRefID; padding[1]`.
Now we have two padding words again (or one, if we add a `materialPaletteStartIndex`
— see P10.5). Keep it simple for now: remove `voxelTypeDataStart/EndIndex`, put
back `padding[2]`.

```
Old layout (4 u32s header data + 1 u32 dataRefID + 1 u32 padding = 8 u32s):
  chunkID, posX/Y/Z, scale, resolution, geomStart, geomEnd, vTypeStart, vTypeEnd,
  dataRefID, padding[1], rotX/Y/Z/W  →  16 u32s

New layout (remove vTypeStart/vTypeEnd, add padding back):
  chunkID, posX/Y/Z, scale, resolution, geomStart, geomEnd, padding[2],
  dataRefID, padding[2], rotX/Y/Z/W  →  same 16 u32s
```

Actually, the current layout from the codebase (post-P4) is:

```
chunkID, posX/Y/Z, scale, resolution, geomStart, geomEnd, vTypeStart, vTypeEnd,
dataRefID, padding[1], rotX/Y/Z/W   = 15 u32s
```

New layout without voxelType fields:

```
chunkID, posX/Y/Z, scale, resolution, geomStart, geomEnd, padding[3],
dataRefID, rotX/Y/Z/W   = 14 u32s
```

Or we keep 15 u32s by adding `materialPaletteStartIndex` — see P10.5.

### P10.2 — Material API (material.h, material.cpp)

**Files:** `include/utils/material.h`, `src/utils/material.cpp`

**P10.2a — Palette management functions**

```cpp
namespace projv::utils {

// Ensure a material with the given name and color exists in the palette.
// Returns its index (0-255). Reuses existing entry if name already in palette.
// name can be empty — then matches by color only.
uint8_t internMaterial(GeometryBlob& blob, const std::string& name, uint32_t packedColor);

// Find material by name or color. Returns INVALID_MATERIAL (== 255) if not found.
uint8_t findMaterialByName(const GeometryBlob& blob, const std::string& name);
uint8_t findMaterialByColor(const GeometryBlob& blob, uint32_t packedColor);

}
```

Implementation of `internMaterial`:

```cpp
uint8_t internMaterial(GeometryBlob& blob, const std::string& name, uint32_t packedColor) {
    // If name is non-empty, check for a match by name first.
    if (!name.empty()) {
        for (size_t i = 0; i < blob.materialPalette.size(); ++i) {
            if (blob.materialPalette[i].name == name) {
                return static_cast<uint8_t>(i);
            }
        }
    }
    // Fallback: match by color.
    for (size_t i = 0; i < blob.materialPalette.size(); ++i) {
        if (blob.materialPalette[i].packedColor == packedColor) {
            return static_cast<uint8_t>(i);
        }
    }
    // Not found — append new material.
    Material mat;
    mat.name = name;
    mat.packedColor = packedColor;
    blob.materialPalette.push_back(mat);
    return static_cast<uint8_t>(blob.materialPalette.size() - 1);
}
```

Note: 255 is reserved for `INVALID_MATERIAL` (max valid index = 254). Guard with
an assert that `blob.materialPalette.size() < 255`.

Implementation of `findMaterialByName` / `findMaterialByColor`:

```cpp
uint8_t findMaterialByName(const GeometryBlob& blob, const std::string& name) {
    for (size_t i = 0; i < blob.materialPalette.size(); ++i) {
        if (blob.materialPalette[i].name == name) {
            return static_cast<uint8_t>(i);
        }
    }
    return 255;  // INVALID_MATERIAL
}

uint8_t findMaterialByColor(const GeometryBlob& blob, uint32_t packedColor) {
    for (size_t i = 0; i < blob.materialPalette.size(); ++i) {
        if (blob.materialPalette[i].packedColor == packedColor) {
            return static_cast<uint8_t>(i);
        }
    }
    return 255;  // INVALID_MATERIAL
}

**P10.2b — Brick map material helpers**

```cpp
// Set material for a single voxel in the brick map (localZOrder → materialID).
void brickMapSetMaterial(BrickData& brick, uint32_t localZOrder, uint8_t materialID);

// Get material for a voxel in the brick map.
// Returns 0 (default material) if the voxel doesn't exist.
uint8_t brickMapGetMaterial(const BrickData& brick, uint32_t localZOrder);
```

Implementation:

```cpp
void brickMapSetMaterial(BrickData& brick, uint32_t localZOrder, uint8_t materialID) {
    brick.materials[localZOrder] = materialID;
}

uint8_t brickMapGetMaterial(const BrickData& brick, uint32_t localZOrder) {
    auto it = brick.materials.find(localZOrder);
    return (it != brick.materials.end()) ? it->second : 0;
}
```

**P10.2c — Update `BrickData` struct**

In `include/data_structures/voxel.h`:

```cpp
struct BrickData {
    uint64_t mask[BRICK_MASK_ROWS] = {};                     // which voxels exist (unchanged)
    std::unordered_map<uint32_t, uint8_t> materials;          // localZOrder → materialID
    // DELETE: std::unordered_map<uint32_t, uint32_t> colors; // no longer needed
};
```

**P10.2d — Update `VoxelBrickMap`**

Remove `defaultNormal` — normals are now part of `Material` for future use, but
for now the material holds only color. The `defaultNormal` is no longer stored.

Actually, keep `defaultNormal` for now — it's harmless and removing it is out of
scope. Just don't reference it from the new material path.

### P10.3 — `bakeMaterials` (voxel_management.cpp)

**Files:** `src/utils/voxel_management.cpp`, `include/utils/voxel_management.h`

**P10.3a — New function: `bakeMaterialsFromBrickMap`**

This function runs AFTER `buildTree64FromBrickMap` and `addPointersTree64` have
produced the final serialized tree64. It makes ONE forward pass over the serialized
geometry, finds every leaf node, and fills the material arrays.

```cpp
void bakeMaterialsFromBrickMap(std::vector<uint32_t>& geometry,
                                GeometryBlob& blob,
                                const VoxelBrickMap& map) {
    std::vector<uint8_t> materialIDs;
    // Estimate: count of populated leaf bits.
    size_t estimatedVoxels = 0;
    for (uint32_t bz = 0; bz < map.totalBricks; ++bz) {
        if (!map.bricks[bz]) continue;
        for (uint32_t row = 0; row < BRICK_MASK_ROWS; ++row) {
            estimatedVoxels += static_cast<size_t>(
                __builtin_popcountll(map.bricks[bz]->mask[row]));
        }
    }
    materialIDs.reserve(estimatedVoxels);

    // Walk the serialized tree64 (root-first, 3 u32s per node).
    size_t nodeCount = geometry.size() / 3;
    for (size_t n = 0; n < nodeCount; ++n) {
        uint32_t ptrFlag = geometry[n * 3 + 2];
        bool isLeaf = (ptrFlag & 1u) != 0;
        if (!isLeaf) continue;

        // This is a leaf node. The material offset goes into ptrFlag bits 1-31.
        uint32_t materialOffset = static_cast<uint32_t>(materialIDs.size());

        // Combined 64-bit mask: mask1 (high 32 bits), mask2 (low 32 bits).
        uint64_t mask1 = geometry[n * 3];
        uint64_t mask2 = geometry[n * 3 + 1];
        uint64_t combinedMask = (mask1 << 32) | mask2;

        // Determine which brick and row this leaf corresponds to.
        // Leaf nodes in the serialized tree64 are in traversal order (root-first).
        // We need to map leaf node → (brickZOrder, row) to look up materials.
        //
        // Strategy: the serialized leaf nodes are in the SAME order that
        // buildTree64FromBrickMap emitted them (descending Z-order of leaf nodes).
        // We iterate bricks in descending Z-order, rows in descending order,
        // and for each populated row, we consume one leaf node from the serialized
        // array.
        //
        // Simpler approach: iterate the leaf nodes in serialization order and
        // track a cursor over (brickIdx, row) in the brick map.

        // ... (cursor logic detailed in P10.3b below) ...

        // Write leaf pointer.
        geometry[n * 3 + 2] = (materialOffset << 1) | 1u;
    }

    blob.materialIDs = std::move(materialIDs);
}
```

**P10.3b — Cursor logic for mapping serialized leaf → brick data**

The serialized tree64 has leaf nodes in the order `buildTree64FromBrickMap`
emitted them: descending Z-order within each brick, bricks in descending Z-order.
So the cursor is:

```cpp
// Before the loop over serialized nodes, set up a cursor.
uint32_t cursorBrick = map.totalBricks;
uint32_t cursorRow = BRICK_MASK_ROWS;  // will decrement before use

// Helper: advance cursor to next populated (brick, row) pair.
auto advanceCursor = [&]() -> bool {
    while (cursorBrick > 0) {
        if (cursorRow > 0) {
            --cursorRow;
        } else {
            --cursorBrick;
            cursorRow = BRICK_MASK_ROWS - 1;
        }
        if (cursorBrick == 0 && cursorRow == 0) {
            // Check this final position before returning false.
        }
        if (cursorBrick < map.totalBricks && map.bricks[cursorBrick] &&
            map.bricks[cursorBrick]->mask[cursorRow] != 0) {
            return true;
        }
        // Continue loop to next position.
    }
    return false;  // exhausted
};
```

Then inside the leaf-node loop:

```cpp
if (!advanceCursor()) break;  // shouldn't happen if nodeCount matches

const BrickData& brick = *map.bricks[cursorBrick];
uint64_t rowBits = brick.mask[cursorRow];
uint64_t combinedMask = (mask1 << 32) | mask2;  // from the leaf node

// Walk bits MSB-first (Z-order 0..63 within the 4x4x4 block).
while (rowBits) {
    int leadingZeros = __builtin_clzll(rowBits);
    uint32_t bitPos = 63 - leadingZeros;
    uint32_t localZOrder = cursorRow * 64 + bitPos;

    auto mit = brick.materials.find(localZOrder);
    uint8_t matID = (mit != brick.materials.end()) ? mit->second : 0;
    materialIDs.push_back(matID);

    rowBits &= ~(1ull << (63 - leadingZeros));
}
```

**IMPORTANT:** The `combinedMask` from the leaf node should equal `rowBits` from
the brick data (they represent the same voxels). Use an assert to verify in debug
builds.

**P10.3c — Update `voxel_management.h` declaration**

```cpp
/**
 * Build materialIDs and materialPalette from a brick map, writing material
 * offsets into the leaf nodes of an already-constructed serialized tree64.
 *
 * @param geometry  Serialized tree64 (output of buildTree64FromBrickMap +
 *                  addPointersTree64). Leaf nodes' pointerAndLeafFlag will be
 *                  overwritten with (materialOffset << 1) | 1.
 * @param blob      Output blob receiving materialIDs and materialPalette.
 * @param map       Source brick map with per-voxel material IDs.
 */
void bakeMaterialsFromBrickMap(std::vector<uint32_t>& geometry,
                                GeometryBlob& blob,
                                const VoxelBrickMap& map);
```

### P10.4 — Update `updateChunkFromBrickMap` (voxel_management.cpp)

**P10.4a — Rewrite to use new material path**

```cpp
void updateChunkFromBrickMap(Chunk& chunk, const VoxelBrickMap& map) {
    auto t0 = std::chrono::high_resolution_clock::now();

    int resolution = chunk.header.resolution;
    if (resolution == 0) {
        resolution = static_cast<int>(map.brickDims.x * BRICK_SIZE);
    }

    auto t1 = std::chrono::high_resolution_clock::now();

    // Step 1: Build tree64 from brick bitmasks directly.
    chunk.geometryData = buildTree64FromBrickMap(map, resolution);

    // Step 2: Produce a temporary blob for baking.
    // (In the normal flow, internChunkGeometry moves geometryData into a blob.
    //  We need a blob here to hold materialPalette and materialIDs.)
    // For the updateChunkFromBrickMap path, we just fill chunk-level fields
    // and let internChunkGeometry move everything into the blob later.
    //
    // Alternative: pass a temporary GeometryBlob to bakeMaterialsFromBrickMap,
    // then move materialIDs and materialPalette into the chunk (or directly
    // into the blob at intern time).
    //
    // Simplest approach: add materialIDs and materialPalette fields to Chunk
    // (or keep them as separate return values).

    auto t2 = std::chrono::high_resolution_clock::now();

    // Step 2 (simplified): just build geometry and remove voxelTypeData.
    // The material baking happens inside bakeMaterials, called by the editing
    // path separately.

    // OLD: chunk.voxelTypeData = buildVoxelTypeDataFromBrickMapFast(map);
    // NEW: nothing here — materials are baked later.

    auto t3 = std::chrono::high_resolution_clock::now();
    chunk.LOD = 0;

    double treeMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    double totalMs = std::chrono::duration<double, std::milli>(t3 - t0).count();
    core::perf("updateChunkFromBrickMap: tree64={:.2f}ms total={:.2}ms",
               treeMs, totalMs);
}
```

**P10.4b — Baking happens in the editing path**

The editing path (`editing.cpp`) already calls `updateChunkFromBrickMap` and then
`internChunkGeometry`. We insert `bakeMaterialsFromBrickMap` between them:

```cpp
// In editing.cpp, in the Chunk rebuild path:
updateChunkFromBrickMap(chunk, *blob.brickMap);
// NEW: bake materials directly into the blob
bakeMaterialsFromBrickMap(chunk.geometryData, blob, *blob.brickMap);
// Move completed geometry into the blob
blob.geometry = std::move(chunk.geometryData);
blob.materialPalette = /* built from blob.brickMap during bake */;
int32_t poolIdx = projv::internChunkGeometry(scene, chunk);
```

But `internChunkGeometry` moves `chunk.geometryData` and (currently)
`chunk.voxelTypeData` into the blob. After P10, it should move `chunk.geometryData`
and set `blob.materialIDs` / `blob.materialPalette`.

**P10.4c — Update `internChunkGeometry`**

```cpp
inline int32_t internChunkGeometry(Scene& scene, Chunk& chunk) {
    if (chunk.geometryPoolIndex >= 0) return chunk.geometryPoolIndex;
    GeometryBlob blob;
    blob.geometry = std::move(chunk.geometryData);
    // REMOVED: blob.voxelTypeData = std::move(chunk.voxelTypeData);
    // materialIDs and materialPalette must be set separately (by bakeMaterials).
    blob.refCount = 1;
    blob.dirty = true;
    chunk.geometryData.clear();
    // REMOVED: chunk.voxelTypeData.clear();
    chunk.geometryPoolIndex = poolInsertBlob(scene, std::move(blob));
    return chunk.geometryPoolIndex;
}
```

**P10.4d — What about load-from-disk?**

The compose loader creates blobs from disk data. Currently it fills
`blob.voxelTypeData`. After P10, it should instead fill `blob.materialIDs` and
`blob.materialPalette` during load.

This likely means the on-disk format also needs to be updated (or a conversion
step added at load time). This is covered in P10.6.

### P10.5 — GPU data structures (gpu_interface.cpp, shaders)

**Files:** `src/graphics/gpu_interface.cpp`, `include/graphics/gpu_interface.h`,
`include/data_structures/gpuData.h`, shader files

**P10.5a — Add material textures to GPUData**

In `include/data_structures/gpuData.h`:

```cpp
struct GPUData {
    // Existing fields...
    bgfx::TextureHandle tree64Texture;
    // REMOVED: bgfx::TextureHandle voxelTypeTexture;

    // NEW: material data
    bgfx::TextureHandle materialIDTexture;       // uint8 per voxel, RG8 texture
    bgfx::TextureHandle materialPaletteTexture;   // Material[] as RGBA32U texture

    // NEW: allocators for material textures
    RangeAllocator materialIDAlloc;
    uint32_t materialIDTextureWidth;
    uint32_t materialIDTextureHeight;

    // Existing fields...
};
```

**GPUChunkHeader update:**

The voxelType start/end indices are removed. The geometry texture still has
`geometryStartIndex` / `geometryEndIndex`. We add:

```cpp
struct GPUChunkHeader {
    uint32_t chunkID;
    float positionX, positionY, positionZ;
    float scale;
    uint32_t resolution;
    uint32_t geometryStartIndex;
    uint32_t geometryEndIndex;
    uint32_t materialIDStartIndex;      // NEW: index into materialID texture (in bytes)
    uint32_t materialIDEndIndex;        // NEW: end index (exclusive, in bytes)
    uint32_t dataRefID;
    uint32_t padding[1];
    float rotationX, rotationY, rotationZ, rotationW;
};
```

Total: 15 u32s (same count as before — we removed 2, added 2).

**P10.5b — Texture format for material IDs**

Material IDs are `uint8_t`. We pack them into an RGBA32U texture (4 bytes per
texel = 4 material IDs per texel). This matches the existing `packVoxelTypeTexels`
pattern but simpler (just copy bytes, no packing needed per se).

```cpp
std::vector<uint32_t> packMaterialIDTexels(const std::vector<uint8_t>& materialIDs) {
    size_t texels = (materialIDs.size() + 3) / 4;
    std::vector<uint32_t> out(texels * 4, 0u);
    std::copy(materialIDs.begin(), materialIDs.end(),
              reinterpret_cast<uint8_t*>(out.data()));
    return out;
}
```

**P10.5c — Texture format for material palette**

Each `Material` is 8 bytes (packedColor + pad). Pack into RGBA32U — one texel
per two materials:

```cpp
std::vector<uint32_t> packMaterialPaletteTexels(const std::vector<Material>& palette) {
    size_t texels = (palette.size() + 1) / 2;
    std::vector<uint32_t> out(texels * 4, 0u);
    for (size_t i = 0; i < palette.size(); ++i) {
        out[i * 2 + 0] = palette[i].packedColor;
        out[i * 2 + 1] = palette[i]._pad;
    }
    return out;
}
```

**P10.5d — GPU shader changes**

In the traversal shader (`pjv_utils_DDA.sc`), replace the binary-search voxelType
lookup with a material-ID lookup:

```glsl
// Current (P9) leaf reach logic:
//   uint zOrder = ...;  // voxel's Z-order within chunk
//   (binary search in voxelTypeData array by zOrder)

// New (P10) leaf reach logic:
//   Tree64NodeData node = tree64(leafIndex);
//   uint materialOffset = node.data3 >> 1;   // bits 1-31 of pointerAndLeafFlag
//   uint childZOrder = ...;                   // which of the 64 children we reached
//   uint mask = uint(node.data1) << 32 | node.data2;
//   uint bitsAbove = mask >> (63 - childZOrder + 1);   // mask with higher bits
//   uint countBefore = countbits(bitsAbove);             // popcount
//   uint materialID = materialIDs[materialOffset + countBefore];
//   Material mat = materialPalette[materialID];
//   color = unpackColor(mat.packedColor);
```

**Sampler declarations:**

```glsl
SAMPLER2D(s_materialIDs, 4);          // flat u8 array via texelFetch
SAMPLER2D(s_materialPalette, 5);      // Material array via texelFetch
```

**P10.5e — Remove voxelType texture from `createTexturesForScene`**

```
- Remove voxelTypeTexture creation.
- Add materialIDTexture + materialPaletteTexture creation.
- Remove voxelTypeAlloc from RangeAllocator setup.
- Add materialIDAlloc.
```

**P10.5f — Update `flushSceneUpdates`**

```
- Remove uploadDirtyVoxelTypeData (or whatever the voxelType incremental upload was).
- Add uploadDirtyMaterialIDs.
- For new/changed blobs: upload materialIDs + update materialPalette.
```

### P10.6 — Compose loader update (compose_io.cpp)

**Files:** `src/utils/compose_io.cpp`

**P10.6a — On-disk format**

The old on-disk format stored `voxelTypeData` as triplets (ZOrder, packedColor,
packedNormal). The new format should store:

```
file: chunk.voxels (version 2)
  Header: version, voxelCount
  For each voxel:
    - uint32_t materialID (0 = default)
  Palette:
    - uint32_t paletteSize
    - For each material:
      - uint32_t packedColor
      - uint32_t packedNormal  (reserved, 0 for now)
```

Or simpler: the palette is stored in a separate file `chunk.materials`.

For backward compatibility, add a conversion step: if the on-disk file has the
old format, read it, build a palette from unique colors, assign material IDs,
and write the new format.

**P10.6b — Loader changes**

When loading a blob from disk, after reading voxels:

```cpp
// Old:
blob.voxelTypeData = std::move(voxelData);

// New:
// 1. Build materialIDs from disk data (1 byte per voxel).
// 2. Build materialPalette from unique colors.
// 3. Set blob.materialIDs and blob.materialPalette.
```

The tree64 geometry is separately loaded from disk (unchanged, already in
`blob.geometry`). After loading, call `bakeMaterialsFromBrickMap` to write
material offsets into leaf nodes.

### P10.7 — Remove old brick map color storage

**Files:** `src/utils/voxel_management.cpp`

**P10.7a — Update editing path**

When `brickMapFromVoxelTypeData` is called (first edit from disk data), it
needs to populate `BrickData::materials` instead of `BrickData::colors`.

For backward compat, convert: for each voxel from the old format, extract its
color, intern it into a material, and store the material ID.

Option: add a helper:

```cpp
void brickMapFromVoxelTypeData(VoxelBrickMap& map,
                                const std::vector<uint32_t>& voxelTypeData,
                                GeometryBlob& blob) {
    // Read triplets, pack unique colors into blob.materialPalette,
    // store materialIDs in brick map.
}
```

**P10.7b — Remove old color map helpers**

```
- DELETE: brickMapSetVoxel  (color variant)
- DELETE: brickMapGetColor
- DELETE: brickMapGetVoxel or similar (replace with material query)
- KEEP:   brickMapSetVoxel  but rename to brickMapSetVoxelMaterial
```

Actually, keep `brickMapSetVoxel` but change it to use material IDs. The editing
code calls it — just update the signature:

```cpp
// Before:
void brickMapSetVoxel(VoxelBrickMap& map, int x, int y, int z, Color color);

// After:
// Replaces brickMapSetVoxel. Sets a voxel with the given material ID.
void brickMapSetVoxel(VoxelBrickMap& map, int x, int y, int z, uint8_t materialID);
```

The caller (editing path) is responsible for ensuring the material exists in the
palette before calling.

**P10.7c — Remove `Color` parameter from `PendingVoxelOp`**

In the editing queue, `PendingVoxelOp` currently stores a `Color`. Replace with
`uint8_t materialID`.

```cpp
struct PendingVoxelOp {
    bool remove;
    core::ivec3 position;
    uint8_t materialID;              // was: Color color
    uint8_t _pad[3];                 // padding
};
```

Update `queueVoxelAdd` to take `uint8_t materialID` instead of `Color`.

Update `edit_demo.cpp` to use material IDs (color → interned material).

### P10.8 — Edit demo update (edit_demo/main.cpp)

**Files:** `docs/examples/edit_demo/main.cpp`

The interactive demo cycles through colors when pressing E. Update:

```cpp
// Before (stored colors):
projv::Color cycleColors[] = {
    {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}
};

// After (intern materials once, cycle IDs):
uint8_t cycleMaterialIDs[4];
GeometryBlob* activeBlob = /* the blob being edited */;
cycleMaterialIDs[0] = projv::utils::internMaterial(*activeBlob, "red",   packColor({255,0,0}));
cycleMaterialIDs[1] = projv::utils::internMaterial(*activeBlob, "green", packColor({0,255,0}));
cycleMaterialIDs[2] = projv::utils::internMaterial(*activeBlob, "blue",  packColor({0,0,255}));
cycleMaterialIDs[3] = projv::utils::internMaterial(*activeBlob, "yellow", packColor({255,255,0}));
```

Then the queue call uses material IDs:

```cpp
PendingVoxelOp{false, pos, cycleMaterialIDs[currentMaterial]}
```

### P10.9 — Test driver update (editing_p1/main.cpp)

**Files:** `docs/examples/editing_p1/main.cpp`

Every assertion that checks `blob.voxelTypeData.size()` changes to check
`blob.materialIDs.size()`:

```cpp
// Before:
size_t origVoxelCount = scene.geometryPool[origPoolIdx].voxelTypeData.size() / 3;

// After:
size_t origVoxelCount = scene.geometryPool[origPoolIdx].materialIDs.size();
```

Every assertion about voxel type data values needs new material-based assertions:
verify `blob.materialPalette` has the right number of entries, verify
`blob.materialIDs` count matches the voxel count.

Add P10-specific assertions:
- `blob.materialIDs` size == voxel count
- `blob.materialPalette` size == number of unique colors
- Each material ID in `materialIDs` is a valid index into `materialPalette`
- Leaf nodes in the tree64 have `pointerAndLeafFlag & 1 == 1` and `>> 1` points
  to valid offsets
- After editing, new material IDs are correctly appended to `materialIDs` and
  `materialPalette`

### P10.10 — Remove `createVoxelTypeData` and old helpers

**Files:** `src/utils/voxel_management.cpp`, `include/utils/voxel_management.h`

```
- DELETE: createVoxelTypeData  (was for testing, unused in production)
- DELETE: buildVoxelTypeDataFromBrickMap  (old slow path)
- DELETE: buildVoxelTypeDataFromBrickMapFast  (the P10-optimized version, no longer needed)
- DELETE: packColor  (replaced by internMaterial from palette; still used for GPU structs)
  Actually, packColor is still used by Material packing during bake and for the GPU.
  Keep packColor/unpackColor as utility, just no longer stored per-voxel.
```

## Implementation Order

1. **P10.1** — Define `Material`, update `GeometryBlob`, `Chunk`, `GPUChunkHeader`.
   - Compile check: project builds with `materialIDs` and `materialPalette` fields
     added (and `voxelTypeData` removed from `Chunk` only — keep on `GeometryBlob`
     temporarily to not break compilation).
2. **P10.2** — Material API + update `BrickData`.
   - `internMaterial`, `findMaterialByName`, `findMaterialByColor`, `brickMapSetMaterial`, `brickMapGetMaterial`.
   - Replace `colors` map in `BrickData` with `materials` map.
3. **P10.3** — `bakeMaterialsFromBrickMap`.
   - Single forward pass over serialized tree64.
   - Cursor mapping: serialized leaf → (brickIdx, row) → material data.
4. **P10.4** — Update `updateChunkFromBrickMap`, `internChunkGeometry`, editing path.
   - Remove `voxelTypeData` from `Chunk`.
   - Baking happens between `updateChunkFromBrickMap` and `internChunkGeometry`.
5. **P10.5** — GPU: remove voxelType texture, add material textures.
   - Update `GPUData`, `makeHeader`, `createTexturesForScene`, `flushSceneUpdates`.
   - Shader changes: remove binary search, add popcount material lookup.
6. **P10.6** — Compose loader: convert old format, write new format.
7. **P10.7** — Remove old brick map color storage, update `PendingVoxelOp`.
8. **P10.8** — Edit demo: use material IDs.
9. **P10.9** — Test driver: update assertions.
10. **P10.10** — Delete old `createVoxelTypeData` and `buildVoxelTypeDataFromBrickMap*`.

## Definition of Done

- `voxelTypeData` does not exist on `Chunk` or `GeometryBlob`.
- `GPUChunkHeader` has `materialIDStartIndex` / `materialIDEndIndex` instead of
  `voxelTypeDataStartIndex` / `voxelTypeDataEndIndex`.
- `GeometryBlob` has `materialIDs` (contiguous `uint8_t[]`, one per valid voxel
  in leaf-node traversal order) and `materialPalette` (dense `Material[]`).
- `BrickData` has `materials` map (`localZOrder → uint8_t`) instead of
  `colors` map (`localZOrder → uint32_t`).
- `PendingVoxelOp` stores `uint8_t materialID` instead of `Color color`.
- `queueVoxelAdd` takes `uint8_t materialID` instead of `Color`.
- `bakeMaterialsFromBrickMap` correctly fills `materialIDs` and `materialPalette`
  AND writes material offsets into leaf node `pointerAndLeafFlag`.
- GPU shader does `popcount(mask, childZOrder)` to index into `materialIDs[offset]`,
  then reads `materialPalette[id].packedColor` — no binary search.
- The interactive edit demo cycles through interned material IDs instead of colors.
- All P1-P9 assertions pass with updated voxel count checks.
- `materialPalette` has ≤ 256 entries (`MAX_MATERIALS_PER_BLOB`).
