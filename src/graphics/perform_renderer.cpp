#include "graphics/perform_renderer.h"

namespace projv::graphics {
    void updateUniforms(const std::unordered_map<std::string, bgfx::UniformHandle>& uniformHandles, const std::unordered_map<std::string, std::vector<uint8_t>>& uniformValues) {
        for(auto& uniform : uniformHandles) {
            std::string name = uniform.first;
            core::info("Name: {}", name);
            bgfx::setUniform(uniform.second, uniformValues.at(name).data());
        }
    }

    void performRenderPasses(bool renderToPrimaryNotNeeded, std::shared_ptr<ConstructedRenderer> constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, core::mat4 viewMat, core::mat4 projMat, GPUData* gpuData) {
        for (size_t i = 0; i < constructedRenderer->dependencyGraph.size(); i++) {
            BGFXDependencyGraph &renderPass = constructedRenderer->dependencyGraph[i];
            core::info("RenderPassID: {}", renderPass.renderPassID);
            bgfx::setViewTransform(renderPass.renderPassID, glm::value_ptr(viewMat), glm::value_ptr(projMat));
            bgfx::setViewRect(renderPass.renderPassID, 0, 0, windowWidth, windowHeight);
            
            // If its not a ping pong buffer, render to primary. If it is, then it depends on when they were last swapped.
            if (!(constructedRenderer->resources.framebuffers.pingPongFBOs.at(renderPass.targetFrameBufferID))) {
                core::info("Rendering to primary FBO (default): {}", renderPass.targetFrameBufferID);
                bgfx::setViewFrameBuffer(renderPass.renderPassID, constructedRenderer->resources.framebuffers.frameBufferHandles[renderPass.targetFrameBufferID]);
            } else if (!constructedRenderer->resources.framebuffers.primaryWasLastRenderedToo.at(renderPass.targetFrameBufferID)) {
                core::info("Rendering to primary FBO: {}", renderPass.targetFrameBufferID);
                bgfx::setViewFrameBuffer(renderPass.renderPassID, constructedRenderer->resources.framebuffers.frameBufferHandles[renderPass.targetFrameBufferID]);
            } else {
                core::info("Rendering to alternate FBO: {}", renderPass.targetFrameBufferID);
                bgfx::setViewFrameBuffer(renderPass.renderPassID, constructedRenderer->resources.framebuffers.frameBufferHandlesAlternate[renderPass.targetFrameBufferID]);
            }

            //bgfx::setViewClear(renderPass.renderPassID, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000FF, 1.0f, 0);
            bgfx::setViewClear(renderPass.renderPassID, BGFX_CLEAR_NONE, 0xFF00FFFF, 1.0f, 0);
            bgfx::setVertexBuffer(0, renderInstance.vertexBuffer);
            bgfx::setIndexBuffer(renderInstance.indexBuffer);

            for (size_t j = 0; j < renderPass.depdendencies.size(); j++) {
                bgfx::UniformInfo info;
                uint textureID = renderPass.depdendencies.at(j).second;
                if (!(constructedRenderer->resources.textures.pingPongFlags.at(textureID))) {
                    core::info("Rendering from primary textureID (default): {}", textureID);
                    bgfx::UniformHandle textureUniformHandle = constructedRenderer->resources.textures.textureSamplerHandles.at(textureID);
                    bgfx::TextureHandle textureHandle = constructedRenderer->resources.textures.textureHandles.at(textureID);
                    bgfx::getUniformInfo(textureUniformHandle, info);
                    bgfx::setTexture(j, renderPass.depdendencies[j].first, textureHandle);
                } else if (!constructedRenderer->resources.framebuffers.primaryWasLastRenderedToo.at(constructedRenderer->resources.textures.textureIDToFrameBufferID.at(textureID))) {
                    core::info("Rendering from alternate textureID: {}", textureID);
                    bgfx::UniformHandle textureUniformHandleAlternate = constructedRenderer->resources.textures.textureSamplerHandlesAlternate.at(textureID);
                    bgfx::TextureHandle textureHandleAlternate = constructedRenderer->resources.textures.textureHandlesAlternate.at(textureID);
                    bgfx::getUniformInfo(textureUniformHandleAlternate, info);
                    bgfx::setTexture(j, renderPass.depdendencies[j].first, textureHandleAlternate);
                } else {
                    core::info("Rendering from primary textureID: {}", textureID);
                    bgfx::UniformHandle textureUniformHandle = constructedRenderer->resources.textures.textureSamplerHandles.at(textureID);
                    bgfx::TextureHandle textureHandle = constructedRenderer->resources.textures.textureHandles.at(textureID);
                    bgfx::getUniformInfo(textureUniformHandle, info);
                    bgfx::setTexture(j, renderPass.depdendencies[j].first, textureHandle);
                }

                core::info("Sampling from texture name: {}", info.name);
                core::info("That texture is bound to: {}", j);
            }

            if (constructedRenderer->resources.framebuffers.pingPongFBOs.at(renderPass.targetFrameBufferID)) {
                constructedRenderer->resources.framebuffers.primaryWasLastRenderedToo[renderPass.targetFrameBufferID] = !constructedRenderer->resources.framebuffers.primaryWasLastRenderedToo[renderPass.targetFrameBufferID];
            }

            bgfx::setTexture(13, gpuData->octreeSampler, gpuData->octreeTexture);
            bgfx::setTexture(14, gpuData->voxelTypeDataSampler, gpuData->voxelTypeDataTexture);
            bgfx::setTexture(15, gpuData->headerSampler, gpuData->headerTexture);

            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);  
            bgfx::submit(renderPass.renderPassID, renderPass.shaderProgram);
        }
    }

    void renderConstructedRenderer(RenderInstance &renderInstance, std::shared_ptr<ConstructedRenderer> constructedRenderer, GPUData* gpuData) {
        static int windowWidth = 0;
        static int windowHeight = 0;
        static int prevWindowWidth = 0;
        static int prevWindowHeight = 0;
        //static bool renderToPrimary = true;

        glfwPollEvents();
        glfwGetWindowSize(renderInstance.window, &windowWidth, &windowHeight);

        projv::core::mat4 view = core::mat4(1.0f);
        projv::core::mat4 proj = core::mat4(1.0f);

        updateUniforms(constructedRenderer->resources.uniformHandles, constructedRenderer->resources.uniformValues);
        resizeFramebuffersAndTheirTexturesIfNeeded(constructedRenderer->resources.textures, constructedRenderer->resources.framebuffers, windowWidth, windowHeight, prevWindowWidth, prevWindowHeight);
        performRenderPasses(true, constructedRenderer, renderInstance, windowWidth, windowHeight, view, proj, gpuData);

        //renderToPrimary = !renderToPrimary;

        prevWindowWidth = windowWidth;
        prevWindowHeight = windowHeight;

        bgfx::frame();
    }
}
