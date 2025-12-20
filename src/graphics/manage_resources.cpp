#include "graphics/manage_resources.h"
#include "data_structures/texture.h"
#include <bgfx/bgfx.h>

namespace projv::graphics {
    std::vector<bgfx::Attachment> getTextureAttachments(const std::unordered_map<uint, bgfx::TextureHandle>& textureHandles, std::vector<uint> textureIDs) {
        std::vector<bgfx::TextureHandle> textures;
        for (size_t i = 0; i < textureIDs.size(); i++) {
            textures.emplace_back(textureHandles.at(textureIDs[i]));
        }

        std::vector<bgfx::Attachment> attachments;
        for (auto texture : textures) {
            bgfx::Attachment attachment;
            attachment.init(texture);
            attachments.emplace_back(attachment);
        }

        return attachments;
    }

    bgfx::ProgramHandle createShaderProgram(const bgfx::ShaderHandle& vertexShader, const bgfx::ShaderHandle& fragmentShaderHandle) {
        bgfx::ProgramHandle shaderProgram = bgfx::createProgram(vertexShader, fragmentShaderHandle, true);
        return shaderProgram;
    }

    ConstructedTextures constructTextures(const std::vector<Texture>& textures) {
        ConstructedTextures constructedTextures;
        for (size_t i = 0; i < textures.size(); i++) {
            Texture texture = textures[i];
            uint resX = texture.resolutionX;
            uint resY = texture.resolutionY;
            if (resX <= 0 || resY <= 0) {
                resX = 1;
                resY = 1;
            }

            if (texture.resizable == true && texture.origin == TextureOrigin::CreateNew) {
                constructedTextures.texturesResizedWithWindow[texture.textureID] = true;
            }
            if (texture.resizable == true && texture.origin == TextureOrigin::CPUBuffer) {
                constructedTextures.texturesResizedWithResourceTextures[texture.textureID] = true;
            }

            uint16_t textureWidth = uint16_t(std::max(1, texture.resolutionX)); 
            uint16_t textureHeight = uint16_t(std::max(1, texture.resolutionY));
            constructedTextures.textureHandles[texture.textureID] = bgfx::createTexture2D(textureWidth, textureHeight, false, 1, texture.format, BGFX_TEXTURE_RT);
            constructedTextures.textureSamplerHandles[texture.textureID] = bgfx::createUniform(texture.name.c_str(), bgfx::UniformType::Sampler);
            constructedTextures.textureHandlesAlternate[texture.textureID] = BGFX_INVALID_HANDLE;
            constructedTextures.textureSamplerHandlesAlternate[texture.textureID] = BGFX_INVALID_HANDLE;
            constructedTextures.pingPongFlags[texture.textureID] = false;
            if (texture.pingPongFlag == true) {
                constructedTextures.textureHandlesAlternate[texture.textureID] = bgfx::createTexture2D(textureWidth, textureHeight, false, 1, texture.format, BGFX_TEXTURE_RT);
                constructedTextures.textureSamplerHandlesAlternate[texture.textureID] = bgfx::createUniform(texture.name.c_str(), bgfx::UniformType::Sampler);
                constructedTextures.pingPongFlags[texture.textureID] = true;
            }
            constructedTextures.textureFormats[texture.textureID] = texture.format;
            constructedTextures.textureResolutions[texture.textureID] = projv::core::ivec2(texture.resolutionX, texture.resolutionY);
        }

        return constructedTextures;
    }
    
    ConstructedFramebuffers constructFramebuffers(const std::vector<FrameBuffer>& frameBuffers, const ConstructedTextures& constructedTextures) {
        ConstructedFramebuffers constructedFramebuffers;
        for (size_t i = 0; i < frameBuffers.size(); i++) {
            FrameBuffer frameBuffer = frameBuffers[i];
            std::vector<bgfx::Attachment> attachments = getTextureAttachments(constructedTextures.textureHandles, frameBuffer.TextureIDs);
            constructedFramebuffers.frameBufferHandles[frameBuffer.frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.TextureIDs.size()), attachments.data(), false); //Bindings in GLSL are determined by the texture order.
            constructedFramebuffers.frameBufferTextureMapping[frameBuffer.frameBufferID] = frameBuffer.TextureIDs;
            constructedFramebuffers.pingPongFBOs[frameBuffer.frameBufferID] = false;
            constructedFramebuffers.frameBufferTextureMapping[frameBuffer.frameBufferID] = frameBuffer.TextureIDs;
            std::cout << "Created frame buffer" << std::endl;
            constructedFramebuffers.frameBufferHandlesAlternate[frameBuffer.frameBufferID] = BGFX_INVALID_HANDLE;
            constructedFramebuffers.primaryWasLastRenderedToo[frameBuffer.frameBufferID] = true;
            if (frameBuffer.pingPongFBO == true) {
                std::vector<bgfx::Attachment> attachmentsAlternate = getTextureAttachments(constructedTextures.textureHandlesAlternate, frameBuffer.TextureIDs);
                std::cout << "Got alternate texture attachments" << std::endl;
                constructedFramebuffers.frameBufferHandlesAlternate[frameBuffer.frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.TextureIDs.size()), attachmentsAlternate.data(), false); //Bindings in GLSL are determined by the texture order.
                constructedFramebuffers.frameBufferTextureMappingAlternate[frameBuffer.frameBufferID] = frameBuffer.TextureIDs;               
                constructedFramebuffers.pingPongFBOs[frameBuffer.frameBufferID] = true;
                std::cout << "Created alternate frame buffer" << std::endl;
            }
        }
        constructedFramebuffers.frameBufferHandles[-1] = BGFX_INVALID_HANDLE;
        constructedFramebuffers.pingPongFBOs[-1] = false;
        return constructedFramebuffers;
    }

    ConstructedTextures assignTexturesToTheirFramebuffers(const ConstructedFramebuffers& frameBuffers, const ConstructedTextures& textures) {
        ConstructedTextures texturesNew = textures;
        for (auto& frameBuffer : frameBuffers.frameBufferTextureMapping) {
            for (size_t i = 0; i < frameBuffer.second.size(); i++) {
                uint textureID = frameBuffer.second[i];
                texturesNew.textureIDToFrameBufferID[textureID] = frameBuffer.first;
            }
        }
        return texturesNew;
    }

    std::unordered_map<std::string, bgfx::UniformHandle> constructUniforms(const std::vector<Uniform>& uniforms) {
        std::unordered_map<std::string, bgfx::UniformHandle> constructedUniformHandles;
        for (size_t i = 0; i < uniforms.size(); i++) {
            Uniform uniform = uniforms[i];
            constructedUniformHandles[uniform.name] = bgfx::createUniform(uniform.name.c_str(), mapUniformType(uniform.type));
        }
        return constructedUniformHandles;
    }

    std::unordered_map<uint, bgfx::ShaderHandle> constructShaders(const std::vector<Shader>& shaders) {
        std::unordered_map<uint, bgfx::ShaderHandle> constructedShaderHandles;
        for (size_t i = 0; i < shaders.size(); i++) {
            Shader shader = shaders[i];
            constructedShaderHandles[shader.shaderID] = loadShader(shader.filePath);
        }
        return constructedShaderHandles;
    }

    std::vector<BGFXDependencyGraph> constructRenderPasses(const BGFXResources& constructedResources, const std::vector<FrameBuffer>& frameBuffers, const std::vector<RenderPass>& renderPasses) {
        std::vector<BGFXDependencyGraph> dependencyGraphs;
        for (size_t i = 0; i < renderPasses.size(); i++) {
            const RenderPass &renderPass = renderPasses[i];
            if (constructedResources.framebuffers
                    .frameBufferHandles.at(renderPass.frameBufferOutputID)
                    .idx != bgfx::kInvalidHandle) {
            }

            BGFXDependencyGraph dependencyGraph;
            dependencyGraph.depdendencies = getDependenciesList(frameBuffers, constructedResources.textures.textureSamplerHandles, renderPass); // Should be removed. We should only store the ID's.
            dependencyGraph.windowWidth = 1;
            dependencyGraph.windowHeight = 1;
            dependencyGraph.targetFrameBufferID = renderPass.frameBufferOutputID;
            dependencyGraph.shaderProgram = createShaderProgram(constructedResources.defaultVertexShader, constructedResources.shaderHandles.at(renderPass.shaderID));
            dependencyGraph.renderPassID = uint(i);

            dependencyGraphs.push_back(dependencyGraph);
        }
        return dependencyGraphs;
    }

    std::shared_ptr<ConstructedRenderer> constructRendererSpecification(RendererSpecification &renderer, bgfx::ShaderHandle vertexShader) {
        ConstructedRenderer constructedRenderer;
        constructedRenderer.resources.defaultVertexShader = vertexShader;
        const Resources &resources = renderer.resources;
        const std::vector<RenderPass> &renderPasses = renderer.dependencyGraph.renderPasses;

        constructedRenderer.resources.textures = constructTextures(resources.textures);
        std::cout << "Constructed textures!" << std::endl;
        constructedRenderer.resources.framebuffers = constructFramebuffers(resources.FrameBuffers, constructedRenderer.resources.textures);
        std::cout << "Constructed frame buffers!" << std::endl;
        constructedRenderer.resources.uniformHandles = constructUniforms(resources.uniforms);
        std::cout << "Constructed uniform handles!" << std::endl;
        constructedRenderer.resources.shaderHandles = constructShaders(resources.shaders);
        std::cout << "Constructed shaders!" << std::endl;
        constructedRenderer.dependencyGraph = constructRenderPasses(constructedRenderer.resources, resources.FrameBuffers, renderPasses);
        std::cout << "Constructed render passes!" << std::endl;
        constructedRenderer.resources.textures = assignTexturesToTheirFramebuffers(constructedRenderer.resources.framebuffers, constructedRenderer.resources.textures);

        return std::make_shared<ConstructedRenderer>(constructedRenderer);
    }

    void resizeFramebuffersAndTheirTexturesIfNeeded(ConstructedTextures& textures, ConstructedFramebuffers& frameBuffers, int windowWidth, int windowHeight, int prevWindowWidth, int prevWindowHeight) {
        if (true && (prevWindowWidth != windowWidth || prevWindowHeight != windowHeight)) {
            bgfx::reset(windowWidth, windowHeight, BGFX_RESET_NONE, bgfx::TextureFormat::Count);

            for (auto textureID : textures.texturesResizedWithWindow) {
                bgfx::TextureHandle handle = textures.textureHandles.at(textureID.first);
                if (bgfx::isValid(handle)) {
                    bgfx::destroy(handle);
                }
                bgfx::TextureFormat::Enum textureFormat = textures.textureFormats[textureID.first];
                textures.textureHandles.at(textureID.first) = bgfx::createTexture2D(windowWidth, windowHeight, false, 1,textureFormat, BGFX_TEXTURE_RT);
            }

            for (auto &frameBuffer : frameBuffers.frameBufferTextureMapping) {
                int frameBufferID = frameBuffer.first; //std::pair<int, std::vector<uint>> first = frameBufferID, second = vector of textureIDs
                std::vector<bgfx::Attachment> attachments = getTextureAttachments(textures.textureHandles, frameBuffer.second);
                frameBuffers.frameBufferHandles[frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.second.size()), attachments.data(), true); // Bindings in GLSL are determined by the textureID order.
                frameBuffers.frameBufferTextureMapping[frameBufferID] = frameBuffer.second;
            }

            // Handle alternate FBO's and textures.
            for (auto textureID : textures.texturesResizedWithWindow) {
                if (textures.pingPongFlags.at(textureID.first) == false) continue;
                bgfx::TextureHandle handle = textures.textureHandlesAlternate.at(textureID.first);
                if (bgfx::isValid(handle)) {
                    bgfx::destroy(handle);
                }
                bgfx::TextureFormat::Enum textureFormat = textures.textureFormats[textureID.first];
                textures.textureHandlesAlternate.at(textureID.first) = bgfx::createTexture2D(windowWidth, windowHeight, false, 1,textureFormat, BGFX_TEXTURE_RT);
            }

            for (auto &frameBuffer : frameBuffers.frameBufferTextureMapping) {
                if (frameBuffers.pingPongFBOs.at(frameBuffer.first) == false) continue;
                int frameBufferID = frameBuffer.first; //std::pair<int, std::vector<uint>> first = frameBufferID, second = vector of textureIDs
                std::vector<bgfx::Attachment> attachments = getTextureAttachments(textures.textureHandlesAlternate, frameBuffer.second);
                frameBuffers.frameBufferHandlesAlternate[frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.second.size()), attachments.data(), true); // Bindings in GLSL are determined by the textureID order.
                frameBuffers.frameBufferTextureMapping[frameBufferID] = frameBuffer.second;
            }
        }
    }
}
