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
    /**
    * Creates a list of texture attachments based off of a list of textureIDs and their matching handles.
    * @param textureHandles An std::unordered_map<uint, bgfx::TextureHandle>& containing at least the texture handles in our textureID vector.
    * @return Returns an std::vector<bgfx::Attachment> containing all of the created texture attachments for our texture IDs.
    */
    std::vector<bgfx::Attachment> getTextureAttachments(const std::unordered_map<uint, bgfx::TextureHandle>& textureHandles, const std::vector<uint> textureIDs);

    /**
    * Creates a bgfx::ProgramHandle from a vertex shader and a fragment shader.
    * @param vertexShader The vertex shader to be put into the shader program.
    * @param fragmentShaderHandle The fragment shader to be put into the shader program.
    * @return Returns a bgfx::ProgramHandle representing the compiled shader program.
    */
    bgfx::ProgramHandle createShaderProgram(const bgfx::ShaderHandle& vertexShader, const bgfx::ShaderHandle& fragmentShaderHandle);

    /**
    * Constructs a set of textures from a list of Texture objects, mapping their internal IDs to bgfx::TextureHandle.
    * @param textures An std::vector<projv::Texture>, each texture containing a texture ID and possibly other metadata.
    * @return Returns a ConstructedTextures object that holds the mapped texture handles for use in rendering.
    */
    ConstructedTextures constructTextures(const std::vector<Texture>& textures);

    /**
    * Constructs a set of framebuffers from a list of Resources, mapping their internal IDs to bgfx::FrameBufferHandle.
    * @param resources Contains all of the rendering resources including the frame buffers to be constructed.
    * @return Returns a ConstructedFramebuffers object that contains all of the bgfx resources for our frame buffers.
    */
    ConstructedFramebuffers constructFramebuffers(const Resources& resources);

    /**
    * Constructs a map of uniform handles for use in shaders, mapping uniform names to their corresponding bgfx::UniformHandle.
    * @param uniforms A vector of Uniform objects, each containing a uniform name and possibly other metadata.
    * @return Returns an std::unordered_map<std::string, bgfx::UniformHandle> mapping uniform names to their handles.
    */
    std::unordered_map<std::string, bgfx::UniformHandle> constructUniforms(const std::vector<Uniform>& uniforms);

    /**
    * Constructs a map of shader handles for use in rendering, mapping shader id's to their corresponding bgfx::ShaderHandle.
    * @param shaders An std::vector<projv::Shader> containing all of our shaders information.
    * @return Returns an std::unordered_map<uint, bgfx::ShaderHandle> mapping shader names to their handles.
    */
    std::unordered_map<uint, bgfx::ShaderHandle> constructShaders(const std::vector<Shader>& shaders);

    /**
    * Constructs a list of render passes for the dependency graph, mapping each pass to its dependencies and outputs.
    * @param constructedResources A collection of all resources (textures, framebuffers, etc.) that have been constructed.
    * @param frameBuffers A list of framebuffer objects used in the render passes.
    * @param renderPasses A list of RenderPass objects, each using a shader to perform rendering logic.
    * @return Returns a vector of projv::BGFXDependencyGraph objects that represent the rendering dependencies and execution order.
    */
    std::vector<BGFXDependencyGraph> constructRenderPasses(const BGFXResources& constructedResources, const std::vector<FrameBuffer>& frameBuffers, const std::vector<RenderPass>& renderPasses);

    /**
    * Constructs a renderer specification object that holds all the necessary information for rendering and resources for rendering.
    * @param renderer A RendererSpecification object that defines the desired rendering configuration (e.g., render passes, viewport settings).
    * @param vertexShader A bgfx::ShaderHandle for the vertex shader to be used in this renderer.
    * @return Returns an std::shared_ptr<ConstructedRenderer> that encapsulates the constructed rendering configuration.
    */
    std::shared_ptr<ConstructedRenderer> constructRendererSpecification(RendererSpecification &renderer, bgfx::ShaderHandle vertexShader);

    /**
    * Resizes framebuffers and their associated textures if the window dimensions have changed.
    * This ensures that all render targets are correctly sized for the new resolution.
    * @param textures A reference to a ConstructedTextures object containing all texture handles.
    * @param frameBuffers A reference to a ConstructedFramebuffers object containing all framebuffer handles.
    * @param windowWidth The new width of the rendering window.
    * @param windowHeight The new height of the rendering window.
    * @param prevWindowWidth The previous width of the rendering window (for comparison).
    * @param prevWindowHeight The previous height of the rendering window (for comparison).
    */
    void resizeFramebuffersAndTheirTexturesIfNeeded(ConstructedTextures& textures, ConstructedFramebuffers& frameBuffers, int windowWidth, int windowHeight, int prevWindowWidth, int prevWindowHeight);
}

#endif
