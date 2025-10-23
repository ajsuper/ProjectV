#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <vector>
#include <unordered_map>

#include "../../external/bgfx/include/bgfx/bgfx.h"

namespace projv {
    struct FrameBuffer {
        uint frameBufferID;
        std::vector<uint> TextureIDs;
    };

    struct ConstructedFramebuffers {
        std::unordered_map<int, bgfx::FrameBufferHandle> frameBufferHandles;
        std::unordered_map<int, std::vector<uint>> frameBufferTextureMapping;
    };
}

#endif
