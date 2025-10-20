#ifndef PROJV_RENDER_H
#define PROJV_RENDER_H

#include <iostream>

#include "data_structures/constructedRenderer.h"
#include "data_structures/renderInstance.h"
#include "data_structures/gpuData.h"
#include "construct_render.h"

namespace projv::graphics {
    void updateUniforms(ConstructedRenderer& constructedRenderer);

    void resizeFramebuffersAndTheirTextures(ConstructedRenderer& constructedRenderer, int windowWidth, int windowHeight, int prevWindowWidth, int prevWindowHeight);

    void performRenderPasses(ConstructedRenderer& constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, void *viewMat, void *projMat, GPUData* gpuData = nullptr);

    void renderIt(RenderInstance &renderInstance, ConstructedRenderer &constructedRenderer, void *viewMat, void *projMat, GPUData* gpuData = nullptr);

    template <typename T>
    void setUniformToValue(ConstructedRenderer& constructedRenderer, std::string uniformName, T& data) {
        bgfx::UniformHandle& uniform = constructedRenderer.resources.uniformHandles[uniformName];
        constructedRenderer.resources.uniformValues[uniformName].resize(sizeof(T));
        memcpy(constructedRenderer.resources.uniformValues[uniformName].data(), &data, sizeof(T));
    }
}

#endif
