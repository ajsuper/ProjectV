#ifndef PROJV_GPU_INTERFACE_H
#define PROJV_GPU_INTERFACE_H

#include <vector>

#include "data_structures/constructedRenderer.h"
#include "data_structures/gpuData.h"
#include "data_structures/scene.h"

#include "bgfx/bgfx.h"

namespace projv::graphics {
    template <typename T>
    void setUniformToValue(std::shared_ptr<ConstructedRenderer> constructedRenderer, std::string uniformName, T& data) {
        bgfx::UniformHandle& uniform = constructedRenderer->resources.uniformHandles[uniformName];
        constructedRenderer->resources.uniformValues[uniformName].resize(sizeof(T));
        memcpy(constructedRenderer->resources.uniformValues[uniformName].data(), &data, sizeof(T));
    }

    void setTextureToData(std::shared_ptr<ConstructedRenderer> constructedRenderer, uint textureID, unsigned char * data, uint textureWidth, uint textureHeight);

    bgfx::TextureHandle createArbitraryTexture(std::vector<uint32_t>& data);

    bgfx::TextureHandle createHeaderTexture(std::vector<projv::GPUChunkHeader>& headers);

    GPUData createTexturesForScene(projv::Scene& scene);
}

#endif
