#include "graphics/render.h"

namespace projv::graphics {
    void updateUniforms(ConstructedRenderer& constructedRenderer) {
        for(auto& uniform : constructedRenderer.resources.uniformHandles) {
            std::string name = uniform.first;
            std::cout << "Name: " << name << std::endl;
            bgfx::setUniform(uniform.second, constructedRenderer.resources.uniformValues.at(name).data());
        }
    }

    void resizeFramebuffersAndTheirTextures(ConstructedRenderer& constructedRenderer, int windowWidth, int windowHeight, int prevWindowWidth, int prevWindowHeight) {
        bgfx::reset(windowWidth, windowHeight, BGFX_RESET_NONE, bgfx::TextureFormat::Count);
        if (true && (prevWindowWidth != windowWidth || prevWindowHeight != windowHeight)) {
            for (size_t i = 0; i < constructedRenderer.resources.textures.texturesResizedWithWindow.size(); i++) {
                uint textureID = constructedRenderer.resources.textures.texturesResizedWithWindow[i];
                bgfx::TextureHandle handle = constructedRenderer.resources.textures.textureHandles.at(textureID);
                if (bgfx::isValid(handle)) {
                    bgfx::destroy(handle);
                }
                bgfx::TextureFormat::Enum textureFormat = constructedRenderer.resources.textures.textureFormats[textureID];
                constructedRenderer.resources.textures.textureHandles[textureID] = bgfx::createTexture2D(windowWidth, windowHeight, false, 1,textureFormat, BGFX_TEXTURE_RT);
            }

            for (auto &frameBuffer : constructedRenderer.resources.framebuffers.frameBufferTextureMapping) {
                int frameBufferID = frameBuffer.first; //std::pair<int, std::vector<uint>> first = frameBufferID, second = vector of textureIDs
                std::vector<bgfx::Attachment> attachments = getTextureAttachments(constructedRenderer.resources.textures.textureHandles, frameBuffer.second);
                constructedRenderer.resources.framebuffers.frameBufferHandles[frameBufferID] = bgfx::createFrameBuffer(uint16_t(frameBuffer.second.size()), attachments.data(), true); // Bindings in GLSL are determined by the textureID order.
                constructedRenderer.resources.framebuffers.frameBufferTextureMapping[frameBufferID] = frameBuffer.second;
            }
        }
    }

    void performRenderPasses(ConstructedRenderer& constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, void *viewMat, void *projMat, GPUData* gpuData) {
        for (size_t i = 0; i < constructedRenderer.dependencyGraph.size(); i++) {
            BGFXDependencyGraph &renderPass = constructedRenderer.dependencyGraph[i];
            std::cout << "RenderPassID: " << renderPass.renderPassID << std::endl;
            bgfx::setViewTransform(renderPass.renderPassID, viewMat, projMat);
            bgfx::setViewRect(renderPass.renderPassID, 0, 0, windowWidth, windowHeight);
            std::cout << "Check1" << std::endl;
            bgfx::setViewFrameBuffer(renderPass.renderPassID, constructedRenderer.resources.framebuffers.frameBufferHandles[renderPass.targetFrameBufferID]);
            bgfx::setViewClear(renderPass.renderPassID, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000FF, 1.0f, 0);
            bgfx::setVertexBuffer(0, renderInstance.vertexBuffer);
            bgfx::setIndexBuffer(renderInstance.indexBuffer);
            std::cout << "Check2" << std::endl;
            // bgfx::setTexture(0, u_texColor, colorTexture);
            for (size_t j = 0; j < renderPass.depdendencies.size(); j++) {
                std::cout << "Sampling from texture index:" << j << std::endl;
                bgfx::setTexture(j, renderPass.depdendencies[j].first, constructedRenderer.resources.textures.textureHandles.at(renderPass.depdendencies[j].second));
            }

            bgfx::setTexture(13, gpuData->octreeSampler, gpuData->octreeTexture);
            bgfx::setTexture(14, gpuData->voxelTypeDataSampler, gpuData->voxelTypeDataTexture);
            bgfx::setTexture(15, gpuData->headerSampler, gpuData->headerTexture);

            std::cout << "Check3" << std::endl;
            bgfx::submit(renderPass.renderPassID, renderPass.shaderProgram);
        }
    }

    void renderIt(RenderInstance &renderInstance, ConstructedRenderer &constructedRenderer, void *viewMat, void *projMat, GPUData* gpuData) {
        static int windowWidth = 0;
        static int windowHeight = 0;
        static int prevWindowWidth = 0;
        static int prevWindowHeight = 0;
        glfwGetWindowSize(renderInstance.window, &windowWidth, &windowHeight);

        updateUniforms(constructedRenderer);
        resizeFramebuffersAndTheirTextures(constructedRenderer, windowWidth, windowHeight, prevWindowWidth, prevWindowHeight);
        performRenderPasses(constructedRenderer, renderInstance, windowWidth, windowHeight, viewMat, projMat, gpuData);

        prevWindowWidth = windowWidth;
        prevWindowHeight = windowHeight;

        bgfx::frame();
    }
}
