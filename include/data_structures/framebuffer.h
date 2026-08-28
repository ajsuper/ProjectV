#ifndef PROJV_FRAMEBUFFER_H
#define PROJV_FRAMEBUFFER_H

#include <cstdint>
#include <vector>
#include <unordered_map>

#include <bgfx/bgfx.h>

namespace projv {
    struct FrameBuffer {
        uint32_t frameBufferID;
        std::vector<uint32_t> TextureIDs;
        bool pingPongFBO = false;
    };

    struct ConstructedFramebuffers {
        std::unordered_map<int, bgfx::FrameBufferHandle> frameBufferHandles;
        std::unordered_map<int, std::vector<uint32_t>> frameBufferTextureMapping;
        std::unordered_map<int, bgfx::FrameBufferHandle> frameBufferHandlesAlternate;
        std::unordered_map<int, std::vector<uint32_t>> frameBufferTextureMappingAlternate;
        std::unordered_map<int, bool> pingPongFBOs;
        std::unordered_map<int, bool> primaryWasLastRenderedToo; // If the primary frame buffer was last rendered to.
    };
}

#endif
