#ifndef GPU_DATA_H
#define GPU_DATA_H

#include <cstdint>
#include <vector>

#include "../../external/bgfx/include/bgfx/bgfx.h"
#include "graphics/range_allocator.h"

namespace projv {
    // Where a geometry-pool blob currently lives inside the GPU data textures. Parallel to
    // Scene.geometryPool (indexed by the same pool index). This is the layout map the old one-shot
    // createTexturesForScene threw away — keeping it is what makes incremental add/update/remove
    // possible. Offsets are in texels (the allocation unit), matching the shader's texel addressing.
    //
    // P6: per-blob over-allocation (geomTexelAllocated / typeTexelAllocated) rounds up the GPU range
    // so that in-place COW edits that grow the blob slightly can stay in the same range without
    // reallocation. The extra slack is sized relative to the blob's actual data (e.g. 25% + 64 min).
    // The allocator and texture headroom both account for this padding via `withHeadroom(paddedUsed)`.
    struct GPUBlobRange {
        uint32_t geomTexelOffset = 0;     // start texel in the tree64 texture; == header.geometryStartIndex
        uint32_t geomTexelLen = 0;        // node count actually used (1 node = 1 RGB texel)
        uint32_t geomTexelAllocated = 0;  // padded allocation; >= geomTexelLen; range is [offset, offset+allocated)
        uint32_t typeTexelOffset = 0;     // start texel in the voxelType texture; *4 == header voxelType start
        uint32_t typeTexelLen = 0;        // used texels (ceil(typeUintLen / 4))
        uint32_t typeTexelAllocated = 0;  // padded allocation; >= typeTexelLen
        uint32_t typeUintLen = 0;         // actual voxelType uint count (header end = start + this)
        bool uploaded = false;            // false for an unreferenced/free pool slot
    };

    struct GPUData {
        bgfx::TextureHandle tree64Texture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle voxelTypeDataTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle headerTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle gridInfoTexture = BGFX_INVALID_HANDLE;   // Top-level grid descriptors + scene counts.
        bgfx::TextureHandle cellMapTexture = BGFX_INVALID_HANDLE;    // Per-grid cell -> chunk index maps.
        bgfx::TextureHandle looseListTexture = BGFX_INVALID_HANDLE;  // Explicit list of loose chunk handles the shader iterates.

        bgfx::UniformHandle tree64Sampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle voxelTypeDataSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle headerSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle gridInfoSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle cellMapSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle looseListSampler = BGFX_INVALID_HANDLE;

        // --- Persistent layout state (managed pools) ---
        // Suballocators over the two geometry data textures. Units are texels: tree64 = 1 node per RGB
        // texel; voxelType = 4 uints per RGBA texel. Freeing a blob returns its range here for reuse.
        graphics::RangeAllocator tree64Alloc;
        graphics::RangeAllocator voxelTypeAlloc;
        // Backing texture dimensions (texels). capacity == width*height; a grow recreates the texture.
        uint32_t tree64Width = 0, tree64Height = 0;
        uint32_t voxelTypeWidth = 0, voxelTypeHeight = 0;

        // Header texture is a single row of (capacity*4) texels; a chunk's handle IS its 4-texel slot.
        uint32_t headerCapacity = 0;

        // Loose-list texture (RGBA, 4 handles per texel), a compact list of `looseCount` live handles.
        uint32_t looseCount = 0;
        uint32_t looseCapacity = 0;

        // P5: chunk handles < uploadedChunkCount have been uploaded at least once.
        // New chunks (handle >= this watermark) need their first header row written.
        uint32_t uploadedChunkCount = 0;

        // Per-pool-blob GPU location; parallel to Scene.geometryPool.
        std::vector<GPUBlobRange> blobRanges;
    };
}

#endif
