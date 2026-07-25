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
        uint32_t geomTexelOffset = 0;
        uint32_t geomTexelLen = 0;
        uint32_t geomTexelAllocated = 0;
        uint32_t matTexelOffset = 0;
        uint32_t matTexelLen = 0;
        uint32_t matTexelAllocated = 0;
        uint32_t matByteLen = 0;
        // Storage LOD of the data actually resident in the textures right now. Distinct from
        // GeometryBlob::renderLOD, which is the *requested* value and may not be flushed yet.
        // makeHeader reads this one so the header's resolution always describes the tree that is
        // really there; uploadDirtyBlobs compares the two to detect an LOD change.
        uint32_t uploadedLOD = 0;
        bool uploaded = false;
    };

    struct GPUData {
        bgfx::TextureHandle tree64Texture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle materialIDTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle materialPaletteTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle headerTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle gridInfoTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle cellMapTexture = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle looseListTexture = BGFX_INVALID_HANDLE;

        bgfx::UniformHandle tree64Sampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle materialIDSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle materialPaletteSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle headerSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle gridInfoSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle cellMapSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle looseListSampler = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle tree64DimsUniform = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle materialIDDimsUniform = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle paletteDimsUniform = BGFX_INVALID_HANDLE;

        graphics::RangeAllocator tree64Alloc;
        graphics::RangeAllocator materialIDAlloc;
        uint32_t tree64Width = 0, tree64Height = 0;
        uint32_t materialIDWidth = 0, materialIDHeight = 0;

        uint32_t paletteWidth = 0; // width of material palette texture (1D, RGBA32U, 4 colors per texel)

        uint32_t headerCapacity = 0;

        uint32_t looseCount = 0;
        uint32_t looseCapacity = 0;

        uint32_t gridInfoTexWidth = 0;
        uint32_t cellMapTexWidth = 0, cellMapTexHeight = 0;
        uint32_t looseListTexWidth = 0, looseListTexHeight = 0;

        uint32_t uploadedChunkCount = 0;

        // Offset of each component's palette in the global palette texture,
        // indexed by ComponentHandle. Parallel to Scene.components.
        std::vector<uint32_t> componentPaletteOffsets;
        // Sum of all components' paletteVersion values; rebuilt when any changes.
        uint64_t componentPaletteVersion = 0;

        std::vector<GPUBlobRange> blobRanges;
    };
}

#endif
