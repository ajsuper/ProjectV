#ifndef PROJV_MANAGE_RESOURCES_H
#define PROJV_MANAGE_RESOURCES_H

#include <vector>
#include <iostream>

#include "data_structures/texture.h"
#include "data_structures/framebuffer.h"
#include "data_structures/resources.h"
#include "data_structures/constructedRenderer.h"
#include "data_structures/rendererSpecification.h"

#include "graphics/type_mapping.h"
#include "graphics/disk_io.h"

#include "bgfx/bgfx.h"

namespace projv::graphics {
    std::vector<bgfx::Attachment> getTextureAttachments(const std::unordered_map<uint, bgfx::TextureHandle>& textureHandles, std::vector<uint> textureIDs);

    bgfx::ProgramHandle createShaderProgram(const bgfx::ShaderHandle& vertexShader, const bgfx::ShaderHandle& fragmentShaderHandle);

    ConstructedTextures constructTextures(const std::vector<Texture>& textures);

    ConstructedFramebuffers constructFramebuffers(const Resources& resources);

    std::unordered_map<std::string, bgfx::UniformHandle> constructUniforms(const std::vector<Uniform>& uniforms);

    std::unordered_map<uint, bgfx::ShaderHandle> constructShaders(const std::vector<Shader>& shaders);

    std::vector<BGFXDependencyGraph> constructRenderPasses(const BGFXResources& constructedResources, const std::vector<FrameBuffer>& frameBuffers, const std::vector<RenderPass>& renderPasses);

    std::shared_ptr<ConstructedRenderer> constructRendererSpecification(RendererSpecification &renderer, bgfx::ShaderHandle vertexShader);

    void resizeFramebuffersAndTheirTexturesIfNeeded(ConstructedTextures& textures, ConstructedFramebuffers& frameBuffers, int windowWidth, int windowHeight, int prevWindowWidth, int prevWindowHeight);
}

#endif
