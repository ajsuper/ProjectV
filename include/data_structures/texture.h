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
        bool pingPongFlag = false; // Allows cyclic dependencies to be possible.
        // Opt-in: creates this texture with BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST so it can
        // be blitted into and read back to the CPU via bgfx::readTexture. Default false so every
        // existing resources.json (which never declares "readBack") is unaffected.
        bool readBack = false;
    };

    struct ConstructedTextures {
        std::unordered_map<uint, bgfx::TextureHandle> textureHandles;
        std::unordered_map<uint, bgfx::UniformHandle> textureSamplerHandles;
        std::unordered_map<uint, bgfx::TextureHandle> textureHandlesAlternate; // Used in ping-poing rendering.
        std::unordered_map<uint, bgfx::UniformHandle> textureSamplerHandlesAlternate; // Used in ping-pong rendering.
        std::unordered_map<uint, bool> texturesResizedWithWindow;
        std::unordered_map<uint, bool> texturesResizedWithResourceTextures;
        std::unordered_map<uint, projv::core::ivec2> textureResolutions;
        std::unordered_map<uint, bgfx::TextureFormat::Enum> textureFormats;
        std::unordered_map<uint, bool> pingPongFlags;
        std::unordered_map<uint, uint> textureIDToFrameBufferID;
    };
}

#endif
