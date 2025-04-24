#ifndef PROJECTV_USER_INPUT_H
#define PROJECTV_USER_INPUT_H

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <math.h>
#include "data_structures/camera.h"

#include "core/log.h"

namespace projv::graphics {
    /**
     * Move/Rotate the camera from WASD QE SPACE/SHIFT (R and F to change movement speeds).
     * 
     * @param window The GLFW window to get user input from.
     */
    void moveCameraFromUserInput(GLFWwindow* window);
}

#endif