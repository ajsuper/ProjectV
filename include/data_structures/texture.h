#ifndef TEXTURE_H
#define TEXTURE_H

#include <string>

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
}

#endif
