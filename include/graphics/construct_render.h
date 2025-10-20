#ifndef PROJV_RENDER_SPECIFICATION_H
#define PROJV_RENDER_SPECIFICATION_H

#include <vector>
#include <unordered_map>
#include <iostream>

#include "data_structures/uniform.h"
#include "data_structures/texture.h"
#include "data_structures/renderInstance.h"
#include "renderer_loader.h"
#include "data_structures/constructedRenderer.h"
#include "data_structures/rendererSpecification.h"
#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv::graphics {
    bgfx::ShaderHandle loadShader(const std::string &path);

    std::vector<bgfx::Attachment> getTextureAttachments(ConstructedRenderer &constructedRenderer, std::vector<uint> textureIDs);

    bgfx::UniformType::Enum mapUniformType(UniformType uniformType);

    std::vector<std::pair<bgfx::UniformHandle, uint>> getDependenciesList(RendererSpecification &rendererSpecification, ConstructedRenderer &constructedRenderer, RenderPass &renderPass);

    bgfx::ProgramHandle createShaderProgram(bgfx::ShaderHandle vertexShader, bgfx::ShaderHandle fragmentShaderHandle);

    void constructFramebuffers(ConstructedRenderer& constructedRenderer, RendererSpecification& renderer, Resources& resources);

    void constructUniforms(ConstructedRenderer& constructedRenderer, RendererSpecification& renderer, Resources& resources);

    void constructShaders(ConstructedRenderer& constructedRenderer, RendererSpecification& renderer, Resources& resources);

    void constructRenderPasses(ConstructedRenderer& constructedRenderer, RendererSpecification& renderer, std::vector<RenderPass>& renderPasses);

    ConstructedRenderer constructRendererSpecification(RendererSpecification &renderer, bgfx::ShaderHandle vertexShader);

    void useConstructedRenderer(RenderInstance &renderInstance, ConstructedRenderer &constructedRenderer);

    void moveRendererSpecification(RenderInstance &renderInstance, RendererSpecification &rendererSpecification, uint rendererID);

    RendererSpecification &getRendererSpecification(RenderInstance &renderInstance, uint rendererID);
}

#endif
