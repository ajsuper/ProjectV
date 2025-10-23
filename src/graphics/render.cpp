#include "graphics/render.h"

namespace projv::graphics {
    void updateUniforms(const std::unordered_map<std::string, bgfx::UniformHandle>& uniformHandles, const std::unordered_map<std::string, std::vector<uint8_t>>& uniformValues) {
        for(auto& uniform : uniformHandles) {
            std::string name = uniform.first;
            std::cout << "Name: " << name << std::endl;
            bgfx::setUniform(uniform.second, uniformValues.at(name).data());
        }
    }

    void resizeFramebuffersAndTheirTexturesIfNeeded(ConstructedTextures& textures, ConstructedFramebuffers& frameBuffers, int windowWidth, int windowHeight, int prevWindowWidth, int prevWindowHeight) {
        bgfx::reset(windowWidth, windowHeight, BGFX_RESET_NONE, bgfx::TextureFormat::Count);
        if (true && (prevWindowWidth != windowWidth || prevWindowHeight != windowHeight)) {
            for (size_t i = 0; i < textures.texturesResizedWithWindow.size(); i++) {
                uint textureID = textures.texturesResizedWithWindow.at(i);
                bgfx::TextureHandle handle = textures.textureHandles.at(textureID);
                if (bgfx::isValid(handle)) {
                    bgfx::destroy(handle);
                }
                bgfx::TextureFormat::Enum textureFormat = textures.textureFormats[textureID];
                textures.textureHandles.at(textureID) = bgfx::createTexture2D(windowWidth, windowHeight, false, 1,textureFormat, BGFX_TEXTURE_RT);
            }

            for (auto &frameBuffer : frameBuffers.frameBufferTextureMapping) {
                int frameBufferID = frameBuffer.first; //std::pair<int, std::vector<uint>> first = frameBufferID, second = vector of textureIDs
                std::vector<bgfx::Attachment> attachments = getTextureAttachments(textures.textureHandles, frameBuffer.second);
                frameBuffers.frameBufferHandles[frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.second.size()), attachments.data(), true); // Bindings in GLSL are determined by the textureID order.
                frameBuffers.frameBufferTextureMapping[frameBufferID] = frameBuffer.second;
            }
        }
    }

    void performRenderPasses(std::shared_ptr<ConstructedRenderer> constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, void *viewMat, void *projMat, GPUData* gpuData) {
        for (size_t i = 0; i < constructedRenderer->dependencyGraph.size(); i++) {
            BGFXDependencyGraph &renderPass = constructedRenderer->dependencyGraph[i];
            std::cout << "RenderPassID: " << renderPass.renderPassID << std::endl;
            bgfx::setViewTransform(renderPass.renderPassID, viewMat, projMat);
            bgfx::setViewRect(renderPass.renderPassID, 0, 0, windowWidth, windowHeight);
            std::cout << "Check1" << std::endl;
            bgfx::setViewFrameBuffer(renderPass.renderPassID, constructedRenderer->resources.framebuffers.frameBufferHandles[renderPass.targetFrameBufferID]);
            bgfx::setViewClear(renderPass.renderPassID, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000FF, 1.0f, 0);
            bgfx::setVertexBuffer(0, renderInstance.vertexBuffer);
            bgfx::setIndexBuffer(renderInstance.indexBuffer);
            std::cout << "Check2" << std::endl;
            // bgfx::setTexture(0, u_texColor, colorTexture);
            for (size_t j = 0; j < renderPass.depdendencies.size(); j++) {
                std::cout << "Sampling from texture index:" << j << std::endl;
                bgfx::setTexture(j, renderPass.depdendencies[j].first, constructedRenderer->resources.textures.textureHandles.at(renderPass.depdendencies[j].second));
            }

            bgfx::setTexture(13, gpuData->octreeSampler, gpuData->octreeTexture);
            bgfx::setTexture(14, gpuData->voxelTypeDataSampler, gpuData->voxelTypeDataTexture);
            bgfx::setTexture(15, gpuData->headerSampler, gpuData->headerTexture);

            std::cout << "Check3" << std::endl;
            bgfx::submit(renderPass.renderPassID, renderPass.shaderProgram);
        }
    }

    void renderConstructedRenderer(RenderInstance &renderInstance, std::shared_ptr<ConstructedRenderer> constructedRenderer, void *viewMat, void *projMat, GPUData* gpuData) {
        static int windowWidth = 0;
        static int windowHeight = 0;
        static int prevWindowWidth = 0;
        static int prevWindowHeight = 0;
        glfwGetWindowSize(renderInstance.window, &windowWidth, &windowHeight);

        updateUniforms(constructedRenderer->resources.uniformHandles, constructedRenderer->resources.uniformValues);
        resizeFramebuffersAndTheirTexturesIfNeeded(constructedRenderer->resources.textures, constructedRenderer->resources.framebuffers, windowWidth, windowHeight, prevWindowWidth, prevWindowHeight);
        performRenderPasses(constructedRenderer, renderInstance, windowWidth, windowHeight, viewMat, projMat, gpuData);

        prevWindowWidth = windowWidth;
        prevWindowHeight = windowHeight;

        bgfx::frame();
    }
}
