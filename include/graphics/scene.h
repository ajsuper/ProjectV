#ifndef PROJV_SCENE_H
#define PROJV_SCENE_H

#include <stdint.h>
#include <vector>
#include <iostream>

#include "data_structures/scene.h"
#include "data_structures/gpuData.h"
#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv::graphics {
    bgfx::TextureHandle createArbitraryTexture(std::vector<uint32_t>& data);

    bgfx::TextureHandle createHeaderTexture(std::vector<projv::GPUChunkHeader>& headers);

    GPUData createTexturesForScene(projv::Scene& scene);
}

#endif
