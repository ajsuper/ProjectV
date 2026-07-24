#include "graphics/gpu_interface.h"
#include "utils/material.h"

#include <chrono>
#include <cstring>
#include <algorithm>
#include <optional>
#include <unordered_set>

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

        // Per-blob over-allocation padding so in-place COW edits that grow the blob slightly
        // don't need a new GPU range. 25% + 64-texel minimum is conservative: it absorbs typical
        // voxel additions while keeping total padding well within the allocator's headroom
        // (which is 50% + 1024 of the padded used total).
        uint32_t paddedAlloc(uint32_t needed) { return needed + std::max(needed / 4u, 64u); }

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

        // Pick texel dimensions for a capacity: height fixed, width grows. Returns actual capacity.
        // maxSz is passed in (not read from caps) so this is callable headless with no bgfx context.
        // Width is rounded up to power of 2 so the shader can use bit ops (&, >>) instead of
        // expensive integer divide/modulo by a non-power-of-2 divisor.
        uint32_t chooseDataDims(uint32_t capTexels, uint32_t& w, uint32_t& h, uint32_t maxSz) {
            h = std::min<uint32_t>(kDataTexHeight, maxSz);
            if (h == 0) h = 1;
            w = (capTexels + h - 1) / h;
            if (w < 1) w = 1;
            w = nextPowerOfTwo(w);
            // Clamp to largest power of 2 <= maxSz so shader bit ops stay correct.
            uint32_t maxPoT = (maxSz >= 0x80000000u) ? 0x80000000u : (nextPowerOfTwo(maxSz + 1u) >> 1u);
            if (w > maxPoT) w = maxPoT;
            return w * h;
        }

        // Create an RGBA32U MUTABLE texture (no initial data → mutable, updateTexture2D works).
        // Fills the texture via updateTexture2D with `texels` (padded with zero).
        bgfx::TextureHandle createDataTexture(uint32_t w, uint32_t h, const std::vector<uint32_t>& texels) {
            bgfx::TextureHandle tex = bgfx::createTexture2D(uint16_t(w), uint16_t(h), false, 1,
                bgfx::TextureFormat::RGBA32U,
                BGFX_SAMPLER_POINT, nullptr);

            std::vector<uint32_t> buf(static_cast<size_t>(w) * h * 4, 0u);
            std::copy(texels.begin(), texels.begin() + std::min(texels.size(), buf.size()), buf.begin());
            const bgfx::Memory* mem = bgfx::copy(buf.data(), buf.size() * sizeof(uint32_t));
            bgfx::updateTexture2D(tex, 0, 0, 0, 0, uint16_t(w), uint16_t(h), mem);
            return tex;
        }

        // Create an RGBA8 MUTABLE texture for material IDs (4 uint8 per texel, 4 bytes/texel).
        bgfx::TextureHandle createDataTexture8(uint32_t w, uint32_t h, const std::vector<uint8_t>& texels) {
            bgfx::TextureHandle tex = bgfx::createTexture2D(uint16_t(w), uint16_t(h), false, 1,
                bgfx::TextureFormat::RGBA8U,
                BGFX_SAMPLER_POINT, nullptr);
            std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4, 0u);
            std::copy(texels.begin(), texels.begin() + std::min(texels.size(), buf.size()), buf.begin());
            const bgfx::Memory* mem = bgfx::copy(buf.data(), buf.size() * sizeof(uint8_t));
            bgfx::updateTexture2D(tex, 0, 0, 0, 0, uint16_t(w), uint16_t(h), mem);
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
                              const Scene& scene) {
    GPUChunkHeader g{};
    g.chunkID = chunk.header.chunkID;
    g.geometryStartIndex = r.geomTexelOffset;
    g.geometryEndIndex = r.geomTexelOffset + r.geomTexelLen;
    g.materialIDStartIndex = r.matTexelOffset * 4u;
    g.materialIDEndIndex = r.matTexelOffset * 4u + r.matByteLen;
    g.positionX = chunk.header.position.x;
    g.positionY = chunk.header.position.y;
    g.positionZ = chunk.header.position.z;
    g.resolution = chunk.header.resolution;
    g.scale = chunk.header.scale;
    g.dataRefID = 0;
    if (chunk.componentHandle < scene.components.size()) {
        int32_t rid = scene.components[chunk.componentHandle].dataRefID;
        if (rid >= 0) g.dataRefID = static_cast<uint32_t>(rid);
    }
    g.paletteOffset = r.paletteOffset;
    g.rotationX = chunk.header.rotation.x;
    g.rotationY = chunk.header.rotation.y;
    g.rotationZ = chunk.header.rotation.z;
    g.rotationW = chunk.header.rotation.w;

    return g;
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



        std::vector<uint8_t> packMaterialIDTexels8(const std::vector<uint8_t>& materialIDs) {
            size_t texels = (materialIDs.size() + 3) / 4;
            std::vector<uint8_t> out(texels * 4, 0u);
            for (size_t i = 0; i < materialIDs.size(); ++i)
                out[i] = materialIDs[i];
            return out;
        }

        } // anonymous namespace

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
        // Create MUTABLE texture (nullptr → mutable, so updateTexture2D works later).
        uint32_t w = headers.size() * 4;
        bgfx::TextureHandle headerTexture = bgfx::createTexture2D(
            uint16_t(w), 1, false, 1, bgfx::TextureFormat::RGBA32U,
            BGFX_SAMPLER_POINT, nullptr);

        // Fill initial data via updateTexture2D.
        const bgfx::Memory* headerMemory = bgfx::copy(
            headers.data(),
            headers.size() * sizeof(projv::GPUChunkHeader));
        bgfx::updateTexture2D(headerTexture, 0, 0, 0, 0, uint16_t(w), 1, headerMemory);
        return headerTexture;
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
        bgfx::TextureHandle tex = bgfx::createTexture2D(dataWidth, textureHeight, false, 1,
                                                         bgfx::TextureFormat::RGBA32U,
                                                         BGFX_SAMPLER_POINT, nullptr);
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
                for (int32_t c : g.cellToChunk)
                    cellMapData.push_back(c < 0 ? 0xFFFFFFFFu : static_cast<uint32_t>(c));
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
        std::vector<uint32_t> tree64Texels, materialPaletteTexels; std::vector<uint8_t> materialIDTexels;
        uint32_t geomUsed = 0, matUsed = 0;
        uint32_t geomUsedPadded = 0, matUsedPadded = 0;
        std::unordered_set<uint32_t> uniquePalettes;
        std::vector<uint32_t> globalPaletteTexels;
        uint32_t paletteOffset = 0;

        for (size_t b = 0; b < scene.geometryPool.size(); b++) {
            const GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) continue;

            uint32_t nodes = static_cast<uint32_t>(blob.geometry.size() / 3);
            uint32_t matTexels = static_cast<uint32_t>((blob.materialIDs.size() + 3) / 4);
            uint32_t gAlloc = paddedAlloc(nodes);
            uint32_t mAlloc = paddedAlloc(matTexels);

            gpuData.blobRanges[b] = GPUBlobRange{
                geomUsedPadded, nodes, gAlloc,
                matUsedPadded, matTexels, mAlloc,
                static_cast<uint32_t>(blob.materialIDs.size()),
                true
            };

            std::vector<uint32_t> gt = packGeometryTexels(blob.geometry);
            std::vector<uint8_t> mt;
            if (!blob.materialIDs.empty()) {
                std::vector<uint32_t> remapped(blob.materialIDs.size());
                for (size_t i = 0; i < blob.materialIDs.size(); i++)
                    remapped[i] = static_cast<uint32_t>(blob.materialIDs[i] + paletteOffset);
                mt = packMaterialIDTexels8(blob.materialIDs);
            } else {
                mt = packMaterialIDTexels8(blob.materialIDs);
            }
            tree64Texels.insert(tree64Texels.end(), gt.begin(), gt.end());
            materialIDTexels.insert(materialIDTexels.end(), mt.begin(), mt.end());
            tree64Texels.insert(tree64Texels.end(), static_cast<size_t>(gAlloc - nodes) * 4, 0u);
            materialIDTexels.insert(materialIDTexels.end(), static_cast<size_t>(mAlloc - matTexels) * 4, 0u);

            if (!blob.materialPalette.empty()) {
                for (const Material& m : blob.materialPalette) {
                    globalPaletteTexels.push_back(m.packedColor);
                }
            }
                        gpuData.blobRanges[b].paletteOffset = paletteOffset;
            paletteOffset += static_cast<uint32_t>(blob.materialPalette.size());

            geomUsed += nodes;
            matUsed += matTexels;
            geomUsedPadded += gAlloc;
            matUsedPadded += mAlloc;
        }

        uint32_t maxSz = maxTexSize();
        uint32_t preAllocCap = static_cast<uint32_t>(double(maxSz) * kDataTexHeight * 0.5);
        uint32_t maxCap = preAllocCap;
        uint32_t geomCap = chooseDataDims(maxCap, gpuData.tree64Width, gpuData.tree64Height, maxSz);
        uint32_t matCap = chooseDataDims(maxCap, gpuData.materialIDWidth, gpuData.materialIDHeight, maxSz);

        if (bgfx::isValid(gpuData.tree64Texture)) bgfx::destroy(gpuData.tree64Texture);
        if (bgfx::isValid(gpuData.materialIDTexture)) bgfx::destroy(gpuData.materialIDTexture);
        gpuData.tree64Texture = createDataTexture(gpuData.tree64Width, gpuData.tree64Height, tree64Texels);
        gpuData.materialIDTexture = createDataTexture8(gpuData.materialIDWidth, gpuData.materialIDHeight, materialIDTexels);

        // Palette texture.
        if (!globalPaletteTexels.empty()) {
            if (bgfx::isValid(gpuData.materialPaletteTexture)) bgfx::destroy(gpuData.materialPaletteTexture);
            while (globalPaletteTexels.size() % 4 != 0) globalPaletteTexels.push_back(0);
            uint32_t pw = static_cast<uint32_t>(globalPaletteTexels.size() / 4);
            std::vector<uint32_t> palTexels(globalPaletteTexels);
            gpuData.materialPaletteTexture = createDataTexture(std::max(pw, 1u), 1, palTexels);
            gpuData.paletteWidth = std::max(pw, 1u);
        }

        gpuData.tree64Alloc.reset(geomCap);
        gpuData.tree64Alloc.reserve(0, geomUsedPadded);
        gpuData.materialIDAlloc.reset(matCap);
        gpuData.materialIDAlloc.reserve(0, matUsedPadded);

        for (GeometryBlob& blob : scene.geometryPool)
            blob.dirty = false;

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core::perf("growDataTextures: {} geomUsed {} matUsed dims ({}x{})/({}x{}): {:.2f}ms",
                   geomUsed, matUsed,
                   gpuData.tree64Width, gpuData.tree64Height,
                   gpuData.materialIDWidth, gpuData.materialIDHeight, ms);
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

        // First pass: build global palette and per-blob palette offsets from ALL live blobs.
        std::vector<uint32_t> globalPalette;
        std::vector<uint32_t> paletteOffsets(scene.geometryPool.size(), 0);
        uint32_t palOff = 0;
        bool paletteNeedsRebuild = false;
        for (size_t b = 0; b < scene.geometryPool.size(); b++) {
            GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) continue;
            paletteOffsets[b] = palOff;
            for (const Material& m : blob.materialPalette)
                globalPalette.push_back(m.packedColor);
            palOff += static_cast<uint32_t>(blob.materialPalette.size());
            if (blob.dirty && !blob.materialPalette.empty())
                paletteNeedsRebuild = true;
        }

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

            uint32_t nodes = static_cast<uint32_t>(blob.geometry.size() / 3);
            uint32_t matTexels = static_cast<uint32_t>((blob.materialIDs.size() + 3) / 4);
            uint32_t matBytes = static_cast<uint32_t>(blob.materialIDs.size());

            bool needRealloc = !r.uploaded || nodes > r.geomTexelAllocated || matTexels > r.matTexelAllocated;

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

            std::vector<uint32_t> geomPacked = packGeometryTexels(blob.geometry);
            uploadTexelSpan(gpuData.tree64Texture, gpuData.tree64Width,
                           geomPacked, r.geomTexelOffset, r.geomTexelLen);

            // Remap per-blob material IDs to global palette indices.
            uint32_t offset = paletteOffsets[b];
            std::vector<uint8_t> matPacked;
            if (!blob.materialIDs.empty() && offset > 0) {
                std::vector<uint32_t> remapped(blob.materialIDs.size());
                for (size_t i = 0; i < blob.materialIDs.size(); i++)
                    remapped[i] = static_cast<uint32_t>(blob.materialIDs[i] + offset);
                matPacked = packMaterialIDTexels8(blob.materialIDs);
            } else {
                matPacked = packMaterialIDTexels8(blob.materialIDs);
            }
            uploadTexelSpan8(gpuData.materialIDTexture, gpuData.materialIDWidth,
                           matPacked, r.matTexelOffset, r.matTexelLen);

            blob.dirty = false;
            uploadedPools.insert(static_cast<uint32_t>(b));
            dirtyUploaded++;
        }

        // Rebuild palette texture from ALL live blobs (not just the last dirty one).
        if (paletteNeedsRebuild && !globalPalette.empty()) {
            while (globalPalette.size() % 4 != 0) globalPalette.push_back(0);
            uint32_t pw = static_cast<uint32_t>(globalPalette.size() / 4);
            if (bgfx::isValid(gpuData.materialPaletteTexture)) bgfx::destroy(gpuData.materialPaletteTexture);
            gpuData.materialPaletteTexture = createDataTexture(std::max(pw, 1u), 1, globalPalette);
            gpuData.paletteWidth = std::max(pw, 1u);
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core_perf_every(60, "uploadDirtyBlobs: {} dirty blobs: {:.2f}ms", dirtyUploaded, ms);
        return uploadedPools;
    }

    // Recreate the header texture with larger capacity (grow fallback). Rewrites all rows from
    // current scene.chunks state. Does not touch the data textures.
    // With pre-allocation at maxSlots, this should only run during initial setup.
    static void growHeaderTexture(projv::Scene& scene, GPUData& gpuData) {
        uint32_t maxSlots = maxTexSize() / 4;
        uint32_t newCap = std::min(withHeadroom(static_cast<uint32_t>(scene.chunks.size())), maxSlots);
        if (newCap <= gpuData.headerCapacity) newCap = gpuData.headerCapacity + 1024;
        if (newCap > maxSlots) newCap = maxSlots;

        if (bgfx::isValid(gpuData.headerTexture)) bgfx::destroy(gpuData.headerTexture);
        gpuData.headerCapacity = newCap;

        std::vector<GPUChunkHeader> headers(newCap, degenerateHeader());
        for (ChunkHandle h = 0; h < scene.chunks.size() && h < newCap; h++) {
            const Chunk& c = scene.chunks[h];
            if (c.alive && c.geometryPoolIndex >= 0 &&
                static_cast<size_t>(c.geometryPoolIndex) < gpuData.blobRanges.size()) {
                headers[h] = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene);
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
                hdr = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene);
            }

            uint16_t x = static_cast<uint16_t>(h * 4);
            const bgfx::Memory* mem = bgfx::copy(&hdr, sizeof(hdr));
            bgfx::updateTexture2D(gpuData.headerTexture, 0, 0, x, 0, 4, 1, mem);
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
        std::vector<uint32_t> globalPaletteTexels;
        uint32_t geomUsed = 0, matUsed = 0;
        uint32_t geomUsedPadded = 0, matUsedPadded = 0;
        uint32_t liveBlobs = 0;
        uint32_t paletteOffset = 0;
        for (size_t b = 0; b < scene.geometryPool.size(); b++) {
            const GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) { gpuData.blobRanges[b].uploaded = false; continue; }
            liveBlobs++;
            uint32_t nodes = static_cast<uint32_t>(blob.geometry.size() / 3);
            uint32_t matTexels = static_cast<uint32_t>((blob.materialIDs.size() + 3) / 4);
            uint32_t gAlloc = paddedAlloc(nodes);
            uint32_t mAlloc = paddedAlloc(matTexels);
            gpuData.blobRanges[b] = GPUBlobRange{geomUsedPadded, nodes, gAlloc,
                                                 matUsedPadded, matTexels, mAlloc,
                                                 static_cast<uint32_t>(blob.materialIDs.size()), true};
            std::vector<uint32_t> gt = packGeometryTexels(blob.geometry);
            std::vector<uint8_t> mt;
            if (!blob.materialIDs.empty()) {
                std::vector<uint32_t> remapped(blob.materialIDs.size());
                for (size_t i = 0; i < blob.materialIDs.size(); i++)
                    remapped[i] = static_cast<uint32_t>(blob.materialIDs[i] + paletteOffset);
                mt = packMaterialIDTexels8(blob.materialIDs);
            } else {
                mt = packMaterialIDTexels8(blob.materialIDs);
            }
            tree64Texels.insert(tree64Texels.end(), gt.begin(), gt.end());
            materialIDTexels.insert(materialIDTexels.end(), mt.begin(), mt.end());
            tree64Texels.insert(tree64Texels.end(), static_cast<size_t>(gAlloc - nodes) * 4, 0u);
            materialIDTexels.insert(materialIDTexels.end(), static_cast<size_t>(mAlloc - matTexels) * 4, 0u);
            for (const Material& mat : blob.materialPalette) {
                globalPaletteTexels.push_back(mat.packedColor);
            }
                        gpuData.blobRanges[b].paletteOffset = paletteOffset;
            paletteOffset += static_cast<uint32_t>(blob.materialPalette.size());
            geomUsed += nodes;
            matUsed += matTexels;
            geomUsedPadded += gAlloc;
            matUsedPadded += mAlloc;
        }

        // Pre-allocate data textures at their maximum possible GPU size to eliminate
        // texture recreation as the scene grows. The shader's texelFetch uses power-of-2
        // width for bit ops, so we allocate at the largest PoT that fits within maxTexSize.
        uint32_t maxSz = maxTexSize();
        uint32_t preAllocCap = static_cast<uint32_t>(double(maxSz) * kDataTexHeight * 0.5);
        uint32_t maxCap = preAllocCap;
        uint32_t geomCap = chooseDataDims(maxCap, gpuData.tree64Width, gpuData.tree64Height, maxSz);
        uint32_t matCap = chooseDataDims(maxCap, gpuData.materialIDWidth, gpuData.materialIDHeight, maxSz);
        if (bgfx::isValid(gpuData.tree64Texture)) bgfx::destroy(gpuData.tree64Texture);
        if (bgfx::isValid(gpuData.materialIDTexture)) bgfx::destroy(gpuData.materialIDTexture);
        gpuData.tree64Texture = createDataTexture(gpuData.tree64Width, gpuData.tree64Height, tree64Texels);
        gpuData.materialIDTexture = createDataTexture8(gpuData.materialIDWidth, gpuData.materialIDHeight, materialIDTexels);

        // Palette texture.
        if (!globalPaletteTexels.empty()) {
            while (globalPaletteTexels.size() % 4 != 0) globalPaletteTexels.push_back(0);
            uint32_t pw = static_cast<uint32_t>(globalPaletteTexels.size() / 4);
            if (bgfx::isValid(gpuData.materialPaletteTexture)) bgfx::destroy(gpuData.materialPaletteTexture);
            gpuData.materialPaletteTexture = createDataTexture(std::max(pw, 1u), 1, globalPaletteTexels);
            gpuData.paletteWidth = std::max(pw, 1u);
        }

        gpuData.tree64Alloc.reset(geomCap); gpuData.tree64Alloc.reserve(0, geomUsedPadded);
        gpuData.materialIDAlloc.reset(matCap); gpuData.materialIDAlloc.reserve(0, matUsedPadded);

        // Pre-allocate header texture at max possible slots so it never grows.
        uint32_t maxSlots = maxSz / 4;
        gpuData.headerCapacity = std::min(withHeadroom(static_cast<uint32_t>(scene.chunks.size())), maxSlots);
        if (scene.chunks.size() > static_cast<size_t>(maxSlots))
            core::error("buildDataAndHeaderTextures: {} chunks exceed header texture slot limit {}", scene.chunks.size(), maxSlots);
        std::vector<GPUChunkHeader> headers(gpuData.headerCapacity, degenerateHeader());
        for (uint32_t h = 0; h < scene.chunks.size() && h < gpuData.headerCapacity; h++) {
            const Chunk& c = scene.chunks[h];
            if (c.alive && c.geometryPoolIndex >= 0)
                headers[h] = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene);
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

        core::render("createTexturesForScene: done geomTex=({}x{}) matTex=({}x{}) headers={} grids={} loose={}",
                     gpuData.tree64Width, gpuData.tree64Height,
                     gpuData.materialIDWidth, gpuData.materialIDHeight,
                     gpuData.headerCapacity, scene.grids.size(), scene.looseChunkCount);

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
            hdr = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene);
        }
        uint16_t x = static_cast<uint16_t>(h * 4);
        const bgfx::Memory* mem = bgfx::copy(&hdr, sizeof(hdr));
        bgfx::updateTexture2D(gpuData.headerTexture, 0, 0, x, 0, 4, 1, mem);
    }
}
