#ifndef PROJV_RENDER_SPECIFICATION_H
#define PROJV_RENDER_SPECIFICATION_H

#include <vector>
#include <unordered_map>
#include <iostream>
#include <memory>

#include "data_structures/uniform.h"
#include "data_structures/texture.h"
#include "data_structures/texture.h"
#include "data_structures/framebuffer.h"
#include "renderer_loader.h"
#include "render_instance.h"
#include "data_structures/constructedRenderer.h"
#include "data_structures/rendererSpecification.h"
#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv::graphics {
    bgfx::ShaderHandle loadShader(const std::string &path);

    std::vector<bgfx::Attachment> getTextureAttachments(const std::unordered_map<uint, bgfx::TextureHandle>& textureHandles, std::vector<uint> textureIDs);

    bgfx::UniformType::Enum mapUniformType(UniformType uniformType);

    std::vector<std::pair<bgfx::UniformHandle, uint>> getDependenciesList(const std::vector<FrameBuffer>& frameBuffers, const std::unordered_map<uint, bgfx::UniformHandle>& textureSamplerHandles, RenderPass &renderPass);

    bgfx::ProgramHandle createShaderProgram(const bgfx::ShaderHandle& vertexShader, const bgfx::ShaderHandle& fragmentShaderHandle);

    ConstructedTextures constructTextures(const std::vector<Texture>& textures);

    ConstructedFramebuffers constructFramebuffers(const Resources& resources);

    std::unordered_map<std::string, bgfx::UniformHandle> constructUniforms(const std::vector<Uniform>& uniforms);

    std::unordered_map<uint, bgfx::ShaderHandle> constructShaders(const std::vector<Shader>& shaders);

    std::vector<BGFXDependencyGraph> constructRenderPasses(const BGFXResources& constructedResources, const std::vector<FrameBuffer>& frameBuffers, const std::vector<RenderPass>& renderPasses);

    std::shared_ptr<ConstructedRenderer> constructRendererSpecification(RendererSpecification &renderer, bgfx::ShaderHandle vertexShader);
}

#endif
