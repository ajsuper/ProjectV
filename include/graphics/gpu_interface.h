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
     * Creates a bgfx::TextureHandle from an std::vector<uint32_t>. It is row-major, RGBA32U. Examples of how to read from it can be found in the uint tree64s(int index); function in the tree64 traversal shader.
     * @param data The std::vector<uint32_t>& to create the texture based on. It is copied by bgfx.
     * @return Returns a bgfx::TextureHandle for the texture created from the data.
     */
    bgfx::TextureHandle createArbitraryTexture(std::vector<uint32_t>& data);

    /**
     * Creates a bgfx::TextureHandle from a collection of projv::GPUChunkHeader. It is row-major, RGBA32U. Examples of how to read from it can be found in the chunkHeader headers(int index); function in the tree64 traversal shader.
     * @param data The std::vector<uint32_t>& to create the texture based on. It is copied by bgfx.
     * @return Returns a bgfx::TextureHandle for the texture created from the data.
     */
    bgfx::TextureHandle createHeaderTexture(std::vector<projv::GPUChunkHeader>& headers);

    /**
     * Creates a projv::GPUData with all of the resources needed to pass a projv::Scene to the GPU.
     * Performs the bulk upload once, then seeds the persistent allocation table (suballocators,
     * blobRanges, capacities with headroom) so the incremental primitives below can mutate the pools
     * afterward without a full rebuild. Any chunk still owning geometry (geometryPoolIndex < 0) is
     * interned into the pool first, so the GPU path is single-model.
     * @param scene A projv::Scene& containing the entire scene to be rendered.
     * @return Returns a projv::GPUData containing all of the created resources for rendering.
     */
    GPUData createTexturesForScene(projv::Scene& scene);

    /**
     * Uploads a chunk that is not yet resident on the GPU: allocates a GPU range for its geometry-pool
     * blob if this is the blob's first resident reference (else reuses the shared range), writes the
     * chunk's header row (== its handle), and registers it in the loose list or a grid cell per its
     * residency key. Grows a data texture if an allocation overflows. Safe to call headless (skips the
     * bgfx uploads when the textures are invalid) so the allocation bookkeeping is unit-testable.
     * @param scene The scene owning the chunk and geometry pool.
     * @param gpuData The GPU data/allocation table to mutate.
     * @param handle The chunk to upload (index into scene.chunks; must be alive and pooled).
     */
    void addChunkToGPU(projv::Scene& scene, GPUData& gpuData, ChunkHandle handle);

    /**
     * Reflects an in-memory geometry edit (e.g. a mutability edit) on the GPU: re-fits the chunk's
     * blob into its GPU range (in place if it still fits, else frees and reallocates), re-uploads the
     * geometry, and rewrites the single header row. No grid/loose membership change.
     * @param scene The scene owning the chunk and geometry pool.
     * @param gpuData The GPU data/allocation table to mutate.
     * @param handle The chunk whose pool blob changed.
     */
    void updateChunkGeometryOnGPU(projv::Scene& scene, GPUData& gpuData, ChunkHandle handle);

    /**
     * Removes a chunk from the GPU: decrements its blob's refcount (freeing the blob's GPU range and
     * recycling its pool slot at zero), writes a degenerate header row (scale <= 0) the shader skips,
     * clears its loose-list/grid-cell entry, and recycles the chunk slot via scene.chunkFreeList.
     * @param scene The scene owning the chunk and geometry pool.
     * @param gpuData The GPU data/allocation table to mutate.
     * @param handle The chunk to remove.
     */
    void removeChunkFromGPU(projv::Scene& scene, GPUData& gpuData, ChunkHandle handle);

    /**
     * Destroys every texture and uniform held by a GPUData and resets its layout state. Fixes the
     * leak from repeatedly rebuilding a scene; call before reloading.
     * @param gpuData The GPU data to tear down.
     */
    void destroyGPUData(GPUData& gpuData);
}

#endif
