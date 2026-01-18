#ifndef GPU_DATA_H
#define GPU_DATA_H

#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv {
    struct GPUData {
        bgfx::TextureHandle octreeTexture;
        bgfx::TextureHandle voxelTypeDataTexture;
        bgfx::TextureHandle headerTexture;

        bgfx::UniformHandle octreeSampler;
        bgfx::UniformHandle voxelTypeDataSampler;
        bgfx::UniformHandle headerSampler;
    };
}

#endif
