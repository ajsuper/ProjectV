#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <vector>
#include <unordered_map>

#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv {
    struct FrameBuffer {
        uint frameBufferID;
        std::vector<uint> TextureIDs;
        bool pingPongFBO = false;
    };

    struct ConstructedFramebuffers {
        std::unordered_map<int, bgfx::FrameBufferHandle> frameBufferHandles;
        std::unordered_map<int, std::vector<uint>> frameBufferTextureMapping;
        std::unordered_map<int, bgfx::FrameBufferHandle> frameBufferHandlesAlternate;
        std::unordered_map<int, std::vector<uint>> frameBufferTextureMappingAlternate;
        std::unordered_map<int, bool> pingPongFBOs;
        std::unordered_map<int, bool> primaryWasLastRenderedToo; // If the primary frame buffer was last rendered to.
    };
}

#endif
