#ifndef PROJV_TEXTURE_H
#define PROJV_TEXTURE_H

#include <cstdint>
#include <string>
#include <unordered_map>

#include "core/math.h"
#include <bgfx/bgfx.h>

namespace projv{
    enum TextureOrigin { CreateNew, CPUBuffer };

    // How a texture's pixel size is decided. This replaced a `bool resizable`, which had come to mean
    // two different things depending on `origin`: with CreateNew it meant "follow the window", with
    // CPUBuffer it meant "don't resize" -- and the second was true only because the code that would
    // have resized it was unreachable. Every texture in every resources.json was declared resizable,
    // including 256x256 images that must never be resized, so the flag carried no information.
    enum class TextureSizeMode {
        // resolutionX/Y are the size, for the life of the renderer. For anything whose size is a
        // property of its contents rather than of the window: uploaded images, lookup tables.
        Fixed,
        // `scale` times the renderer's render resolution, rounded up. scale 1.0 is a full-resolution
        // target; 0.5 is half on each axis, so a quarter of the pixels.
        Relative,
    };

    struct Texture {
        unsigned int textureID;
        std::string name;
        bgfx::TextureFormat::Enum format;
        int resolutionX;   // Fixed only; unread for Relative, whose size comes from the driver.
        int resolutionY;   // Fixed only.
        TextureSizeMode sizeMode = TextureSizeMode::Fixed;
        float scale = 1.0f; // Relative only.
        TextureOrigin origin;
        bool pingPongFlag = false; // Allows cyclic dependencies to be possible.
        // Opt-in: creates this texture with BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST so it can
        // be blitted into and read back to the CPU via bgfx::readTexture. Default false so every
        // existing resources.json (which never declares "readBack") is unaffected.
        bool readBack = false;
    };

    struct ConstructedTextures {
        std::unordered_map<uint32_t, bgfx::TextureHandle> textureHandles;
        std::unordered_map<uint32_t, bgfx::UniformHandle> textureSamplerHandles;
        std::unordered_map<uint32_t, bgfx::TextureHandle> textureHandlesAlternate; // Used in ping-poing rendering.
        std::unordered_map<uint32_t, bgfx::UniformHandle> textureSamplerHandlesAlternate; // Used in ping-pong rendering.
        // Scale factor per Relative texture. Presence in this map IS the sizing rule: a texture in
        // here follows the render resolution by its scale, one absent from it is Fixed. That is one
        // map instead of the two parallel bool maps this replaced, so a texture cannot be classified
        // inconsistently.
        std::unordered_map<uint32_t, float> relativeTextureScales;
        std::unordered_map<uint32_t, projv::core::ivec2> textureResolutions;
        std::unordered_map<uint32_t, bgfx::TextureFormat::Enum> textureFormats;
        // The bgfx creation flags each texture was built with, so a resize can rebuild it as the
        // same kind of texture. Without this a resize recreates every target with BGFX_TEXTURE_RT
        // alone, which silently drops BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST -- a texture
        // that is both resizable and readBack stops being readable the first time the window moves,
        // and nothing reports it because the handle is still perfectly valid.
        std::unordered_map<uint32_t, uint64_t> textureFlags;
        std::unordered_map<uint32_t, bool> pingPongFlags;
        std::unordered_map<uint32_t, uint32_t> textureIDToFrameBufferID;
    };
}

#endif
