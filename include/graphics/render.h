#ifndef PROJV_RENDER_H
#define PROJV_RENDER_H

#include <iostream>
#include <unordered_map>

#include "data_structures/constructedRenderer.h"
#include "data_structures/gpuData.h"
#include "construct_render.h"
#include "data_structures/texture.h"
#include "render_instance.h"

namespace projv::graphics {
    void updateUniforms(const std::unordered_map<std::string, bgfx::UniformHandle>& uniformHandles, const std::unordered_map<std::string, std::vector<uint8_t>>& uniformValues);

    void resizeFramebuffersAndTheirTexturesIfNeeded(ConstructedTextures& textures, ConstructedFramebuffers& frameBuffers, int windowWidth, int windowHeight, int prevWindowWidth, int prevWindowHeight);

    void performRenderPasses(std::shared_ptr<ConstructedRenderer> constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, void *viewMat, void *projMat, GPUData* gpuData);

    void renderConstructedRenderer(RenderInstance &renderInstance, std::shared_ptr<ConstructedRenderer> constructedRenderer, void *viewMat, void *projMat, GPUData* gpuData);
}

#endif
