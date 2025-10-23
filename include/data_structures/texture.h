#ifndef TEXTURE_H
#define TEXTURE_H

#include <string>
#include <unordered_map>

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

    struct ConstructedTextures {
        std::unordered_map<uint, bgfx::TextureHandle> textureHandles;
        std::unordered_map<uint, bgfx::UniformHandle> textureSamplerHandles;
        std::unordered_map<uint, bool> texturesResizedWithWindow;
        std::unordered_map<uint, bool> texturesResizedWithResourceTextures;
        std::unordered_map<uint, projv::core::ivec2> textureResolutions;
        std::unordered_map<uint, bgfx::TextureFormat::Enum> textureFormats;
    };
}

#endif
