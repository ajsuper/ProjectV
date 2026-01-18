#ifndef PROJV_DISK_IO_H
#define PROJV_DISK_IO_H

#include <vector>
#include <string>
#include <iostream>
#include <fstream>

#include "data_structures/uniform.h"
#include "data_structures/shader.h"
#include "data_structures/texture.h"
#include "data_structures/framebuffer.h"
#include "data_structures/rendererSpecification.h"
#include "data_structures/dependencyGraph.h"

#include "graphics/type_mapping.h"
#include "core/log.h"

#include "nlohmann/json.hpp"

namespace projv::graphics {
    /**
     * Reads a bgfx compiled shader file from the path.
     * @param path The path of the shader file to be loaded.
     * @return Returns a bgfx::ShaderHandle created from the shader.
     */
    bgfx::ShaderHandle loadShader(const std::string &path);

    /**
     * Reads a file from the path specified in filename.
     * @param filename The path of the file to read.
     * @return Returns an std::vector<char> with the arbitrary data read from the file.
     */
    std::vector<char> readFile(const std::string &filename);

    /**
     * Loads the uniforms based on the json resource description of a renderer.
     * @param resourceData The json resource description containing the information for a renderer.
     * @return Returns an std::vector<projv::Uniform> containing all of the uniforms described in the json resource description.
     */
    std::vector<Uniform> loadUniformTypes(nlohmann::json& resourceData);

    /**
     * Loads the shaders from the resource description of a renderer. Doesn't seem to be fully implemented?
     * @param resourceData The json resource description containing all of the information for a renderer.
     * @param rendererPath The path of the renderer used to access the shaders.
     * @return Returns an std::vector<projv::Shader> containing all of the shaders described in the json resource description.
     */
    std::vector<Shader> loadShaders(nlohmann::json& resourceData, std::string rendererPath);

    /**
     * Loads the textures based on the json resource desciption of a renderer.
     * @param resourceData The json resource description containing the information of a renderer.
     * @return Returns an std::vector<projv::Texture> containing all of the textures described in the json resource description.
     */
    std::vector<Texture> loadTextures(nlohmann::json& resourceData);

    /**
     * Loads the framebuffers based on the json resource description of a renderer.
     * @param resourceData The json resource description containing the information of a renderer.
     * @return Returns an std::vector<projv::FrameBuffer> containing all of the frame buffers described in the json resource description.
     */
    std::vector<FrameBuffer> loadFrameBuffers(nlohmann::json& resourceData);

    /**
     * Loads the render passes based on the json dependency graph description of a renderer.
     * @param dependencyGraphData The json dependency graph description containing all of the information of a renderer.
     * @return Returns an std::vector<projv::RenderPass> containing all of the render passes described in the json dependency graph description.
     */
    std::vector<RenderPass> loadRenderPasses(nlohmann::json& dependencyGraphData);

    /**
     * Loads all of the components of a projv::RendererSpecification from the folder specified in rendererPath.
     * @param rendererPath The path of the folder of the entire renderer.
     * @return Returns a projv::RendererSpecification containing the description of all the resources and render passes required to render.
     */
    RendererSpecification loadRendererSpecification(std::string rendererPath);
}

#endif
