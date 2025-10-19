#ifndef GPU_DATA_H
#define GPU_DATA_H

#include "../../external/bgfx/include/bgfx/bgfx.h"

struct GPUData { // Done
    bgfx::TextureHandle octreeTexture;
    bgfx::TextureHandle voxelTypeDataTexture;
    bgfx::TextureHandle headerTexture;

    bgfx::UniformHandle octreeSampler;
    bgfx::UniformHandle voxelTypeDataSampler;
    bgfx::UniformHandle headerSampler;
};

#endif
