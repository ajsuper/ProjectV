#ifndef PROJECTV_CORE_H
#define PROJECTV_CORE_H

#include <fstream>
#include <sstream>
#include <iostream>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <bitset>
#include <chrono>
#include <vector>
#include <cmath>

namespace projv{
    /**
     * Move/Rotate the camera from WASD QE SPACE/SHIFT (R and F to change movement speeds).
     * 
     * @param window The GLFW window to get user input from.
     */
    void moveCameraFromUserInput(GLFWwindow* window);

    /**
     * Initializes GLFW and GLEW, and creates a window.
     * 
     * @param window A reference to the GLFWwindow pointer that will be initialized.
     * @param windowWidth The width of the window to be created.
     * @param windowHeight The height of the window to be created.
     * @param windowName The title of the window to be created.
     * @return true if initialization and window creation were successful, false otherwise.
     */
    bool initializeGLFWandGLEWWindow(GLFWwindow*& window, int windowWidth, int windowHeight, const std::string& windowName);
}

#endif
