#ifndef GPU_DATA_H
#define GPU_DATA_H

#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv {
    struct GPUData {
        bgfx::TextureHandle tree64Texture;
        bgfx::TextureHandle voxelTypeDataTexture;
        bgfx::TextureHandle headerTexture;

        bgfx::UniformHandle tree64Sampler;
        bgfx::UniformHandle voxelTypeDataSampler;
        bgfx::UniformHandle headerSampler;
    };
}

#endif
