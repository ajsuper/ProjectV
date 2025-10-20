#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <vector>

namespace projv {
    struct FrameBuffer {
        uint frameBufferID;
        std::vector<uint> TextureIDs;
    };
}

#endif
