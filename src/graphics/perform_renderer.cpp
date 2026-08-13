#include "graphics/perform_renderer.h"
#include <algorithm>
#include <cmath>

namespace projv::graphics {
    void updateUniforms(const std::unordered_map<std::string, bgfx::UniformHandle>& uniformHandles, const std::unordered_map<std::string, std::vector<uint8_t>>& uniformValues) {
#if defined(PROJV_ENABLE_RENDER)
        static bool uniformsLogged = false;
        if (!uniformsLogged) {
            for(auto& uniform : uniformHandles) {
                std::string name = uniform.first;
                core::render("Name: {}", name);
            }
            uniformsLogged = true;
        }
#endif
        for(auto& uniform : uniformHandles) {
            auto it = uniformValues.find(uniform.first);
            if (it == uniformValues.end()) {
                core::error("Missing uniform value: '{}'. Maybe you are passing a const reference to setUniformToValue?", uniform.first);
                continue;
            }
            bgfx::setUniform(uniform.second, it->second.data());
        }
    }

    void performRenderPasses(bool renderToPrimaryNotNeeded, std::shared_ptr<ConstructedRenderer> constructedRenderer, RenderInstance& renderInstance, int backBufferWidth, int backBufferHeight, core::mat4 viewMat, core::mat4 projMat, GPUData* gpuData) {
#if defined(PROJV_ENABLE_RENDER)
        static bool renderGraphLogged = false;
#endif
        const core::ivec2 backBufferSize(backBufferWidth, backBufferHeight);

        for (size_t i = 0; i < constructedRenderer->dependencyGraph.size(); i++) {
            BGFXDependencyGraph &renderPass = constructedRenderer->dependencyGraph[i];
#if defined(PROJV_ENABLE_RENDER)
            if (!renderGraphLogged) {
                core::render("RenderPassID: {}", renderPass.renderPassID);
            }
#endif
            // The rasterized area comes from this pass's OWN target, not from one size shared by every
            // pass in the frame. That shared size is what made a target's declared resolution
            // advisory: a pass writing a half-size framebuffer still rasterized a full-size rect into
            // it and got clipped, so no target could differ from any other. Passes are free to differ
            // now, and each one covers exactly the framebuffer it writes.
            const core::ivec2 targetSize = resolveTargetSize(constructedRenderer->resources.textures,
                                                             constructedRenderer->resources.framebuffers,
                                                             renderPass.targetFrameBufferID,
                                                             backBufferSize);
#if defined(PROJV_ENABLE_RENDER)
            if (!renderGraphLogged) {
                core::render("Target size: {}x{}", targetSize.x, targetSize.y);
            }
#endif
            bgfx::setViewTransform(renderPass.renderPassID, glm::value_ptr(viewMat), glm::value_ptr(projMat));
            bgfx::setViewRect(renderPass.renderPassID, 0, 0, uint16_t(targetSize.x), uint16_t(targetSize.y));

            // What the shader must measure its own pixels against. See BGFXResources::passTargetRes.
            const float passTargetRes[4] = {
                float(targetSize.x),
                float(targetSize.y),
                1.0f / float(std::max(targetSize.x, 1)),
                1.0f / float(std::max(targetSize.y, 1))
            };
            bgfx::setUniform(constructedRenderer->resources.passTargetRes, passTargetRes);

            // If its not a ping pong buffer, render to primary. If it is, then it depends on when they were last swapped.
            if (!(constructedRenderer->resources.framebuffers.pingPongFBOs.at(renderPass.targetFrameBufferID))) {
#if defined(PROJV_ENABLE_RENDER)
                if (!renderGraphLogged) {
                    core::render("Rendering to primary FBO (default): {}", renderPass.targetFrameBufferID);
                }
#endif
                bgfx::setViewFrameBuffer(renderPass.renderPassID, constructedRenderer->resources.framebuffers.frameBufferHandles[renderPass.targetFrameBufferID]);
            } else if (!constructedRenderer->resources.framebuffers.primaryWasLastRenderedToo.at(renderPass.targetFrameBufferID)) {
#if defined(PROJV_ENABLE_RENDER)
                if (!renderGraphLogged) {
                    core::render("Rendering to primary FBO: {}", renderPass.targetFrameBufferID);
                }
#endif
                bgfx::setViewFrameBuffer(renderPass.renderPassID, constructedRenderer->resources.framebuffers.frameBufferHandles[renderPass.targetFrameBufferID]);
            } else {
#if defined(PROJV_ENABLE_RENDER)
                if (!renderGraphLogged) {
                    core::render("Rendering to alternate FBO: {}", renderPass.targetFrameBufferID);
                }
#endif
                bgfx::setViewFrameBuffer(renderPass.renderPassID, constructedRenderer->resources.framebuffers.frameBufferHandlesAlternate[renderPass.targetFrameBufferID]);
            }

            //bgfx::setViewClear(renderPass.renderPassID, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x000000FF, 1.0f, 0);
            bgfx::setViewClear(renderPass.renderPassID, BGFX_CLEAR_NONE, 0xFF00FFFF, 1.0f, 0);
            bgfx::setVertexBuffer(0, renderInstance.vertexBuffer);
            bgfx::setIndexBuffer(renderInstance.indexBuffer);

#if defined(PROJV_ENABLE_RENDER)
            if (!renderGraphLogged) {
                core::render("Render pass # of dependencies: {}", renderPass.depdendencies.size());
            }
#endif
            // Sizes of this pass's bound inputs, indexed by sampler slot. See
            // BGFXResources::passInputRes. Filled in the binding loop below, where the slot is known,
            // and submitted once afterwards.
            float passInputRes[PROJV_MAX_PASS_INPUTS][4];
            for (int slot = 0; slot < PROJV_MAX_PASS_INPUTS; slot++) {
                passInputRes[slot][0] = 0.0f; passInputRes[slot][1] = 0.0f;
                passInputRes[slot][2] = 0.0f; passInputRes[slot][3] = 0.0f;
            }

            for (size_t j = 0; j < renderPass.depdendencies.size(); j++) {
                bgfx::UniformInfo info;
                uint textureID = renderPass.depdendencies.at(j).second;

                if (j < PROJV_MAX_PASS_INPUTS) {
                    auto inputResolution = constructedRenderer->resources.textures.textureResolutions.find(textureID);
                    if (inputResolution != constructedRenderer->resources.textures.textureResolutions.end()) {
                        const core::ivec2 size = inputResolution->second;
                        passInputRes[j][0] = float(size.x);
                        passInputRes[j][1] = float(size.y);
                        passInputRes[j][2] = 1.0f / float(std::max(size.x, 1));
                        passInputRes[j][3] = 1.0f / float(std::max(size.y, 1));
                    }
                }

                if (!(constructedRenderer->resources.textures.pingPongFlags.at(textureID))) {
#if defined(PROJV_ENABLE_RENDER)
                    if (!renderGraphLogged) {
                        core::render("Rendering from primary textureID (default): {}", textureID);
                    }
#endif
                    bgfx::UniformHandle textureUniformHandle = constructedRenderer->resources.textures.textureSamplerHandles.at(textureID);
                    bgfx::TextureHandle textureHandle = constructedRenderer->resources.textures.textureHandles.at(textureID);
                    bgfx::getUniformInfo(textureUniformHandle, info);
                    bgfx::setTexture(j, renderPass.depdendencies[j].first, textureHandle);
                } else if (!constructedRenderer->resources.framebuffers.primaryWasLastRenderedToo.at(constructedRenderer->resources.textures.textureIDToFrameBufferID.at(textureID))) {
#if defined(PROJV_ENABLE_RENDER)
                    if (!renderGraphLogged) {
                        core::render("Rendering from alternate textureID: {}", textureID);
                    }
#endif
                    bgfx::UniformHandle textureUniformHandleAlternate = constructedRenderer->resources.textures.textureSamplerHandlesAlternate.at(textureID);
                    bgfx::TextureHandle textureHandleAlternate = constructedRenderer->resources.textures.textureHandlesAlternate.at(textureID);
                    bgfx::getUniformInfo(textureUniformHandleAlternate, info);
                    bgfx::setTexture(j, renderPass.depdendencies[j].first, textureHandleAlternate);
                } else {
#if defined(PROJV_ENABLE_RENDER)
                    if (!renderGraphLogged) {
                        core::render("Rendering from primary textureID: {}", textureID);
                    }
#endif
                    bgfx::UniformHandle textureUniformHandle = constructedRenderer->resources.textures.textureSamplerHandles.at(textureID);
                    bgfx::TextureHandle textureHandle = constructedRenderer->resources.textures.textureHandles.at(textureID);
                    bgfx::getUniformInfo(textureUniformHandle, info);
                    bgfx::setTexture(j, renderPass.depdendencies[j].first, textureHandle);
                }

#if defined(PROJV_ENABLE_RENDER)
                if (!renderGraphLogged) {
                    core::render("Sampling from texture name: {}", info.name);
                    core::render("That texture is bound to: {}", j);
                }
#endif
            }

            bgfx::setUniform(constructedRenderer->resources.passInputRes, passInputRes, PROJV_MAX_PASS_INPUTS);

            // Set multi pass pass number.
            float multiPassPassNumberVector[4] = {
                (float)renderPass.multiPassPassNumber,  // x component  
                0.0f,           // y component    
                0.0f,           // z component  
                0.0f            // w component  
            };

            bgfx::setUniform(renderPass.multiPassPassNumberUniform, multiPassPassNumberVector);

            if (constructedRenderer->resources.framebuffers.pingPongFBOs.at(renderPass.targetFrameBufferID)) {
                constructedRenderer->resources.framebuffers.primaryWasLastRenderedToo[renderPass.targetFrameBufferID] = !constructedRenderer->resources.framebuffers.primaryWasLastRenderedToo[renderPass.targetFrameBufferID];
            }

            bgfx::setTexture(14, gpuData->tree64Sampler, gpuData->tree64Texture);
            bgfx::setTexture(9, gpuData->materialIDSampler, gpuData->materialIDTexture);
            bgfx::setTexture(10, gpuData->materialPaletteSampler, gpuData->materialPaletteTexture);
            bgfx::setTexture(15, gpuData->headerSampler, gpuData->headerTexture);
            bgfx::setTexture(12, gpuData->gridInfoSampler, gpuData->gridInfoTexture);
            bgfx::setTexture(13, gpuData->cellMapSampler, gpuData->cellMapTexture);
            bgfx::setTexture(11, gpuData->looseListSampler, gpuData->looseListTexture);

            {
                float tree64Dims[4] = {
                    static_cast<float>(gpuData->tree64Width),
                    std::log2(static_cast<float>(gpuData->tree64Width)),
                    0.0f, 0.0f
                };
                bgfx::setUniform(gpuData->tree64DimsUniform, tree64Dims);
            }
            {
                float voxelTypeDims[4] = {
                    static_cast<float>(gpuData->materialIDWidth),
                    std::log2(static_cast<float>(gpuData->materialIDWidth)),
                    0.0f, 0.0f
                };
                bgfx::setUniform(gpuData->materialIDDimsUniform, voxelTypeDims);
            }
            {
                float paletteDims[4] = {
                    static_cast<float>(gpuData->paletteWidth),
                    std::log2(static_cast<float>(gpuData->paletteWidth)),
                    0.0f, 0.0f
                };
                bgfx::setUniform(gpuData->paletteDimsUniform, paletteDims);
            }

            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);  
            bgfx::submit(renderPass.renderPassID, renderPass.shaderProgram);
        }
#if defined(PROJV_ENABLE_RENDER)
        renderGraphLogged = true;
#endif
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

        // This driver's renderer draws into the window, so the back buffer and the render targets are
        // the same size -- but they are now set separately, because only the driver knows that. A
        // renderer drawing into a panel (the scene editor) resets the back buffer to the window and
        // its targets to the panel, using these same two calls with different numbers.
        if (prevWindowWidth != windowWidth || prevWindowHeight != windowHeight) {
            bgfx::reset(uint32_t(windowWidth), uint32_t(windowHeight), BGFX_RESET_NONE, bgfx::TextureFormat::Count);
        }
        resizeRenderTargets(constructedRenderer->resources.textures, constructedRenderer->resources.framebuffers, windowWidth, windowHeight);
        performRenderPasses(true, constructedRenderer, renderInstance, windowWidth, windowHeight, view, proj, gpuData);

        //renderToPrimary = !renderToPrimary;

        prevWindowWidth = windowWidth;
        prevWindowHeight = windowHeight;

        bgfx::frame();
    }
}
