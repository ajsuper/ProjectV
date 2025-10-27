#ifndef PROJV_PERFORM_RENDERER_H
#define PROJV_PERFORM_RENDERER_H

#include <unordered_map>
#include <iostream>
#include <optional>

#include "data_structures/constructedRenderer.h"
#include "data_structures/gpuData.h"

#include "graphics/manage_resources.h"

#include "render_instance.h"
#include "bgfx/bgfx.h"

namespace projv::graphics {
    void updateUniforms(const std::unordered_map<std::string, bgfx::UniformHandle>& uniformHandles, const std::unordered_map<std::string, std::vector<uint8_t>>& uniformValues);

    void performRenderPasses(std::shared_ptr<ConstructedRenderer> constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, core::mat4 viewMat, core::mat4 projMat, GPUData* gpuData);

    void renderConstructedRenderer(RenderInstance &renderInstance, std::shared_ptr<ConstructedRenderer> constructedRenderer, GPUData* gpuData);
}

#endif
