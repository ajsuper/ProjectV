#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <GL/glew.h>
#include <string>
#include <vector>

#include "texture.h"

namespace projv{
    /**
     * Contains a width, height, buffer for storing it's OpenGL ID, and textures - a vector of Texture objects that we have added to the FrameBuffer.
     */
    struct FrameBuffer {
        int width, height;
        GLuint buffer;
        std::vector<Texture> textures;
    };
}

#endif