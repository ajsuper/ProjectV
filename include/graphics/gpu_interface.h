#ifndef PROJV_GPU_INTERFACE_H
#define PROJV_GPU_INTERFACE_H

#include <vector>
#include <iostream>
#include <type_traits>

#include "data_structures/constructedRenderer.h"
#include "data_structures/gpuData.h"
#include "data_structures/scene.h"

#include "core/log.h"

#include "bgfx/bgfx.h"

namespace projv::graphics {
    template <typename T>
    void setUniformToValue(std::shared_ptr<ConstructedRenderer> constructedRenderer, std::string uniformName, T& data) {
        std::vector<uint8_t> buffer;
        if constexpr (std::is_same_v<T, float>) {
            core::warn("Function: setUniformToValue. Typename T for data is a float. float is not supported, it will be passed as a vec4");
            buffer.resize(sizeof(float[4]));
            core::vec4 temp(data, 0.0f, 0.0f, 0.0f);
            memcpy(buffer.data(), &temp, sizeof(float[4]));
        } else if constexpr (std::is_same_v<T, core::vec2>) {
            core::warn("Function: setUniformToValue. Typename T for data is a vec2. vec2 is not supported, it will be passed as a vec4");
            buffer.resize(sizeof(float[4]));
            core::vec4 temp(data.x, data.y, 0.0f, 0.0f);
            memcpy(buffer.data(), &temp, sizeof(float[4]));
        } else if constexpr (std::is_same_v<T, core::vec3>) {
            core::warn("Function: setUniformToValue. Typename T for data is a vec3. vec3 is not supported, it will be passed as a vec4");
            buffer.resize(sizeof(float[4]));
            core::vec4 temp(data.x, data.y, data.z, 0.0f);
            memcpy(buffer.data(), &temp, sizeof(float[4]));
        } else if constexpr (std::is_same_v<T, core::vec4>) {
            buffer.resize(sizeof(float[4]));
            memcpy(buffer.data(), &data, sizeof(float[4]));
        } else if constexpr (std::is_same_v<T, core::mat3>) {
            buffer.resize(sizeof(float[9]));
            memcpy(buffer.data(), &data, sizeof(float[9]));
        } else if constexpr (std::is_same_v<T, core::mat4>) {
            buffer.resize(sizeof(float[16]));
            memcpy(buffer.data(), &data, sizeof(float[16]));
        } else {
            core::error("Function: setUniformToValue. Typename T for data is unkown.");
            return;
        }

        constructedRenderer->resources.uniformValues[uniformName] = std::move(buffer);
    }

    void setTextureToData(std::shared_ptr<ConstructedRenderer> constructedRenderer, uint textureID, unsigned char * data, uint textureWidth, uint textureHeight);

    bgfx::TextureHandle createArbitraryTexture(std::vector<uint32_t>& data);

    bgfx::TextureHandle createHeaderTexture(std::vector<projv::GPUChunkHeader>& headers);

    GPUData createTexturesForScene(projv::Scene& scene);
}

#endif
