#ifndef RENDER_INSTANCE_H
#define RENDER_INSTANCE_H

#include <GLFW/glfw3.h>
#include <math.h>

namespace projv {
    struct RenderInstance {
        GLFWwindow* window;
        GLuint VAO, VBO;
        uint frameCount;
    };
}

#endif