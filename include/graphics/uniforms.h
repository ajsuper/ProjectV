#ifndef PROJECTV_UNIFORMS_H
#define PROJECTV_UNIFORMS_H

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>

#include <data_structures/camera.h>

namespace projv::graphics {
    /**
     * Finds a floating point uniform in our shaders and passes the floating point variable to it.
     * 
     * @param shader The GLSL shader program to update the variable in.
     * @param variable The floating point number to send to the shader.
     * @param variableNameInShader The name of the variable in our GLSL shader that our float will be assigned to.
     */
    void updateFloatInShader(GLuint shader, float variable, const char variableNameInShader[24]); // Uniform

    /**
     * Updates the camera position and direction in the shader.
     */
    void updateCameraInShader(GLuint shader); // Uniform or core or shader?

    /**
     * Updates the resolution in the shader.
     * 
     * @param window The GLFW window to get the framebuffer size from.
     */
    void updateResolutionInShader(GLuint shader, GLFWwindow* window); // Uniform or core or shader?

}

#endif