#include "graphics/gpu_interface.h"
#include "graphics/scene_dynamics.h"

#include <cstring>
#include <algorithm>

namespace projv::graphics {
    namespace {
        constexpr uint32_t kDataTexHeight = 4096; // fixed height; data textures grow in width.

        uint32_t maxTexSize() {
            const bgfx::Caps* caps = bgfx::getCaps();
            uint32_t m = caps ? static_cast<uint32_t>(caps->limits.maxTextureSize) : 16384u;
            return m ? m : 16384u;
        }

        // Extra slack so early incremental adds don't immediately force a grow.
        uint32_t withHeadroom(uint32_t used) { return used + used / 2 + 1024; }

        // Pick texel dimensions for a capacity: height fixed, width grows. Returns actual capacity.
        // maxSz is passed in (not read from caps) so this is callable headless with no bgfx context.
        uint32_t chooseDataDims(uint32_t capTexels, uint32_t& w, uint32_t& h, uint32_t maxSz) {
            h = std::min<uint32_t>(kDataTexHeight, maxSz);
            if (h == 0) h = 1;
            w = (capTexels + h - 1) / h;
            if (w < 1) w = 1;
            if (w > maxSz) w = maxSz; // best effort; overflow is warned where it matters
            return w * h;
        }

        // Create an RGBA32U texture of w*h texels, initialised from `texels` (padded with zero).
        bgfx::TextureHandle createDataTexture(uint32_t w, uint32_t h, const std::vector<uint32_t>& texels) {
            std::vector<uint32_t> buf(static_cast<size_t>(w) * h * 4, 0u);
            std::copy(texels.begin(), texels.begin() + std::min(texels.size(), buf.size()), buf.begin());
            const bgfx::Memory* mem = bgfx::copy(buf.data(), buf.size() * sizeof(uint32_t));
            return bgfx::createTexture2D(uint16_t(w), uint16_t(h), false, 1, bgfx::TextureFormat::RGBA32U,
                                         BGFX_TEXTURE_NONE | BGFX_SAMPLER_POINT, mem);
        }

        // Upload `texelData` (RGBA32U, 4 uints per texel) to `tex` starting at linear texel
        // `startTexel`. A linear range wraps rows, but instead of one updateTexture2D PER ROW (which for
        // a large blob is thousands of calls per frame and floods bgfx's transient memory -> silently
        // dropped/corrupted uploads), we upload the contiguous full-width middle as a SINGLE rectangular
        // update, with at most a partial head row and partial tail row around it (<= 3 calls total).
        // No-op when the texture is invalid, so the CPU bookkeeping is headless-testable.
        void uploadTexelRange(bgfx::TextureHandle tex, uint32_t texWidth, uint32_t startTexel,
                              const std::vector<uint32_t>& texelData) {
            if (!bgfx::isValid(tex) || texWidth == 0) return;
            uint32_t numTexels = static_cast<uint32_t>(texelData.size() / 4);
            uint32_t t = startTexel, src = 0, remaining = numTexels;
            while (remaining > 0) {
                uint32_t x = t % texWidth;
                uint32_t y = t / texWidth;
                if (x == 0 && remaining >= texWidth) {
                    // Block of full-width rows in one call (row-major slice matches the rectangle).
                    uint32_t rows = remaining / texWidth;
                    const bgfx::Memory* mem =
                        bgfx::copy(&texelData[size_t(src) * 4], size_t(texWidth) * rows * 4 * sizeof(uint32_t));
                    bgfx::updateTexture2D(tex, 0, 0, uint16_t(x), uint16_t(y), uint16_t(texWidth), uint16_t(rows), mem);
                    uint32_t consumed = texWidth * rows;
                    t += consumed; src += consumed; remaining -= consumed;
                } else {
                    // Partial row: head up to the next row boundary, or a final short tail.
                    uint32_t run = std::min(remaining, texWidth - x);
                    const bgfx::Memory* mem = bgfx::copy(&texelData[size_t(src) * 4], run * 4 * sizeof(uint32_t));
                    bgfx::updateTexture2D(tex, 0, 0, uint16_t(x), uint16_t(y), uint16_t(run), 1, mem);
                    t += run; src += run; remaining -= run;
                }
            }
        }

        // A dead/unused header row: scale <= 0 makes the shader's broadphase reject it.
        GPUChunkHeader degenerateHeader() {
            GPUChunkHeader h{};
            h.scale = 0.0f;
            h.rotationW = 1.0f;
            return h;
        }

// Build the GPU header for a live chunk from its pool blob's current GPU range.
GPUChunkHeader makeHeader(const Chunk& chunk, const GPUBlobRange& r) {
    GPUChunkHeader g{};
    g.chunkID = chunk.header.chunkID;
    g.geometryStartIndex = r.geomTexelOffset;
    g.geometryEndIndex = r.geomTexelOffset + r.geomTexelLen;
    g.voxelTypeDataStartIndex = r.typeTexelOffset * 4u;
    g.voxelTypeDataEndIndex = r.typeTexelOffset * 4u + r.typeUintLen;
    g.positionX = chunk.header.position.x;
    g.positionY = chunk.header.position.y;
    g.positionZ = chunk.header.position.z;
    g.resolution = chunk.header.resolution;
    g.scale = chunk.header.scale;
    g.padding[0] = 0; g.padding[1] = 0;
    g.rotationX = chunk.header.rotation.x;
    g.rotationY = chunk.header.rotation.y;
    g.rotationZ = chunk.header.rotation.z;
    g.rotationW = chunk.header.rotation.w;
    
    core::warn("EDITTEST: makeHeader: chunkID={}, geomStart={}, geomEnd={}, typeStart={}, typeEnd={}, resolution={}, scale={}",
               g.chunkID, g.geometryStartIndex, g.geometryEndIndex, g.voxelTypeDataStartIndex, g.voxelTypeDataEndIndex,
               g.resolution, g.scale);
    
    return g;
}

        // Pack a blob's geometry into RGBA32U texels (1 node -> RGB + zero alpha).
        std::vector<uint32_t> packGeometryTexels(const std::vector<uint32_t>& geometry) {
            uint32_t nodes = static_cast<uint32_t>(geometry.size() / 3);
            std::vector<uint32_t> out(size_t(nodes) * 4, 0u);
            for (uint32_t n = 0; n < nodes; n++) {
                out[size_t(n) * 4 + 0] = geometry[size_t(n) * 3 + 0];
                out[size_t(n) * 4 + 1] = geometry[size_t(n) * 3 + 1];
                out[size_t(n) * 4 + 2] = geometry[size_t(n) * 3 + 2];
            }
            return out;
        }

        // Pack a blob's voxelType uints into RGBA32U texels (4 uints per texel, zero-padded tail).
        std::vector<uint32_t> packVoxelTypeTexels(const std::vector<uint32_t>& data) {
            uint32_t texels = static_cast<uint32_t>((data.size() + 3) / 4);
            std::vector<uint32_t> out(size_t(texels) * 4, 0u);
            std::copy(data.begin(), data.end(), out.begin());
            return out;
        }
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

    bgfx::TextureHandle createHeaderTexture(std::vector<projv::GPUChunkHeader>& headers) {
        const bgfx::Memory* headerMemory = bgfx::copy(  
            headers.data(),  
            headers.size() * sizeof(projv::GPUChunkHeader)
        );

        bgfx::TextureHandle headerTexture = bgfx::createTexture2D(
            headers.size() * 4, // 4 RGBA32U texels per header (12 fields + rotation quaternion).
            1,
            false,
            1,
            bgfx::TextureFormat::RGBA32U,
            BGFX_TEXTURE_NONE|BGFX_SAMPLER_POINT,
            headerMemory
        );
        return headerTexture;
    }

    // Builds a 1-row RGBA32U texture from a raw uint list (4 uints per texel). Used for the
    // small grid-info table, read in the shader via texelFetch(gridInfo, ivec2(texel, 0)).
    bgfx::TextureHandle createUintRowTexture(std::vector<uint32_t> data) {
        while (data.size() % 4 != 0) data.push_back(0);
        if (data.empty()) data.resize(4, 0);
        int width = static_cast<int>(data.size() / 4);
        const bgfx::Memory* mem = bgfx::copy(data.data(), data.size() * sizeof(uint32_t));
        return bgfx::createTexture2D(width, 1, false, 1, bgfx::TextureFormat::RGBA32U,
                                     BGFX_TEXTURE_NONE | BGFX_SAMPLER_POINT, mem);
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
        const bgfx::Memory* mem = bgfx::copy(data.data(), data.size() * sizeof(uint32_t));
        return bgfx::createTexture2D(dataWidth, textureHeight, false, 1, bgfx::TextureFormat::RGBA32U,
                                     BGFX_TEXTURE_NONE | BGFX_SAMPLER_POINT, mem);
    }

    namespace {
        // Grow the geometry-pool ranges vector to match the scene's pool size, so a blob added since
        // the last GPU touch (COW fork, intern) has a slot. New slots start not-uploaded.
        void ensureBlobRanges(const projv::Scene& scene, GPUData& gpuData) {
            if (gpuData.blobRanges.size() < scene.geometryPool.size())
                gpuData.blobRanges.resize(scene.geometryPool.size());
        }

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
        // from current CPU state. Small and off the hot path, so a membership change just rebuilds
        // them; old handles are destroyed to avoid leaking.
        void syncSceneTables(projv::Scene& scene, GPUData& gpuData) {
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
                for (int32_t c : g.cellToChunk)
                    cellMapData.push_back(c < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(c));
            }

            gpuData.looseCapacity = static_cast<uint32_t>(looseList.size());
            // Only touch bgfx on a real context. Headless (all handles invalid) keeps just the CPU
            // counts above, which is what the allocation-table tests inspect.
            if (!bgfx::isValid(gpuData.headerTexture)) return;
            if (bgfx::isValid(gpuData.gridInfoTexture)) bgfx::destroy(gpuData.gridInfoTexture);
            if (bgfx::isValid(gpuData.cellMapTexture)) bgfx::destroy(gpuData.cellMapTexture);
            if (bgfx::isValid(gpuData.looseListTexture)) bgfx::destroy(gpuData.looseListTexture);
            gpuData.gridInfoTexture = createUintRowTexture(gridInfoData);
            gpuData.cellMapTexture = createCellMapTexture(cellMapData);
            gpuData.looseListTexture = createCellMapTexture(looseList);
        }

        // Write one chunk's 4-texel header slot (row == handle). No-op if the header texture is
        // invalid (headless).
        void writeHeaderRow(GPUData& gpuData, ChunkHandle handle, const GPUChunkHeader& h) {
            if (!bgfx::isValid(gpuData.headerTexture)) return;
            const bgfx::Memory* mem = bgfx::copy(&h, sizeof(GPUChunkHeader));
            bgfx::updateTexture2D(gpuData.headerTexture, 0, 0, uint16_t(handle * 4), 0, 4, 1, mem);
        }

        // Enlarge a data texture in place, preserving every existing blob's linear offset (the shader
        // addresses texels by linear index, so a width change is transparent). Adds the new tail to
        // the allocator. Rebuilds the backing texture from the pool when it is live; pure bookkeeping
        // when headless.
        void growDataTexture(projv::Scene& scene, GPUData& gpuData, bool tree64, uint32_t minExtraTexels) {
            RangeAllocator& alloc = tree64 ? gpuData.tree64Alloc : gpuData.voxelTypeAlloc;
            uint32_t& tw = tree64 ? gpuData.tree64Width : gpuData.voxelTypeWidth;
            uint32_t& th = tree64 ? gpuData.tree64Height : gpuData.voxelTypeHeight;
            bgfx::TextureHandle& tex = tree64 ? gpuData.tree64Texture : gpuData.voxelTypeDataTexture;

            uint32_t oldCap = tw * th;
            uint32_t target = oldCap + std::max(minExtraTexels, oldCap / 2 + 1024);
            uint32_t w, h;
            uint32_t maxSz = bgfx::isValid(tex) ? maxTexSize() : 0xFFFFFFFFu;
            uint32_t newCap = chooseDataDims(target, w, h, maxSz);
            if (newCap <= oldCap) newCap = oldCap + minExtraTexels; // best effort under the size cap
            if (newCap <= oldCap) { core::error("growDataTexture: cannot grow past max texture size"); return; }

            if (bgfx::isValid(tex)) {
                std::vector<uint32_t> buf(static_cast<size_t>(w) * h * 4, 0u);
                for (size_t b = 0; b < scene.geometryPool.size(); b++) {
                    const GPUBlobRange& r = gpuData.blobRanges[b];
                    if (!r.uploaded) continue;
                    std::vector<uint32_t> texels = tree64 ? packGeometryTexels(scene.geometryPool[b].geometry)
                                                          : packVoxelTypeTexels(scene.geometryPool[b].voxelTypeData);
                    uint32_t off = tree64 ? r.geomTexelOffset : r.typeTexelOffset;
                    std::copy(texels.begin(), texels.end(), buf.begin() + size_t(off) * 4);
                }
                bgfx::destroy(tex);
                tex = createDataTexture(w, h, buf);
            }
            tw = w; th = h;
            alloc.capacity = newCap;
            alloc.free(oldCap, newCap - oldCap);
        }

        // Allocate `texels` from a data texture, growing it if the pool is full.
        uint32_t allocOrGrow(projv::Scene& scene, GPUData& gpuData, bool tree64, uint32_t texels) {
            RangeAllocator& alloc = tree64 ? gpuData.tree64Alloc : gpuData.voxelTypeAlloc;
            uint32_t off = alloc.alloc(texels);
            if (off == RangeAllocator::INVALID) {
                growDataTexture(scene, gpuData, tree64, texels);
                off = alloc.alloc(texels);
            }
            return off;
        }

        // Ensure the header texture can hold `handle` as a slot, growing (rebuilding from all chunks)
        // if needed.
        void ensureHeaderCapacity(projv::Scene& scene, GPUData& gpuData, ChunkHandle handle) {
            if (handle < gpuData.headerCapacity) return;
            bool live = bgfx::isValid(gpuData.headerTexture);
            uint32_t maxSlots = live ? maxTexSize() / 4 : 0x3FFFFFFFu;
            uint32_t newCap = std::min(withHeadroom(handle + 1), maxSlots);
            if (newCap <= handle) { core::error("ensureHeaderCapacity: header slot {} exceeds max texture width", handle); return; }
            gpuData.headerCapacity = newCap;
            if (!live) return; // headless: just track the capacity
            std::vector<GPUChunkHeader> headers(newCap, degenerateHeader());
            for (uint32_t h = 0; h < scene.chunks.size() && h < newCap; h++) {
                const Chunk& c = scene.chunks[h];
                if (c.alive && c.geometryPoolIndex >= 0)
                    headers[h] = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex]);
            }
            bgfx::destroy(gpuData.headerTexture);
            gpuData.headerTexture = createHeaderTexture(headers);
        }

// Upload a blob's geometry to its (already-allocated) GPU range.
void uploadBlobGeometry(GPUData& gpuData, const GeometryBlob& blob, const GPUBlobRange& r) {
    core::warn("VOXELEDIT: uploadBlobGeometry: uploading {} geometry uints to offset {}, {} voxelType uints to offset {}",
               blob.geometry.size(), r.geomTexelOffset, blob.voxelTypeData.size(), r.typeTexelOffset);
    uploadTexelRange(gpuData.tree64Texture, gpuData.tree64Width, r.geomTexelOffset,
                     packGeometryTexels(blob.geometry));
    uploadTexelRange(gpuData.voxelTypeDataTexture, gpuData.voxelTypeWidth, r.typeTexelOffset,
                     packVoxelTypeTexels(blob.voxelTypeData));
}
    }

    // Build (or rebuild) the tree64/voxelType/header textures from current scene state via one contiguous
    // bulk pack + a single createTexture2D per texture — the proven, always-correct upload the eager path
    // uses. Reassigns every blob's GPU range and reseeds the allocators. Used both for the initial build
    // and, from applySceneMutations, whenever the resident blob set changes (so streaming never depends on
    // the incremental per-blob upload for a blob's *contents*). Destroys the old data/header textures first;
    // leaves samplers and the small scene tables alone.
    static void buildDataAndHeaderTextures(projv::Scene& scene, GPUData& gpuData) {
        // 1. Assign each live blob a contiguous GPU range and pack the linear texel buffers.
        gpuData.blobRanges.assign(scene.geometryPool.size(), GPUBlobRange{});
        std::vector<uint32_t> tree64Texels, voxelTypeTexels;
        uint32_t geomUsed = 0, typeUsed = 0;
        for (size_t b = 0; b < scene.geometryPool.size(); b++) {
            const GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) { gpuData.blobRanges[b].uploaded = false; continue; }
            uint32_t nodes = static_cast<uint32_t>(blob.geometry.size() / 3);
            uint32_t typeTexels = static_cast<uint32_t>((blob.voxelTypeData.size() + 3) / 4);
            gpuData.blobRanges[b] = GPUBlobRange{geomUsed, nodes, typeUsed, typeTexels,
                                                 static_cast<uint32_t>(blob.voxelTypeData.size()), true};
            std::vector<uint32_t> gt = packGeometryTexels(blob.geometry);
            std::vector<uint32_t> vt = packVoxelTypeTexels(blob.voxelTypeData);
            tree64Texels.insert(tree64Texels.end(), gt.begin(), gt.end());
            voxelTypeTexels.insert(voxelTypeTexels.end(), vt.begin(), vt.end());
            geomUsed += nodes;
            typeUsed += typeTexels;
        }

        // 2. Create the geometry textures with headroom, and seed the suballocators to match.
        uint32_t maxSz = maxTexSize();
        uint32_t geomCap = chooseDataDims(withHeadroom(geomUsed), gpuData.tree64Width, gpuData.tree64Height, maxSz);
        uint32_t typeCap = chooseDataDims(withHeadroom(typeUsed), gpuData.voxelTypeWidth, gpuData.voxelTypeHeight, maxSz);
        core::info("buildDataAndHeaderTextures: tree64 {} texels used / {} capacity; voxelType {} / {}",
                   geomUsed, geomCap, typeUsed, typeCap);
        if (bgfx::isValid(gpuData.tree64Texture)) bgfx::destroy(gpuData.tree64Texture);
        if (bgfx::isValid(gpuData.voxelTypeDataTexture)) bgfx::destroy(gpuData.voxelTypeDataTexture);
        gpuData.tree64Texture = createDataTexture(gpuData.tree64Width, gpuData.tree64Height, tree64Texels);
        gpuData.voxelTypeDataTexture = createDataTexture(gpuData.voxelTypeWidth, gpuData.voxelTypeHeight, voxelTypeTexels);
        gpuData.tree64Alloc.reset(geomCap); gpuData.tree64Alloc.reserve(0, geomUsed);
        gpuData.voxelTypeAlloc.reset(typeCap); gpuData.voxelTypeAlloc.reserve(0, typeUsed);

        // 3. Header texture: one 4-texel slot per chunk handle (with headroom); dead/absent = degenerate.
        uint32_t maxSlots = maxTexSize() / 4;
        gpuData.headerCapacity = std::min(withHeadroom(static_cast<uint32_t>(scene.chunks.size())), maxSlots);
        if (scene.chunks.size() > maxSlots)
            core::error("buildDataAndHeaderTextures: {} chunks exceed header texture slot limit {}", scene.chunks.size(), maxSlots);
        std::vector<GPUChunkHeader> headers(gpuData.headerCapacity, degenerateHeader());
        for (uint32_t h = 0; h < scene.chunks.size() && h < gpuData.headerCapacity; h++) {
            const Chunk& c = scene.chunks[h];
            if (c.alive && c.geometryPoolIndex >= 0)
                headers[h] = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex]);
        }
        if (bgfx::isValid(gpuData.headerTexture)) bgfx::destroy(gpuData.headerTexture);
        gpuData.headerTexture = createHeaderTexture(headers);
    }

    // Rebuild ONLY the header texture from current chunk state, in one bulk upload. Used after a batch
    // that changed chunk membership but not the blob set (e.g. a second instance sharing existing blobs
    // loading/unloading): the shared-blob instances' header rows would otherwise depend on the incremental
    // per-chunk writeHeaderRow path, which desyncs. A no-op until the header texture exists (headless/pre-build).
    static void rebuildHeaderTexture(projv::Scene& scene, GPUData& gpuData) {
        uint32_t maxSlots = maxTexSize() / 4;
        gpuData.headerCapacity = std::min(withHeadroom(static_cast<uint32_t>(scene.chunks.size())), maxSlots);
        if (!bgfx::isValid(gpuData.headerTexture)) return;
        std::vector<GPUChunkHeader> headers(gpuData.headerCapacity, degenerateHeader());
        core::warn("DIAG rebuildHeaderTexture: chunks={} headerCapacity={} blobRanges.size={} geometryPool.size={}",
                   scene.chunks.size(), gpuData.headerCapacity, gpuData.blobRanges.size(), scene.geometryPool.size());
        for (uint32_t h = 0; h < scene.chunks.size() && h < gpuData.headerCapacity; h++) {
            const Chunk& c = scene.chunks[h];
            if (c.alive && c.geometryPoolIndex >= 0) {
                if (static_cast<size_t>(c.geometryPoolIndex) >= gpuData.blobRanges.size()) {
                    core::warn("DIAG rebuildHeaderTexture: chunk {} geometryPoolIndex={} OUT OF RANGE for blobRanges.size={}",
                               h, c.geometryPoolIndex, gpuData.blobRanges.size());
                    continue;
                }
                headers[h] = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex]);
                if (h == 4) {
                    core::warn("DIAG rebuildHeaderTexture: chunk 4 header geomStart={} geomEnd={} typeStart={} typeEnd={} scale={} pos=({},{},{})",
                               headers[h].geometryStartIndex, headers[h].geometryEndIndex,
                               headers[h].voxelTypeDataStartIndex, headers[h].voxelTypeDataEndIndex,
                               headers[h].scale, headers[h].positionX, headers[h].positionY, headers[h].positionZ);
                }
            }
        }
        bgfx::destroy(gpuData.headerTexture);
        gpuData.headerTexture = createHeaderTexture(headers);
    }

    GPUData createTexturesForScene(projv::Scene& scene) {
        GPUData gpuData;

        // Single-model: fold any chunk still owning geometry (legacy loaders) into the refcounted pool
        // so the build and every incremental primitive read only from Scene.geometryPool.
        for (Chunk& c : scene.chunks)
            if (c.alive && c.geometryPoolIndex < 0) internChunkGeometry(scene, c);

        // 1-3. Data + header textures via the bulk pack.
        buildDataAndHeaderTextures(scene, gpuData);

        // 4. Small scene tables (gridInfo / cellMap / looseList).
        syncSceneTables(scene, gpuData);

        // 5. Samplers.
        gpuData.tree64Sampler = bgfx::createUniform("tree64Data", bgfx::UniformType::Sampler);
        gpuData.voxelTypeDataSampler = bgfx::createUniform("voxelTypeData", bgfx::UniformType::Sampler);
        gpuData.headerSampler = bgfx::createUniform("headerData", bgfx::UniformType::Sampler);
        gpuData.gridInfoSampler = bgfx::createUniform("gridInfo", bgfx::UniformType::Sampler);
        gpuData.cellMapSampler = bgfx::createUniform("cellMap", bgfx::UniformType::Sampler);
        gpuData.looseListSampler = bgfx::createUniform("looseList", bgfx::UniformType::Sampler);

        return gpuData;
    }

    // No-sync core of addChunkToGPU: does everything except rebuilding the small scene tables, so a
    // batch (applySceneMutations) can apply many adds and sync once. The public wrapper syncs per call.
    static void addChunkToGPUCore(projv::Scene& scene, GPUData& gpuData, ChunkHandle handle) {
        if (handle >= scene.chunks.size()) { core::warn("addChunkToGPU: handle {} out of range", handle); return; }
        Chunk& chunk = scene.chunks[handle];
        if (!chunk.alive) { core::warn("addChunkToGPU: chunk {} is not alive", handle); return; }
        internChunkGeometry(scene, chunk);
        ensureBlobRanges(scene, gpuData);
        int32_t b = chunk.geometryPoolIndex;
        GPUBlobRange& r = gpuData.blobRanges[b];
        if (!r.uploaded) {
            const GeometryBlob& blob = scene.geometryPool[b];
            uint32_t nodes = static_cast<uint32_t>(blob.geometry.size() / 3);
            uint32_t typeTexels = static_cast<uint32_t>((blob.voxelTypeData.size() + 3) / 4);
            uint32_t go = allocOrGrow(scene, gpuData, true, nodes);
            uint32_t to = allocOrGrow(scene, gpuData, false, typeTexels);
            r = GPUBlobRange{go, nodes, to, typeTexels, static_cast<uint32_t>(blob.voxelTypeData.size()), true};
            uploadBlobGeometry(gpuData, blob, r);
            gpuData.blobEpoch++; // resident blob set changed -> applySceneMutations will bulk-rebuild
        }
        ensureHeaderCapacity(scene, gpuData, handle);
        writeHeaderRow(gpuData, handle, makeHeader(chunk, r));

        // Membership: register in the loose list or the grid cell per the residency key.
        if (chunk.gridIndex < 0) {
            if (std::find(scene.looseChunks.begin(), scene.looseChunks.end(), handle) == scene.looseChunks.end())
                scene.looseChunks.push_back(handle);
        } else if (chunk.gridIndex < static_cast<int32_t>(scene.grids.size())) {
            SceneGrid& g = scene.grids[chunk.gridIndex];
            if (chunk.cellIndex >= 0 && chunk.cellIndex < static_cast<int32_t>(g.cellToChunk.size()))
                g.cellToChunk[chunk.cellIndex] = static_cast<int32_t>(handle);
        }
        scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());
    }

    void addChunkToGPU(projv::Scene& scene, GPUData& gpuData, ChunkHandle handle) {
        addChunkToGPUCore(scene, gpuData, handle);
        syncSceneTables(scene, gpuData);
    }

void updateChunkGeometryOnGPU(projv::Scene& scene, GPUData& gpuData, ChunkHandle handle) {
    core::warn("EDITTEST: updateChunkGeometryOnGPU: handle={}", handle);
    if (handle >= scene.chunks.size()) { core::warn("updateChunkGeometryOnGPU: handle {} out of range", handle); return; }
    Chunk& chunk = scene.chunks[handle];
    core::warn("EDITTEST: chunk alive={}, geometryPoolIndex={}, resolution={}, scale={}", 
               chunk.alive, chunk.geometryPoolIndex, chunk.header.resolution, chunk.header.scale);
    if (!chunk.alive || chunk.geometryPoolIndex < 0) { core::warn("updateChunkGeometryOnGPU: chunk {} not renderable", handle); return; }
    ensureBlobRanges(scene, gpuData);
    int32_t b = chunk.geometryPoolIndex;
    const GeometryBlob& blob = scene.geometryPool[b];
    GPUBlobRange& r = gpuData.blobRanges[b];
    uint32_t nodes = static_cast<uint32_t>(blob.geometry.size() / 3);
    uint32_t typeTexels = static_cast<uint32_t>((blob.voxelTypeData.size() + 3) / 4);
    
    core::warn("EDITTEST: blob has {} nodes ({} geometry uints), {} typeTexels ({} voxelType uints)",
               nodes, blob.geometry.size(), typeTexels, blob.voxelTypeData.size());
    core::warn("EDITTEST: current range: geomOff={}, geomLen={}, typeOff={}, typeLen={}, uploaded={}",
               r.geomTexelOffset, r.geomTexelLen, r.typeTexelOffset, r.typeTexelLen, r.uploaded);

        // Remember the pre-edit range so we can tell whether the blob RELOCATED (grew past its slot).
        uint32_t oldGeomOff = r.geomTexelOffset, oldGeomLen = r.geomTexelLen;
        uint32_t oldTypeOff = r.typeTexelOffset, oldTypeLen = r.typeTexelLen;
        bool wasUploaded = r.uploaded;

        // scene.geometryPool[b]'s CPU content is ALREADY the new (edited) geometry by this point (the
        // caller baked the edit before enqueuing this Update). If growing past the current slot needs
        // more texels than are free, allocOrGrow below can trigger growDataTexture's rebuild-from-pool
        // fallback -- which packs every blobRanges[]-"uploaded" blob from its CURRENT (already-new)
        // scene.geometryPool content at its RECORDED (still-old, too-small) offset. For every OTHER
        // blob that's a correct no-op (content and recorded range still match); for THIS blob it would
        // write the new, larger packed data at the old, smaller slot and overflow into whatever sits
        // next in the buffer -- corrupting a neighboring chunk. Marking this blob "not uploaded" for
        // the duration excludes it from that rebuild; it's re-uploaded to its correct new range below
        // regardless, via uploadBlobGeometry's own targeted texel-range upload.
        r.uploaded = false;

        // Re-fit geometry: reuse the range in place when it still fits, else grow-then-free (allocate
        // the new slot before releasing the old one, so this blob's old range is never presented as
        // free while still logically "ours" mid-reallocation).
        bool geomRelocated = !wasUploaded || nodes > oldGeomLen;
        if (geomRelocated) {
            r.geomTexelOffset = allocOrGrow(scene, gpuData, true, nodes);
            if (wasUploaded) gpuData.tree64Alloc.free(oldGeomOff, oldGeomLen);
        }
        r.geomTexelLen = nodes;
        bool typeRelocated = !wasUploaded || typeTexels > oldTypeLen;
        if (typeRelocated) {
            r.typeTexelOffset = allocOrGrow(scene, gpuData, false, typeTexels);
            if (wasUploaded) gpuData.voxelTypeAlloc.free(oldTypeOff, oldTypeLen);
        }
    r.typeTexelLen = typeTexels;
    r.typeUintLen = static_cast<uint32_t>(blob.voxelTypeData.size());
    r.uploaded = true;
    uploadBlobGeometry(gpuData, blob, r);
    
    core::warn("EDITTEST: after upload: geomOff={}, geomLen={}, typeOff={}, typeLen={}",
               r.geomTexelOffset, r.geomTexelLen, r.typeTexelOffset, r.typeTexelLen);

    core::warn("DIAG updateChunkGeometryOnGPU: handle={} poolIdx={} refCount={} nodes={} oldGeomLen={} "
               "geomRelocated={} newGeomOff={} newGeomLen={} typeTexels={} oldTypeLen={} typeRelocated={} "
               "newTypeOff={} newTypeLen={}",
               handle, b, scene.geometryPool[b].refCount, nodes, oldGeomLen, geomRelocated,
               r.geomTexelOffset, r.geomTexelLen, typeTexels, oldTypeLen, typeRelocated,
               r.typeTexelOffset, r.typeTexelLen);

    // Header fanout: the header row carries the blob's GPU offsets, so if the range moved, EVERY
    // chunk instancing this shared blob (refCount may be > 1) now has a stale row pointing at the
    // freed offset — not just `handle`. Rewrite all of them. A move is rare (only on a grow that
    // didn't fit in place), so the O(n) scan is gated on it; an in-place edit stays O(1).
    bool relocated = wasUploaded && (r.geomTexelOffset != oldGeomOff || r.typeTexelOffset != oldTypeOff);
    core::warn("EDITTEST: relocated={}, wasUploaded={}, oldGeomOff={}, oldTypeOff={}",
               relocated, wasUploaded, oldGeomOff, oldTypeOff);
    core::warn("DIAG updateChunkGeometryOnGPU: relocated={} wasUploaded={} oldGeomOff={} oldTypeOff={}",
               relocated, wasUploaded, oldGeomOff, oldTypeOff);
    
    // When geometry changes (relocates OR changes size), increment blobEpoch to trigger full texture 
    // rebuild in applySceneMutations. Even in-place edits need this because the GPU texture may have
    // stale data beyond the new range.
    if (relocated || nodes != oldGeomLen || typeTexels != oldTypeLen) {
        gpuData.blobEpoch++;
        core::warn("EDITTEST: geometry changed (relocated={} or size changed), incremented blobEpoch to {}", 
                   relocated, gpuData.blobEpoch);
    }
    if (relocated && scene.geometryPool[b].refCount > 1) {
        int rewritten = 0;
        for (uint32_t h = 0; h < scene.chunks.size(); h++) {
            const Chunk& c = scene.chunks[h];
            if (c.alive && c.geometryPoolIndex == b) {
                writeHeaderRow(gpuData, h, makeHeader(c, r));
                rewritten++;
            }
        }
        core::warn("EDITTEST: fanout rewrote {} header rows for poolIdx={}", rewritten, b);
        core::warn("DIAG updateChunkGeometryOnGPU: fanout rewrote {} header rows for poolIdx={}", rewritten, b);
    } else {
        core::warn("EDITTEST: writing single header row for handle={}", handle);
        writeHeaderRow(gpuData, handle, makeHeader(chunk, r));
    }
}

    // No-sync core of removeChunkFromGPU (see addChunkToGPUCore). The public wrapper syncs per call.
    static void removeChunkFromGPUCore(projv::Scene& scene, GPUData& gpuData, ChunkHandle handle) {
        if (handle >= scene.chunks.size()) { core::warn("removeChunkFromGPU: handle {} out of range", handle); return; }
        Chunk& chunk = scene.chunks[handle];
        if (!chunk.alive) return;
        core::warn("DIAG removeChunkFromGPUCore: EVICTING handle={} poolIdx={} gridIndex={} cellIndex={} refCountBefore={}",
                   handle, chunk.geometryPoolIndex, chunk.gridIndex, chunk.cellIndex,
                   chunk.geometryPoolIndex >= 0 ? scene.geometryPool[chunk.geometryPoolIndex].refCount : 0);
        ensureBlobRanges(scene, gpuData);
        int32_t b = chunk.geometryPoolIndex;
        if (b >= 0) {
            GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount > 0) blob.refCount--;
            if (blob.refCount == 0) {
                GPUBlobRange& r = gpuData.blobRanges[b];
                if (r.uploaded) {
                    gpuData.tree64Alloc.free(r.geomTexelOffset, r.geomTexelLen);
                    gpuData.voxelTypeAlloc.free(r.typeTexelOffset, r.typeTexelLen);
                    r = GPUBlobRange{};
                }
                blob = GeometryBlob{};                 // release memory
                scene.blobFreeList.push_back(static_cast<uint32_t>(b)); // recycle pool slot
                gpuData.blobEpoch++; // resident blob set changed -> applySceneMutations will bulk-rebuild
            }
        }

        // Clear membership.
        if (chunk.gridIndex >= 0 && chunk.gridIndex < static_cast<int32_t>(scene.grids.size())) {
            SceneGrid& g = scene.grids[chunk.gridIndex];
            if (chunk.cellIndex >= 0 && chunk.cellIndex < static_cast<int32_t>(g.cellToChunk.size()))
                g.cellToChunk[chunk.cellIndex] = -1;
        } else {
            auto it = std::find(scene.looseChunks.begin(), scene.looseChunks.end(), handle);
            if (it != scene.looseChunks.end()) scene.looseChunks.erase(it);
        }

        // Degenerate header + recycle the chunk slot (handle) for reuse.
        writeHeaderRow(gpuData, handle, degenerateHeader());
        chunk.alive = false;
        chunk.geometryPoolIndex = -1;
        scene.chunkFreeList.push_back(handle);
        scene.looseChunkCount = static_cast<uint32_t>(scene.looseChunks.size());
    }

    void removeChunkFromGPU(projv::Scene& scene, GPUData& gpuData, ChunkHandle handle) {
        removeChunkFromGPUCore(scene, gpuData, handle);
        syncSceneTables(scene, gpuData);
    }

    // Drains a mutation queue with a single table sync at the end (see scene_dynamics.h). The Add/Remove
    // cores skip their per-call syncSceneTables; Update never changes membership, so it needs no sync.
    void applySceneMutations(projv::Scene& scene, GPUData& gpuData, std::vector<PendingSceneMutation>& queue) {
        core::warn("EDITTEST: applySceneMutations: queue size={}", queue.size());
        uint32_t epochBefore = gpuData.blobEpoch;
        bool membershipChanged = false;
        for (const PendingSceneMutation& m : queue) {
            core::warn("EDITTEST:   processing mutation: kind={}, handle={}",
                       m.kind == SceneMutationKind::Add ? "Add" :
                       m.kind == SceneMutationKind::Update ? "Update" : "Remove",
                       m.handle);
            switch (m.kind) {
                case SceneMutationKind::Add:    addChunkToGPUCore(scene, gpuData, m.handle); membershipChanged = true; break;
                case SceneMutationKind::Update: updateChunkGeometryOnGPU(scene, gpuData, m.handle); break;
                case SceneMutationKind::Remove: removeChunkFromGPUCore(scene, gpuData, m.handle); membershipChanged = true; break;
            }
        }
        // Bulk-rebuild the GPU textures rather than trusting the incremental per-chunk uploads (which
        // desync for shared-blob instances loading/unloading). If the resident blob SET changed, repack
        // the data + header textures via the proven bulk path; otherwise, if only membership changed
        // (e.g. a second instance sharing existing blobs), rebuild just the header texture. Both are single
        // createTexture2D uploads. syncSceneTables always rebuilds the small cell/grid/loose tables.
        core::warn("EDITTEST: after processing: blobEpoch={} (was {}), membershipChanged={}",
                   gpuData.blobEpoch, epochBefore, membershipChanged);
        if (gpuData.blobEpoch != epochBefore && bgfx::isValid(gpuData.tree64Texture)) {
            core::warn("EDITTEST: blobEpoch changed, calling buildDataAndHeaderTextures");
            buildDataAndHeaderTextures(scene, gpuData);
        } else if (membershipChanged) {
            core::warn("EDITTEST: membershipChanged, calling rebuildHeaderTexture");
            rebuildHeaderTexture(scene, gpuData);
        } else {
            core::warn("EDITTEST: no bulk rebuild needed");
        }
        if (!queue.empty()) syncSceneTables(scene, gpuData);
        queue.clear();
    }

    void destroyGPUData(GPUData& gpuData) {
        auto killT = [](bgfx::TextureHandle& t) { if (bgfx::isValid(t)) { bgfx::destroy(t); t = BGFX_INVALID_HANDLE; } };
        auto killU = [](bgfx::UniformHandle& u) { if (bgfx::isValid(u)) { bgfx::destroy(u); u = BGFX_INVALID_HANDLE; } };
        killT(gpuData.tree64Texture); killT(gpuData.voxelTypeDataTexture); killT(gpuData.headerTexture);
        killT(gpuData.gridInfoTexture); killT(gpuData.cellMapTexture); killT(gpuData.looseListTexture);
        killU(gpuData.tree64Sampler); killU(gpuData.voxelTypeDataSampler); killU(gpuData.headerSampler);
        killU(gpuData.gridInfoSampler); killU(gpuData.cellMapSampler); killU(gpuData.looseListSampler);
        gpuData = GPUData{};
    }
}
