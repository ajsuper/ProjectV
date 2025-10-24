#ifndef PROJV_DISK_IO_H
#define PROJV_DISK_IO_H

#include <vector>
#include <string>

#include "data_structures/uniform.h"
#include "data_structures/shader.h"
#include "data_structures/texture.h"
#include "data_structures/framebuffer.h"
#include "data_structures/rendererSpecification.h"
#include "data_structures/dependencyGraph.h"

#include "nlohmann/json.hpp"

namespace projv::graphics {
    bgfx::ShaderHandle loadShader(const std::string &path);

    static std::vector<char> readFile(const std::string &filename);

    std::vector<Uniform> loadUniformTypes(nlohmann::json& resourceData);

    std::vector<Shader> loadShaders(nlohmann::json& resourceData, std::string rendererPath);

    std::vector<Texture> loadTextures(nlohmann::json& resourceData);

    std::vector<FrameBuffer> loadFrameBuffers(nlohmann::json& resourceData);

    std::vector<RenderPass> loadRenderPasses(nlohmann::json& dependencyGraphData);

    RendererSpecification loadRendererSpecification(std::string rendererPath);
}

#endif
