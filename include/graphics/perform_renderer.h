#ifndef PROJV_PERFORM_RENDERER_H
#define PROJV_PERFORM_RENDERER_H


#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <optional>

#include "data_structures/constructedRenderer.h"
#include "data_structures/gpuData.h"

#include "graphics/manage_resources.h"
#include "core/log.h"

#include "render_instance.h"
#include "bgfx/bgfx.h"

namespace projv::graphics {

    /**
     * Updates the values of uniforms in the shader program.
     * This function maps uniform names to their corresponding bgfx::UniformHandle
     * and sets the new values for each uniform.
     *
     * @param uniformHandles A map of uniform names to their bgfx::UniformHandle.
     * @param uniformValues A map of uniform names to their new values (as a vector of bytes).
     */
    void updateUniforms(const std::unordered_map<std::string, bgfx::UniformHandle>& uniformHandles, const std::unordered_map<std::string, std::vector<uint8_t>>& uniformValues);

    /**
     * Performs all the render passes defined in a ConstructedRenderer.
     * This function processes each render pass, sets up the necessary state, 
     * and executes the shader.
     *
     * Each pass rasterizes at the size of the framebuffer it writes, resolved per frame by
     * resolveTargetSize, so passes in one renderer may render at different resolutions.
     *
     * @param constructedRenderer A shared pointer to a ConstructedRenderer object that contains all the necessary rendering configuration.
     * @param renderInstance Contains resources like our window and the bgfx instance.
     * @param backBufferWidth Width of the back buffer, used only by passes that write the default
     *                        framebuffer (frameBufferOutputID -1). Every other pass takes its size
     *                        from its own target and ignores this.
     * @param backBufferHeight Height of the back buffer, as above.
     * @param viewMat A 4x4 matrix representing the camera's view transformation.
     * @param projMat A 4x4 matrix representing the projection transformation.
     * @param gpuData A pointer to our projv::Scene serialized as projv::GPUData.
     */
    void performRenderPasses(bool renderToPrimary, std::shared_ptr<ConstructedRenderer> constructedRenderer, RenderInstance& renderInstance, int backBufferWidth, int backBufferHeight, core::mat4 viewMat, core::mat4 projMat, GPUData* gpuData);

    /**
     * Renders a ConstructedRenderer by executing all its render passes.
     * This function is the main rendering entry point and orchestrates
     * the entire rendering process, including uniform updates, render passes,
     * and viewport setup.
     *
     * @param renderInstance Contains resoources like our window and bgfx instance.
     * @param constructedRenderer A shared pointer to a ConstructedRenderer object that contains all the necessary rendering configuration.
     * @param gpuData A pointer to our projv::Scene serialized as projv::GPUData.
     */
    void renderConstructedRenderer(RenderInstance &renderInstance, std::shared_ptr<ConstructedRenderer> constructedRenderer, GPUData* gpuData);

    /**
     * The size a renderer's window-relative targets are made at for a window of `window`:
     * `window * renderScale`, rounded, at least 1x1. renderConstructedRenderer sizes them so.
     */
    core::ivec2 renderTargetSize(core::ivec2 window, float renderScale);

    /**
     * Turns the passes named `name` (BGFXDependencyGraph::name: the shader file's stem, "dof") on or
     * off. An off pass is skipped outright: nothing drawn, its target keeps what it last held, so it
     * suits a pass whose output the frame no longer reads. Returns how many passes it named (0: none,
     * which is likely a typo).
     */
    int setPassEnabled(ConstructedRenderer& renderer, const std::string& name, bool enabled);

    /**
     * Ends the frame: draws the render instance's overlays (RenderInstance::addOverlay) over what the
     * renderer drew, then submits it (bgfx::frame). renderConstructedRenderer ends with this; a program
     * that submits its own passes calls it in place of bgfx::frame so the UI still draws.
     * @param backBufferSize The back buffer's size, which the overlays draw at.
     * @return bgfx::frame's frame number.
     */
    uint32_t endFrame(RenderInstance &renderInstance, core::ivec2 backBufferSize);
}

#endif
