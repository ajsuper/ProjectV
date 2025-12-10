#include "graphics/perform_renderer.h"

namespace projv::graphics {
    void updateUniforms(const std::unordered_map<std::string, bgfx::UniformHandle>& uniformHandles, const std::unordered_map<std::string, std::vector<uint8_t>>& uniformValues) {
        for(auto& uniform : uniformHandles) {
            std::string name = uniform.first;
            std::cout << "Name: " << name << std::endl;
            bgfx::setUniform(uniform.second, uniformValues.at(name).data());
        }
    }

    void performRenderPasses(std::shared_ptr<ConstructedRenderer> constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, core::mat4 viewMat, core::mat4 projMat, GPUData* gpuData) {
        for (size_t i = 0; i < constructedRenderer->dependencyGraph.size(); i++) {
            BGFXDependencyGraph &renderPass = constructedRenderer->dependencyGraph[i];
            std::cout << "RenderPassID: " << renderPass.renderPassID << std::endl;
            bgfx::setViewTransform(renderPass.renderPassID, glm::value_ptr(viewMat), glm::value_ptr(projMat));
            bgfx::setViewRect(renderPass.renderPassID, 0, 0, windowWidth, windowHeight);
            bgfx::setViewFrameBuffer(renderPass.renderPassID, constructedRenderer->resources.framebuffers.frameBufferHandles[renderPass.targetFrameBufferID]);
            bgfx::setViewClear(renderPass.renderPassID, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000FF, 1.0f, 0);
            bgfx::setVertexBuffer(0, renderInstance.vertexBuffer);
            bgfx::setIndexBuffer(renderInstance.indexBuffer);

            for (size_t j = 0; j < renderPass.depdendencies.size(); j++) {
                bgfx::UniformInfo info;
                bgfx::getUniformInfo(renderPass.depdendencies[j].first, info);
                std::cout << "Sampling from texture name: " << info.name << std::endl;
                std::cout << "That texture is bound to: " << j << std::endl;
                bgfx::setTexture(j, renderPass.depdendencies[j].first, constructedRenderer->resources.textures.textureHandles.at(renderPass.depdendencies[j].second));
            }

            bgfx::setTexture(13, gpuData->octreeSampler, gpuData->octreeTexture);
            bgfx::setTexture(14, gpuData->voxelTypeDataSampler, gpuData->voxelTypeDataTexture);
            bgfx::setTexture(15, gpuData->headerSampler, gpuData->headerTexture);

            bgfx::submit(renderPass.renderPassID, renderPass.shaderProgram);
        }
    }

    void renderConstructedRenderer(RenderInstance &renderInstance, std::shared_ptr<ConstructedRenderer> constructedRenderer, GPUData* gpuData) {
        static int windowWidth = 0;
        static int windowHeight = 0;
        static int prevWindowWidth = 0;
        static int prevWindowHeight = 0;

        glfwPollEvents();
        glfwGetWindowSize(renderInstance.window, &windowWidth, &windowHeight);

        projv::core::mat4 view = core::mat4(1.0f);
        projv::core::mat4 proj = core::mat4(1.0f);

        updateUniforms(constructedRenderer->resources.uniformHandles, constructedRenderer->resources.uniformValues);
        resizeFramebuffersAndTheirTexturesIfNeeded(constructedRenderer->resources.textures, constructedRenderer->resources.framebuffers, windowWidth, windowHeight, prevWindowWidth, prevWindowHeight);
        performRenderPasses(constructedRenderer, renderInstance, windowWidth, windowHeight, view, proj, gpuData);

        prevWindowWidth = windowWidth;
        prevWindowHeight = windowHeight;

        bgfx::frame();
    }
}
