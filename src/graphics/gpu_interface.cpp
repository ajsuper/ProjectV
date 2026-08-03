#include "graphics/gpu_interface.h"
#include "utils/material.h"
#include "utils/voxel_management.h"

#include <chrono>
#include <cstring>
#include <algorithm>
#include <optional>
#include <unordered_set>

namespace projv::graphics {
    namespace {
        // ---- Data texture VRAM budget ----
        //
        // The two data textures are sized from actual GPU memory rather than fixed constants, so a
        // bigger card automatically buys a bigger scene.
        //
        // Share of GPU memory the two data textures may claim BETWEEN THEM. Everything else the
        // renderer needs -- cascade/gbuffer/TAA targets, the header and scene tables, bgfx's own
        // staging -- comes out of the remainder, so this cannot safely approach 1.0.
        constexpr double kVramFraction = 0.85;

        // Split of the byte budget between the two textures. Measured per-chunk demand after
        // uniform-leaf material compression (including paddedAlloc's 25% slack) is ~1.14:1
        // geometry:material in TEXELS; tree64 is RGBA32U (16 B/texel) and materialID is RGBA8U
        // (4 B/texel), so that is ~4.55:1 in BYTES.
        //
        //            res 64          res 256
        //   tree64     5.1k texels   168.1k texels
        //   material   6.6k texels   147.7k texels
        //
        // Re-measure if content changes character: sparse or high-frequency material detail lowers
        // the ~96% uniform-leaf hit rate and pushes the material share back up.
        constexpr double kGeomByteShare = 0.82;

        // bgfx only reports GPU memory on Vulkan and D3D11/12, and not until a frame has been
        // submitted -- which has NOT happened when createTexturesForScene first runs. Until then we
        // size from this fallback; the first growDataTextures re-queries and repacks into the real
        // budget, so the sizing self-corrects rather than being stuck here.
        //
        // Kept deliberately small: that one resize is the only moment two sets of data textures are
        // resident at once (bgfx creates in cmdPre and destroys in cmdPost), so the transient peak is
        // budget + this. Every later repack reuses the textures in place and costs nothing extra.
        constexpr uint64_t kVramFallbackBytes = 512ull << 20;

        // Never size below this, even if the driver reports an implausibly small budget.
        constexpr uint64_t kVramFloorBytes = 256ull << 20;

        // HARD per-texture ceiling, independent of how much VRAM exists.
        //
        // bimg::imageGetSize accumulates a texture's byte size into a uint32 and bgfx exposes it as
        // TextureInfo::storageSize (also uint32), so ANY single texture >= 4 GB silently wraps. A
        // 32768x9222 RGBA32U tree64 is 4.83 GB and was reported to bgfx as 539 MB — it created
        // without error and rendered nothing at all.
        //
        // 2 GB keeps a factor of two clear of that wall and also avoids any signed-int32 size
        // intermediate. Raising it toward 3 GB is possible and buys proportionally more chunks, but
        // do it one step at a time and watch for a black screen — that is what going over looks like.
        // Going meaningfully past ~4 GB of geometry needs a second texture, not a bigger one.
        constexpr uint64_t kMaxTextureBytes = 2ull << 30;

        uint32_t maxTexSize() {
            const bgfx::Caps* caps = bgfx::getCaps();
            uint32_t m = caps ? static_cast<uint32_t>(caps->limits.maxTextureSize) : 16384u;
            return m ? m : 16384u;
        }

        // Estimate bytes used by a texture (RGBA32U = 16 bytes/texel, RGBA8U = 4 bytes/texel).
        uint64_t texBytes(uint32_t w, uint32_t h, bool rgba32) {
            return static_cast<uint64_t>(w) * h * (rgba32 ? 16u : 4u);
        }

        // Guard the uint32 storageSize wall described at kMaxTextureBytes. A texture at or past 4 GB
        // is mis-reported to bgfx and renders nothing, so this is an error rather than a warning.
        void checkTextureSize(const char* name, uint64_t bytes) {
            constexpr uint64_t kGB = 1024ull * 1024 * 1024;
            if (bytes >= (4ull << 30)) {
                core::error("{} texture is {:.2f} GB — bgfx stores texture size in a uint32, so this "
                            "wraps to {:.2f} GB and the texture will render nothing",
                            name, double(bytes) / kGB, double(bytes & 0xFFFFFFFFull) / kGB);
            } else if (bytes > kMaxTextureBytes) {
                core::info("WARNING: {} texture is {:.2f} GB — above the {:.2f} GB ceiling we size to",
                           name, double(bytes) / kGB, double(kMaxTextureBytes) / kGB);
            }
        }

        // Extra slack so early incremental adds don't immediately force a grow.
        uint32_t withHeadroom(uint32_t used) { return used + used / 2 + 1024; }

        // Per-blob over-allocation padding so in-place COW edits that grow the blob slightly
        // don't need a new GPU range. The padded total feeds withHeadroom, so the allocator's own
        // free space scales with it.
        //
        // Slack is PROPORTIONAL ONLY, and only on blobs big enough to be an edit target. The rule
        // was `needed + max(needed/4, 64)`, and that flat 64-texel floor was sized for blobs of a
        // few thousand texels. A streaming world's blobs are not all that size: on the outer LOD
        // ring a geometry blob is about 58 texels, which the floor took to 122 -- 111% overhead --
        // to absorb an edit those chunks never receive, because an edit only ever lands where the
        // player can reach and that is always the full-resolution near ring.
        //
        // Measured over a settled radius-110 view (155k outer-ring chunks, [VRAMFIT]): the old rule
        // spent 396 MiB of a 1522 MiB data-texture footprint on padding, and the outer ring ran at
        // 70% efficiency. That matters because the materialID texture's capacity (32768x12747 texels,
        // 1556 MiB) is the hard ceiling on view distance -- overflowing it drops material bytes and
        // renders wrong colours, so every MiB of padding there is view distance not spent.
        //
        // A blob below the threshold gets none. If an edit ever does land on one, the growth test in
        // uploadDirtyBlobs sees `nodes > geomTexelAllocated`, frees and reallocates -- correct, and
        // cheap next to the re-bake that edit already triggered.
        constexpr uint32_t kPadMinTexels = 4096;
        uint32_t paddedAlloc(uint32_t needed) {
            if (needed < kPadMinTexels) return needed;
            return needed + needed / 4u;
        }

        static uint32_t nextPowerOfTwo(uint32_t v) {
            if (v == 0) return 1;
            v--;
            v |= v >> 1;
            v |= v >> 2;
            v |= v >> 4;
            v |= v >> 8;
            v |= v >> 16;
            v++;
            return v;
        }

        inline bool isPowerOfTwo(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }
        inline bool isPowerOfFour(uint32_t v) { return isPowerOfTwo(v) && (v & 0x55555555u) != 0u; }

        static uint32_t nextPowerOfFour(uint32_t v) {
            uint32_t p = 1;
            while (p < v) p <<= 2;
            return p;
        }

        // Total bytes the two data textures may use, together.
        //
        // bgfx reports gpuMemoryMax as the device-local heap BUDGET, i.e. memory currently free --
        // which already excludes the desktop and our own render targets. Our existing data textures
        // are part of what is *not* free, so they are added back before taking the fraction.
        // Without that the budget would shrink every time we regrow into it (each grow sees less
        // free memory because the previous grow consumed it) and the scene would ratchet downward.
        uint64_t dataTextureBudget(const GPUData& gpuData) {
            const bgfx::Stats* stats = bgfx::getStats();
            uint64_t ours = texBytes(gpuData.tree64Width, gpuData.tree64Height, true)
                          + texBytes(gpuData.materialIDWidth, gpuData.materialIDHeight, false);
            if (stats == nullptr || stats->gpuMemoryMax <= 0) {
                core::info("dataTextureBudget: GPU memory not reported yet — using {} MB fallback "
                           "(will re-query on the first repack)", kVramFallbackBytes >> 20);
                return kVramFallbackBytes;
            }
            uint64_t pool = static_cast<uint64_t>(stats->gpuMemoryMax) + ours;
            uint64_t budget = static_cast<uint64_t>(double(pool) * kVramFraction);
            core::info("dataTextureBudget: {} MB free + {} MB already ours = {} MB pool, "
                       "{:.0f}% -> {} MB budget",
                       uint64_t(stats->gpuMemoryMax) >> 20, ours >> 20, pool >> 20,
                       kVramFraction * 100.0, budget >> 20);
            return std::max<uint64_t>(budget, kVramFloorBytes);
        }

        // Fit a data texture into `byteBudget`. Width takes the largest power of two the GPU allows
        // (the shader addresses texels with & (w-1) and >> log2(w), so it MUST be a power of two);
        // height then absorbs whatever the budget allows, capped at maxTextureSize.
        // Returns capacity in texels. maxSz is passed in (not read from caps) so this is callable
        // headless with no bgfx context.
        uint32_t fitDataDims(uint64_t byteBudget, uint32_t bytesPerTexel,
                             uint32_t& w, uint32_t& h, uint32_t maxSz) {
            // Largest power of 2 <= maxSz, so shader bit ops stay correct.
            uint32_t maxPoT = (maxSz >= 0x80000000u) ? 0x80000000u : (nextPowerOfTwo(maxSz + 1u) >> 1u);
            w = maxPoT;

            // Clamp to the per-texture ceiling before anything else: no amount of free VRAM makes a
            // texture bgfx cannot describe usable. See kMaxTextureBytes.
            if (byteBudget > kMaxTextureBytes) {
                core::info("fitDataDims: {} MB budget clamped to the {} MB per-texture ceiling "
                           "(bgfx TextureInfo::storageSize is uint32)",
                           byteBudget >> 20, kMaxTextureBytes >> 20);
                byteBudget = kMaxTextureBytes;
            }
            uint64_t rows = (byteBudget / bytesPerTexel) / w;
            h = static_cast<uint32_t>(std::min<uint64_t>(std::max<uint64_t>(rows, 1ull), maxSz));

            // materialIDStartIndex is a uint32 BYTE index (matTexelOffset * 4), so the material
            // texture must stay under 2^30 texels or header offsets wrap. The byte ceiling above
            // already implies this, but keep it explicit so the two limits cannot drift apart.
            const uint64_t kMaxTexels = (bytesPerTexel == 4u) ? (1ull << 30) : (1ull << 31);
            while (uint64_t(w) * h > kMaxTexels && h > 1u) h >>= 1;
            return w * h;
        }

        // Upload a contiguous texel prefix into a fresh texture, one band of whole rows at a time.
        //
        // Deliberately does NOT materialise a full-size host buffer. At multi-GB dimensions that
        // would cost the texture's size in a std::vector plus the same again inside bgfx::copy, and
        // bgfx::Memory::size is a uint32 so a >=4 GB single copy is not even expressible. Banding
        // keeps transient host memory at kBandBytes and makes upload cost proportional to the data
        // actually present rather than to the texture's capacity.
        //
        // Texels past `src` are left UNINITIALISED rather than zeroed. Every read is bounded by
        // GPUBlobRange/header offsets, so untouched texels are never fetched.
        void uploadPrefixBanded(bgfx::TextureHandle tex, uint32_t w, uint32_t h,
                                const void* src, size_t srcBytes, uint32_t bytesPerTexel) {
            if (!bgfx::isValid(tex) || srcBytes == 0) return;
            const size_t rowBytes = static_cast<size_t>(w) * bytesPerTexel;
            const size_t rowsTotal = std::min<size_t>(h, (srcBytes + rowBytes - 1) / rowBytes);
            constexpr size_t kBandBytes = 64ull << 20;
            const size_t rowsPerBand = std::max<size_t>(1, kBandBytes / rowBytes);

            std::vector<uint8_t> tail;   // only used for a final partial row
            for (size_t row = 0; row < rowsTotal; row += rowsPerBand) {
                size_t rows = std::min(rowsPerBand, rowsTotal - row);
                size_t off  = row * rowBytes;
                size_t want = rows * rowBytes;
                size_t have = std::min(want, srcBytes - off);
                const void* data = static_cast<const uint8_t*>(src) + off;
                if (have < want) {
                    // updateTexture2D needs a whole rectangle; pad the last band's remainder.
                    tail.assign(want, 0u);
                    std::memcpy(tail.data(), data, have);
                    data = tail.data();
                }
                const bgfx::Memory* mem = bgfx::copy(data, static_cast<uint32_t>(want));
                bgfx::updateTexture2D(tex, 0, 0, 0, uint16_t(row), uint16_t(w), uint16_t(rows), mem);
            }
        }

        // Rewrite an existing data texture's leading texels in place. Used by a repack when the
        // dimensions have not changed, which avoids a destroy+create pair: bgfx puts CreateTexture
        // in cmdPre (start of frame) and DestroyTexture in cmdPost (end of frame), so the pair
        // transiently holds BOTH allocations — at multi-GB sizes that is an out-of-memory.
        void refillDataTexture(bgfx::TextureHandle tex, uint32_t w, uint32_t h,
                               const std::vector<uint32_t>& texels) {
            size_t capacityWords = static_cast<size_t>(w) * h * 4;
            if (texels.size() > capacityWords) {
                core::error("tree64 texture ({}x{}): packed layout is {} words but the texture holds "
                            "{} — {} words DROPPED; blobs past the cut render garbage geometry",
                            w, h, texels.size(), capacityWords, texels.size() - capacityWords);
            }
            uploadPrefixBanded(tex, w, h, texels.data(),
                               std::min(texels.size(), capacityWords) * sizeof(uint32_t), 16u);
        }

        void refillDataTexture8(bgfx::TextureHandle tex, uint32_t w, uint32_t h,
                                const std::vector<uint8_t>& texels) {
            size_t capacityBytes = static_cast<size_t>(w) * h * 4;
            if (texels.size() > capacityBytes) {
                core::error("materialID texture ({}x{}): packed layout is {} bytes but the texture holds "
                            "{} — {} bytes DROPPED; blobs past the cut render wrong colors",
                            w, h, texels.size(), capacityBytes, texels.size() - capacityBytes);
            }
            uploadPrefixBanded(tex, w, h, texels.data(), std::min(texels.size(), capacityBytes), 4u);
        }

        // Create an RGBA32U MUTABLE texture (no initial data → mutable, updateTexture2D works)
        // and fill its leading texels from `texels` (4 uints per texel).
        bgfx::TextureHandle createDataTexture(uint32_t w, uint32_t h, const std::vector<uint32_t>& texels) {
            checkTextureSize("RGBA32U", texBytes(w, h, true));
            bgfx::TextureHandle tex = bgfx::createTexture2D(uint16_t(w), uint16_t(h), false, 1,
                bgfx::TextureFormat::RGBA32U,
                BGFX_SAMPLER_POINT, nullptr);
            if (!bgfx::isValid(tex)) {
                core::error("createDataTexture({}x{}) RGBA32U ({:.2f} GB) failed — out of VRAM? "
                            "Nothing will render. Lower kVramFraction.",
                            w, h, double(texBytes(w, h, true)) / double(1ull << 30));
                return BGFX_INVALID_HANDLE;
            }
            refillDataTexture(tex, w, h, texels);
            return tex;
        }

        // Create an RGBA8 MUTABLE texture for material IDs (4 uint8 per texel, 4 bytes/texel).
        bgfx::TextureHandle createDataTexture8(uint32_t w, uint32_t h, const std::vector<uint8_t>& texels) {
            checkTextureSize("RGBA8U", texBytes(w, h, false));
            bgfx::TextureHandle tex = bgfx::createTexture2D(uint16_t(w), uint16_t(h), false, 1,
                bgfx::TextureFormat::RGBA8U,
                BGFX_SAMPLER_POINT, nullptr);
            if (!bgfx::isValid(tex)) {
                core::error("createDataTexture8({}x{}) RGBA8U ({:.2f} GB) failed — out of VRAM? "
                            "Nothing will render. Lower kVramFraction.",
                            w, h, double(texBytes(w, h, false)) / double(1ull << 30));
                return BGFX_INVALID_HANDLE;
            }
            refillDataTexture8(tex, w, h, texels);
            return tex;
        }

        // A dead/unused header row: scale <= 0 makes the shader's broadphase reject it.
        GPUChunkHeader degenerateHeader() {
            GPUChunkHeader h{};
            h.scale = 0.0f;
            h.rotationW = 1.0f;
            return h;
        }

// Build the GPU header for a live chunk from its pool blob's current GPU range.
GPUChunkHeader makeHeader(const Chunk& chunk, const GPUBlobRange& r,
                              const Scene& scene, const GPUData& gpuData) {
    GPUChunkHeader g{};
    g.chunkID = chunk.header.chunkID;
    g.geometryStartIndex = r.geomTexelOffset;
    g.geometryEndIndex = r.geomTexelOffset + r.geomTexelLen;
    g.materialIDStartIndex = r.matTexelOffset * 4u;
    g.materialIDEndIndex = r.matTexelOffset * 4u + r.matByteLen;
    g.positionX = chunk.header.position.x;
    g.positionY = chunk.header.position.y;
    g.positionZ = chunk.header.position.z;
    // Storage LOD: the chunk's CPU resolution is always full detail, but the tree actually resident
    // in the texture may have had levels dropped. The header must describe the tree that is really
    // there, so it reads the range's uploadedLOD rather than the blob's (possibly not-yet-flushed)
    // requested value. `scale` is deliberately NOT adjusted -- holding it fixed while resolution
    // shrinks is exactly what grows each voxel's world size and keeps the chunk's world footprint,
    // position and grid cell identical.
    g.resolution = resolutionAtLOD(chunk.header.resolution, r.uploadedLOD);
    g.scale = chunk.header.scale;
    g.dataRefID = 0;
    if (chunk.componentHandle < scene.components.size()) {
        int32_t rid = scene.components[chunk.componentHandle].dataRefID;
        if (rid >= 0) g.dataRefID = static_cast<uint32_t>(rid);
    }

    ComponentHandle comp = INVALID_COMPONENT_HANDLE;
    if (chunk.gridIndex >= 0 && static_cast<size_t>(chunk.gridIndex) < scene.grids.size())
        comp = scene.grids[chunk.gridIndex].componentHandle;
    else
        comp = chunk.componentHandle;
    g.paletteOffset = (comp < gpuData.componentPaletteOffsets.size())
        ? gpuData.componentPaletteOffsets[comp] : 0u;
#if defined(PROJV_ENABLE_RENDER)
    if (g.paletteOffset != 0)
        core::info("MAKE-HDR h={} comp={} compOffSz={} compOff={}",
                   chunk.header.chunkID, comp, gpuData.componentPaletteOffsets.size(), g.paletteOffset);
#endif

    g.rotationX = chunk.header.rotation.x;
    g.rotationY = chunk.header.rotation.y;
    g.rotationZ = chunk.header.rotation.z;
    g.rotationW = chunk.header.rotation.w;

    // Traversal LOD: how many extra levels *this chunk instance* wants the shader to stop short of
    // what's actually uploaded (r.uploadedLOD), on top of the ray-distance-based cutoff it already
    // applies (see computeTargetLOD in pjv_utils_DDA.sc). Only meaningful when this instance wants
    // *less* detail than the shared blob currently has resident -- e.g. one of several chunk
    // instances sharing a blob that another, closer instance forced finer. The `>` guard matters: in
    // a transient frame where reconciliation just lowered the blob's requested LOD but the upload
    // flush hasn't happened yet, r.uploadedLOD can still exceed chunk.requestedLOD, and an unguarded
    // unsigned subtraction here would underflow.
    g.traversalLOD = (chunk.requestedLOD > r.uploadedLOD) ? (chunk.requestedLOD - r.uploadedLOD) : 0u;

    return g;
}

        // Seed an allocator from a freshly packed contiguous layout.
        //
        // If the layout does not fit the texture, reserve() marks NOTHING and leaves the whole
        // capacity free -- the next alloc() then returns offset 0 and the upload overwrites live
        // blobs one at a time (this is what produced "geometry is fine but old chunks' colors get
        // corrupted after a texture resize"). Fall back to reserving everything so alloc() fails
        // cleanly instead: new blobs go un-uploaded and render as missing, which is recoverable,
        // rather than silently trashing blobs that were already correct.
        void seedAllocator(RangeAllocator& alloc, uint32_t cap, uint32_t usedPadded, const char* what) {
            alloc.reset(cap);
            if (alloc.reserve(0, usedPadded)) return;
            core::error("{}: packed layout needs {} texels but capacity is {} — cannot seed allocator. "
                        "Locking it so new blobs fail to allocate instead of overwriting live blobs.",
                        what, usedPadded, cap);
            alloc.reset(cap);
            (void)alloc.reserve(0, cap);
        }

        // Pack a blob's geometry into RGBA32U texels (1 node -> RGB + zero alpha).
        std::vector<uint32_t> packGeometryTexels(const std::vector<uint32_t>& geometry) {
            auto t0 = std::chrono::high_resolution_clock::now();
            uint32_t nodes = static_cast<uint32_t>(geometry.size() / 3);
            std::vector<uint32_t> out(size_t(nodes) * 4, 0u);
            for (uint32_t n = 0; n < nodes; n++) {
                out[size_t(n) * 4 + 0] = geometry[size_t(n) * 3 + 0];
                out[size_t(n) * 4 + 1] = geometry[size_t(n) * 3 + 1];
                out[size_t(n) * 4 + 2] = geometry[size_t(n) * 3 + 2];
                uint32_t childPtr = geometry[size_t(n) * 3 + 2] >> 1;
                out[size_t(n) * 4 + 3] = childPtr;
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            core::perf("packGeometryTexels: {} nodes: {:.2f}ms", nodes, ms);
            return out;
        }



        // Resolve what a blob should actually upload, applying its storage LOD.
        //
        // Both upload paths (uploadDirtyBlobs and the growDataTextures repack) go through here, so
        // they cannot disagree about a blob's on-GPU size -- a disagreement would mean the allocator
        // reserved one length and the header advertised another.
        //
        // At renderLOD 0 the scratch buffers are untouched and the blob's own arrays are used
        // directly, so the common case costs nothing. Otherwise the coarsened copy is built into
        // caller-owned scratch: the blob's CPU arrays always stay full resolution.
        void effectiveBlobData(const GeometryBlob& blob,
                               std::vector<uint32_t>& scratchGeom,
                               std::vector<uint8_t>& scratchMat,
                               const std::vector<uint32_t>*& outGeom,
                               const std::vector<uint8_t>*& outMat) {
            if (blob.renderLOD == 0) {
                outGeom = &blob.geometry;
                outMat = &blob.materialIDs;
                return;
            }
            utils::downsampleTree64(blob.geometry, blob.materialIDs, blob.renderLOD,
                                    scratchGeom, scratchMat);
            outGeom = &scratchGeom;
            outMat = &scratchMat;
        }

        std::vector<uint8_t> packMaterialIDTexels8(const std::vector<uint8_t>& materialIDs) {
            size_t texels = (materialIDs.size() + 3) / 4;
            std::vector<uint8_t> out(texels * 4, 0u);
            for (size_t i = 0; i < materialIDs.size(); ++i)
                out[i] = materialIDs[i];
            return out;
        }

        // Guardrail: two live blobs must never share GPU texels, and no range may run past the end
        // of its texture. A material overlap here is exactly the "geometry is fine but the colors
        // are wrong" failure -- the allocator handed out a range that still holds another blob's
        // material IDs. It names the offenders when it fires.
        //
        // OFF by default -- build with -DPROJV_VALIDATE_GPU_RANGES to enable. This was called on
        // every flush, i.e. on essentially every frame that streams anything, and it is not the
        // "cheap O(n log n)" its original note claimed: n is the live blob count, which a streaming
        // scene carries in the tens of thousands (57,000 measured in the terrain example), so each
        // call builds two ~1MB vectors and sorts them. That is milliseconds of frame time per
        // frame, spent re-proving an invariant that only changes when the allocator changes.
#if defined(PROJV_VALIDATE_GPU_RANGES)
        void validateBlobRanges(const projv::Scene& scene, const GPUData& gpuData, const char* where) {
            struct Span { uint32_t lo, hi; size_t blob; };
            std::vector<Span> geom, mat;
            size_t n = std::min(scene.geometryPool.size(), gpuData.blobRanges.size());
            for (size_t b = 0; b < n; b++) {
                if (scene.geometryPool[b].refCount == 0) continue;
                const GPUBlobRange& r = gpuData.blobRanges[b];
                if (!r.uploaded) continue;
                if (r.geomTexelLen) geom.push_back({r.geomTexelOffset, r.geomTexelOffset + r.geomTexelLen, b});
                if (r.matTexelLen)  mat.push_back({r.matTexelOffset,   r.matTexelOffset + r.matTexelLen,   b});
            }
            auto check = [&](std::vector<Span>& v, const char* kind, uint32_t cap) {
                std::sort(v.begin(), v.end(), [](const Span& a, const Span& b) { return a.lo < b.lo; });
                uint32_t reported = 0;
                for (size_t i = 0; i + 1 < v.size(); i++) {
                    if (v[i].hi > v[i + 1].lo && reported++ < 4) {
                        core::error("{}: {} OVERLAP — blob {} [{},{}) vs blob {} [{},{})",
                                    where, kind, v[i].blob, v[i].lo, v[i].hi,
                                    v[i + 1].blob, v[i + 1].lo, v[i + 1].hi);
                    }
                }
                if (reported > 4)
                    core::error("{}: {} … {} more overlapping pairs", where, kind, reported - 4);
                if (!v.empty() && v.back().hi > cap)
                    core::error("{}: {} range ends at {} but texture capacity is {}",
                                where, kind, v.back().hi, cap);
            };
            check(geom, "geometry", gpuData.tree64Width * gpuData.tree64Height);
            check(mat, "materialID", gpuData.materialIDWidth * gpuData.materialIDHeight);
        }
#else
        inline void validateBlobRanges(const projv::Scene&, const GPUData&, const char*) {}
#endif

        } // anonymous namespace

    // Build the global palette texture from every component's materialPalette.
    // Stores per-component offsets in gpuData.componentPaletteOffsets.
    // Returns true if the texture was actually rebuilt.
    static bool rebuildGlobalPaletteTexture(const Scene& scene, GPUData& gpuData) {
        std::vector<uint32_t> globalPalette;
        gpuData.componentPaletteOffsets.assign(scene.components.size(), 0);
        uint32_t palOff = 0;
        uint64_t totalVersion = 0;

        {
            // Locked against the same mutex internMaterial uses: this loop live-iterates
            // ComponentRecord::materialPalette and reads paletteVersion, both of which a worker
            // thread can now be concurrently mutating via the thread-safe internMaterial -- without
            // this, a concurrent push_back reallocating the vector mid-iteration here would be a
            // genuine data race (iterator invalidation), not just a stale read.
            std::lock_guard<std::mutex> lock(scene.materialPaletteMutex);
            for (size_t h = 0; h < scene.components.size(); ++h) {
                const ComponentRecord& comp = scene.components[h];
                gpuData.componentPaletteOffsets[h] = palOff;
                for (const Material& m : comp.materialPalette)
                    globalPalette.push_back(m.packedColor);
                palOff += static_cast<uint32_t>(comp.materialPalette.size());
                totalVersion += comp.paletteVersion;
            }
        }

        core::info("PAL-CPU: compOffsets[0]={} totalVer={} storedVer={} rebuilt={}",
                   gpuData.componentPaletteOffsets.empty() ? 0xFFFF : gpuData.componentPaletteOffsets[0],
                   totalVersion, gpuData.componentPaletteVersion,
                   (totalVersion != gpuData.componentPaletteVersion || totalVersion == 0));

        if (totalVersion == gpuData.componentPaletteVersion && !globalPalette.empty())
            return false;
        gpuData.componentPaletteVersion = totalVersion;

        if (globalPalette.empty()) return false;

        while (globalPalette.size() % 4 != 0) globalPalette.push_back(0);
        uint32_t pw = static_cast<uint32_t>(globalPalette.size() / 4);
        uint32_t pwPot = nextPowerOfFour(pw);
        while (globalPalette.size() < pwPot * 4) globalPalette.push_back(0);

        if (pwPot != gpuData.paletteWidth || !bgfx::isValid(gpuData.materialPaletteTexture)) {
            if (bgfx::isValid(gpuData.materialPaletteTexture)) bgfx::destroy(gpuData.materialPaletteTexture);
            gpuData.materialPaletteTexture = createDataTexture(pwPot, 1, globalPalette);
        } else {
            const bgfx::Memory* mem = bgfx::copy(globalPalette.data(), globalPalette.size() * sizeof(uint32_t));
            bgfx::updateTexture2D(gpuData.materialPaletteTexture, 0, 0, 0, 0, uint16_t(pwPot), 1, mem);
        }
        core::info("rebuildGlobalPaletteTexture: {} components, {} palette entries, width={}",
                   scene.components.size(), palOff, pwPot);
        core::info("PALETTE-REBUILT: {} components, {} entries, width={} totalVersion={}",
                   scene.components.size(), palOff, pwPot, totalVersion);
        gpuData.paletteWidth = pwPot;
        if (!isPowerOfFour(pwPot)) core::info("PALETTE NOT Po4: width={}", pwPot);
        return true;
    }

    // Not rebuilt: return false so callers know headers don't need rewriting.

    namespace {
        // The colour sitting at a global palette index, resolved back through the per-component
        // offsets the last rebuild recorded. Indices past the final component's palette are the
        // padding rebuildGlobalPaletteTexture adds to reach a whole texel, and read as zero.
        // Caller holds scene.materialPaletteMutex.
        uint32_t globalPaletteEntry(const Scene& scene, const GPUData& gpuData, uint32_t globalIndex) {
            for (size_t h = 0; h < scene.components.size() && h < gpuData.componentPaletteOffsets.size(); ++h) {
                uint32_t offset = gpuData.componentPaletteOffsets[h];
                const std::vector<Material>& palette = scene.components[h].materialPalette;
                if (globalIndex >= offset && globalIndex < offset + palette.size()) {
                    return palette[globalIndex - offset].packedColor;
                }
            }
            return 0;
        }
    }

    void updatePaletteEntry(const Scene& scene, GPUData& gpuData, ComponentHandle componentHandle, uint8_t slot) {
        if (!bgfx::isValid(gpuData.materialPaletteTexture) || gpuData.paletteWidth == 0) {
            return;   // Nothing uploaded yet; the first build will pick the value up.
        }
        if (componentHandle >= gpuData.componentPaletteOffsets.size() ||
            componentHandle >= scene.components.size()) {
            core::warn("updatePaletteEntry: component handle {} has no palette offset yet.", componentHandle);
            return;
        }

        std::lock_guard<std::mutex> lock(scene.materialPaletteMutex);
        if (slot >= scene.components[componentHandle].materialPalette.size()) {
            core::warn("updatePaletteEntry: slot {} out of range for component {}.", slot, componentHandle);
            return;
        }

        // The palette texture is RGBA32U — four entries per texel — so the whole texel is rewritten,
        // including the three neighbours, which may belong to the next component's palette.
        uint32_t globalIndex = gpuData.componentPaletteOffsets[componentHandle] + slot;
        uint32_t texelIndex = globalIndex >> 2;
        uint32_t texel[4];
        for (uint32_t i = 0; i < 4; ++i) {
            texel[i] = globalPaletteEntry(scene, gpuData, (texelIndex << 2) + i);
        }

        uint32_t x = texelIndex % gpuData.paletteWidth;
        uint32_t y = texelIndex / gpuData.paletteWidth;
        const bgfx::Memory* mem = bgfx::copy(texel, sizeof(texel));
        bgfx::updateTexture2D(gpuData.materialPaletteTexture, 0, 0, uint16_t(x), uint16_t(y), 1, 1, mem);

        // The GPU now matches the CPU, so move the watermark rebuildGlobalPaletteTexture compares
        // against. Without this the next flushSceneUpdates would see a version it has not seen,
        // rebuild the entire palette texture, and dirty every chunk header to no effect.
        uint64_t totalVersion = 0;
        for (const ComponentRecord& component : scene.components) {
            totalVersion += component.paletteVersion;
        }
        gpuData.componentPaletteVersion = totalVersion;
    }

    void setTextureToData(std::shared_ptr<ConstructedRenderer> constructedRenderer, uint textureID, unsigned char * data, uint textureWidth, uint textureHeight) {  
        projv::core::ivec2 textureDimensions = constructedRenderer->resources.textures.textureResolutions.at(textureID);  
        bool textureIsResizable = false;
        for(size_t i = 0; i < constructedRenderer->resources.textures.texturesResizedWithResourceTextures.size(); i++) {
            if(constructedRenderer->resources.textures.texturesResizedWithResourceTextures[i] == textureID) {
                textureIsResizable = true;
            }
        }

        if(textureDimensions.x != int(textureWidth) || textureDimensions.y != int(textureHeight)) {
            if(textureIsResizable) {
                std::invalid_argument(
                    "Passed texture dimensions don't match. Expected: (" + 
                    std::to_string(textureDimensions.x) + ", " + std::to_string(textureDimensions.y) + 
                    "), got: (" + std::to_string(textureWidth) + 
                    ", " + 
                    std::to_string(textureHeight) + 
                    "). Since texture is resizable, it will automatically change it's size to match."
                );
            } else {
                throw std::invalid_argument(
                    "Passed texture dimensions don't match. Expected: (" + 
                    std::to_string(textureDimensions.x) + ", " + std::to_string(textureDimensions.y) + 
                    "), got: (" + std::to_string(textureWidth) + 
                    ", " + 
                    std::to_string(textureHeight) + 
                    "). Ensure the resolutions in resources.json and the loaded texture match."
                );
            }
        }
        uint32_t dataSize = textureWidth * textureHeight * 4; // e.g., 4 for RGBA8  
        
        const bgfx::Memory* textureMemory = bgfx::copy(data, dataSize);  
        
        // Then update the texture with this memory  
        bgfx::updateTexture2D(  
            constructedRenderer->resources.textures.textureHandles[textureID],  
            0,      // layer  
            0,      // mip  
            0,      // x  
            0,      // y  
            textureWidth,  // width  
            textureHeight,  // height  
            textureMemory  
        );  
    }

    bgfx::TextureHandle createArbitraryTexture(std::vector<uint32_t>& data) {
        int textureHeight = 4096;
        int maxTextureSize = bgfx::getCaps()->limits.maxTextureSize;
        if(maxTextureSize < textureHeight) {
            textureHeight = maxTextureSize;
        }
        int pixelSize = data.size() / 4;
        int dataWidth = (pixelSize / textureHeight);
        core::info("createArbitraryTexture: Creating texture with height {}px", textureHeight);
        core::info("createArbitraryTexture: Creating texture with width {}px", dataWidth);
        if(pixelSize % textureHeight != 0) {
            dataWidth += 1;
        } 
        const bgfx::Memory* dataMemory = bgfx::copy(data.data(), dataWidth * textureHeight * sizeof(uint32_t) * 4);
        bgfx::TextureHandle dataTexture = bgfx::createTexture2D(dataWidth, textureHeight, false, 1, bgfx::TextureFormat::RGBA32U, BGFX_TEXTURE_NONE|BGFX_SAMPLER_POINT, dataMemory);
        return dataTexture;
    }


    bool verifyTextureWithReadback(bgfx::TextureHandle texture,
                                const std::vector<uint32_t>& originalData,
                                int textureWidth,
                                int textureHeight)
    {
        constexpr uint32_t valuesPerPixel = 3;

        uint32_t pixelCount =
            (originalData.size() + valuesPerPixel - 1) / valuesPerPixel;

        int expectedWidth =
            (pixelCount + textureHeight - 1) / textureHeight;

        if (expectedWidth != textureWidth) {
            core::error("Width mismatch before readback.");
            return false;
        }

        size_t totalElements = size_t(textureWidth) * textureHeight * 4;

        std::vector<uint32_t> gpuData(totalElements, 0);

        // Request readback
        uint32_t frame = bgfx::readTexture(texture, gpuData.data());

        // Wait until frame is ready
        while (bgfx::frame() < frame) {
            // spin until GPU catches up
        }

        // Rebuild expected packed data
        std::vector<uint32_t> expected(totalElements, 0);

        uint32_t src = 0;
        uint32_t dst = 0;

        while (src < originalData.size()) {
            expected[dst + 0] = originalData[src++];

            if (src < originalData.size())
                expected[dst + 1] = originalData[src++];

            if (src < originalData.size())
                expected[dst + 2] = originalData[src++];

            expected[dst + 3] = 0;

            dst += 4;
        }

        // Compare
        for (size_t i = 0; i < totalElements; ++i) {
            if (gpuData[i] != expected[i]) {
                core::error(
                    "Mismatch at {}: GPU={}, CPU={}",
                    i, gpuData[i], expected[i]
                );
                return false;
            }
        }

        core::info("GPU texture matches original data.");
        return true;
    }

    bgfx::TextureHandle createArbitraryTextureRGB(std::vector<uint32_t>& data)
    {
        int textureHeight = 4096;
        int maxTextureSize = bgfx::getCaps()->limits.maxTextureSize;
        if (maxTextureSize < textureHeight) {
            textureHeight = maxTextureSize;
        }

        constexpr uint32_t valuesPerPixel = 3;

        uint32_t pixelCount =
            (data.size() + valuesPerPixel - 1) / valuesPerPixel;

        int dataWidth = pixelCount / textureHeight;
        if (pixelCount % textureHeight != 0) {
            dataWidth += 1;
        }

        core::info("createArbitraryTextureRGB: Creating texture with height {}px", textureHeight);
        core::info("createArbitraryTextureRGB: Creating texture with width {}px", dataWidth);

        // RGBA32U requires 4 uint32 per pixel
        std::vector<uint32_t> packed(dataWidth * textureHeight * 4, 0);

        uint32_t src = 0;
        uint32_t dst = 0;

        while (src < data.size()) {
            // R
            packed[dst + 0] = data[src++];

            // G
            if (src < data.size())
                packed[dst + 1] = data[src++];

            // B
            if (src < data.size())
                packed[dst + 2] = data[src++];

            // A stays 0
            packed[dst + 3] = 0;

            dst += 4;
        }

        const bgfx::Memory* mem =
            bgfx::copy(packed.data(), packed.size() * sizeof(uint32_t));

        bgfx::TextureHandle texture = bgfx::createTexture2D(
            dataWidth,
            textureHeight,
            false,
            1,
            bgfx::TextureFormat::RGBA32U,
            BGFX_TEXTURE_NONE | BGFX_SAMPLER_POINT,
            mem
        );

        return texture;
    }

    // ---- Header texture layout ----
    //
    // One header is 5 consecutive RGBA32U texels (grown from 4 to fit the added traversalLOD +
    // reserved words -- a texel is atomic, so one new field forces a whole new texel). The texture
    // used to be a single row, which capped the scene at maxTextureSize/N chunks on a 32768-wide
    // GPU -- and past that cap chunks silently got no header row (invisible) while
    // updateDirtyHeaders re-entered growHeaderTexture every frame, rebuilding the whole texture. It
    // is now 2D: a fixed row width, growing in rows.
    //
    // The width is held to a multiple of 5 so a header never straddles a row boundary. That keeps a
    // single-header update one updateTexture2D of 5x1, and lets the shader resolve a header with one
    // divide instead of five. The shader derives the same layout from textureSize() -- see headers()
    // in pjv_utils_DDA.sc -- so there is no uniform to keep in sync, but the "width is a multiple of
    // 5" invariant is load-bearing on both sides.
    uint32_t headerTexWidth() {
        uint32_t w = maxTexSize();
        return w - (w % 5u);   // maxTextureSize is a power of two in practice; be safe anyway
    }
    uint32_t headersPerRow() { return headerTexWidth() / 5u; }

    // Largest number of chunk slots the header texture can address.
    uint32_t maxHeaderSlots() {
        return headersPerRow() * maxTexSize();
    }

    bgfx::TextureHandle createHeaderTexture(std::vector<projv::GPUChunkHeader>& headers) {
        const uint32_t hpr = headersPerRow();
        const uint32_t w = headerTexWidth();
        const uint32_t h = std::max<uint32_t>(1u, (static_cast<uint32_t>(headers.size()) + hpr - 1) / hpr);
        checkTextureSize("header", texBytes(w, h, true));

        // Pad to a whole number of rows so the fill below is one contiguous upload. Dead slots are
        // degenerate (scale <= 0), which the shader's broadphase rejects.
        headers.resize(static_cast<size_t>(h) * hpr, degenerateHeader());

        // Create MUTABLE texture (nullptr → mutable, so updateTexture2D works later).
        bgfx::TextureHandle headerTexture = bgfx::createTexture2D(
            uint16_t(w), uint16_t(h), false, 1, bgfx::TextureFormat::RGBA32U,
            BGFX_SAMPLER_POINT, nullptr);
        if (!bgfx::isValid(headerTexture)) {
            core::info("ERROR: createHeaderTexture({}x{}) failed — out of VRAM?", w, h);
            return BGFX_INVALID_HANDLE;
        }

        const bgfx::Memory* headerMemory = bgfx::copy(
            headers.data(),
            headers.size() * sizeof(projv::GPUChunkHeader));
        bgfx::updateTexture2D(headerTexture, 0, 0, 0, 0, uint16_t(w), uint16_t(h), headerMemory);
        return headerTexture;
    }

    // Blit one header row in place. Single point of truth for header->(x,y) addressing.
    void writeHeaderTexel(bgfx::TextureHandle tex, uint32_t slot, const projv::GPUChunkHeader& hdr) {
        const uint32_t hpr = headersPerRow();
        uint16_t x = static_cast<uint16_t>((slot % hpr) * 5u);
        uint16_t y = static_cast<uint16_t>(slot / hpr);
        const bgfx::Memory* mem = bgfx::copy(&hdr, sizeof(hdr));
        bgfx::updateTexture2D(tex, 0, 0, x, y, 5, 1, mem);
    }

    // Builds a 1-row RGBA32U texture from a raw uint list (4 uints per texel). Used for the
    // small grid-info table, read in the shader via texelFetch(gridInfo, ivec2(texel, 0)).
    bgfx::TextureHandle createUintRowTexture(std::vector<uint32_t> data) {
        while (data.size() % 4 != 0) data.push_back(0);
        if (data.empty()) data.resize(4, 0);
        int width = static_cast<int>(data.size() / 4);
        bgfx::TextureHandle tex = bgfx::createTexture2D(width, 1, false, 1, bgfx::TextureFormat::RGBA32U,
                                                         BGFX_SAMPLER_POINT, nullptr);
        const bgfx::Memory* mem = bgfx::copy(data.data(), data.size() * sizeof(uint32_t));
        bgfx::updateTexture2D(tex, 0, 0, 0, 0, uint16_t(width), 1, mem);
        return tex;
    }

    // Builds the flattened cell -> chunk map as an RGBA32U texture. Same 2D layout as
    // createArbitraryTexture (so the shader cellMap() accessor mirrors voxelTypeDatas()),
    // but padded to the full texture size to avoid over-reading the source.
    bgfx::TextureHandle createCellMapTexture(std::vector<uint32_t> data) {
        if (data.empty()) data.push_back(0xFFFFFFFFu);
        int textureHeight = 4096;
        int maxTextureSize = bgfx::getCaps()->limits.maxTextureSize;
        if (maxTextureSize < textureHeight) textureHeight = maxTextureSize;
        int pixelSize = static_cast<int>((data.size() + 3) / 4);
        int dataWidth = (pixelSize + textureHeight - 1) / textureHeight;
        if (dataWidth < 1) dataWidth = 1;
        data.resize(static_cast<size_t>(dataWidth) * textureHeight * 4, 0xFFFFFFFFu);
        checkTextureSize("cellMap", texBytes(dataWidth, textureHeight, true));
        bgfx::TextureHandle tex = bgfx::createTexture2D(dataWidth, textureHeight, false, 1,
                                                         bgfx::TextureFormat::RGBA32U,
                                                         BGFX_SAMPLER_POINT, nullptr);
        if (!bgfx::isValid(tex)) {
            core::info("ERROR: createCellMapTexture({}x{}) failed — out of VRAM?",
                      dataWidth, textureHeight);
            return BGFX_INVALID_HANDLE;
        }
        const bgfx::Memory* mem = bgfx::copy(data.data(), data.size() * sizeof(uint32_t));
        bgfx::updateTexture2D(tex, 0, 0, 0, 0, uint16_t(dataWidth), uint16_t(textureHeight), mem);
        return tex;
    }

    namespace {
        // Compute the loose-handle list the shader iterates. Compose scenes carry it explicitly (may
        // be empty when every chunk is gridded); a legacy scene with neither loose list nor grids
        // treats all live chunks as loose.
        std::vector<uint32_t> computeLooseList(const projv::Scene& scene) {
            std::vector<uint32_t> loose;
            if (scene.grids.empty() && scene.looseChunks.empty()) {
                for (uint32_t h = 0; h < scene.chunks.size(); h++)
                    if (scene.chunks[h].alive) loose.push_back(h);
            } else {
                loose.assign(scene.looseChunks.begin(), scene.looseChunks.end());
            }
            return loose;
        }

        // (Re)build the three small scene tables (gridInfo counts+descriptors, cellMap, looseList)
        // from current CPU state.
        void syncSceneTables(projv::Scene& scene, GPUData& gpuData) {
            auto t0 = std::chrono::high_resolution_clock::now();
            std::vector<uint32_t> looseList = computeLooseList(scene);
            gpuData.looseCount = static_cast<uint32_t>(looseList.size());

            std::vector<uint32_t> gridInfoData, cellMapData;
            auto pushU = [](std::vector<uint32_t>& v, uint32_t u) { v.push_back(u); };
            auto pushF = [](std::vector<uint32_t>& v, float f) { uint32_t u; std::memcpy(&u, &f, 4); v.push_back(u); };
            pushU(gridInfoData, static_cast<uint32_t>(scene.grids.size()));
            pushU(gridInfoData, gpuData.looseCount);
            pushU(gridInfoData, static_cast<uint32_t>(scene.chunks.size()));
            pushU(gridInfoData, 0);
            for (const projv::SceneGrid& g : scene.grids) {
                uint32_t cellMapOffset = static_cast<uint32_t>(cellMapData.size());
                pushF(gridInfoData, g.origin.x); pushF(gridInfoData, g.origin.y); pushF(gridInfoData, g.origin.z);
                pushF(gridInfoData, g.cellSize);
                pushU(gridInfoData, static_cast<uint32_t>(g.dims.x));
                pushU(gridInfoData, static_cast<uint32_t>(g.dims.y));
                pushU(gridInfoData, static_cast<uint32_t>(g.dims.z));
                pushU(gridInfoData, cellMapOffset);
                pushF(gridInfoData, g.rotation.x); pushF(gridInfoData, g.rotation.y);
                pushF(gridInfoData, g.rotation.z); pushF(gridInfoData, g.rotation.w);
                // Sized up front and written by index: push_back over a million cells reallocates
                // its way up from nothing on every single call.
                size_t base = cellMapData.size();
                cellMapData.resize(base + g.cellToChunk.size());
                for (size_t i = 0; i < g.cellToChunk.size(); i++) {
                    int32_t c = g.cellToChunk[i];
                    cellMapData[base + i] = c < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(c);
                }
            }

            gpuData.looseCapacity = static_cast<uint32_t>(looseList.size());
            if (!bgfx::isValid(gpuData.headerTexture)) { auto t1 = std::chrono::high_resolution_clock::now(); double ms = std::chrono::duration<double, std::milli>(t1 - t0).count(); core::perf("syncSceneTables (headless): {:.2f}ms", ms); return; }

            // --- GridInfo (1-row) ---
            {
                while (gridInfoData.size() % 4 != 0) gridInfoData.push_back(0);
                if (gridInfoData.empty()) gridInfoData.resize(4, 0);
                uint32_t neededW = static_cast<uint32_t>(gridInfoData.size() / 4);
                if (neededW != gpuData.gridInfoTexWidth || !bgfx::isValid(gpuData.gridInfoTexture)) {
                    if (bgfx::isValid(gpuData.gridInfoTexture)) bgfx::destroy(gpuData.gridInfoTexture);
                    gpuData.gridInfoTexture = createUintRowTexture(gridInfoData);
                    gpuData.gridInfoTexWidth = neededW;
                } else {
                    const bgfx::Memory* mem = bgfx::copy(gridInfoData.data(), gridInfoData.size() * sizeof(uint32_t));
                    bgfx::updateTexture2D(gpuData.gridInfoTexture, 0, 0, 0, 0, uint16_t(neededW), 1, mem);
                }
            }

            // --- CellMap (2D) ---
            {
                if (cellMapData.empty()) cellMapData.push_back(0xFFFFFFFFu);
                int texH = 4096;
                int maxSz = bgfx::getCaps()->limits.maxTextureSize;
                if (maxSz < texH) texH = maxSz;
                int pixelSize = static_cast<int>((cellMapData.size() + 3) / 4);
                uint32_t neededW = static_cast<uint32_t>((pixelSize + texH - 1) / texH);
                if (neededW < 1) neededW = 1;
                uint32_t neededH = static_cast<uint32_t>(texH);
                if (neededW != gpuData.cellMapTexWidth || neededH != gpuData.cellMapTexHeight || !bgfx::isValid(gpuData.cellMapTexture)) {
                    if (bgfx::isValid(gpuData.cellMapTexture)) bgfx::destroy(gpuData.cellMapTexture);
                    gpuData.cellMapTexture = createCellMapTexture(cellMapData);
                    gpuData.cellMapTexWidth = neededW;
                    gpuData.cellMapTexHeight = neededH;
                } else {
                    // NOTE (perf): this rebuilds and re-uploads the entire cellMap on every flush,
                    // and the cellMap is sized by the grid's CELL COUNT rather than by how many
                    // cells are occupied -- a 221x22x221 streaming grid is 1.07M cells / 4.3MB from
                    // the very first frame. Measured at ~1.1ms per flush in the terrain example,
                    // constant, whether one cell changed or none did.
                    //
                    // Diffing the rebuilt buffer against a cached copy was tried and did not pay:
                    // the rebuild and the comparison together cost about what the upload did, and
                    // during streaming the changed rows span most of the texture anyway (a grid
                    // cell's linear index is unrelated to its 3D proximity to the camera, so a
                    // handful of nearby admissions scatter across the whole table). The fix that
                    // would actually work is dirty-cell tracking at the SceneGrid level -- record
                    // the touched index range where cellToChunk is written -- which is a change to
                    // the grid's write sites, not to this function.
                    cellMapData.resize(static_cast<size_t>(neededW) * neededH * 4, 0xFFFFFFFFu);
                    const bgfx::Memory* mem = bgfx::copy(cellMapData.data(), cellMapData.size() * sizeof(uint32_t));
                    bgfx::updateTexture2D(gpuData.cellMapTexture, 0, 0, 0, 0, uint16_t(neededW), uint16_t(neededH), mem);
                }
            }

            // --- LooseList (2D) ---
            {
                if (looseList.empty()) looseList.push_back(0xFFFFFFFFu);
                int texH = 4096;
                int maxSz = bgfx::getCaps()->limits.maxTextureSize;
                if (maxSz < texH) texH = maxSz;
                int pixelSize = static_cast<int>((looseList.size() + 3) / 4);
                uint32_t neededW = static_cast<uint32_t>((pixelSize + texH - 1) / texH);
                if (neededW < 1) neededW = 1;
                uint32_t neededH = static_cast<uint32_t>(texH);
                if (neededW != gpuData.looseListTexWidth || neededH != gpuData.looseListTexHeight || !bgfx::isValid(gpuData.looseListTexture)) {
                    if (bgfx::isValid(gpuData.looseListTexture)) bgfx::destroy(gpuData.looseListTexture);
                    gpuData.looseListTexture = createCellMapTexture(looseList);
                    gpuData.looseListTexWidth = neededW;
                    gpuData.looseListTexHeight = neededH;
                } else {
                    looseList.resize(static_cast<size_t>(neededW) * neededH * 4, 0xFFFFFFFFu);
                    const bgfx::Memory* mem = bgfx::copy(looseList.data(), looseList.size() * sizeof(uint32_t));
                    bgfx::updateTexture2D(gpuData.looseListTexture, 0, 0, 0, 0, uint16_t(neededW), uint16_t(neededH), mem);
                }
            }

            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            core_perf_every(60, "syncSceneTables: {} grids {} loose {} chunks: {:.2f}ms",
                       scene.grids.size(), looseList.size(), scene.chunks.size(), ms);
        }
    }

    // ======================== P5 — Incremental GPU upload ========================

    // Upload a contiguous span of RGBA32U texels into a 2D texture, handling row wrapping.
    // `packed` contains the full blob's packed RGBA texels (4 uint32 per texel).
    // `texelOffset` = 1D start texel in the texture; `texelCount` = how many texels.
    static void uploadTexelSpan(bgfx::TextureHandle tex, uint32_t texWidth,
                                 const std::vector<uint32_t>& packed,
                                 uint32_t texelOffset, uint32_t texelCount) {
        if (texelCount == 0 || !bgfx::isValid(tex)) return;

        auto t0 = std::chrono::high_resolution_clock::now();

        uint32_t remaining = texelCount;
        uint32_t srcIdx = 0;           // index of first texel in packed (for this blob)
        uint16_t col = static_cast<uint16_t>(texelOffset % texWidth);
        uint16_t row = static_cast<uint16_t>(texelOffset / texWidth);
        uint16_t maxCol = static_cast<uint16_t>(texWidth);

        while (remaining > 0) {
            uint16_t avail = static_cast<uint16_t>(maxCol - col);
            uint16_t thisRow = static_cast<uint16_t>(std::min<uint32_t>(remaining, avail));

            // Source data: packed[srcIdx*4 .. srcIdx*4 + thisRow*4)
            const bgfx::Memory* mem = bgfx::copy(
                &packed[static_cast<size_t>(srcIdx) * 4],
                static_cast<uint32_t>(thisRow) * 4 * sizeof(uint32_t));

            bgfx::updateTexture2D(tex, 0, 0, col, row, thisRow, 1, mem);

            srcIdx += thisRow;
            remaining -= thisRow;
            col = 0;
            row++;
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core::perf("uploadTexelSpan: {} texels across {} rows: {:.2f}ms",
                   texelCount, (row - static_cast<uint16_t>(texelOffset / texWidth)), ms);
    }

    static void uploadTexelSpan8(bgfx::TextureHandle tex, uint32_t texWidth,
                                 const std::vector<uint8_t>& packed,
                                 uint32_t texelOffset, uint32_t texelCount) {
        if (texelCount == 0 || !bgfx::isValid(tex)) return;
        uint32_t remaining = texelCount;
        uint32_t srcIdx = 0;
        uint16_t col = static_cast<uint16_t>(texelOffset % texWidth);
        uint16_t row = static_cast<uint16_t>(texelOffset / texWidth);
        uint16_t maxCol = static_cast<uint16_t>(texWidth);
        while (remaining > 0) {
            uint16_t avail = static_cast<uint16_t>(maxCol - col);
            uint16_t thisRow = static_cast<uint16_t>(std::min<uint32_t>(remaining, avail));
            uint32_t byteLen = static_cast<uint32_t>(thisRow) * 4 * sizeof(uint8_t);
            if (byteLen == 0) break;
            const bgfx::Memory* mem = bgfx::copy(
                &packed[static_cast<size_t>(srcIdx) * 4],
                byteLen);
            bgfx::updateTexture2D(tex, 0, 0, col, row, thisRow, 1, mem);
            srcIdx += thisRow;
            remaining -= thisRow;
            col = 0;
            row++;
        }
    }

    // Full repack of all live blobs into fresh contiguous ranges. Destroys and recreates the
    // data textures (tree64 + voxelType) with headroom, seeds the allocators, clears all dirty
    // flags, and rebuilds all blobRanges. Called only when the allocator is full (rare — amortized
    // O(1) grows per edit due to withHeadroom slack). Does NOT touch the header texture or scene
    // tables — the caller handles those.
    //
    // P6: per-blob over-allocation (paddedAlloc) rounds up each blob's GPU range so in-place COW
    // edits that grow the blob slightly fit within the existing allocation. The padded total is
    // used for withHeadroom so free space scales proportionally.
    static void growDataTextures(projv::Scene& scene, GPUData& gpuData) {
        auto t0 = std::chrono::high_resolution_clock::now();

        gpuData.blobRanges.assign(scene.geometryPool.size(), GPUBlobRange{});
        std::vector<uint32_t> tree64Texels; std::vector<uint8_t> materialIDTexels;
        uint32_t geomUsed = 0, matUsed = 0;
        uint32_t geomUsedPadded = 0, matUsedPadded = 0;
        uint32_t liveBlobs = 0;
        // Reused across blobs; see the matching scratch in uploadDirtyBlobs.
        std::vector<uint32_t> lodGeomScratch;
        std::vector<uint8_t> lodMatScratch;

        for (size_t b = 0; b < scene.geometryPool.size(); b++) {
            const GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) continue;
            liveBlobs++;

            const std::vector<uint32_t>* geomSrc = nullptr;
            const std::vector<uint8_t>* matSrc = nullptr;
            effectiveBlobData(blob, lodGeomScratch, lodMatScratch, geomSrc, matSrc);

            uint32_t nodes = static_cast<uint32_t>(geomSrc->size() / 3);
            uint32_t matTexels = static_cast<uint32_t>((matSrc->size() + 3) / 4);
            uint32_t gAlloc = paddedAlloc(nodes);
            uint32_t mAlloc = paddedAlloc(matTexels);

            GPUBlobRange rng{};
            rng.geomTexelOffset    = geomUsedPadded;
            rng.geomTexelLen       = nodes;
            rng.geomTexelAllocated = gAlloc;
            rng.matTexelOffset     = matUsedPadded;
            rng.matTexelLen        = matTexels;
            rng.matTexelAllocated  = mAlloc;
            rng.matByteLen         = static_cast<uint32_t>(matSrc->size());
            rng.uploadedLOD        = blob.renderLOD;
            rng.uploaded           = true;
            gpuData.blobRanges[b]  = rng;

            std::vector<uint32_t> gt = packGeometryTexels(*geomSrc);
            std::vector<uint8_t> mt = packMaterialIDTexels8(*matSrc);
            tree64Texels.insert(tree64Texels.end(), gt.begin(), gt.end());
            materialIDTexels.insert(materialIDTexels.end(), mt.begin(), mt.end());
            tree64Texels.insert(tree64Texels.end(), static_cast<size_t>(gAlloc - nodes) * 4, 0u);
            materialIDTexels.insert(materialIDTexels.end(), static_cast<size_t>(mAlloc - matTexels) * 4, 0u);

            geomUsed += nodes;
            matUsed += matTexels;
            geomUsedPadded += gAlloc;
            matUsedPadded += mAlloc;
        }

        uint32_t maxSz = maxTexSize();
        uint32_t prevGeomW = gpuData.tree64Width,     prevGeomH = gpuData.tree64Height;
        uint32_t prevMatW  = gpuData.materialIDWidth, prevMatH  = gpuData.materialIDHeight;
        uint64_t budget = dataTextureBudget(gpuData);
        uint32_t geomCap = fitDataDims(uint64_t(double(budget) * kGeomByteShare), 16u,
                                       gpuData.tree64Width, gpuData.tree64Height, maxSz);
        uint32_t matCap = fitDataDims(uint64_t(double(budget) * (1.0 - kGeomByteShare)), 4u,
                                      gpuData.materialIDWidth, gpuData.materialIDHeight, maxSz);

        // A repack cannot recover from a layout that exceeds capacity — it is already as tight as it
        // gets. Say so plainly; seedAllocator then keeps it from corrupting what is already correct.
        if (geomUsedPadded > geomCap || matUsedPadded > matCap) {
            core::error("growDataTextures: scene exceeds data texture capacity — geometry {}/{} ({:.0f}%), "
                        "material {}/{} ({:.0f}%). Reduce view radius or raise kVramFraction.",
                        geomUsedPadded, geomCap, 100.0 * geomUsedPadded / geomCap,
                        matUsedPadded, matCap, 100.0 * matUsedPadded / matCap);
        }

        // Reuse each texture when its dimensions are unchanged (the steady state once the budget has
        // settled) and only rewrite the contents. See refillDataTexture: destroy+create would hold
        // both allocations at once, which multi-GB data textures cannot afford.
        if (bgfx::isValid(gpuData.tree64Texture) &&
            prevGeomW == gpuData.tree64Width && prevGeomH == gpuData.tree64Height) {
            refillDataTexture(gpuData.tree64Texture, gpuData.tree64Width, gpuData.tree64Height, tree64Texels);
        } else {
            if (bgfx::isValid(gpuData.tree64Texture)) bgfx::destroy(gpuData.tree64Texture);
            gpuData.tree64Texture = createDataTexture(gpuData.tree64Width, gpuData.tree64Height, tree64Texels);
        }
        if (bgfx::isValid(gpuData.materialIDTexture) &&
            prevMatW == gpuData.materialIDWidth && prevMatH == gpuData.materialIDHeight) {
            refillDataTexture8(gpuData.materialIDTexture, gpuData.materialIDWidth, gpuData.materialIDHeight, materialIDTexels);
        } else {
            if (bgfx::isValid(gpuData.materialIDTexture)) bgfx::destroy(gpuData.materialIDTexture);
            gpuData.materialIDTexture = createDataTexture8(gpuData.materialIDWidth, gpuData.materialIDHeight, materialIDTexels);
        }

        // Palette texture — built from component palettes.
        rebuildGlobalPaletteTexture(scene, gpuData);

        seedAllocator(gpuData.tree64Alloc, geomCap, geomUsedPadded, "growDataTextures/tree64");
        seedAllocator(gpuData.materialIDAlloc, matCap, matUsedPadded, "growDataTextures/materialID");

        for (GeometryBlob& blob : scene.geometryPool)
            blob.dirty = false;

        core::info("GROW-DATA: liveBlobs={} geom={}/{} ({:.1f}%) mat={}/{} ({:.1f}%) "
                   "geomTex={}x{} matTex={}x{}",
                   liveBlobs, geomUsedPadded, geomCap, 100.0 * geomUsedPadded / geomCap,
                   matUsedPadded, matCap, 100.0 * matUsedPadded / matCap,
                   gpuData.tree64Width, gpuData.tree64Height,
                   gpuData.materialIDWidth, gpuData.materialIDHeight);
        validateBlobRanges(scene, gpuData, "growDataTextures");

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core::perf("growDataTextures: {} geomUsed {} matUsed dims ({}x{})/({}x{}): {:.2f}ms, vram=~{:.1f}GB",
                   geomUsed, matUsed,
                   gpuData.tree64Width, gpuData.tree64Height,
                   gpuData.materialIDWidth, gpuData.materialIDHeight, ms,
                   static_cast<double>(texBytes(gpuData.tree64Width, gpuData.tree64Height, true) +
                                      texBytes(gpuData.materialIDWidth, gpuData.materialIDHeight, false)) / 1e9);
    }

    // Upload only blobs flagged dirty (new forks / interned chunks) to their existing or newly
    // allocated GPU ranges. Returns the set of pool indices that were just uploaded (for header
    // rewrite). Returns std::nullopt when the allocator is full (caller should fall back to grow).
    //
    // P6: per-blob over-allocation. Allocations use paddedAlloc() so in-place edits that grow the
    // blob slightly fit without reallocation. The reallocation check compares against the *allocated*
    // size, not the used size. When freeing, the padded allocated length is returned, not the used.
    static std::optional<std::unordered_set<uint32_t>> uploadDirtyBlobs(projv::Scene& scene, GPUData& gpuData) {
        auto t0 = std::chrono::high_resolution_clock::now();
        std::unordered_set<uint32_t> uploadedPools;
        uint32_t dirtyUploaded = 0;

        if (gpuData.blobRanges.size() < scene.geometryPool.size())
            gpuData.blobRanges.resize(scene.geometryPool.size());

        // Rebuild the global palette texture from every component's materialPalette.
        // If the palette texture changed (new components, palette growth), mark all
        // chunks headerDirty so updateDirtyHeaders rewrites every header row with
        // the current componentPaletteOffsets.
        bool paletteRebuilt = rebuildGlobalPaletteTexture(scene, gpuData);
        if (paletteRebuilt) {
            for (Chunk& c : scene.chunks) {
                if (c.alive) c.headerDirty = true;
            }
        }

        // Reused across blobs so a flush that coarsens many chunks does not reallocate per blob.
        std::vector<uint32_t> lodGeomScratch;
        std::vector<uint8_t> lodMatScratch;

        for (size_t b = 0; b < scene.geometryPool.size(); b++) {
            GeometryBlob& blob = scene.geometryPool[b];
            GPUBlobRange& r = gpuData.blobRanges[b];

            if (blob.refCount == 0 && r.uploaded) {
                gpuData.tree64Alloc.free(r.geomTexelOffset, r.geomTexelAllocated);
                gpuData.materialIDAlloc.free(r.matTexelOffset, r.matTexelAllocated);
                r.uploaded = false;
                continue;
            }
            if (blob.refCount == 0) continue;
            if (!blob.dirty) continue;

            const std::vector<uint32_t>* geomSrc = nullptr;
            const std::vector<uint8_t>* matSrc = nullptr;
            effectiveBlobData(blob, lodGeomScratch, lodMatScratch, geomSrc, matSrc);

            uint32_t nodes = static_cast<uint32_t>(geomSrc->size() / 3);
            uint32_t matTexels = static_cast<uint32_t>((matSrc->size() + 3) / 4);
            uint32_t matBytes = static_cast<uint32_t>(matSrc->size());

            // An LOD change forces a realloc even when the blob shrank. The normal test only fires
            // on growth, so without this a demoted blob would keep its old oversized range until
            // the next full repack -- and reclaiming that VRAM is the entire point of the LOD. The
            // condition is deliberately scoped to LOD changes so the editing path's realloc
            // behaviour (and its 25% growth slack) is left alone.
            bool lodChanged = r.uploaded && r.uploadedLOD != blob.renderLOD;
            bool needRealloc = !r.uploaded || nodes > r.geomTexelAllocated
                               || matTexels > r.matTexelAllocated || lodChanged;

            if (needRealloc) {
                if (r.uploaded) {
                    gpuData.tree64Alloc.free(r.geomTexelOffset, r.geomTexelAllocated);
                    gpuData.materialIDAlloc.free(r.matTexelOffset, r.matTexelAllocated);
                }
                uint32_t gAlloc = paddedAlloc(nodes);
                uint32_t mAlloc = paddedAlloc(matTexels);
                uint32_t gOff = gpuData.tree64Alloc.alloc(gAlloc);
                uint32_t mOff = gpuData.materialIDAlloc.alloc(mAlloc);
                if (gOff == RangeAllocator::INVALID || mOff == RangeAllocator::INVALID) {
                    // Hand back whichever half succeeded. The caller falls back to growDataTextures,
                    // which resets both allocators wholesale, but bailing without this leaks the
                    // range on any path that ever retries without a full repack.
                    if (gOff != RangeAllocator::INVALID) gpuData.tree64Alloc.free(gOff, gAlloc);
                    if (mOff != RangeAllocator::INVALID) gpuData.materialIDAlloc.free(mOff, mAlloc);
                    core::perf("uploadDirtyBlobs: allocator full at blob {} (geom={} mat={})",
                               b, nodes, matTexels);
                    return std::nullopt;
                }
                r.geomTexelOffset = gOff;
                r.geomTexelLen = nodes;
                r.geomTexelAllocated = gAlloc;
                r.matTexelOffset = mOff;
                r.matTexelLen = matTexels;
                r.matTexelAllocated = mAlloc;
                r.matByteLen = matBytes;
                r.uploaded = true;
            } else {
                r.geomTexelLen = nodes;
                r.matTexelLen = matTexels;
                r.matByteLen = matBytes;
            }
            // Record what is now resident, so makeHeader advertises the matching resolution.
            r.uploadedLOD = blob.renderLOD;

            std::vector<uint32_t> geomPacked = packGeometryTexels(*geomSrc);
            uploadTexelSpan(gpuData.tree64Texture, gpuData.tree64Width,
                           geomPacked, r.geomTexelOffset, r.geomTexelLen);

            // Per-blob material IDs are stored as local indices (0..N-1) in the texture.
            // The shader adds h.padding[1] (palette offset) to get a global palette index.
            // packMaterialIDTexels8 packs them as raw bytes — no remapping needed.
            std::vector<uint8_t> matPacked = packMaterialIDTexels8(*matSrc);
            uploadTexelSpan8(gpuData.materialIDTexture, gpuData.materialIDWidth,
                           matPacked, r.matTexelOffset, r.matTexelLen);

            blob.dirty = false;
            uploadedPools.insert(static_cast<uint32_t>(b));
            dirtyUploaded++;
        }

#if defined(PROJV_ENABLE_RENDER)
        core::info("FLUSH: blobs={} liveBlobs={} dirtyUploaded={} paletteW={} paletteVer={} chunks={} compPalettes={}",
                   scene.geometryPool.size(), uploadedPools.size(), dirtyUploaded,
                   gpuData.paletteWidth, gpuData.componentPaletteVersion,
                   scene.chunks.size(), gpuData.componentPaletteOffsets.size());
#endif
        validateBlobRanges(scene, gpuData, "uploadDirtyBlobs");

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core_perf_every(60, "uploadDirtyBlobs: {} dirty blobs: {:.2f}ms", dirtyUploaded, ms);
        return uploadedPools;
    }

    // Recreate the header texture with larger capacity (grow fallback). Rewrites all rows from
    // current scene.chunks state. Does not touch the data textures.
    // With pre-allocation at maxSlots, this should only run during initial setup.
    static void growHeaderTexture(projv::Scene& scene, GPUData& gpuData) {
        uint32_t maxSlots = maxHeaderSlots();
        uint32_t newCap = std::min(withHeadroom(static_cast<uint32_t>(scene.chunks.size())), maxSlots);
        if (newCap <= gpuData.headerCapacity) newCap = gpuData.headerCapacity + 1024;
        if (newCap > maxSlots) newCap = maxSlots;
        // Past this the scene simply cannot be addressed; say so once rather than silently
        // dropping chunks (which is what the old single-row texture did at 8192).
        if (scene.chunks.size() > maxSlots) {
            core::error("growHeaderTexture: {} chunks exceeds the {} addressable header slots — "
                        "chunks past that will not render", scene.chunks.size(), maxSlots);
        }

        if (bgfx::isValid(gpuData.headerTexture)) bgfx::destroy(gpuData.headerTexture);
        gpuData.headerCapacity = newCap;

        std::vector<GPUChunkHeader> headers(newCap, degenerateHeader());
        for (ChunkHandle h = 0; h < scene.chunks.size() && h < newCap; h++) {
            const Chunk& c = scene.chunks[h];
            if (c.alive && c.geometryPoolIndex >= 0 &&
                static_cast<size_t>(c.geometryPoolIndex) < gpuData.blobRanges.size()) {
                headers[h] = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene, gpuData);
            }
        }
        gpuData.headerTexture = createHeaderTexture(headers);
        if (newCap > gpuData.uploadedChunkCount)
            gpuData.uploadedChunkCount = static_cast<uint32_t>(scene.chunks.size());
        core::perf("growHeaderTexture: new capacity {}", newCap);
    }

    // Rewrite the GPU header row for chunks that are new or whose geometryPoolIndex points at a
    // freshly uploaded blob. `uploadedPools` is the set of pool indices uploaded this cycle
    // (returned by uploadDirtyBlobs). Grows the header texture if chunk count exceeds capacity.
    static void updateDirtyHeaders(projv::Scene& scene, GPUData& gpuData,
                                    const std::unordered_set<uint32_t>& uploadedPools) {
        auto t0 = std::chrono::high_resolution_clock::now();
        uint32_t updated = 0;

        for (ChunkHandle h = 0; h < scene.chunks.size(); h++) {
            const Chunk& c = scene.chunks[h];
            if (!c.alive) continue;

            bool needsUpdate = (h >= gpuData.uploadedChunkCount);  // brand new chunk
            if (!needsUpdate && c.headerDirty) {
                needsUpdate = true;                       // header changed (P6 transform)
            }
            if (!needsUpdate && c.geometryPoolIndex >= 0) {
                // Existing chunk — check if its pool blob was just uploaded.
                uint32_t pIdx = static_cast<uint32_t>(c.geometryPoolIndex);
                if (uploadedPools.find(pIdx) != uploadedPools.end())
                    needsUpdate = true;
            }

            if (!needsUpdate) continue;

            if (h >= gpuData.headerCapacity) {
                // Already at the addressable ceiling: growing cannot help, and re-entering it every
                // frame would rebuild the whole texture every frame. Skip the chunk instead.
                if (gpuData.headerCapacity >= maxHeaderSlots()) continue;
                growHeaderTexture(scene, gpuData);
                // grow rewrote all headers; we're done.
                auto t1 = std::chrono::high_resolution_clock::now();
                double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                core::perf("updateDirtyHeaders (via grow): all {} rows: {:.2f}ms", scene.chunks.size(), ms);
                return;
            }

            GPUChunkHeader hdr = degenerateHeader();
            if (c.geometryPoolIndex >= 0 &&
                static_cast<size_t>(c.geometryPoolIndex) < gpuData.blobRanges.size()) {
                hdr = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene, gpuData);
            }

            writeHeaderTexel(gpuData.headerTexture, h, hdr);
#if defined(PROJV_ENABLE_RENDER)
            if (h < 64) // sample first 64 chunks each flush
                core::info("HEADER h={} matStart={} palOff={}",
                           h, hdr.geometryStartIndex, hdr.materialIDStartIndex, hdr.paletteOffset);
#endif
            scene.chunks[h].headerDirty = false;   // P6: clear after sync
            updated++;
        }

        // Advance the watermark to cover all existing chunks.
        if (gpuData.uploadedChunkCount < scene.chunks.size())
            gpuData.uploadedChunkCount = static_cast<uint32_t>(scene.chunks.size());

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core_perf_every(60, "updateDirtyHeaders: {} rows: {:.2f}ms", updated, ms);
    }

    // Build (or rebuild) the tree64/voxelType/header textures from current scene state via one contiguous
    // bulk pack + a single createTexture2D per texture. Reassigns every blob's GPU range and reseeds the
    // allocators. Destroys the old data/header textures first; leaves samplers and the small scene tables alone.
    static void buildDataAndHeaderTextures(projv::Scene& scene, GPUData& gpuData) {
        auto t0 = std::chrono::high_resolution_clock::now();
        gpuData.blobRanges.assign(scene.geometryPool.size(), GPUBlobRange{});
        std::vector<uint32_t> tree64Texels; std::vector<uint8_t> materialIDTexels;
        uint32_t geomUsed = 0, matUsed = 0;
        uint32_t geomUsedPadded = 0, matUsedPadded = 0;
        uint32_t liveBlobs = 0;
        // Reused across blobs; see the matching scratch in uploadDirtyBlobs.
        std::vector<uint32_t> lodGeomScratch;
        std::vector<uint8_t> lodMatScratch;
        for (size_t b = 0; b < scene.geometryPool.size(); b++) {
            const GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) { gpuData.blobRanges[b].uploaded = false; continue; }
            liveBlobs++;
            const std::vector<uint32_t>* geomSrc = nullptr;
            const std::vector<uint8_t>* matSrc = nullptr;
            effectiveBlobData(blob, lodGeomScratch, lodMatScratch, geomSrc, matSrc);
            uint32_t nodes = static_cast<uint32_t>(geomSrc->size() / 3);
            uint32_t matTexels = static_cast<uint32_t>((matSrc->size() + 3) / 4);
            uint32_t gAlloc = paddedAlloc(nodes);
            uint32_t mAlloc = paddedAlloc(matTexels);
            GPUBlobRange rng{};
            rng.geomTexelOffset    = geomUsedPadded;
            rng.geomTexelLen       = nodes;
            rng.geomTexelAllocated = gAlloc;
            rng.matTexelOffset     = matUsedPadded;
            rng.matTexelLen        = matTexels;
            rng.matTexelAllocated  = mAlloc;
            rng.matByteLen         = static_cast<uint32_t>(matSrc->size());
            rng.uploadedLOD        = blob.renderLOD;
            rng.uploaded           = true;
            gpuData.blobRanges[b]  = rng;
            std::vector<uint32_t> gt = packGeometryTexels(*geomSrc);
            std::vector<uint8_t> mt = packMaterialIDTexels8(*matSrc);
            tree64Texels.insert(tree64Texels.end(), gt.begin(), gt.end());
            materialIDTexels.insert(materialIDTexels.end(), mt.begin(), mt.end());
            tree64Texels.insert(tree64Texels.end(), static_cast<size_t>(gAlloc - nodes) * 4, 0u);
            materialIDTexels.insert(materialIDTexels.end(), static_cast<size_t>(mAlloc - matTexels) * 4, 0u);
            geomUsed += nodes;
            matUsed += matTexels;
            geomUsedPadded += gAlloc;
            matUsedPadded += mAlloc;
        }

        // Size the data textures from the VRAM budget so they do not need recreating as the scene
        // grows. At first-build time bgfx cannot report GPU memory yet (no frame submitted), so this
        // lands on the fallback; the first growDataTextures re-queries and repacks into the real
        // budget. The shader's texelFetch needs a power-of-2 width, which fitDataDims guarantees.
        uint32_t maxSz = maxTexSize();
        uint64_t budget = dataTextureBudget(gpuData);
        uint32_t geomCap = fitDataDims(uint64_t(double(budget) * kGeomByteShare), 16u,
                                       gpuData.tree64Width, gpuData.tree64Height, maxSz);
        uint32_t matCap = fitDataDims(uint64_t(double(budget) * (1.0 - kGeomByteShare)), 4u,
                                      gpuData.materialIDWidth, gpuData.materialIDHeight, maxSz);
        if (bgfx::isValid(gpuData.tree64Texture)) bgfx::destroy(gpuData.tree64Texture);
        if (bgfx::isValid(gpuData.materialIDTexture)) bgfx::destroy(gpuData.materialIDTexture);
        gpuData.tree64Texture = createDataTexture(gpuData.tree64Width, gpuData.tree64Height, tree64Texels);
        gpuData.materialIDTexture = createDataTexture8(gpuData.materialIDWidth, gpuData.materialIDHeight, materialIDTexels);

        // Palette texture — built from component palettes.
        rebuildGlobalPaletteTexture(scene, gpuData);

        seedAllocator(gpuData.tree64Alloc, geomCap, geomUsedPadded, "buildDataAndHeaderTextures/tree64");
        seedAllocator(gpuData.materialIDAlloc, matCap, matUsedPadded, "buildDataAndHeaderTextures/materialID");
        validateBlobRanges(scene, gpuData, "buildDataAndHeaderTextures");

        // Pre-allocate header texture at max possible slots so it never grows.
        uint32_t maxSlots = maxHeaderSlots();
        gpuData.headerCapacity = std::min(withHeadroom(static_cast<uint32_t>(scene.chunks.size())), maxSlots);
        if (scene.chunks.size() > static_cast<size_t>(maxSlots))
            core::error("buildDataAndHeaderTextures: {} chunks exceed header texture slot limit {}", scene.chunks.size(), maxSlots);
        std::vector<GPUChunkHeader> headers(gpuData.headerCapacity, degenerateHeader());
        for (uint32_t h = 0; h < scene.chunks.size() && h < gpuData.headerCapacity; h++) {
            const Chunk& c = scene.chunks[h];
            if (c.alive && c.geometryPoolIndex >= 0)
                headers[h] = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene, gpuData);
        }
        if (bgfx::isValid(gpuData.headerTexture)) bgfx::destroy(gpuData.headerTexture);
        gpuData.headerTexture = createHeaderTexture(headers);
        gpuData.uploadedChunkCount = static_cast<uint32_t>(scene.chunks.size());
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core::perf("buildDataAndHeaderTextures: {} liveBlobs {} chunks {} geomTexels {} matTexels: {:.2f}ms",
                   liveBlobs, scene.chunks.size(), geomUsed, matUsed, ms);
    }

    GPUData createTexturesForScene(projv::Scene& scene) {
        core::render("createTexturesForScene: {} chunks {} grids {} blobs",
                     scene.chunks.size(), scene.grids.size(), scene.geometryPool.size());

        GPUData gpuData;

        // Single-model: fold any chunk still owning geometry (legacy loaders) into the refcounted pool
        // so the build reads only from Scene.geometryPool.
        for (Chunk& c : scene.chunks)
            if (c.alive && c.geometryPoolIndex < 0) internChunkGeometry(scene, c);

        // 1-3. Data + header textures via the bulk pack.
        buildDataAndHeaderTextures(scene, gpuData);

        // 4. Small scene tables (gridInfo / cellMap / looseList).
        syncSceneTables(scene, gpuData);

        // 5. Samplers.
        gpuData.tree64Sampler = bgfx::createUniform("tree64Data", bgfx::UniformType::Sampler);
        gpuData.materialIDSampler = bgfx::createUniform("materialIDs", bgfx::UniformType::Sampler);
        gpuData.materialPaletteSampler = bgfx::createUniform("materialPalette", bgfx::UniformType::Sampler);
        gpuData.headerSampler = bgfx::createUniform("headerData", bgfx::UniformType::Sampler);
        gpuData.gridInfoSampler = bgfx::createUniform("gridInfo", bgfx::UniformType::Sampler);
        gpuData.cellMapSampler = bgfx::createUniform("cellMap", bgfx::UniformType::Sampler);
        gpuData.looseListSampler = bgfx::createUniform("looseList", bgfx::UniformType::Sampler);

        gpuData.tree64DimsUniform = bgfx::createUniform("tree64Dims", bgfx::UniformType::Vec4);
        gpuData.materialIDDimsUniform = bgfx::createUniform("voxelTypeDims", bgfx::UniformType::Vec4);
        gpuData.paletteDimsUniform = bgfx::createUniform("paletteDims", bgfx::UniformType::Vec4);

        core::render("createTexturesForScene: done geomTex=({}x{}) matTex=({}x{}) headers={} grids={} loose={} vram=~{:.1f}GB",
                     gpuData.tree64Width, gpuData.tree64Height,
                     gpuData.materialIDWidth, gpuData.materialIDHeight,
                     gpuData.headerCapacity, scene.grids.size(), scene.looseChunkCount,
                     static_cast<double>(texBytes(gpuData.tree64Width, gpuData.tree64Height, true) +
                                        texBytes(gpuData.materialIDWidth, gpuData.materialIDHeight, false)) / 1e9);

        return gpuData;
    }

    void flushSceneUpdates(projv::Scene& scene, GPUData& gpuData) {
        auto t0 = std::chrono::high_resolution_clock::now();
        core_render_every(60, "flushSceneUpdates: start chunks={} blobs={}", scene.chunks.size(), scene.geometryPool.size());

        // 1. Incremental blob upload (or full grow fallback).
        auto maybePools = uploadDirtyBlobs(scene, gpuData);
        std::unordered_set<uint32_t> uploadedPools;

        if (maybePools.has_value()) {
            uploadedPools = std::move(maybePools.value());
        } else {
            core::perf("flushSceneUpdates: allocator full, growing data textures");
            core::info("========== TEXTURE RESIZE ========== chunks={} blobs={}", scene.chunks.size(), scene.geometryPool.size());
            growDataTextures(scene, gpuData);
            for (size_t b = 0; b < scene.geometryPool.size(); b++)
                if (scene.geometryPool[b].refCount > 0)
                    uploadedPools.insert(static_cast<uint32_t>(b));
        }

        // 2. Incremental header row rewrite.
        updateDirtyHeaders(scene, gpuData, uploadedPools);

        // 3. Rebuild small scene tables (full rebuild — they are tiny).
        syncSceneTables(scene, gpuData);

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core_perf_every_ms(2000, "flushSceneUpdates: {} pools uploaded: {:.2f}ms",
                   uploadedPools.size(), ms);
        core_render_every(60, "flushSceneUpdates: done pools={} chunks={} grids={}",
                     uploadedPools.size(), scene.chunks.size(), scene.grids.size());
    }

    void rebuildSceneTextures(projv::Scene& scene, GPUData& gpuData) {
        // 1-3. Rebuild data + header textures (destroys old, creates new).
        buildDataAndHeaderTextures(scene, gpuData);

        // 4. Rebuild small scene tables (gridInfo / cellMap / looseList).
        syncSceneTables(scene, gpuData);

        // Samplers are preserved from the initial createTexturesForScene call.
    }

    void destroyGPUData(GPUData& gpuData) {
        auto killT = [](bgfx::TextureHandle& t) { if (bgfx::isValid(t)) { bgfx::destroy(t); t = BGFX_INVALID_HANDLE; } };
        auto killU = [](bgfx::UniformHandle& u) { if (bgfx::isValid(u)) { bgfx::destroy(u); u = BGFX_INVALID_HANDLE; } };
        killT(gpuData.tree64Texture); killT(gpuData.materialIDTexture); killT(gpuData.materialPaletteTexture);
        killT(gpuData.headerTexture);
        killT(gpuData.gridInfoTexture); killT(gpuData.cellMapTexture); killT(gpuData.looseListTexture);
        killU(gpuData.tree64Sampler); killU(gpuData.materialIDSampler); killU(gpuData.materialPaletteSampler);
        killU(gpuData.headerSampler);
        killU(gpuData.gridInfoSampler); killU(gpuData.cellMapSampler); killU(gpuData.looseListSampler);
        killU(gpuData.tree64DimsUniform); killU(gpuData.materialIDDimsUniform); killU(gpuData.paletteDimsUniform);
        gpuData = GPUData{};
    }

    void updateChunkHeader(projv::Scene& scene, GPUData& gpuData, ChunkHandle h) {
        if (h >= scene.chunks.size() || !scene.chunks[h].alive) return;
        if (!bgfx::isValid(gpuData.headerTexture)) return;
        const Chunk& c = scene.chunks[h];
        GPUChunkHeader hdr = degenerateHeader();
        if (c.geometryPoolIndex >= 0 &&
            static_cast<size_t>(c.geometryPoolIndex) < gpuData.blobRanges.size()) {
            hdr = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene, gpuData);
        }
        if (h >= gpuData.headerCapacity) return;
        writeHeaderTexel(gpuData.headerTexture, h, hdr);
    }
}
