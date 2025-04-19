#ifndef PROJECTV_RENDER_H
#define PROJECTV_RENDER_H

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <iostream>

#include "data_structures/renderInstance.h"
#include "data_structures/framebuffer.h"

namespace projv::graphics {
    /**
     * Renders a fragment/vertex shader shader program to the targetBuffer.
     * 
     * @param shaderProgram The shader that will be used to render to the targetBuffer.
     * @param targetBuffer What the shaderProgram will render to.
     */
    void renderFragmentShaderToTargetBuffer(RenderInstance renderInstance, GLuint shaderProgram, FrameBuffer targetBuffer);

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
    void renderFragmentShaderToTargetBufferWithOneInputBuffer(RenderInstance renderInstance, GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer targetBuffer);

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
    void renderFragmentShaderToTargetBufferWithTwoInputBuffers(RenderInstance renderInstance, GLuint shaderProgram, FrameBuffer inputBuffer1, FrameBuffer inputBuffer2, FrameBuffer targetBuffer);

    /**
     * Uses a ping-pong method to switch between frameBuffer1 and frameBuffer2 to perform operations that require multiple passes.
     * 
     * @param numberOfPassses How many passes the shader will run for. Switches between frameBuffer1 and frameBuffer2 until the last pass where it renders to targetBuffer
     * @param multiPassShaderProgram The shader that will be ran for the buffers.
     * @param frameBuffer1 The initial buffer that is used as an input for the shader.
     * @param frameBuffer2 The intermediate buffer that is first outputed to the shader.
     * @param targetBuffer The final buffer that the last pass will output to.
     */
    void renderMultipassFragmentShaderToTargetBuffer(RenderInstance renderInstance, int numberOfPasses, GLuint multiPassShaderProgram, FrameBuffer frameBuffer1, FrameBuffer frameBuffer2, FrameBuffer targetBuffer);
}

#endif