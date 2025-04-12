#ifndef PROJECTV_GRAPHICS_H
#define PROJECTV_GRAPHICS_H

#include <fstream>
#include <sstream>
#include <iostream>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <bitset>
#include <chrono>
#include <vector>
#include <cmath>
#include <cstring>

#include "data_structures/texture.h"
#include "data_structures/framebuffer.h"
#include "data_structures/scene.h"

namespace projv{

    // Global variables
    extern int frameCount;

    /**
     * Adds a texture to the frameBuffer with the specified format and name.
     * 
     * @param frameBuffer The frame buffer to add the texture to.
     * @param format The format of the texture. (Prints an error when non-supported formats are used)
     * @param textureName The name of the texture. Used whenever you use the texture as a sampler2D in a shader. inputBuffer(buffer #)_ will pe prepended to the name specified when used as inputs.
     */
    void addTextureToFrameBuffer(FrameBuffer &frameBuffer, GLuint format, std::string textureName);

    /**
     * Creates a frame buffer object with the specified width and height.
     * 
     * @param width The width of the frame buffer object.
     * @param height The height of the frame buffer object.
     */
    FrameBuffer createFrameBufferObjectAdvanced(int width, int height);

    Texture loadPNGIntoTexture(std::string filePath, std::string textureName, GLuint format);

    /**
     * Renders a fragment/vertex shader shader program to the targetBuffer.
     * 
     * @param shaderProgram The shader that will be used to render to the targetBuffer.
     * @param targetBuffer What the shaderProgram will render to.
     */
    void renderFragmentShaderToTargetBuffer(GLuint shaderProgram, FrameBuffer targetBuffer);

    /**
     * @brief Renders a fragment/vertex shader program to the target buffer at its resolution.
     * 
     * @details 
     * **Texture Names:**  
     * To access input buffer textures as `sampler2D`s, this function prepends `inputBuffer1_`  
     * to the name of the texture in the shader.
     * 
     * @b Example:  
     * If you pass a texture named `"PositionTexture"` to the shader,  
     * you will access it as `"inputBuffer1_PositionTexture"` in the shader.
     * 
     * ---
     * 
     * **Target Buffer:**  
     * To output to a target buffer, use the following GLSL syntax:
     * @code
     * layout (location = (The texture's number, corresponding to the order you assigned 
     * the textures to the frame buffer)) out (GLSL corresponding format) VariableName;
     * @endcode
     * 
     * @b Example:  
     * If you assign textures in this order:  
     * 1. `"colorTexture"` with `GL_RGBA8` format  
     * 2. `"normalTexture"` with `GL_RGB32F` format  
     * 
     * You would output to them in GLSL as:  
     * @code
     * layout (location = 0) out vec3 FragColor;  // Outputs to colorTexture
     * layout (location = 1) out vec3 FragNormal; // Outputs to normalTexture
     * @endcode
     * 
     * ---
     * 
     * @param shaderProgram The shader program that will be used to render to the target buffer.
     * @param inputBuffer1 The buffer that will be passed to the shader program as a texture input.
     * @param targetBuffer The buffer where the shader program will render its output.
     */
    void renderFragmentShaderToTargetBufferWithOneInputBuffer(GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer targetBuffer);

    /**
     * Renders a fragment/vertex shader shader program to the targetBuffer at it's resolution.
     * 
     * Texture Names:
     * To access your input buffer textures as sampler2D's, be aware that this function prepends inputBuffer1_ or inputBuffer2_ correspondingly to the name of the texture in the shader.
     * Ex:
     * If inputBuffer1 has a texture named "PositionTexture" and inputBuffer2 has a texture named "NormalTexture", you will access them as "inputBuffer1_PositionTexture" and "inputBuffer2_NormalTexture" in the shader.
     * 
     * @param shaderProgram The shader that will be used to render to the targetBuffer.
     * @param inputBuffer1 The first buffer that will be passed to the shaderProgram as a texture input.
     * @param inputBuffer2 The second buffer that will be passed to the shaderProgram as a texture input.
     * @param targetBuffer What the shaderProgram will render to.
     */
    void renderFragmentShaderToTargetBufferWithTwoInputBuffersAdvanced(GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer inputBuffer2, FrameBuffer targetBuffer);

    //void renderFragmentShaderToTargetBufferWithTwoInputBuffers(GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer inputBuffer2, FrameBuffer targetBuffer);

    /**
     * Uses a ping-pong method to switch between frameBuffer1 and frameBuffer2 to perform operations that require multiple passes.
     * 
     * @param numberOfPassses How many passes the shader will run for. Switches between frameBuffer1 and frameBuffer2 until the last pass where it renders to targetBuffer
     * @param multiPassShaderProgram The shader that will be ran for the buffers.
     * @param frameBuffer1 The initial buffer that is used as an input for the shader.
     * @param frameBuffer2 The intermediate buffer that is first outputed to the shader.
     * @param targetBuffer The final buffer that the last pass will output to.
     */
    void renderMultipassFragmentShaderToTargetBuffer(int numberOfPasses, GLuint multiPassShaderProgram, FrameBuffer frameBuffer1, FrameBuffer frameBuffer2, FrameBuffer targetBuffer);

    /**
     * Creates a quad for rendering to the whole window.
     */
    void createRenderQuad();

    /**
     * Finds a floating point uniform in our shaders and passes the floating point variable to it.
     * 
     * @param shader The GLSL shader program to update the variable in.
     * @param variable The floating point number to send to the shader.
     * @param variableNameInShader The name of the variable in our GLSL shader that our float will be assigned to.
     */
    void updateFloatInShader(GLuint shader, float variable, const char variableNameInShader[24]);

    /**
     * Updates the time variable in the shader.
     */
    void updateTimeInShader(GLuint shader);

    /**
     * Updates the camera position and direction in the shader.
     */
    void updateCameraInShader(GLuint shader);

    /**
     * Updates the resolution in the shader.
     * 
     * @param window The GLFW window to get the framebuffer size from.
     */
    void updateResolutionInShader(GLuint shader, GLFWwindow* window);

    /**
     * Loads GLSL shader source code from a file.
     * 
     * @param filepath The path to the shader file.
     * @return The shader source code as a string.
     */
    std::string loadShaderSource(const char* filepath);

    /**
     * Passes the scene to the fragment shader, very costly as it updates entire scene every time it is sent.
     * 
     * @param sceneToRender The scene to be passed to the fragment shader.
     */
    void passSceneToFrag(Scene& sceneToRender);

    /**
     * Creates an OpenGL shader program for a vertex and fragment shader.
     * 
     * @param vertexPath The file path of our vertex shader.
     * @param fragmentPath The file path of our fragment shader.
     * 
     * @return A GLuint shader program with our compiled vertex and fragment shaders.
     */
    GLuint compileVertexAndFragmentShaders(const char* vertexPath, const char* fragmentPath);
}

#endif
