#ifndef TEXTURE_H
#define TEXTURE_H

#include <string>

#include "core/math.h"
#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv{
    enum TextureOrigin { CreateNew, CPUBuffer };

    struct Texture {
        unsigned int textureID;
        std::string name;
        bgfx::TextureFormat::Enum format;
        int resolutionX;
        int resolutionY;
        bool resizable;
        TextureOrigin origin;
    };

    struct ConstructedTexture {
        bgfx::TextureHandle textureHandle;
        bgfx::UniformHandle textureSamplerHandle;
        bool resizedWithWindow = false;
        bool resizedWithResourceTextures = false;
        projv::core::ivec2 resolution;
        bgfx::TextureFormat::Enum textureFormat;
    };
}

#endif
