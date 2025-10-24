#ifndef PROJV_PERFORM_RENDERER_H
#define PROJV_PERFORM_RENDERER_H

#include <unordered_map>

#include "data_structures/constructedRenderer.h"
#include "data_structures/gpuData.h"

#include "render_instance.h"
#include "bgfx/bgfx.h"

namespace projv::graphics {
    void updateUniforms(const std::unordered_map<std::string, bgfx::UniformHandle>& uniformHandles, const std::unordered_map<std::string, std::vector<uint8_t>>& uniformValues);

    void performRenderPasses(std::shared_ptr<ConstructedRenderer> constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, void *viewMat, void *projMat, GPUData* gpuData);

    void renderConstructedRenderer(RenderInstance &renderInstance, std::shared_ptr<ConstructedRenderer> constructedRenderer, void *viewMat, void *projMat, GPUData* gpuData);

    bgfx::UniformType::Enum mapUniformType(UniformType uniformType);

    std::vector<std::pair<bgfx::UniformHandle, uint>> getDependenciesList(const std::vector<FrameBuffer>& frameBuffers, const std::unordered_map<uint, bgfx::UniformHandle>& textureSamplerHandles, RenderPass &renderPass);

    UniformType getUniformType(std::string type);

    bgfx::TextureFormat::Enum getFormat(std::string format);

    TextureOrigin getOrigin(std::string origin);
}

#endif
