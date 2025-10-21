#ifndef PROJV_RESOURCE_MANAGEMENT
#define PROJV_RESOURCE_MANAGEMENT

#include <iostream>

#include "../../external/bgfx/include/bgfx/bgfx.h"
#include "data_structures/constructedRenderer.h"

namespace projv::graphics {
    template <typename T>
    void setUniformToValue(ConstructedRenderer& constructedRenderer, std::string uniformName, T& data) {
        bgfx::UniformHandle& uniform = constructedRenderer.resources.uniformHandles[uniformName];
        constructedRenderer.resources.uniformValues[uniformName].resize(sizeof(T));
        memcpy(constructedRenderer.resources.uniformValues[uniformName].data(), &data, sizeof(T));
    }

    void setTextureToData(ConstructedRenderer& constructedRenderer, uint textureID, unsigned char * data, uint textureWidth, uint textureHeight);
}

#endif
