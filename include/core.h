#ifndef PROJECTV_CORE_H
#define PROJECTV_CORE_H
// Last edited on: 31-12-2024.

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
     * Error callback for GLFW.
     * 
     * @param error The error code.
     * @param description A description of the error.
     */
    void error_callback(int error, const char* description);

    /**
     * Frame size callback for GLFW.
     * 
     * @param window The GLFW window.
     * @param width The new width of the framebuffer.
     * @param height The new height of the framebuffer.
     */
    void framebuffer_size_callback(GLFWwindow* window, int width, int height);

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