#include "graphics/scene.h"

namespace projv::graphics {
    bgfx::TextureHandle createArbitraryTexture(std::vector<uint32_t>& data) {
        int maxTextureSize = bgfx::getCaps()->limits.maxTextureSize;
        int pixelSize = data.size() / 4;
        int dataWidth = (pixelSize / maxTextureSize);
        std::cout << "Data width: " << dataWidth << std::endl;
        if(pixelSize % maxTextureSize != 0) {
            dataWidth += 1;
        } 
        const bgfx::Memory* dataMemory = bgfx::copy(data.data(), dataWidth * maxTextureSize * sizeof(uint32_t) * 4);
        bgfx::TextureHandle dataTexture = bgfx::createTexture2D(dataWidth, maxTextureSize, false, 1, bgfx::TextureFormat::RGBA32U, BGFX_TEXTURE_NONE|BGFX_SAMPLER_POINT, dataMemory);
        return dataTexture;
    }

    bgfx::TextureHandle createHeaderTexture(std::vector<projv::GPUChunkHeader>& headers) {
        const bgfx::Memory* headerMemory = bgfx::copy(  
            headers.data(),  
            headers.size() * sizeof(projv::GPUChunkHeader)
        );

        bgfx::TextureHandle headerTexture = bgfx::createTexture2D(
            headers.size() * 3,
            1,
            false,
            1,
            bgfx::TextureFormat::RGBA32U,
            BGFX_TEXTURE_NONE|BGFX_SAMPLER_POINT,
            headerMemory
        );
        return headerTexture;
    }

    GPUData createTexturesForScene(projv::Scene& scene) {
        GPUData gpuData;
        std::vector<uint32_t> octreeData;
        std::vector<uint32_t> voxelTypeData;
        std::vector<projv::GPUChunkHeader> gpuChunkHeaderData;
        // Combine the voxelTypeData, octree, and headers for each chunk into just 3 vectors.
        for(size_t i = 0; i < scene.chunks.size(); i++) {
            int octreeStartIndex = octreeData.size();
            int voxelTypeDataStartIndex = voxelTypeData.size();

            octreeData.insert(octreeData.end(), scene.chunks[i].geometryData.begin(), scene.chunks[i].geometryData.end());
            for(size_t j = 0; j < scene.chunks[i].geometryData.size(); j++) {
                //octreeData.emplace_back(scene.chunks[i].geometryData[j]);
            }
            voxelTypeData.insert(voxelTypeData.end(), scene.chunks[i].voxelTypeData.begin(), scene.chunks[i].voxelTypeData.end());

            int octreeEndIndex = octreeData.size();
            int voxelTypeDataEndIndex = voxelTypeData.size();

            projv::GPUChunkHeader gpuChunkHeader;
            gpuChunkHeader.chunkID = scene.chunks[i].header.chunkID;
            gpuChunkHeader.geometryStartIndex = octreeStartIndex;
            gpuChunkHeader.geometryEndIndex = octreeEndIndex;
            gpuChunkHeader.voxelTypeDataStartIndex = voxelTypeDataStartIndex;
            gpuChunkHeader.voxelTypeDataEndIndex = voxelTypeDataEndIndex;
            gpuChunkHeader.positionX = scene.chunks[i].header.position.x;
            gpuChunkHeader.positionY = scene.chunks[i].header.position.y;
            gpuChunkHeader.positionZ = scene.chunks[i].header.position.z;
            gpuChunkHeader.resolution = scene.chunks[i].header.resolution;
            gpuChunkHeader.scale = scene.chunks[i].header.scale;
            gpuChunkHeader.padding[0] = 0;
            gpuChunkHeader.padding[1] = 0;

            gpuChunkHeaderData.emplace_back(gpuChunkHeader);
        }

        gpuData.octreeTexture = createArbitraryTexture(octreeData);
        gpuData.voxelTypeDataTexture = createArbitraryTexture(voxelTypeData);
        gpuData.headerTexture = createHeaderTexture(gpuChunkHeaderData);
        
        gpuData.octreeSampler = bgfx::createUniform("octreeData", bgfx::UniformType::Sampler);
        gpuData.voxelTypeDataSampler = bgfx::createUniform("voxelTypeData", bgfx::UniformType::Sampler);
        gpuData.headerSampler = bgfx::createUniform("headerData", bgfx::UniformType::Sampler);

        return gpuData;
    }
}
