#ifndef PROJECTV_RENDERER_LOADER_H
#define PROJECTV_RENDERER_LOADER_H

#include <unordered_map>
#include <vector>
#include <fstream>
#include <iostream>

#include "data_structures/uniform.h"
#include "data_structures/texture.h"
#include "data_structures/shader.h"
#include "data_structures/rendererSpecification.h"

#include "bgfx/bgfx.h"
#include "nlohmann/json.hpp"

namespace projv::graphics {
    UniformType getUniformType(std::string type);

    static std::vector<char> readFile(const std::string &filename);

    bgfx::TextureFormat::Enum getFormat(std::string format);

    TextureOrigin getOrigin(std::string origin);

    std::vector<Uniform> loadUniformTypes(nlohmann::json& resourceData);

    std::vector<Shader> loadShaders(nlohmann::json& resourceData, std::string rendererPath);

    std::vector<Texture> loadTextures(nlohmann::json& resourceData);

    std::vector<FrameBuffer> loadFrameBuffers(nlohmann::json& resourceData);

    std::vector<RenderPass> loadRenderPasses(nlohmann::json& dependencyGraphData);

    RendererSpecification loadRendererSpecification(std::string rendererPath);
}

#endif
