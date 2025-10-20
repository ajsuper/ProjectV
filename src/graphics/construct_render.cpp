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

    std::vector<bgfx::Attachment> getTextureAttachments(ConstructedRenderer &constructedRenderer, std::vector<uint> textureIDs) {
        std::vector<bgfx::TextureHandle> textures;
        for (size_t i = 0; i < textureIDs.size(); i++) {
            textures.emplace_back(constructedRenderer.resources.textureHandles.at(textureIDs[i]));
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

    std::vector<std::pair<bgfx::UniformHandle, uint>> getDependenciesList(RendererSpecification &rendererSpecification, ConstructedRenderer &constructedRenderer, RenderPass &renderPass) {
        std::cout << "Getting dependencies!" << std::endl;
        std::vector<std::pair<bgfx::UniformHandle, uint>> dependencies;

        if (renderPass.frameBufferInputIDs.size() == 0) {
            return dependencies;
        }

        uint frameBufferInputID = renderPass.frameBufferInputIDs[0];
        std::vector<uint> textureIDs;
        for (size_t i = 0; i < rendererSpecification.resources.FrameBuffers.size(); i++) {
            if (rendererSpecification.resources.FrameBuffers[i].frameBufferID == frameBufferInputID) {
                textureIDs = rendererSpecification.resources.FrameBuffers[i].TextureIDs;
            }
        }

        for (size_t i = 0; i < renderPass.textureResourceIDs.size(); i++) {
            textureIDs.emplace_back(renderPass.textureResourceIDs[i]);
        }

        for (size_t i = 0; i < textureIDs.size(); i++) {
            dependencies.emplace_back(constructedRenderer.resources.textureSamplerHandles[textureIDs[i]], textureIDs[i]);
        }

        std::cout << "Acquired Dependencies!!" << std::endl;
        return dependencies;
    }

    bgfx::ProgramHandle createShaderProgram(bgfx::ShaderHandle vertexShader, bgfx::ShaderHandle fragmentShaderHandle) {
        bgfx::ProgramHandle shaderProgram = bgfx::createProgram(vertexShader, fragmentShaderHandle, true);
        return shaderProgram;
    }

    void constructTextures(ConstructedRenderer& constructedRenderer, RendererSpecification& renderer, Resources& resources) {
        for (size_t i = 0; i < resources.textures.size(); i++) {
            Texture texture = resources.textures[i];
            uint resX = texture.resolutionX;
            uint resY = texture.resolutionY;
            if (resX <= 0 || resY <= 0) {
                resX = 1;
                resY = 1;
            }

            if (texture.resizable == true && texture.origin == TextureOrigin::CreateNew) {
                constructedRenderer.resources.texturesResizedWithWindow.push_back(texture.textureID);
            }
            if (texture.resizable == true && texture.origin == TextureOrigin::CPUBuffer) {
                constructedRenderer.resources.texturesResizedWithResourceTextures.push_back(texture.textureID);
            }

            uint16_t textureWidth = uint16_t(std::max(1, texture.resolutionX)); 
            uint16_t textureHeight = uint16_t(std::max(1, texture.resolutionY));
            constructedRenderer.resources.textureHandles[texture.textureID] = bgfx::createTexture2D(textureWidth, textureHeight, false, 1, texture.format, BGFX_TEXTURE_RT);
            constructedRenderer.resources.textureSamplerHandles[texture.textureID] = bgfx::createUniform(texture.name.c_str(), bgfx::UniformType::Sampler);
            constructedRenderer.resources.textureFormats[texture.textureID] = texture.format;
            constructedRenderer.resources.textureResolutions[texture.textureID] = projv::core::ivec2(texture.resolutionX, texture.resolutionY);
        }
    }

    void constructFramebuffers(ConstructedRenderer& constructedRenderer, RendererSpecification& renderer, Resources& resources) {
        for (size_t i = 0; i < resources.FrameBuffers.size(); i++) {
            FrameBuffer frameBuffer = resources.FrameBuffers[i];
            std::vector<bgfx::Attachment> attachments = getTextureAttachments(constructedRenderer, frameBuffer.TextureIDs);
            constructedRenderer.resources.frameBufferHandles[frameBuffer.frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.TextureIDs.size()), attachments.data(), true); //Bindings in GLSL are determined by the texture order.
            constructedRenderer.resources.frameBufferTextureMapping[frameBuffer.frameBufferID] = frameBuffer.TextureIDs;
        }
        constructedRenderer.resources.frameBufferHandles[-1] = BGFX_INVALID_HANDLE;
    }

    void constructUniforms(ConstructedRenderer& constructedRenderer, RendererSpecification& renderer, Resources& resources) {
        for (size_t i = 0; i < resources.uniforms.size(); i++) {
            Uniform uniform = resources.uniforms[i];
            constructedRenderer.resources.uniformHandles[uniform.name] = bgfx::createUniform(uniform.name.c_str(), mapUniformType(uniform.type));
        }
    }

    void constructShaders(ConstructedRenderer& constructedRenderer, RendererSpecification& renderer, Resources& resources) {
        for (size_t i = 0; i < resources.shaders.size(); i++) {
            Shader shader = resources.shaders[i];
            constructedRenderer.resources.shaderHandles[shader.shaderID] = loadShader(shader.filePath);
        }
    }

    void constructRenderPasses(ConstructedRenderer& constructedRenderer, RendererSpecification& renderer, std::vector<RenderPass>& renderPasses) {
        for (size_t i = 0; i < renderPasses.size(); i++) {
            RenderPass &renderPass = renderPasses[i];
            if (constructedRenderer.resources
                    .frameBufferHandles[renderPass.frameBufferOutputID]
                    .idx != bgfx::kInvalidHandle) {
            //std::cout << "XCV Notcorrect!!" << std::endl;
            }

            BGFXDependencyGraph dependencyGraph;
            dependencyGraph.depdendencies = getDependenciesList(renderer, constructedRenderer, renderPass);
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

        constructTextures(constructedRenderer, renderer, resources);
        constructFramebuffers(constructedRenderer, renderer, resources); // Failed here
        std::cout << "Did darn did it!" << std::endl;
        constructUniforms(constructedRenderer, renderer, resources);
        constructShaders(constructedRenderer, renderer, resources);
        constructRenderPasses(constructedRenderer, renderer, renderPasses);

        return constructedRenderer;
    }
}

