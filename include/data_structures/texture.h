#ifndef TEXTURE_H
#define TEXTURE_H

#include <GL/glew.h>
#include <string>

namespace projv{
    /**
     * Contains a textureID, format, and name for a texture to be used in a FrameBufferObject.
     */
    struct Texture {
        GLuint textureID;
        GLuint format;
        std::string name = {};
    };
}

#endif