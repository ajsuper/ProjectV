#ifndef PROJV_RENDER_SPECIFICATION_H
#define PROJV_RENDER_SPECIFICATION_H

#include <vector>
#include <unordered_map>
#include <iostream>

#include "data_structures/uniform.h"
#include "data_structures/texture.h"
#include "data_structures/renderInstance.h"
#include "data_structures/texture.h"
#include "renderer_loader.h"
#include "data_structures/constructedRenderer.h"
#include "data_structures/rendererSpecification.h"
#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv::graphics {
    bgfx::ShaderHandle loadShader(const std::string &path);

    std::vector<bgfx::Attachment> getTextureAttachments(const std::unordered_map<uint, bgfx::TextureHandle>& textureHandles, std::vector<uint> textureIDs);

    bgfx::UniformType::Enum mapUniformType(UniformType uniformType);

    std::vector<std::pair<bgfx::UniformHandle, uint>> getDependenciesList(const std::vector<FrameBuffer>& frameBuffers, const std::unordered_map<uint, bgfx::UniformHandle>& textureSamplerHandles, RenderPass &renderPass);

    bgfx::ProgramHandle createShaderProgram(bgfx::ShaderHandle vertexShader, bgfx::ShaderHandle fragmentShaderHandle);

    ConstructedTextures constructTextures(const std::vector<Texture>& textures);

    void constructFramebuffers(ConstructedRenderer& constructedRenderer, const Resources& resources);

    void constructUniforms(ConstructedRenderer& constructedRenderer, const Resources& resources);

    void constructShaders(ConstructedRenderer& constructedRenderer, const Resources& resources);

    void constructRenderPasses(ConstructedRenderer& constructedRenderer, const RendererSpecification& renderer, std::vector<RenderPass>& renderPasses);

    ConstructedRenderer constructRendererSpecification(RendererSpecification &renderer, bgfx::ShaderHandle vertexShader);

    void useConstructedRenderer(RenderInstance &renderInstance, ConstructedRenderer &constructedRenderer);

    void moveRendererSpecification(RenderInstance &renderInstance, RendererSpecification &rendererSpecification, uint rendererID);

    RendererSpecification &getRendererSpecification(RenderInstance &renderInstance, uint rendererID);
}

#endif
