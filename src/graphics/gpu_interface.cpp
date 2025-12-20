#include "graphics/gpu_interface.h"

namespace projv::graphics {
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
        core::info("Data height: {}", textureHeight);
        core::info("Data width: {}", dataWidth);
        if(pixelSize % textureHeight != 0) {
            dataWidth += 1;
        } 
        const bgfx::Memory* dataMemory = bgfx::copy(data.data(), dataWidth * textureHeight * sizeof(uint32_t) * 4);
        bgfx::TextureHandle dataTexture = bgfx::createTexture2D(dataWidth, textureHeight, false, 1, bgfx::TextureFormat::RGBA32U, BGFX_TEXTURE_NONE|BGFX_SAMPLER_POINT, dataMemory);
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
            core::info("Serializing data for chunk {}", scene.chunks[i].header.chunkID);
            core::info("Octree size for chunk: {}", scene.chunks[i].geometryData.size());
            core::info("Voxel Type Data size for chunk: {}", scene.chunks[i].voxelTypeData.size());
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
        
        core::info("Creating octree texture. std::vector<uint32_t> size: {}", octreeData.size());
        gpuData.octreeTexture = createArbitraryTexture(octreeData);
        core::info("Creating voxel type data texture. std::vector<uint32_t> size: {}", voxelTypeData.size());
        gpuData.voxelTypeDataTexture = createArbitraryTexture(voxelTypeData);
        core::info("Creating header texture. std::vector<projv::GPUChunkHeader> size: {}", gpuChunkHeaderData.size());
        gpuData.headerTexture = createHeaderTexture(gpuChunkHeaderData);
        
        gpuData.octreeSampler = bgfx::createUniform("octreeData", bgfx::UniformType::Sampler);
        gpuData.voxelTypeDataSampler = bgfx::createUniform("voxelTypeData", bgfx::UniformType::Sampler);
        gpuData.headerSampler = bgfx::createUniform("headerData", bgfx::UniformType::Sampler);

        return gpuData;
    }
}
