#ifndef PROJV_TYPE_MAPPING_H
#define PROJV_TYPE_MAPPING_H

#include <string>
#include <vector>
#include <iostream>

#include "data_structures/uniform.h"
#include "data_structures/texture.h"
#include "data_structures/framebuffer.h"
#include "data_structures/dependencyGraph.h"
#include "bgfx/bgfx.h"

namespace projv::graphics {
    bgfx::UniformType::Enum mapUniformType(UniformType uniformType);

    UniformType getUniformType(std::string type);

    bgfx::TextureFormat::Enum getFormat(std::string format);

    TextureOrigin getOrigin(std::string origin);

    std::vector<std::pair<bgfx::UniformHandle, uint>> getDependenciesList(const std::vector<FrameBuffer>& frameBuffers, const std::unordered_map<uint, bgfx::UniformHandle>& textureSamplerHandles, const RenderPass &renderPass);
}

#endif
