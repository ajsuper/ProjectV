#include "graphics/gpu_interface.h"

#include <chrono>
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
    g.voxelTypeDataStartIndex = r.typeTexelOffset * 4u;
    g.voxelTypeDataEndIndex = r.typeTexelOffset * 4u + r.typeUintLen;
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
    g.padding[0] = 0;
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
            }
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            core::warn("[PERF] packGeometryTexels: {} nodes: {:.2f}ms", nodes, ms);
            return out;
        }

        // Pack a blob's voxelType uints into RGBA32U texels (4 uints per texel, zero-padded tail).
        std::vector<uint32_t> packVoxelTypeTexels(const std::vector<uint32_t>& data) {
            auto t0 = std::chrono::high_resolution_clock::now();
            uint32_t texels = static_cast<uint32_t>((data.size() + 3) / 4);
            std::vector<uint32_t> out(size_t(texels) * 4, 0u);
            std::copy(data.begin(), data.end(), out.begin());
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            core::warn("[PERF] packVoxelTypeTexels: {} texels: {:.2f}ms", texels, ms);
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
            if (!bgfx::isValid(gpuData.headerTexture)) { auto t1 = std::chrono::high_resolution_clock::now(); double ms = std::chrono::duration<double, std::milli>(t1 - t0).count(); core::warn("[PERF] syncSceneTables (headless): {:.2f}ms", ms); return; }
            if (bgfx::isValid(gpuData.gridInfoTexture)) bgfx::destroy(gpuData.gridInfoTexture);
            if (bgfx::isValid(gpuData.cellMapTexture)) bgfx::destroy(gpuData.cellMapTexture);
            if (bgfx::isValid(gpuData.looseListTexture)) bgfx::destroy(gpuData.looseListTexture);
            gpuData.gridInfoTexture = createUintRowTexture(gridInfoData);
            gpuData.cellMapTexture = createCellMapTexture(cellMapData);
            gpuData.looseListTexture = createCellMapTexture(looseList);
            auto t1 = std::chrono::high_resolution_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            core::warn("[PERF] syncSceneTables: {} grids {} loose {} chunks: {:.2f}ms",
                       scene.grids.size(), looseList.size(), scene.chunks.size(), ms);
        }
    }

    // Build (or rebuild) the tree64/voxelType/header textures from current scene state via one contiguous
    // bulk pack + a single createTexture2D per texture. Reassigns every blob's GPU range and reseeds the
    // allocators. Destroys the old data/header textures first; leaves samplers and the small scene tables alone.
    static void buildDataAndHeaderTextures(projv::Scene& scene, GPUData& gpuData) {
        auto t0 = std::chrono::high_resolution_clock::now();
        // 1. Assign each live blob a contiguous GPU range and pack the linear texel buffers.
        gpuData.blobRanges.assign(scene.geometryPool.size(), GPUBlobRange{});
        std::vector<uint32_t> tree64Texels, voxelTypeTexels;
        uint32_t geomUsed = 0, typeUsed = 0;
        uint32_t liveBlobs = 0;
        for (size_t b = 0; b < scene.geometryPool.size(); b++) {
            const GeometryBlob& blob = scene.geometryPool[b];
            if (blob.refCount == 0) { gpuData.blobRanges[b].uploaded = false; continue; }
            liveBlobs++;
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
                headers[h] = makeHeader(c, gpuData.blobRanges[c.geometryPoolIndex], scene);
        }
        if (bgfx::isValid(gpuData.headerTexture)) bgfx::destroy(gpuData.headerTexture);
        gpuData.headerTexture = createHeaderTexture(headers);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        core::warn("[PERF] buildDataAndHeaderTextures: {} liveBlobs {} chunks {} geomTexels {} typeTexels: {:.2f}ms",
                   liveBlobs, scene.chunks.size(), geomUsed, typeUsed, ms);
    }

    GPUData createTexturesForScene(projv::Scene& scene) {
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
        gpuData.voxelTypeDataSampler = bgfx::createUniform("voxelTypeData", bgfx::UniformType::Sampler);
        gpuData.headerSampler = bgfx::createUniform("headerData", bgfx::UniformType::Sampler);
        gpuData.gridInfoSampler = bgfx::createUniform("gridInfo", bgfx::UniformType::Sampler);
        gpuData.cellMapSampler = bgfx::createUniform("cellMap", bgfx::UniformType::Sampler);
        gpuData.looseListSampler = bgfx::createUniform("looseList", bgfx::UniformType::Sampler);

        return gpuData;
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
        killT(gpuData.tree64Texture); killT(gpuData.voxelTypeDataTexture); killT(gpuData.headerTexture);
        killT(gpuData.gridInfoTexture); killT(gpuData.cellMapTexture); killT(gpuData.looseListTexture);
        killU(gpuData.tree64Sampler); killU(gpuData.voxelTypeDataSampler); killU(gpuData.headerSampler);
        killU(gpuData.gridInfoSampler); killU(gpuData.cellMapSampler); killU(gpuData.looseListSampler);
        gpuData = GPUData{};
    }
}
