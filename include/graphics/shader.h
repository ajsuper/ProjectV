#ifndef PROJECTV_SHADER_H
#define PROJECTV_SHADER_H

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>

#include "data_structures/renderInstance.h"
#include "core/log.h"

namespace projv::graphics {
    /**
     * Loads GLSL shader source code from a file.
     * 
     * @param filepath The path to the shader file.
     * @return The shader source code as a string.
     */
    std::string loadShaderSource(const char* filepath);

    /**
     * Creates an OpenGL shader program for a vertex and fragment shader.
     * 
     * @param vertexPath The file path of our vertex shader.
     * @param fragmentPath The file path of our fragment shader.
     * 
     * @return A GLuint shader program with our compiled vertex and fragment shaders.
     */
    GLuint compileVertexAndFragmentShaders(const char* vertexPath, const char* fragmentPath);

    /**
     * Adds an OpenGL shader to our render instance. Useful when using the ECS system to pass shaders globally.
     * 
     * @param renderInstance The RenderInstance to which the shader will be added.
     * @param shader The OpenGL shader ID to be added.
     * @param shaderName The name of the shader to be used as a key for retrieval.
     */
    void addShaderToRenderInstance(RenderInstance& renderInstance, GLuint shader, std::string shaderName);

    /**
     * Removes an OpenGL shader from our render instance.
     * 
     * @param renderInstance The RenderInstance from which the shader will be removed.
     * @param shaderName The name of the shader to be removed.
     */
    void removeShaderFromRenderInstance(RenderInstance& renderInstance, std::string shaderName);

    /**
     * Retrieves an OpenGL shader from our render instance.
     * 
     * @param renderInstance The RenderInstance from which the shader will be retrieved.
     * @param shaderName The name of the shader to retrieve.
     * @return The OpenGL shader ID associated with the given shader name.
     */
    GLuint getShaderFromRenderInstance(RenderInstance& renderInstance, std::string shaderName);
}

#endif