#ifndef WINDOW_H
#define WINDOW_H

#include <iostream>
#include "data_structures/posTexVertex.h"
#include "render_instance.h"
#include "/opt/homebrew/Cellar/glfw/3.4/include/GLFW/glfw3.h"
#include "../external/bgfx/include/bgfx/platform.h"
#define GLFW_EXPOSE_NATIVE_COCOA
#include "/opt/homebrew/Cellar/glfw/3.4/include/GLFW/glfw3native.h"

namespace projv::graphics {
    RenderInstance initializeRenderInstance(int width, int height, std::string name);
}

#endif
