#ifndef RENDER_INSTANCE_H
#define RENDER_INSTANCE_H

#include <GLFW/glfw3.h>
#include <math.h>
#include <unordered_map>

#include "data_structures/framebuffer.h"

namespace projv {
    struct RenderInstance {
        GLFWwindow* window;
        GLuint VAO, VBO;
        uint frameCount;
        std::unordered_map<std::string, GLuint> shaders;
        std::unordered_map<std::string, projv::FrameBuffer> frameBufferObjects;
    };
}

#endif