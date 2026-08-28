#ifndef PROJV_RESOURCES_H
#define PROJV_RESOURCES_H
#include <vector>

#include "uniform.h"
#include "shader.h"
#include "texture.h"
#include "framebuffer.h"

namespace projv {
    struct Resources {
        std::vector<Uniform> uniforms;
        std::vector<Shader> shaders;
        std::vector<Texture> textures;
        std::vector<FrameBuffer> FrameBuffers;
    };
}

#endif
