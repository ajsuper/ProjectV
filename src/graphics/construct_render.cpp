#include "graphics/construct_render.h"

namespace projv::graphics {
    bgfx::ShaderHandle loadShader(const std::string &path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            fprintf(stderr, "Failed to open shader: %s\n", path.c_str());
            return BGFX_INVALID_HANDLE;
        }

        std::streamsize fileSize = file.tellg();
        if (fileSize <= 0) {
            fprintf(stderr, "Shader file is empty or error reading size: %s\n", path.c_str());
            return BGFX_INVALID_HANDLE;
        }
        file.seekg(0, std::ios::beg);

        std::vector<char> loadedBuffer(fileSize);
        if (!file.read(loadedBuffer.data(), fileSize)) {
            fprintf(stderr, "Failed to read shader: %s\n", path.c_str());
            return BGFX_INVALID_HANDLE;
        }

        const bgfx::Memory *fileMemory = bgfx::copy(loadedBuffer.data(), static_cast<uint32_t>(fileSize));
        return bgfx::createShader(fileMemory);
    }

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

    bgfx::UniformType::Enum mapUniformType(UniformType uniformType) {
        bgfx::UniformType bgfxUniformType;
        const std::unordered_map<UniformType, bgfx::UniformType::Enum> uniformMap = {
            {UniformType::Vec4, bgfx::UniformType::Vec4},
            {UniformType::Mat3x3, bgfx::UniformType::Mat3},
            {UniformType::Mat4x4, bgfx::UniformType::Mat4}
        };
        return uniformMap.at(uniformType);
    };

    std::vector<std::pair<bgfx::UniformHandle, uint>> getDependenciesList(const std::vector<FrameBuffer>& frameBuffers, const std::unordered_map<uint, bgfx::UniformHandle>& textureSamplerHandles, RenderPass &renderPass) {
        std::cout << "Getting dependencies!" << std::endl;
        std::vector<std::pair<bgfx::UniformHandle, uint>> dependencies;

        if (renderPass.frameBufferInputIDs.size() == 0) {
            return dependencies;
        }

        uint frameBufferInputID = renderPass.frameBufferInputIDs[0];
        std::vector<uint> textureIDs;
        for (size_t i = 0; i < frameBuffers.size(); i++) {
            if (frameBuffers[i].frameBufferID == frameBufferInputID) {
                textureIDs = frameBuffers[i].TextureIDs;
            }
        }

        for (size_t i = 0; i < renderPass.textureResourceIDs.size(); i++) {
            textureIDs.emplace_back(renderPass.textureResourceIDs[i]);
        }

        for (size_t i = 0; i < textureIDs.size(); i++) {
            dependencies.emplace_back(textureSamplerHandles.at(textureIDs[i]), textureIDs[i]);
        }

        std::cout << "Acquired Dependencies!!" << std::endl;
        return dependencies;
    }

    bgfx::ProgramHandle createShaderProgram(bgfx::ShaderHandle vertexShader, bgfx::ShaderHandle fragmentShaderHandle) {
        bgfx::ProgramHandle shaderProgram = bgfx::createProgram(vertexShader, fragmentShaderHandle, true);
        return shaderProgram;
    }

    ConstructedTextures constructTextures(const Resources& resources) {
        ConstructedTextures constructedTextures;
        for (size_t i = 0; i < resources.textures.size(); i++) {
            Texture texture = resources.textures[i];
            uint resX = texture.resolutionX;
            uint resY = texture.resolutionY;
            if (resX <= 0 || resY <= 0) {
                resX = 1;
                resY = 1;
            }

            if (texture.resizable == true && texture.origin == TextureOrigin::CreateNew) {
                //constructedRenderer.resources.texturesResizedWithWindow.push_back(texture.textureID);
                constructedTextures.texturesResizedWithWindow[texture.textureID] = true;
            }
            if (texture.resizable == true && texture.origin == TextureOrigin::CPUBuffer) {
                constructedTextures.texturesResizedWithResourceTextures[texture.textureID] = true;
            }

            uint16_t textureWidth = uint16_t(std::max(1, texture.resolutionX)); 
            uint16_t textureHeight = uint16_t(std::max(1, texture.resolutionY));
            constructedTextures.textureHandles[texture.textureID] = bgfx::createTexture2D(textureWidth, textureHeight, false, 1, texture.format, BGFX_TEXTURE_RT);
            constructedTextures.textureSamplerHandles[texture.textureID] = bgfx::createUniform(texture.name.c_str(), bgfx::UniformType::Sampler);
            constructedTextures.textureFormats[texture.textureID] = texture.format;
            constructedTextures.textureResolutions[texture.textureID] = projv::core::ivec2(texture.resolutionX, texture.resolutionY);
        }

        return constructedTextures;
    }

    void constructFramebuffers(ConstructedRenderer& constructedRenderer, const Resources& resources) {
        for (size_t i = 0; i < resources.FrameBuffers.size(); i++) {
            FrameBuffer frameBuffer = resources.FrameBuffers[i];
            std::vector<bgfx::Attachment> attachments = getTextureAttachments(constructedRenderer.resources.textures.textureHandles, frameBuffer.TextureIDs);
            constructedRenderer.resources.frameBufferHandles[frameBuffer.frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.TextureIDs.size()), attachments.data(), true); //Bindings in GLSL are determined by the texture order.
            constructedRenderer.resources.frameBufferTextureMapping[frameBuffer.frameBufferID] = frameBuffer.TextureIDs;
        }
        constructedRenderer.resources.frameBufferHandles[-1] = BGFX_INVALID_HANDLE;
    }

    void constructUniforms(ConstructedRenderer& constructedRenderer, const Resources& resources) {
        for (size_t i = 0; i < resources.uniforms.size(); i++) {
            Uniform uniform = resources.uniforms[i];
            constructedRenderer.resources.uniformHandles[uniform.name] = bgfx::createUniform(uniform.name.c_str(), mapUniformType(uniform.type));
        }
    }

    void constructShaders(ConstructedRenderer& constructedRenderer, const Resources& resources) {
        for (size_t i = 0; i < resources.shaders.size(); i++) {
            Shader shader = resources.shaders[i];
            constructedRenderer.resources.shaderHandles[shader.shaderID] = loadShader(shader.filePath);
        }
    }

    void constructRenderPasses(ConstructedRenderer& constructedRenderer, const RendererSpecification& renderer, std::vector<RenderPass>& renderPasses) {
        for (size_t i = 0; i < renderPasses.size(); i++) {
            RenderPass &renderPass = renderPasses[i];
            if (constructedRenderer.resources
                    .frameBufferHandles[renderPass.frameBufferOutputID]
                    .idx != bgfx::kInvalidHandle) {
            //std::cout << "XCV Notcorrect!!" << std::endl;
            }

            BGFXDependencyGraph dependencyGraph;
            dependencyGraph.depdendencies = getDependenciesList(renderer.resources.FrameBuffers, constructedRenderer.resources.textures.textureSamplerHandles, renderPass);
            dependencyGraph.windowWidth = 1;
            dependencyGraph.windowHeight = 1;
            dependencyGraph.targetFrameBufferID = renderPass.frameBufferOutputID;
            dependencyGraph.shaderProgram = createShaderProgram(constructedRenderer.resources.defaultVertexShader, constructedRenderer.resources.shaderHandles[renderPass.shaderID]);
            dependencyGraph.renderPassID = uint(i);
            constructedRenderer.dependencyGraph.push_back(dependencyGraph);
        }
    }

    ConstructedRenderer constructRendererSpecification(RendererSpecification &renderer, bgfx::ShaderHandle vertexShader) {
        ConstructedRenderer constructedRenderer;
        constructedRenderer.resources.defaultVertexShader = vertexShader;
        Resources &resources = renderer.resources;
        std::vector<RenderPass> &renderPasses = renderer.dependencyGraph.renderPasses;

        constructedRenderer.resources.textures = constructTextures(resources);
        constructFramebuffers(constructedRenderer, resources); // Failed here
        std::cout << "Did darn did it!" << std::endl;
        constructUniforms(constructedRenderer, resources);
        constructShaders(constructedRenderer, resources);
        constructRenderPasses(constructedRenderer, renderer, renderPasses);

        return constructedRenderer;
    }

    void useConstructedRenderer(RenderInstance &renderInstance, ConstructedRenderer &constructedRenderer) {
        renderInstance.constructedRenderer = constructedRenderer;
    }

    void moveRendererSpecification(RenderInstance &renderInstance, RendererSpecification &rendererSpecification, uint rendererID) {
        renderInstance.renderers[rendererID] = std::move(rendererSpecification);
    }

    RendererSpecification &getRendererSpecification(RenderInstance &renderInstance, uint rendererID) {
        return renderInstance.renderers[rendererID];
    }
}

