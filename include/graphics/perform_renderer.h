#ifndef PROJV_PERFORM_RENDERER_H
#define PROJV_PERFORM_RENDERER_H

#include <unordered_map>
#include <iostream>
#include <optional>

#include "data_structures/constructedRenderer.h"
#include "data_structures/gpuData.h"

#include "graphics/manage_resources.h"

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
     * @param constructedRenderer A shared pointer to a ConstructedRenderer object that contains all the necessary rendering configuration.
     * @param renderInstance Contains resources like our window and the bgfx instance.
     * @param windowWidth The width of the current render target/window.
     * @param windowHeight The height of the current render target/window.
     * @param viewMat A 4x4 matrix representing the camera's view transformation.
     * @param projMat A 4x4 matrix representing the projection transformation.
     * @param gpuData A pointer to our projv::Scene serialized as projv::GPUData.
     */
    void performRenderPasses(bool renderToPrimary, std::shared_ptr<ConstructedRenderer> constructedRenderer, RenderInstance& renderInstance, int windowWidth, int windowHeight, core::mat4 viewMat, core::mat4 projMat, GPUData* gpuData);

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
}

#endif
