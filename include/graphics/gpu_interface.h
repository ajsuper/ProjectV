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
    /**
     * Sets the uniform value in constructed renderer. Values are passed to the GPU automatically when the constructed renderer is performed.
     * @param constructedRenderer The constructed renderer where the values will be updated.
     * @param uniformName The name of the uniform to be updated in constructedRenderer. This corresponds to the name specified in the resource json.
     * @param data A reference to the data to set the uniform to. ONLY supports: vec4, mat3, mat4, but float, vec2, and vec3 will be automatically converted to vec4.
     */
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

    /**
     * Allows you to set the data for a texture in resources.json created with the CPUBuffer origin.
     * @param constructedRenderer The constructed renderer where the values will be updated.
     * @param textureID The texID specified in your resources.json.
     * @param data A raw char* to the data you want to set to texture to. Ensure the resolutions and formats match.
     * @param textureWidth The width of the texture you are passing. It automatically checks if they are the same and warns and resizes if not.
     * @param textureHeight The height of the texture you are passing. It automatically checks if they are the same and warns and resizes if not.
     */
    void setTextureToData(std::shared_ptr<ConstructedRenderer> constructedRenderer, uint textureID, unsigned char * data, uint textureWidth, uint textureHeight);

    /**
     * Creates a bgfx::TextureHandle from an std::vector<uint32_t>. It is row-major, RGBA32U. Examples of how to read from it can be found in the uint octrees(int index); function in the octree traversal shader.
     * @param data The std::vector<uint32_t>& to create the texture based on. It is copied by bgfx.
     * @return Returns a bgfx::TextureHandle for the texture created from the data.
     */
    bgfx::TextureHandle createArbitraryTexture(std::vector<uint32_t>& data);

    /**
     * Creates a bgfx::TextureHandle from a collection of projv::GPUChunkHeader. It is row-major, RGBA32U. Examples of how to read from it can be found in the chunkHeader headers(int index); function in the octree traversal shader.
     * @param data The std::vector<uint32_t>& to create the texture based on. It is copied by bgfx.
     * @return Returns a bgfx::TextureHandle for the texture created from the data.
     */
    bgfx::TextureHandle createHeaderTexture(std::vector<projv::GPUChunkHeader>& headers);

    /**
     * Creates a projv::GPUData with all of the resources needed to pass a projv::Scene to the GPU.
     * @param scene A projv::Scene& containing the entire scene to be rendered.
     * @return Returns a projv::GPUData containing all of the created resources for rendering.
     */
    GPUData createTexturesForScene(projv::Scene& scene);
}

#endif
