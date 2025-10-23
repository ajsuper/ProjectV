#ifndef PROJV_RENDER_H
#define PROJV_RENDER_H

#include <iostream>

#include "data_structures/constructedRenderer.h"
#include "data_structures/gpuData.h"
#include "construct_render.h"
#include "render_instance.h"

namespace projv::graphics {
    void updateUniforms(ConstructedRenderer& constructedRenderer);

    void resizeFramebuffersAndTheirTextures(ConstructedRenderer& constructedRenderer, int windowWidth, int windowHeight, int prevWindowWidth, int prevWindowHeight);

    void performRenderPasses(ConstructedRenderer& constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, void *viewMat, void *projMat, GPUData* gpuData = nullptr);

    void renderIt(RenderInstance &renderInstance, std::shared_ptr<ConstructedRenderer> constructedRenderer, void *viewMat, void *projMat, GPUData* gpuData = nullptr);
}

#endif
