#ifndef PROJV_GPU_DATA_H
#define PROJV_GPU_DATA_H
#include <cstdint>
#include <vector>

#include <bgfx/bgfx.h>
#include "graphics/range_allocator.h"
#include "utils/animation.h"

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
        // ---- The animation envelope's ranges, when this blob has one ----------------------------
        //
        // The envelope tree64 is allocated out of the SAME tree64Alloc as the geometry, and the
        // motion bytes out of the same materialIDAlloc. That is the whole reason this feature needs
        // no new texture and no new sampler: bgfx has sixteen texture stages, the engine already
        // parks the scene on 9..15, and 0..8 belong to a pass's own inputs (PROJV_MAX_PASS_SAMPLERS).
        // There was no stage to spend, and it turns out none is needed -- a second tree in the same
        // flat array is just another range.
        //
        // envTexelLen == 0 means this blob has no envelope, which is the case for almost every blob
        // and for every blob loaded from a v2 .data.
        uint32_t envTexelOffset = 0;
        uint32_t envTexelLen = 0;
        uint32_t envTexelAllocated = 0;
        uint32_t envMotionTexelOffset = 0;
        uint32_t envMotionTexelLen = 0;
        uint32_t envMotionTexelAllocated = 0;
        uint32_t envMotionByteLen = 0;
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

        // Material palette texture dimensions, in texels. RGBA32U with ONE ENTRY PER TEXEL: the four
        // words of a Material (see scene.h) are the texel's four components, so a global palette
        // index is a texel index directly. Width is a power of two because the shader addresses it
        // with `& (width - 1)` and `>> log2(width)`; height exists because a palette can outgrow one
        // row now that an entry costs a whole texel.
        uint32_t paletteWidth = 0;
        uint32_t paletteHeight = 0;

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

        // The motion table and the current time, uploaded to every pass that declares them.
        //
        // It lives here rather than in a file-scope global because a renderer writing an image
        // sequence and a voxelizer asked for frame 37 both need to sample deterministically --
        // `inline Camera cam` is the pattern this deliberately does not repeat. Assign it before
        // rendering; the engine uploads whatever is here.
        AnimationState animation;
    };
}

#endif
