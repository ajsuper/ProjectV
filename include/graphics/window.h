#ifndef PROJECTV_WINDOW_H
#define PROJECTV_WINDOW_H

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <string>
#include <iostream>

#include "data_structures/scene.h"
#include "data_structures/renderInstance.h" // Include the header defining RenderInstance

namespace projv::graphics {
    /**
     * Initializes GLFW and GLEW, and creates a window.
     * 
     * @param window A reference to the GLFWwindow pointer that will be initialized.
     * @param windowWidth The width of the window to be created.
     * @param windowHeight The height of the window to be created.
     * @param windowName The title of the window to be created.
     * @return true if initialization and window creation were successful, false otherwise.
     */
    bool initializeGLFWandGLEWWindow(GLFWwindow*& window, int windowWidth, int windowHeight, const std::string& windowName); // Graphics

    /**
     * Initializes GLFW, GLEW, our fullscreen render quad, and creates a window.
     * 
     * @param windowWidth The width of the window.
     * @param windowHeight The height of the window.
     * @param windowName The name of the window.
     * @return Returns a RenderInstance with the initialized variables.
     */
    RenderInstance initializeRenderInstance(int windowWidth, int windowHeight, const std::string& windowName); // Graphics

    /**
     * Sets the error callback(what happens when an OpenGL error occurs) that OpenGL will use
     * 
     * @param renderInstance The renderInstance that we want to set the callback for.
     * @param error_callback A function pointer that stores the user defined function to handle error's.
     */
    void setErrorCallback(RenderInstance& renderInstance, void (*error_callback)(int error_code, const char* description)); // Graphics

    /**
     * Sets the frame buffer size callback(what happens on window resize) that OpenGL will use.
     */
    void setFrameBufferSizeCallback(RenderInstance& renderInstance, void (*framebuffer_callback)(GLFWwindow* window, int width, int height)); // Graphics
}

#endif