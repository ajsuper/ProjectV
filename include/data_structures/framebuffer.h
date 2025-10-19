#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <vector>

struct FrameBuffer {
    uint frameBufferID;
    std::vector<uint> TextureIDs;
};

#endif
